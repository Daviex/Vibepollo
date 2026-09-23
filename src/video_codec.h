/**
 * @file src/video_codec.h
 * @brief Codec identities shared by negotiation, selection and diagnostics.
 */
#pragma once

#include <cstddef>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace video {

  // Values 0-2 are the existing GameStream wire values. Value 3 is private to
  // the versioned VibePollo PyroWave extension and requires explicit negotiation.
  // Recognizing a value here does not imply that its backend is available.
  enum class codec_e : int {
    h264 = 0,
    hevc = 1,
    av1 = 2,
    pyrowave = 3,
  };

  constexpr std::size_t standard_codec_count = 3;
  using codec_mask_t = std::uint32_t;

  constexpr int codec_wire_value(codec_e codec) {
    return static_cast<int>(codec);
  }

  constexpr std::optional<codec_e> codec_from_wire(int value) {
    switch (value) {
      case 0:
        return codec_e::h264;
      case 1:
        return codec_e::hevc;
      case 2:
        return codec_e::av1;
      case 3:
        return codec_e::pyrowave;
      default:
        return std::nullopt;
    }
  }

  constexpr bool is_standard_codec(codec_e codec) {
    return codec == codec_e::h264 || codec == codec_e::hevc || codec == codec_e::av1;
  }

  inline std::optional<codec_e> parse_wire_codec(std::string_view value) {
    if (value.empty() || value.front() < '0' || value.front() > '9') {
      return std::nullopt;
    }
    int wire = -1;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), wire);
    if (error != std::errc {} || end != value.data() + value.size()) {
      return std::nullopt;
    }
    return codec_from_wire(wire);
  }

  constexpr bool is_standard_wire_codec(int value) {
    const auto codec = codec_from_wire(value);
    return codec && is_standard_codec(*codec);
  }

  // Only standard codecs index the legacy three-slot capability arrays.
  constexpr std::optional<std::size_t> standard_codec_index(codec_e codec) {
    return is_standard_codec(codec) ? std::optional<std::size_t>(codec_wire_value(codec)) : std::nullopt;
  }

  constexpr codec_mask_t codec_bit(codec_e codec) {
    return codec_from_wire(codec_wire_value(codec)) ? (codec_mask_t {1} << codec_wire_value(codec)) : 0;
  }

  constexpr codec_mask_t standard_codec_mask =
    codec_bit(codec_e::h264) | codec_bit(codec_e::hevc) | codec_bit(codec_e::av1);

  constexpr std::string_view codec_name(codec_e codec) {
    switch (codec) {
      case codec_e::h264:
        return "H.264";
      case codec_e::hevc:
        return "HEVC";
      case codec_e::av1:
        return "AV1";
      case codec_e::pyrowave:
        return "PyroWave";
      default:
        return "unknown";
    }
  }

  constexpr std::string_view codec_name_from_wire(int value) {
    const auto codec = codec_from_wire(value);
    return codec ? codec_name(*codec) : "unknown";
  }

}  // namespace video
