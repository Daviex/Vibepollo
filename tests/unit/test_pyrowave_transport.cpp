#include "../tests_common.h"
#include "src/crypto.h"
#include "src/pyrowave_transport.h"

extern "C" {
#include "src/rswrapper.h"
}

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <numeric>

extern "C" {
#include "third-party/moonlight-common-c/src/Limelight.h"
#include "third-party/moonlight-common-c/src/Video.h"
}

namespace {
  namespace protocol = pyrowave::protocol;
  namespace transport = pyrowave::transport;
  using bytes_t = std::vector<std::uint8_t>;
  constexpr std::uint32_t frame_index = 0x01020304;

  // The production serializer writes explicit wire offsets. Fail this component
  // at compile time if the bundled Moonlight transport layout ever changes.
  static_assert(sizeof(RTP_PACKET) == 12 && MAX_RTP_HEADER_SIZE == 16);
  static_assert(offsetof(RTP_PACKET, header) == 0);
  static_assert(offsetof(RTP_PACKET, sequenceNumber) == 2);
  static_assert(offsetof(RTP_PACKET, timestamp) == 4);
  static_assert(offsetof(RTP_PACKET, ssrc) == 8);
  static_assert(sizeof(NV_VIDEO_PACKET) == 16);
  static_assert(offsetof(NV_VIDEO_PACKET, streamPacketIndex) == 0);
  static_assert(offsetof(NV_VIDEO_PACKET, frameIndex) == 4);
  static_assert(offsetof(NV_VIDEO_PACKET, flags) == 8);
  static_assert(offsetof(NV_VIDEO_PACKET, extraFlags) == 9);
  static_assert(offsetof(NV_VIDEO_PACKET, multiFecFlags) == 10);
  static_assert(offsetof(NV_VIDEO_PACKET, multiFecBlocks) == 11);
  static_assert(offsetof(NV_VIDEO_PACKET, fecInfo) == 12);
  static_assert(transport::packet_header_bytes == MAX_RTP_HEADER_SIZE + sizeof(NV_VIDEO_PACKET));
  static_assert(sizeof(ENC_VIDEO_HEADER) == transport::encryption_prefix_bytes);
  static_assert(offsetof(ENC_VIDEO_HEADER, iv) == 0);
  static_assert(offsetof(ENC_VIDEO_HEADER, frameNumber) == 12);
  static_assert(offsetof(ENC_VIDEO_HEADER, tag) == 16);
  static_assert(FLAG_CONTAINS_PIC_DATA == 1 && FLAG_EOF == 2 && FLAG_SOF == 4 && FLAG_EXTENSION == 0x10);

  std::uint32_t little(std::span<const std::uint8_t> data, std::size_t offset, unsigned bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= std::uint32_t(data[offset + i]) << (8 * i);
    return value;
  }

  bytes_t sample_frame(std::size_t native_size = 30000) {
    bytes_t native(native_size);
    for (std::size_t i = 0; i < native.size(); ++i) native[i] = static_cast<std::uint8_t>(i * 31 + i / 256);
    const std::array<std::span<const std::uint8_t>, 1> packets {native};
    return *protocol::serialize_frame(frame_index, 123456789, packets, protocol::max_frame_size);
  }

  crypto::aes_t key() {
    return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  }

  struct encoded_frame_t {
    bytes_t frame;
    protocol::transport_config_t config;
    protocol::transport_plan_t plan;
    std::vector<std::vector<bytes_t>> blocks;
  };

