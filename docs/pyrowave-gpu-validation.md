# PyroWave Windows GPU validation

> **PW-X extension status (23 September 2026):** This document records the original SDR 4:2:0 phase at commit `205c510c`. Its limits, hashes and test results are historical. For the extended implementation, use the [capability matrix](pyrowave-capability-matrix.md), [protocol v2](pyrowave-protocol-v2.md) and [extension build report](pyrowave-extension-build.md). The earlier results do not qualify HDR, 4:4:4 or the new transport/runtime paths.


Date: 2026-09-23. This records local synthetic GPU checks, actual Windows desktop
capture/conversion checks, and offline reference decoding of synthetic fixtures.
It does not establish client presentation or a production streaming session.

## Environment and scope

- GPU: NVIDIA GeForce RTX 3080 Ti, DXGI LUID `0:10e3bbe` for this Windows session.
- Windows display driver: `32.0.16.1714`.
- Compiler: MSYS2 UCRT64 GCC 15.2, Release, C++23.
- PyroWave: `d2997ac172bdc00e29c58e3f2938acb7e94580bf`, C API 0.5.0.
- Granite: `9d44761debb9ac31d8d800cac8b030a7a0390b7e`.
- Local NT handle ownership patch SHA-256:
  `ba9f00d2fda290d4fd93d5792b3f08100eb12e9e8fb92add5012a2e76302fc8d`.
- Tool: `tools/pyrowave_gpu_smoke.cpp`, built through `tools/pyrowave/CMakeLists.txt`.

The tool creates D3D11 input textures on the named GPU, imports their NT handles
into the production PyroWave runtime adapter, uses the production shared-fence
and packetization path, and decodes every complete frame using the upstream
reference decoder. It checks every decoded sample against the synthetic source.
The first frame also runs through the upstream CPU-input encoder on the same GPU
as an independent control for the external-image path. CPU-input encoding is a
test control; it is not a production fallback or a software codec.

The synthetic checks cover image sharing, plane selection, repeated fence values,
encoding, packet boundaries, reference reconstruction and actual output budgets.
By themselves they do not cover WGC/DXGI desktop capture, RGB conversion, crop/rotation,
letterboxing, HDR-to-SDR behavior, PWVF/UDP/FEC/encryption, display mode changes,
multiple simultaneous sessions, driver loss or client presentation. The constant
and gradient sources are not representative game workloads. The full server
validation tasks remain necessary. The additional real-capture check below
extends coverage to the actual factory and conversion shaders.

## NV12 finding and corrective change

The first implementation imported a shared `DXGI_FORMAT_NV12` texture directly.
On this host it produced corrupt chroma even with a constant source and a
3,000,000-byte frame budget:

| Comparison | Y mean absolute error | Cb mean absolute error | Cr mean absolute error |
|---|---:|---:|---:|
| CPU-input encode/reference decode vs source | 0 | 0 | 0 |
| Direct D3D11 NV12 input vs CPU-input result | 0 | 8.403 | 13.940 |

The failed external-input image contained chroma values from 0 to 255 despite a
constant source. Increasing the budget did not fix it. This isolated the
external NV12 input route from the lossy codec itself; it does not establish the
exact underlying driver defect. The acceptance tolerance was not relaxed.

The production path now renders directly into two non-planar shared textures:
`R8_UNORM` luma at full resolution and `R8G8_UNORM` chroma at half width/height.
The existing D3D11 conversion shaders write those render targets. Both resources
participate in Vulkan acquire/release with the same D3D11 timeline fence. The
API's R/G swizzles expose Cb and Cr separately. This introduces no pixel readback
and no extra full-frame conversion or copy.

The converted profile remains SDR BT.709, full range, 8-bit, 4:2:0, left chroma
siting. PWVF metadata is authoritative because the pinned upstream packetizer
currently writes default color metadata.

## Historical synthetic measurements after the plane correction

