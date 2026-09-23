#pragma once

#include "pyrowave_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace pyrowave::protocol {
  // Private VibePollo extension; these are not GameStream codec capability bits.
  inline constexpr std::uint16_t version = 1;
  // Negotiation v2 extends profile selection; the PWVF envelope stays v1.
  inline constexpr std::uint16_t profile_negotiation_version = 2;
  inline constexpr std::string_view legacy_transport_name = "complete-v1";
  inline constexpr std::string_view fragmented_transport_name = "fragments-v2";
  inline constexpr std::string_view bitstream_revision = "d2997ac172bdc00e29c58e3f2938acb7e94580bf";
  inline constexpr std::string_view profile = legacy_profile_name;
  inline constexpr std::uint32_t max_dimension = 16384;
  inline constexpr std::size_t frame_header_size = 32;
  inline constexpr std::size_t max_native_packets = 4096;
  inline constexpr std::size_t max_frame_size = 4 * 1024 * 1024;
  inline constexpr std::uint32_t short_frame_header_size = 8;
  inline constexpr std::size_t fragment_header_size = 48;
  inline constexpr std::size_t fragment_record_header_size = 16;
  inline constexpr std::size_t max_sideband_words = 32768;
  inline constexpr std::size_t native_packet_boundary = 65536;

  struct frame_view_t {
    std::uint64_t frame_index;
    std::uint64_t timestamp_us;
    std::vector<std::span<const std::uint8_t>> packets;
  };

  // PWVF uses explicit little-endian fields, never a packed native C++ struct.
  std::optional<std::vector<std::uint8_t>> serialize_frame(
    std::uint64_t frame_index,
    std::uint64_t timestamp_us,
    std::span<const std::span<const std::uint8_t>> packets,
    std::size_t byte_limit
  );
  std::optional<frame_view_t> parse_frame(std::span<const std::uint8_t> bytes);

  struct transport_config_t {
    std::uint32_t packet_size = 1024;  // GameStream packetSize, excludes 16-byte RTP header.
    std::uint32_t path_mtu = 1280;  // IP packet including IP/UDP and encryption headers.
    std::uint32_t ip_header_size = 40;  // 20 for IPv4, 40 for IPv6.
    std::uint32_t fec_percentage = 20;
    std::uint32_t min_fec_packets = 0;
    bool encrypted = false;
    bool fragmented = false;
    std::uint32_t critical_fec_percentage = 40;
  };

  struct transport_limits_t {
    std::uint32_t payload_bytes;
    std::uint32_t datagram_bytes;  // UDP payload, including encryption prefix.
    std::uint32_t on_wire_bytes;  // IP packet size; excludes link-layer overhead.
    std::uint32_t max_data_shards;
    std::uint32_t max_frame_bytes;  // PWVF bytes, excludes GameStream short frame header.
  };

  struct fec_block_t {
    std::uint32_t data_shards = 0;
    std::uint32_t parity_shards = 0;
    // UINT32_MAX preserves the original caller-supplied FEC configuration.
    std::uint32_t fec_percentage = 0xffffffffU;
  };

  struct transport_plan_t {
    std::array<fec_block_t, 4> blocks {};
    std::uint32_t block_count = 0;
    std::uint32_t data_shards = 0;
    std::uint32_t wire_bytes = 0;
  };

  std::optional<transport_limits_t> transport_limits(const transport_config_t &config);
  std::optional<transport_plan_t> plan_transport(std::size_t frame_bytes, const transport_config_t &config);
  // Largest PWVF frame which fits both transport limits and the per-frame IP budget.
  // A zero result means that the request cannot carry even a frame envelope.
  std::uint32_t frame_budget(std::uint32_t wire_byte_budget, const transport_config_t &config);

  struct partial_metadata_t {
    std::array<std::uint32_t, 5> critical_packets {};
    std::uint32_t active_block_bands = 3;
    std::uint32_t active_block_count = 0;
    std::span<const std::uint32_t> active_block_words;
  };

  struct fragment_record_view_t {
    bool manifest = false;
    bool critical = false;
    std::uint32_t item_index = 0;
    std::uint32_t item_size = 0;
    std::uint32_t item_offset = 0;
    std::span<const std::uint8_t> data;
  };

  struct fragment_view_t {
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp_us = 0;
    std::uint16_t shard_index = 0;
    std::uint16_t shard_count = 0;
    std::uint16_t native_packet_count = 0;
    std::uint16_t critical_packet_count = 0;
    std::uint8_t active_block_bands = 0;
    std::uint32_t active_block_count = 0;
    std::array<std::uint8_t, 8> sequence_header {};
    std::vector<fragment_record_view_t> records;
  };

  struct partial_frame_t {
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp_us = 0;
    std::array<std::uint8_t, 8> sequence_header {};
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<std::uint32_t> packet_indices;
    std::array<std::uint32_t, 5> critical_packets {};
    std::uint32_t active_block_bands = 0;
    std::uint32_t active_block_count = 0;
    std::vector<std::uint32_t> active_block_words;
    std::uint32_t missing_packets = 0;
    bool manifest_complete = false;
    bool critical_complete = false;
    bool complete = false;
  };

  // Size of the five critical-count words plus the active-block mask. This
  // mirrors upstream's padded block mapping and performs no GPU allocation.
  std::optional<std::uint32_t> sideband_size_bound(
    std::uint32_t width, std::uint32_t height, bool chroma_444, std::uint32_t bands = 3);

  // Conservative native-byte budget for upstream's 64 KiB packet boundary.
  // frame_byte_budget includes all PWPF headers, records, manifest and padding.
  std::uint32_t fragmented_payload_budget(
    std::uint32_t frame_byte_budget, const transport_config_t &config, std::uint32_t sideband_byte_limit);

  std::optional<std::vector<std::uint8_t>> serialize_fragmented_frame(
    std::uint64_t frame_index,
    std::uint64_t timestamp_us,
    std::span<const std::span<const std::uint8_t>> packets,
    const partial_metadata_t &metadata,
    const transport_config_t &config,
    std::size_t byte_limit);

  // Receives a single decrypted/recovered UDP data-shard payload, after the
  // existing RTP/NV header. It never depends on another shard's bytes.
  std::optional<fragment_view_t> parse_fragment(std::span<const std::uint8_t> payload);

  // Sender validation/planning for a complete concatenation of PWPF slots.
  std::optional<transport_plan_t> plan_fragmented_transport(
    std::span<const std::uint8_t> frame, const transport_config_t &config);

  // Reference receiver seam: tolerates reordering and identical duplicates;
  // rejects conflicts, bounds all declarations before allocating packet data,
  // and emits only complete native packets. Push sequence_header separately
  // before these packets when packet zero was not recovered.
  std::optional<partial_frame_t> reassemble_partial_frame(
    std::span<const std::span<const std::uint8_t>> payloads, const transport_config_t &config);
}  // namespace pyrowave::protocol
