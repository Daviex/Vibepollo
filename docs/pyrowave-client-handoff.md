# PyroWave: handoff for the subsequent client phase

The client application is the next phase. This guide collects the implemented
experimental server contract, fixtures and verification entry points. Windows
OFF/ON builds, component checks, real DXGI/WGC capture, isolated RTSP/UDP reference
decoding and repeated resource-lifecycle checks passed on the hardware recorded
below. Consult Vikunja PW-S01–PW-S11 and the reports for exact evidence and scope.

The final local package is under `build/pyrowave-portable-on`; reproduction and
its manifest are described in the build report. It has not been installed or
published. Sustained game workloads, other GPUs/drivers and client presentation
still require qualification; historical timing samples are labeled separately
from final-runtime regression results.

## Sources of truth

| Concern | Reference |
| --- | --- |
| Discovery, launch, ANNOUNCE, profile, errors, payload layout and limits | [Protocol v1](pyrowave-protocol-v1.md) |
| Exact dependencies, build commands and isolated packaging | [Build validation](pyrowave-build-validation.md) |
| GPU, driver, color checks and measured timings | [GPU validation](pyrowave-gpu-validation.md) |
| UDP/FEC/encryption checks and their scope | [Transport validation](pyrowave-transport-validation.md) |
| Work, acceptance criteria and Vikunja mapping | [Server plan](pyrowave-server-plan.md) |
| Header layout and budget calculation | `src/pyrowave_protocol.h/.cpp` |
| Accepted session configuration | `src/pyrowave_negotiation.h/.cpp` |
| Production UDP payload, Reed–Solomon and AES-GCM construction | `src/pyrowave_transport.h/.cpp` |
| Receiver assertions and loopback examples | `tests/unit/test_pyrowave_transport.cpp` |
| Real RTSP startup, rejection transcripts and server UDP checks | [RTSP validation](pyrowave-rtsp-validation.md) |
| Persisted synthetic PWVF/native packets, hashes and offline decoder commands | [Fixture guide](../tools/pyrowave/fixtures/README.md) |

## Connection sequence

1. Use the existing paired HTTPS connection and authorization flow. Add
   `pyrowave=1` to server-info discovery. Require all four PyroWave fields and an
   exact match for protocol version, pinned bitstream revision and profile.
2. Add `pyrowave=1` to the normal launch/resume request and request SDR. Keep the
   existing application identifier, permissions, keys, audio and control setup.
   A failed capability probe is an explicit failure, not a standard-codec fallback.
3. Supply codec `3` and all extension attributes in ANNOUNCE, alongside the normal
   GameStream attributes. Use the MTU that the client actually intends to honor.
   Host packet-size restrictions must be satisfied by the request itself.
4. Continue the existing RTSP setup, UDP pings, encrypted control and audio flows.
   Decode only after video decryption, FEC recovery and complete-frame reassembly.
5. Reconnect with a new launch/resume and ANNOUNCE to change codec, bitrate or
   profile. Version 1 does not negotiate those changes inside a running session.

## Decoder rules

- Preserve native packet boundaries from PWVF length records. Do not parse the
  compressed bytes as Annex-B, H.264 NALs, HEVC NALs or AV1 OBUs.
- Validate outer sizes/counts before allocating. Validate every native record and
  reject truncation, trailing bytes and unsupported versions. The negotiated
  transport budget takes precedence over the absolute 4 MiB parser ceiling.
- Treat negotiated BT.709 full-range 4:2:0 with left chroma siting as authoritative.
  The pinned packetizer's default color metadata does not describe this profile.
- Match frame identity across blocks. With encryption, authenticate each shard
  before interpreting its video headers. The existing clear-text encryption
  prefix's frame index is not AAD; compare it with the authenticated NV header.
- Keep reassembly bounded. Deduplicate shard indices, tolerate reordering, recover
  within Reed–Solomon parity, and discard an incomplete frame after its deadline.
  The next frame is independent; version 1 does not expose partial native recovery.
- The PWVF index is 64 bit. Native/RTP/NV counters have smaller ranges. RTP uses
  90 kHz; PWVF presentation time uses microseconds from a host monotonic origin.
  Neither timestamp is a wall-clock time or a direct one-way latency measurement.
- IDR/reference-invalidation requests do not require inter-frame codec repair.
  Every frame is independently decodable.

## Experimental server setup and rollback

Build for Windows x64 with `SUNSHINE_ENABLE_PYROWAVE=ON`, keeping the packaged
`libpyrowave-shared-0.dll` next to the executable and its licenses in the package.
Enable `pyrowave_enabled` in the server settings, save, and use the availability
check. A compatible Vulkan device, matching capture adapter and SDR desktop are
required. The feature remains disabled by default.

Disable `pyrowave_enabled` to return future sessions to the standard selection
flow. Once the setting is applied, an active PyroWave capture stops; it never
changes codec silently. Existing deferred-configuration rules still apply.

One PyroWave session is admitted at a time. Other GPU/driver combinations, HDR,
4:4:4, non-Windows hosts and browser decoding are not qualified by the current
checks. Game workload contention and client presentation latency require their
own measurements during subsequent validation.

The experimental Windows runtime keeps one PyroWave device and its DLL for the
process lifetime, bound to the first successfully selected capture LUID. Session
encoders, images and fences still have their own lifetime. Changing the GPU after
initialization, or recovering from a lost/poisoned Vulkan device, requires an
explicit server restart. This bounds retained contexts and avoids repeated device
creation on the tested driver, which also leaked handles in a bare Vulkan control.
Disabling the feature stops its use; process shutdown releases the retained device.
An initialization failure also requires a restart before retrying. Use the bundled
runtime: the loader checks both API 0.5.0 and VibePollo's exact revision/ownership
contract, since an arbitrary upstream DLL with the same API number is insufficient.
