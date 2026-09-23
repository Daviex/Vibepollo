# PyroWave: handoff for the subsequent client phase

The client application is the next phase. The server now contains the extended
profile and partial-frame transport implementation. The original SDR 4:2:0
qualification is historical evidence for commit `205c510c`; it does not qualify
HDR, 4:4:4, transport v2, concurrency or the new platform backends.

## Sources of truth

| Concern | Reference |
| --- | --- |
| Extended discovery, profiles, PWPF fragments and recovery | [Protocol v2](pyrowave-protocol-v2.md) |
| Legacy negotiation and complete-frame PWVF transport | [Protocol v1](pyrowave-protocol-v1.md) |
| Implemented capabilities and remaining qualifications | [Capability matrix](pyrowave-capability-matrix.md) |
| Extended build evidence and limitations | [Extension build report](pyrowave-extension-build.md) |
| Work and Vikunja mapping | [Server plan](pyrowave-server-plan.md) |
| Exact profile parsing and enumeration | `src/pyrowave_profile.h` |
| Accepted session configuration and bitrate budgets | `src/pyrowave_negotiation.h/.cpp` |
| Native framing and bounded partial reassembly | `src/pyrowave_protocol.h/.cpp` |
| UDP, Reed–Solomon and AES-GCM construction | `src/pyrowave_transport.h/.cpp` |
| Historical SDR GPU/RTSP evidence | [GPU report](pyrowave-gpu-validation.md), [RTSP report](pyrowave-rtsp-validation.md) |
| Historical SDR fixtures and reference decoder | [Fixture guide](../tools/pyrowave/fixtures/README.md) |

## Connection sequence

1. Use the existing paired HTTPS authorization flow. Add `pyrowave=1` to
   server-info discovery. Match the pinned bitstream revision. New clients read
   `PyroWaveProfileNegotiationVersion`, `PyroWaveProfiles` and
   `PyroWaveProfileProbeRequired`; the legacy fields remain available.
2. Select an advertised profile. A profile denotes input precision, chroma,
   range, siting, primaries, matrix and transfer independently. Advertisement
   describes implemented choices; ANNOUNCE performs a profile-specific probe.
   The probe uses a small image; real-size allocation can still fail explicitly.
3. Add `pyrowave=1` to launch/resume. Keep application permissions, keys, audio
   and control setup. Use codec `3` and all PyroWave attributes in ANNOUNCE.
   Chroma, dynamic range and CSC attributes must agree with the selected profile.
4. Select version `2` for PWPF fragments and all extended profiles. Version `1`
   retains only `sdr-bt709-full-left-420` and complete-frame PWVF transport.
   Follow the exact layouts in the protocol documents; v2 has no GameStream
   short-frame header before the PWPF data slots.
5. Continue RTSP setup, UDP pings, encrypted control and audio. Authenticate
   shards, recover FEC and validate fragments before calling the native decoder.
6. Codec/profile changes require reconnection. Existing host bitrate updates can
   adjust the encoder budget during a session, within the initial negotiated
   limit, without recreating the encoder. Requests too small for the transport
   overhead are rejected. There is no new unauthenticated bitrate control API.

## Decoder rules

- Preserve native packet boundaries; these bytes are not H.264/HEVC Annex-B or
  AV1 OBUs. Match both the negotiated profile and native color VUI. The bundled
  runtime now writes the actual five VUI fields into native packets.
- Input `p16` is normalized 16-bit input storage, not a P010 layout or a native
  bitstream bit-depth flag. PyroWave transforms floating-point samples.
- PWPF repeats the native sequence header and frame identity in each data slot.
  Reconstruct complete native packets from records. A lost fragment makes that
  native packet unavailable; other complete packets remain usable.
- The manifest carries all five critical-packet counts and the active-block mask.
  Use upstream decoder APIs for available packets and missing blocks. The server
  includes a bounded reference reassembler, not a finished client renderer.
- Validate counts, offsets, packet consistency and allocation ceilings. Reject
  conflicting duplicate records. Cap incomplete-frame lifetime and memory.
  The negotiated transport budget is stricter than the absolute 4 MiB ceiling.
- With encryption, authenticate every shard before interpreting video headers.
  Compare the untrusted clear encryption prefix with authenticated frame fields.
  Observe each FEC block's actual percentage: critical data can use stronger FEC.
- Frame indices and presentation time in PWVF/PWPF are 64-bit; time is in
  microseconds from a host monotonic origin. RTP uses 90 kHz. Neither is a
  wall-clock or a direct one-way latency measurement.
- All frames are intra. IDR/reference-invalidation requests do not need repair
  of an inter-frame reference chain. The client still needs loss concealment,
  color-managed presentation and correct HDR control metadata handling.

## Platform and runtime requirements

The feature is optional (`SUNSHINE_ENABLE_PYROWAVE=ON`) and disabled by default
at runtime (`pyrowave_enabled`). Ship the exact bundled runtime and licenses:

| Host | Runtime | Input/capture path |
| --- | --- | --- |
| Windows x64 | `libpyrowave-shared-0.dll` beside the executable | D3D11 capture and conversion, Vulkan encoder; matching capture LUID |
| Linux 64-bit | `libpyrowave-shared.so.0` beside the executable or installed library directory | System-memory capture/conversion and GPU upload; Vulkan encoder |
| macOS | `libpyrowave-metal.0.dylib` in app `Frameworks` or installed library directory | AVFoundation BGRA8 capture/conversion; Metal encoder on supported Apple GPU |

Windows and Linux implement 128 profile combinations; Metal implements 64 p8
combinations because the pinned Metal API accepts only 8-bit planes. Windows
scRGB capture preserves HDR before conversion. Linux native HDR requires an
unambiguous KMS RGB10/PQ/BT.2020 source with supported color state. Other Linux
capture paths remain SDR. macOS currently converts SDR capture even when the
output transfer is PQ; native desktop HDR capture is not implemented there.

`pyrowave_precision` selects auto, FP16, FP32 math with FP16 storage, or FP32.
It is independent of p8/p16 input. Changing it after the first encoder requires
restarting the host. Linux can select an encoder GPU by `pyrowave_device_uuid`;
with the CPU upload path, it need not be the display GPU.

Windows/Linux retain contexts per selected GPU and admit multiple encoders.
Calls into the Vulkan C API are serialized to protect global dispatch and shared
resources; this is not a promise of simultaneous GPU execution or throughput.
Device loss or malformed native output poisons the affected context until restart.
Ordinary unsupported-profile or allocation admission failures do not invalidate
other encoders. Metal retains its selected default device. Every loader checks
API 0.5.0 and the exact additive contract in `src/pyrowave_runtime_contract.h`.

Disable `pyrowave_enabled` to stop using the codec under the existing configuration
application rules. No automatic fallback changes an active session's codec.

## Evidence still required

The build report records only the compilation actually completed. New GPU color
qualification, HDR luminance checks, loss/cipher streaming, multi-session stress,
Linux/ARM full-host qualification and macOS build/device qualification remain
open in Vikunja. The earlier fixtures and test reports cover the original SDR
contract; their old rejection expectations must be revised when the extended
paths are explicitly tested. No new tests were added or run for this extension.
