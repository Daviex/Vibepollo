#include "pyrowave_colors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace pyrowave::colors {
  namespace {
    using rgb_t = std::array<double, 3>;
    constexpr std::uint64_t plane_byte_limit = 512ull * 1024 * 1024;

    std::uint16_t read16(const std::uint8_t *data) {
      return std::uint16_t(data[0]) | (std::uint16_t(data[1]) << 8);
    }

    double half(std::uint16_t value) {
      const auto exponent = (value >> 10) & 31;
      const auto mantissa = value & 1023;
      const auto magnitude = exponent == 0 ? std::ldexp(double(mantissa), -24) :
        exponent == 31 ? 0.0 : std::ldexp(1.0 + double(mantissa) / 1024.0, int(exponent) - 15);
      return value & 0x8000 ? -magnitude : magnitude;
    }

    double srgb_to_linear(double value) {
      return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }

    double pq_to_linear(double value) {
      const auto p = std::pow(std::clamp(value, 0.0, 1.0), 1.0 / 78.84375);
      return std::pow(std::max(p - 0.8359375, 0.0) / std::max(18.8515625 - 18.6875 * p, 1e-12),
                      1.0 / 0.1593017578125) * (10000.0 / 80.0);
    }

    double encode_transfer(double value, bool pq) {
      value = std::max(value, 0.0);
      if (!pq) {
        value = std::min(value, 1.0);
        return value < 0.018 ? 4.5 * value : 1.099 * std::pow(value, 0.45) - 0.099;
      }
      const auto p = std::pow(std::min(value * (80.0 / 10000.0), 1.0), 0.1593017578125);
      return std::pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
    }

    rgb_t to_2020(rgb_t rgb) {
      return {0.627403896 * rgb[0] + 0.329283038 * rgb[1] + 0.043313066 * rgb[2],
              0.069097289 * rgb[0] + 0.919540395 * rgb[1] + 0.011362316 * rgb[2],
              0.016391439 * rgb[0] + 0.088013308 * rgb[1] + 0.895595253 * rgb[2]};
    }

    rgb_t to_709(rgb_t rgb) {
      return {1.660491002 * rgb[0] - 0.587641139 * rgb[1] - 0.072849863 * rgb[2],
             -0.124550475 * rgb[0] + 1.132899897 * rgb[1] - 0.008349423 * rgb[2],
             -0.018150763 * rgb[0] - 0.100578898 * rgb[1] + 1.118729661 * rgb[2]};
    }

    rgb_t read_pixel(const source_t &source, int x, int y) {
      x = std::clamp(x, 0, int(source.width) - 1);
      y = std::clamp(y, 0, int(source.height) - 1);
      const auto *row = source.bytes.data() + std::size_t(y) * source.row_stride;
      if (source.format == input_format_e::bgra8_srgb) {
        const auto *p = row + std::size_t(x) * 4;
        return {srgb_to_linear(p[2] / 255.0), srgb_to_linear(p[1] / 255.0), srgb_to_linear(p[0] / 255.0)};
      }
      const auto *p = row + std::size_t(x) * 8;
      if (source.format == input_format_e::rgba16f_scrgb) {
        return {half(read16(p)), half(read16(p + 2)), half(read16(p + 4))};
      }
      return to_709({pq_to_linear(read16(p + 4) / 65535.0),
                     pq_to_linear(read16(p + 2) / 65535.0), pq_to_linear(read16(p) / 65535.0)});
    }

    rgb_t sample(const source_t &source, double x, double y) {
      const auto ix = int(std::floor(x)), iy = int(std::floor(y));
      const auto fx = x - ix, fy = y - iy;
      const auto a = read_pixel(source, ix, iy), b = read_pixel(source, ix + 1, iy);
      const auto c = read_pixel(source, ix, iy + 1), d = read_pixel(source, ix + 1, iy + 1);
      rgb_t result;
      for (unsigned i = 0; i < 3; ++i) result[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
      return result;
    }

    void store(std::vector<std::uint8_t> &data, std::size_t pixel, double value, bool high_precision) {
      const auto code = std::uint32_t(std::lround(std::clamp(value, 0.0, 1.0) * (high_precision ? 65535.0 : 255.0)));
      if (high_precision) {
        data[2 * pixel] = code & 255;
        data[2 * pixel + 1] = code >> 8;
      } else data[pixel] = code;
    }
  }

  bool convert(const source_t &source, std::uint32_t width, std::uint32_t height,
               const profile_t &profile, planes_t &output, std::string &error) {
    if (source.format != input_format_e::bgra8_srgb && source.format != input_format_e::rgba16f_scrgb &&
        source.format != input_format_e::bgra16_pq2020) {
      error = "Unsupported PyroWave source pixel format";
      return false;
    }
    const std::size_t pixel_bytes = source.format == input_format_e::bgra8_srgb ? 4 : 8;
    if (!width || !height || width > 16384 || height > 16384 ||
        (!profile.chroma_444 && ((width | height) & 1)) ||
        !source.width || !source.height || source.width > 16384 || source.height > 16384 ||
        source.row_stride < std::uint64_t(source.width) * pixel_bytes ||
        source.row_stride > std::numeric_limits<std::size_t>::max() / source.height ||
        source.bytes.size() < source.row_stride * source.height ||
        profile.input_plane_bytes(width, height) > plane_byte_limit) {
      error = "Unsupported PyroWave input geometry, stride or plane allocation";
      return false;
    }
    const auto chroma_width = profile.chroma_444 ? width : width / 2;
    const auto chroma_height = profile.chroma_444 ? height : height / 2;
    const auto bytes_per_sample = profile.high_precision ? 2u : 1u;
    try {
      output.row_strides = {std::size_t(width) * bytes_per_sample, std::size_t(chroma_width) * bytes_per_sample, std::size_t(chroma_width) * bytes_per_sample};
      output.data[0].resize(output.row_strides[0] * height);
      output.data[1].resize(output.row_strides[1] * chroma_height);
      output.data[2].resize(output.row_strides[2] * chroma_height);
      const double scale = std::min(double(width) / source.width, double(height) / source.height);
      const double left = (width - source.width * scale) / 2, top = (height - source.height * scale) / 2;
      const double kr = profile.matrix_bt2020 ? 0.2627 : 0.2126;
      const double kb = profile.matrix_bt2020 ? 0.0593 : 0.0722;
      auto yuv_at = [&](int x, int y) -> rgb_t {
        const double px = std::clamp(x, 0, int(width) - 1) + 0.5, py = std::clamp(y, 0, int(height) - 1) + 0.5;
        rgb_t rgb {};
        if (px >= left && py >= top && px < width - left && py < height - top) {
          rgb = sample(source, (px - left) / scale - 0.5, (py - top) / scale - 0.5);
          if (source.hdr && !profile.transfer_pq) {
            const auto luminance = std::max(0.0, 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]);
            if (luminance > 0.75) {
              const auto d = luminance - 0.75;
              const auto ratio = (0.75 + d / (1 + d / 0.25)) / luminance;
              for (auto &v : rgb) v *= ratio;
            }
          }
          if (profile.primaries_bt2020) rgb = to_2020(rgb);
          for (auto &v : rgb) v = encode_transfer(v, profile.transfer_pq);
        }
        const double luma = kr * rgb[0] + (1 - kr - kb) * rgb[1] + kb * rgb[2];
        return {luma, (rgb[2] - luma) / (2 * (1 - kb)), (rgb[0] - luma) / (2 * (1 - kr))};
      };
      for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        auto luma = yuv_at(x, y)[0];
        if (!profile.full_range) luma = 16.0 / 255.0 + luma * (219.0 / 255.0);
        store(output.data[0], std::size_t(y) * width + x, luma, profile.high_precision);
      }
      for (std::uint32_t y = 0; y < chroma_height; ++y) for (std::uint32_t x = 0; x < chroma_width; ++x) {
        rgb_t chroma {};
        if (profile.chroma_444) chroma = yuv_at(x, y);
        else {
          const int start = profile.chroma_left ? -1 : 0, stop = 1;
          for (int dy = 0; dy < 2; ++dy) for (int dx = start; dx <= stop; ++dx) {
            const auto s = yuv_at(int(2 * x) + dx, int(2 * y) + dy);
            const auto weight = profile.chroma_left ? (dx == 0 ? 0.25 : 0.125) : 0.25;
            chroma[1] += s[1] * weight;
            chroma[2] += s[2] * weight;
          }
        }
        for (unsigned c = 1; c < 3; ++c) {
          const auto code = 128.0 / 255.0 + chroma[c] * (profile.full_range ? 1.0 : 224.0 / 255.0);
          store(output.data[c], std::size_t(y) * chroma_width + x, code, profile.high_precision);
        }
      }
      error.clear();
      return true;
    } catch (const std::bad_alloc &) {
      error = "PyroWave CPU conversion plane allocation failed";
      return false;
    }
  }
}
