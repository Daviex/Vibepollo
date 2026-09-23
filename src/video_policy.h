#pragma once

#include "video_codec.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace video::policy {
  // Keep the negotiated cadence across callback jitter. At most one frame of
  // scheduling debt is retained; IDR requests never bypass the rate cap.
  class pyrowave_pacer_t {
  public:
    using clock = std::chrono::steady_clock;
    explicit pyrowave_pacer_t(clock::duration interval): interval(interval) {}
    bool due(clock::time_point now, bool fresh, bool idr) const {
      return !started || (now >= next_due && (fresh || idr || now - last_sent >= std::chrono::milliseconds(100)));
    }
    void sent(clock::time_point now) {
      next_due = started ? std::max(next_due + interval, now) : now + interval;
      last_sent = now;
      started = true;
    }

  private:
    clock::duration interval;
    clock::time_point next_due {}, last_sent {};
    bool started = false;
  };

  struct rational_t {
    int numerator;
    int denominator;
    friend bool operator==(const rational_t &, const rational_t &) = default;
  };

  rational_t framerate_x100_to_rational(std::int32_t value);

  struct encoder_requirements_t {
    bool hdr = false;
    bool yuv444 = false;
    codec_e codec = codec_e::h264;
  };
  struct encoder_capabilities_t {
    bool available = false;
    bool hdr = false;
    bool yuv444 = false;
    codec_mask_t supported_codecs = standard_codec_mask;
  };
  class encoder_capability_provider_t {
  public:
    virtual ~encoder_capability_provider_t() = default;
    virtual encoder_capabilities_t capabilities(std::string_view encoder) const = 0;
  };

  std::optional<std::string> select_encoder(
    std::span<const std::string_view> preference,
    encoder_requirements_t requirements,
    const encoder_capability_provider_t &provider
  );
}  // namespace video::policy