The measurements in this section were collected after switching to separate
R8/RG8 planes, before the final context-retention and NT-handle-ownership changes.
They used the same pinned upstream codec revision, but are **not benchmarks of
the final packaged DLL**. Final-DLL functional regression is recorded separately
below. All runs below passed. The first frame is excluded from timing percentiles.
MAE is the largest mean absolute error of any plane in any tested frame, measured
in 8-bit code values. GPU/CPU MAE compares the first external-input reconstruction
with the CPU-input control and was zero in every run.

| Source | Frames | Budget bytes/frame | Actual mean / maximum bytes | Plane MAE | GPU/CPU MAE |
|---|---:|---:|---:|---:|---:|
| 64×64 constant | 8 | 65,536 | 221 / 228 | 0.000 | 0.000 |
| 1280×720 constant | 30 | 500,000 | 3,736 / 4,016 | 0.000 | 0.000 |
| 1280×720 gradient | 30 | 500,000 | 115,374 / 120,972 | 0.056 | 0.000 |
| 1920×1080 gradient | 60 | 416,664 | 264,241 / 283,852 | 0.056 | 0.000 |
| 3840×2160 gradient | 60 | 416,664 | 416,621 / 416,664 | 0.261 | 0.000 |

| Source | Encode + packetize p50 / p95 ms | Synthetic upload + encode p50 / p95 ms | Reference decode + readback p50 / p95 ms |
|---|---:|---:|---:|
| 1280×720 constant | 0.777 / 1.031 | 1.002 / 1.513 | 0.646 / 0.806 |
| 1280×720 gradient | 0.744 / 0.919 | 0.971 / 1.444 | 0.599 / 0.759 |
| 1920×1080 gradient | 0.968 / 1.159 | 1.326 / 1.688 | 1.011 / 1.095 |
| 3840×2160 gradient | 1.331 / 1.776 | 2.377 / 2.791 | 3.243 / 3.777 |

Encoder initialization took 535–585 ms in these runs; reference decoder
initialization took 117–132 ms. First encodes were 3.660–4.306 ms after driver
shader caches had been warmed by earlier investigation. The initial failed NV12
run observed a first encode of 335 ms. These numbers are wall-clock measurements
with application overhead, not isolated GPU shader timings or end-to-end latency.

The 416,664-byte budget corresponds approximately to 200 Mbit/s at 60 frames/s
for codec bytes only. It does not include PWVF, transport, FEC or encryption
overhead and is not a recommendation for a negotiated wire bitrate.

Local run logs are under `build/pyrowave-gpu-smoke/result-*.txt`.

A copy of the harness executable placed in an otherwise empty output directory
also failed cleanly with `Cannot load libpyrowave-shared-0.dll beside the
executable (Windows 126)`. This checks the optional loader's missing-file error;
it is not a substitute for verifying normal Sunshine startup without the DLL.

## Reproduction

Build the optional dependency first to obtain its staged headers and pinned DLL.
Use the current ON build outputs, including the local ownership patch:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
cmake -S tools/pyrowave -B build/pyrowave-gpu-smoke -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DPYROWAVE_API_INCLUDE="$PWD/build/pyrowave-server-on/_deps/pyrowave/stage/include" `
  -DPYROWAVE_RUNTIME_DLL="$PWD/build/pyrowave-server-on/libpyrowave-shared-0.dll"
cmake --build build/pyrowave-gpu-smoke -j 2
build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe --width 1280 --height 720 --frames 30 --target-bytes 500000 --pattern constant
build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe --width 1280 --height 720 --frames 30 --target-bytes 500000 --pattern gradient
build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe --width 1920 --height 1080 --frames 60 --target-bytes 416664 --pattern gradient
build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe --width 3840 --height 2160 --frames 60 --target-bytes 416664 --pattern gradient
```

Use `--adapter N` to select a different enumerated DXGI adapter explicitly. The
runtime verifies that a compatible Vulkan device has the same LUID. Adapter
indices and LUIDs are session-specific, not persistent hardware identifiers.

The harness returns nonzero on initialization, synchronization, packetization,
budget, decode or sample-validation failure. It accepts at most 600 frames and
8192×8192 even dimensions; server negotiation imposes its own stricter limits.
The runtime uses a 3-second GPU completion wait before packetization. Upstream
driver calls and destruction can still block internally; this test does not
prove watchdog-bounded teardown after device loss.

