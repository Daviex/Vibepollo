#pragma once

#include "pyrowave_protocol.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pyrowave::negotiation {
  // These are allocation ceilings for the experimental protocol, not GPU
  // performance qualifications. The adapter probe is an independent gate.
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
  };

  result_t negotiate(const request_t &request, const protocol::transport_config_t &transport);
}  // namespace pyrowave::negotiation
