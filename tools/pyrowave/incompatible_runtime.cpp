// Deliberately incomplete fake DLL for testing version rejection in isolation.
#include <cstdint>
extern "C" __declspec(dllexport) void pyrowave_get_api_version(std::uint32_t *major, std::uint32_t *minor, std::uint32_t *patch) {
  *major = 0;
#ifdef PYROWAVE_FAKE_CONTRACT
  *minor = 5;
#else
  *minor = 4;
#endif
  *patch = 0;
}
#ifdef PYROWAVE_FAKE_CONTRACT
extern "C" __declspec(dllexport) const char *pyrowave_vibepollo_runtime_contract() {
  return "intentionally-incompatible-contract";
}
#endif
