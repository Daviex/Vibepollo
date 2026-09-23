#pragma once
// Shared native reference codec helpers for standalone validation harnesses.
#include "src/platform/windows/pyrowave_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <dxgi1_2.h>
#include <wrl/client.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>

namespace pyrowave_smoke {
  using smoke_clock_t = std::chrono::steady_clock;
  template<typename T>
  using com_ptr = Microsoft::WRL::ComPtr<T>;

  inline void checked(HRESULT result, const char *operation) {
    if (FAILED(result)) {
      throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(static_cast<unsigned long>(result)));
    }
  }

  inline void checked(pyrowave_result result, const char *operation) {
    if (result != PYROWAVE_SUCCESS) {
      throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(static_cast<int>(result)));
    }
  }

  struct reference_decoder_t {
    HMODULE module = nullptr;
    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
    pyrowave_encoder comparison_encoder = nullptr;
#define PYROWAVE_FUNCTION(name) decltype(&::name) name = nullptr
    PYROWAVE_FUNCTION(pyrowave_get_api_version);
    PYROWAVE_FUNCTION(pyrowave_create_device_by_compat);
    PYROWAVE_FUNCTION(pyrowave_device_destroy);
    PYROWAVE_FUNCTION(pyrowave_decoder_create);
    PYROWAVE_FUNCTION(pyrowave_decoder_clear);
    PYROWAVE_FUNCTION(pyrowave_decoder_push_packet);
    PYROWAVE_FUNCTION(pyrowave_decoder_decode_is_ready);
    PYROWAVE_FUNCTION(pyrowave_decoder_decode_cpu_buffer_synchronous);
    PYROWAVE_FUNCTION(pyrowave_decoder_destroy);
    PYROWAVE_FUNCTION(pyrowave_encoder_create);
    PYROWAVE_FUNCTION(pyrowave_encoder_destroy);
    PYROWAVE_FUNCTION(pyrowave_encoder_encode_cpu_synchronous);
    PYROWAVE_FUNCTION(pyrowave_encoder_get_mapped_raw_bitstream);
    PYROWAVE_FUNCTION(pyrowave_encoder_compute_num_packets);
    PYROWAVE_FUNCTION(pyrowave_encoder_packetize);
#undef PYROWAVE_FUNCTION

    ~reference_decoder_t() {
      if (comparison_encoder) {
        pyrowave_encoder_destroy(comparison_encoder);
      }
      if (decoder) {
        pyrowave_decoder_destroy(decoder);
      }
      if (device) {
        pyrowave_device_destroy(device);
      }
      if (module) {
        FreeLibrary(module);
      }
    }

    void initialize(const LUID &luid, int width, int height) {
      std::array<wchar_t, 32768> executable {};
      const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
      if (!length || length >= executable.size()) {
        throw std::runtime_error("Cannot locate the test executable");
      }
      const auto path = std::filesystem::path(std::wstring(executable.data(), length)).parent_path() / L"libpyrowave-shared-0.dll";
      module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module) {
        throw std::runtime_error("Cannot load reference decoder DLL");
      }
#define PYROWAVE_LOAD(name) \
  name = std::bit_cast<decltype(name)>(GetProcAddress(module, #name)); \
  if (!name) { throw std::runtime_error("Missing " #name); }
      PYROWAVE_LOAD(pyrowave_get_api_version);
      std::uint32_t major = 0, minor = 0, patch = 0;
      pyrowave_get_api_version(&major, &minor, &patch);
      if (major != 0 || minor != 5 || patch != 0) throw std::runtime_error("Reference decoder requires PyroWave API 0.5.0");
      using contract_fn = const char *(*)();
      const auto contract = std::bit_cast<contract_fn>(GetProcAddress(module, "pyrowave_vibepollo_runtime_contract"));
      const char *value = contract ? contract() : nullptr;
      if (!value || std::strcmp(value, "d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1") != 0) throw std::runtime_error("Reference decoder requires the bundled pinned runtime contract");
      PYROWAVE_LOAD(pyrowave_create_device_by_compat);
      PYROWAVE_LOAD(pyrowave_device_destroy);
      PYROWAVE_LOAD(pyrowave_decoder_create);
      PYROWAVE_LOAD(pyrowave_decoder_clear);
      PYROWAVE_LOAD(pyrowave_decoder_push_packet);
      PYROWAVE_LOAD(pyrowave_decoder_decode_is_ready);
      PYROWAVE_LOAD(pyrowave_decoder_decode_cpu_buffer_synchronous);
      PYROWAVE_LOAD(pyrowave_decoder_destroy);
      PYROWAVE_LOAD(pyrowave_encoder_create);
      PYROWAVE_LOAD(pyrowave_encoder_destroy);
      PYROWAVE_LOAD(pyrowave_encoder_encode_cpu_synchronous);
      PYROWAVE_LOAD(pyrowave_encoder_get_mapped_raw_bitstream);
      PYROWAVE_LOAD(pyrowave_encoder_compute_num_packets);
      PYROWAVE_LOAD(pyrowave_encoder_packetize);
#undef PYROWAVE_LOAD
      pyrowave_luid identity {};
      static_assert(sizeof(identity.luid) == sizeof(luid));
      std::memcpy(identity.luid, &luid, sizeof(luid));
      checked(pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, &identity, &device), "Create reference device");
      configure_decoder(width, height);
    }

    // Retain one reference context when measuring encoder teardown, so the
    // driver's independently reproduced device-creation leak is not counted
    // once per oracle invocation.
    void configure_decoder(int width, int height) {
      if (decoder) {
        pyrowave_decoder_destroy(decoder);
        decoder = nullptr;
      }
      pyrowave_decoder_create_info info {};
      info.device = device;
      info.width = width;
      info.height = height;
      info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
      checked(pyrowave_decoder_create(&info, &decoder), "Create reference decoder");
    }

    // Diagnostic comparison only: this uses the same Vulkan codec with CPU
    // input to distinguish codec behavior from the D3D11 import path.
    platf::pyrowave::packet_list_t encode_cpu(std::vector<std::uint8_t> &source, int width, int height, std::size_t budget) {
      if (!comparison_encoder) {
        pyrowave_encoder_create_info info {device, width, height, PYROWAVE_CHROMA_SUBSAMPLING_420};
        checked(pyrowave_encoder_create(&info, &comparison_encoder), "Create comparison encoder");
      }
      const auto luma_size = static_cast<std::size_t>(width) * height;
      pyrowave_cpu_buffer input {};
      input.width = width;
      input.height = height;
      input.format = PYROWAVE_CPU_BUFFER_FORMAT_NV12;
      input.data[0] = source.data();
      input.data[1] = source.data() + luma_size;
      input.row_stride_in_bytes[0] = input.row_stride_in_bytes[1] = width;
      input.plane_size_in_bytes[0] = luma_size;
      input.plane_size_in_bytes[1] = luma_size / 2;
      pyrowave_rate_control rate {budget};
      checked(pyrowave_encoder_encode_cpu_synchronous(comparison_encoder, &input, &rate), "Encode CPU comparison");
      const void *raw = nullptr, *meta = nullptr;
      std::size_t raw_size = 0, meta_size = 0, count = 0;
      checked(pyrowave_encoder_get_mapped_raw_bitstream(comparison_encoder, &raw, &raw_size, &meta, &meta_size), "Read comparison allocation");
      if (raw_size > 64 * 1024 * 1024) throw std::runtime_error("Invalid comparison allocation");
      checked(pyrowave_encoder_compute_num_packets(comparison_encoder, 65536, &count), "Count comparison packets");
      if (count == 0 || count > 65536) throw std::runtime_error("Invalid comparison packet count");
      std::vector<pyrowave_packet> descriptors(count);
      std::vector<std::uint8_t> packed(raw_size + 8);
      std::size_t written = 0;
      checked(pyrowave_encoder_packetize(comparison_encoder, descriptors.data(), 65536, &written, packed.data(), packed.size()), "Packetize comparison");
      if (written != count) throw std::runtime_error("Comparison packet count changed");
      platf::pyrowave::packet_list_t packets;
      for (const auto &packet : descriptors) {
        if (packet.offset > packed.size() || !packet.size || packet.size > packed.size() - packet.offset) throw std::runtime_error("Invalid comparison packet");
        packets.emplace_back(packed.data() + packet.offset, packed.data() + packet.offset + packet.size);
      }
      return packets;
    }

    void decode(const platf::pyrowave::packet_list_t &packets, pyrowave_cpu_buffer &output) {
      pyrowave_decoder_clear(decoder);
      for (const auto &packet : packets) {
        if (packet.empty() || packet.size() > 8 * 1024 * 1024) {
          throw std::runtime_error("Invalid native packet length");
        }
        checked(pyrowave_decoder_push_packet(decoder, packet.data(), packet.size()), "Push native packet");
      }
      if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
        throw std::runtime_error("Reference decoder did not receive a complete frame");
      }
      checked(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &output), "Decode reference frame");
    }
  };

  inline double milliseconds(smoke_clock_t::time_point start) {
    return std::chrono::duration<double, std::milli>(smoke_clock_t::now() - start).count();
  }

  inline double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1];
  }
}  // namespace pyrowave_smoke
