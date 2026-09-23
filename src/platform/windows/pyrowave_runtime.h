/**
 * @file src/platform/windows/pyrowave_runtime.h
 * @brief Optional, dynamically loaded PyroWave D3D11/Vulkan encoder.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>

namespace platf::pyrowave {
  using packet_list_t = std::vector<std::vector<std::uint8_t>>;

  struct frame_statistics_t {
    // CPU wall time. Encode wait includes completion of preceding D3D work;
    // these counters are not isolated GPU timestamp measurements.
    double interop_submit_us = 0;
    double encode_wait_us = 0;
    double packetize_us = 0;
    std::size_t native_bytes = 0;
    std::size_t native_packets = 0;
  };

  /**
   * Owns imported R8 luma / R8G8 chroma targets and one encoder. All calls, including
   * destruction, belong to the same encode worker; frames cannot overlap.
   * The caller keeps the capture display alive until this object is destroyed.
   * One Vulkan context and DLL are retained for the first adapter for the host
   * process lifetime. Changing GPU or recovering a failed context requires a
   * host restart; per-session encoder/images/fence are always released.
   */
  class encoder_t {
  public:
    static std::unique_ptr<encoder_t> create(
      ID3D11Device *device,
      ID3D11DeviceContext *context,
      ID3D11Texture2D *luma_target,
      ID3D11Texture2D *chroma_target,
      const LUID &adapter_luid,
      std::string &error
    );

    ~encoder_t();
    encoder_t(const encoder_t &) = delete;
    encoder_t &operator=(const encoder_t &) = delete;

    // Call before rendering into the target, then after conversion completes.
    bool prepare_target(std::string &error);
    bool submit_conversion(std::string &error);

    // Returns native upstream packets; the video layer adds the PWVF envelope.
    std::optional<packet_list_t> encode(std::size_t target_bytes, std::string &error);
    frame_statistics_t last_frame_statistics() const;

  private:
    struct impl_t;
    explicit encoder_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::pyrowave
