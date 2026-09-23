/**
 * @file src/platform/windows/pyrowave_runtime.h
 * @brief Optional, dynamically loaded PyroWave D3D11/Vulkan encoder.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>

#include "src/pyrowave_profile.h"

namespace platf::pyrowave {
  using packet_list_t = std::vector<std::vector<std::uint8_t>>;

  // Call before the first encoder. -1 preserves the upstream environment/default.
  // After the first encoder, a changed explicit precision requires a host restart.
  bool configure_precision(int requested, int &effective, std::string &error);
  int effective_precision();


  struct frame_statistics_t {
    // CPU wall time. Encode wait includes completion of preceding D3D work;
    // these counters are not isolated GPU timestamp measurements.
    double interop_submit_us = 0;
    double encode_wait_us = 0;
    double packetize_us = 0;
    std::size_t native_bytes = 0;
    std::size_t native_packets = 0;
    // Number of initial native packets covering each pristine-band count 0..4.
    std::array<std::size_t, 5> critical_packets {};
    int active_block_bands = 3;
    std::size_t active_block_count = 0;
    std::vector<std::uint32_t> active_block_words;
  };

  /**
   * Owns imported R8/R16 luma and RG8/RG16 chroma targets and one encoder. All calls, including
   * destruction, belong to the same encode worker; frames cannot overlap.
   * The caller keeps the capture display alive until this object is destroyed.
   * A Vulkan context is retained for each capture adapter for the host process
   * lifetime; sessions share it under a process-wide C API lock. A failed adapter
   * context requires a host restart; other adapters remain independent. All
   * per-session encoder/images/fence resources are released on session teardown.
   */
  class encoder_t {
  public:
    static std::unique_ptr<encoder_t> create(
      ID3D11Device *device,
      ID3D11DeviceContext *context,
      ID3D11Texture2D *luma_target,
      ID3D11Texture2D *chroma_target,
      const LUID &adapter_luid,
      const ::pyrowave::profile_t &profile,
      std::string &error
    );

    // Compatibility overload for the original SDR BT.709 full-range 4:2:0 input.
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
    // Upstream GPU timestamps/memory diagnostics accumulated by the shared device.
    std::vector<std::string> performance_statistics(bool reset = false) const;

  private:
    struct impl_t;
    explicit encoder_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::pyrowave