  encoded_frame_t encode_frame(protocol::transport_config_t config, std::size_t native_size = 30000) {
    reed_solomon_init();
    encoded_frame_t result {sample_frame(native_size), config, {}, {}};
    const auto plan = protocol::plan_transport(result.frame.size(), config);
    if (!plan) throw std::runtime_error("invalid test frame transport budget");
    result.plan = *plan;
    const auto packetized = transport::packetize_frame(result.frame, config, 123);
    if (!packetized) throw std::runtime_error("packetization failed");
    crypto::cipher::gcm_t cipher(key(), false);
    std::uint64_t counter = 1234;
    std::uint32_t sequence = 65530;  // Exercise 16-bit RTP sequence wrap.
    std::size_t offset = 0;
    for (std::uint32_t block_index = 0; block_index < plan->block_count; ++block_index) {
      const auto block_bytes = std::min<std::size_t>(plan->blocks[block_index].data_shards * (config.packet_size + 16), packetized->size() - offset);
      const auto encoded = transport::encode_block(std::span(*packetized).subspan(offset, block_bytes), plan->blocks[block_index], config,
                                                   {frame_index, sequence, 0x10203040, block_index, plan->block_count},
                                                   config.encrypted ? &cipher : nullptr, counter);
      if (!encoded) throw std::runtime_error("encoding failed");
      std::vector<bytes_t> datagrams;
      for (std::size_t i = 0; i < encoded->size(); ++i) {
        bytes_t datagram(encoded->prefix(i).begin(), encoded->prefix(i).end());
        const auto shard = encoded->shard(i);
        datagram.insert(datagram.end(), shard.begin(), shard.end());
        datagrams.push_back(std::move(datagram));
      }
      sequence += static_cast<std::uint32_t>(encoded->size());
      offset += block_bytes;
      result.blocks.push_back(std::move(datagrams));
    }
    return result;
  }

  // Independent bounded receiver harness. It consumes only serialized UDP
  // datagrams and derives block sizes from the wire headers, like Moonlight.
  class receiver_t {
  public:
    explicit receiver_t(protocol::transport_config_t config, crypto::aes_t aes_key = key()):
        config(config), cipher(aes_key, false) {}

    bool receive(std::span<const std::uint8_t> datagram) {
      const auto limits = protocol::transport_limits(config);
      if (!limits || datagram.size() != limits->datagram_bytes) return false;
      bytes_t plain;
      if (config.encrypted) {
        crypto::aes_t iv(datagram.begin(), datagram.begin() + 12);
        if (datagram[11] != 'V' || little(datagram, 12, 4) != frame_index ||
            cipher.decrypt({reinterpret_cast<const char *>(datagram.data() + 16), datagram.size() - 16}, plain, &iv) != 0) return false;
      } else {
        plain.assign(datagram.begin(), datagram.end());
      }
      if (plain.size() != config.packet_size + 16 || plain[0] != 0x90 || little(plain, 20, 4) != frame_index) return false;
      const auto fec = little(plain, 28, 4);
      const auto data_count = fec >> 22;
      const auto percentage = (fec >> 4) & 255;
      const auto shard_index = (fec >> 12) & 1023;
      const auto parity_count = (data_count * percentage + 99) / 100;
      const auto block_index = (plain[27] >> 4) & 3;
      const auto block_count = (plain[27] >> 6) + 1;
      if (data_count == 0 || data_count + parity_count > 255 || shard_index >= data_count + parity_count || block_index >= block_count) return false;
      if (count && *count != block_count) return false;
      count = block_count;
      auto &block = blocks[block_index];
      if (block.data_count && (block.data_count != data_count || block.parity_count != parity_count)) return false;
      block.data_count = data_count;
      block.parity_count = parity_count;
      const auto found = block.shards.find(shard_index);
      if (found != block.shards.end()) return found->second == plain;
      block.shards.emplace(shard_index, std::move(plain));
      return true;
    }

    std::optional<bytes_t> reconstruct() {
      if (!count || blocks.size() != *count) return std::nullopt;
      bytes_t joined;
      const auto shard_bytes = config.packet_size + 16;
      const auto payload_bytes = config.packet_size - 16;
      for (std::uint32_t block_index = 0; block_index < *count; ++block_index) {
        auto &block = blocks.at(block_index);
        if (block.shards.size() < block.data_count) return std::nullopt;
        const auto total = block.data_count + block.parity_count;
        std::vector<bytes_t> shards(total, bytes_t(shard_bytes, 0));
        std::vector<std::uint8_t *> pointers(total);
        bytes_t missing(total, 1);
        for (std::uint32_t i = 0; i < total; ++i) {
          if (const auto found = block.shards.find(i); found != block.shards.end()) {
            shards[i] = found->second;
            missing[i] = 0;
          }
          pointers[i] = shards[i].data();
        }
        if (std::any_of(missing.begin(), missing.begin() + block.data_count, [](auto value) { return value != 0; })) {
          if (block.parity_count == 0) return std::nullopt;
          std::unique_ptr<reed_solomon, decltype(reed_solomon_release)> rs {
            reed_solomon_new(block.data_count, block.parity_count), reed_solomon_release};
          if (!rs || reed_solomon_decode(rs.get(), pointers.data(), missing.data(), total, shard_bytes) != 0) return std::nullopt;
        }
        for (std::uint32_t i = 0; i < block.data_count; ++i) {
          const auto expected_flags = 1 | (i == 0 ? 4 : 0) | (i + 1 == block.data_count ? 2 : 0);
          if (shards[i][24] != expected_flags) return std::nullopt;
          joined.insert(joined.end(), shards[i].begin() + transport::packet_header_bytes, shards[i].end());
        }
      }
      if (joined.size() < 8 || joined[0] != 1 || joined[3] != 2) return std::nullopt;
      const auto last_payload = little(joined, 4, 2);
      if (last_payload == 0 || last_payload > payload_bytes) return std::nullopt;
      joined.resize(joined.size() - payload_bytes + last_payload);
      if (joined.size() < 8) return std::nullopt;
      bytes_t frame(joined.begin() + 8, joined.end());
      const auto parsed = protocol::parse_frame(frame);
      if (!parsed || static_cast<std::uint32_t>(parsed->frame_index) != frame_index) return std::nullopt;
      return frame;
    }

