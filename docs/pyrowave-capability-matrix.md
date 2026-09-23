# PyroWave capability inventory and extension acceptance criteria

Audit date: 2026-09-23. Upstream `refs/heads/master` was checked with
`git ls-remote` and is still `d2997ac172bdc00e29c58e3f2938acb7e94580bf`, the
revision pinned by VibePollo. The C API version is 0.5.0.

This document records the gap between that upstream revision and the first
VibePollo server implementation, commit `205c510c9a2aa3d9b37011c4cca0986ebbb65a6c`.
The extension now has server implementation paths for the capabilities below.
Implementation and qualification are separate: a parser, profile declaration,
or successful build alone does not establish that a capability works through
capture, conversion, encoding, transport, and decoding. No new execution tests
are recorded by this audit. The earlier SDR validation reports remain evidence
for their original scope only.

## Source inventory

Primary sources at the audited revision:

- [README](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/README.md): intra-only GPU codec, 4:2:0/4:4:4, exact rate control, supported build environments, and developer tools.
- [Bitstream specification](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/bitstream/bitstream.md): dimensions, floating-point reconstruction, independent color fields, native packets, and missing-block semantics.
- [C API](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/pyrowave.h): device/queue ownership, GPU/CPU buffers, byte limits, packet padding, critical bands, active-block sideband, and partial decoding.
- [Encoder implementation](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/pyrowave_encoder.cpp): allocation requirements, emitted sequence headers, and packet ordering.
- [Shared implementation](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/pyrowave_common.cpp) and [build options](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/CMakeLists.txt): internal precision modes 0/1/2 and padded wavelet allocation.
- [Metal API](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/metal/pyrowave_metal.h) and [Metal notes](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/metal/README.md): Apple implementation and its separate resource interface.

The actual supported chroma choices are **4:2:0 and 4:4:4**. The audited revision
does not define a 4:4:0 or 4:2:2 bitstream mode. Its HDR transfer choice is PQ;
HLG, Dolby Vision, and HDR10+ are not defined bitstream capabilities.

## Server feature matrix

“Implemented” describes the source path reviewed in this extension. It does not
mean the new mode has been exercised on hardware or through a production client.
The last column states the remaining qualification or implementation boundary.

