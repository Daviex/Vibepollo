#pragma once

#include "pyrowave_protocol.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pyrowave::negotiation {
  // Bitstream representability and transport budgets are checked here. Image
  // allocation, format support and performance remain adapter-specific gates.
  struct request_t {
    int width = 0;
    int height = 0;
    int framerate = 0;
    int framerate_x100 = 0;
    int encoder_bitrate_kbps = 0;
    int video_wire_bitrate_kbps = 0;
    int dynamic_range = 0;
    int chroma_sampling = 0;
    int encoder_csc_mode = 0;
    std::string_view version;
    std::string_view bitstream_revision;
    std::string_view profile;
    bool host_enabled = false;
    bool adapter_supported = false;
  };

  struct result_t {
    bool accepted = false;
    std::string reason;
    std::uint32_t frame_budget = 0;
    std::uint32_t wire_byte_budget = 0;
    std::uint32_t encoder_target_bytes = 0;
    std::uint32_t fps_x100 = 0;
    std::uint16_t negotiated_version = 0;
    ::pyrowave::profile_t profile {};
    std::uint64_t input_plane_bytes = 0;
  };

  struct rate_budget_t {
    std::uint32_t frame_budget = 0;
    std::uint32_t wire_byte_budget = 0;
    std::uint32_t encoder_target_bytes = 0;
  };

  // The admitted rate and wire budget are immutable session values. A control
  // request outside that admission, or too small for the envelope, is rejected
  // before changing public session metadata or the encoder's active budget.
  std::optional<rate_budget_t> rate_budget(
    int requested_encoder_kbps, int negotiated_encoder_kbps,
    std::uint32_t negotiated_wire_byte_budget, std::uint32_t fps_x100,
    std::uint32_t width, std::uint32_t height, const ::pyrowave::profile_t &profile,
    const protocol::transport_config_t &transport);

  result_t negotiate(const request_t &request, const protocol::transport_config_t &transport);
}  // namespace pyrowave::negotiation
