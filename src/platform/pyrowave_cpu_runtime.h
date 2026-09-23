#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/pyrowave_profile.h"

namespace platf::pyrowave_cpu {
  using packet_list_t = std::vector<std::vector<std::uint8_t>>;
  using plane_views_t = std::array<std::span<const std::uint8_t>, 3>;
  using plane_strides_t = std::array<std::size_t, 3>;

  struct adapter_identity_t {
    std::string name;
    std::array<std::uint8_t, 16> uuid {};
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;

    std::string uuid_string() const;
  };

  struct frame_statistics_t {
    // CPU wall time. This bridge uploads CPU planes and still encodes on Vulkan.
    double interop_submit_us = 0;
    double encode_wait_us = 0;
    double packetize_us = 0;
    std::size_t native_bytes = 0;
    std::size_t native_packets = 0;
    std::array<std::size_t, 5> critical_packets {};
    int active_block_bands = 3;
    std::size_t active_block_count = 0;
    std::vector<std::uint32_t> active_block_words;
  };

  bool configure_precision(int requested, int &effective, std::string &error);
  int effective_precision();

  /**
   * Portable CPU-to-Vulkan input bridge for Linux x86_64/ARM64.
   * All encoder operations belong to one worker; calls from different encoder
   * instances are serialized while using the shared Vulkan C API.
   * A process-lifetime context is retained for each requested device selector.
   * This avoids repeated driver device creation between sessions and probes.
   */
  class encoder_t {
  public:
    // Empty UUID selects the Vulkan default GPU. A UUID selects the encoder GPU,
    // independently of the display/capture GPU used by the CPU input bridge.
    static std::unique_ptr<encoder_t> create(
      int width,
      int height,
      const ::pyrowave::profile_t &profile,
      std::string &error,
      std::string_view device_uuid = {}
    );

    ~encoder_t();
    encoder_t(const encoder_t &) = delete;
    encoder_t &operator=(const encoder_t &) = delete;

    // Three separate Y/Cb/Cr planes. The profile selects R8 or little-endian
    // UNORM16 storage and half/full chroma dimensions. Padding is allowed.
    std::optional<packet_list_t> encode(
      const plane_views_t &planes,
      const plane_strides_t &row_strides,
      std::size_t target_bytes,
      std::string &error
    );

    adapter_identity_t adapter_identity() const;
    std::string adapter_identity_text() const;
    frame_statistics_t last_frame_statistics() const;
    // Upstream GPU timestamps and memory diagnostics, accumulated per device.
    std::vector<std::string> performance_statistics(bool reset = false) const;

  private:
    struct impl_t;
    explicit encoder_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::pyrowave_cpu
