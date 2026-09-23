#include "../tests_common.h"
#include "src/pyrowave_protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace {
  namespace protocol = pyrowave::protocol;
  using bytes_t = std::vector<std::uint8_t>;

  bytes_t fixture() {
    return {
      'P', 'W', 'V', 'F', 1, 0, 32, 0,
      52, 0, 0, 0, 2, 0, 0, 0,
      1, 2, 3, 4, 5, 6, 7, 8,
      17, 18, 19, 20, 21, 22, 23, 24,
      4, 0, 0, 0, 1, 2, 3, 4,
      8, 0, 0, 0, 10, 11, 12, 13, 14, 15, 16, 17,
    };
  }

  void overwrite_le(bytes_t &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
      bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }

  std::uint32_t divide_up(std::uint32_t numerator, std::uint32_t denominator) {
    return numerator / denominator + (numerator % denominator != 0);
  }

  // Model the receiver's reconstruction from the 8-bit percentage. This uses
  // only the reported data count and transmitted percentage, not planner fields.
  std::uint32_t receiver_parity(std::uint32_t data, std::uint32_t percentage) {
    return divide_up(data * percentage, 100);
  }

  void expect_valid_plan(const protocol::transport_plan_t &plan, const protocol::transport_config_t &config) {
    ASSERT_GE(plan.block_count, 1U);
    ASSERT_LE(plan.block_count, 4U);
    std::uint32_t actual_data = 0;
    std::uint32_t total_shards = 0;
    for (std::size_t i = 0; i < plan.blocks.size(); ++i) {
      const auto &block = plan.blocks[i];
      if (i >= plan.block_count) {
        EXPECT_EQ(block.data_shards, 0U);
        EXPECT_EQ(block.parity_shards, 0U);
        continue;
      }
      ASSERT_GT(block.data_shards, 0U);
      EXPECT_LE(block.data_shards + block.parity_shards, 255U);
      EXPECT_LT(block.data_shards + block.parity_shards, 1024U);
      const auto natural_parity = receiver_parity(block.data_shards, config.fec_percentage);
      const auto expected_parity = config.fec_percentage == 0 ? 0 : std::max(natural_parity, config.min_fec_packets);
      EXPECT_EQ(block.parity_shards, expected_parity);
      const auto signaled_percentage = expected_parity > natural_parity ?
                                          100 * expected_parity / block.data_shards :
                                          config.fec_percentage;
      EXPECT_LE(signaled_percentage, 255U);
      EXPECT_EQ(receiver_parity(block.data_shards, signaled_percentage), expected_parity);
      actual_data += block.data_shards;
      total_shards += block.data_shards + block.parity_shards;
    }
    EXPECT_EQ(plan.data_shards, actual_data);
    const auto on_wire = config.packet_size + 16 + (config.encrypted ? 32 : 0) + config.ip_header_size + 8;
    EXPECT_EQ(plan.wire_bytes, total_shards * on_wire);
  }
}  // namespace

TEST(PyroWaveFrame, SerializesExactLittleEndianWireLayout) {
  const std::array<std::uint8_t, 4> first {1, 2, 3, 4};
  const std::array<std::uint8_t, 8> second {10, 11, 12, 13, 14, 15, 16, 17};
  const std::array<std::span<const std::uint8_t>, 2> packets {first, second};
  const auto serialized = protocol::serialize_frame(0x0807060504030201ULL, 0x1817161514131211ULL, packets, 52);
  ASSERT_TRUE(serialized);
  EXPECT_EQ(*serialized, fixture());
  EXPECT_EQ(protocol::profile, "sdr-bt709-full-left-420");
}

TEST(PyroWaveFrame, ParsesIndependentWireFixtureWithoutCopyingPacketData) {
  const auto bytes = fixture();
  const auto parsed = protocol::parse_frame(bytes);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->frame_index, 0x0807060504030201ULL);
  EXPECT_EQ(parsed->timestamp_us, 0x1817161514131211ULL);
  ASSERT_EQ(parsed->packets.size(), 2U);
  EXPECT_EQ(parsed->packets[0].data(), bytes.data() + 36);
  EXPECT_EQ(parsed->packets[0].size(), 4U);
  EXPECT_EQ(parsed->packets[1].data(), bytes.data() + 44);
  EXPECT_EQ(parsed->packets[1].size(), 8U);
}

