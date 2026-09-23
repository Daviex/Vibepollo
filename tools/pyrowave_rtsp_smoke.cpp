/**
 * Exercise the real RTSP TCP parser and ANNOUNCE in isolation. The positive
 * case uses real probe/allocation/start/threads, with OS callbacks isolated by
 * an explicit test-only macro. Rejections also use an input_only safety guard.
 * Build with tools/pyrowave/build_rtsp_smoke.py, which isolates HDR event names.
 */
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/pyrowave_protocol.h"
#include "src/rtsp.h"
#include "src/stream.h"
#ifdef SUNSHINE_ENABLE_PYROWAVE
  #include "pyrowave/udp_receiver.h"
#endif

extern "C" {
#include "src/rswrapper.h"
}

#ifndef SUNSHINE_PYROWAVE_RTSP_HARNESS
  #error Build this standalone validation tool with build_rtsp_smoke.py
#endif

namespace platf::dxgi { int init(); }

#include <boost/asio.hpp>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

namespace {
  using tcp = boost::asio::ip::tcp;
  using namespace std::chrono_literals;

  struct test_t {
    std::string name;
    int status = 406;
    std::string reason;
    std::string payload;
    bool opted_in = true;
    bool host_enabled = true;
  };

  test_t make_test(const std::string &name) {
    std::map<std::string, std::string> args {
      {"x-nv-vqos[0].bitStreamFormat", "3"},
      {"x-vp-pyrowave.version", "1"},
      {"x-vp-pyrowave.bitstreamRevision", std::string(pyrowave::protocol::bitstream_revision)},
      {"x-vp-pyrowave.profile", std::string(pyrowave::protocol::profile)},
      {"x-vp-pyrowave.pathMtu", "1500"},
      {"x-nv-video[0].clientViewportHt", "720"},
      {"x-nv-video[0].clientViewportWd", "1280"},
      {"x-nv-video[0].maxFPS", "60"},
      {"x-nv-video[0].clientRefreshRateX100", "6000"},
      {"x-nv-video[0].packetSize", "1392"},
      {"x-nv-video[0].encoderCscMode", "3"},
      {"x-nv-video[0].dynamicRangeMode", "0"},
      {"x-ss-video[0].chromaSamplingType", "0"},
      {"x-nv-vqos[0].bw.maximumBitrateKbps", "100000"},
      {"x-ml-video.configuredBitrateKbps", "100000"},
      {"x-nv-vqos[0].fec.minRequiredFecPackets", "0"},
      {"x-nv-audio.surround.numChannels", "2"},
      {"x-nv-audio.surround.channelMask", "3"},
      {"x-nv-audio.surround.AudioQuality", "0"},
      {"x-nv-video[0].videoEncoderSlicesPerFrame", "1"},
      {"x-nv-video[0].maxNumReferenceFrames", "1"},
    };
    test_t result {.name = name, .reason = "Unsupported PyroWave session"};
    if (name == "unknown-codec" || name == "malformed-codec") {
      args["x-nv-vqos[0].bitStreamFormat"] = name == "unknown-codec" ? "2147483647" : "3junk";
      result.status = 400;
      result.reason = "Unsupported video codec";
    } else if (name == "no-launch-opt-in") {
      result.opted_in = false;
      result.reason = "Codec does not match launch request";
    } else if (name == "host-disabled") {
      result.host_enabled = false;
    } else if (name == "wrong-profile") {
      args["x-vp-pyrowave.profile"] = "hdr-bt2020";
    } else if (name == "wrong-revision") {
      args["x-vp-pyrowave.bitstreamRevision"] = "0000000000000000000000000000000000000000";
    } else if (name == "wrong-version") {
      args["x-vp-pyrowave.version"] = "2";
    } else if (name == "missing-version") {
      args.erase("x-vp-pyrowave.version");
    } else if (name == "hdr-profile") {
      args["x-nv-video[0].dynamicRangeMode"] = "1";
      result.reason = "Unsupported PyroWave profile";
    } else if (name == "malformed-number") {
      args["x-vp-pyrowave.pathMtu"] = "1500junk";
      result.status = 400;
      result.reason = "Invalid PyroWave numeric parameter";
    } else if (name == "overflow-number") {
      args["x-nv-video[0].clientViewportWd"] = "4294967296";
      result.status = 400;
      result.reason = "Invalid PyroWave numeric parameter";
    } else if (name.starts_with("encryption-")) {
      const std::map<std::string, std::string> values {
        {"encryption-empty", ""}, {"encryption-negative", "-2"}, {"encryption-junk", "2foo"},
        {"encryption-overflow", "4294967296"}, {"encryption-unknown-bits", "8"},
      };
      args["x-ss-general.encryptionEnabled"] = values.at(name);
      result.status = name == "encryption-unknown-bits" ? 406 : 400;
      result.reason = result.status == 400 ? "Invalid PyroWave numeric parameter" : "Unsupported PyroWave profile";
    } else if (name == "duplicate-number") {
      result.status = 400;
      result.reason = "Conflicting PyroWave parameters";
    } else if (name == "mismatched-fps") {
      args["x-nv-video[0].clientRefreshRateX100"] = "12000";
    } else if (name == "duplicate-extension" || name == "last-line-no-newline") {
      result.status = 400;
      result.reason = "Conflicting codec parameters";
    } else if (name == "positive-start-stop" || name == "positive-media") {
      result.status = 200;
      result.reason = "OK";
    } else if (name == "feature-off") {
      result.reason = "PyroWave unavailable in this build";
    } else {
      throw std::runtime_error("Unknown rejection case: " + name);
    }
    result.payload = "v=0\r\ns=PyroWave loopback rejection validation\r\n";
    for (const auto &[key, value] : args) result.payload += "a=" + key + ":" + value + "\r\n";
    if (name == "duplicate-extension" || name == "last-line-no-newline") {
      result.payload += "a=x-vp-pyrowave.version:2";
      if (name == "duplicate-extension") result.payload += "\r\n";
    }
    if (name == "duplicate-number") result.payload += "a=x-nv-video[0].maxFPS:120\r\n";
    return result;
  }

