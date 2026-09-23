# PyroWave — extended implementation build report

Date: 23 September 2026. Branch: `codex/pyrowave-server`.

This report covers the PW-X extension after the original SDR commit `205c510c`.
It records compilation and static inspection, not GPU or streaming qualification.
The earlier 71 component checks, RTSP transcripts and GPU results belong to the
previous implementation. No new tests were added or run for this extension.

The current user-requested server scope is **Windows only**. Linux and macOS
results below are retained as historical compilation evidence. Further work or
qualification on those platforms is outside the current objective and is not a
condition for completing the Windows server implementation. The client remains
a subsequent phase.

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
| Windows host, PyroWave ON | PASS, exit 0 | Final portability rebuild: `build/pyrowave-server-on/extension-build-latest.log`, including the `sunshine.exe` link |
| Windows host, PyroWave OFF | PASS, exit 0 | `build/pyrowave-server-off/extension-build.log`, including the `sunshine.exe` link |
| Windows conversion shaders | PASS, no warnings | `build/pyrowave-shader-build/compile.log`; three D3DCompileFromFile entrypoints |
| Linux PyroWave shared library | PASS | 59 Ninja steps; CMake 3.25.1, GCC 12.2.0 in WSL Debian x86-64 |
| Linux six C++ objects | PASS | KMS capture, CPU device, color conversion, CPU runtime, protocol, negotiation; GCC 12.2 C++23 |
| Linux full host | Not built; outside current scope | Missing full-host dependencies and C++ standard library `<format>` |
| Linux ARM64 | Not built; outside current scope | Any architecture-specific compilation/device qualification is future work |
| macOS Metal shared library | PASS, compiled and linked | [Apple Silicon CI on commit `724d4c67`](https://github.com/Daviex/Vibepollo/actions/runs/35857794571/job/107170376071): `libpyrowave-metal.0.5.0.dylib` linked successfully |
| macOS full host | First CI build failed; outside current scope | The same job stops in `nvhttp.cpp` because `has_stream_session_activity` is undeclared outside Windows; no successful host link is recorded in that run |

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

The macOS result distinguishes the optional codec library from the full host.
The CI log records the Metal library link before the unrelated host portability
error. The library result does not establish a linked macOS server or Metal
device execution. These historical results do not impose a macOS/Linux build
requirement on the current Windows-only scope.

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

## Remaining Windows qualifications and subsequent work

- Actual extended profile encode/decode and luminance/chroma measurements on GPU.
- Real v2 FEC, encryption, packet loss, partial decoder and ABR streaming.
- Multiple sessions/devices, memory pressure and device-loss behavior.
- Full MSI packaging, code signing, sustained Windows performance and the
  subsequent client app.

Future platform work, outside the current objective, includes full Linux/macOS
host builds, Linux ARM64, KMS HDR compositor/driver behavior, native macOS HDR
capture, Metal p16 input, and DMG packaging. Existing partial implementations
and successful library/object builds do not qualify those platform features.

The server remains experimental and disabled by default. The Windows
qualification and subsequent client items remain separate from implementation
and are tracked in Vikunja; a successful build alone does not close them.

## Linux object compilation details

All six final translation units compiled with real dependency headers, C++23 and
without permissive compiler flags or compatibility shims. The four independent
units emitted no warnings. CPU device warnings are in shared headers; KMS
warnings are in shared headers and preexisting lines outside the PyroWave change.
Two existing portability issues were fixed: the qualified `nvenc::split_encode_mode`
type/member declaration and the missing `mem_type_e::vulkan` enumerator (appended
so existing numeric values stay unchanged). Source/header/object SHA-256 values
and exact commands are in `build/pyrowave-linux-compile/host-object-build.json`.
Build dependencies were extracted locally; no global Linux packages were installed.

## Isolated Windows staging

The three CMake install components `application`, `assets` and `Unspecified`
completed into `build/pyrowave-portable-extended`. The 209-file directory includes
the new conversion shader and the extended DLL/contract/license manifest. It was
not installed as a service or started. The executable rebuilt after commit
`d132eaf75b078262abfb202c5962c2a37592eb8c` has SHA-256
`8baaf8d68f7679d470c1512f3ce4af34759c0d33702cc4a0ed1e2c7b835eff5c` and its DLL
`a3cd076eea4465493374318dae4546c6211648dff439903b6b9275ebce4f1322`.
The full 209-file manifest is
`build/pyrowave-server-on/extension-portable-manifest.json`; the successful
incremental build log is `extension-build-source-d132eaf7.log`. These hashes
identify the artifacts; the local build's embedded version retains its configured
dirty-build metadata and is not asserted to be a clean Git release identifier.

The existing negotiation tests had only their old limit/version parameters
updated to the new contract; no test cases were added, compiled or executed.