TEST(PyroWaveFrame, RejectsEmptyUnalignedAndOverBudgetPackets) {
  const std::array<std::uint8_t, 4> aligned {1, 2, 3, 4};
  const std::array<std::uint8_t, 3> unaligned {1, 2, 3};
  const std::array<std::span<const std::uint8_t>, 1> valid {aligned};
  const std::array<std::span<const std::uint8_t>, 1> empty {std::span<const std::uint8_t> {}};
  const std::array<std::span<const std::uint8_t>, 1> invalid {unaligned};
  EXPECT_FALSE(protocol::serialize_frame(0, 0, {}, 100));
  EXPECT_FALSE(protocol::serialize_frame(0, 0, empty, 100));
  EXPECT_FALSE(protocol::serialize_frame(0, 0, invalid, 100));
  for (std::size_t limit = 0; limit < 40; ++limit) {
    EXPECT_FALSE(protocol::serialize_frame(0, 0, valid, limit));
  }
  EXPECT_TRUE(protocol::serialize_frame(0, 0, valid, 40));
}

TEST(PyroWaveFrame, EnforcesMaximumPacketCountAndFrameBytes) {
  const std::array<std::uint8_t, 4> data {1, 2, 3, 4};
  std::vector<std::span<const std::uint8_t>> packets(protocol::max_native_packets, data);
  const auto serialized = protocol::serialize_frame(0, 0, packets, protocol::max_frame_size);
  ASSERT_TRUE(serialized);
  const auto parsed = protocol::parse_frame(*serialized);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->packets.size(), protocol::max_native_packets);
  packets.emplace_back(data);
  EXPECT_FALSE(protocol::serialize_frame(0, 0, packets, protocol::max_frame_size));

  bytes_t maximum_data(protocol::max_frame_size - protocol::frame_header_size - 4, 0x5a);
  std::array<std::span<const std::uint8_t>, 1> large_packet {maximum_data};
  const auto maximum_frame = protocol::serialize_frame(std::numeric_limits<std::uint64_t>::max(),
                                                      std::numeric_limits<std::uint64_t>::max(), large_packet,
                                                      std::numeric_limits<std::size_t>::max());
  ASSERT_TRUE(maximum_frame);
  EXPECT_EQ(maximum_frame->size(), protocol::max_frame_size);
  const auto maximum_parsed = protocol::parse_frame(*maximum_frame);
  ASSERT_TRUE(maximum_parsed);
  EXPECT_EQ(maximum_parsed->frame_index, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(maximum_parsed->timestamp_us, std::numeric_limits<std::uint64_t>::max());
  maximum_data.resize(maximum_data.size() + 4);
  large_packet[0] = maximum_data;
  EXPECT_FALSE(protocol::serialize_frame(0, 0, large_packet, std::numeric_limits<std::size_t>::max()));
  auto excessive_frame = *maximum_frame;
  excessive_frame.resize(excessive_frame.size() + 4);
  overwrite_le(excessive_frame, 8, static_cast<std::uint32_t>(excessive_frame.size()));
  EXPECT_FALSE(protocol::parse_frame(excessive_frame));
}

TEST(PyroWaveFrame, RejectsEveryTruncatedFixtureAndTrailingData) {
  const auto bytes = fixture();
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    SCOPED_TRACE(size);
    EXPECT_FALSE(protocol::parse_frame(std::span(bytes).first(size)));
  }
  auto trailing = bytes;
  trailing.insert(trailing.end(), 4, 0);
  EXPECT_FALSE(protocol::parse_frame(trailing));
  overwrite_le(trailing, 8, static_cast<std::uint32_t>(trailing.size()));
  EXPECT_FALSE(protocol::parse_frame(trailing));
}

TEST(PyroWaveFrame, RejectsInvalidHeaderCountsAndPacketLengths) {
  for (const auto offset : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U}) {
    auto bytes = fixture();
    bytes[offset] ^= 0x80;
    EXPECT_FALSE(protocol::parse_frame(bytes)) << "header offset " << offset;
  }
  for (const auto count : {0U, 1U, 3U, 4097U, std::numeric_limits<std::uint32_t>::max()}) {
    auto bytes = fixture();
    overwrite_le(bytes, 12, count);
    EXPECT_FALSE(protocol::parse_frame(bytes)) << "count " << count;
  }
  for (const auto size : {0U, 1U, 3U, 8U, 12U, std::numeric_limits<std::uint32_t>::max()}) {
    auto bytes = fixture();
    overwrite_le(bytes, 32, size);
    EXPECT_FALSE(protocol::parse_frame(bytes)) << "first length " << size;
  }
  for (const auto size : {0U, 1U, 3U, 4U, 12U, std::numeric_limits<std::uint32_t>::max()}) {
    auto bytes = fixture();
    overwrite_le(bytes, 40, size);
    EXPECT_FALSE(protocol::parse_frame(bytes)) << "second length " << size;
  }
}

