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

  result_t negotiate(const request_t &request, const protocol::transport_config_t &transport) {
    if (!request.host_enabled) {
      return reject("PyroWave is disabled on the host");
    }
    if (!request.adapter_supported) {
      return reject("PyroWave is unavailable on the capture adapter");
    }
    if (request.version != "1" || request.bitstream_revision != protocol::bitstream_revision || request.profile != protocol::profile) {
      return reject("Unsupported PyroWave protocol version, bitstream revision or profile");
    }
    if (request.dynamic_range != 0 || request.chroma_sampling != 0 || request.encoder_csc_mode != 3) {
      return reject("PyroWave requires SDR BT.709 full-range 4:2:0");
    }
    if (!in_range(request.width, 64, 4096) || !in_range(request.height, 64, 4096) ||
        request.width % 2 != 0 || request.height % 2 != 0) {
      return reject("PyroWave dimensions must be even and between 64 and 4096 pixels");
    }
    if (!in_range(request.framerate, 1, 240) ||
        (request.framerate_x100 != 0 && !in_range(request.framerate_x100, 100, 24000))) {
      return reject("PyroWave framerate must be between 1 and 240 fps");
    }
    const auto fps_x100 = static_cast<std::uint32_t>(request.framerate_x100 == 0 ? request.framerate * 100 : request.framerate_x100);
    if ((fps_x100 + 50) / 100 != static_cast<std::uint32_t>(request.framerate)) {
      return reject("PyroWave integer and fractional framerates are inconsistent");
    }
    if (!in_range(request.encoder_bitrate_kbps, 1000, 800000) || !in_range(request.video_wire_bitrate_kbps, 1000, 800000)) {
      return reject("PyroWave video bitrates must be between 1000 and 800000 Kbps");
    }
    if (!protocol::transport_limits(transport)) {
      return reject("PyroWave packet size, MTU or FEC configuration is unsupported");
    }
    // A low-detail frame can be much smaller than the target. It must remain
    // sendable: admitting only a valid maximum would drop every small frame
    // when minimum parity cannot be expressed by the 8-bit FEC percentage.
    if (!protocol::plan_transport(protocol::frame_header_size + 8, transport)) {
      return reject("PyroWave minimum FEC parity cannot represent small frames");
    }
    const auto wire_bytes = bytes_per_frame(request.video_wire_bitrate_kbps, fps_x100);
    const auto encoder_bytes = bytes_per_frame(request.encoder_bitrate_kbps, fps_x100);
    if (wire_bytes > std::numeric_limits<std::uint32_t>::max()) {
      return reject("PyroWave per-frame wire budget exceeds the supported range");
    }
    const auto frame_budget = protocol::frame_budget(static_cast<std::uint32_t>(wire_bytes), transport);
    // Reserve a length word for every permitted native packet. The actual
    // envelope is generally smaller; this ceiling guarantees safe allocation
    // before the encoder has selected its packet count.
    constexpr auto envelope_overhead = protocol::frame_header_size + 4 * protocol::max_native_packets;
    if (frame_budget <= envelope_overhead + 8) {
      return reject("PyroWave bandwidth cannot carry the frame envelope and native payload");
    }
    const auto target = std::min<std::uint64_t>(encoder_bytes, frame_budget - envelope_overhead) & ~std::uint64_t {3};
    if (target <= 8) {
      return reject("PyroWave encoder byte budget is too small");
    }
    return {
      .accepted = true,
      .reason = {},
      .frame_budget = frame_budget,
      .wire_byte_budget = static_cast<std::uint32_t>(wire_bytes),
      .encoder_target_bytes = static_cast<std::uint32_t>(target),
      .fps_x100 = fps_x100,
    };
  }
}  // namespace pyrowave::negotiation
