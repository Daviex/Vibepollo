/**
 * @file src/platform/windows/pyrowave_runtime.cpp
 * @brief PyroWave C API adapter with a GPU-only D3D11 input path.
 */
#include "pyrowave_runtime.h"
#include "src/pyrowave_runtime_contract.h"

#ifdef SUNSHINE_ENABLE_PYROWAVE

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <sstream>
#include <utility>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>

namespace platf::pyrowave {
  namespace {
    constexpr std::size_t maximum_target_bytes = 8 * 1024 * 1024;
    constexpr std::size_t maximum_raw_bytes = 64 * 1024 * 1024;
    constexpr std::size_t native_packet_boundary = 64 * 1024;
    constexpr std::size_t maximum_native_packets = 65536;
    constexpr std::size_t maximum_sideband_words = 32768;
    constexpr std::uint64_t gpu_wait_nanoseconds = 3'000'000'000;

    using statistics_clock_t = std::chrono::steady_clock;
    double elapsed_microseconds(statistics_clock_t::time_point start) {
      return std::chrono::duration<double, std::micro>(statistics_clock_t::now() - start).count();
    }

    std::string hresult_message(const char *operation, HRESULT result) {
      std::ostringstream text;
      text << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
      return text.str();
    }

    std::string api_message(const char *operation, pyrowave_result result) {
      return std::string(operation) + " failed (PyroWave " + std::to_string(static_cast<int>(result)) + ')';
    }

    struct handle_t {
      HANDLE value = nullptr;
      ~handle_t() {
        if (value) {
          CloseHandle(value);
        }
      }
      HANDLE release() {
        return std::exchange(value, nullptr);
      }
    };

    struct module_t {
      HMODULE value = nullptr;
      ~module_t() {
        if (value) {
          FreeLibrary(value);
        }
      }
    };

    struct api_t {
      module_t module;

#define PYROWAVE_ENTRY(name) decltype(&::name) name = nullptr
      PYROWAVE_ENTRY(pyrowave_get_api_version);
      PYROWAVE_ENTRY(pyrowave_vibepollo_configure_precision);
      PYROWAVE_ENTRY(pyrowave_create_device_by_compat);
      PYROWAVE_ENTRY(pyrowave_device_set_queue_type);
      PYROWAVE_ENTRY(pyrowave_device_destroy);
      PYROWAVE_ENTRY(pyrowave_vibepollo_device_activate);
      PYROWAVE_ENTRY(pyrowave_device_report_performance_stats);
      PYROWAVE_ENTRY(pyrowave_image_create);
      PYROWAVE_ENTRY(pyrowave_image_get_image_view);
      PYROWAVE_ENTRY(pyrowave_image_destroy);
      PYROWAVE_ENTRY(pyrowave_sync_object_create);
      PYROWAVE_ENTRY(pyrowave_sync_object_get_semaphore);
      PYROWAVE_ENTRY(pyrowave_sync_object_cpu_wait);
      PYROWAVE_ENTRY(pyrowave_sync_object_destroy);
      PYROWAVE_ENTRY(pyrowave_encoder_create);
      PYROWAVE_ENTRY(pyrowave_vibepollo_encoder_set_color_info);
      PYROWAVE_ENTRY(pyrowave_encoder_encode_gpu_synchronous);
      PYROWAVE_ENTRY(pyrowave_encoder_get_mapped_raw_bitstream);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_packets);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_critical_packets);
      PYROWAVE_ENTRY(pyrowave_encoder_get_num_active_blocks);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_block_active_words);
      PYROWAVE_ENTRY(pyrowave_encoder_packetize);
      PYROWAVE_ENTRY(pyrowave_encoder_destroy);
