# VibePollo PyroWave protocol, version 1

> **PW-X extension status (23 September 2026):** This document records the original SDR 4:2:0 phase at commit `205c510c`. Its limits, hashes and test results are historical. For the extended implementation, use the [capability matrix](pyrowave-capability-matrix.md), [protocol v2](pyrowave-protocol-v2.md) and [extension build report](pyrowave-extension-build.md). The earlier results do not qualify HDR, 4:4:4 or the new transport/runtime paths.


Status: implemented experimental Windows server contract, validated on the local
hardware and within the limits recorded in the server reports. The client
application is the subsequent phase. Work and evidence are tracked in Vikunja,
**VibePollo → PyroWave — Server**, PW-S01–PW-S11.

## Scope and compatibility

Version 1 carries complete, independently decodable PyroWave frames inside the
existing authenticated RTSP/GameStream video transport. Pairing, launch
authorization, control, input, audio and video encryption retain their existing
roles. Implementing an application client is a subsequent phase.

The standard wire codec values remain H.264=0, HEVC=1 and AV1=2. Value **3** is a
private PyroWave identifier, usable only with the explicit extension below. It is
not a standard GameStream codec assignment or a bit in `ServerCodecModeSupport`.
Unknown identifiers, malformed integers and unnegotiated private codecs fail
before capture starts. A running session never changes codecs implicitly. To fall
back, the client must negotiate a new standard-codec session.

Build and runtime activation are opt-in. Standard clients and WebRTC continue to
use the standard codec paths. A positive PyroWave capability requires a real probe
of the adapter that owns the capture display, including shared R8/R8G8 image import, a shared fence
and an encoded frame. A Vulkan loader or matching vendor name alone is insufficient.

## Upstream compatibility and color profile

The bitstream revision is the complete upstream commit ID:

`d2997ac172bdc00e29c58e3f2938acb7e94580bf`

Both ends must agree on that identifier in addition to protocol version 1. It is
not an upstream bitstream version number. The server uses C API 0.5.0 from this
revision; an incompatible runtime is unavailable. A future dependency update must
explicitly revisit bitstream compatibility and negotiation.

The only initial profile is **`sdr-bt709-full-left-420`**:

- SDR, BT.709 matrix/primaries, full-range YCbCr, 4:2:0.
- Horizontal chroma siting is left; vertical siting is centered, matching the
  existing VibePollo NV12 conversion shader.
- The GPU converter writes separate 8-bit R8 luma and R8G8 chroma textures. PyroWave's coefficients do not encode a fixed
  output quantization depth; the first implementation prepares 8-bit input.
- `encoderCscMode=3`, `dynamicRangeMode=0`, `chromaSamplingType=0` are mandatory.
- HDR, 4:4:4 and a different range/matrix fail negotiation rather than silently
  changing the requested profile.

This negotiated profile is authoritative. The pinned upstream packetizer writes
default BT.709/full/centered metadata and has no C API setter for chroma siting.
A receiver must apply the profile above when converting the reconstructed planes
to RGB. This distinction must be covered by the reference-decoder color fixtures.

## Discovery and negotiation

A future client explicitly opts into discovery using `pyrowave=1` on its existing
paired HTTPS server-info request. A capable, enabled host with a positive adapter probe adds:

```xml
<PyroWaveSupport>1</PyroWaveSupport>
<PyroWaveProtocolVersion>1</PyroWaveProtocolVersion>
<PyroWaveBitstreamRevision>d2997ac172bdc00e29c58e3f2938acb7e94580bf</PyroWaveBitstreamRevision>
<PyroWaveProfile>sdr-bt709-full-left-420</PyroWaveProfile>
```

Without the opt-in or a positive probe, no PyroWave capability is advertised.
`ServerCodecModeSupport` keeps its existing standard-codec meaning. Discovery is
advisory: ANNOUNCE must revalidate current runtime configuration and the adapter.

The client also supplies `pyrowave=1` on launch/resume. That selects the dedicated
GPU probe before the app session is admitted, avoiding the standard backend's
H.264 prerequisite. ANNOUNCE must match the launch choice. A PyroWave launch cannot
subsequently request a standard codec; negotiate a new launch/resume instead.

The normal ANNOUNCE fields are still required. A PyroWave request adds all these
attributes (this fragment is not a complete ANNOUNCE request):

