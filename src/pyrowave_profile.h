#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pyrowave {
  inline constexpr std::string_view legacy_profile_name = "sdr-bt709-full-left-420";
  // Host allocation policy; bitstream dimension representability is separate.
  inline constexpr std::uint64_t maximum_input_plane_bytes = 512ULL * 1024 * 1024;

  // Input storage precision is independent of the bitstream's five VUI bits.
  // PyroWave transforms floating-point samples; it has no 8/10-bit stream flag.
  struct profile_t {
    bool chroma_444 = false;
    bool high_precision = false;
    bool full_range = true;
    bool chroma_left = true;
    bool primaries_bt2020 = false;
    bool matrix_bt2020 = false;
    bool transfer_pq = false;

    bool operator==(const profile_t &) const = default;

    constexpr int encoder_csc_mode() const {
      return (matrix_bt2020 ? 4 : 2) + (full_range ? 1 : 0);
    }

    constexpr std::uint64_t input_plane_bytes(std::uint32_t width, std::uint32_t height) const {
      const auto pixels = std::uint64_t {width} * height;
      const auto samples = chroma_444 ? pixels * 3 : pixels + pixels / 2;
      return samples * (high_precision ? 2 : 1);
    }
  };

  // v2 names spell out every independent input/VUI choice:
  // yuv{420|444}-p{8|16}-{full|limited}-{left|center}-
  // {bt709|bt2020 primaries}-{bt709|bt2020 matrix}-{bt709|pq transfer}.
  // The original v1 name remains the preferred name for its exact profile.
  inline std::string profile_name(const profile_t &profile) {
    if (profile == profile_t {}) {
      return std::string {legacy_profile_name};
    }
    std::string name = profile.chroma_444 ? "yuv444" : "yuv420";
    name += profile.high_precision ? "-p16" : "-p8";
    name += profile.full_range ? "-full" : "-limited";
    name += profile.chroma_left ? "-left" : "-center";
    name += profile.primaries_bt2020 ? "-bt2020" : "-bt709";
    name += profile.matrix_bt2020 ? "-bt2020" : "-bt709";
    name += profile.transfer_pq ? "-pq" : "-bt709";
    return name;
  }

  inline std::optional<profile_t> parse_profile(std::string_view name) {
    if (name == legacy_profile_name) {
      return profile_t {};
    }
    std::array<std::string_view, 7> parts;
    for (std::size_t index = 0; index < parts.size(); ++index) {
      const auto end = name.find('-');
      if (end == std::string_view::npos) {
        if (index != parts.size() - 1) {
          return std::nullopt;
        }
        parts[index] = name;
        name = {};
      } else {
        if (index == parts.size() - 1) {
          return std::nullopt;
        }
        parts[index] = name.substr(0, end);
        name.remove_prefix(end + 1);
      }
    }

    profile_t result;
    const auto choose = [](std::string_view value, std::string_view first, std::string_view second, bool &out) {
      if (value != first && value != second) {
        return false;
      }
      out = value == second;
      return true;
    };
    if (!choose(parts[0], "yuv420", "yuv444", result.chroma_444) ||
        !choose(parts[1], "p8", "p16", result.high_precision) ||
        !choose(parts[2], "limited", "full", result.full_range) ||
        !choose(parts[3], "center", "left", result.chroma_left) ||
        !choose(parts[4], "bt709", "bt2020", result.primaries_bt2020) ||
        !choose(parts[5], "bt709", "bt2020", result.matrix_bt2020) ||
        !choose(parts[6], "bt709", "pq", result.transfer_pq)) {
      return std::nullopt;
    }
    return result;
  }

  // Enumerate all 2^7 profiles without treating common color combinations as
  // the only legal ones. Capability advertisement can filter device support.
  inline const std::vector<std::string> &profiles() {
    static const auto names = [] {
      std::vector<std::string> result;
      result.reserve(128);
      for (unsigned bits = 0; bits < 128; ++bits) {
        result.push_back(profile_name({
          .chroma_444 = (bits & 1) != 0,
          .high_precision = (bits & 2) != 0,
          .full_range = (bits & 4) != 0,
          .chroma_left = (bits & 8) != 0,
          .primaries_bt2020 = (bits & 16) != 0,
          .matrix_bt2020 = (bits & 32) != 0,
          .transfer_pq = (bits & 64) != 0,
        }));
      }
      return result;
    }();
    return names;
  }
}  // namespace pyrowave