## Actual desktop capture and server conversion

`tools/pyrowave/capture_smoke.cpp` links the completed server's objects and exact
libraries. `build_capture_smoke.py` copies and renames the server main symbol;
the original entry point is never executed and original build objects are not
modified. The harness calls `platf::display`, the real PyroWave device factory,
the existing RGB conversion, the production runtime and the reference decoder.
It initializes only shader compilation and local logging, without loading or
writing a user configuration, listening on ports, installing a service, or
applying display/GPU preferences. WGC starts its normal capture helper.

In the initial capture implementation, each backend ran one warmup phase
(3 frames), followed by three fresh
display/encoder/decoder lifetimes (8 frames each). Encoder output sizes were
2560×1440, 1280×720 and 2560×1440. The desktop itself remained 2560×1440, SDR,
119.998 Hz. This validates output-resource resizing and orderly recreation;
it does **not** validate a physical display mode transition or driver loss.

The first real frame of every phase is independently read back **only in the
harness**. A CPU oracle evaluates the BT.709 full-range matrix, left chroma
siting, bilinear taps and downscaling chroma filter. For scRGB it evaluates the
same documented fast sRGB curve after filtering. Production encoding does not
read back pixels. No desktop image or desktop bitstream is written to disk.

The current SDR display exposed FP16 scRGB through DXGI duplication and BGRA8
through WGC. The initial oracle accepted only BGRA8 and correctly refused the
FP16 fixture; extending the oracle resolved that limitation without changing
the production profile or acceptance threshold.

Historical capture runs (`result-ddx-final.txt`, `result-wgc-final.txt` in
`build/pyrowave-capture-smoke`; their filenames refer to that investigation stage,
before the final context cache, ownership patch and desktop-handle correction):

| Backend / source | Output phase | Fresh / encoded frames | Y / Cb / Cr oracle MAE | Conversion + encode p50 / p95 ms |
|---|---|---:|---:|---:|
| DXGI / scRGB FP16 | 2560×1440 | 3 / 8 | 0.0391 / 0.0754 / 0.0704 | 2.878 / 2.983 |
| DXGI / scRGB FP16 | 1280×720 | 3 / 8 | 0.0390 / 0.0760 / 0.0670 | 2.526 / 2.867 |
| DXGI / scRGB FP16 | 2560×1440 recreated | 3 / 8 | 0.0391 / 0.0755 / 0.0704 | 2.987 / 3.101 |
| WGC / BGRA8 | 2560×1440 | 5 / 8 | 0.0657 / 0.2004 / 0.0488 | 3.879 / 4.789 |
| WGC / BGRA8 | 1280×720 | 2 / 8 | 0.0514 / 0.1736 / 0.0515 | 3.077 / 4.022 |
| WGC / BGRA8 | 2560×1440 recreated | 3 / 8 | 0.0657 / 0.2004 / 0.0488 | 3.339 / 3.700 |

Every phase had real captured content; on an idle desktop the last captured
frame was reused explicitly. Placeholder/blank input was rejected. Capture and
encoder initialization measured 392–884 ms, decoder initialization 88–200 ms.
First conversion/encode is separate from percentiles. These brief tests used a
4 MiB **native codec** budget to isolate conversion fidelity; they do not
validate a negotiated network bitrate or include capture waiting, transport,
FEC, encryption or client presentation. The initial cold run observed 234 ms
for first conversion/encode, while the tabulated runs used warmed driver caches.

In the initial implementation, post-warmup resource growth over three repeated lifetimes was +36 process
handles / 45,707,264 private bytes for DXGI, and +30 / 81,440,768 for WGC. Both
passed the initial gross-growth limits of 128 handles / 128 MiB. Those broad
limits were insufficient: the extended check below exposed linear handle
growth, so the initial lifecycle result was **not accepted**. No WGC helper remained after the
run. A test-only signed/unsigned subtraction bug initially misreported a
decrease in memory as overflow; the metric was corrected before the tabulated runs.

The harness has a 10-second callback deadline per phase and a 60-second own
process watchdog. Forcefully ending that isolated validation process is a test
backstop, not a production guarantee for driver teardown.