  private:
    struct block_t {
      std::uint32_t data_count = 0;
      std::uint32_t parity_count = 0;
      std::map<std::uint32_t, bytes_t> shards;
    };
    protocol::transport_config_t config;
    crypto::cipher::gcm_t cipher;
    std::optional<std::uint32_t> count;
    std::map<std::uint32_t, block_t> blocks;
  };

  class loopback_t {
  public:
    explicit loopback_t(bool ipv6) {
#ifdef _WIN32
      WSADATA data {};
      if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
#endif
      const auto family = ipv6 ? AF_INET6 : AF_INET;
      receiver = socket(family, SOCK_DGRAM, IPPROTO_UDP);
      sender = socket(family, SOCK_DGRAM, IPPROTO_UDP);
      if (receiver == invalid || sender == invalid) throw std::runtime_error("UDP socket failed");
      if (ipv6) {
        auto *address = reinterpret_cast<sockaddr_in6 *>(&endpoint);
        address->sin6_family = AF_INET6;
        address->sin6_addr = in6addr_loopback;
        endpoint_size = sizeof(*address);
      } else {
        auto *address = reinterpret_cast<sockaddr_in *>(&endpoint);
        address->sin_family = AF_INET;
        address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint_size = sizeof(*address);
      }
      if (bind(receiver, reinterpret_cast<sockaddr *>(&endpoint), endpoint_size) != 0 ||
          getsockname(receiver, reinterpret_cast<sockaddr *>(&endpoint), &endpoint_size) != 0) throw std::runtime_error("UDP bind failed");
#ifdef _WIN32
      DWORD timeout = 2000;
#else
      timeval timeout {2, 0};
#endif
      setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    }

    ~loopback_t() {
#ifdef _WIN32
      closesocket(sender);
      closesocket(receiver);
      WSACleanup();
#else
      close(sender);
      close(receiver);
#endif
    }

    bytes_t transfer(const bytes_t &datagram) {
      if (sendto(sender, reinterpret_cast<const char *>(datagram.data()), static_cast<int>(datagram.size()), 0,
                 reinterpret_cast<const sockaddr *>(&endpoint), endpoint_size) != static_cast<int>(datagram.size())) throw std::runtime_error("UDP send failed");
      bytes_t received(1600);
      const auto size = recvfrom(receiver, reinterpret_cast<char *>(received.data()), static_cast<int>(received.size()), 0, nullptr, nullptr);
      if (size < 0) throw std::runtime_error("UDP receive failed");
      received.resize(size);
      return received;
    }

  private:
#ifdef _WIN32
    using socket_t = SOCKET;
    static constexpr auto invalid = INVALID_SOCKET;
    int endpoint_size = 0;
#else
    using socket_t = int;
    static constexpr auto invalid = -1;
    socklen_t endpoint_size = 0;
#endif
    socket_t receiver = invalid;
    socket_t sender = invalid;
    sockaddr_storage endpoint {};
  };
}  // namespace