TEST(PyroWaveTransport, AccountsForIpv4Ipv6EncryptionAndExactPathMtu) {
  for (const auto ip_header : {20U, 40U}) {
    for (const bool encrypted : {false, true}) {
      for (const auto mtu : {1280U, 1500U}) {
        protocol::transport_config_t config;
        config.path_mtu = mtu;
        config.ip_header_size = ip_header;
        config.encrypted = encrypted;
        config.packet_size = mtu - ip_header - 8 - 16 - (encrypted ? 32 : 0);
        const auto limits = protocol::transport_limits(config);
        ASSERT_TRUE(limits);
        EXPECT_EQ(limits->payload_bytes, config.packet_size - 16);
        EXPECT_EQ(limits->datagram_bytes, mtu - ip_header - 8);
        EXPECT_EQ(limits->on_wire_bytes, mtu);
        ++config.packet_size;
        EXPECT_FALSE(protocol::transport_limits(config));
      }
    }
  }
}

TEST(PyroWaveTransport, RejectsInvalidConfigurationAndFrameSizes) {
  using config_t = protocol::transport_config_t;
  const std::array<config_t, 10> invalid {{
    {.packet_size = 199}, {.packet_size = 1501}, {.path_mtu = 1279}, {.path_mtu = 1501},
    {.ip_header_size = 0}, {.ip_header_size = 60}, {.fec_percentage = 256}, {.min_fec_packets = 255},
    {.packet_size = std::numeric_limits<std::uint32_t>::max()},
    {.fec_percentage = std::numeric_limits<std::uint32_t>::max()},
  }};
  for (const auto &config : invalid) {
    EXPECT_FALSE(protocol::transport_limits(config));
    EXPECT_FALSE(protocol::plan_transport(40, config));
    EXPECT_EQ(protocol::frame_budget(std::numeric_limits<std::uint32_t>::max(), config), 0U);
  }
  for (const auto size : {0U, 1U, 31U}) {
    EXPECT_FALSE(protocol::plan_transport(size, {}));
  }
  EXPECT_FALSE(protocol::plan_transport(std::numeric_limits<std::size_t>::max(), {}));
}

TEST(PyroWaveTransport, PreservesFecAtEveryFourBlockBoundary) {
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 5> expected {{{0, 255}, {1, 252}, {20, 212}, {100, 127}, {255, 71}}};
  for (const auto [percentage, max_data] : expected) {
    SCOPED_TRACE(percentage);
    protocol::transport_config_t config;
    config.fec_percentage = percentage;
    const auto limits = protocol::transport_limits(config);
    ASSERT_TRUE(limits);
    EXPECT_EQ(limits->max_data_shards, max_data);
    for (std::uint32_t blocks = 1; blocks <= 4; ++blocks) {
      const auto bytes = blocks * max_data * limits->payload_bytes - protocol::short_frame_header_size;
      const auto plan = protocol::plan_transport(bytes, config);
      ASSERT_TRUE(plan);
      EXPECT_EQ(plan->block_count, blocks);
      expect_valid_plan(*plan, config);
      const auto next = protocol::plan_transport(bytes + 1, config);
      if (blocks < 4) {
        ASSERT_TRUE(next);
        EXPECT_EQ(next->block_count, blocks + 1);
      } else {
        EXPECT_FALSE(next);
      }
    }
  }
}

TEST(PyroWaveTransport, RejectsUnrepresentableMinimumWithoutDroppingFec) {
  protocol::transport_config_t config;
  config.fec_percentage = 1;
  config.min_fec_packets = 16;
  const auto limits = protocol::transport_limits(config);
  ASSERT_TRUE(limits);
  EXPECT_EQ(limits->max_data_shards, 239U);
  // At D=239, min parity 16 would signal 6%; the receiver computes 15.
  EXPECT_FALSE(protocol::plan_transport(239 * limits->payload_bytes - 8, config));
  // At D=228, 7% reconstructs all 16 parity packets exactly.
  const auto valid = protocol::plan_transport(228 * limits->payload_bytes - 8, config);
  ASSERT_TRUE(valid);
  EXPECT_EQ(valid->blocks[0].data_shards, 228U);
  EXPECT_EQ(valid->blocks[0].parity_shards, 16U);
  // A tiny frame would need a percentage larger than the wire field permits.
  EXPECT_FALSE(protocol::plan_transport(40, config));
}

