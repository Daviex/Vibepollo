// Isolate C API device/decoder lifetimes from capture, conversion and imports.
#include "reference_codec.h"
#include <psapi.h>
#include <condition_variable>
#include <thread>

namespace {
  void bare_vulkan(const LUID &luid, bool with_device) {
    HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) throw std::runtime_error("Cannot load system Vulkan for control");
    auto proc = std::bit_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(proc(nullptr, "vkCreateInstance"));
    VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = VK_API_VERSION_1_2;
    application.pApplicationName = "pyrowave-lifecycle-control";
    VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) throw std::runtime_error("Bare Vulkan instance failed");
    if (with_device) {
      auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proc(instance, "vkEnumeratePhysicalDevices"));
      auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(proc(instance, "vkGetPhysicalDeviceProperties2"));
      auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
      auto create_device = reinterpret_cast<PFN_vkCreateDevice>(proc(instance, "vkCreateDevice"));
      auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(proc(instance, "vkDestroyDevice"));
      std::uint32_t count = 0;
      enumerate(instance, &count, nullptr);
      std::vector<VkPhysicalDevice> physical(count);
      enumerate(instance, &count, physical.data());
      VkPhysicalDevice selected = VK_NULL_HANDLE;
      for (auto candidate : physical) {
        VkPhysicalDeviceIDProperties ids {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &ids;
        properties(candidate, &props);
        if (ids.deviceLUIDValid && std::memcmp(ids.deviceLUID, &luid, sizeof(luid)) == 0) selected = candidate;
      }
      if (!selected) throw std::runtime_error("Bare Vulkan matching GPU missing");
      queues(selected, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      queues(selected, &count, families.data());
      std::uint32_t family = 0;
      for (; family < count; ++family) if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
      if (family == count) throw std::runtime_error("Bare Vulkan graphics queue missing");
      const float priority = 1;
      VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue_info.queueFamilyIndex = family;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &priority;
      VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      device_info.queueCreateInfoCount = 1;
      device_info.pQueueCreateInfos = &queue_info;
      VkDevice device = VK_NULL_HANDLE;
      if (create_device(selected, &device_info, nullptr, &device) != VK_SUCCESS) throw std::runtime_error("Bare Vulkan device failed");
      destroy_device(device, nullptr);
    }
    auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(proc(instance, "vkDestroyInstance"));
    destroy_instance(instance, nullptr);
    FreeLibrary(loader);
  }

  DWORD resources(int cycle) {
    DWORD handles = 0;
    PROCESS_MEMORY_COUNTERS_EX memory {};
    memory.cb = sizeof(memory);
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles) || !GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) throw std::runtime_error("Resource inspection failed");
    std::cout << "resources_cycle=" << cycle << " handles=" << handles << " private_bytes=" << memory.PrivateUsage << std::endl;
    return handles;
  }
}
int main(int argc, char **argv) {
  using namespace pyrowave_smoke;
  std::mutex watchdog_mutex;
  std::condition_variable_any watchdog_condition;
  std::jthread watchdog([&](std::stop_token stop) {
    std::unique_lock lock(watchdog_mutex);
    watchdog_condition.wait_for(lock, stop, std::chrono::seconds(30), [] { return false; });
    if (!stop.stop_requested()) TerminateProcess(GetCurrentProcess(), 124);
  });
  try {
    const std::string mode = argc > 1 ? argv[1] : "device-held";
    if (argc > 2 || (mode != "device-held" && mode != "device-unload" && mode != "decoder-held" && mode != "decoder-unload" && mode != "decoder-reconfigure" && mode != "vulkan-instance" && mode != "vulkan-device" && mode != "runtime" && mode != "runtime-new-targets" && mode != "runtime-new-device" && mode != "d3d-device" && mode != "runtime-poison")) throw std::runtime_error("Expected device-held/device-unload/decoder-held/decoder-unload/decoder-reconfigure/vulkan-instance/vulkan-device/runtime/runtime-new-targets/runtime-new-device/d3d-device/runtime-poison");
    com_ptr<IDXGIFactory1> factory;
    com_ptr<IDXGIAdapter1> adapter;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "Create lifecycle DXGI factory");
    checked(factory->EnumAdapters1(0, adapter.GetAddressOf()), "Select lifecycle adapter");
    DXGI_ADAPTER_DESC1 desc {};
    checked(adapter->GetDesc1(&desc), "Read lifecycle adapter");
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID3D11DeviceContext> d3d_context;
    com_ptr<ID3D11Texture2D> luma, chroma;
    if (mode.starts_with("runtime")) {
      const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr, d3d_context.GetAddressOf()), "Create lifecycle D3D device");
      D3D11_TEXTURE2D_DESC info {};
      info.Width = info.Height = 64;
      info.MipLevels = info.ArraySize = info.SampleDesc.Count = 1;
      info.Format = DXGI_FORMAT_R8_UNORM;
      info.Usage = D3D11_USAGE_DEFAULT;
      info.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      info.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
      checked(d3d->CreateTexture2D(&info, nullptr, luma.GetAddressOf()), "Create lifecycle luma");
      info.Width = info.Height = 32;
      info.Format = DXGI_FORMAT_R8G8_UNORM;
      checked(d3d->CreateTexture2D(&info, nullptr, chroma.GetAddressOf()), "Create lifecycle chroma");
    }
    std::array<wchar_t, 32768> executable {};
    auto length = GetModuleFileNameW(nullptr, executable.data(), executable.size());
    if (!length || length >= executable.size()) throw std::runtime_error("Executable path unavailable");
    const auto dll = std::filesystem::path(std::wstring(executable.data(), length)).parent_path() / L"libpyrowave-shared-0.dll";
    HMODULE held = nullptr;
    if (mode.ends_with("-held")) {
      held = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!held) throw std::runtime_error("Cannot hold pinned runtime module");
    }
    std::cout << "mode=" << mode << std::endl;
    if (mode == "runtime-poison") {
      std::string failure;
      if (platf::pyrowave::encoder_t::create(d3d.Get(), d3d_context.Get(), luma.Get(), chroma.Get(), LUID {0xffffffff, -1}, failure)) throw std::runtime_error("Invalid first adapter was accepted");
      std::cout << "first_failure=" << failure << std::endl;
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (platf::pyrowave::encoder_t::create(d3d.Get(), d3d_context.Get(), luma.Get(), chroma.Get(), desc.AdapterLuid, failure) || failure.find("process context failed") == std::string::npos) throw std::runtime_error("Failed context was retried instead of remaining poisoned");
      }
      std::cout << "PASS failed_context_cached_and_restart_required" << std::endl;
      return 0;
    }
    std::vector<DWORD> handle_samples;
    reference_decoder_t retained_reference;
    platf::pyrowave::packet_list_t reference_packets;
    std::vector<std::uint8_t> reference_pixels;
    pyrowave_cpu_buffer reference_output {};
    if (mode == "decoder-reconfigure") {
      reference_pixels.resize(1280 * 720 * 3 / 2, 128);
      retained_reference.initialize(desc.AdapterLuid, 1280, 720);
      reference_packets = retained_reference.encode_cpu(reference_pixels, 1280, 720, 65536);
      reference_output.width = 1280;
      reference_output.height = 720;
      reference_output.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
      std::size_t offset = 0;
      for (unsigned plane = 0; plane < 3; ++plane) {
        const unsigned scale = plane ? 2 : 1;
        reference_output.data[plane] = reference_pixels.data() + offset;
        reference_output.row_stride_in_bytes[plane] = 1280 / scale;
        reference_output.plane_size_in_bytes[plane] = 1280 * 720 / (scale * scale);
        offset += reference_output.plane_size_in_bytes[plane];
      }
    }
    for (int cycle = 0; cycle <= 12; ++cycle) {
      if (mode == "runtime" || mode == "runtime-new-targets" || mode == "runtime-new-device") {
        if (mode != "runtime") {
          D3D11_TEXTURE2D_DESC info {};
          luma->GetDesc(&info);
          luma.Reset();
          chroma.Reset();
          if (mode == "runtime-new-device") {
            d3d_context.Reset();
            d3d.Reset();
            const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr, d3d_context.GetAddressOf()), "Recreate lifecycle D3D device");
          }
          checked(d3d->CreateTexture2D(&info, nullptr, luma.GetAddressOf()), "Recreate lifecycle luma");
          info.Width = info.Height = 32;
          info.Format = DXGI_FORMAT_R8G8_UNORM;
          checked(d3d->CreateTexture2D(&info, nullptr, chroma.GetAddressOf()), "Recreate lifecycle chroma");
        }
        std::string failure;
        auto encoder = platf::pyrowave::encoder_t::create(d3d.Get(), d3d_context.Get(), luma.Get(), chroma.Get(), desc.AdapterLuid, failure);
        if (!encoder) throw std::runtime_error(failure);
        std::vector<std::uint8_t> y(64 * 64, 64), uv(32 * 32 * 2, 128);
        if (!encoder->prepare_target(failure)) throw std::runtime_error(failure);
        d3d_context->UpdateSubresource(luma.Get(), 0, nullptr, y.data(), 64, 0);
        d3d_context->UpdateSubresource(chroma.Get(), 0, nullptr, uv.data(), 64, 0);
        if (!encoder->submit_conversion(failure) || !encoder->encode(65536, failure)) throw std::runtime_error(failure);
      } else if (mode == "decoder-reconfigure") {
        retained_reference.configure_decoder(1280, 720);
        retained_reference.decode(reference_packets, reference_output);
      } else if (mode == "d3d-device") {
        com_ptr<ID3D11Device> control_device;
        com_ptr<ID3D11DeviceContext> control_context;
        const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, control_device.GetAddressOf(), nullptr, control_context.GetAddressOf()), "Create bare D3D device");
      } else if (mode.starts_with("vulkan-")) {
        bare_vulkan(desc.AdapterLuid, mode == "vulkan-device");
      } else if (mode.starts_with("decoder-")) {
        reference_decoder_t reference;
        reference.initialize(desc.AdapterLuid, 64, 64);
      } else {
        HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("Cannot load pinned runtime module");
        auto create = std::bit_cast<decltype(&pyrowave_create_device_by_compat)>(GetProcAddress(module, "pyrowave_create_device_by_compat"));
        auto destroy = std::bit_cast<decltype(&pyrowave_device_destroy)>(GetProcAddress(module, "pyrowave_device_destroy"));
        if (!create || !destroy) throw std::runtime_error("Missing lifecycle entry point");
        pyrowave_luid identity {};
        std::memcpy(identity.luid, &desc.AdapterLuid, sizeof(desc.AdapterLuid));
        pyrowave_device device = nullptr;
        checked(create(0, 0, nullptr, nullptr, &identity, &device), "Create bare C API device");
        destroy(device);
        FreeLibrary(module);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      handle_samples.push_back(resources(cycle));
    }
    if (held) FreeLibrary(held);
    if (mode == "runtime" || mode == "runtime-new-targets" || mode == "runtime-new-device") {
      const auto [low, high] = std::minmax_element(handle_samples.end() - 6, handle_samples.end());
      const auto growth = static_cast<std::int64_t>(handle_samples.back()) - handle_samples.front();
      if (*high - *low > 2 || growth > 4) throw std::runtime_error("Runtime handles did not stabilize after warmup");
      std::cout << "PASS runtime_handle_plateau growth=" << growth << " tail_range=" << *high - *low << std::endl;
    }
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << "FAIL: " << failure.what() << std::endl;
    return 1;
  }
}