TEST(PyroWaveTransportBytes, ExactRtpNvShortHeaderAndEncryptionPrefixLayout) {
  protocol::transport_config_t config;
  const auto encoded = encode_frame(config, 100);
  ASSERT_EQ(encoded.blocks.size(), 1U);
  ASSERT_EQ(encoded.blocks.front().size(), 2U);
  const bytes_t expected_header {
    0x90, 0x00, 0xff, 0xfa, 0x10, 0x20, 0x30, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xfa, 0xff, 0x00, 0x04, 0x03, 0x02, 0x01,
    0x07, 0x00, 0x10, 0x00, 0x40, 0x01, 0x40, 0x00,
  };
  const bytes_t expected_short_header {1, 123, 0, 2, 144, 0, 0, 0};
  const auto &first = encoded.blocks.front().front();
  EXPECT_TRUE(std::equal(expected_header.begin(), expected_header.end(), first.begin()));
  EXPECT_TRUE(std::equal(expected_short_header.begin(), expected_short_header.end(), first.begin() + 32));
  EXPECT_TRUE(std::equal(encoded.frame.begin(), encoded.frame.end(), first.begin() + 40));
  EXPECT_TRUE(std::all_of(first.begin() + 40 + encoded.frame.size(), first.end(), [](auto byte) { return byte == 0; }));

  const auto large = encode_frame(config);
  const auto &wrapped = large.blocks.front().at(6);
  EXPECT_EQ(wrapped[2], 0);
  EXPECT_EQ(wrapped[3], 0);
  EXPECT_EQ(little(wrapped, 16, 4), 0x01000000U);

  config.encrypted = true;
  const auto encrypted = encode_frame(config, 100);
  const auto &with_prefix = encrypted.blocks.front().front();
  const bytes_t expected_prefix {0xd2, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'V', 4, 3, 2, 1};
  EXPECT_TRUE(std::equal(expected_prefix.begin(), expected_prefix.end(), with_prefix.begin()));
  crypto::cipher::gcm_t cipher(key(), false);
  crypto::aes_t iv(with_prefix.begin(), with_prefix.begin() + 12);
  bytes_t decrypted;
  ASSERT_EQ(cipher.decrypt({reinterpret_cast<const char *>(with_prefix.data() + 16), with_prefix.size() - 16}, decrypted, &iv), 0);
  EXPECT_EQ(decrypted, first);
}

TEST(PyroWaveTransportBytes, ProductionPacketsRoundTripOnIpv4AndIpv6Loopback) {
  for (const bool ipv6 : {false, true}) {
    for (const bool encrypted : {false, true}) {
      protocol::transport_config_t config;
      config.ip_header_size = ipv6 ? 40 : 20;
      config.encrypted = encrypted;
      config.packet_size = config.path_mtu - config.ip_header_size - 8 - 16 - (encrypted ? 32 : 0);
      const auto encoded = encode_frame(config);
      receiver_t receiver(config);
      loopback_t socket(ipv6);
      std::uint32_t wire_bytes = 0;
      for (const auto &block : encoded.blocks) {
        for (const auto &datagram : block) {
          EXPECT_EQ(datagram.size() + config.ip_header_size + 8, config.path_mtu);
          wire_bytes += static_cast<std::uint32_t>(datagram.size()) + config.ip_header_size + 8;
          ASSERT_TRUE(receiver.receive(socket.transfer(datagram)));
        }
      }
      EXPECT_EQ(wire_bytes, encoded.plan.wire_bytes);
      const auto reconstructed = receiver.reconstruct();
      ASSERT_TRUE(reconstructed);
      EXPECT_EQ(*reconstructed, encoded.frame);
    }
  }
}

TEST(PyroWaveTransportBytes, RecoversDataLossAfterReverseOrderAndDuplicatesAcrossFourBlocks) {
  protocol::transport_config_t config;
  config.encrypted = true;
  const auto encoded = encode_frame(config, 800000);
  ASSERT_EQ(encoded.plan.block_count, 4U);
  receiver_t receiver(config);
  for (std::size_t block = encoded.blocks.size(); block-- > 0;) {
    const auto &datagrams = encoded.blocks[block];
    for (std::size_t shard = datagrams.size(); shard-- > 1;) {  // Lose first data shard in every block.
      ASSERT_TRUE(receiver.receive(datagrams[shard]));
      ASSERT_TRUE(receiver.receive(datagrams[shard]));
    }
  }
  const auto reconstructed = receiver.reconstruct();
  ASSERT_TRUE(reconstructed);
  EXPECT_EQ(*reconstructed, encoded.frame);
}

