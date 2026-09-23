#pragma once

#include <string_view>

namespace pyrowave {
  // Additive bundled C API contract, separate from upstream version 0.5.0.
  inline constexpr std::string_view runtime_contract = "d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1;color-metadata-v1;multi-device-v1;allocation-checks-v1;precision-config-v1;cpu-planar-v1;metal-extensions-v1";
}
