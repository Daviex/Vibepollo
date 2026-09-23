/** Optional, dynamically loaded PyroWave Metal encoder. */
#include "pyrowave_metal_runtime.h"

#if defined(SUNSHINE_ENABLE_PYROWAVE) && defined(__APPLE__)

#include "src/logging.h"
#include "src/pyrowave_runtime_contract.h"
#include <pyrowave/pyrowave_metal.h>
#import <Metal/Metal.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <chrono>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace platf::pyrowave_metal {
  namespace {
    constexpr std::size_t maximum_target_bytes = 8 * 1024 * 1024;
    constexpr std::size_t maximum_raw_bytes = 64 * 1024 * 1024;
    constexpr std::size_t maximum_native_packets = 65536;
    constexpr std::size_t maximum_sideband_words = 32768;
    constexpr std::size_t native_packet_boundary = 64 * 1024;
    constexpr auto runtime_name = "libpyrowave-metal.0.dylib";
    using clock_t = std::chrono::steady_clock;

    double elapsed_us(clock_t::time_point start) {
      return std::chrono::duration<double, std::micro>(clock_t::now() - start).count();
    }

    // No symbol from the optional runtime is linked into the host binary.
    struct api_t {
      void *library = nullptr;
#define PYROWAVE_ENTRY(name) decltype(&::name) name = nullptr
      PYROWAVE_ENTRY(pyrowave_get_api_version);
      PYROWAVE_ENTRY(pyrowave_vibepollo_runtime_contract);
      PYROWAVE_ENTRY(pyrowave_vibepollo_configure_precision);
      PYROWAVE_ENTRY(pyrowave_vibepollo_encoder_set_color_info);
      PYROWAVE_ENTRY(pyrowave_result_to_string);
      PYROWAVE_ENTRY(pyrowave_device_create);
      PYROWAVE_ENTRY(pyrowave_device_is_supported);
      PYROWAVE_ENTRY(pyrowave_device_destroy);
      PYROWAVE_ENTRY(pyrowave_encoder_create);
      PYROWAVE_ENTRY(pyrowave_encoder_destroy);
      PYROWAVE_ENTRY(pyrowave_encoder_encode_cpu_synchronous);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_packets);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_num_critical_packets);
      PYROWAVE_ENTRY(pyrowave_encoder_packetize);
      PYROWAVE_ENTRY(pyrowave_encoder_get_mapped_raw_bitstream);
      PYROWAVE_ENTRY(pyrowave_encoder_get_num_active_blocks);
      PYROWAVE_ENTRY(pyrowave_encoder_compute_block_active_words);
