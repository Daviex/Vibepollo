/**
 * @file tools/pyrowave_gpu_smoke.cpp
 * @brief Real D3D11/Vulkan encode and reference-decode check without a client.
 *
 * Uses synthetic NV12 input. This does not exercise WGC/DXGI desktop capture,
 * RGB conversion, PWVF/UDP transport, or end-to-end client presentation.
 */
#include "pyrowave/reference_codec.h"
#include "pyrowave/fixture.h"
using namespace pyrowave_smoke;

int main(int argc, char **argv) {
  try {
    int width = 1280, height = 720, frames = 30, adapter_index = 0;
    std::size_t target_bytes = 500000;
    std::string pattern = "gradient";
    std::filesystem::path fixture_directory;
    bool negative_cases = false;
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 >= argc) {
        throw std::runtime_error("Options: --width N --height N --frames N --adapter N --target-bytes N");
      }
      const std::string key = argv[i];
      if (key == "--output-dir") {
        fixture_directory = argv[i + 1];
        continue;
      }
      if (key == "--pattern") {
        pattern = argv[i + 1];
        if (pattern != "gradient" && pattern != "constant") throw std::runtime_error("Expected --pattern gradient or constant");
        continue;
      }
      const auto value = std::stoll(argv[i + 1]);
      if (value < 0 || value > 8 * 1024 * 1024) {
        throw std::runtime_error("Option is outside the supported range");
      }
      if (key == "--width") width = static_cast<int>(value);
      else if (key == "--height") height = static_cast<int>(value);
      else if (key == "--frames") frames = static_cast<int>(value);
      else if (key == "--adapter") adapter_index = static_cast<int>(value);
      else if (key == "--target-bytes") target_bytes = static_cast<std::size_t>(value);
      else if (key == "--negative-cases") negative_cases = value != 0;
      else throw std::runtime_error("Unknown option: " + key);
    }
    if (width < 2 || height < 2 || width > 8192 || height > 8192 || (width & 1) || (height & 1) ||
        frames < 2 || frames > 600 || target_bytes < 512 || target_bytes > 4 * 1024 * 1024) {
      throw std::runtime_error("Invalid dimensions, frame count, or byte budget");
    }

    com_ptr<IDXGIFactory1> factory;
    com_ptr<IDXGIAdapter1> adapter;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "Create DXGI factory");
    checked(factory->EnumAdapters1(adapter_index, adapter.GetAddressOf()), "Select GPU adapter");
    DXGI_ADAPTER_DESC1 adapter_desc {};
    checked(adapter->GetDesc1(&adapter_desc), "Read GPU identity");
    std::wcout << L"GPU=" << adapter_desc.Description << L" LUID=" << std::hex
               << static_cast<std::uint32_t>(adapter_desc.AdapterLuid.HighPart) << ':' << adapter_desc.AdapterLuid.LowPart << std::dec << '\n';

    const auto initialization_start = smoke_clock_t::now();
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
              levels, 2, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()), "Create D3D11 device");
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    com_ptr<ID3D11Texture2D> target;
    checked(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf()), "Create shared luma texture");
    desc.Width /= 2;
    desc.Height /= 2;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    com_ptr<ID3D11Texture2D> chroma_target;
    checked(device->CreateTexture2D(&desc, nullptr, chroma_target.GetAddressOf()), "Create shared chroma texture");
    std::string error;
    auto encoder = platf::pyrowave::encoder_t::create(device.Get(), context.Get(), target.Get(), chroma_target.Get(), adapter_desc.AdapterLuid, error);
    if (!encoder) {
      throw std::runtime_error(error);
    }
    if (negative_cases) {
      const LUID nonexistent {0xffffffff, -1};
      if (platf::pyrowave::encoder_t::create(device.Get(), context.Get(), target.Get(), chroma_target.Get(), nonexistent, error)) {
        throw std::runtime_error("Invalid adapter LUID was accepted");
      }
      std::cout << "PASS invalid_adapter_luid_rejected: " << error << '\n';
      if (platf::pyrowave::encoder_t::create(device.Get(), context.Get(), target.Get(), chroma_target.Get(), adapter_desc.AdapterLuid, error)) {
        throw std::runtime_error("Overlapping runtime encoder was accepted");
      }
      std::cout << "PASS overlapping_encoder_rejected: " << error << '\n';
    }
    const auto encoder_initialization_ms = milliseconds(initialization_start);
    reference_decoder_t reference;
    const auto decoder_initialization_start = smoke_clock_t::now();
    reference.initialize(adapter_desc.AdapterLuid, width, height);
    const auto decoder_initialization_ms = milliseconds(decoder_initialization_start);

    const auto luma_size = static_cast<std::size_t>(width) * height;
    const auto chroma_size = luma_size / 4;
    std::vector<std::uint8_t> source(luma_size + 2 * chroma_size);
    std::array<std::vector<std::uint8_t>, 3> decoded {
      std::vector<std::uint8_t>(luma_size), std::vector<std::uint8_t>(chroma_size), std::vector<std::uint8_t>(chroma_size)
    };
    pyrowave_cpu_buffer output {};
    output.width = width;
    output.height = height;
    output.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int plane = 0; plane < 3; ++plane) {
      output.data[plane] = decoded[plane].data();
      output.row_stride_in_bytes[plane] = width / (plane ? 2 : 1);
      output.plane_size_in_bytes[plane] = decoded[plane].size();
    }
    std::vector<double> encode_ms, decode_ms, upload_encode_ms;
    std::size_t total_bytes = 0, maximum_bytes = 0, total_packets = 0;
    double maximum_mean_absolute_error = 0;
    double maximum_gpu_cpu_difference = 0;
    platf::pyrowave::frame_statistics_t runtime_totals;
    for (int frame = 0; frame < frames; ++frame) {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          source[static_cast<std::size_t>(y) * width + x] = pattern == "constant" ? 64 + frame % 128 : 32 + ((x / 4 + y / 4 + frame * 3) % 192);
        }
      }
      for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width / 2; ++x) {
          const auto offset = luma_size + static_cast<std::size_t>(y) * width + x * 2;
          source[offset] = pattern == "constant" ? 96 + frame % 32 : 96 + ((x / 16 + frame) % 64);
          source[offset + 1] = pattern == "constant" ? 160 - frame % 32 : 96 + ((y / 16 + frame) % 64);
        }
      }

      const auto upload_start = smoke_clock_t::now();
      if (!encoder->prepare_target(error)) throw std::runtime_error(error);
      context->UpdateSubresource(target.Get(), 0, nullptr, source.data(), width, 0);
      context->UpdateSubresource(chroma_target.Get(), 0, nullptr, source.data() + luma_size, width, 0);
      if (!encoder->submit_conversion(error)) throw std::runtime_error(error);
      const auto encode_start = smoke_clock_t::now();
      auto packets = encoder->encode(target_bytes, error);
      if (!packets) throw std::runtime_error(error);
      const auto elapsed_encode = milliseconds(encode_start);
      const auto elapsed_upload_encode = milliseconds(upload_start);
      // Keep cold shader/pipeline initialization separate from steady samples.
      if (frame != 0) {
        encode_ms.push_back(elapsed_encode);
        upload_encode_ms.push_back(elapsed_upload_encode);
        const auto statistics = encoder->last_frame_statistics();
        runtime_totals.interop_submit_us += statistics.interop_submit_us;
        runtime_totals.encode_wait_us += statistics.encode_wait_us;
        runtime_totals.packetize_us += statistics.packetize_us;
      } else {
        std::cout << "first_encode_ms=" << elapsed_encode << '\n';
      }
      const auto decode_start = smoke_clock_t::now();
      reference.decode(*packets, output);
      if (frame == 0 && !fixture_directory.empty()) {
        save_fixture(fixture_directory, *packets, source, decoded, width, height, pattern, target_bytes, reference);
      }
      if (frame != 0) decode_ms.push_back(milliseconds(decode_start));
      const auto frame_bytes = std::accumulate(packets->begin(), packets->end(), std::size_t {}, [](std::size_t total, const auto &packet) { return total + packet.size(); });
      if (!frame_bytes || frame_bytes > target_bytes) throw std::runtime_error("Encoded frame exceeded the requested byte budget");
      total_bytes += frame_bytes;
      maximum_bytes = std::max(maximum_bytes, frame_bytes);
      total_packets += packets->size();

      for (int plane = 0; plane < 3; ++plane) {
        double absolute_error = 0;
        for (std::size_t sample = 0; sample < decoded[plane].size(); ++sample) {
          const auto expected = plane == 0 ? source[sample] : source[luma_size + sample * 2 + plane - 1];
          absolute_error += std::abs(static_cast<int>(decoded[plane][sample]) - static_cast<int>(expected));
        }
        const auto mean_error = absolute_error / decoded[plane].size();
        maximum_mean_absolute_error = std::max(maximum_mean_absolute_error, mean_error);
        // A broad corruption/stale-frame/color-plane guard, not a quality claim.
        if (mean_error > 12.0) {
          std::cerr << "frame=" << frame << " plane=" << plane << " decoded_min=" << static_cast<int>(*std::min_element(decoded[plane].begin(), decoded[plane].end()))
                    << " decoded_max=" << static_cast<int>(*std::max_element(decoded[plane].begin(), decoded[plane].end())) << " samples(expected/actual):";
          for (std::size_t sample : {std::size_t {0}, std::size_t {25}, std::size_t {400}, decoded[plane].size() / 2}) {
            if (sample < decoded[plane].size()) {
              const auto expected = plane == 0 ? source[sample] : source[luma_size + sample * 2 + plane - 1];
              std::cerr << ' ' << static_cast<int>(expected) << '/' << static_cast<int>(decoded[plane][sample]);
            }
          }
          std::cerr << '\n';
          const auto gpu_decoded = decoded;
          reference.decode(reference.encode_cpu(source, width, height, target_bytes), output);
          for (int comparison_plane = 0; comparison_plane < 3; ++comparison_plane) {
            double baseline_error = 0, difference = 0;
            for (std::size_t sample = 0; sample < decoded[comparison_plane].size(); ++sample) {
              const auto expected = comparison_plane == 0 ? source[sample] : source[luma_size + sample * 2 + comparison_plane - 1];
              baseline_error += std::abs(static_cast<int>(decoded[comparison_plane][sample]) - static_cast<int>(expected));
              difference += std::abs(static_cast<int>(decoded[comparison_plane][sample]) - static_cast<int>(gpu_decoded[comparison_plane][sample]));
            }
            std::cerr << "comparison_plane=" << comparison_plane << " cpu_input_mae=" << baseline_error / decoded[comparison_plane].size()
                      << " gpu_vs_cpu_input_mae=" << difference / decoded[comparison_plane].size() << '\n';
          }
          throw std::runtime_error("Reference plane differs excessively from the synthetic source (MAE " + std::to_string(mean_error) + ')');
        }
      }
      if (frame == 0) {
        // A CPU-input control run uses the identical Vulkan codec. This checks
        // the external image route independently of the lossy-codec tolerance.
        const auto gpu_decoded = decoded;
        reference.decode(reference.encode_cpu(source, width, height, target_bytes), output);
        for (int plane = 0; plane < 3; ++plane) {
          double difference = 0;
          for (std::size_t sample = 0; sample < decoded[plane].size(); ++sample) {
            difference += std::abs(static_cast<int>(decoded[plane][sample]) - static_cast<int>(gpu_decoded[plane][sample]));
          }
          maximum_gpu_cpu_difference = std::max(maximum_gpu_cpu_difference, difference / decoded[plane].size());
        }
        if (maximum_gpu_cpu_difference > 1.0) {
          throw std::runtime_error("External-image encode differs from CPU-input control (MAE " + std::to_string(maximum_gpu_cpu_difference) + ')');
        }
      }
      std::fill(decoded[0].begin(), decoded[0].end(), 0xff);
      std::fill(decoded[1].begin(), decoded[1].end(), 0xff);
      std::fill(decoded[2].begin(), decoded[2].end(), 0xff);
    }
    std::cout << std::fixed << std::setprecision(3)
              << "PASS dimensions=" << width << 'x' << height << " frames=" << frames << " pattern=" << pattern << " target_bytes=" << target_bytes
              << " native_packets=" << total_packets << " mean_frame_bytes=" << total_bytes / frames << " max_frame_bytes=" << maximum_bytes << '\n'
              << "encoder_init_ms=" << encoder_initialization_ms << " decoder_init_ms=" << decoder_initialization_ms << '\n'
              << "encode_and_packetize_ms p50=" << percentile(encode_ms, 0.50) << " p95=" << percentile(encode_ms, 0.95) << '\n'
              << "synthetic_upload_plus_encode_ms p50=" << percentile(upload_encode_ms, 0.50) << " p95=" << percentile(upload_encode_ms, 0.95) << '\n'
              << "reference_decode_and_readback_ms p50=" << percentile(decode_ms, 0.50) << " p95=" << percentile(decode_ms, 0.95) << '\n'
              << "runtime_mean_us interop_submit=" << runtime_totals.interop_submit_us / (frames - 1)
              << " encode_wait=" << runtime_totals.encode_wait_us / (frames - 1) << " packetize=" << runtime_totals.packetize_us / (frames - 1) << '\n'
              << "maximum_plane_mae=" << maximum_mean_absolute_error << " first_frame_gpu_vs_cpu_mae=" << maximum_gpu_cpu_difference << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
