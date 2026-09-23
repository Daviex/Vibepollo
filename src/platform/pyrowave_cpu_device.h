#pragma once
#include <memory>

namespace platf {
  struct pyrowave_encode_device_t;
  std::unique_ptr<pyrowave_encode_device_t> make_pyrowave_cpu_encode_device(bool source_hdr = false);
}