### Extended lifecycle investigation and bounded context retention

The extended DXGI run created/destroyed 12 more capture/encoder/reference-decoder
lifetimes after warmup, waiting 100 ms after each teardown. Handles rose
407, 419, 431, ... 551: exactly **12 per cycle**, with no plateau. All images
still decoded correctly. Private bytes oscillated between approximately
354–482 MB rather than increasing monotonically. This was a genuine failure of
the initial resource lifecycle acceptance criterion.

`pyrowave-lifecycle-smoke` then isolated progressively smaller operations:

| Control, 12 cycles after warmup | Handle change per cycle | Result |
|---|---:|---|
| C API device only, DLL loaded/unloaded each cycle | +7 | Growth without encoding or shared image imports |
| C API device only, DLL held for the process | +3 | Retaining the DLL removes one contribution |
| Bare Vulkan instance creation/destruction, no PyroWave/Granite | 0 | Stable at 320 handles |
| Bare Vulkan instance + one device/graphics queue, no extensions | +3 | Same growth without the codec |
| Bare Vulkan device with implicit layers disabled in the test process | +3 | Overlay layers do not explain this contribution |

The bare Vulkan device control used system `vulkan-1.dll`, matched the actual
adapter LUID, created one graphics queue, then called `vkDestroyDevice` and
`vkDestroyInstance`. No resources were imported and no command buffers submitted.
This isolates the +3 contribution to the local Vulkan stack; it does not identify
the exact driver/loader defect. Test environment overrides were process-local
and no installed driver, overlay, registry entry or production process changed.

The production runtime now retains **one** successfully created C API device
and DLL for the process, bound to its first adapter LUID. Session encoders,
imported image views and the shared fence are still destroyed on every stop or
reinitialization. Only one active encoder may use the retained context. There
is no growing map of adapters. A different GPU LUID, failed context, device loss,
initial DLL load failure or ABI mismatch requires a host restart; replacing or
installing the optional DLL after such a failure also requires a restart.

The runtime-only test performs 13 actual import/encode/packetize/destroy cycles
on one D3D device. The final patched-DLL run observed 499 handles after warmup,
one cache step to 501 at cycle 4, and 501 through cycle 12. Private bytes were
185,868,288 after warmup and 189,038,592 at the end. Earlier runs remained at
521 or 523 handles throughout; absolute counts vary between processes. The
criterion requires a plateau, with a last-six range of at most two handles and
at most four handles added after warmup. This tests repeated session resources
independently of the known device-creation contribution.
The capture oracle now similarly retains one reference device while rebuilding
its decoder for each output size, so its own extra Vulkan devices do not
distort the production lifecycle measurement.

Further 13-cycle controls isolated the remaining full-capture behavior:

| Control | Handles after warmup / end | Observation |
|---|---:|---|
| Recreate both shared R8/RG8 targets each cycle | 499 / 499 | No imported-target handle growth |
| Recreate the D3D device and both targets each cycle | 499 / 477 | Stable at 477 after cycle 1 |
| Bare D3D device creation/destruction | 205 / 205 | No handle growth |
| Recreate and use the 720p reference decoder on one device | 351 / 351 | No handle growth; private bytes plateaued around 194 MB |

The first full-capture run after context retention still failed: DXGI added
two handles on every cycle. WGC had no corresponding linear slope. Explicitly
clearing the converter's D3D bindings did not remove the DXGI growth. An isolated
Windows desktop-handle control then reproduced a pre-existing issue in
`syncThreadDesktop()`: `OpenInputDesktop` and `SetThreadDesktop` succeeded,
but closing the now-current desktop returned `ERROR_BUSY` (170). Thirteen calls
added thirteen handles; restoring the original desktop and closing the retained
handles returned the count to baseline. DXGI initialization invokes this helper
twice, accounting for its exact two-handle slope. This finding is separate from
the Vulkan device-creation contribution above and from driver memory caches.

