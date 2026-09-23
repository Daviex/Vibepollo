#include "pyrowave_transport.h"

#include "crypto.h"
#include "stream_protocol.h"

#include <algorithm>
#include <limits>
#include <memory>

extern "C" {
#include "rswrapper.h"
}

namespace pyrowave::transport {
  namespace {
    void put_le(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value, unsigned count) {
      for (unsigned i = 0; i < count; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    void put_be(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value, unsigned count) {
      for (unsigned i = 0; i < count; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * (count - i - 1)));
    }
  }  // namespace

  std::optional<std::vector<std::uint8_t>> packetize_frame(
    std::span<const std::uint8_t> frame,
    const protocol::transport_config_t &config,
    std::uint16_t processing_latency
  ) {
    const auto limits = protocol::transport_limits(config);
    if (!limits || !protocol::plan_transport(frame.size(), config) || !protocol::parse_frame(frame)) {
      return std::nullopt;
    }
    std::array<std::uint8_t, protocol::short_frame_header_size> header {};
    header[0] = 1;
    put_le(header, 1, processing_latency, 2);
    header[3] = 2;  // Every PyroWave frame is independently decodable.
    auto last_payload = (frame.size() + header.size()) % limits->payload_bytes;
    if (last_payload == 0) last_payload = limits->payload_bytes;
    put_le(header, 4, last_payload, 2);
    return stream::concat_and_insert(packet_header_bytes, limits->payload_bytes,
                                    {reinterpret_cast<const char *>(header.data()), header.size()},
                                    {reinterpret_cast<const char *>(frame.data()), frame.size()});
  }

  std::optional<encoded_block_t> encode_block(
    std::span<const std::uint8_t> data,
    const protocol::fec_block_t &planned,
    const protocol::transport_config_t &config,
    const block_info_t &info,
    crypto::cipher::gcm_t *cipher,
    std::uint64_t &iv_counter
  ) {
    const auto limits = protocol::transport_limits(config);
    if (!limits || info.block_count == 0 || info.block_count > 4 || info.block_index >= info.block_count ||
        planned.data_shards == 0 || planned.data_shards > 255 || planned.parity_shards > 254 ||
        planned.data_shards + planned.parity_shards > 255 || config.encrypted != (cipher != nullptr)) {
      return std::nullopt;
    }
    const auto shard_bytes = config.packet_size + 16;
    const auto total_shards = planned.data_shards + planned.parity_shards;
    if (data.empty() || data.size() > std::size_t(planned.data_shards) * shard_bytes ||
        (data.size() + shard_bytes - 1) / shard_bytes != planned.data_shards ||
        (data.size() % shard_bytes != 0 && data.size() % shard_bytes <= packet_header_bytes)) {
      return std::nullopt;
    }
    const auto natural = (planned.data_shards * config.fec_percentage + 99) / 100;
    const auto parity = config.fec_percentage == 0 ? 0 : std::max(natural, config.min_fec_packets);
    const auto percentage = parity > natural ? 100 * parity / planned.data_shards : config.fec_percentage;
    if (parity != planned.parity_shards || percentage > 255 || (planned.data_shards * percentage + 99) / 100 != parity ||
        (cipher && total_shards > std::numeric_limits<std::uint64_t>::max() - iv_counter)) {
      return std::nullopt;
    }
    encoded_block_t result {planned.data_shards, parity, percentage, shard_bytes, cipher ? 32U : 0U, {}, {}};
    result.shards.resize(std::size_t(total_shards) * shard_bytes, 0);
    result.prefixes.resize(std::size_t(total_shards) * result.prefix_bytes, 0);
    std::copy(data.begin(), data.end(), result.shards.begin());
    std::vector<std::uint8_t *> shard_pointers(total_shards);
    for (std::uint32_t i = 0; i < total_shards; ++i) shard_pointers[i] = result.shards.data() + std::size_t(i) * shard_bytes;
    for (std::uint32_t i = 0; i < planned.data_shards; ++i) {
      auto shard = std::span(shard_pointers[i], shard_bytes);
      std::fill_n(shard.begin(), packet_header_bytes, 0);
      put_le(shard, 16, (info.sequence_base + i) << 8, 4);
      put_le(shard, 20, info.frame_index, 4);
      shard[24] = 1 | (i == 0 ? 4 : 0) | (i + 1 == planned.data_shards ? 2 : 0);
      shard[26] = 0x10;
      shard[27] = static_cast<std::uint8_t>((info.block_index << 4) | ((info.block_count - 1) << 6));
    }
    if (parity != 0) {
      std::unique_ptr<reed_solomon, decltype(reed_solomon_release)> rs {
        reed_solomon_new(static_cast<int>(planned.data_shards), static_cast<int>(parity)), reed_solomon_release};
      if (!rs || reed_solomon_encode(rs.get(), shard_pointers.data(), static_cast<int>(total_shards), static_cast<int>(shard_bytes)) != 0) {
        return std::nullopt;
      }
    }
    for (std::uint32_t i = 0; i < total_shards; ++i) {
      auto shard = std::span(shard_pointers[i], shard_bytes);
      shard[0] = 0x90;
      put_be(shard, 2, static_cast<std::uint16_t>(info.sequence_base + i), 2);
      put_be(shard, 4, info.timestamp_90khz, 4);
      put_le(shard, 20, info.frame_index, 4);
      shard[27] = static_cast<std::uint8_t>((info.block_index << 4) | ((info.block_count - 1) << 6));
      put_le(shard, 28, (i << 12) | (planned.data_shards << 22) | (percentage << 4), 4);
      if (cipher) {
        auto prefix = std::span(result.prefixes).subspan(std::size_t(i) * 32, 32);
        crypto::aes_t iv(12, 0);
        put_le(iv, 0, iv_counter++, 8);
        iv[11] = 'V';
        std::copy(iv.begin(), iv.end(), prefix.begin());
        put_le(prefix, 12, info.frame_index, 4);
        if (cipher->encrypt({reinterpret_cast<const char *>(shard.data()), shard.size()}, prefix.data() + 16, shard.data(), &iv) != static_cast<int>(shard_bytes)) {
          return std::nullopt;
        }
      }
    }
    return result;
  }
}  // namespace pyrowave::transport
