#include "../tests_common.h"

#include "src/video_policy.h"

#include <array>
#include <map>
#include <limits>

namespace {
  class FakeEncoderProvider: public video::policy::encoder_capability_provider_t {
  public:
    std::map<std::string, video::policy::encoder_capabilities_t> values;
    video::policy::encoder_capabilities_t capabilities(std::string_view encoder) const override {
      const auto found = values.find(std::string(encoder));
      return found == values.end() ? video::policy::encoder_capabilities_t {} : found->second;
    }
  };
}

TEST(EncoderPolicy, SelectsFirstAvailableCapableEncoderWithoutHardwareProbe) {
  FakeEncoderProvider provider;
  provider.values["nvenc"] = {false, true, true};
  provider.values["software"] = {true, true, false};
  const std::array<std::string_view, 2> preference {"nvenc", "software"};
  EXPECT_EQ(video::policy::select_encoder(preference, {.hdr = true}, provider), "software");
}

TEST(EncoderPolicy, RejectsEncoderThatCannotMeetRequestedFormat) {
  FakeEncoderProvider provider;
  provider.values["software"] = {true, true, false};
  const std::array<std::string_view, 1> preference {"software"};
  EXPECT_FALSE(video::policy::select_encoder(preference, {.hdr = true, .yuv444 = true}, provider));
}

TEST(PyroWavePacing, IdrCannotBypassFrameRateAndStaticFramesCanRepeat) {
  using namespace std::chrono_literals;
  const auto start = video::policy::pyrowave_pacer_t::clock::time_point {};
  video::policy::pyrowave_pacer_t pacer(10ms);
  EXPECT_TRUE(pacer.due(start, true, false));
  pacer.sent(start);
  EXPECT_FALSE(pacer.due(start + 1ms, true, true));
  EXPECT_FALSE(pacer.due(start + 9ms, false, true));
  EXPECT_TRUE(pacer.due(start + 10ms, false, true));
  EXPECT_FALSE(pacer.due(start + 99ms, false, false));
  EXPECT_TRUE(pacer.due(start + 100ms, false, false));
}

TEST(PyroWavePacing, CallbackJitterDoesNotKeepHalvingFrameRate) {
  using namespace std::chrono_literals;
  const auto start = video::policy::pyrowave_pacer_t::clock::time_point {};
  video::policy::pyrowave_pacer_t pacer(10ms);
  pacer.sent(start);
  int frames = 0;
  for (int tick = 1; tick <= 100; ++tick) {
    const auto now = start + tick * 10ms + (tick % 2 ? -1us : 1us);
    if (pacer.due(now, true, false)) {
      ++frames;
      pacer.sent(now);
    }
  }
  EXPECT_GE(frames, 98);
  // Long idle periods cannot accumulate an unbounded burst of frame credits.
  const auto later = start + 10s;
  int burst = 0;
  while (pacer.due(later, true, true) && burst < 10) {
    pacer.sent(later);
    ++burst;
  }
  EXPECT_LE(burst, 2);
}

TEST(VideoCodec, PreservesStandardWireValuesAndRecognizesPrivateCodec) {
  EXPECT_EQ(video::codec_from_wire(0), video::codec_e::h264);
  EXPECT_EQ(video::codec_from_wire(1), video::codec_e::hevc);
  EXPECT_EQ(video::codec_from_wire(2), video::codec_e::av1);
  EXPECT_EQ(video::codec_from_wire(3), video::codec_e::pyrowave);
  EXPECT_EQ(video::codec_wire_value(video::codec_e::pyrowave), 3);
  EXPECT_EQ(video::codec_name_from_wire(3), "PyroWave");
}

