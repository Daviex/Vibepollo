/**
 * @file src/platform/windows/pyrowave_runtime.cpp
 * @brief PyroWave C API adapter with a GPU-only D3D11 input path.
 */
#include "pyrowave_runtime.h"

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
      PYROWAVE_ENTRY(pyrowave_create_device_by_compat);
      PYROWAVE_ENTRY(pyrowave_device_set_queue_type);
      PYROWAVE_ENTRY(pyrowave_device_destroy);
      PYROWAVE_ENTRY(pyrowave_image_create);
      PYROWAVE_ENTRY(pyrowave_image_get_image_view);
      PYROWAVE_ENTRY(pyrowave_image_destroy);
      PYROWAVE_ENTRY(pyrowave_sync_object_create);
      PYROWAVE_ENTRY(pyrowave_sync_object_get_semaphore);
      PYROWAVE_ENTRY(pyrowave_sync_object_cpu_wait);
      PYROWAVE_ENTRY(pyrowave_sync_object_destroy);
      PYROWAVE_ENTRY(pyrowave_encoder_create);
      PYROWAVE_ENTRY(pyrowave_encoder_encode_gpu_synchronous);
      PYROWAVE_ENTRY(pyrowave_encoder_get_mapped_raw_bitstream);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_packets);
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
        constexpr const char *expected_contract = "d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1";
        const char *actual_contract = runtime_contract ? runtime_contract() : nullptr;
        if (!actual_contract || std::strcmp(actual_contract, expected_contract) != 0) {
          error = "PyroWave runtime contract is missing or incompatible; install the bundled pinned runtime";
          return false;
        }
        PYROWAVE_LOAD(pyrowave_create_device_by_compat);
        PYROWAVE_LOAD(pyrowave_device_set_queue_type);
        PYROWAVE_LOAD(pyrowave_device_destroy);
        PYROWAVE_LOAD(pyrowave_image_create);
        PYROWAVE_LOAD(pyrowave_image_get_image_view);
        PYROWAVE_LOAD(pyrowave_image_destroy);
        PYROWAVE_LOAD(pyrowave_sync_object_create);
        PYROWAVE_LOAD(pyrowave_sync_object_get_semaphore);
        PYROWAVE_LOAD(pyrowave_sync_object_cpu_wait);
        PYROWAVE_LOAD(pyrowave_sync_object_destroy);
        PYROWAVE_LOAD(pyrowave_encoder_create);
        PYROWAVE_LOAD(pyrowave_encoder_encode_gpu_synchronous);
        PYROWAVE_LOAD(pyrowave_encoder_get_mapped_raw_bitstream);
        PYROWAVE_LOAD(pyrowave_encoder_compute_num_packets);
        PYROWAVE_LOAD(pyrowave_encoder_packetize);
        PYROWAVE_LOAD(pyrowave_encoder_destroy);
