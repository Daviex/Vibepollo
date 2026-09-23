#pragma once

// Isolated loopback video-only receiver for the real RTSP/sender harness.
// No desktop pixels or bitstreams leave RAM. It is not a client implementation.
#include <boost/asio.hpp>
#include "src/platform/windows/display.h"
#include "src/pyrowave_protocol.h"
#include "reference_codec.h"
#include <map>
#include <thread>

namespace pyrowave_smoke {
  struct udp_result_t {
    std::uint32_t frames = 0;
    std::uint32_t fragments = 0;
    std::uint32_t parity_fragments = 0;
    std::uint32_t discarded_frames = 0;
    // Three buffered frames can drain much faster than the capture cadence.
    double receiver_decode_drain_fps = 0;
    unsigned max_decoded_luma_span = 0;
    std::uint64_t first_presentation_us = 0;
    std::uint64_t last_presentation_us = 0;
    std::uint64_t total_pwvf_bytes = 0;
    std::uint32_t max_pwvf_bytes = 0;
  };

  inline udp_result_t run_udp_receive_decode(std::uint16_t video_port, int width, int height, std::chrono::milliseconds deadline) {
    using udp = boost::asio::ip::udp;
    if (!video_port || width < 64 || height < 64 || width > 4096 || height > 4096 ||
        (width & 1) || (height & 1) || deadline.count() <= 0 || deadline > std::chrono::seconds(10)) {
      throw std::runtime_error("Invalid bounded UDP receiver configuration");
    }
    const auto identity = platf::dxgi::resolve_automatic_capture_output(platf::mem_type_e::dxgi);
    if (!identity) throw std::runtime_error("No compatible capture GPU for loopback decoder");
    reference_decoder_t reference;
    reference.initialize(LUID {identity->adapter_id.low_part, identity->adapter_id.high_part}, width, height);
    std::array<std::vector<std::uint8_t>, 3> decoded;
    pyrowave_cpu_buffer output {};
    output.width = width;
    output.height = height;
    output.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int plane = 0; plane < 3; ++plane) {
      const int scale = plane ? 2 : 1;
      decoded[plane].resize(static_cast<std::size_t>(width / scale) * (height / scale));
      output.data[plane] = decoded[plane].data();
      output.row_stride_in_bytes[plane] = width / scale;
      output.plane_size_in_bytes[plane] = decoded[plane].size();
    }
    auto le = [](std::span<const std::uint8_t> bytes, std::size_t offset, unsigned count) {
      std::uint32_t result = 0;
      for (unsigned i = 0; i < count; ++i) result |= std::uint32_t(bytes[offset + i]) << (8 * i);
      return result;
    };
    struct block_t {
      std::uint32_t data_count = 0;
      std::map<std::uint32_t, std::vector<std::uint8_t>> data;
    };
    struct frame_t {
      std::uint32_t block_count = 0;
      std::array<block_t, 4> blocks;
    };
    std::map<std::uint32_t, frame_t> pending;
    std::optional<std::uint32_t> last_decoded;
    boost::asio::io_context context;
    const udp::endpoint server(boost::asio::ip::make_address("127.0.0.1"), video_port);
    udp::socket socket(context, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    socket.set_option(boost::asio::socket_base::receive_buffer_size(4 * 1024 * 1024));
    socket.non_blocking(true);
    udp_result_t result;
    std::optional<smoke_clock_t::time_point> first_decoded;
    const auto stop_at = smoke_clock_t::now() + deadline;
    auto next_ping = smoke_clock_t::time_point::min();
    std::array<std::uint8_t, 2048> buffer {};
    while (smoke_clock_t::now() < stop_at && result.frames < 3) {
      if (smoke_clock_t::now() >= next_ping) {
        socket.send_to(boost::asio::buffer("PING", 4), server);
        next_ping = smoke_clock_t::now() + std::chrono::milliseconds(50);
      }
      udp::endpoint sender;
      boost::system::error_code ec;
      const auto count = socket.receive_from(boost::asio::buffer(buffer), sender, 0, ec);
      if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (ec) throw boost::system::system_error(ec);
      if (sender != server) throw std::runtime_error("Unexpected loopback sender endpoint");
      // This harness negotiates unencrypted packetSize=1392, path MTU=1500.
      if (count != 1408 || buffer[0] != 0x90) throw std::runtime_error("Unexpected RTP shard length/header");
      ++result.fragments;
      const auto bytes = std::span<const std::uint8_t>(buffer.data(), count);
      const auto frame_index = le(bytes, 20, 4);
      const auto fec = le(bytes, 28, 4);
      const auto data_count = fec >> 22;
      const auto shard_index = (fec >> 12) & 0x3ff;
      const auto percentage = (fec >> 4) & 0xff;
      const auto block_index = (buffer[27] >> 4) & 3;
      const auto block_count = (buffer[27] >> 6) + 1;
      const auto parity_count = (data_count * percentage + 99) / 100;
      if (percentage != 20 || data_count == 0 || data_count + parity_count > 255 || shard_index >= data_count + parity_count || block_index >= block_count) {
        throw std::runtime_error("Invalid negotiated FEC shard metadata");
      }
      if (shard_index >= data_count) {
        ++result.parity_fragments;
        continue;  // No induced packet loss; parity recovery has separate tests.
      }
      if (last_decoded && frame_index <= *last_decoded) continue;
      auto &frame = pending[frame_index];
      if (frame.block_count && frame.block_count != block_count) throw std::runtime_error("FEC block count changed inside frame");
      frame.block_count = block_count;
      auto &block = frame.blocks[block_index];
      if (block.data_count && block.data_count != data_count) throw std::runtime_error("FEC data shard count changed inside block");
      block.data_count = data_count;
      block.data.try_emplace(shard_index, bytes.begin() + 32, bytes.end());
      bool complete = true;
      for (std::uint32_t i = 0; i < block_count; ++i) complete &= frame.blocks[i].data_count != 0 && frame.blocks[i].data.size() == frame.blocks[i].data_count;
      if (complete) {
        std::vector<std::uint8_t> payload;
        for (std::uint32_t i = 0; i < block_count; ++i) for (const auto &[index, data] : frame.blocks[i].data) payload.insert(payload.end(), data.begin(), data.end());
        if (payload.size() < 40 || payload[0] != 1 || payload[3] != 2) throw std::runtime_error("Invalid short video frame header");
        const auto envelope_size = le(payload, 16, 4);  // short header8 + PWVF total offset8.
        if (envelope_size > ::pyrowave::protocol::max_frame_size || envelope_size + 8 > payload.size()) throw std::runtime_error("Invalid reassembled PWVF size");
        const auto trailing = std::span<const std::uint8_t>(payload).subspan(envelope_size + 8);
        if (!std::all_of(trailing.begin(), trailing.end(), [](auto byte) { return byte == 0; })) throw std::runtime_error("Nonzero data beyond PWVF boundary");
        const auto parsed = ::pyrowave::protocol::parse_frame(std::span<const std::uint8_t>(payload).subspan(8, envelope_size));
        if (!parsed || parsed->frame_index != frame_index) throw std::runtime_error("PWVF envelope/frame identity invalid");
        if (result.frames != 0 && parsed->timestamp_us <= result.last_presentation_us) throw std::runtime_error("PWVF presentation timestamps did not increase strictly");
        if (result.frames == 0) result.first_presentation_us = parsed->timestamp_us;
        result.last_presentation_us = parsed->timestamp_us;
        result.total_pwvf_bytes += envelope_size;
        result.max_pwvf_bytes = std::max(result.max_pwvf_bytes, envelope_size);
        platf::pyrowave::packet_list_t native;
        for (const auto packet : parsed->packets) native.emplace_back(packet.begin(), packet.end());
        reference.decode(native, output);
        const auto [minimum, maximum] = std::minmax_element(decoded[0].begin(), decoded[0].end());
        result.max_decoded_luma_span = std::max(result.max_decoded_luma_span, unsigned(*maximum - *minimum));
        if (!first_decoded) first_decoded = smoke_clock_t::now();
        ++result.frames;
        last_decoded = frame_index;
        pending.erase(pending.begin(), pending.upper_bound(frame_index));
      }
      while (pending.size() > 4) {
        pending.erase(pending.begin());
        ++result.discarded_frames;
      }
    }
    if (result.frames != 3 || result.max_decoded_luma_span < 8) throw std::runtime_error("Loopback did not decode three nonblank desktop frames before deadline");
    result.receiver_decode_drain_fps = (result.frames - 1) / std::chrono::duration<double>(smoke_clock_t::now() - *first_decoded).count();
    return result;
  }
}