The corrected helper retains one owned desktop per thread, reuses the same
desktop identity, and switches away before closing its handle. This follows the
[Windows CloseDesktop lifetime rule](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-closedesktop).
`python tools/pyrowave/verify_desktop_ownership.py` extracts the production
function and exercises real Win32 calls with controlled failure wrappers.
Normal, missing-comparison-API and missing-initial-desktop modes all passed:
64 repeated calls, 13 open failures, 13 switch failures and 13 replacements,
with process handles returning from 77 to 77 after worker exit and no failed
close. No input events, windows or hooks are created. This also checks the shared
helper used by standard capture; interactive secure-desktop transitions and
threads owning windows/hooks remain outside this isolated test.

The final capture resource baseline is taken after warming native output,
half-size output, then native output again (three frames in each phase). Twelve
measured phases follow, alternating output sizes, for 45 decoded frames per
backend. The acceptance bounds are unchanged: at most four handles above the
baseline, a last-six range of at most four handles, and at most 128 MiB private
memory growth at the end of the run. Both sizes are warmed so the baseline does not count the initial
decoder/allocation cache for an unseen size as session leakage. These bounded
checks still cannot establish the absence of small or long-running leaks.

### Final capture lifecycle results

Both backends passed after the desktop-handle fix, with the final packaged DLL
and rebuilt server objects. Each decoded 45 real-capture frames across 15
display/encoder lifetimes, including the three warmup lifetimes. Output alternated
between 2560×1440 and 1280×720; desktop mode and HDR settings were unchanged.

| Backend | Maximum oracle plane MAE | Handles baseline / final | Private bytes baseline / final | Peak sampled private bytes |
|---|---:|---:|---:|---:|
| DXGI, scRGB FP16 | 0.0333 | 451 / 455 | 559,280,128 / 659,963,904 | 694,468,608 |
| WGC, BGRA8 | 0.0862 | 453 / 455 | 515,739,648 / 584,540,160 | 621,211,648 |

DXGI stayed at 451 handles through measured cycle 6, then at 455 from cycle 7
through 12. WGC stayed at 453 through cycle 9, then at 455 through cycle 12.
The earlier exact two-handle-per-cycle growth is gone. These isolated cache
steps are reported rather than treated as zero change. DXGI private bytes
oscillated between 634–694 MB across the final six samples; WGC between 547–621
MB. End-of-run growth was 100,683,776 and 68,800,512 bytes respectively, within
the unchanged 128 MiB endpoint bound. The peak column is separate from that
endpoint criterion; neither result proves the absence of smaller long-term leaks.

The converter's explicit `ClearState`/`Flush` remains orderly resource cleanup.
The A/B investigation demonstrated that it did **not** repair the linear DXGI
handle leak; the `syncThreadDesktop()` ownership correction did.

Final local evidence is in
`build/pyrowave-capture-smoke/result-final-desktopfix-ddx-lifecycle12.txt` and
`result-final-desktopfix-wgc-lifecycle12.txt`. No desktop frame or bitstream was
persisted. No validation helper remained running afterward, and the existing
production Sunshine process was not stopped or replaced.

The current packaged DLL was compared byte-for-byte by SHA-256 in the server,
dependency stage and validation executable directories:
`d6105cbc2861df660b7116fc04270408aba28adc94db513be7127e9798645f73`.

With that final DLL, the 1280×720 gradient regression encoded and reference-decoded
eight frames at a 416,664-byte budget. Its first-frame GPU/CPU reconstruction MAE
was zero and maximum per-plane source MAE was 0.056. The persisted 64×64 and 720p
fixtures also passed source/native/PWVF hash checks and independent decode hash
verification. These establish functional regression of the final runtime; the
historical timing tables above are not relabeled as final-DLL performance data.

Separate process checks also passed for a changed LUID after successful
initialization, an overlapping encoder, and a first context failure followed
by three retry attempts: retries remained poisoned and explicitly required a
restart, rather than recreating contexts indefinitely.

```powershell
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe vulkan-instance
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe vulkan-device
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe device-unload
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe device-held
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe runtime
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe runtime-new-targets
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe runtime-new-device
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe d3d-device
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe decoder-reconfigure
build/pyrowave-gpu-smoke/pyrowave-lifecycle-smoke.exe runtime-poison
# Run capture commands from the configured server build directory:
../pyrowave-capture-smoke/pyrowave-capture-smoke.exe ddx 12 3
../pyrowave-capture-smoke/pyrowave-capture-smoke.exe wgc 12 3
```