  std::string request(tcp::endpoint endpoint, const std::string &payload) {
    boost::asio::io_context context;
    tcp::socket socket(context);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    boost::system::error_code ec;
    do {
      socket.connect(endpoint, ec);
      if (!ec) break;
      socket.close();
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("RTSP bind/connect deadline exceeded");
      std::this_thread::sleep_for(10ms);
    } while (true);
    const auto message = "ANNOUNCE rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 1\r\nHost: 0.0.0.0\r\nContent-Type: application/sdp\r\nContent-Length: " +
                         std::to_string(payload.size()) + "\r\n\r\n" + payload;
    boost::asio::write(socket, boost::asio::buffer(message));
    socket.non_blocking(true);
    std::string response;
    std::array<char, 4096> bytes;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto count = socket.read_some(boost::asio::buffer(bytes), ec);
      response.append(bytes.data(), count);
      if (ec == boost::asio::error::eof) return response;
      if (ec && ec != boost::asio::error::would_block && ec != boost::asio::error::try_again) throw boost::system::system_error(ec);
      std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("RTSP response deadline exceeded");
  }
}

int main(int argc, char **argv) {
  std::mutex watchdog_mutex;
  std::condition_variable_any watchdog_condition;
  std::jthread watchdog([&](std::stop_token stop) {
    std::unique_lock lock(watchdog_mutex);
    watchdog_condition.wait_for(lock, stop, 30s, [] { return false; });
    if (!stop.stop_requested()) TerminateProcess(GetCurrentProcess(), 124);
  });
  try {
    if (argc != 2) throw std::runtime_error("Usage: pyrowave-rtsp-smoke.exe <rejection-case>");
    const auto test = make_test(argv[1]);
#ifdef SUNSHINE_ENABLE_PYROWAVE
    if (test.name == "feature-off") throw std::runtime_error("feature-off requires a disabled server build");
#else
    if (test.name != "feature-off" && test.name != "unknown-codec" && test.name != "malformed-codec" &&
        test.name != "no-launch-opt-in" && test.name != "duplicate-extension" && test.name != "last-line-no-newline") {
      throw std::runtime_error("This rejection case requires an enabled server build");
    }
#endif
    const bool positive = test.name == "positive-start-stop" || test.name == "positive-media";
    const bool media = test.name == "positive-media";
    auto logger = logging::init_single_file(2, "pyrowave-rtsp-smoke.log");
    mail::man = std::make_shared<safe::mail_raw_t>();
    config::sunshine.bind_address = "127.0.0.1";
    config::sunshine.address_family = "ipv4";
    config::sunshine.session_history_enabled = false;
    config::sunshine.system_tray = false;
    config::video.pyrowave_enabled = test.host_enabled;
    config::video.dd.vulkan_hdr_layer = false;
    config::video.dd.config_revert_on_disconnect = false;
    config::video.capture = "ddx";
    config::video.adapter_name.clear();
    config::video.adapter_pnp_id.clear();
    config::video.nv_realtime_hags = false;
    config::stream.lan_encryption_mode = config::ENCRYPTION_MODE_NEVER;
    config::stream.packetsize = 0;
    config::stream.fec_percentage = 20;
    config::stream.ping_timeout = positive ? 8s : 20s;
    if (positive) {
      if (enet_initialize() != 0) throw std::runtime_error("ENet initialization failed");
      reed_solomon_init();
      if (platf::dxgi::init() != 0) throw std::runtime_error("DXGI shader initialization failed");
    }

    boost::asio::io_context ports;
    tcp::acceptor reservation(ports, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    const auto port = reservation.local_endpoint().port();
    config::sunshine.port = port - rtsp_stream::RTSP_SETUP_PORT;
    reservation.close();
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = 1;
    launch->unique_id = "pyrowave-rtsp-isolated-rejection";
    launch->pyrowave_requested = test.opted_in;
    // Ensures even an unexpected acceptance by negotiation cannot enter the
    // GPU probe or stream::session::start. Specific response/log reasons below
    // still distinguish the intended rejection from this extra safety guard.
    launch->input_only = !positive;
    launch->fps = 60000;
    launch->width = 1280;
    launch->height = 720;
    launch->gcm_key.resize(16);
    launch->iv.resize(16);
    rtsp_stream::launch_session_raise(launch);
    auto shutdown = mail::man->event<bool>(mail::shutdown);
    std::thread server(rtsp_stream::start);
    auto stop = util::fail_guard([&] { shutdown->raise(true); server.join(); });
    const auto response = request({boost::asio::ip::make_address("127.0.0.1"), port}, test.payload);
    const auto expected = "RTSP/1.0 " + std::to_string(test.status) + " " + test.reason;
    const auto first_line = response.substr(0, response.find("\r\n"));
    if (first_line != expected) throw std::runtime_error("Unexpected response: " + first_line + "; expected: " + expected);
    if (positive) {
      const auto sessions = stream::get_all_session_info();
      if (sessions.size() != 1 || sessions.front().video_format != video::codec_wire_value(video::codec_e::pyrowave) ||
          stream::session::running_sessions.load() != 1) throw std::runtime_error("ANNOUNCE did not publish the real PyroWave session");
      if (video::acquire_pyrowave_session()) throw std::runtime_error("Active session did not retain its exclusive PyroWave lease");
#ifdef SUNSHINE_ENABLE_PYROWAVE
      if (media) {
        const auto received = pyrowave_smoke::run_udp_receive_decode(port - rtsp_stream::RTSP_SETUP_PORT + stream::VIDEO_STREAM_PORT,
                                                                    launch->width, launch->height, 3s);
        std::cout << "UDP frames=" << received.frames << " fragments=" << received.fragments
                  << " parity_fragments=" << received.parity_fragments << " discarded_frames=" << received.discarded_frames
                  << " receiver_decode_drain_fps=" << received.receiver_decode_drain_fps << " max_decoded_luma_span=" << received.max_decoded_luma_span
                  << " first_presentation_us=" << received.first_presentation_us << " last_presentation_us=" << received.last_presentation_us
                  << " total_pwvf_bytes=" << received.total_pwvf_bytes << " max_pwvf_bytes=" << received.max_pwvf_bytes << std::endl;
        const auto active = stream::get_all_session_info();
        if (active.size() != 1 || active.front().state != "running") throw std::runtime_error("Media session stopped before bitrate control checks");
        const int original_bitrate = active.front().encoder_bitrate_kbps;
        stream::request_idr_for_all_sessions();
        if (stream::set_bitrate_for_sessions(launch->unique_id, original_bitrate) != 1) throw std::runtime_error("Same-bitrate control did not select the session");
        std::this_thread::sleep_for(150ms);
        const auto after_same = stream::get_all_session_info();
        if (after_same.size() != 1 || after_same.front().state != "running") throw std::runtime_error("Same-bitrate/IDR control stopped the all-intra session");
        const int new_bitrate = original_bitrate + 1000;
        if (stream::set_bitrate_for_sessions(launch->unique_id, new_bitrate) != 1) throw std::runtime_error("Changed-bitrate control did not select the session");
        const auto after_change = stream::get_all_session_info();
        if (!after_change.empty() && after_change.front().encoder_bitrate_kbps != new_bitrate) throw std::runtime_error("Session metadata did not record bitrate control");
        const auto stop_deadline = std::chrono::steady_clock::now() + 2s;
        bool policy_stopped = false;
        do {
          const auto observed = stream::get_all_session_info();
          policy_stopped = observed.empty() || observed.front().state == "stopping" || observed.front().state == "stopped";
          if (policy_stopped) break;
          std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < stop_deadline);
        if (!policy_stopped) throw std::runtime_error("Changed bitrate did not stop PyroWave for renegotiation");
        std::cout << "CONTROL same_bitrate=" << original_bitrate << " idr_kept_running=true changed_bitrate=" << new_bitrate
                  << " policy_stopped=true" << std::endl;
      }
#endif
      shutdown->raise(true);
      server.join();
      stop.disable();
      if (rtsp_stream::session_count_no_cleanup() != 0 || stream::session::running_sessions.load() != 0 ||
          stream::session::teardown_sessions.load() != 0) throw std::runtime_error("Shutdown did not release session ownership");
      auto lease = video::acquire_pyrowave_session();
      if (!lease) throw std::runtime_error("Shutdown did not release the PyroWave lease");
      lease.reset();
      if (media) {
        const auto handle_count = [] {
          DWORD count = 0;
          if (!GetProcessHandleCount(GetCurrentProcess(), &count)) throw std::runtime_error("Cannot read probe lifecycle handle count");
          return count;
        };
        const auto before = handle_count();
        std::vector<DWORD> counts;
        counts.reserve(13);
        for (int cycle = 0; cycle != 13; ++cycle) {
          const auto probe = video::probe_pyrowave(true);
          if (!probe.available) throw std::runtime_error("Forced probe lifecycle failed: " + probe.reason);
          counts.push_back(handle_count());
          std::cout << "PROBE cycle=" << cycle << " warmup=" << (cycle == 0) << " handles=" << counts.back() << std::endl;
        }
        const auto [minimum, maximum] = std::minmax_element(counts.end() - 6, counts.end());
        std::cout << "PROBE cycles=12 warmup_cycles=1 before=" << before << " after_warmup=" << counts.front()
                  << " after=" << counts.back() << " last_six_min=" << *minimum << " last_six_max=" << *maximum << std::endl;
        if (*maximum - *minimum > 4 || counts.back() > counts.front() + 4) throw std::runtime_error("Forced probes did not reach a stable handle plateau");
      }
      std::cout << "PASS case=" << test.name << " port=" << port << " response=" << first_line
                << " real_probe=true codec=3 running_sessions=1 stopped_sessions=0 lease_reacquired=true os_callbacks=isolated video_ping="
                << (media ? "true" : "false") << " audio_ping=false" << std::endl;
    } else {
      if (rtsp_stream::session_count_no_cleanup() != 0) throw std::runtime_error("Rejection created a streaming session");
      if (GetModuleHandleW(L"libpyrowave-shared-0.dll") != nullptr) throw std::runtime_error("Rejection loaded PyroWave");
      std::cout << "PASS case=" << test.name << " port=" << port << " response=" << first_line
                << " sessions=0 runtime_loaded=false" << std::endl;
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "FAIL " << ex.what() << std::endl;
    return 1;
  }
}