```text
a=x-nv-vqos[0].bitStreamFormat:3
a=x-vp-pyrowave.version:1
a=x-vp-pyrowave.bitstreamRevision:d2997ac172bdc00e29c58e3f2938acb7e94580bf
a=x-vp-pyrowave.profile:sdr-bt709-full-left-420
a=x-vp-pyrowave.pathMtu:1280
a=x-nv-video[0].encoderCscMode:3
a=x-nv-video[0].dynamicRangeMode:0
a=x-ss-video[0].chromaSamplingType:0
a=x-nv-video[0].packetSize:1024
```

Missing or conflicting extension fields, unsupported version/revision/profile,
invalid dimensions or an impossible packet budget are rejected. The intended
responses are `400` for malformed parameters, `406` for an unsupported profile or
unavailable codec, and the existing `403` for unmet encryption requirements. A
successful response remains the existing RTSP `200 OK`; it does not bypass launch
authorization. The client must not send PyroWave payloads through a browser WebRTC
session.

The first implementation admits one PyroWave video session at a time. The
admission check must cover concurrent starts and probes; it is released on every
failure/teardown path. Additional concurrency requires separate GPU and lifecycle
validation. Standard session policies otherwise retain their existing behavior.

Allocation ceilings are even dimensions from 64 to 4096 pixels on each axis and
1–240 fps. Integer and fractional rates must agree after rounding to the nearest
fps (for example, 60 and 5994). Effective encoder and video-wire bitrates after
the server's reservations must be 1000–800000 Kbps, subject to a stricter
MTU/FEC/frame budget. The optional `x-ml-video.configuredBitrateKbps` may be zero
or absent to use `x-nv-vqos[0].bw.maximumBitrateKbps`; reservations and all budget
checks still apply. A value inside these ranges may still be rejected if it
cannot carry the frame. These ceilings do not claim that any GPU achieves every
resolution/rate combination.

The capture desktop must also be SDR. HDR desktop tone mapping is outside this
profile. Version 1 fixes the frame rate, encoder byte target, IP byte budget and
FEC percentage when ANNOUNCE succeeds. A later host FEC configuration applies
to new sessions. A bitrate change terminates the current PyroWave session and
requires a new negotiation; it cannot silently enlarge the wire budget. The
internal bitrate event is a no-op only when it repeats the current encoder
bitrate after server reservations. This is not necessarily the original client
wire bitrate; clients must renegotiate instead of relying on adaptive bitrate
events in version 1. Disabling the runtime feature stops an active PyroWave
capture without changing its codec. Restart/resume negotiates new budgets and
performs a fresh adapter probe.

## Complete-frame envelope: PWVF

The existing 8-byte GameStream short frame header precedes this envelope. Its
`frameType` is 2 (independent frame), and `lastPayloadLen` trims transport padding.
After reassembly/decryption/FEC, the PWVF envelope must have the exact declared
length. All integer fields below are unsigned **little endian**. Implementations
must serialize fields explicitly, without transmitting C++ structures.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `PWVF` |
| 4 | 2 | Protocol version, exactly 1 |
| 6 | 2 | Header size, exactly 32 |
| 8 | 4 | Total PWVF bytes including this header and all packet records |
| 12 | 4 | Native packet count, 1–4096 |
| 16 | 8 | Full stream frame index |
| 24 | 8 | Host monotonic presentation timestamp in microseconds |
| 32 | variable | Packet records in upstream packetizer order |

Each packet record consists of a 4-byte little-endian length followed by exactly
that many native packet bytes. A length is nonzero and divisible by 4. Native
words are preserved in the pinned format's little-endian order. No padding is
inserted between records. The sum of all records plus 32 must equal total size;
trailing bytes, truncation, zero records and oversized allocations are rejected.

The absolute parser safety ceiling is 4 MiB per PWVF frame. The negotiated
transport limit is lower and takes precedence. Native packet boundaries do not
need to align with UDP boundaries: the existing transport may split any record.
The receiver passes the recovered native packets to the pinned decoder in their
original order. The 64-bit frame index disambiguates the native codec's short
sequence counter. Timestamps use an arbitrary monotonic origin, not wall time;
only differences are meaningful. The RTP clock remains 90 kHz as in the existing
transport.

Version 1 decodes a frame only after all its bytes have been recovered. Packet
loss is handled by transport FEC; an unrecoverable frame is discarded. The next
frame is independent. Partial native-codec recovery can be added in a future
protocol revision. IDR requests do not need an inter-frame encoder reset because
every output frame is intra; the server must still consume the control requests.

## MTU, FEC and rate control

The client supplies an IP path MTU between 1280 and 1500. This is a protocol safety
range, not path-MTU discovery. The configured GameStream packet size `S` must be at
least 200 and fit the following equation:

`S + 16 + encryption_prefix + UDP_header + IP_header <= path_MTU`

| Component | Bytes |
| --- | ---: |
| GameStream `packetSize` (`S`) | negotiated |
| RTP header/reserved bytes outside `packetSize` | 16 |
| NV video header, already included in `packetSize` | 16 |
| AES-GCM prefix when video encryption is enabled | 32 |
| UDP header | 8 |
| IPv4 / IPv6 header used in this budget | 20 / 40 |
| Per-frame GameStream short header | 8 |
| Per-frame PWVF header | 32 |
| Each native-packet record length | 4 |

Each data shard carries `S - 16` bytes of the short header plus PWVF. The maximum
is four FEC blocks, each with at most 255 total data/parity shards. Version 1 keeps
this conservative 255-shard bound even with FEC disabled; it never approaches the
legacy 1024-packet index boundary. FEC configuration supports the existing
percentage range 0–255. Minimum parity is included before selecting a frame size.

For a block with `D` data shards and percentage `F`, parity is
`max(ceil(D * F / 100), minimum_parity)` when FEC is active, and zero when disabled.
The adjusted percentage carried in `fecInfo` must fit 8 bits and reconstruct the
same parity count at the receiver. Some large minimum-parity requests are not
representable and must fail, rather than produce inconsistent FEC headers.
Negotiation also requires the smallest one-shard frame to be representable: a
mostly static desktop may compress far below the target. For example, minimum
parity 4 with FEC 20% is rejected even if a large frame would fit. Minimum parity
0–2 works for small frames; 3 requires a base percentage already producing three
parity shards (201–255%). Minimum parity is ignored when FEC is disabled.

`src/pyrowave_protocol.cpp` computes the exact plan: choose enough blocks for the
data, then distribute remaining shards across the first blocks so counts differ
by at most one. The sender must use these exact counts. Reusing the legacy split
with a rounded fixed block byte size would produce a different parity budget.

The frame budget includes IP/UDP, video headers, encryption, minimum parity and
the chosen block layout. It is derived from the video share of the session's wire
bitrate and the actual fractional frame rate, with audio/control budget reserved
separately. The encoder target additionally subtracts PWVF overhead. Before
transmitting, recompute and validate the actual serialized frame's plan. A frame
that exceeds the contract is rejected/dropped with diagnostics; FEC is never
silently disabled to make it fit.

Upstream `max_bitstream_size` includes the native 8-byte frame header and is
rounded to words. The pinned packetizer's output-capacity check is only an assert;
the adapter must allocate from the mapped raw allocation size plus header,
validate packet counts and every returned span, and then enforce the smaller
negotiated byte budget. An encoder target is not a safe packetizer allocation size.

## Validation and client handoff

The first Windows implementation retains a single runtime device and DLL for the
process lifetime, bound to one capture-adapter LUID. A new adapter or a failed
runtime context requires a host restart. This is a host implementation limit;
session encoders, imported images and fences are still released at teardown.
The packaging manifest identifies local ownership/cleanup patches separately
from the unchanged upstream bitstream revision above.

Implementation checks must cover:

- Exact layout, byte order, length/count overflow, truncation and trailing data.
- Unknown/malformed codec IDs and missing/version-mismatched opt-in fields.
- MTU at thresholds, IPv4/IPv6, encryption, FEC 0/1/20/100/255 and minimum parity.
- Every block count and the exact parity reconstructed from `fecInfo`.
- Actual server packets reassembled and decoded with the pinned reference decoder.
- Color/range/siting fixtures, resolution changes, stop/restart, device loss and
  bounded application queues.
- Standard-codec regressions, feature OFF/ON and absent/incompatible runtime.

Runtime resolution/FPS qualification, GPU/driver compatibility and latency are
recorded from actual measurements in PW-S04–PW-S11. A parser, a compile or a Vulkan
feature check alone does not qualify a streaming mode. Client handoff includes
request/response transcripts, packet fixtures, exact upstream revisions,
reproduction commands, measured support and remaining limitations.

Sources: [pinned API](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/pyrowave.h),
[packetizer](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/pyrowave_encoder.cpp),
[C API adapter](https://github.com/Themaister/pyrowave/blob/d2997ac172bdc00e29c58e3f2938acb7e94580bf/pyrowave_c.cpp),
and VibePollo's `src/stream.cpp`, `src/rtsp.cpp` and Windows NV12 conversion shaders.