Reproduce from an ON server build using MinGW/Ninja:

```powershell
cmake --build build/pyrowave-server-on --target sunshine -- -d keeprsp
python tools/pyrowave/build_capture_smoke.py --server-build build/pyrowave-server-on --output-dir build/pyrowave-capture-smoke
Push-Location build/pyrowave-server-on
../pyrowave-capture-smoke/pyrowave-capture-smoke.exe ddx
../pyrowave-capture-smoke/pyrowave-capture-smoke.exe wgc
Pop-Location
```

The build must contain the normal shaders under `assets/shaders` and the WGC
helper in `tools/sunshine_wgc_capture.exe`. The helper is copied beside the
harness. The CPU oracle currently requires an existing SDR, unrotated display
whose dimensions divide exactly into the native and half-size even outputs.

## Isolated RTSP and actual UDP sender

`tools/pyrowave/udp_receiver.h` adds a bounded in-memory reference receiver to
the separate real RTSP harness. Following its successful negotiation, it sends
the legacy four-byte video ping to the loopback sender, collects actual RTP
fragments, reassembles complete PWVF envelopes, validates increasing frame
timestamps and decodes three frames using the pinned reference API. Real
capture, server conversion, encoding, PWVF construction and the sender thread
are exercised together. The receiver checks decoded luma detail and retains
neither desktop images nor compressed desktop files.

Parity fragments are counted; this positive loopback receiver requires complete
data fragments and does not implement FEC recovery. Audio/input/controller traffic
is absent. OS callbacks that could apply production display/profile changes are
isolated in that harness. Its `receiver_decode_drain_fps` measures draining and
decoding a small buffered sample, not source capture cadence or client frame
presentation. It establishes neither sustained 60-fps performance nor latency.
See [RTSP validation](pyrowave-rtsp-validation.md) for the final run and
[transport validation](pyrowave-transport-validation.md) for separate loss,
fragmentation and encryption checks. The client application remains a later step.

## Negative checks and runtime metrics

- Missing optional DLL: rejected cleanly, Windows error 126.
- Isolated fake DLL exporting only API version 0.4.0: rejected as incompatible
  before resolving the remaining entry points. The real DLL was untouched.
- Upstream API 0.5.0 DLL without the local contract export: rejected before
  Vulkan device creation. API version compatibility alone is insufficient.
- Isolated API 0.5.0 DLL with an incorrect contract: rejected at the same stage.
- Nonexistent adapter LUID: Vulkan compatibility creation rejected with
  `PYROWAVE_ERROR_NO_VULKAN` (numeric -5), as returned by the pinned API for
  failure to create a matching compatible Vulkan context.
- After that first-context failure, three retries with the real adapter remain
  poisoned and require restart. The runtime does not recreate contexts.
- A different LUID requested after successful initialization is rejected with
  the explicit instruction to restart before changing GPU.
- An overlapping encoder is rejected while the original encoder remains usable
  and completes the synthetic encode/decode sequence.
- Actual server device factory: zero and odd output width rejected before GPU
  codec initialization. Both capture backends subsequently encoded normally.

All four loader-negative checks returned exit status 1 with the specific cause
and the requirement to restart the host before retrying. They ran from isolated
harness directories; no production DLL or system runtime was replaced.

An actually missing system Vulkan loader and missing required GPU features were
not reproduced on this capable host. The nonexistent-LUID test exercises failure
of compatible-device creation only; its `PYROWAVE_ERROR_NO_VULKAN` result does not
establish validation on a machine without Vulkan. No installed driver, system
loader or feature support was removed or replaced for fault injection.

The accepted DLL must export `pyrowave_vibepollo_runtime_contract()` returning
exactly `d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1`.
The pinned patch makes image and semaphore imports consume a Windows NT handle
only after the entire C API operation succeeds. On failure the caller retains
ownership. This avoids a double-close where Granite previously consumed an
image handle before a later allocation/bind failure. The runtime rejects a DLL
without this contract before using imported resources.