#undef PYROWAVE_ENTRY

      bool load(std::string &error) {
        std::array<wchar_t, 32768> executable {};
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) {
          error = "Cannot resolve the executable directory for the PyroWave runtime";
          return false;
        }
        const auto dll = std::filesystem::path(std::wstring(executable.data(), length)).parent_path() / L"libpyrowave-shared-0.dll";
        // The runtime is optional. Never search CWD/PATH or load it at process startup.
        module.value = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module.value) {
          error = "Cannot load libpyrowave-shared-0.dll beside the executable (Windows " + std::to_string(GetLastError()) + ')';
          return false;
        }

#define PYROWAVE_LOAD(name) \
  name = std::bit_cast<decltype(name)>(GetProcAddress(module.value, #name)); \
  if (!name) { \
    error = "PyroWave runtime is missing " #name; \
    return false; \
  }
        PYROWAVE_LOAD(pyrowave_get_api_version);
        std::uint32_t major = 0, minor = 0, patch = 0;
        pyrowave_get_api_version(&major, &minor, &patch);
        if (major != 0 || minor != 5 || patch != 0) {
          error = "Unsupported PyroWave API " + std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch) + "; expected 0.5.0";
          return false;
        }
        // The local ownership patch changes failure-path HANDLE consumption.
        // ABI 0.5.0 alone is insufficient to accept an unpatched upstream DLL.
        using runtime_contract_fn = const char *(*)();
        const auto runtime_contract = std::bit_cast<runtime_contract_fn>(GetProcAddress(module.value, "pyrowave_vibepollo_runtime_contract"));
        constexpr auto expected_contract = ::pyrowave::runtime_contract;
        const char *actual_contract = runtime_contract ? runtime_contract() : nullptr;
        if (!actual_contract || actual_contract != expected_contract) {
          error = "PyroWave runtime contract is missing or incompatible; install the bundled pinned runtime";
          return false;
        }
        PYROWAVE_LOAD(pyrowave_vibepollo_configure_precision);
        PYROWAVE_LOAD(pyrowave_create_device_by_compat);
        PYROWAVE_LOAD(pyrowave_device_set_queue_type);
        PYROWAVE_LOAD(pyrowave_device_destroy);
        PYROWAVE_LOAD(pyrowave_vibepollo_device_activate);
        PYROWAVE_LOAD(pyrowave_device_report_performance_stats);
        PYROWAVE_LOAD(pyrowave_image_create);
        PYROWAVE_LOAD(pyrowave_image_get_image_view);
        PYROWAVE_LOAD(pyrowave_image_destroy);
        PYROWAVE_LOAD(pyrowave_sync_object_create);
        PYROWAVE_LOAD(pyrowave_sync_object_get_semaphore);
        PYROWAVE_LOAD(pyrowave_sync_object_cpu_wait);
        PYROWAVE_LOAD(pyrowave_sync_object_destroy);
        PYROWAVE_LOAD(pyrowave_encoder_create);
        PYROWAVE_LOAD(pyrowave_vibepollo_encoder_set_color_info);
        PYROWAVE_LOAD(pyrowave_encoder_encode_gpu_synchronous);
        PYROWAVE_LOAD(pyrowave_encoder_get_mapped_raw_bitstream);
        PYROWAVE_LOAD(pyrowave_encoder_compute_num_packets);
        PYROWAVE_LOAD(pyrowave_encoder_compute_num_critical_packets);
        PYROWAVE_LOAD(pyrowave_encoder_get_num_active_blocks);
        PYROWAVE_LOAD(pyrowave_encoder_compute_block_active_words);
        PYROWAVE_LOAD(pyrowave_encoder_packetize);
        PYROWAVE_LOAD(pyrowave_encoder_destroy);
#undef PYROWAVE_LOAD
        return true;
      }
    };

    struct process_runtime_t {
      // Granite keeps device dispatch tables, but volk's instance dispatch is
      // global. Serialize C API groups and reactivate their instance each time.
      std::mutex mutex;
      api_t api;
      bool attempted = false;
      bool available = false;
      std::string load_error;
      int precision = -1;

      bool load() {
        if (!attempted) {
          attempted = true;
          available = api.load(load_error);
        }
        return available;
      }
    };

    std::shared_ptr<process_runtime_t> process_runtime() {
      static const auto runtime = std::make_shared<process_runtime_t>();
      return runtime;
    }

    struct process_context_t {
      std::shared_ptr<process_runtime_t> runtime;
      std::mutex &mutex;
      api_t &api;
      pyrowave_device device = nullptr;
      LUID luid {};
      bool attempted = false;
      std::atomic_bool poisoned {false};

      process_context_t(std::shared_ptr<process_runtime_t> shared_runtime, const LUID &identity):
          runtime {std::move(shared_runtime)}, mutex {runtime->mutex}, api {runtime->api}, luid {identity} {
      }

      ~process_context_t() {
        std::lock_guard lock {mutex};
        if (device) {
          api.pyrowave_vibepollo_device_activate(device);
          api.pyrowave_device_destroy(device);
        }
      }
    };

    std::shared_ptr<process_context_t> process_context(const LUID &luid) {
      struct registry_t {
        // Declaration order and shared ownership retain the lock and DLL until
        // every device/session is destroyed, including during process shutdown.
        std::shared_ptr<process_runtime_t> runtime = process_runtime();
        std::map<std::uint64_t, std::shared_ptr<process_context_t>> adapters;
      };
      static registry_t registry;
      std::lock_guard lock {registry.runtime->mutex};
      const auto key = (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
      auto &entry = registry.adapters[key];
      if (!entry) entry = std::make_shared<process_context_t>(registry.runtime, luid);
      // Retain one context for each actual adapter used. Avoid recreating Vulkan
      // devices across probes/sessions (which leaks driver handles on this host).
      return entry;
    }
  }  // namespace

  bool configure_precision(int requested, int &effective, std::string &error) {
    error.clear();
    auto runtime = process_runtime();
    std::lock_guard lock {runtime->mutex};
    effective = runtime->precision;
    if (requested < -1 || requested > 2) {
      error = "PyroWave precision must be auto (-1), FP16 (0), mixed (1), or FP32 (2)";
      return false;
    }
    if (!runtime->load()) {
      error = runtime->load_error;
      return false;
    }
    const auto result = runtime->api.pyrowave_vibepollo_configure_precision(requested, &runtime->precision);
    effective = runtime->precision;
    if (result != PYROWAVE_SUCCESS) {
      error = "PyroWave wavelet precision is fixed after the first encoder; restart the host to change it";
      return false;
    }
    return true;
  }

  int effective_precision() {
    auto runtime = process_runtime();
    std::lock_guard lock {runtime->mutex};
    return runtime->precision;
  }

  struct encoder_t::impl_t {
    std::shared_ptr<process_context_t> cached_context;
    api_t &api;

    explicit impl_t(const LUID &luid): cached_context {process_context(luid)}, api {cached_context->api} {
    }
    // D3D resources outlive every imported Vulkan resource.
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> targets;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    pyrowave_device device = nullptr;
    std::array<pyrowave_image, 2> images {};
    pyrowave_sync_object sync = nullptr;
    pyrowave_encoder encoder = nullptr;
    pyrowave_gpu_buffers buffers {};
    std::uint64_t timeline = 0;
    std::uint64_t pending_release = 0;
    bool conversion_ready = false;
    bool failed = false;
    frame_statistics_t statistics;

    struct exception_guard_t {
      impl_t &state;
      int exceptions = std::uncaught_exceptions();
      ~exception_guard_t() {
        if (std::uncaught_exceptions() > exceptions) {
          state.failed = true;
          state.cached_context->poisoned = true;
        }
      }
    };

    ~impl_t() {
      std::lock_guard lock {cached_context->mutex};
      if (device) api.pyrowave_vibepollo_device_activate(device);
      // Upstream teardown waits for submitted work. Do not free external D3D
      // resources first, even after a failed wait/device-loss notification.
      if (encoder) {
        api.pyrowave_encoder_destroy(encoder);
      }
      for (auto image : images) {
        if (image) {
          api.pyrowave_image_destroy(image);
        }
      }
      if (sync) {
        api.pyrowave_sync_object_destroy(sync);
      }
      if (context) {
        context->Flush();
      }
    }

    bool check(pyrowave_result result, const char *operation, std::string &error) {
      if (result == PYROWAVE_SUCCESS) {
        return true;
      }
      error = api_message(operation, result);
      failed = true;
      cached_context->poisoned = true;
      error += "; restart the host before retrying PyroWave";
      return false;
    }

    bool check_resource(pyrowave_result result, const char *operation, std::string &error) {
      if (result == PYROWAVE_SUCCESS) return true;
      // Allocation/import rejection precedes submission by this encoder. Its
      // resources unwind independently and must not stop other active sessions.
      error = api_message(operation, result);
      failed = true;
      return false;
    }

    bool initialize(ID3D11Device *d3d_device, ID3D11DeviceContext *d3d_context, ID3D11Texture2D *luma, ID3D11Texture2D *chroma, const LUID &luid, const ::pyrowave::profile_t &profile, std::string &error) {
      std::array<D3D11_TEXTURE2D_DESC, 2> descriptions {};
      targets = {luma, chroma};
      for (unsigned plane = 0; plane < 2; ++plane) {
        auto &desc = descriptions[plane];
        targets[plane]->GetDesc(&desc);
        const auto expected_format = profile.high_precision ?
          (plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM) :
          (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
        if (desc.Format != expected_format || !desc.Width || !desc.Height ||
            desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
          error = "PyroWave requires shared luma/chroma UNORM textures matching the selected 8-bit or 16-bit profile";
          return false;
        }
      }
      const auto &desc = descriptions[0];
      const auto chroma_divisor = profile.chroma_444 ? 1u : 2u;
      if ((!profile.chroma_444 && ((desc.Width & 1) || (desc.Height & 1))) || desc.Width > 16384 || desc.Height > 16384 ||
          descriptions[1].Width != desc.Width / chroma_divisor || descriptions[1].Height != desc.Height / chroma_divisor) {
        error = "PyroWave requires dimensions up to 16384, full-size chroma for 4:4:4 or even dimensions with half-size chroma for 4:2:0";
        return false;
      }

      if (profile.input_plane_bytes(desc.Width, desc.Height) > ::pyrowave::maximum_input_plane_bytes) {
        error = "PyroWave input planes exceed the host allocation limit";
        return false;
      }

      Microsoft::WRL::ComPtr<ID3D11Device5> device5;
      auto status = d3d_device->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()));
      if (FAILED(status)) {
        error = hresult_message("Query ID3D11Device5", status);
        return false;
      }
      status = d3d_context->QueryInterface(IID_PPV_ARGS(context.GetAddressOf()));
      if (FAILED(status)) {
        error = hresult_message("Query ID3D11DeviceContext4", status);
        return false;
      }

      if (cached_context->poisoned) {
        error = "PyroWave process context failed; restart the host before retrying";
        return false;
      }
      if (!cached_context->device) {
        cached_context->attempted = true;
        auto &runtime = *cached_context->runtime;
        if (!runtime.load()) {
          cached_context->poisoned = true;
          error = runtime.load_error + "; restart the host before retrying PyroWave";
          return false;
        }
        if (!check(api.pyrowave_vibepollo_configure_precision(-1, &runtime.precision), "Read wavelet precision", error)) return false;
        pyrowave_luid identity {};
        static_assert(sizeof(identity.luid) == sizeof(luid));
        std::memcpy(identity.luid, &luid, sizeof(luid));
        if (!check(api.pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, &identity, &cached_context->device), "Create Vulkan device on capture adapter", error) ||
            !check(api.pyrowave_device_set_queue_type(cached_context->device, VK_QUEUE_COMPUTE_BIT), "Select compute queue", error)) {
          return false;
        }
      }
      device = cached_context->device;
      if (!check(api.pyrowave_vibepollo_device_activate(device), "Activate Vulkan instance", error)) {
        return false;
      }

      // Separate planes avoid D3D11/Vulkan multi-planar layout disagreements
      // observed on NVIDIA. The existing converter renders straight into these
      // textures; no intermediate pixel copies or CPU readback are needed.
      for (unsigned plane = 0; plane < 2; ++plane) {
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        status = targets[plane]->QueryInterface(IID_PPV_ARGS(resource.GetAddressOf()));
        if (FAILED(status)) {
          error = hresult_message("Query shared YUV plane", status);
          return false;
        }
        handle_t shared_texture;
        status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared_texture.value);
        if (FAILED(status)) {
          error = hresult_message("Export YUV plane", status);
          return false;
        }
        VkImageCreateInfo vk {};
        vk.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        vk.imageType = VK_IMAGE_TYPE_2D;
        vk.format = profile.high_precision ?
          (plane ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16_UNORM) :
          (plane ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM);
        vk.extent = {descriptions[plane].Width, descriptions[plane].Height, 1};
        vk.mipLevels = vk.arrayLayers = 1;
        vk.samples = VK_SAMPLE_COUNT_1_BIT;
        vk.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        vk.tiling = VK_IMAGE_TILING_OPTIMAL;
        vk.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        pyrowave_image_create_info image_info {};
        image_info.device = device;
        image_info.external_handle = reinterpret_cast<pyrowave_os_handle>(shared_texture.value);
        image_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        image_info.image_create_info = &vk;
        if (!check_resource(api.pyrowave_image_create(&image_info, &images[plane]), "Import YUV plane", error)) {
          return false;
        }
        (void) shared_texture.release();  // Successful import consumes the NT handle.
      }
      for (unsigned plane = 0; plane < 3; ++plane) {
        if (!check_resource(api.pyrowave_image_get_image_view(images[plane ? 1 : 0], static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_PLANE_0_BIT << plane), VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[plane]), "Create YUV plane view", error)) {
          return false;
        }
      }

      status = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.GetAddressOf()));
      if (FAILED(status)) {
        error = hresult_message("Create shared D3D11 fence", status);
        return false;
      }
      handle_t shared_fence;
      status = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_fence.value);
      if (FAILED(status)) {
        error = hresult_message("Export D3D11 fence", status);
        return false;
      }
      pyrowave_sync_object_create_info sync_info {};
      sync_info.device = device;
      sync_info.external_handle = reinterpret_cast<pyrowave_os_handle>(shared_fence.value);
      sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
      sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
      if (!check_resource(api.pyrowave_sync_object_create(&sync_info, &sync), "Import D3D11 fence", error)) {
        return false;
      }
      (void) shared_fence.release();

      pyrowave_encoder_create_info encoder_info {};
      encoder_info.device = device;
      encoder_info.width = static_cast<int>(desc.Width);
      encoder_info.height = static_cast<int>(desc.Height);
      encoder_info.chroma = profile.chroma_444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
      if (!check_resource(api.pyrowave_encoder_create(&encoder_info, &encoder), "Create PyroWave encoder", error)) {
        return false;
      }
      const pyrowave_vibepollo_color_info color_info {
        profile.primaries_bt2020 ? 1u : 0u,
        profile.transfer_pq ? 1u : 0u,
        profile.matrix_bt2020 ? 1u : 0u,
        profile.full_range ? 0u : 1u,
        profile.chroma_left ? 1u : 0u
      };
      return check_resource(api.pyrowave_vibepollo_encoder_set_color_info(encoder, &color_info), "Set PyroWave input color metadata", error);
    }

    bool wait_for_release(std::string &error) {
      if (failed || cached_context->poisoned) {
        error = "PyroWave process context has failed; restart the host before retrying";
        return false;
      }
      if (pending_release == 0) {
        return true;
      }
      if (!check(api.pyrowave_sync_object_cpu_wait(sync, pending_release, gpu_wait_nanoseconds), "Wait for PyroWave GPU completion", error)) {
        return false;
      }
      pending_release = 0;
      return true;
    }
  };

  encoder_t::encoder_t(std::unique_ptr<impl_t> impl):
      impl {std::move(impl)} {
  }

  encoder_t::~encoder_t() = default;

  std::unique_ptr<encoder_t> encoder_t::create(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *luma_target, ID3D11Texture2D *chroma_target, const LUID &adapter_luid, const ::pyrowave::profile_t &profile, std::string &error) {
    error.clear();
    if (!device || !context || !luma_target || !chroma_target) {
      error = "Missing D3D11 input for PyroWave";
      return nullptr;
    }
    // Reject a forged/stale identity before allocating a persistent registry
    // entry. Only adapters backing a real capture D3D device are cached.
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc {};
    auto status = device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
    if (SUCCEEDED(status)) status = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (SUCCEEDED(status)) status = adapter->GetDesc(&adapter_desc);
    if (FAILED(status)) {
      error = hresult_message("Resolve capture adapter identity", status);
      return nullptr;
    }
    if (adapter_desc.AdapterLuid.HighPart != adapter_luid.HighPart || adapter_desc.AdapterLuid.LowPart != adapter_luid.LowPart) {
      error = "PyroWave adapter LUID does not match the capture D3D device";
      return nullptr;
    }
    auto impl = std::make_unique<impl_t>(adapter_luid);
    bool initialized;
    {
      // Upstream configures global Vulkan entry points. All C API operations
      // share this lock; each group activates its own cached adapter instance.
      std::lock_guard lock {impl->cached_context->mutex};
      try {
        initialized = impl->initialize(device, context, luma_target, chroma_target, adapter_luid, profile, error);
      } catch (const std::exception &failure) {
        if (impl->cached_context->attempted) impl->cached_context->poisoned = true;
        impl->failed = true;
        error = std::string("PyroWave initialization threw: ") + failure.what() + "; restart the host before retrying";
        initialized = false;
      } catch (...) {
        if (impl->cached_context->attempted) impl->cached_context->poisoned = true;
        impl->failed = true;
        error = "PyroWave initialization threw an unknown exception; restart the host before retrying";
        initialized = false;
      }
    }
    if (!initialized) {
      return nullptr;
    }
    return std::unique_ptr<encoder_t>(new encoder_t(std::move(impl)));
  }

  std::unique_ptr<encoder_t> encoder_t::create(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *luma_target, ID3D11Texture2D *chroma_target, const LUID &adapter_luid, std::string &error) {
    return create(device, context, luma_target, chroma_target, adapter_luid, ::pyrowave::profile_t {}, error);
  }

  bool encoder_t::prepare_target(std::string &error) {
    std::lock_guard lock {impl->cached_context->mutex};
    impl_t::exception_guard_t exception_guard {*impl};
    const auto start = statistics_clock_t::now();
    error.clear();
    if (!impl->check(impl->api.pyrowave_vibepollo_device_activate(impl->device), "Activate Vulkan instance", error)) return false;
    // A newer converted frame may replace a frame which was deliberately not
    // submitted (e.g. the startup dummy). There is no Vulkan ownership yet.
    impl->conversion_ready = false;
    impl->statistics = {};
    const bool ready = impl->wait_for_release(error);
    impl->statistics.interop_submit_us = elapsed_microseconds(start);
    return ready;
  }

  bool encoder_t::submit_conversion(std::string &error) {
    const auto start = statistics_clock_t::now();
    error.clear();
    if (impl->failed || impl->cached_context->poisoned || impl->pending_release != 0 || impl->timeline >= std::numeric_limits<std::uint64_t>::max() - 2) {
      error = "Invalid PyroWave conversion state";
      return false;
    }
    const auto result = impl->context->Signal(impl->fence.Get(), ++impl->timeline);
    if (FAILED(result)) {
      impl->failed = true;
      impl->cached_context->poisoned = true;
      error = hresult_message("Signal converted YUV frame", result);
      return false;
    }
    impl->context->Flush();
    impl->conversion_ready = true;
    impl->statistics.interop_submit_us += elapsed_microseconds(start);
    return true;
  }

  std::optional<packet_list_t> encoder_t::encode(std::size_t target_bytes, std::string &error) {
    std::lock_guard lock {impl->cached_context->mutex};
    impl_t::exception_guard_t exception_guard {*impl};
    const auto encode_start = statistics_clock_t::now();
    error.clear();
    target_bytes &= ~std::size_t {3};
    if (impl->failed || impl->cached_context->poisoned || !impl->conversion_ready || target_bytes < 8 || target_bytes > maximum_target_bytes) {
      error = "Invalid PyroWave frame state or byte budget";
      return std::nullopt;
    }
    if (!impl->check(impl->api.pyrowave_vibepollo_device_activate(impl->device), "Activate Vulkan instance", error)) return std::nullopt;
    impl->conversion_ready = false;
    pyrowave_gpu_external_reference external[2] {{impl->images[0], VK_QUEUE_FAMILY_EXTERNAL}, {impl->images[1], VK_QUEUE_FAMILY_EXTERNAL}};
    const auto semaphore = impl->api.pyrowave_sync_object_get_semaphore(impl->sync);
    pyrowave_gpu_sync_operation acquire {external, 2, {semaphore, impl->timeline}};
    pyrowave_gpu_sync_operation release {external, 2, {semaphore, ++impl->timeline}};
    pyrowave_rate_control rate {target_bytes};
    if (!impl->check(impl->api.pyrowave_encoder_encode_gpu_synchronous(impl->encoder, &acquire, &release, &impl->buffers, &rate), "Encode YCbCr frame", error)) {
      return std::nullopt;
    }
    impl->pending_release = release.sync.value;
    if (!impl->wait_for_release(error)) {
      return std::nullopt;
    }

    impl->statistics.encode_wait_us = elapsed_microseconds(encode_start);
    const auto packetize_start = statistics_clock_t::now();

    const void *raw = nullptr, *metadata = nullptr;
    std::size_t raw_size = 0, metadata_size = 0;
    if (!impl->check(impl->api.pyrowave_encoder_get_mapped_raw_bitstream(impl->encoder, &raw, &raw_size, &metadata, &metadata_size), "Read bitstream allocation size", error)) {
      return std::nullopt;
    }
    // Upstream only asserts packetizer buffer bounds. Reserve the full raw
    // allocation plus its serialized 8-byte sequence header, not just the RDO
    // target. The mapped bytes remain opaque to this adapter.
    if (!raw || !metadata || raw_size < target_bytes || raw_size > maximum_raw_bytes - 8 || metadata_size > maximum_raw_bytes) {
      impl->failed = true;
      impl->cached_context->poisoned = true;
      error = "PyroWave returned an invalid bitstream allocation";
      return std::nullopt;
    }
    std::size_t packet_count = 0;
    if (!impl->check(impl->api.pyrowave_encoder_compute_num_packets(impl->encoder, native_packet_boundary, &packet_count), "Count native packets", error)) {
      return std::nullopt;
    }
    if (packet_count == 0 || packet_count > maximum_native_packets) {
      impl->failed = true;
      impl->cached_context->poisoned = true;
      error = "PyroWave returned an invalid native packet count";
      return std::nullopt;
    }

    try {
      std::vector<pyrowave_packet> packets(packet_count);
      std::vector<std::uint8_t> packed(raw_size + 8);
      std::size_t written_packets = 0;
      if (!impl->check(impl->api.pyrowave_encoder_packetize(impl->encoder, packets.data(), native_packet_boundary, &written_packets, packed.data(), packed.size()), "Packetize frame", error)) {
        return std::nullopt;
      }
      if (written_packets != packet_count) {
        impl->failed = true;
        impl->cached_context->poisoned = true;
        error = "PyroWave packet count changed during packetization";
        return std::nullopt;
      }
      std::size_t total = 0;
      for (const auto &packet : packets) {
        if (packet.offset != total || packet.size == 0 || packet.offset > packed.size() || packet.size > packed.size() - packet.offset) {
          impl->failed = true;
          impl->cached_context->poisoned = true;
          error = "PyroWave returned invalid native packet boundaries; restart the host before retrying";
          return std::nullopt;
        }
        if (total > target_bytes || packet.size > target_bytes - total) {
          error = "PyroWave frame exceeds its byte budget";
          return std::nullopt;
        }
        total += packet.size;
      }
      for (int bands = 0; bands <= 4; ++bands) {
        auto &critical_count = impl->statistics.critical_packets[bands];
        if (!impl->check(impl->api.pyrowave_encoder_compute_num_critical_packets(impl->encoder, bands, native_packet_boundary, 0, &critical_count), "Count critical native packets", error)) {
          return std::nullopt;
        }
        if (critical_count > packet_count) {
          impl->failed = true;
          impl->cached_context->poisoned = true;
          error = "PyroWave returned an invalid critical packet count";
          return std::nullopt;
        }
      }
      auto &active_count = impl->statistics.active_block_count;
      const int bands = impl->statistics.active_block_bands;
      if (!impl->check(impl->api.pyrowave_encoder_get_num_active_blocks(impl->encoder, bands, &active_count), "Count sideband blocks", error)) {
        return std::nullopt;
      }
      if (active_count > maximum_sideband_words * 32) {
        impl->failed = true;
        impl->cached_context->poisoned = true;
        error = "PyroWave returned an invalid sideband block count";
        return std::nullopt;
      }
      auto &active_words = impl->statistics.active_block_words;
      active_words.resize((active_count + 31) / 32);
      if (!impl->check(impl->api.pyrowave_encoder_compute_block_active_words(impl->encoder, bands, active_words.data(), active_words.size()), "Compute active-block sideband", error)) {
        return std::nullopt;
      }
      packet_list_t result;
      result.reserve(packet_count);
      for (const auto &packet : packets) {
        result.emplace_back(packed.data() + packet.offset, packed.data() + packet.offset + packet.size);
      }
      impl->statistics.packetize_us = elapsed_microseconds(packetize_start);
      impl->statistics.native_bytes = total;
      impl->statistics.native_packets = packet_count;
      return result;
    } catch (const std::bad_alloc &) {
      impl->failed = true;
      impl->cached_context->poisoned = true;
      error = "Cannot allocate PyroWave packet buffers; restart the host before retrying";
      return std::nullopt;
    }
  }

  frame_statistics_t encoder_t::last_frame_statistics() const {
    return impl->statistics;
  }

  std::vector<std::string> encoder_t::performance_statistics(bool reset) const {
    std::lock_guard lock {impl->cached_context->mutex};
    std::vector<std::string> messages;
    if (impl->failed || impl->cached_context->poisoned) return messages;
    if (impl->api.pyrowave_vibepollo_device_activate(impl->device) != PYROWAVE_SUCCESS) return messages;
    // Never propagate C++ exceptions across the C callback boundary.
    const auto append = [](void *userdata, const char *message) {
      auto &output = *static_cast<std::vector<std::string> *>(userdata);
      try {
        if (message && output.size() < 128) output.emplace_back(message);
      } catch (...) {
        // Diagnostics are best effort and must not terminate a healthy stream.
      }
    };
    impl->api.pyrowave_device_report_performance_stats(impl->device, append, &messages, reset);
    return messages;
  }
}  // namespace platf::pyrowave

#endif  // SUNSHINE_ENABLE_PYROWAVE