| Capability | Upstream contract | Current server implementation | Remaining evidence / boundary |
| --- | --- | --- | --- |
| 4:2:0 / 4:4:4 | Half-size / full-size chroma planes | Implemented in profile negotiation, Windows GPU conversion, portable CPU conversion, and runtime creation | New 4:4:4 paths, including odd dimensions, need decoded hardware fixtures |
| Input precision | UNORM GPU inputs; floating-point bitstream reconstruction has no 8/10-bit flag | Windows R8/RG8 and R16/RG16; Linux planar p8/p16 upload extension; Metal p8 only | Verify precision retention; Metal explicitly rejects p16 and advertises only its 64 p8 profiles |
| Primaries | Independent BT.709 / BT.2020 bit | Both conversion choices and native sequence-header signaling implemented | Independent gamut/color oracle needed |
| YCbCr matrix | Independent BT.709 / BT.2020 bit | Both matrices implemented independently from primaries/transfer | Verify nonstandard but representable combinations |
| Transfer | Independent BT.709 / PQ bit | BT.709/PQ output and session signaling implemented; source capture differs by platform | Native HDR capture is Windows and restricted Linux KMS; macOS currently converts captured SDR to PQ |
| YCbCr range | Full / limited | Conversion and native signaling implemented for both | New decoded range fixtures needed |
| Chroma siting | Center / left | Both filters implemented; bundled runtime extension writes the chosen native bit | Verify actual sample positions and native bit together |
| Profile combinations | Five independent VUI bits plus chroma; input storage chosen externally | 128 profile values in the contract; Apple capability list filters out p16 | Platform lists describe supported paths, not 128 completed hardware probes |
| Dimensions | 1..16384 per axis; 4:2:0 even | Representability checked; known 512 MiB input-plane limit rejected during negotiation; runtime allocation/adapter checks follow | 16384 is a bitstream limit, not a promise that every 16K profile allocates or performs adequately |
| Fractional frame rates | Pacing outside codec | 240 fps policy cap removed; integer/fractional consistency and signed milli-FPS checked | Sustained rates still require platform-specific measurement |
| Maximum bytes per frame / live rate | Per-call byte ceiling | Shared admission helper for initial negotiation, HTTP control, and encode worker; per-frame immutable native/network budgets | Requests must fit the original admitted encoder ceiling and envelope; new live-session evidence needed |
| Intra-only frames | No inter-frame prediction dependency | Preserved; control IDR requests can repeat the current source image | New transport regression pending |
| Native packetization | Packets can exceed an MTU | V1 PWVF preserved; v2 PWPF fragments native packets into independent UDP data slots | Native packet remains the reconstruction unit; one missing fragment drops that packet |
| Lossy/partial reception | Missing coefficient blocks reconstruct as zero | V2 server fragments and bounded reference reassembler implemented | Production client timeout, presentation, partial decode policy, and network/GPU loss evidence remain pending |
| Critical wavelet bands | Critical counts and active-block masks | Manifest contains five counts plus bands-3 mask; selected critical prefix receives stronger FEC where feasible | Verify masks, selective wire percentages, and decoded output after unrecoverable noncritical loss |
| Native packet padding | Alternate packetizer reserves first-packet space | PWPF provides explicit record/slot framing without requesting upstream native padding | This alternate API is not a separate missing stream mode |
| Internal wavelet precision | Process-wide modes 0/1/2 | Configuration and effective diagnostics implemented through bundled runtime extension; changes after initialization require restart | Compile evidence does not qualify the performance/accuracy matrix; unrelated to p8/p16 input storage |
| GPU execution | Graphics or compute queue integration | Vulkan paths request compute with upstream fallback; upstream diagnostics exposed; Metal uses its native device | No selectable graphics-queue user setting; queue selection is an integration policy, not an additional bitstream mode |
| Device identity / sync | UUID/LUID, external images/semaphores | Windows LUID contexts and NT texture/fence sharing; Linux UUID-selected upload contexts; Metal default GPU identity | New lifetime/failure/adapter-switch evidence needed |
| Multiple sessions / GPUs | Multiple objects with external call synchronization | Windows per-LUID and Linux per-selector contexts; serialized C API access; independent session encoders; Metal shared default device | Concurrent throughput, resource pressure, and multiple actual GPUs are not qualified by object creation |

## Platform coverage and evidence

| Platform path | Captured source and conversion | Supported profile paths | Evidence and limits |
| --- | --- | --- | --- |
| Windows D3D11 → Vulkan | SDR BGRA8 or HDR FP16/scRGB; GPU conversion into shared Y/CbCr targets | All 128 values subject to adapter/image/memory admission | Prior SDR v1 evidence remains valid only for its recorded build; new HDR/4:4:4/precision/partial paths require qualification |
| Linux system memory → Vulkan | Portable BGRA8 SDR conversion/upload; restricted KMS HDR readback into BGRA16 PQ/BT.2020 | All 128 values subject to backend and GPU admission | Native Linux library and selected C++ objects have compile-only records; this is not a linked or GPU-qualified Linux server |
| macOS AVFoundation → Metal | AVFoundation explicitly supplies SDR BGRA8, followed by CPU conversion/upload | 64 p8 profiles, including PQ output generated from SDR | Native HDR capture is not implemented in this path. PQ output does not recover highlights/gamut absent from SDR capture; p16 is rejected. No Apple compiler/hardware execution evidence |