`encoder_t::last_frame_statistics()` reports CPU wall time for preparation plus
fence submission, encode plus GPU completion wait, and native packetization.
Encode wait includes preceding D3D conversion dependencies; it is not a pure
codec GPU timestamp. The server device logs means every 300 successful frames,
including native bytes and packet count. The synthetic harness prints those
same counters. Transport-layer timings and drops are reported separately.

Build the intentionally incompatible DLL only on explicit request:

```powershell
cmake --build build/pyrowave-gpu-smoke --target pyrowave-incompatible-runtime
Copy-Item build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe build/pyrowave-gpu-smoke/abi-mismatch/
build/pyrowave-gpu-smoke/abi-mismatch/pyrowave-gpu-smoke.exe --width 64 --height 64 --frames 2
# A second isolated fixture exports API 0.5.0 but an invalid local contract:
cmake --build build/pyrowave-gpu-smoke --target pyrowave-invalid-contract
Copy-Item build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe build/pyrowave-gpu-smoke/contract-mismatch/
build/pyrowave-gpu-smoke/contract-mismatch/pyrowave-gpu-smoke.exe --width 64 --height 64 --frames 2
```

## Synthetic fixtures for client implementation

The synthetic harness accepts `--output-dir EMPTY_DIRECTORY`. It writes only
frame zero of its generated pattern: `frame.pwvf`, separate native packets and
`manifest.json`. The manifest records the pinned bitstream revision, API,
authoritative color profile, dimensions, byte budget, input layout/pattern,
SHA-256 hashes for source/packet/envelope/decoded planes and per-plane MAE.
Existing non-empty directories are rejected. The real desktop harness has no
dump option.

The production PWVF serializer/parser roundtrip is followed by another actual
reference decode before a fixture is accepted. `pyrowave-decode-fixture` reads
the persisted PWVF in a separate process and outputs decoded plane hashes;
`verify_fixture.py` regenerates the synthetic source, verifies hashes and packet
boundaries, then compares the independent decoder output with the manifest.
Its decoder child has a 30-second process timeout. A compatible Vulkan GPU and
the pinned runtime are required; this is not a software-decoder fallback.

Validated fixtures:

- 64×64 constant, budget 65,536: exact source reconstruction.
- 1280×720 gradient, budget 416,664: Y MAE 0.055549, Cb/Cr MAE 0;
  PWVF size 112,200 bytes containing two native packets.

The versioned handoff fixtures are under `tools/pyrowave/fixtures`, with a
README identifying their synthetic source and license. Recheck them directly:

```powershell
python tools/pyrowave/verify_fixture.py --fixture-dir tools/pyrowave/fixtures/64x64-constant --decoder build/pyrowave-gpu-smoke/pyrowave-decode-fixture.exe
python tools/pyrowave/verify_fixture.py --fixture-dir tools/pyrowave/fixtures/720p-gradient --decoder build/pyrowave-gpu-smoke/pyrowave-decode-fixture.exe
```

```powershell
build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe --width 64 --height 64 --frames 8 --target-bytes 65536 --pattern constant --output-dir build/pyrowave-fixtures/64x64-constant
build/pyrowave-gpu-smoke/pyrowave-gpu-smoke.exe --width 1280 --height 720 --frames 8 --target-bytes 416664 --pattern gradient --negative-cases 1 --output-dir build/pyrowave-fixtures/720p-gradient
python tools/pyrowave/verify_fixture.py --fixture-dir build/pyrowave-fixtures/64x64-constant --decoder build/pyrowave-gpu-smoke/pyrowave-decode-fixture.exe
python tools/pyrowave/verify_fixture.py --fixture-dir build/pyrowave-fixtures/720p-gradient --decoder build/pyrowave-gpu-smoke/pyrowave-decode-fixture.exe
```

These fixtures are a version-specific client handoff, not a promise that another
GPU/driver will generate identical encoder bytes. Decode hash comparisons were
validated on this host; cross-vendor precision equivalence remains untested.
