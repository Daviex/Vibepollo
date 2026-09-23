"""Compile the patched C API functions against deterministic failure adapters.

No Vulkan device, driver, real NT handle or application is opened. Function and
handle-close policy bodies are extracted from the supplied patched dependency.
This checks ownership commits, not real driver allocation or Granite teardown.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile


def body(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--headers', type=Path, help='Vulkan-Headers include directory, defaults to the source dependency')
parser.add_argument('--compiler', default='C:/msys64/ucrt64/bin/g++.exe')
args = parser.parse_args()
source = args.source.resolve()
headers = (args.headers or source / 'Granite/third_party/khronos/vulkan-headers/include').resolve()
c_api = (source / 'pyrowave_c.cpp').read_text(encoding='utf-8')
common = (source / 'Granite/vulkan/vulkan_common.hpp').read_text(encoding='utf-8')
allocator = (source / 'Granite/vulkan/memory_allocator.cpp').read_text(encoding='utf-8')
semaphore = (source / 'Granite/vulkan/semaphore.cpp').read_text(encoding='utf-8')
exports = (source / 'pyrowave-shared.def').read_text(encoding='utf-8').splitlines()
if sum(line.strip() == 'pyrowave_vibepollo_runtime_contract' for line in exports) != 1:
    raise SystemExit('Runtime-contract getter must appear exactly once in the Windows export list')
if 'consume_win32_handle_on_import = false' not in c_api:
    raise SystemExit('Expected the patched dependency source')

external = body(common, 'struct ExternalHandle\n') + ';'
image_create = body(c_api, 'pyrowave_result pyrowave_image_create(')
sync_create = body(c_api, 'pyrowave_result\npyrowave_sync_object_create(')
runtime_contract = body(c_api, 'const char *pyrowave_vibepollo_runtime_contract(void)')
memory_close = body(allocator, 'if (external && bool(*external) &&')
semaphore_close = body(semaphore, 'if (ExternalHandle::semaphore_handle_type_imports_by_reference(import.handleType))')

preamble = r'''
#include <vulkan/vulkan_core.h>
#include "pyrowave.h"
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#define VK_ASSERT assert
static int cookie, closes, imports, wrappers;
static bool open_handle, fail_wrapper;
static std::function<bool()> published;
static int CloseHandle(void *handle) {
  assert(handle == &cookie && open_handle);
  assert(published && published());
  open_handle = false;
  ++closes;
  return 1;
}
static int null_logger;
namespace Util { static void set_thread_logging_interface(int *) {} }
enum class ImageDomain { Physical };
enum class ImageLayout { General };
enum { IMAGE_MISC_EXTERNAL_MEMORY_BIT = 1, IMAGE_MISC_NO_DEFAULT_VIEWS_BIT = 2 };
'''

adapters = r'''
enum class Failure { none, allocation, binding, image_pool, exception, semaphore_create, semaphore_import };
static Failure failure;
struct ImageCreateInfo {
  ImageDomain domain;
  unsigned misc;
  ExternalHandle external;
  void *pnext;
  ImageLayout layout;
  VkImageLayout initial_layout;
  VkImageType type;
  VkFormat format;
  VkImageCreateFlags flags;
  unsigned width, height, depth, layers, levels;
  VkSampleCountFlagBits samples;
  VkImageUsageFlags usage;
};
struct Features {
  bool supports_external = true;
  VkDriverId driver_id = VK_DRIVER_ID_MESA_LLVMPIPE;
  struct { bool videoMaintenance1 = false; } video_maintenance1_features;
};
static void memory_import(ExternalHandle value) {
  auto *external = &value;
  ++imports;
  MEMORY_CLOSE
}
struct SemaphoreObject {
  bool import_from_handle(ExternalHandle handle) {
    ++imports;
    if (failure == Failure::semaphore_import) return false;
    struct { VkExternalSemaphoreHandleTypeFlagBits handleType; } import {handle.semaphore_handle_type};
    SEMAPHORE_CLOSE
    return true;
  }
};
using Semaphore = std::shared_ptr<SemaphoreObject>;
using ImageHandle = std::shared_ptr<int>;
struct Device {
  Features features;
  Features &get_device_features() { return features; }
  void get_format_properties(VkFormat, VkFormatProperties3 *) {}
  ImageHandle create_image(const ImageCreateInfo &info) {
    assert(!info.external.consume_win32_handle_on_import);
    memory_import(info.external);
    if (failure == Failure::exception) throw std::bad_alloc();
    if (failure == Failure::allocation || failure == Failure::binding || failure == Failure::image_pool) return {};
    return std::make_shared<int>(1);
  }
  Semaphore request_semaphore_external(VkSemaphoreType, VkExternalSemaphoreHandleTypeFlagBits) {
    if (failure == Failure::semaphore_create) return {};
    return std::make_shared<SemaphoreObject>();
  }
};
struct WrapperAllocation {
  WrapperAllocation() { ++wrappers; }
  ~WrapperAllocation() { --wrappers; }
  static void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    return fail_wrapper ? nullptr : std::malloc(size);
  }
  static void operator delete(void *value) noexcept { std::free(value); }
  static void operator delete(void *value, const std::nothrow_t &) noexcept { std::free(value); }
};
struct pyrowave_device_opaque { Device device; };
struct pyrowave_image_opaque : WrapperAllocation { Device *device = nullptr; ImageHandle img; };
struct pyrowave_sync_object_opaque : WrapperAllocation { Device *device = nullptr; Semaphore semaphore; };
'''.replace('MEMORY_CLOSE', memory_close).replace('SEMAPHORE_CLOSE', semaphore_close)

tests = r'''
static void reset() {
  assert(wrappers == 0);
  open_handle = true;
  closes = imports = 0;
  fail_wrapper = false;
  failure = Failure::none;
  published = {};
}
int main() {
  assert(std::string_view(pyrowave_vibepollo_runtime_contract()) == "d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1");
  pyrowave_device_opaque device;
  VkImageCreateInfo vk {};
  vk.imageType = VK_IMAGE_TYPE_2D;
  vk.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vk.tiling = VK_IMAGE_TILING_OPTIMAL;
  pyrowave_image_create_info image_info {};
  image_info.device = &device;
  image_info.external_handle = reinterpret_cast<pyrowave_os_handle>(&cookie);
  image_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
  image_info.image_create_info = &vk;
  pyrowave_sync_object_create_info sync_info {};
  sync_info.device = &device;
  sync_info.external_handle = reinterpret_cast<pyrowave_os_handle>(&cookie);
  sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
  sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
  int cases = 0;
  for (auto stage : {Failure::allocation, Failure::binding, Failure::image_pool, Failure::exception}) {
    reset(); failure = stage;
    pyrowave_image out = nullptr;
    try {
      assert(pyrowave_image_create(&image_info, &out) == PYROWAVE_ERROR_FAILED_EXTERNAL_HANDLE);
      assert(stage != Failure::exception);
    } catch (const std::bad_alloc &) { assert(stage == Failure::exception); }
    assert(out == nullptr && open_handle && closes == 0 && imports == 1 && wrappers == 0);
    ++cases;
  }
  for (bool image : {false, true}) {
    reset(); fail_wrapper = true;
    pyrowave_image image_out = nullptr;
    pyrowave_sync_object sync_out = nullptr;
    const auto status = image ? pyrowave_image_create(&image_info, &image_out) : pyrowave_sync_object_create(&sync_info, &sync_out);
    assert(status == PYROWAVE_ERROR_OUT_OF_HOST_MEMORY && imports == 0 && closes == 0 && open_handle && wrappers == 0);
    ++cases;
  }
  for (auto stage : {Failure::semaphore_create, Failure::semaphore_import}) {
    reset(); failure = stage;
    pyrowave_sync_object out = nullptr;
    const auto status = pyrowave_sync_object_create(&sync_info, &out);
    assert(status == (stage == Failure::semaphore_create ? PYROWAVE_ERROR_UNSUPPORTED_EXTERNAL_HANDLE : PYROWAVE_ERROR_FAILED_EXTERNAL_HANDLE));
    assert(out == nullptr && open_handle && closes == 0 && wrappers == 0);
    ++cases;
  }
  reset();
  pyrowave_image image_out = nullptr;
  published = [&] { return image_out != nullptr; };
  assert(pyrowave_image_create(&image_info, &image_out) == PYROWAVE_SUCCESS);
  assert(!open_handle && closes == 1 && wrappers == 1);
  delete image_out; ++cases;
  reset();
  pyrowave_sync_object sync_out = nullptr;
  published = [&] { return sync_out != nullptr; };
  assert(pyrowave_sync_object_create(&sync_info, &sync_out) == PYROWAVE_SUCCESS);
  assert(!open_handle && closes == 1 && wrappers == 1);
  delete sync_out; ++cases;
  reset();
  image_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
  image_out = nullptr;
  assert(pyrowave_image_create(&image_info, &image_out) == PYROWAVE_SUCCESS);
  assert(open_handle && closes == 0 && wrappers == 1);
  delete image_out; ++cases;
  reset();
  sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
  sync_out = nullptr;
  assert(pyrowave_sync_object_create(&sync_info, &sync_out) == PYROWAVE_SUCCESS);
  assert(open_handle && closes == 0 && wrappers == 1);
  delete sync_out; ++cases;
  reset();
  assert(pyrowave_image_create(nullptr, &image_out) == PYROWAVE_ERROR_INVALID_ARGUMENT);
  assert(pyrowave_image_create(&image_info, nullptr) == PYROWAVE_ERROR_INVALID_ARGUMENT);
  assert(pyrowave_sync_object_create(nullptr, &sync_out) == PYROWAVE_ERROR_INVALID_ARGUMENT);
  assert(pyrowave_sync_object_create(&sync_info, nullptr) == PYROWAVE_ERROR_INVALID_ARGUMENT);
  assert(imports == 0 && closes == 0 && open_handle && wrappers == 0);
  ++cases;
  std::cout << "PASS " << cases << " deterministic ownership cases and exact runtime contract; extracted C API and close guards, no Vulkan driver\n";
}
'''

with tempfile.TemporaryDirectory(prefix='pyrowave-nt-ownership-') as directory:
    directory = Path(directory)
    cpp = directory / 'ownership.cpp'
    executable = directory / 'ownership.exe'
    cpp.write_text(preamble + external + adapters + '\n' + runtime_contract + '\n' + image_create + '\n' + sync_create + tests, encoding='utf-8')
    subprocess.run([args.compiler, '-std=c++17', '-O0', '-g', '-I', str(source), '-I', str(headers), str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=15)