#undef PYROWAVE_ENTRY

      ~api_t() {
        if (library) dlclose(library);
      }

      template<class T> bool bind(T &entry, const char *name, std::string &error) {
        entry = reinterpret_cast<T>(dlsym(library, name));
        if (entry) return true;
        error = std::string("PyroWave Metal runtime is missing ") + name;
        return false;
      }

      bool load(std::string &error) {
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        if (size == 0 || size > 1024 * 1024) {
          error = "Cannot resolve the host executable path for PyroWave Metal";
          return false;
        }
        std::vector<char> path(size);
        if (_NSGetExecutablePath(path.data(), &size) != 0) {
          error = "Cannot resolve the host executable path for PyroWave Metal";
          return false;
        }
        std::error_code fs_error;
        auto executable = std::filesystem::canonical(path.data(), fs_error);
        if (fs_error || !executable.is_absolute()) {
          error = "Cannot resolve the absolute host executable path for PyroWave Metal";
          return false;
        }
        std::vector<std::filesystem::path> candidates {
          executable.parent_path() / runtime_name,
          executable.parent_path().parent_path() / "Frameworks" / runtime_name,
        };
#ifdef SUNSHINE_PYROWAVE_RUNTIME_INSTALL_DIR
        const std::filesystem::path installed {SUNSHINE_PYROWAVE_RUNTIME_INSTALL_DIR};
        if (installed.is_absolute()) candidates.emplace_back(installed / runtime_name);
#endif
        // Resolve only explicit installation paths, never a CWD/search-path name.
        for (const auto &candidate : candidates) {
          if (!std::filesystem::is_regular_file(candidate, fs_error)) continue;
          library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
          if (!library) {
            const char *detail = dlerror();
            error = "Cannot load PyroWave Metal runtime: " + std::string(detail ? detail : "unknown loader error");
            return false;
          }
          break;
        }
        if (!library) {
          error = "PyroWave Metal runtime is missing beside the executable or in the installed Frameworks/library directory";
          return false;
        }
#define PYROWAVE_BIND(name) if (!bind(name, #name, error)) return false
        PYROWAVE_BIND(pyrowave_get_api_version);
        std::uint32_t major = 0, minor = 0, patch = 0;
        pyrowave_get_api_version(&major, &minor, &patch);
        if (major != 0 || minor != 5 || patch != 0) {
          error = "Incompatible PyroWave Metal API: expected 0.5.0";
          return false;
        }
        PYROWAVE_BIND(pyrowave_vibepollo_runtime_contract);
        const char *contract = pyrowave_vibepollo_runtime_contract();
        if (!contract || ::pyrowave::runtime_contract != contract) {
          error = "Incompatible PyroWave Metal runtime contract; install the library bundled with this host";
          return false;
        }
        PYROWAVE_BIND(pyrowave_vibepollo_configure_precision);
        PYROWAVE_BIND(pyrowave_vibepollo_encoder_set_color_info);
        PYROWAVE_BIND(pyrowave_result_to_string);
        PYROWAVE_BIND(pyrowave_device_create);
        PYROWAVE_BIND(pyrowave_device_is_supported);
        PYROWAVE_BIND(pyrowave_device_destroy);
        PYROWAVE_BIND(pyrowave_encoder_create);
        PYROWAVE_BIND(pyrowave_encoder_destroy);
        PYROWAVE_BIND(pyrowave_encoder_encode_cpu_synchronous);
        PYROWAVE_BIND(pyrowave_encoder_compute_num_packets);
        PYROWAVE_BIND(pyrowave_encoder_compute_num_critical_packets);
        PYROWAVE_BIND(pyrowave_encoder_packetize);
        PYROWAVE_BIND(pyrowave_encoder_get_mapped_raw_bitstream);
        PYROWAVE_BIND(pyrowave_encoder_get_num_active_blocks);
        PYROWAVE_BIND(pyrowave_encoder_compute_block_active_words);
#undef PYROWAVE_BIND
        return true;
      }
    };

    struct shared_context_t {
      std::mutex mutex;
      api_t api;
      pyrowave_device device = nullptr;
      bool attempted = false;
      bool poisoned = false;
      int precision = -1;
      std::string identity;
      std::string failure;

      ~shared_context_t() {
        // API and its library outlive the device, including when the last live
        // encoder keeps this shared context alive past static storage teardown.
        if (device) api.pyrowave_device_destroy(device);
      }

      bool fail(std::string detail, std::string &error) {
        poisoned = true;
        failure = std::move(detail) + "; restart the host before retrying";
        error = failure;
        return false;
      }

      bool load(std::string &error) {
        if (poisoned) { error = failure; return false; }
        if (attempted) return true;
        attempted = true;
        try {
          if (!api.load(error)) return fail(error, error);
          return true;
        } catch (const std::exception &e) {
          return fail(std::string("Cannot initialize PyroWave Metal: ") + e.what(), error);
        } catch (...) {
          return fail("Cannot initialize PyroWave Metal", error);
        }
      }

      bool check(pyrowave_result result, const char *operation, std::string &error) {
        if (result == PYROWAVE_SUCCESS) return true;
        return fail(std::string(operation) + ": " + api.pyrowave_result_to_string(result), error);
      }

      bool initialize(std::string &error) {
        if (!load(error)) return false;
        if (device) return true;
        @autoreleasepool {
          id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
          if (!mtl) return fail("No default Metal GPU is available", error);
          // The PyroWave C API retains its own reference on successful creation.
          struct release_device_t {
            id<MTLDevice> object;
            ~release_device_t() {
#if !__has_feature(objc_arc)
              [object release];
#endif
            }
          } release {mtl};
          if (!api.pyrowave_device_is_supported((__bridge void *) mtl)) {
            return fail("PyroWave Metal requires an Apple7-or-newer GPU; Intel and AMD Mac GPUs are unsupported", error);
          }
          identity = std::string(mtl.name.UTF8String ?: "Metal default GPU") + " (registry " + std::to_string(mtl.registryID) + ")";
          pyrowave_device_create_info info {};
          info.mtl_device = (__bridge void *) mtl;
          info.message_callback = [](void *, const char *message) {
            try { if (message) BOOST_LOG(debug) << "PyroWave Metal: " << message; } catch (...) {}
          };
          if (!check(api.pyrowave_device_create(&info, &device), "Create PyroWave Metal device", error)) return false;
          if (!device) return fail("PyroWave Metal returned an empty device", error);
          if (!check(api.pyrowave_vibepollo_configure_precision(-1, &precision), "Read PyroWave Metal precision", error)) return false;
        }
        return true;
      }
    };

    std::shared_ptr<shared_context_t> shared_context() {
      static const auto context = std::make_shared<shared_context_t>();
      return context;
    }
  }

  struct encoder_t::impl_t {
    std::shared_ptr<shared_context_t> context;
    pyrowave_encoder encoder = nullptr;
    ::pyrowave::profile_t profile;
    int width = 0, height = 0;
    frame_statistics_t statistics;
    std::uint64_t reported_frames = 0;
    double encode_total_us = 0, packetize_total_us = 0;

    ~impl_t() {
      if (encoder) {
        std::lock_guard lock {context->mutex};
        context->api.pyrowave_encoder_destroy(encoder);
      }
    }
  };

  bool configure_precision(int requested, int &effective, std::string &error) {
    error.clear();
    effective = -1;
    if (requested < -1 || requested > 2) { error = "Invalid PyroWave wavelet precision"; return false; }
    const auto context = shared_context();
    std::lock_guard lock {context->mutex};
    if (!context->load(error)) return false;
    const auto result = context->api.pyrowave_vibepollo_configure_precision(requested, &effective);
    if (result != PYROWAVE_SUCCESS) {
      error = "PyroWave wavelet precision cannot change after device creation; restart the host to apply it";
      return false;
    }
    context->precision = effective;
    return true;
  }

  int effective_precision() {
    const auto context = shared_context();
    std::lock_guard lock {context->mutex};
    return context->precision;
  }

  encoder_t::encoder_t(std::unique_ptr<impl_t> value): impl(std::move(value)) {}
  encoder_t::~encoder_t() = default;

  std::unique_ptr<encoder_t> encoder_t::create(int width, int height,
    const ::pyrowave::profile_t &profile, std::string &error) {
    error.clear();
    if (profile.high_precision) {
      error = "PyroWave Metal CPU input supports 8-bit planes only; the negotiated 16-bit profile is unsupported";
      return nullptr;
    }
    if (width < 1 || height < 1 || width > 16384 || height > 16384 ||
        (!profile.chroma_444 && ((width | height) & 1)) ||
        profile.input_plane_bytes(width, height) > ::pyrowave::maximum_input_plane_bytes) {
      error = "Invalid PyroWave Metal dimensions or input planes exceed the 512 MiB limit";
      return nullptr;
    }
    auto value = std::make_unique<impl_t>();
    value->context = shared_context();
    value->profile = profile;
    value->width = width;
    value->height = height;
    // value is declared before the lock so failure releases the lock before
    // destroying an encoder and acquiring the same lock in its destructor.
    std::lock_guard lock {value->context->mutex};
    auto &context = *value->context;
    bool shared_device_ready = false;
    try {
      if (!context.initialize(error)) return nullptr;
      shared_device_ready = true;
      pyrowave_encoder_create_info info {};
      info.device = context.device;
      info.width = width;
      info.height = height;
      info.chroma = profile.chroma_444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
      const auto created = context.api.pyrowave_encoder_create(&info, &value->encoder);
      if (created != PYROWAVE_SUCCESS || !value->encoder) {
        // A resource/admission failure belongs to this session. Existing
        // encoders on the shared device can continue using their allocations.
        error = "Cannot create PyroWave Metal encoder: " + std::string(context.api.pyrowave_result_to_string(created));
        return nullptr;
      }
      const pyrowave_vibepollo_color_info color {
        profile.primaries_bt2020, profile.transfer_pq, profile.matrix_bt2020, !profile.full_range, profile.chroma_left,
      };
      const auto configured = context.api.pyrowave_vibepollo_encoder_set_color_info(value->encoder, &color);
      if (configured != PYROWAVE_SUCCESS) {
        error = "Cannot configure PyroWave Metal color metadata: " + std::string(context.api.pyrowave_result_to_string(configured));
        return nullptr;
      }
      return std::unique_ptr<encoder_t>(new encoder_t(std::move(value)));
    } catch (const std::bad_alloc &) {
      if (shared_device_ready) error = "Cannot allocate PyroWave Metal session resources";
      else context.fail("Cannot allocate the shared PyroWave Metal device", error);
      return nullptr;
    } catch (const std::exception &e) {
      context.fail(std::string("Cannot initialize PyroWave Metal encoder: ") + e.what(), error);
      return nullptr;
    } catch (...) {
      context.fail("Cannot initialize PyroWave Metal encoder", error);
      return nullptr;
    }
  }

  std::optional<packet_list_t> encoder_t::encode(
    const std::array<std::span<const std::uint8_t>, 3> &planes,
    const std::array<std::size_t, 3> &row_strides,
    std::size_t target_bytes, std::string &error) {
    error.clear();
    auto &context = *impl->context;
    std::lock_guard lock {context.mutex};
    if (context.poisoned) { error = context.failure; return std::nullopt; }
    target_bytes &= ~std::size_t {3};
    if (target_bytes < 8 || target_bytes > maximum_target_bytes) {
      error = "Invalid PyroWave Metal frame byte budget";
      return std::nullopt;
    }
    pyrowave_cpu_buffer input {};
    input.width = impl->width;
    input.height = impl->height;
    input.format = impl->profile.chroma_444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    std::size_t total_input = 0;
    for (std::size_t i = 0; i < planes.size(); ++i) {
      const auto w = std::size_t(i == 0 || impl->profile.chroma_444 ? impl->width : impl->width / 2);
      const auto h = std::size_t(i == 0 || impl->profile.chroma_444 ? impl->height : impl->height / 2);
      if (row_strides[i] < w || row_strides[i] > ::pyrowave::maximum_input_plane_bytes / h ||
          planes[i].size() < row_strides[i] * h ||
          row_strides[i] * h > ::pyrowave::maximum_input_plane_bytes - total_input) {
        error = "Invalid PyroWave Metal input plane extent or stride";
        return std::nullopt;
      }
      total_input += row_strides[i] * h;
      input.data[i] = const_cast<std::uint8_t *>(planes[i].data());
      input.row_stride_in_bytes[i] = row_strides[i];
      input.plane_size_in_bytes[i] = planes[i].size();
    }

    try {
      @autoreleasepool {
        impl->statistics = {};
        const auto start = clock_t::now();
        const pyrowave_rate_control rate {target_bytes};
        if (!context.check(context.api.pyrowave_encoder_encode_cpu_synchronous(impl->encoder, &input, &rate), "Upload and encode Metal frame", error)) return std::nullopt;
        const void *raw = nullptr, *metadata = nullptr;
        std::size_t raw_size = 0, metadata_size = 0;
        // This query is the upstream GPU completion wait. It is not cancellable.
        if (!context.check(context.api.pyrowave_encoder_get_mapped_raw_bitstream(impl->encoder, &raw, &raw_size, &metadata, &metadata_size), "Wait for Metal frame", error)) return std::nullopt;
        impl->statistics.encode_wait_us = elapsed_us(start);
        const auto packet_start = clock_t::now();
        if (!raw || !metadata || raw_size < target_bytes || raw_size > maximum_raw_bytes - 8 || metadata_size > maximum_raw_bytes) {
          context.fail("PyroWave Metal returned invalid mapped allocation bounds", error);
          return std::nullopt;
        }
        std::size_t packet_count = 0;
        if (!context.check(context.api.pyrowave_encoder_compute_num_packets(impl->encoder, native_packet_boundary, &packet_count), "Count Metal packets", error)) return std::nullopt;
        if (!packet_count || packet_count > maximum_native_packets) {
          context.fail("PyroWave Metal returned an invalid packet count", error);
          return std::nullopt;
        }
        std::vector<pyrowave_packet> packets(packet_count);
        std::vector<std::uint8_t> packed(raw_size + 8);
        std::size_t written = 0;
        if (!context.check(context.api.pyrowave_encoder_packetize(impl->encoder, packets.data(), native_packet_boundary, &written, packed.data(), packed.size()), "Packetize Metal frame", error)) return std::nullopt;
        if (written != packet_count) {
          context.fail("PyroWave Metal packet count changed during packetization", error);
          return std::nullopt;
        }
        std::size_t total = 0;
        for (const auto &packet : packets) {
          if (packet.offset != total || !packet.size || packet.offset > packed.size() || packet.size > packed.size() - packet.offset) {
            context.fail("PyroWave Metal returned invalid packet boundaries", error);
            return std::nullopt;
          }
          if (total > target_bytes || packet.size > target_bytes - total) {
            error = "PyroWave Metal frame exceeds its byte budget";
            return std::nullopt;
          }
          total += packet.size;
        }
        for (int bands = 0; bands <= 4; ++bands) {
          auto &count = impl->statistics.critical_packets[bands];
          if (!context.check(context.api.pyrowave_encoder_compute_num_critical_packets(impl->encoder, bands, native_packet_boundary, 0, &count), "Count critical Metal packets", error)) return std::nullopt;
          if (count > packet_count) { context.fail("Invalid Metal critical packet count", error); return std::nullopt; }
        }
        auto &active_count = impl->statistics.active_block_count;
        const int bands = impl->statistics.active_block_bands;
        if (!context.check(context.api.pyrowave_encoder_get_num_active_blocks(impl->encoder, bands, &active_count), "Count Metal sideband blocks", error)) return std::nullopt;
        if (active_count > maximum_sideband_words * 32) { context.fail("Invalid Metal sideband extent", error); return std::nullopt; }
        auto &words = impl->statistics.active_block_words;
        words.resize((active_count + 31) / 32);
        if (!context.check(context.api.pyrowave_encoder_compute_block_active_words(impl->encoder, bands, words.data(), words.size()), "Compute Metal active block mask", error)) return std::nullopt;
        packet_list_t result;
        result.reserve(packet_count);
        for (const auto &packet : packets) result.emplace_back(packed.data() + packet.offset, packed.data() + packet.offset + packet.size);
        impl->statistics.native_bytes = total;
        impl->statistics.native_packets = packet_count;
        impl->statistics.packetize_us = elapsed_us(packet_start);
        ++impl->reported_frames;
        impl->encode_total_us += impl->statistics.encode_wait_us;
        impl->packetize_total_us += impl->statistics.packetize_us;
        return result;
      }
    } catch (const std::exception &e) {
      context.fail(std::string("PyroWave Metal frame failed: ") + e.what(), error);
      return std::nullopt;
    } catch (...) {
      context.fail("PyroWave Metal frame failed", error);
      return std::nullopt;
    }
  }

  frame_statistics_t encoder_t::last_frame_statistics() const {
    std::lock_guard lock {impl->context->mutex};
    return impl->statistics;
  }

  std::string encoder_t::adapter_identity_text() const {
    std::lock_guard lock {impl->context->mutex};
    return impl->context->identity;
  }

  std::vector<std::string> encoder_t::performance_statistics(bool reset) const {
    std::lock_guard lock {impl->context->mutex};
    if (!impl->reported_frames) return {};
    std::ostringstream text;
    text << "Metal CPU wall averages over " << impl->reported_frames << " frames: upload+encode+GPU wait "
         << impl->encode_total_us / impl->reported_frames << " us, packetization "
         << impl->packetize_total_us / impl->reported_frames
         << " us (upstream Metal C API has no isolated GPU timestamp report)";
    if (reset) { impl->reported_frames = 0; impl->encode_total_us = 0; impl->packetize_total_us = 0; }
    return {text.str()};
  }
}
#endif
