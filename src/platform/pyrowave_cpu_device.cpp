#include "pyrowave_cpu_device.h"
#include "common.h"
#include "src/video.h"
#include "src/pyrowave_colors.h"

#if defined(__APPLE__)
  #include "pyrowave_metal_runtime.h"
  namespace backend = platf::pyrowave_metal;
#else
  #include "pyrowave_cpu_runtime.h"
  namespace backend = platf::pyrowave_cpu;
#endif

namespace platf {
  namespace {
    class cpu_pyrowave_device_t final: public pyrowave_encode_device_t {
    public:
      explicit cpu_pyrowave_device_t(bool source_hdr): source_hdr(source_hdr) {}

      bool init_encoder(const video::config_t &config, const video::sunshine_colorspace_t &requested_colorspace) override {
        if (encoder) { error = "PyroWave encoder is already initialized"; return false; }
        int precision = -1;
        if (!backend::configure_precision(config::video.pyrowave_precision, precision, error)) return false;
        profile = config.pyrowave_profile;
        width = config.width;
        height = config.height;
        colorspace = requested_colorspace;
#ifdef __APPLE__
        encoder = backend::encoder_t::create(width, height, profile, error);
#else
        encoder = backend::encoder_t::create(width, height, profile, error, config::video.pyrowave_device_uuid);
#endif
        return static_cast<bool>(encoder);
      }

      int convert(img_t &image) override {
        if (!encoder || !image.data || image.width < 1 || image.height < 1 || image.row_pitch < 1) {
          error = "PyroWave system-memory capture is unavailable";
          return -1;
        }
        const ::pyrowave::colors::source_t source {
          .bytes = {image.data, std::size_t(image.row_pitch) * std::size_t(image.height)},
          .width = static_cast<std::uint32_t>(image.width), .height = static_cast<std::uint32_t>(image.height),
          .row_stride = static_cast<std::size_t>(image.row_pitch), .format = image.pyrowave_pixel_format, .hdr = source_hdr,
        };
        return ::pyrowave::colors::convert(source, width, height, profile, planes, error) ? 0 : -1;
      }

      std::optional<std::vector<std::vector<std::uint8_t>>> encode_frame(std::uint64_t, std::size_t target_bytes) override {
        if (!encoder) { error = "PyroWave encoder is not initialized"; return std::nullopt; }
        auto result = encoder->encode(planes.views(), planes.row_strides, target_bytes, error);
        if (result && ++frames % 300 == 0) {
          for (const auto &line : encoder->performance_statistics()) BOOST_LOG(debug) << "PyroWave GPU: " << line;
        }
        return result;
      }

      std::string error_reason() const override { return error; }
      std::string adapter_identity() const override { return encoder ? encoder->adapter_identity_text() : std::string {}; }
      frame_metadata_t frame_metadata() const override {
        frame_metadata_t result;
        if (!encoder) return result;
        const auto sample = encoder->last_frame_statistics();
        for (std::size_t i = 0; i < 5; ++i) result.critical_packets[i] = static_cast<std::uint32_t>(sample.critical_packets[i]);
        result.active_block_bands = static_cast<std::uint32_t>(sample.active_block_bands);
        result.active_block_count = static_cast<std::uint32_t>(sample.active_block_count);
        result.active_block_words = sample.active_block_words;
        return result;
      }

    private:
      bool source_hdr;
      std::uint32_t width = 0, height = 0;
      std::uint64_t frames = 0;
      ::pyrowave::profile_t profile;
      ::pyrowave::colors::planes_t planes;
      std::unique_ptr<backend::encoder_t> encoder;
      std::string error;
    };
  }

  std::unique_ptr<pyrowave_encode_device_t> make_pyrowave_cpu_encode_device(bool source_hdr) {
    return std::make_unique<cpu_pyrowave_device_t>(source_hdr);
  }
}