TEST(PyroWaveTransport, ThresholdMatrixMatchesReceiverParityAndHeaderLimits) {
  for (const auto percentage : {0U, 1U, 20U, 100U, 255U}) {
    for (const auto minimum : {0U, 2U, 16U, 254U}) {
      for (const auto ip_header : {20U, 40U}) {
        for (const bool encrypted : {false, true}) {
          SCOPED_TRACE(testing::Message() << "fec=" << percentage << " min=" << minimum
                                          << " ip=" << ip_header << " encrypted=" << encrypted);
          protocol::transport_config_t config;
          config.fec_percentage = percentage;
          config.min_fec_packets = minimum;
          config.ip_header_size = ip_header;
          config.encrypted = encrypted;
          const auto limits = protocol::transport_limits(config);
          if (percentage != 0 && minimum == 254) {
            EXPECT_FALSE(limits);
            continue;
          }
          ASSERT_TRUE(limits);
          for (std::uint32_t shards = 1; shards <= 4 * limits->max_data_shards; ++shards) {
            const auto frame_bytes = shards * limits->payload_bytes - protocol::short_frame_header_size;
            const auto plan = protocol::plan_transport(frame_bytes, config);
            // Minimum parity can be unrepresentable in the 8-bit percentage.
            // Calculate this independently for each expected balanced block.
            const auto blocks = divide_up(shards, limits->max_data_shards);
            bool representable = true;
            for (std::uint32_t block = 0; block < blocks; ++block) {
              const auto data = shards / blocks + (block < shards % blocks);
              const auto natural = receiver_parity(data, percentage);
              const auto parity = percentage == 0 ? 0 : std::max(natural, minimum);
              const auto signaled = parity > natural ? 100 * parity / data : percentage;
              representable = representable && data + parity <= 255 && signaled <= 255 &&
                              receiver_parity(data, signaled) == parity;
            }
            ASSERT_EQ(plan.has_value(), representable) << "data shards=" << shards;
            if (plan) {
              EXPECT_EQ(plan->data_shards, shards);
              EXPECT_EQ(plan->block_count, blocks);
              expect_valid_plan(*plan, config);
            }
          }
          EXPECT_FALSE(protocol::plan_transport(limits->max_frame_bytes + 1, config));
        }
      }
    }
  }
}

TEST(PyroWaveTransport, BudgetIsLargestValidFrameAcrossRepresentabilityHoles) {
  for (const auto percentage : {0U, 1U, 20U, 100U, 255U}) {
    for (const auto minimum : {0U, 2U, 16U, 254U}) {
      SCOPED_TRACE(testing::Message() << "fec=" << percentage << " min=" << minimum);
      protocol::transport_config_t config;
      config.fec_percentage = percentage;
      config.min_fec_packets = minimum;
      config.encrypted = true;
      const auto limits = protocol::transport_limits(config);
      if (!limits) {
        EXPECT_EQ(protocol::frame_budget(std::numeric_limits<std::uint32_t>::max(), config), 0U);
        continue;
      }
      std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;
      std::set<std::uint32_t> budgets {0, 1, std::numeric_limits<std::uint32_t>::max()};
      for (std::uint32_t shards = 1; shards <= 4 * limits->max_data_shards; ++shards) {
        const auto bytes = shards * limits->payload_bytes - protocol::short_frame_header_size;
        const auto plan = protocol::plan_transport(bytes, config);
        if (!plan) continue;
        candidates.emplace_back(bytes, plan->wire_bytes);
        if (shards <= 8 || shards % 53 == 0 || shards >= 4 * limits->max_data_shards - 2) {
          budgets.insert(plan->wire_bytes - 1);
          budgets.insert(plan->wire_bytes);
          budgets.insert(plan->wire_bytes + 1);
        }
      }
      for (const auto budget : budgets) {
        std::uint32_t expected_bytes = 0;
        for (const auto [bytes, wire_bytes] : candidates) {
          if (wire_bytes <= budget) expected_bytes = std::max(expected_bytes, bytes);
        }
        const auto actual = protocol::frame_budget(budget, config);
        ASSERT_EQ(actual, expected_bytes) << "IP budget=" << budget;
        if (actual != 0) {
          const auto plan = protocol::plan_transport(actual, config);
          ASSERT_TRUE(plan);
          EXPECT_LE(plan->wire_bytes, budget);
        }
      }
    }
  }
}
