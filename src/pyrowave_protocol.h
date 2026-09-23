#pragma once

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
  inline constexpr std::string_view bitstream_revision = "d2997ac172bdc00e29c58e3f2938acb7e94580bf";
  inline constexpr std::string_view profile = "sdr-bt709-full-left-420";
  inline constexpr std::size_t frame_header_size = 32;
  inline constexpr std::size_t max_native_packets = 4096;
  inline constexpr std::size_t max_frame_size = 4 * 1024 * 1024;
  inline constexpr std::uint32_t short_frame_header_size = 8;

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
}  // namespace pyrowave::protocol