TEST(VideoCodec, UnknownValuesNeverAliasH264) {
  for (const auto value : {-1, 4, 255, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
    EXPECT_FALSE(video::codec_from_wire(value));
    EXPECT_FALSE(video::is_standard_wire_codec(value));
    EXPECT_EQ(video::codec_name_from_wire(value), "unknown");
    EXPECT_EQ(video::codec_bit(static_cast<video::codec_e>(value)), 0U);
  }
}

TEST(VideoCodec, ParsesOnlyCompleteUnsignedWireCodecValues) {
  EXPECT_EQ(video::parse_wire_codec("0"), video::codec_e::h264);
  EXPECT_EQ(video::parse_wire_codec("1"), video::codec_e::hevc);
  EXPECT_EQ(video::parse_wire_codec("2"), video::codec_e::av1);
  EXPECT_EQ(video::parse_wire_codec("3"), video::codec_e::pyrowave);
  for (const std::string_view value : {"", " ", " 0", "0 ", "3\n", "+0", "-0", "+3", "-1", "4", "3x", "1.0",
                                      "0x0", "h264", "2147483648", "-2147483649", "99999999999999999999999999999"}) {
    EXPECT_FALSE(video::parse_wire_codec(value)) << "input='" << value << "'";
  }
  EXPECT_FALSE(video::parse_wire_codec(std::string_view("0\0", 2)));
}

TEST(VideoCodec, PrivateCodecCannotIndexStandardCapabilities) {
  EXPECT_EQ(video::standard_codec_count, 3U);
  EXPECT_EQ(video::standard_codec_index(video::codec_e::h264), 0U);
  EXPECT_EQ(video::standard_codec_index(video::codec_e::hevc), 1U);
  EXPECT_EQ(video::standard_codec_index(video::codec_e::av1), 2U);
  EXPECT_FALSE(video::standard_codec_index(video::codec_e::pyrowave));
  EXPECT_FALSE(video::is_standard_wire_codec(3));
  EXPECT_EQ(video::standard_codec_mask & video::codec_bit(video::codec_e::pyrowave), 0U);
}

TEST(EncoderPolicy, StandardEncoderCannotSatisfyPyroWaveRequest) {
  FakeEncoderProvider provider;
  provider.values["software"] = {true, true, true};
  const std::array<std::string_view, 1> preference {"software"};
  EXPECT_FALSE(video::policy::select_encoder(preference, {.codec = video::codec_e::pyrowave}, provider));
}

TEST(EncoderPolicy, DedicatedPyroWaveBackendNeverAdvertisesH264) {
  FakeEncoderProvider provider;
  provider.values["pyrowave"] = {true, false, false, video::codec_bit(video::codec_e::pyrowave)};
  provider.values["software"] = {true, false, false};
  const std::array<std::string_view, 2> preference {"pyrowave", "software"};
  EXPECT_EQ(video::policy::select_encoder(preference, {}, provider), "software");
  EXPECT_EQ(video::policy::select_encoder(preference, {.codec = video::codec_e::pyrowave}, provider), "pyrowave");
  EXPECT_FALSE(video::policy::select_encoder(preference, {.hdr = true, .codec = video::codec_e::pyrowave}, provider));
  EXPECT_FALSE(video::policy::select_encoder(preference, {.yuv444 = true, .codec = video::codec_e::pyrowave}, provider));
}

TEST(EncoderPolicy, UnknownCodecCannotSelectAnyEncoder) {
  FakeEncoderProvider provider;
  provider.values["software"] = {true, true, true};
  const std::array<std::string_view, 1> preference {"software"};
  EXPECT_FALSE(video::policy::select_encoder(preference, {.codec = static_cast<video::codec_e>(99)}, provider));
}

struct FramerateX100Test: testing::TestWithParam<std::tuple<std::int32_t, video::policy::rational_t>> {};
TEST_P(FramerateX100Test, Run) {
  const auto &[value, expected] = GetParam();
  EXPECT_EQ(video::policy::framerate_x100_to_rational(value), expected);
}
INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, video::policy::rational_t {24000, 1001}),
    std::make_tuple(2398, video::policy::rational_t {24000, 1001}),
    std::make_tuple(2500, video::policy::rational_t {25, 1}),
    std::make_tuple(2997, video::policy::rational_t {30000, 1001}),
    std::make_tuple(6000, video::policy::rational_t {60, 1}),
    std::make_tuple(11988, video::policy::rational_t {120000, 1001}),
    std::make_tuple(23976, video::policy::rational_t {240000, 1001}),
    std::make_tuple(9498, video::policy::rational_t {4749, 50})
  )
);
