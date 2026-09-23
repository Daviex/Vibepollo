# PyroWave — extended implementation build report

Date: 23 September 2026. Branch: `codex/pyrowave-server`.

This report covers the PW-X extension after the original SDR commit `205c510c`.
It records compilation and static inspection, not GPU or streaming qualification.
The earlier 71 component checks, RTSP transcripts and GPU results belong to the
previous implementation. No new tests were added or run for this extension.

## Source and runtime identity

- Upstream PyroWave: `d2997ac172bdc00e29c58e3f2938acb7e94580bf`, API 0.5.0.
- Patch SHA-256: `8f3b4ee860c114a753b3c32bff62fca44a915df94080e0916cfa494372ea36d2`.
- Exact additive loader contract: `src/pyrowave_runtime_contract.h`.
- Patch contents and earlier ownership evidence: `cmake/patches/README.md`.

Upstream bitstream syntax remains pinned. The patch writes the selected VUI,
adds checked CPU planar input and precision configuration, protects ownership
and allocations, and permits serialized activation of multiple Vulkan devices.
Metal has its own input limits; see the capability matrix.

## Compilation evidence

| Artifact | Result | Evidence and scope |
| --- | --- | --- |
| Windows PyroWave shared library | PASS | Final patched dependency compiled as part of the host build |
| Windows host, PyroWave ON | In progress | `build/pyrowave-server-on/extension-build.log` |
| Windows host, PyroWave OFF | Pending | Required after the common source changes |
| Windows conversion shaders | PASS, no warnings | `build/pyrowave-shader-build/compile.log`; three D3DCompileFromFile entrypoints |
| Linux PyroWave shared library | PASS | 59 Ninja steps; CMake 3.25.1, GCC 12.2.0 in WSL Debian x86-64 |
| Linux independent C++ objects | PASS | Color conversion, CPU runtime, protocol, negotiation; GCC 12.2 C++23 |
| Linux full host | Not built | Missing full-host dependencies and C++ standard library `<format>` |
| Linux ARM64 | Not built | Architecture-specific compilation/device qualification pending |
| macOS host and Metal library | Not built | No macOS SDK/toolchain/device available locally |

Windows uses MSYS2 UCRT64 GCC 15.2, CMake 4.3.1 and Ninja. The configuration is
Release, `SUNSHINE_ENABLE_WEBRTC=OFF`, `BUILD_TESTS=OFF`, `BUILD_DOCS=OFF`, with
virtual-display driver/probe/tools/layer builds disabled. These are the same
isolated directories described in the historical build report.

The final HLSL compilation produced `main_vs` 1260 bytes, `main_y_ps` 5104 bytes
and `main_uv_ps` 42824 bytes. Compilation executes only the compiler, not capture
or shader work on the GPU. An FXC warning on branch returns was removed by using
an initialized return value. The host compiler also identified a COM constness
error in the cached shader blobs; their storage was corrected before rebuilding.

The Linux runtime is ELF x86-64, SONAME `libpyrowave-shared.so.0`, with all seven
additive C entrypoints exported and the expected contract present. Its SHA-256
is `109cbefd71b0e2a5bb7f584150668833c1c615a034691fff9a11d14bd7a7047e`.
Local logs and dependency inventory are under `build/pyrowave-linux-compile`.
The library was not loaded or executed on Linux.

## Reproduction

Build the server using the options above and:

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;'+$env:PATH
cmake --build build/pyrowave-server-on --target sunshine -j 4 -- -d keeprsp
```

The optional source archives and patch are hash checked before compilation.
Linux/macOS use the same optional feature switch; Linux selects the Vulkan C API
and macOS the upstream `metal` subdirectory. The host dynamically loads the
runtime and checks its exact contract before creating a device.

## Remaining qualifications

- Actual extended profile encode/decode and luminance/chroma measurements on GPU.
- Real v2 FEC, encryption, packet loss, partial decoder and ABR streaming.
- Multiple sessions/devices, memory pressure and device-loss behavior.
- Full Linux host, Linux ARM64 and macOS compilation/device validation.
- KMS HDR behavior with real compositor/driver color pipelines; ambiguous hardware
  transforms are rejected rather than incorrectly tagged as PQ.
- Native macOS HDR capture needs a separate ScreenCaptureKit capture backend;
  the current AVFoundation path remains SDR even when encoded as PQ.
- Full MSI/DMG packaging, code signing, sustained performance and the client app.

The server remains experimental and disabled by default. These residual items
are recorded in Vikunja PW-X01–PW-X11; a successful build does not close them.
