#include "pyrowave_cpu_runtime.h"
#include "src/pyrowave_runtime_contract.h"

#if defined(SUNSHINE_ENABLE_PYROWAVE) && defined(__linux__)

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include <dlfcn.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>

namespace platf::pyrowave_cpu {
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

    std::string api_message(const char *operation, pyrowave_result result) {
      return std::string(operation) + " failed (PyroWave " + std::to_string(static_cast<int>(result)) + ')';
    }

    std::string format_uuid(const std::array<std::uint8_t, 16> &uuid) {
      constexpr char hex[] = "0123456789abcdef";
      std::string text;
      text.reserve(32);
      for (auto byte : uuid) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
      }
      return text;
    }

    std::optional<std::array<std::uint8_t, 16>> parse_uuid(std::string_view text) {
      if (text.size() != 32 && text.size() != 36) return std::nullopt;
      std::array<std::uint8_t, 16> uuid {};
      const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      std::size_t source = 0;
      for (std::size_t byte = 0; byte < uuid.size(); ++byte) {
        if (text.size() == 36 && (byte == 4 || byte == 6 || byte == 8 || byte == 10)) {
          if (text[source++] != '-') return std::nullopt;
        }
        const int high = digit(text[source++]);
        const int low = digit(text[source++]);
        if (high < 0 || low < 0) return std::nullopt;
        uuid[byte] = static_cast<std::uint8_t>((high << 4) | low);
      }
      return uuid;
    }

    struct module_t {
      void *value = nullptr;
      ~module_t() {
        if (value) dlclose(value);
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
      PYROWAVE_ENTRY(pyrowave_vibepollo_device_get_identity);
      PYROWAVE_ENTRY(pyrowave_device_report_performance_stats);
      PYROWAVE_ENTRY(pyrowave_encoder_create);
      PYROWAVE_ENTRY(pyrowave_vibepollo_encoder_set_color_info);
      PYROWAVE_ENTRY(pyrowave_vibepollo_encoder_encode_cpu_planar_synchronous);
      PYROWAVE_ENTRY(pyrowave_vibepollo_encoder_wait_timeout);
      PYROWAVE_ENTRY(pyrowave_encoder_get_mapped_raw_bitstream);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_packets);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_critical_packets);
      PYROWAVE_ENTRY(pyrowave_encoder_get_num_active_blocks);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_block_active_words);
      PYROWAVE_ENTRY(pyrowave_encoder_packetize);
      PYROWAVE_ENTRY(pyrowave_encoder_destroy);
