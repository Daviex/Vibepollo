#include "pyrowave_protocol.h"

#include <algorithm>
#include <limits>

namespace pyrowave::protocol {
  namespace {
    void put_le(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned size) {
      for (unsigned i = 0; i < size; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
      }
    }

    std::uint64_t get_le(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned size) {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < size; ++i) {
        value |= std::uint64_t(bytes[offset + i]) << (i * 8);
      }
      return value;
    }

    std::uint32_t parity_shards(std::uint32_t data, const transport_config_t &config) {
      return config.fec_percentage == 0 ? 0 : std::max((data * config.fec_percentage + 99) / 100, config.min_fec_packets);
    }
  }  // namespace

  std::optional<std::vector<std::uint8_t>> serialize_frame(
    std::uint64_t frame_index,
    std::uint64_t timestamp_us,
    std::span<const std::span<const std::uint8_t>> packets,
    std::size_t byte_limit
  ) {
    byte_limit = std::min(byte_limit, max_frame_size);
    if (packets.empty() || packets.size() > max_native_packets || byte_limit < frame_header_size) {
      return std::nullopt;
    }
    std::size_t total = frame_header_size;
    for (const auto packet : packets) {
      if (packet.empty() || packet.size() % 4 != 0 || byte_limit - total < 4 || packet.size() > byte_limit - total - 4) {
        return std::nullopt;
      }
      total += 4 + packet.size();
    }

    std::vector<std::uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'P', 'W', 'V', 'F'});
    put_le(out, version, 2);
    put_le(out, frame_header_size, 2);
    put_le(out, total, 4);
    put_le(out, packets.size(), 4);
    put_le(out, frame_index, 8);
    put_le(out, timestamp_us, 8);
    for (const auto packet : packets) {
      put_le(out, packet.size(), 4);
      out.insert(out.end(), packet.begin(), packet.end());
    }
    return out;
  }

  std::optional<frame_view_t> parse_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < frame_header_size || bytes.size() > max_frame_size ||
        bytes[0] != 'P' || bytes[1] != 'W' || bytes[2] != 'V' || bytes[3] != 'F' ||
        get_le(bytes, 4, 2) != version || get_le(bytes, 6, 2) != frame_header_size ||
        get_le(bytes, 8, 4) != bytes.size()) {
      return std::nullopt;
    }
    const auto count = get_le(bytes, 12, 4);
    if (count == 0 || count > max_native_packets || count > (bytes.size() - frame_header_size) / 8) {
      return std::nullopt;
    }
    frame_view_t result {get_le(bytes, 16, 8), get_le(bytes, 24, 8), {}};
    result.packets.reserve(static_cast<std::size_t>(count));
    std::size_t offset = frame_header_size;
    for (std::size_t i = 0; i < count; ++i) {
      if (bytes.size() - offset < 4) {
        return std::nullopt;
      }
      const auto size = get_le(bytes, offset, 4);
      offset += 4;
      if (size == 0 || size % 4 != 0 || size > bytes.size() - offset) {
        return std::nullopt;
      }
      result.packets.emplace_back(bytes.subspan(offset, static_cast<std::size_t>(size)));
      offset += static_cast<std::size_t>(size);
    }
    if (offset != bytes.size()) {
      return std::nullopt;
    }
    return result;
  }

  std::optional<transport_limits_t> transport_limits(const transport_config_t &config) {
    if (config.path_mtu < 1280 || config.path_mtu > 1500 ||
        (config.ip_header_size != 20 && config.ip_header_size != 40) ||
        config.packet_size < 200 || config.packet_size > 1500 ||
        config.fec_percentage > 255 || config.min_fec_packets > 254) {
      return std::nullopt;
    }
    // packetSize includes the 16-byte NV header, but excludes RTP and encryption.
    const auto datagram_bytes = config.packet_size + 16 + (config.encrypted ? 32 : 0);
    const auto wire_bytes = datagram_bytes + config.ip_header_size + 8;
    if (wire_bytes > config.path_mtu) {
      return std::nullopt;
    }
    std::uint32_t max_data = 0;
    for (std::uint32_t data = 1; data <= 255; ++data) {
      if (data + parity_shards(data, config) <= 255) {
        max_data = data;
      }
    }
    if (max_data == 0 || (config.fec_percentage != 0 && 100 * config.min_fec_packets / max_data > 255)) {
      return std::nullopt;
    }
    const auto payload_bytes = config.packet_size - 16;
    return transport_limits_t {payload_bytes, datagram_bytes, wire_bytes, max_data,
                               4 * max_data * payload_bytes - short_frame_header_size};
  }

  std::optional<transport_plan_t> plan_transport(std::size_t frame_bytes, const transport_config_t &config) {
    const auto limits = transport_limits(config);
    if (!limits || frame_bytes < frame_header_size || frame_bytes > limits->max_frame_bytes) {
      return std::nullopt;
    }
    const auto data = static_cast<std::uint32_t>((frame_bytes + short_frame_header_size + limits->payload_bytes - 1) / limits->payload_bytes);
    transport_plan_t result;
    result.data_shards = data;
    result.block_count = (data + limits->max_data_shards - 1) / limits->max_data_shards;
    for (std::uint32_t i = 0; i < result.block_count; ++i) {
      const auto block_data = data / result.block_count + (i < data % result.block_count ? 1 : 0);
      const auto parity = parity_shards(block_data, config);
      // fecInfo has 8 bits for the adjusted percentage and 10 bits for the index.
      const auto base_parity = (block_data * config.fec_percentage + 99) / 100;
      const auto percentage = parity > base_parity ? 100 * parity / block_data : config.fec_percentage;
      if (block_data + parity > 255 || percentage > 255 ||
          (block_data * percentage + 99) / 100 != parity) {
        return std::nullopt;
      }
      result.blocks[i] = {block_data, parity};
      result.wire_bytes += (block_data + parity) * limits->on_wire_bytes;
    }
    return result;
  }

  std::uint32_t frame_budget(std::uint32_t wire_byte_budget, const transport_config_t &config) {
    const auto limits = transport_limits(config);
    if (!limits) {
      return 0;
    }
    std::uint32_t budget = 0;
    for (std::uint32_t shards = 1; shards <= 4 * limits->max_data_shards; ++shards) {
      const auto bytes = shards * limits->payload_bytes - short_frame_header_size;
      const auto plan = plan_transport(bytes, config);
      if (plan && plan->wire_bytes <= wire_byte_budget) {
        budget = bytes;
      }
    }
    return budget;
  }
}  // namespace pyrowave::protocol