TEST(PyroWaveTransportBytes, MissingDataWithoutParityAndLossBeyondParityCannotRecover) {
  for (const auto fec : {0U, 20U}) {
    protocol::transport_config_t config;
    config.fec_percentage = fec;
    const auto encoded = encode_frame(config);
    receiver_t receiver(config);
    for (std::size_t block = 0; block < encoded.blocks.size(); ++block) {
      const auto drop = encoded.plan.blocks[block].parity_shards + 1;
      for (std::size_t shard = drop; shard < encoded.blocks[block].size(); ++shard) ASSERT_TRUE(receiver.receive(encoded.blocks[block][shard]));
    }
    EXPECT_FALSE(receiver.reconstruct());
  }
}

TEST(PyroWaveTransportBytes, ExactPayloadBoundaryAndMinimumParityRoundTrip) {
  for (const auto native_size : {964U, 968U, 100U}) {
    protocol::transport_config_t config;
    config.min_fec_packets = 2;
    const auto encoded = encode_frame(config, native_size);
    receiver_t receiver(config);
    for (const auto &block : encoded.blocks) {
      for (std::size_t i = 1; i < block.size(); ++i) ASSERT_TRUE(receiver.receive(block[i]));
    }
    const auto reconstructed = receiver.reconstruct();
    ASSERT_TRUE(reconstructed);
    EXPECT_EQ(*reconstructed, encoded.frame);
  }
}

TEST(PyroWaveTransportBytes, ParityMatrixUsesExactWirePercentage) {
  for (const auto percentage : {0U, 1U, 20U, 100U, 255U}) {
    for (const auto minimum : {0U, 2U, 16U}) {
      protocol::transport_config_t config;
      config.fec_percentage = percentage;
      config.min_fec_packets = minimum;
      const auto encoded = encode_frame(config);
      receiver_t receiver(config);
      for (const auto &block : encoded.blocks) {
        for (std::size_t i = percentage == 0 ? 0 : 1; i < block.size(); ++i) ASSERT_TRUE(receiver.receive(block[i]));
      }
      const auto reconstructed = receiver.reconstruct();
      ASSERT_TRUE(reconstructed) << "fec=" << percentage << " minimum=" << minimum;
      EXPECT_EQ(*reconstructed, encoded.frame);
    }
  }
}

TEST(PyroWaveTransportBytes, RejectsTruncationAndAuthenticationFailures) {
  protocol::transport_config_t config;
  config.encrypted = true;
  const auto encoded = encode_frame(config);
  const auto &datagram = encoded.blocks.front().front();
  for (const auto offset : {0U, 11U, 12U, 16U, 31U, 32U, 100U}) {
    auto damaged = datagram;
    damaged[offset] ^= 0x80;
    receiver_t receiver(config);
    EXPECT_FALSE(receiver.receive(damaged)) << "corruption offset=" << offset;
  }
  for (const auto length : {0U, 1U, 31U, 32U, 100U}) {
    receiver_t receiver(config);
    EXPECT_FALSE(receiver.receive(std::span(datagram).first(length)));
  }
  auto incorrect_key = key();
  incorrect_key[0] ^= 1;
  receiver_t receiver(config, incorrect_key);
  EXPECT_FALSE(receiver.receive(datagram));
}

TEST(PyroWaveTransportBytes, RefusesMismatchedCipherAndCounterWrap) {
  reed_solomon_init();
  protocol::transport_config_t config;
  config.encrypted = true;
  const auto frame = sample_frame(100);
  const auto data = transport::packetize_frame(frame, config);
  ASSERT_TRUE(data);
  const auto plan = protocol::plan_transport(frame.size(), config);
  ASSERT_TRUE(plan);
  crypto::cipher::gcm_t cipher(key(), false);
  std::uint64_t counter = 0;
  const transport::block_info_t info {frame_index, 0, 90000, 0, 1};
  EXPECT_FALSE(transport::encode_block(*data, plan->blocks[0], config, info, nullptr, counter));
  counter = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(transport::encode_block(*data, plan->blocks[0], config, info, &cipher, counter));
  EXPECT_EQ(counter, std::numeric_limits<std::uint64_t>::max());
  auto truncated = *data;
  truncated.resize(1);
  counter = 0;
  // A partial shard must at least contain the reserved packet header.
  EXPECT_FALSE(transport::encode_block(truncated, plan->blocks[0], config, info, &cipher, counter));
}