#undef PYROWAVE_LOAD
        return true;
      }
    };

    struct process_context_t {
      // Member order keeps the DLL loaded through device destruction. The
      // shared owner also keeps this mutex alive if a session outlives the
      // process cache's static reference during shutdown.
      std::mutex mutex;
      api_t api;
      pyrowave_device device = nullptr;
      LUID luid {};
      bool attempted = false;
      bool active_encoder = false;
      std::atomic_bool poisoned {false};

      ~process_context_t() {
        std::lock_guard lock {mutex};
        if (device) api.pyrowave_device_destroy(device);
      }
    };

    std::shared_ptr<process_context_t> process_context() {
      // Exactly one adapter/context per process. Repeated Vulkan device
      // creation leaks handles even in the bare Vulkan control on this host;
      // retain this bounded context across probes and capture reinitialization.
      static const auto context = std::make_shared<process_context_t>();
      return context;
    }
  }  // namespace

  struct encoder_t::impl_t {
    std::shared_ptr<process_context_t> cached_context = process_context();
    api_t &api = cached_context->api;
    bool context_lease = false;
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
      if (context_lease) cached_context->active_encoder = false;
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

    bool initialize(ID3D11Device *d3d_device, ID3D11DeviceContext *d3d_context, ID3D11Texture2D *luma, ID3D11Texture2D *chroma, const LUID &luid, std::string &error) {
      std::array<D3D11_TEXTURE2D_DESC, 2> descriptions {};
      targets = {luma, chroma};
      for (unsigned plane = 0; plane < 2; ++plane) {
        auto &desc = descriptions[plane];
        targets[plane]->GetDesc(&desc);
        if (desc.Format != (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM) || !desc.Width || !desc.Height ||
            desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
          error = "PyroWave requires shared R8 luma and R8G8 chroma textures";
          return false;
        }
      }
      const auto &desc = descriptions[0];
      if ((desc.Width & 1) || (desc.Height & 1) || desc.Width > 8192 || desc.Height > 8192 ||
          descriptions[1].Width != desc.Width / 2 || descriptions[1].Height != desc.Height / 2) {
        error = "PyroWave requires even luma dimensions up to 8192 and half-size chroma";
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
      if (cached_context->attempted && (cached_context->luid.HighPart != luid.HighPart || cached_context->luid.LowPart != luid.LowPart)) {
        error = "PyroWave is bound to a different adapter LUID; restart the host to change GPU";
        return false;
      }
      if (cached_context->active_encoder) {
        error = "PyroWave process context already has an active encoder";
        return false;
      }
      if (!cached_context->device) {
        cached_context->attempted = true;
        cached_context->luid = luid;
        if (!api.load(error)) {
          cached_context->poisoned = true;
          error += "; restart the host before retrying PyroWave";
          return false;
        }
        pyrowave_luid identity {};
        static_assert(sizeof(identity.luid) == sizeof(luid));
        std::memcpy(identity.luid, &luid, sizeof(luid));
        if (!check(api.pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, &identity, &cached_context->device), "Create Vulkan device on capture adapter", error) ||
            !check(api.pyrowave_device_set_queue_type(cached_context->device, VK_QUEUE_COMPUTE_BIT), "Select compute queue", error)) {
          return false;
        }
      }
      device = cached_context->device;
      cached_context->active_encoder = true;
      context_lease = true;

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
        vk.format = plane ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM;
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
        if (!check(api.pyrowave_image_create(&image_info, &images[plane]), "Import YUV plane", error)) {
          return false;
        }
        (void) shared_texture.release();  // Successful import consumes the NT handle.
      }
      for (unsigned plane = 0; plane < 3; ++plane) {
        if (!check(api.pyrowave_image_get_image_view(images[plane ? 1 : 0], static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_PLANE_0_BIT << plane), VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[plane]), "Create YUV plane view", error)) {
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
      if (!check(api.pyrowave_sync_object_create(&sync_info, &sync), "Import D3D11 fence", error)) {
        return false;
      }
      (void) shared_fence.release();

      pyrowave_encoder_create_info encoder_info {};
      encoder_info.device = device;
      encoder_info.width = static_cast<int>(desc.Width);
      encoder_info.height = static_cast<int>(desc.Height);
      encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
      return check(api.pyrowave_encoder_create(&encoder_info, &encoder), "Create PyroWave encoder", error);
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

  std::unique_ptr<encoder_t> encoder_t::create(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *luma_target, ID3D11Texture2D *chroma_target, const LUID &adapter_luid, std::string &error) {
    error.clear();
    if (!device || !context || !luma_target || !chroma_target) {
      error = "Missing D3D11 input for PyroWave";
      return nullptr;
    }
    auto impl = std::make_unique<impl_t>();
    bool initialized;
    {
      // Upstream configures global Vulkan entry points: one cached context and
      // serialized resource creation/destruction prevent overlapping mutation.
      std::lock_guard lock {impl->cached_context->mutex};
      try {
        initialized = impl->initialize(device, context, luma_target, chroma_target, adapter_luid, error);
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

  bool encoder_t::prepare_target(std::string &error) {
    impl_t::exception_guard_t exception_guard {*impl};
    const auto start = statistics_clock_t::now();
    error.clear();
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
    if (impl->failed || impl->pending_release != 0 || impl->timeline >= std::numeric_limits<std::uint64_t>::max() - 2) {
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
    impl_t::exception_guard_t exception_guard {*impl};
    const auto encode_start = statistics_clock_t::now();
    error.clear();
    target_bytes &= ~std::size_t {3};
    if (impl->failed || !impl->conversion_ready || target_bytes < 8 || target_bytes > maximum_target_bytes) {
      error = "Invalid PyroWave frame state or byte budget";
      return std::nullopt;
    }
    impl->conversion_ready = false;
    pyrowave_gpu_external_reference external[2] {{impl->images[0], VK_QUEUE_FAMILY_EXTERNAL}, {impl->images[1], VK_QUEUE_FAMILY_EXTERNAL}};
    const auto semaphore = impl->api.pyrowave_sync_object_get_semaphore(impl->sync);
    pyrowave_gpu_sync_operation acquire {external, 2, {semaphore, impl->timeline}};
    pyrowave_gpu_sync_operation release {external, 2, {semaphore, ++impl->timeline}};
    pyrowave_rate_control rate {target_bytes};
    if (!impl->check(impl->api.pyrowave_encoder_encode_gpu_synchronous(impl->encoder, &acquire, &release, &impl->buffers, &rate), "Encode NV12 frame", error)) {
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
}  // namespace platf::pyrowave

#endif  // SUNSHINE_ENABLE_PYROWAVE
