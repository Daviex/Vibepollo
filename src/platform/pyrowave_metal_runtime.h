/** Optional PyroWave Metal backend for Apple7-or-newer GPUs. */
#pragma once

#include "src/pyrowave_profile.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace platf::pyrowave_metal {
  using packet_list_t = std::vector<std::vector<std::uint8_t>>;
  bool configure_precision(int requested, int &effective, std::string &error);
  int effective_precision();

  struct frame_statistics_t {
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

  // Retains one default Metal device/library for the process. C API calls are
  // serialized; encoders are session-local. A failed context requires restart.
  // Upstream packet queries wait for GPU completion without a timeout API.
  class encoder_t {
  public:
    static std::unique_ptr<encoder_t> create(int width, int height,
      const ::pyrowave::profile_t &profile, std::string &error);
    ~encoder_t();
    encoder_t(const encoder_t &) = delete;
    encoder_t &operator=(const encoder_t &) = delete;
    std::optional<packet_list_t> encode(
      const std::array<std::span<const std::uint8_t>, 3> &planes,
      const std::array<std::size_t, 3> &row_strides,
      std::size_t target_bytes, std::string &error);
    frame_statistics_t last_frame_statistics() const;
    std::vector<std::string> performance_statistics(bool reset = false) const;
    std::string adapter_identity_text() const;

  private:
    struct impl_t;
    explicit encoder_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}