#undef PYROWAVE_ENTRY

      bool load(std::string &error) {
        std::vector<std::filesystem::path> candidates;
        std::error_code path_error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", path_error);
        if (!path_error && executable.is_absolute()) {
          candidates.emplace_back(executable.parent_path() / "libpyrowave-shared.so.0");
        }
#ifdef SUNSHINE_PYROWAVE_RUNTIME_INSTALL_DIR
        const std::filesystem::path installed {SUNSHINE_PYROWAVE_RUNTIME_INSTALL_DIR};
        if (installed.is_absolute()) candidates.emplace_back(installed / "libpyrowave-shared.so.0");
#endif
        // Only trusted explicit bundle/install paths. Never use a bare soname,
        // CWD, a user-supplied library path, or the dynamic loader search path.
        for (const auto &path : candidates) {
          module.value = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
          if (module.value) break;
          const char *detail = dlerror();
          if (!error.empty()) error += "; ";
          error += path.string() + ": " + (detail ? detail : "cannot load runtime");
        }
        if (!module.value) {
          if (error.empty()) error = "Cannot resolve an absolute PyroWave bundle/install path";
          return false;
        }
        error.clear();
#define PYROWAVE_LOAD(name) \
  name = std::bit_cast<decltype(name)>(dlsym(module.value, #name)); \
  if (!name) { \
    error = "PyroWave runtime is missing " #name; \
    return false; \
  }
        PYROWAVE_LOAD(pyrowave_get_api_version);
        std::uint32_t major = 0, minor = 0, patch = 0;
        pyrowave_get_api_version(&major, &minor, &patch);
        if (major != 0 || minor != 5 || patch != 0) {
          error = "Unsupported PyroWave API; expected 0.5.0";
          return false;
        }
        using contract_fn = const char *(*)();
        const auto contract = std::bit_cast<contract_fn>(dlsym(module.value, "pyrowave_vibepollo_runtime_contract"));
        const char *actual_contract = contract ? contract() : nullptr;
        if (!actual_contract || actual_contract != ::pyrowave::runtime_contract) {
          error = "PyroWave runtime contract is missing or incompatible; install the bundled pinned runtime";
          return false;
        }
        PYROWAVE_LOAD(pyrowave_vibepollo_configure_precision);
        PYROWAVE_LOAD(pyrowave_create_device_by_compat);
        PYROWAVE_LOAD(pyrowave_device_set_queue_type);
        PYROWAVE_LOAD(pyrowave_device_destroy);
        PYROWAVE_LOAD(pyrowave_vibepollo_device_activate);
        PYROWAVE_LOAD(pyrowave_vibepollo_device_get_identity);
        PYROWAVE_LOAD(pyrowave_device_report_performance_stats);
        PYROWAVE_LOAD(pyrowave_encoder_create);
        PYROWAVE_LOAD(pyrowave_vibepollo_encoder_set_color_info);
        PYROWAVE_LOAD(pyrowave_vibepollo_encoder_encode_cpu_planar_synchronous);
        PYROWAVE_LOAD(pyrowave_vibepollo_encoder_wait_timeout);
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
      std::optional<std::array<std::uint8_t, 16>> requested_uuid;
      adapter_identity_t identity;
      std::atomic_bool poisoned {false};

      process_context_t(std::shared_ptr<process_runtime_t> shared_runtime, std::optional<std::array<std::uint8_t, 16>> uuid):
          runtime {std::move(shared_runtime)}, mutex {runtime->mutex}, api {runtime->api}, requested_uuid {uuid} {
      }

      ~process_context_t() {
        std::lock_guard lock {mutex};
        if (device) {
          api.pyrowave_vibepollo_device_activate(device);
          api.pyrowave_device_destroy(device);
        }
      }
    };

    std::shared_ptr<process_context_t> process_context(std::optional<std::array<std::uint8_t, 16>> uuid) {
      struct registry_t {
        std::shared_ptr<process_runtime_t> runtime = process_runtime();
        std::map<std::string, std::shared_ptr<process_context_t>> selectors;
      };
      static registry_t registry;
      std::lock_guard lock {registry.runtime->mutex};
      const std::string key = uuid ? format_uuid(*uuid) : "default";
      auto &context = registry.selectors[key];
      if (!context) context = std::make_shared<process_context_t>(registry.runtime, uuid);
      return context;
    }
  }  // namespace

  std::string adapter_identity_t::uuid_string() const {
    return format_uuid(uuid);
  }

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
    pyrowave_encoder encoder = nullptr;
    int width;
    int height;
    ::pyrowave::profile_t profile;
    bool failed = false;
    frame_statistics_t statistics;

    impl_t(int width, int height, const ::pyrowave::profile_t &profile, std::optional<std::array<std::uint8_t, 16>> uuid):
        cached_context {process_context(uuid)}, api {cached_context->api}, width {width}, height {height}, profile {profile} {
    }

    ~impl_t() {
      std::lock_guard lock {cached_context->mutex};
      if (encoder) {
        api.pyrowave_vibepollo_device_activate(cached_context->device);
        api.pyrowave_encoder_destroy(encoder);
      }
    }

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

    bool check(pyrowave_result result, const char *operation, std::string &error, bool poison = true) {
      if (result == PYROWAVE_SUCCESS) return true;
      error = api_message(operation, result);
      failed = true;
      if (poison) {
        cached_context->poisoned = true;
        error += "; restart the host before retrying this PyroWave device";
      }
      return false;
    }

    bool initialize(std::string &error) {
      if (cached_context->poisoned) {
        error = "PyroWave adapter context failed; restart the host before retrying";
        return false;
      }
      auto &runtime = *cached_context->runtime;
      if (!runtime.load()) {
        error = runtime.load_error;
        return false;
      }
      if (!cached_context->device) {
        if (!check(api.pyrowave_vibepollo_configure_precision(-1, &runtime.precision), "Read wavelet precision", error)) return false;
        pyrowave_uuid uuid {};
        if (cached_context->requested_uuid) std::copy(cached_context->requested_uuid->begin(), cached_context->requested_uuid->end(), uuid.uuid);
        if (!check(api.pyrowave_create_device_by_compat(0, 0, cached_context->requested_uuid ? &uuid : nullptr, nullptr, nullptr, &cached_context->device), "Create Vulkan encoder device", error) ||
            !check(api.pyrowave_device_set_queue_type(cached_context->device, VK_QUEUE_COMPUTE_BIT), "Select compute queue", error)) return false;
        pyrowave_vibepollo_device_identity identity {};
        if (!check(api.pyrowave_vibepollo_device_get_identity(cached_context->device, &identity), "Query Vulkan encoder identity", error)) return false;
        cached_context->identity.name = identity.name;
        std::copy(std::begin(identity.uuid), std::end(identity.uuid), cached_context->identity.uuid.begin());
        cached_context->identity.vendor_id = identity.vendor_id;
        cached_context->identity.device_id = identity.device_id;
      }
      if (!check(api.pyrowave_vibepollo_device_activate(cached_context->device), "Activate Vulkan instance", error)) return false;
      const pyrowave_encoder_create_info create_info {
        cached_context->device, width, height,
        profile.chroma_444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420
      };
      if (!check(api.pyrowave_encoder_create(&create_info, &encoder), "Create PyroWave encoder", error, false)) return false;
      const pyrowave_vibepollo_color_info color_info {
        profile.primaries_bt2020 ? 1u : 0u, profile.transfer_pq ? 1u : 0u,
        profile.matrix_bt2020 ? 1u : 0u, profile.full_range ? 0u : 1u, profile.chroma_left ? 1u : 0u
      };
      return check(api.pyrowave_vibepollo_encoder_set_color_info(encoder, &color_info), "Set input color metadata", error, false);
    }
  };

  encoder_t::encoder_t(std::unique_ptr<impl_t> implementation): impl {std::move(implementation)} {
  }

  encoder_t::~encoder_t() = default;

  std::unique_ptr<encoder_t> encoder_t::create(int width, int height, const ::pyrowave::profile_t &profile, std::string &error, std::string_view device_uuid) {
    error.clear();
    static_assert(std::endian::native == std::endian::little, "PyroWave CPU input requires a little-endian host");
    if (width < 1 || height < 1 || width > 16384 || height > 16384 ||
        (!profile.chroma_444 && ((width & 1) || (height & 1))) ||
        profile.input_plane_bytes(width, height) > ::pyrowave::maximum_input_plane_bytes) {
      error = "Invalid PyroWave dimensions/chroma or input planes exceed the host allocation limit";
      return nullptr;
    }
    std::optional<std::array<std::uint8_t, 16>> uuid;
    if (!device_uuid.empty()) {
      uuid = parse_uuid(device_uuid);
      if (!uuid) {
        error = "PyroWave encoder GPU UUID must contain 32 hexadecimal digits (optional standard hyphens)";
        return nullptr;
      }
    }
    auto implementation = std::make_unique<impl_t>(width, height, profile, uuid);
    bool initialized;
    {
      std::lock_guard lock {implementation->cached_context->mutex};
      try {
        initialized = implementation->initialize(error);
      } catch (const std::exception &failure) {
        implementation->cached_context->poisoned = true;
        error = std::string("PyroWave initialization threw: ") + failure.what();
        initialized = false;
      } catch (...) {
        implementation->cached_context->poisoned = true;
        error = "PyroWave initialization threw an unknown exception";
        initialized = false;
      }
    }
    if (!initialized) return nullptr;
    return std::unique_ptr<encoder_t>(new encoder_t(std::move(implementation)));
  }

  std::optional<packet_list_t> encoder_t::encode(const plane_views_t &planes, const plane_strides_t &row_strides, std::size_t target_bytes, std::string &error) {
    std::lock_guard lock {impl->cached_context->mutex};
    impl_t::exception_guard_t exception_guard {*impl};
    error.clear();
    impl->statistics = {};
    target_bytes &= ~std::size_t {3};
    if (impl->failed || impl->cached_context->poisoned || target_bytes < 8 || target_bytes > maximum_target_bytes) {
      error = "Invalid PyroWave frame state or byte budget";
      return std::nullopt;
    }
    const unsigned sample_bits = impl->profile.high_precision ? 16u : 8u;
    const std::size_t sample_bytes = sample_bits / 8;
    pyrowave_cpu_buffer buffers {};
    buffers.width = impl->width;
    buffers.height = impl->height;
    buffers.format = impl->profile.chroma_444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    std::size_t total_input = 0;
    for (unsigned plane = 0; plane < 3; ++plane) {
      const std::size_t divisor = plane && !impl->profile.chroma_444 ? 2 : 1;
      const auto width = std::size_t(impl->width) / divisor;
      const auto height = std::size_t(impl->height) / divisor;
      const auto stride = row_strides[plane];
      if (planes[plane].empty() || stride < width * sample_bytes || stride % sample_bytes ||
          stride / sample_bytes > std::numeric_limits<std::uint32_t>::max() || planes[plane].size() / height < stride ||
          stride > ::pyrowave::maximum_input_plane_bytes / height ||
          stride * height > ::pyrowave::maximum_input_plane_bytes - total_input) {
        error = "Invalid PyroWave input plane extent, stride or storage precision";
        return std::nullopt;
      }
      total_input += stride * height;
      buffers.data[plane] = const_cast<std::uint8_t *>(planes[plane].data());
      buffers.row_stride_in_bytes[plane] = stride;
      buffers.plane_size_in_bytes[plane] = planes[plane].size();
    }
    if (!impl->check(impl->api.pyrowave_vibepollo_device_activate(impl->cached_context->device), "Activate Vulkan instance", error)) return std::nullopt;
    const auto encode_start = statistics_clock_t::now();
    const pyrowave_rate_control rate {target_bytes};
    if (!impl->check(impl->api.pyrowave_vibepollo_encoder_encode_cpu_planar_synchronous(impl->encoder, &buffers, sample_bits, &rate), "Upload and encode YCbCr frame", error) ||
        !impl->check(impl->api.pyrowave_vibepollo_encoder_wait_timeout(impl->encoder, gpu_wait_nanoseconds), "Wait for GPU encode", error)) return std::nullopt;
    impl->statistics.encode_wait_us = elapsed_microseconds(encode_start);
    const auto packetize_start = statistics_clock_t::now();
    const void *raw = nullptr, *metadata = nullptr;
    std::size_t raw_size = 0, metadata_size = 0;
    if (!impl->check(impl->api.pyrowave_encoder_get_mapped_raw_bitstream(impl->encoder, &raw, &raw_size, &metadata, &metadata_size), "Read bitstream allocation", error)) return std::nullopt;
    if (!raw || !metadata || raw_size < target_bytes || raw_size > maximum_raw_bytes - 8 || metadata_size > maximum_raw_bytes) {
      impl->check(PYROWAVE_ERROR_GENERIC, "Invalid PyroWave bitstream allocation", error);
      return std::nullopt;
    }
    std::size_t packet_count = 0;
    if (!impl->check(impl->api.pyrowave_encoder_compute_num_packets(impl->encoder, native_packet_boundary, &packet_count), "Count native packets", error)) return std::nullopt;
    if (!packet_count || packet_count > maximum_native_packets) {
      impl->check(PYROWAVE_ERROR_GENERIC, "Invalid PyroWave packet count", error);
      return std::nullopt;
    }
    try {
      std::vector<pyrowave_packet> packets(packet_count);
      std::vector<std::uint8_t> packed(raw_size + 8);
      std::size_t written_packets = 0;
      if (!impl->check(impl->api.pyrowave_encoder_packetize(impl->encoder, packets.data(), native_packet_boundary, &written_packets, packed.data(), packed.size()), "Packetize frame", error)) return std::nullopt;
      if (written_packets != packet_count) {
        impl->check(PYROWAVE_ERROR_GENERIC, "PyroWave packet count changed during packetization", error);
        return std::nullopt;
      }
      std::size_t total = 0;
      for (const auto &packet : packets) {
        if (packet.offset != total || packet.size == 0 || packet.offset > packed.size() || packet.size > packed.size() - packet.offset) {
          impl->check(PYROWAVE_ERROR_GENERIC, "Invalid PyroWave packet boundaries", error);
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
        if (!impl->check(impl->api.pyrowave_encoder_compute_num_critical_packets(impl->encoder, bands, native_packet_boundary, 0, &critical_count), "Count critical packets", error)) return std::nullopt;
        if (critical_count > packet_count) {
          impl->check(PYROWAVE_ERROR_GENERIC, "Invalid PyroWave critical packet count", error);
          return std::nullopt;
        }
      }
      auto &active_count = impl->statistics.active_block_count;
      const int bands = impl->statistics.active_block_bands;
      if (!impl->check(impl->api.pyrowave_encoder_get_num_active_blocks(impl->encoder, bands, &active_count), "Count sideband blocks", error)) return std::nullopt;
      if (active_count > maximum_sideband_words * 32) {
        impl->check(PYROWAVE_ERROR_GENERIC, "Invalid PyroWave sideband block count", error);
        return std::nullopt;
      }
      auto &words = impl->statistics.active_block_words;
      words.resize((active_count + 31) / 32);
      if (!impl->check(impl->api.pyrowave_encoder_compute_block_active_words(impl->encoder, bands, words.data(), words.size()), "Compute active-block sideband", error)) return std::nullopt;
      packet_list_t output;
      output.reserve(packet_count);
      for (const auto &packet : packets) output.emplace_back(packed.data() + packet.offset, packed.data() + packet.offset + packet.size);
      impl->statistics.packetize_us = elapsed_microseconds(packetize_start);
      impl->statistics.native_bytes = total;
      impl->statistics.native_packets = packet_count;
      return output;
    } catch (const std::bad_alloc &) {
      impl->check(PYROWAVE_ERROR_OUT_OF_HOST_MEMORY, "Allocate packet buffers", error);
      return std::nullopt;
    }
  }

  adapter_identity_t encoder_t::adapter_identity() const {
    return impl->cached_context->identity;
  }

  std::string encoder_t::adapter_identity_text() const {
    const auto &identity = impl->cached_context->identity;
    return identity.name + " (Vulkan UUID " + identity.uuid_string() + ')';
  }

  frame_statistics_t encoder_t::last_frame_statistics() const {
    return impl->statistics;
  }

  std::vector<std::string> encoder_t::performance_statistics(bool reset) const {
    std::lock_guard lock {impl->cached_context->mutex};
    std::vector<std::string> messages;
    if (impl->failed || impl->cached_context->poisoned) return messages;
    if (impl->api.pyrowave_vibepollo_device_activate(impl->cached_context->device) != PYROWAVE_SUCCESS) return messages;
    const auto append = [](void *userdata, const char *message) {
      auto &output = *static_cast<std::vector<std::string> *>(userdata);
      try {
        if (message && output.size() < 128) output.emplace_back(message);
      } catch (...) {
        // Optional diagnostics must not throw across the C callback boundary.
      }
    };
    impl->api.pyrowave_device_report_performance_stats(impl->cached_context->device, append, &messages, reset);
    return messages;
  }
}  // namespace platf::pyrowave_cpu

#endif  // SUNSHINE_ENABLE_PYROWAVE && __linux__
