#include "../tests_common.h"
#include "src/pyrowave_negotiation.h"

#include <array>
#include <limits>

namespace {
  namespace negotiation = pyrowave::negotiation;
  namespace protocol = pyrowave::protocol;

  negotiation::request_t valid_request() {
    return {
      .width = 1920,
      .height = 1080,
      .framerate = 60,
      .framerate_x100 = 6000,
      .encoder_bitrate_kbps = 200000,
      .video_wire_bitrate_kbps = 250000,
      .dynamic_range = 0,
      .chroma_sampling = 0,
      .encoder_csc_mode = 3,
      .version = "1",
      .bitstream_revision = protocol::bitstream_revision,
      .profile = protocol::profile,
      .host_enabled = true,
      .adapter_supported = true,
    };
  }

  void expect_rejection(const negotiation::request_t &request, const protocol::transport_config_t &transport = {}) {
    const auto result = negotiation::negotiate(request, transport);
    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.reason.empty());
    EXPECT_EQ(result.frame_budget, 0U);
    EXPECT_EQ(result.wire_byte_budget, 0U);
    EXPECT_EQ(result.encoder_target_bytes, 0U);
    EXPECT_EQ(result.fps_x100, 0U);
  }
}  // namespace

TEST(PyroWaveNegotiation, RequiresHostEnableAndAdapterSupport) {
  auto request = valid_request();
  request.host_enabled = false;
  expect_rejection(request);
  request.host_enabled = true;
  request.adapter_supported = false;
  expect_rejection(request);
}

TEST(PyroWaveNegotiation, PreservesExactWireBudgetForSenderValidation) {
  auto request = valid_request();
  request.framerate_x100 = 5994;
  const auto result = negotiation::negotiate(request, {});
  ASSERT_TRUE(result.accepted);
  EXPECT_EQ(result.wire_byte_budget, std::uint64_t(request.video_wire_bitrate_kbps) * 1000 * 100 / (8 * 5994));
  const auto plan = protocol::plan_transport(result.frame_budget, {});
  ASSERT_TRUE(plan);
  EXPECT_LE(plan->wire_bytes, result.wire_byte_budget);
}

TEST(PyroWaveNegotiation, RequiresExactExplicitExtensionContract) {
  for (const std::string_view version : {"", "0", "3", "01", "+1", "1 ", "1x", "999999999999999999999"}) {
    auto request = valid_request();
    request.version = version;
    expect_rejection(request);
  }
  for (const std::string_view revision : {"", "latest", "d2997ac", "D2997AC172BDC00E29C58E3F2938ACB7E94580BF"}) {
    auto request = valid_request();
    request.bitstream_revision = revision;
    expect_rejection(request);
  }
  for (const std::string_view profile : {"", "sdr", "sdr-bt709-full-left-444", "hdr-bt2020-full-left-420"}) {
    auto request = valid_request();
    request.profile = profile;
    expect_rejection(request);
  }
}

TEST(PyroWaveNegotiation, RejectsHdrChromaAndColorChangesWithoutDownprofiling) {
  for (const auto value : {-1, 1, 2, std::numeric_limits<int>::max()}) {
    auto request = valid_request();
    request.dynamic_range = value;
    expect_rejection(request);
    request = valid_request();
    request.chroma_sampling = value;
    expect_rejection(request);
  }
  for (const auto csc : {-1, 0, 1, 2, 4, 5, std::numeric_limits<int>::max()}) {
    auto request = valid_request();
    request.encoder_csc_mode = csc;
    expect_rejection(request);
  }
}

TEST(PyroWaveNegotiation, AcceptsEvenAllocationCeilingsIndependentOfPerformance) {
  for (const auto width : {2, 1920, 16384}) {
    for (const auto height : {2, 1080, 16384}) {
      auto request = valid_request();
      request.width = width;
      request.height = height;
      EXPECT_TRUE(negotiation::negotiate(request, {}).accepted);
    }
  }
}

TEST(PyroWaveNegotiation, RejectsOddNegativeAndExcessiveDimensions) {
  for (const auto value : {std::numeric_limits<int>::min(), -2, 0, 1, 3, 65, 16383, 16385, 16386, std::numeric_limits<int>::max()}) {
    auto request = valid_request();
    request.width = value;
    expect_rejection(request);
    request = valid_request();
    request.height = value;
    expect_rejection(request);
  }
}

TEST(PyroWaveNegotiation, AcceptsIntegerAndConsistentFractionalFramerates) {
  const std::array<std::pair<int, int>, 9> rates {{{1, 0}, {24, 2397}, {24, 2398}, {30, 2997}, {60, 5994},
                                               {60, 0}, {120, 11988}, {240, 23976}, {480, 48000}}};
  for (const auto [integer, fractional] : rates) {
    auto request = valid_request();
    request.framerate = integer;
    request.framerate_x100 = fractional;
    const auto result = negotiation::negotiate(request, {});
    ASSERT_TRUE(result.accepted) << integer << '/' << fractional << ": " << result.reason;
    EXPECT_EQ(result.fps_x100, fractional == 0 ? integer * 100U : fractional);
  }
}

