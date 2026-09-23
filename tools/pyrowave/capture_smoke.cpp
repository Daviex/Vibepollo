/**
 * Capture validation using the actual server display factory and converter.
 * Links a copy of the server objects, but never calls the server entry point.
 * No display modes are changed and no captured pixels/bitstreams are saved.
 */
#include "src/platform/windows/display_vram.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "reference_codec.h"

#include <condition_variable>
#include <thread>
#include <psapi.h>

namespace platf::dxgi { int init(); }

namespace {
  using namespace pyrowave_smoke;
  constexpr video::sunshine_colorspace_t profile {video::colorspace_e::rec709, true, 8};

  struct planes_t {
    std::array<std::vector<std::uint8_t>, 3> pixels;
    pyrowave_cpu_buffer buffer {};
    planes_t(int width, int height) {
      buffer.width = width;
      buffer.height = height;
      buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
      for (int p = 0; p < 3; ++p) {
        const int scale = p ? 2 : 1;
        pixels[p].resize(static_cast<std::size_t>(width / scale) * (height / scale));
        buffer.data[p] = pixels[p].data();
        buffer.row_stride_in_bytes[p] = width / scale;
        buffer.plane_size_in_bytes[p] = pixels[p].size();
      }
    }
  };

  // Test-only readback of the same captured texture passed to the converter.
  // Production encoding remains GPU-only until the compressed bitstream.
  struct rgb_image_t {
    std::vector<float> pixels;
    bool linear;
  };

  float half_to_float(std::uint16_t bits) {
    const auto exponent = (bits >> 10) & 31;
    const auto mantissa = bits & 1023;
    if (exponent == 31) throw std::runtime_error("Non-finite captured scRGB pixel");
    const auto magnitude = exponent ? std::ldexp(static_cast<float>(1024 + mantissa), int(exponent) - 25) : std::ldexp(static_cast<float>(mantissa), -24);
    return bits & 0x8000 ? -magnitude : magnitude;
  }