Linux KMS accepts HDR only when it can identify the captured framebuffer as
normalized RGB10 with SMPTE ST 2084 metadata, explicit `BT2020_RGB` connector
colorimetry, and no active KMS hardware LUT/CTM/transfer pipeline. HLG, traditional
gamma HDR, unknown metadata, unsupported fourccs, or ambiguous hardware color
transforms fail explicitly. The EGL import must retain at least ten RGB bits.
Readback uses 16-bit channels, bounds image allocation to 512 MiB, and converts
the premultiplied sRGB cursor through linear BT.709/BT.2020 before PQ blending at
80-nit SDR white. This restriction follows the distinction between framebuffer
data and connector output in the [DRM color-management contract](https://cdn.kernel.org/doc/html/latest/gpu/drm-kms.html).

Compile records are stored under `build/pyrowave-linux-compile/`: the patched
upstream Vulkan library linked in 59 Ninja steps, and the color converter,
portable runtime, protocol, and negotiation translation units have successful
GCC 12 C++23 object-build records. Consult the source/patch identity in those
records when comparing later edits. No executable, GPU, unit, or network test
was run for this audit. Full Linux host dependencies and any additional object
build results are recorded separately in that directory. The old SDR reports
do not qualify the new platform paths.

## Profile contract implemented in the extension

The unchanged version 1 profile name is `sdr-bt709-full-left-420`, with 8-bit
input. Negotiation version 2 accepts all seven choices independently:

```text
yuv{420|444}-p{8|16}-{full|limited}-{left|center}-
{bt709|bt2020 primaries}-{bt709|bt2020 matrix}-{bt709|pq transfer}
```

The line break above is explanatory; real names are single strings, for example:

```text
yuv444-p16-full-left-bt2020-bt2020-pq
yuv420-p16-limited-center-bt709-bt709-bt709
yuv444-p8-full-center-bt2020-bt709-pq
```

The last example is intentionally accepted: the bitstream represents those
fields independently. The converter must implement the described transformation
instead of quietly replacing it with a more usual combination.

`profile_name(profile_t{})` returns the legacy name. The canonical spelling
`yuv420-p8-full-left-bt709-bt709-bt709` also parses in v2 and normalizes to that
legacy name. `profiles()` enumerates 128 distinct profile values using one name
per value. Names are exact, case-sensitive tokens with no ignored suffixes.

Compatibility fields are checked against the explicit profile:

- `dynamicRangeMode` is 1 for PQ and 0 for BT.709 transfer in this private extension.
- `chromaSamplingType` is 1 for 4:4:4 and 0 for 4:2:0.
- `encoderCscMode` describes the matrix and range: 2/3 for BT.709 limited/full,
  4/5 for BT.2020 limited/full. Primaries and transfer come from the profile;
  they are not inferred from this compatibility field.
- Input precision comes from the profile. An SDR profile may use 16-bit input;
  the independent bitstream fields do not prohibit PQ with 8-bit input either.

Negotiation v2 requires the explicit `fragments-v2` transport and its PWPF
slots, specified in [the v2 protocol contract](pyrowave-protocol-v2.md).
Negotiation v1 retains `complete-v1` and the unchanged PWVF envelope. A receiver
must select the wire layout from the negotiation, not infer it from a color
profile. Platform capability lists can be narrower than the full profile model.

The negotiation result reports the selected profile/version and nominal input
plane bytes using 64-bit arithmetic, rejecting values above the host's 512 MiB
input-plane ceiling before encoder creation. Those bytes do **not** include capture
surfaces, wavelet textures, staging images, encoder statistics, or driver
overhead, and are not an assertion that the adapter can allocate them.

## Allocation and arithmetic boundaries

The unmodified upstream Vulkan C API checks positive dimensions and even 4:2:0
dimensions, but its encoder creation does not enforce the bitstream's 16384
limit. The host and bundled runtime patch now check that limit. Upstream aligns
dimensions to 32 with a minimum internal extent of 128. The bundle also adds
allocation-result checks to wavelet/encoder resources and catches allocation
exceptions at the extended C API boundaries. Fault injection and device recovery
remain unqualified; source hardening does not establish every driver failure
path as safe.

At 16384 x 16384, 16-bit 4:4:4 input planes alone occupy 1.5 GiB. The wavelet
pyramid with 12 layers uses roughly 2 GiB with FP16 storage, or roughly 4 GiB
with FP32 storage. Encoder statistics, metadata, scratch data, capture resources,
and temporary encoded buffers add more. These are order-of-magnitude source
estimates, not an exact driver allocation budget. A dimension admitted by the
bitstream parser still needs adapter-specific checks and a safe allocation path.

Host rate arithmetic uses signed milli-FPS, so a requested hundredth-fps rate
must not exceed `INT_MAX / 10`. The integer fps field must remain positive and
agree with the rounded fractional field. Bitrates must be positive signed
integers; byte calculations use 64-bit arithmetic and cap the sender's
representable budget before applying the much smaller transport frame ceiling.
The envelope and FEC/MTU limits remain independent of the codec's dimension and
rate representability.

The shared `negotiation::rate_budget()` helper admits both the initial rate and
live rate changes. It keeps the original negotiated encoder rate and wire
ceiling immutable, scales the wire allowance from that original pair, and
reserves the selected transport/FEC/manifest overhead before deriving the native
byte target. A live rate must be positive, at most the original encoder rate,
and large enough for a valid frame. The control setter checks it before changing
session metadata or raising the rate event; if no matching session can admit the
change, `/bitrate` returns an XML status of 406. Each queued frame retains its own
budgets. The existing control endpoint also applies the host `max_bitrate`
setting and its 500000 kbps absolute ceiling; these are host policies, not
PyroWave bitstream limits. Server-side autonomous ABR decision-making is not
implemented: clients drive changes through this endpoint.

The adapter probe encodes a 64 x 64 dummy image using the requested profile and
checks legacy PWVF serialization. This does not qualify allocation at the
requested session dimensions, PWPF loss behavior, or decoded color accuracy.

## API alternatives, platform coverage, and client phase

These items remain in the inventory so that “all PyroWave capabilities” is not
silently narrowed to color-profile support:

| Upstream surface | Role | Current integration disposition |
| --- | --- | --- |
| CPU NV12/YUV420P/YUV444P entry points | Upload/download convenience around GPU codec execution; not a CPU-only encoder | Linux uses the bundled planar p8/p16 upload extension; macOS uses the portable converter with Metal p8 upload. GPU codec execution is still required |
| Borrowed Vulkan devices / external command buffers / queue callbacks | Alternative integration mechanics for applications already owning Vulkan work | Current Windows backend owns its matched Vulkan context. These are not additional bitstream modes, but alternate platform backends would need to choose and verify their ownership model |
| External image formats / component swizzles | Ways of supplying the same three sampled component planes | Windows chooses separate Y/CbCr shared textures; portable paths upload converted planes. Supporting every external handle type is not required to express a profile; direct NV12 import previously failed the local chroma oracle |
| Compute and fragment decoders | Client decoding paths; fragment path is intended for some mobile GPUs | Existing reference tools cover their recorded SDR scope. New profile/partial output and production client decoder selection remain unqualified and part of the subsequent client phase |
| Partial decode policy and active-block readiness | Client decision of when to present an incomplete frame | Server metadata, fragmentation, and bounded reference reassembly are implemented. Client queue lifetime, presentation policy, and actual partial GPU decode remain next |
| Metal encoder/decoder, IOSurface import | Separate Apple backend, with its own API and GPU requirements | macOS server has an AVFoundation SDR capture, CPU conversion, and optional Metal p8 runtime path for the default Apple7+ GPU. Native HDR capture, p16, direct IOSurface import, Apple build/hardware qualification, and a client are not completed |
| Linux / Android Vulkan builds | Other library build targets and external-memory integrations | Linux system-memory capture/upload and UUID-selected Vulkan contexts are implemented, with restrictive KMS HDR support. Native library/object compilation is recorded separately; full Linux execution and Android integration are not established |
| Encoder/decoder CLI, viewer, evaluator, PSNR tools, benchmark, sample capture | Developer/reference programs | Useful validation tools rather than negotiated codec modes. Their existence is inventoried without treating packaging every sample application as a streaming feature |
| Debug pre-transformed encode / wavelet inspection | Internal C++ research hooks | Not exposed as a product stream mode; any use needs a separate purpose and validation |

The server extension can claim qualified coverage only for rows and platforms
with both implementation and corresponding evidence. Client functionality,
native source capture, input precision, and platform qualification remain
separate boundaries even when the same output profile name is accepted.

## Verification still required for the extension

- Exhaustive round-trip parsing and negotiation for all 128 profile values,
  plus malformed names, v1 compatibility, field conflicts, and unknown versions.
- 4:4:4 odd dimensions; tiny valid 4:2:0/4:4:4 dimensions; upper-bound
  representability and allocation refusal without device poisoning.
- Independent color oracles for BT.709/PQ, both primaries/matrices, ranges,
  sitings, and input precisions; inspect the actual native sequence-header bits.
- Decoded GPU fixtures with more than 8-bit input variation, HDR luminance ramps,
  saturated colors, and high-frequency chroma; avoid validating conversion only
  against another copy of the same shader formula.
- Runtime bitrate changes, sustained per-frame byte ceilings, encrypted and
  unencrypted transport, loss/reordering/duplicates, critical-band sideband,
  partial recovery, and backwards compatibility.
- Build feature ON/OFF, missing/incompatible runtime, allocation faults,
  repeated teardown, and resource lifetime checks for the newly used formats.
- Effective internal precision and queue settings in diagnostics, with any
  unavailable hardware or platform qualification stated explicitly.