TEST(PyroWaveNegotiation, RejectsInvalidOrInconsistentFrameratesBeforeArithmetic) {
  for (const auto value : {std::numeric_limits<int>::min(), -1, 0, std::numeric_limits<int>::max() / 1000 + 1, std::numeric_limits<int>::max()}) {
    auto request = valid_request();
    request.framerate = value;
    request.framerate_x100 = 0;
    expect_rejection(request);
  }
  for (const auto value : {std::numeric_limits<int>::min(), -1, 1, 99, 100, 2997, std::numeric_limits<int>::max() / 10 + 1, std::numeric_limits<int>::max()}) {
    auto request = valid_request();
    request.framerate_x100 = value;
    expect_rejection(request);
  }
}

TEST(PyroWaveNegotiation, BoundsBothBitratesBeforeMultiplication) {
  for (const auto bitrate : {std::numeric_limits<int>::min(), -1, 0, 1, 4, 5}) {
    auto request = valid_request();
    request.encoder_bitrate_kbps = bitrate;
    expect_rejection(request);
    request = valid_request();
    request.video_wire_bitrate_kbps = bitrate;
    expect_rejection(request);
  }
  for (const auto bitrate : {999, std::numeric_limits<int>::max()}) {
    auto request = valid_request();
    request.framerate = 1;
    request.framerate_x100 = 0;
    request.encoder_bitrate_kbps = bitrate;
    request.video_wire_bitrate_kbps = bitrate;
    const auto result = negotiation::negotiate(request, {});
    EXPECT_TRUE(result.accepted) << result.reason;
  }
}

TEST(PyroWaveNegotiation, UsesWideArithmeticAndCapsMaximumBitrateToTransport) {
  auto request = valid_request();
  request.framerate = 1;
  request.framerate_x100 = 0;
  request.encoder_bitrate_kbps = std::numeric_limits<int>::max();
  request.video_wire_bitrate_kbps = std::numeric_limits<int>::max();
  const auto result = negotiation::negotiate(request, {});
  ASSERT_TRUE(result.accepted);
  const auto limits = protocol::transport_limits({});
  ASSERT_TRUE(limits);
  EXPECT_EQ(result.frame_budget, limits->max_frame_bytes);
  EXPECT_EQ(result.encoder_target_bytes, (result.frame_budget - protocol::frame_header_size - 4 * protocol::max_native_packets) & ~3U);
}

TEST(PyroWaveNegotiation, ReservesWorstCasePacketLengthOverheadAndAlignsNativeBytes) {
  for (const auto rate : {2397, 5994, 11988, 23976}) {
    for (const auto encoder_kbps : {1000, 123457, 200000, 800000}) {
      auto request = valid_request();
      request.framerate = (rate + 50) / 100;
      request.framerate_x100 = rate;
      request.encoder_bitrate_kbps = encoder_kbps;
      const auto result = negotiation::negotiate(request, {});
      ASSERT_TRUE(result.accepted) << result.reason;
      const auto wire_bytes = std::uint64_t(request.video_wire_bitrate_kbps) * 100000 / (8ULL * rate);
      const auto native_bytes = std::uint64_t(encoder_kbps) * 100000 / (8ULL * rate);
      constexpr auto reserve = protocol::frame_header_size + 4 * protocol::max_native_packets;
      EXPECT_EQ(result.frame_budget, protocol::frame_budget(static_cast<std::uint32_t>(wire_bytes), {}));
      const auto expected_target = std::min<std::uint64_t>(native_bytes, result.frame_budget - reserve) & ~3ULL;
      EXPECT_EQ(result.encoder_target_bytes, expected_target);
      EXPECT_EQ(result.encoder_target_bytes % 4, 0U);
      EXPECT_GT(result.encoder_target_bytes, 8U);
      EXPECT_LE(result.encoder_target_bytes + reserve, result.frame_budget);
    }
  }
}

TEST(PyroWaveNegotiation, RefusesWireBudgetThatCannotCarryConservativeEnvelope) {
  auto request = valid_request();
  request.video_wire_bitrate_kbps = 1000;
  request.encoder_bitrate_kbps = 1000;
  expect_rejection(request);
}

TEST(PyroWaveNegotiation, RejectsInvalidTransportAndIncludesEncryptionInBudget) {
  const auto request = valid_request();
  protocol::transport_config_t transport;
  transport.packet_size = 1500;
  expect_rejection(request, transport);
  transport = {};
  const auto plain = negotiation::negotiate(request, transport);
  ASSERT_TRUE(plain.accepted);
  transport.encrypted = true;
  const auto encrypted = negotiation::negotiate(request, transport);
  ASSERT_TRUE(encrypted.accepted);
  EXPECT_LT(encrypted.frame_budget, plain.frame_budget);
  transport.min_fec_packets = 254;
  expect_rejection(request, transport);
}

TEST(PyroWaveNegotiation, RequiresFecToRepresentSmallFramesAsWellAsTargetBudget) {
  const auto request = valid_request();
  protocol::transport_config_t transport;
  transport.min_fec_packets = 4;
  EXPECT_GT(protocol::frame_budget(500000, transport), 0U);
  expect_rejection(request, transport);
  transport.min_fec_packets = 2;
  ASSERT_TRUE(negotiation::negotiate(request, transport).accepted);
  transport.min_fec_packets = 3;
  expect_rejection(request, transport);
  transport.fec_percentage = 255;
  ASSERT_TRUE(negotiation::negotiate(request, transport).accepted);
  transport.fec_percentage = 0;
  transport.min_fec_packets = 254;  // Minimum parity is ignored when FEC is off.
  ASSERT_TRUE(negotiation::negotiate(request, transport).accepted);
}