  rgb_image_t read_rgb(platf::dxgi::display_base_t &display, platf::dxgi::img_d3d_t &img) {
    D3D11_TEXTURE2D_DESC desc {};
    img.capture_texture->GetDesc(&desc);
    const bool linear = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((!linear && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) || desc.Width != static_cast<UINT>(display.width) ||
        desc.Height != static_cast<UINT>(display.height)) {
      throw std::runtime_error("The CPU oracle requires BGRA8 or scRGB FP16 and unrotated capture");
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    com_ptr<ID3D11Texture2D> staging;
    checked(display.device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "Create oracle readback");
    if (img.capture_mutex && img.capture_mutex->AcquireSync(0, 3000) != S_OK) {
      throw std::runtime_error("Timed out acquiring captured texture for oracle");
    }
    display.device_ctx->CopyResource(staging.Get(), img.capture_texture.get());
    if (img.capture_mutex) checked(img.capture_mutex->ReleaseSync(0), "Release oracle capture mutex");
    D3D11_MAPPED_SUBRESOURCE mapped {};
    checked(display.device_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read captured pixels");
    auto unmap = util::fail_guard([&]() { display.device_ctx->Unmap(staging.Get(), 0); });
    rgb_image_t image {std::vector<float>(static_cast<std::size_t>(desc.Width) * desc.Height * 3), linear};
    for (UINT y = 0; y < desc.Height; ++y) {
      const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
      for (UINT x = 0; x < desc.Width; ++x) for (int c = 0; c < 3; ++c) {
        image.pixels[(static_cast<std::size_t>(y) * desc.Width + x) * 3 + c] = linear ?
          half_to_float(reinterpret_cast<const std::uint16_t *>(row)[x * 4 + c]) : row[x * 4 + 2 - c] / 255.0f;
      }
    }
    std::cout << "source_format=" << (linear ? "scRGB-FP16" : "BGRA8") << " source_hdr=false" << std::endl;
    return image;
  }

  // Independent CPU evaluation of the server's documented BT.709 matrix and
  // left-sited shader taps, including its separate downscaling UV filter.
  void expected_planes(const rgb_image_t &source, int source_width, int source_height, planes_t &expected) {
    const int width = expected.buffer.width, height = expected.buffer.height;
    const auto *matrix = video::color_vectors_from_colorspace(profile, true);
    auto sample = [&](double u, double v) {
      const double x = u * source_width - 0.5, y = v * source_height - 0.5;
      const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
      const double fx = x - x0, fy = y - y0;
      std::array<double, 3> rgb {};
      for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
        const auto offset = (static_cast<std::size_t>(std::clamp(y0 + j, 0, source_height - 1)) * source_width +
                            std::clamp(x0 + i, 0, source_width - 1)) * 3;
        const double weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        for (int c = 0; c < 3; ++c) rgb[c] += source.pixels[offset + c] * weight;
      }
      return rgb;
    };
    auto value = [&](std::array<double, 3> rgb, const float *vec, const float *range) {
      for (auto &component : rgb) {
        component = std::clamp(component, 0.0, 1.0);
        if (source.linear) component = component < 0.0031308 ? 12.92 * component : 1.13005 * std::sqrt(component - 0.00228) - 0.13448 * component + 0.005719;
      }
      const double converted = (rgb[0] * vec[0] + rgb[1] * vec[1] + rgb[2] * vec[2] + vec[3]) * range[0] + range[1];
      return static_cast<std::uint8_t>(std::clamp(std::lround(converted * 255), 0L, 255L));
    };
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      expected.pixels[0][static_cast<std::size_t>(y) * width + x] = value(sample((x + 0.5) / width, (y + 0.5) / height), matrix->color_vec_y, matrix->range_y);
    }
    const bool downscale = source_width > width || source_height > height;
    for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width / 2; ++x) {
      const double u = (2.0 * x + 1) / width, v = (2.0 * y + 1) / height;
      std::array<double, 3> rgb {};
      if (downscale) {
        for (int j = 0; j < 2; ++j) for (int i = 0; i < 3; ++i) {
          const auto tap = sample(u + (0.5 - i) / width, v + (j ? 0.5 : -0.5) / height);
          for (int c = 0; c < 3; ++c) rgb[c] += tap[c] * (i == 1 ? 2.0 : 1.0) / 8.0;
        }
      } else {
        const auto left = sample(u - 1.0 / width, v), right = sample(u, v);
        for (int c = 0; c < 3; ++c) rgb[c] = (left[c] + right[c]) / 2;
      }
      const auto offset = static_cast<std::size_t>(y) * (width / 2) + x;
      expected.pixels[1][offset] = value(rgb, matrix->color_vec_u, matrix->range_uv);
      expected.pixels[2][offset] = value(rgb, matrix->color_vec_v, matrix->range_uv);
    }
  }

  void run_phase(const std::string &backend, const platf::dxgi::capture_output_identity_t &identity,
                 int phase, int divisor, int frames, std::size_t budget, reference_decoder_t &reference) {
    video::config_t request {};
    request.width = 1280;
    request.height = 720;
    request.framerate = request.encodingFramerate = 30;
    request.framerateX100 = 3000;
    request.encoderCscMode = 3;
    request.force_sdr = true;
    const auto start = smoke_clock_t::now();
    auto display = platf::display(platf::mem_type_e::dxgi, identity.output_name, request, identity.adapter_id);
    if (!display) throw std::runtime_error("Actual display factory failed for " + backend);
    auto *native_display = dynamic_cast<platf::dxgi::display_base_t *>(display.get());
    if (!native_display || display->is_hdr()) throw std::runtime_error("This validation requires an existing SDR desktop; desktop settings were not changed");
    if (native_display->display_rotation != DXGI_MODE_ROTATION_IDENTITY && native_display->display_rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
      throw std::runtime_error("CPU oracle currently requires an unrotated output");
    }
    if ((display->width % (2 * divisor)) || (display->height % (2 * divisor))) throw std::runtime_error("Capture size is not divisible by the requested exact scaling factor");
    request.width = display->width / divisor;
    request.height = display->height / divisor;
    if (phase == 0) {
      auto rejected = display->make_pyrowave_encode_device();
      auto invalid = request;
      invalid.width = 0;
      if (!rejected || rejected->init_encoder(invalid, profile)) throw std::runtime_error("Zero target width was accepted");
      invalid.width = 63;
      if (rejected->init_encoder(invalid, profile)) throw std::runtime_error("Odd target width was accepted");
      std::cout << "PASS zero_and_odd_output_dimensions_rejected" << std::endl;
    }
    auto encoder = display->make_pyrowave_encode_device();
    if (!encoder || !encoder->init_encoder(request, profile)) throw std::runtime_error("Actual server PyroWave factory failed");
    const auto encoder_init_ms = milliseconds(start);
    const auto decoder_start = smoke_clock_t::now();
    if (!reference.device) reference.initialize(native_display->captured_adapter_luid, request.width, request.height);
    else reference.configure_decoder(request.width, request.height);
    const auto decoder_init_ms = milliseconds(decoder_start);
    planes_t decoded(request.width, request.height), expected(request.width, request.height);
    auto image = display->alloc_img();
    if (!image) throw std::runtime_error("Capture image allocation failed");
    auto cleanup = util::fail_guard([&]() {
      display->prepare_for_reinit();
      encoder.reset();
      image.reset();
      display.reset();
    });
    bool cursor = false;
    int encoded = 0, fresh = 0;
    bool have_source = false;
    std::array<double, 3> first_mae {};
    std::size_t total_bytes = 0, max_bytes = 0;
    std::vector<double> conversion_encode_ms, decode_ms;
    const auto deadline = smoke_clock_t::now() + std::chrono::seconds(10);
    auto push = [&](std::shared_ptr<platf::img_t> &&captured, bool captured_new) {
      if (smoke_clock_t::now() >= deadline) return false;
      if (captured_new && captured) {
        image = std::move(captured);
        auto *native = dynamic_cast<platf::dxgi::img_d3d_t *>(image.get());
        if (!native || native->blank || native->dummy) throw std::runtime_error("Capture returned a placeholder");
        have_source = true;
        ++fresh;
      }
      if (!have_source) return true;
      if (encoded == 0) {
        auto rgb = read_rgb(*native_display, *static_cast<platf::dxgi::img_d3d_t *>(image.get()));
        expected_planes(rgb, display->width, display->height, expected);
      }
      const auto encode_start = smoke_clock_t::now();
      if (encoder->convert(*image) != 0) throw std::runtime_error("Actual server RGB conversion failed");
      auto packets = encoder->encode_frame(encoded, budget);
      if (!packets || packets->empty()) throw std::runtime_error("Actual server native encode failed");
      const auto elapsed_encode_ms = milliseconds(encode_start);
      std::size_t bytes = 0;
      for (const auto &packet : *packets) bytes += packet.size();
      if (bytes > budget) throw std::runtime_error("Native frame exceeded its byte budget");
      max_bytes = std::max(max_bytes, bytes);
      total_bytes += bytes;
      const auto decode_start = smoke_clock_t::now();
      reference.decode(*packets, decoded.buffer);
      const auto elapsed_decode_ms = milliseconds(decode_start);
      if (encoded > 0) {
        conversion_encode_ms.push_back(elapsed_encode_ms);
        decode_ms.push_back(elapsed_decode_ms);
      } else {
        for (int p = 0; p < 3; ++p) {
          std::uint64_t difference = 0;
          for (std::size_t i = 0; i < decoded.pixels[p].size(); ++i) difference += std::abs(int(decoded.pixels[p][i]) - int(expected.pixels[p][i]));
          first_mae[p] = static_cast<double>(difference) / decoded.pixels[p].size();
        }
        std::cout << "phase=" << phase << " first_conversion_encode_ms=" << elapsed_encode_ms
                  << " oracle_mae_y=" << first_mae[0] << " oracle_mae_u=" << first_mae[1] << " oracle_mae_v=" << first_mae[2] << std::endl;
        if (*std::max_element(first_mae.begin(), first_mae.end()) > 12) throw std::runtime_error("Decoded desktop differs from independent RGB/BT.709 oracle");
        const auto [low, high] = std::minmax_element(decoded.pixels[0].begin(), decoded.pixels[0].end());
        if (int(*high) - int(*low) < 8) throw std::runtime_error("Decoded desktop lacks luma detail; this fixture cannot validate real capture");
      }
      return ++encoded < frames;
    };
    auto pull = [&](std::shared_ptr<platf::img_t> &output) {
      if (smoke_clock_t::now() >= deadline) return false;
      output = image;
      return true;
    };
    const auto result = display->capture(push, pull, &cursor);
    if (result != platf::capture_e::ok || encoded != frames || fresh == 0) throw std::runtime_error("Capture did not complete the requested frame sequence");
    const bool actual_wgc = dynamic_cast<platf::dxgi::display_wgc_ipc_vram_t *>(display.get()) != nullptr;
    std::cout << "PASS phase=" << phase << " requested_backend=" << backend << " factory_backend=" << (actual_wgc ? "wgc" : "ddx")
              << " capture=" << display->width << 'x' << display->height << " output=" << request.width << 'x' << request.height
              << " frames=" << encoded << " fresh_frames=" << fresh << " budget=" << budget << " mean_native_bytes=" << total_bytes / encoded
              << " max_native_bytes=" << max_bytes << " encoder_capture_init_ms=" << encoder_init_ms << " decoder_init_ms=" << decoder_init_ms
              << " conversion_encode_p50_ms=" << percentile(conversion_encode_ms, 0.5) << " conversion_encode_p95_ms=" << percentile(conversion_encode_ms, 0.95)
              << " decode_readback_p50_ms=" << percentile(decode_ms, 0.5) << std::endl;
  }

  struct process_resources_t { DWORD handles; SIZE_T private_bytes; };
  process_resources_t process_resources() {
    DWORD handles = 0;
    PROCESS_MEMORY_COUNTERS_EX memory {};
    memory.cb = sizeof(memory);
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles) ||
        !GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
      throw std::runtime_error("Cannot inspect validation process resources");
    }
    return {handles, memory.PrivateUsage};
  }
}

