#pragma once

#include "pyrowave_protocol.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace crypto::cipher {
  class gcm_t;
}

namespace pyrowave::transport {
  inline constexpr std::size_t packet_header_bytes = 32;
  inline constexpr std::size_t encryption_prefix_bytes = 32;

  struct block_info_t {
    std::uint32_t frame_index;
    std::uint32_t sequence_base;
    std::uint32_t timestamp_90khz;
    std::uint32_t block_index;
    std::uint32_t block_count;
  };

  struct encoded_block_t {
    std::uint32_t data_shards;
    std::uint32_t parity_shards;
    std::uint32_t percentage;
    std::uint32_t shard_bytes;
    std::uint32_t prefix_bytes;
    std::vector<std::uint8_t> shards;
    std::vector<std::uint8_t> prefixes;

    std::size_t size() const { return data_shards + parity_shards; }
    std::span<const std::uint8_t> shard(std::size_t index) const {
      return std::span(shards).subspan(index * shard_bytes, shard_bytes);
    }
    std::span<const std::uint8_t> prefix(std::size_t index) const {
      return std::span(prefixes).subspan(index * prefix_bytes, prefix_bytes);
    }
  };

  // Produces the same short-frame header and reserved NV/RTP slots consumed by
  // the sender. The final data shard remains short until encode_block pads it.
  std::optional<std::vector<std::uint8_t>> packetize_frame(
    std::span<const std::uint8_t> frame,
    const protocol::transport_config_t &config,
    std::uint16_t processing_latency = 0
  );

  // Uses the production Reed-Solomon implementation and negotiated AES-GCM
  // object. reed_solomon_init() must already have been called by application
  // initialization. The cipher and IV counter belong to a single send worker.
  std::optional<encoded_block_t> encode_block(
    std::span<const std::uint8_t> data,
    const protocol::fec_block_t &planned,
    const protocol::transport_config_t &config,
    const block_info_t &info,
    crypto::cipher::gcm_t *cipher,
    std::uint64_t &iv_counter
  );
}  // namespace pyrowave::transport
