#!/usr/bin/env python3
"""Exercise the production desktop helper in an isolated Windows process.

The function is extracted verbatim from misc.cpp and compiled with real Win32
desktop calls. Wrappers inject open/switch failures and missing comparison API.
No windows, hooks, input events, display modes, or service changes are created.
"""

import argparse
import pathlib
import subprocess
import tempfile


HARNESS = r'''
#include <windows.h>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
using namespace std::literals;
using compare_t = BOOL(WINAPI *)(HANDLE, HANDLE);
static compare_t actual_compare;
static bool missing_compare;
static thread_local bool fail_open, fail_switch, bypass_compare, fail_initial;
static std::atomic<unsigned> close_failures = 0;
static unsigned handle_count() {
  DWORD n = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &n)) std::abort();
  return n;
}
static void require(bool condition, const char *message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static HDESK test_open(DWORD flags, BOOL inherit, ACCESS_MASK access) {
  if (fail_open) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
  return OpenInputDesktop(flags, inherit, access);
}
static BOOL test_switch(HDESK desktop) {
  if (fail_switch) { SetLastError(ERROR_BUSY); return FALSE; }
  return SetThreadDesktop(desktop);
}
static BOOL test_close(HDESK desktop) {
  const auto result = CloseDesktop(desktop);
  if (!result) ++close_failures;
  return result;
}
static HDESK test_current(DWORD thread) {
  return fail_initial ? nullptr : GetThreadDesktop(thread);
}
static BOOL WINAPI test_compare(HANDLE first, HANDLE second) {
  return bypass_compare ? FALSE : actual_compare(first, second);
}
static FARPROC test_proc(HMODULE, const char *) {
  return missing_compare ? nullptr : std::bit_cast<FARPROC>(&test_compare);
}
struct null_log_t { template<class T> null_log_t &operator<<(const T &) { return *this; } };
namespace util {
  struct hex_t { const char *to_string_view() const { return "test"; } };
  template<class T> hex_t hex(T) { return {}; }
}
#define BOOST_LOG(level) null_log_t{}
#define OpenInputDesktop test_open
#define SetThreadDesktop test_switch
#define CloseDesktop test_close
#define GetThreadDesktop test_current
#define GetProcAddress test_proc
namespace platf {
// PRODUCTION_HELPER
}
#undef OpenInputDesktop
#undef SetThreadDesktop
#undef CloseDesktop
#undef GetThreadDesktop
#undef GetProcAddress
int main(int argc, char **argv) {
  missing_compare = argc > 1 && std::string_view(argv[1]) == "--without-compare";
  const bool missing_initial = argc > 1 && std::string_view(argv[1]) == "--without-initial";
  actual_compare = std::bit_cast<compare_t>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles"));
  require(actual_compare, "CompareObjectHandles unavailable on test host");
  const auto first = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
  const auto second = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
  require(first && second && first != second, "Two desktop handles must open independently");
  require(actual_compare(first, second), "CompareObjectHandles must identify equal HDESK objects");
  require(CloseDesktop(first) && CloseDesktop(second), "Close unassociated comparison handles");
  // Warm up the MinGW emulated-TLS bookkeeping, which creates two process-wide
  // synchronization handles on its first registration of a TLS destructor.
  std::thread([] { require(platf::syncThreadDesktop(), "TLS warmup must succeed"); }).join();
  const auto process_baseline = handle_count();
  std::thread worker([&] {
    require(GetThreadDesktop(GetCurrentThreadId()), "Worker must have an initial desktop");
    const auto baseline = handle_count();
    if (missing_initial) {
      fail_initial = true;
      for (int i = 0; i < 64; ++i) require(!platf::syncThreadDesktop(), "Missing initial handle must fail");
      require(handle_count() == baseline, "Missing initial handle must not leak");
      fail_initial = false;
      return;
    }
    const auto first_owned = platf::syncThreadDesktop();
    std::cout << "worker_handles=" << baseline << "->" << handle_count() << '\n';
    require(first_owned && GetThreadDesktop(GetCurrentThreadId()) == first_owned, "First call must associate returned handle");
    require(handle_count() == baseline + 1, "Exactly one desktop handle must be retained");
    for (int i = 0; i < 64; ++i) {
      const auto current = platf::syncThreadDesktop();
      require(current && GetThreadDesktop(GetCurrentThreadId()) == current, "Returned handle must be current");
      if (!missing_compare) require(current == first_owned, "Unchanged desktop must have stable returned identity");
      require(handle_count() == baseline + 1, "Repeated calls must not leak");
    }
    const auto before_failures = GetThreadDesktop(GetCurrentThreadId());
    fail_open = true;
    for (int i = 0; i < 13; ++i) require(!platf::syncThreadDesktop(), "Open failure must not report success");
    fail_open = false;
    require(handle_count() == baseline + 1, "Open failure must not leak");
    fail_switch = bypass_compare = true;
    for (int i = 0; i < 13; ++i) require(!platf::syncThreadDesktop(), "Switch failure must not report success");
    fail_switch = false;
    require(GetThreadDesktop(GetCurrentThreadId()) == before_failures, "Failed switch must preserve current desktop");
    require(handle_count() == baseline + 1, "Failed switch must close every unused new handle");
    for (int i = 0; i < 13; ++i) require(platf::syncThreadDesktop(), "Switch and retire path must succeed");
    bypass_compare = false;
    require(handle_count() == baseline + 1, "Successful replacement must close each old handle");
    require(platf::syncThreadDesktop(), "Helper must remain usable after failures");
  });
  worker.join();
  require(close_failures == 0, "No CloseDesktop call should target an associated handle");
  require(handle_count() == process_baseline, "Thread exit must restore initial desktop and close owned handle");
  std::cout << "PASS mode=" << (missing_initial ? "no-initial" : missing_compare ? "no-compare" : "normal")
            << " process_handles=" << process_baseline << "->" << handle_count() << '\n';
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[2]
    source = (repo / "src/platform/windows/misc.cpp").read_text(encoding="utf-8")
    begin = source.index("  HDESK syncThreadDesktop() {")
    end = source.index("\n  void print_status(", begin)
    helper = source[begin:end]
    with tempfile.TemporaryDirectory(prefix="vibepollo-desktop-ownership-") as directory:
        cpp = pathlib.Path(directory) / "verify.cpp"
        exe = pathlib.Path(directory) / "verify.exe"
        cpp.write_text(HARNESS.replace("// PRODUCTION_HELPER", helper), encoding="utf-8")
        command = [args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", str(cpp), "-luser32", "-o", str(exe)]
        print("Compiling the exact production helper", flush=True)
        subprocess.run(command, check=True, timeout=60)
        for mode in ([], ["--without-compare"], ["--without-initial"]):
            subprocess.run([str(exe), *mode], check=True, timeout=20)


if __name__ == "__main__":
    main()
