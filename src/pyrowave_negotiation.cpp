#include "pyrowave_negotiation.h"

#include <algorithm>
#include <limits>

namespace pyrowave::negotiation {
  namespace {
    result_t reject(std::string reason) {
      return {.reason = std::move(reason)};
    }

    bool in_range(int value, int minimum, int maximum) {
      return value >= minimum && value <= maximum;
    }

    std::uint64_t bytes_per_frame(int bitrate_kbps, std::uint32_t fps_x100) {
      return std::uint64_t(bitrate_kbps) * 1000 * 100 / (8 * std::uint64_t(fps_x100));
    }
  }  // namespace

  std::optional<rate_budget_t> rate_budget(
    int requested_encoder_kbps, int negotiated_encoder_kbps,
    std::uint32_t negotiated_wire_byte_budget, std::uint32_t fps_x100,
    std::uint32_t width, std::uint32_t height, const ::pyrowave::profile_t &profile,
    const protocol::transport_config_t &transport
  ) {
    if (requested_encoder_kbps <= 0 || negotiated_encoder_kbps <= 0 || requested_encoder_kbps > negotiated_encoder_kbps ||
        fps_x100 == 0 || fps_x100 > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 10) ||
        width == 0 || height == 0 || width > protocol::max_dimension || height > protocol::max_dimension ||
        (!profile.chroma_444 && ((width | height) & 1)) ||
        profile.input_plane_bytes(width, height) > maximum_input_plane_bytes) return std::nullopt;
    const auto wire_bytes = static_cast<std::uint32_t>(std::uint64_t {negotiated_wire_byte_budget} *
                                                      requested_encoder_kbps / negotiated_encoder_kbps);
    const auto frame_bytes = protocol::frame_budget(wire_bytes, transport);
    constexpr auto overhead = protocol::frame_header_size + 4 * protocol::max_native_packets;
    const auto sideband_bytes = protocol::sideband_size_bound(width, height, profile.chroma_444);
    if (!sideband_bytes) return std::nullopt;
    const auto payload_budget = transport.fragmented ?
      protocol::fragmented_payload_budget(frame_bytes, transport, *sideband_bytes) :
      frame_bytes > overhead ? static_cast<std::uint32_t>(frame_bytes - overhead) : 0;
    const auto target = std::min<std::uint64_t>(bytes_per_frame(requested_encoder_kbps, fps_x100), payload_budget) & ~std::uint64_t {3};
    if (target <= 8) return std::nullopt;
    return rate_budget_t {frame_bytes, wire_bytes, static_cast<std::uint32_t>(target)};
  }

  result_t negotiate(const request_t &request, const protocol::transport_config_t &transport) {
    if (!request.host_enabled) {
      return reject("PyroWave is disabled on the host");
    }
    if (!request.adapter_supported) {
      return reject("PyroWave is unavailable on the capture adapter");
    }
    const auto profile = parse_profile(request.profile);
    const bool legacy = request.version == "1" && request.profile == protocol::profile;
    const bool extended = request.version == "2";
    if ((!legacy && !extended) || request.bitstream_revision != protocol::bitstream_revision || !profile) {
      return reject("Unsupported PyroWave protocol version, bitstream revision or profile");
    }
    // The explicit profile supplies primaries and transfer independently. The
    // legacy fields must agree wherever they carry an equivalent choice.
    if (request.dynamic_range != static_cast<int>(profile->transfer_pq) ||
        request.chroma_sampling != static_cast<int>(profile->chroma_444) ||
        request.encoder_csc_mode != profile->encoder_csc_mode()) {
      return reject("PyroWave profile disagrees with dynamic range, chroma sampling or color matrix/range");
    }
    if (!in_range(request.width, 1, protocol::max_dimension) || !in_range(request.height, 1, protocol::max_dimension) ||
        (!profile->chroma_444 && (request.width % 2 != 0 || request.height % 2 != 0))) {
      return reject("PyroWave dimensions must be between 1 and 16384 pixels; 4:2:0 requires even dimensions");
    }
    if (profile->input_plane_bytes(static_cast<std::uint32_t>(request.width), static_cast<std::uint32_t>(request.height)) > maximum_input_plane_bytes) {
      return reject("PyroWave input planes exceed the host 512 MiB allocation limit");
    }
    if (request.framerate <= 0 || request.framerate_x100 < 0) {
      return reject("PyroWave framerate must be positive");
    }
    const auto requested_fps_x100 = request.framerate_x100 == 0 ? std::uint64_t {static_cast<unsigned>(request.framerate)} * 100 :
                                                               static_cast<std::uint64_t>(request.framerate_x100);
    // The capture pipeline also represents the rate as signed milli-FPS.
    // This is a host arithmetic ceiling, not a codec frame-rate restriction.
    if (requested_fps_x100 > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / 10)) {
      return reject("PyroWave framerate exceeds the host milli-FPS integer range");
    }
    const auto fps_x100 = static_cast<std::uint32_t>(requested_fps_x100);
    if ((requested_fps_x100 + 50) / 100 != static_cast<std::uint32_t>(request.framerate)) {
      return reject("PyroWave integer and fractional framerates are inconsistent");
    }
    if (request.encoder_bitrate_kbps <= 0 || request.video_wire_bitrate_kbps <= 0) {
      return reject("PyroWave video bitrates must be positive");
    }
    const auto limits = protocol::transport_limits(transport);
    if (!limits || transport.fragmented != !legacy) {
      return reject("PyroWave packet size, MTU or FEC configuration is unsupported");
    }
    // A low-detail frame can be much smaller than the target. It must remain
    // sendable: admitting only a valid maximum would drop every small frame
    // when minimum parity cannot be expressed by the 8-bit FEC percentage.
    if (!protocol::plan_transport(transport.fragmented ? limits->payload_bytes : protocol::frame_header_size + 8, transport)) {
      return reject("PyroWave minimum FEC parity cannot represent small frames");
    }
    const auto wire_bytes = bytes_per_frame(request.video_wire_bitrate_kbps, fps_x100);
    // The sender budget is uint32_t, but a larger requested bandwidth can be
    // safely capped here: a single frame already has a smaller transport cap.
    const auto effective_wire_bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(wire_bytes, std::numeric_limits<std::uint32_t>::max()));
    const auto budget = rate_budget(request.encoder_bitrate_kbps, request.encoder_bitrate_kbps,
      effective_wire_bytes, fps_x100, static_cast<std::uint32_t>(request.width), static_cast<std::uint32_t>(request.height), *profile, transport);
    if (!budget) {
      return reject("PyroWave encoder byte budget is too small");
    }
    return {
      .accepted = true,
      .reason = {},
      .frame_budget = budget->frame_budget,
      .wire_byte_budget = budget->wire_byte_budget,
      .encoder_target_bytes = budget->encoder_target_bytes,
      .fps_x100 = fps_x100,
      .negotiated_version = static_cast<std::uint16_t>(legacy ? 1 : protocol::profile_negotiation_version),
      .profile = *profile,
      .input_plane_bytes = profile->input_plane_bytes(static_cast<std::uint32_t>(request.width), static_cast<std::uint32_t>(request.height)),
    };
  }
}  // namespace pyrowave::negotiation