int main(int argc, char **argv) {
  std::mutex watchdog_mutex;
  std::condition_variable_any watchdog_condition;
  std::jthread watchdog([&](std::stop_token stop) {
    std::unique_lock lock(watchdog_mutex);
    watchdog_condition.wait_for(lock, stop, std::chrono::seconds(60), [] { return false; });
    if (!stop.stop_requested()) {
      std::cerr << "FAIL: validation exceeded its 60 second process deadline" << std::endl;
      TerminateProcess(GetCurrentProcess(), 124);
    }
  });
  try {
    std::cout << std::fixed << std::setprecision(4);
    const std::string backend = argc > 1 ? argv[1] : "ddx";
    const int cycles = argc > 2 ? std::stoi(argv[2]) : 3;
    const int frames_per_cycle = argc > 3 ? std::stoi(argv[3]) : 8;
    if (argc > 4 || (backend != "ddx" && backend != "wgc") || cycles < 3 || cycles > 20 || frames_per_cycle < 2 || frames_per_cycle > 30) {
      throw std::runtime_error("Usage: pyrowave-capture-smoke.exe [ddx|wgc] [cycles3..20] [frames2..30]");
    }
    auto logger = logging::init_single_file(2, "pyrowave-capture-smoke.log");
    mail::man = std::make_shared<safe::mail_raw_t>();
    // In-memory settings only. Do not parse/save a user configuration or invoke
    // platform initialization that applies display/GPU preferences.
    config::video.capture = backend;
    config::video.adapter_name.clear();
    config::video.adapter_pnp_id.clear();
    config::video.nv_realtime_hags = false;
    if (platf::dxgi::init() != 0) throw std::runtime_error("Server shader initialization failed");
    const auto identity = platf::dxgi::resolve_automatic_capture_output(platf::mem_type_e::dxgi);
    if (!identity) throw std::runtime_error("No capturable output is available");
    std::cout << "OUTPUT=" << identity->output_name << " LUID=" << std::hex << identity->adapter_id.high_part << ':' << identity->adapter_id.low_part << std::dec << std::endl;
    // Warm both output sizes and the return to native before the resource
    // baseline. Every measured phase still creates new display/device lifetimes.
    // This changes encoder resources, never desktop modes.
    reference_decoder_t reference;
    run_phase(backend, *identity, 0, 1, 3, 4 * 1024 * 1024, reference);
    run_phase(backend, *identity, -1, 2, 3, 4 * 1024 * 1024, reference);
    run_phase(backend, *identity, -2, 1, 3, 4 * 1024 * 1024, reference);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto baseline = process_resources();
    std::cout << "resources_cycle=0 handles=" << baseline.handles << " private_bytes=" << baseline.private_bytes << std::endl;
    std::vector<DWORD> handle_samples {baseline.handles};
    for (int cycle = 1; cycle <= cycles; ++cycle) {
      run_phase(backend, *identity, cycle, cycle % 2 ? 1 : 2, frames_per_cycle, 4 * 1024 * 1024, reference);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const auto resources = process_resources();
      std::cout << "resources_cycle=" << cycle << " handles=" << resources.handles << " private_bytes=" << resources.private_bytes << std::endl;
      handle_samples.push_back(resources.handles);
    }
    const auto final_resources = process_resources();
    const auto handle_growth = static_cast<std::int64_t>(final_resources.handles) - baseline.handles;
    const auto private_growth = static_cast<std::int64_t>(final_resources.private_bytes) - static_cast<std::int64_t>(baseline.private_bytes);
    std::cout << "post_warmup_handle_growth=" << handle_growth << " post_warmup_private_bytes_growth=" << private_growth << std::endl;
    // The original loose aggregate bound hid +12 handles on every cycle. A
    // lifecycle pass now requires a handle plateau, including the last six
    // observations in longer runs. Memory still allows bounded driver caches.
    const auto tail_count = std::min<std::size_t>(6, handle_samples.size());
    const auto [low_handles, high_handles] = std::minmax_element(handle_samples.end() - tail_count, handle_samples.end());
    if (*high_handles - *low_handles > 4 || handle_growth > 4) throw std::runtime_error("Capture handles did not stabilize after warmup");
    if (private_growth > 128 * 1024 * 1024) throw std::runtime_error("Process memory growth exceeded the bounded-cache allowance");
    std::cout << "PASS capture/conversion/native encode/reference decode; stop/recreate/output resize; desktop mode unchanged" << std::endl;
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << "FAIL: " << failure.what() << std::endl;
    return 1;
  }
}
