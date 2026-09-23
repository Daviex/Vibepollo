#pragma once

#include "pyrowave_profile.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pyrowave::colors {
  enum class input_format_e { bgra8_srgb, rgba16f_scrgb, bgra16_pq2020 };

  struct source_t {
    std::span<const std::uint8_t> bytes;
    std::uint32_t width = 0, height = 0;
    std::size_t row_stride = 0;
    input_format_e format = input_format_e::bgra8_srgb;
    bool hdr = false;
  };

  struct planes_t {
    std::array<std::vector<std::uint8_t>, 3> data;
    std::array<std::size_t, 3> row_strides {};
    std::array<std::span<const std::uint8_t>, 3> views() const {
      return {data[0], data[1], data[2]};
    }
  };

  // Portable capture bridge. Encoding still runs on Vulkan/Metal; this path
  // trades CPU conversion and an upload for broad system-memory capture support.
  // Sixteen-bit planes are little-endian UNORM, without P010 quantization.
  bool convert(const source_t &source, std::uint32_t width, std::uint32_t height,
               const profile_t &profile, planes_t &output, std::string &error);
}
