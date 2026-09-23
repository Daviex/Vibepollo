# PyroWave private protocol v2: profiles and partial frame transport

Status: server code and reference receiver helpers are integrated; qualification
is pending. This document specifies the new contract; it does not claim that a production
client or hardware/transport qualification has been completed. Version 1 and its
PWVF layout remain available unchanged.

## Negotiation

Version 2 selects the extended profiles documented in
[the capability inventory](pyrowave-capability-matrix.md), with the same exact
upstream bitstream revision as v1. Its required transport identifier is
`fragments-v2`; v1 requires `complete-v1`. A version/transport mismatch is rejected.
Legacy v1 accepts only its original SDR 4:2:0 profile. V2 retains that profile
and admits the extended profile model subject to the advertised platform list
and runtime checks.

Windows/Linux expose paths for all 128 profile values. macOS currently exposes
only the 64 p8 values because its Metal upload path rejects p16. The macOS
AVFoundation source is SDR BGRA8, so a PQ output profile there converts SDR to PQ
and does not imply native HDR desktop capture. Linux native HDR capture requires
the restrictive KMS color/format checks in the capability inventory. Output
profile metadata must not be treated as proof of the captured source's dynamic
range.

For v2, the sender uses `transport_config_t::fragmented=true`. Every UDP **data**
shard contains a complete, independently identifiable PWPF slot after its RTP/NV
header. The 8-byte GameStream short-frame header used by v1 is **absent**. This is
a private wire contract, so a receiver must select it through explicit v2
negotiation and must not interpret PWPF as the legacy short header or PWVF.

Let `S` be the negotiated GameStream `packetSize`:

- The existing RTP/NV header is 32 bytes.
- Each PWPF slot is exactly `S - 16` bytes, including its own headers and padding.
- The unencrypted UDP shard is `S + 16` bytes.
- Existing AES-GCM, when selected, adds the same 32-byte prefix as v1.
- IP/UDP headers, MTU validation, at most four FEC blocks, and at most 255 total
  shards per Reed-Solomon block keep their existing meanings.

The serialized frame handed to the sender is simply the concatenation of these
fixed-size PWPF slots. Its exact size is a multiple of the slot size. No global
prefix is needed to interpret an individual received slot. RTP/NV data shards
carry PWPF; parity shards carry Reed-Solomon output and must first be identified
and, if needed, recovered using the existing transport header.

## PWPF slot header

All multi-byte fields below are unsigned little-endian integers. The fixed
header is 48 bytes. Every data shard repeats it, including the complete native
PyroWave sequence header.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `PWPF` |
| 4 | 2 | Format version, exactly 2 |
| 6 | 2 | Header size, exactly 48 |
| 8 | 8 | Full frame index |
| 16 | 8 | Presentation timestamp in microseconds |
| 24 | 2 | Data-shard index across the entire frame, starting at zero |
| 26 | 2 | Number of data shards in the entire frame |
| 28 | 2 | Number of native PyroWave packets |
| 30 | 2 | Number of records in this slot |
| 32 | 2 | Number of initial native packets considered critical for the selected pristine-band count |
| 34 | 1 | Active-block/pristine-band count, 0..4; the server currently uses 3 |
| 35 | 1 | Reserved, exactly zero |
| 36 | 4 | Number of possible active blocks covered by the sideband mask |
| 40 | 8 | Exact native PyroWave start-of-frame sequence header |

The native header carries dimensions, chroma mode, sequence counter, nonzero
block count, and the five color fields. Its dimensions and chroma mode determine
the expected active-block count using the pinned upstream padded block mapping.
The parser checks this relationship before allocating receiver data.

All slots belonging to a frame must agree on the frame index, timestamp, total
shard count, native packet count, critical packet count, band count, active-block
count, and all eight sequence-header bytes. The application must also associate
the frame with its authenticated session; the frame index alone is not a global
session identifier.

## Fragment records

Records follow the 48-byte header. Each has a 16-byte header followed by a
positive, four-byte-aligned amount of item data. Several records may share a
slot. A large native packet can span as many slots as necessary within the frame
budget. No native packet is assumed to fit inside the path MTU.

| Offset within record | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Kind: 0 = native packet, 1 = frame manifest |
| 1 | 1 | Flags: bit 0 = critical; all other bits zero |
| 2 | 2 | Data bytes following this record header |
| 4 | 4 | Item index: native packet index, or zero for the manifest |
| 8 | 4 | Total item size in bytes |
| 12 | 4 | Offset of this fragment within the item |

Item size, offset, and fragment length are multiples of four. The fragment must
fit entirely inside the declared item. Native packet indices are less than the
header's native packet count. Every manifest fragment is critical; a native
fragment is critical precisely when its packet index is below the repeated
critical packet count. The receiver checks these flags rather than trusting
them as independent declarations.

After the declared records, every remaining byte in the slot is zero padding.
The serializer places the manifest first, followed by native packets in their
upstream order, with contiguous increasing item offsets. Thus critical shards
form an initial prefix. The sender planner requires this canonical complete
ordering. The receiver reassembler accepts arrival in any order.

## Protected manifest

The manifest contains:

1. Five 32-bit critical native-packet counts for upstream pristine-band counts
   0, 1, 2, 3, and 4, in that order.
2. `ceil(active_block_count / 32)` 32-bit active-block words, in upstream order.

Its size is exactly `20 + 4 * ceil(active_block_count / 32)` bytes. Counts are
nonzero, nondecreasing, and no greater than the native packet count. The count
selected by the slot's band field must equal its repeated critical packet
count. Bits beyond `active_block_count` in the final mask word must be zero.

The active-block count is calculated by the same padded block ordering used by
upstream `get_num_active_blocks()`. In particular, band count 1 can include the
preceding components' high-pass blocks because the mask is an initial prefix of
the upstream block order; replacing that with an intuitive three-LL-band count
would give the wrong mask size.

## FEC and encryption

The server protects the initial shards containing the manifest and critical
native packets with `max(base_fec_percentage, critical_fec_percentage)`;
`critical_fec_percentage` defaults to 40. Later shards use the base percentage.
The effective percentage of every block remains encoded in its normal FEC
header, so recovery requires no new Reed-Solomon scheme.

The planner separates the critical prefix into early blocks where capacity
allows. It can include extra noncritical shards in the final critical block to
stay within the four-block limit. Negotiation reserves bandwidth as if **all**
blocks used critical protection. If a selective split would exceed that
conservative wire budget, require a fifth block, or produce a minimum-parity
percentage that cannot be represented, the sender uses the conservative
all-critical plan. It never compensates by disabling FEC for an oversized
PyroWave frame.

The server preserves an explicit zero-FEC setting by negotiating both base and
critical percentages as zero. For a nonzero base setting it negotiates critical
protection of at least 40 percent. Low-level callers constructing a transport
configuration directly must set both percentages to zero to disable FEC; the
struct's default critical percentage is 40. The send worker uses the negotiated
critical value rather than reconstructing a different default.

The existing AES-GCM implementation encrypts/authenticates each prepared shard,
including its PWPF bytes. Its IV counter, wrap rejection, prefix, RTP/NV fields,
and FEC-before-encryption order are preserved. Selective protection changes only
the data/parity count chosen for each block.

## Byte budgeting

`frame_budget()` accounts for MTU, IPv4/IPv6, encryption, parity, and block
capacity. In v2 its result is an exact number of PWPF slots, with no subtraction
for a legacy short-frame header.

Before encoding, `sideband_size_bound(width, height, chroma, bands)` computes the
manifest size without creating a GPU encoder. `fragmented_payload_budget()`
then reserves room for that manifest, slot headers, record headers, alignment,
and fragment boundaries. The returned native byte limit is rounded down to a
multiple of four and still must exceed the encoder's minimum accepted size.

The conservative native-packet count used by this helper relies on the current
upstream packet boundary of 64 KiB. A single coded block can contain at most
4095 32-bit words; therefore every nonfinal native packet produced by that
packetizer contains at least `65536 - 4095 * 4` bytes. Changing the runtime
packet boundary requires changing the helper's bound too. The serializer also
checks the actual item and slot sizes; a bound mismatch is a rejected frame,
never an over-budget send.

The encode worker and send worker must use the immutable budget attached to
each queued frame. A later rate update cannot retroactively change the allowed
size of an already encoded frame. The sender validates the complete PWPF layout
and the actual selected FEC plan against that frame's byte and wire budgets.

Geometry admission separately caps nominal input planes at 512 MiB using 64-bit
arithmetic. This limit is checked during negotiation and runtime setup; it does
not include capture surfaces, wavelet resources, driver overhead, or guarantee
that a particular GPU can allocate the admitted geometry.

## Live rate control

Initial negotiation, the control setter, and the encode worker share
`negotiation::rate_budget()`. The session retains its original negotiated encoder
rate and wire allowance. A requested live rate must be positive, no greater
than that initial encoder rate, and sufficient for the transport's minimum
frame. The helper scales the wire allowance relative to the original pair and
then applies the same FEC, framing, and manifest reservations used at admission.

The setter validates a PyroWave change before updating session metadata or
sending a rate event. If matching sessions exist but none can admit it, the
`/bitrate` response carries XML status 406 and bitrate zero. A rejected direct
worker event retains the prior encoder settings. The normal endpoint still
applies the host `max_bitrate` setting and its existing 500000 kbps absolute
ceiling. Raising the negotiated encoder ceiling requires a new session.

This is client-driven rate control; server-side automatic ABR decisions are not
implemented. Frames already queued keep their original frame/wire/native byte
budgets when a later rate change is accepted.

## Reference receiver contract

`parse_fragment()` accepts one decrypted or FEC-recovered **data-shard payload**,
with RTP/NV headers already removed. It validates lengths and bounds without
requiring any other datagram.

`reassemble_partial_frame()` accepts a bounded collection of these payloads. It:

- accepts reordered shards and identical duplicates;
- rejects duplicate shard indices with different bytes, inconsistent frame
  metadata, conflicting item sizes, and conflicting overlapping fragment words;
- validates every item size and the aggregate declared size before allocating
  packet buffers;
- emits only complete, structurally valid native packets, with their original
  indices;
- verifies a recovered native packet zero begins with the repeated sequence
  header;
- returns the manifest only when all its fragments were recovered;
- reports missing native packets, manifest availability, complete-frame status,
  and whether the manifest plus all selected critical native packets survived.

The aggregate item data is capped at 4 MiB, the native packet count at 4096, the
active mask at 32768 words, and the shard count at the negotiated transport
limit. The single-call input collection allows at most 2040 entries including
duplicates. Applications must separately bound the number and lifetime of
simultaneously queued frames. The helper does not create an unbounded network
session queue.

To decode a partial frame, a future client feeds the recovered sequence header
first when native packet zero is missing, then feeds the complete recovered
native packets. It can pass the recovered active mask to upstream's sideband
readiness function and apply its chosen presentation timeout / minimum packet
ratio. Missing native packets are omitted; they are never fabricated with
zero-filled encoded bytes. Upstream then reconstructs missing coefficient blocks
according to its native partial-decoding semantics.

The current reassembler works at the native packet level. Losing one fragment
of a 64 KiB native packet prevents that entire native packet from being emitted,
while complete surviving native packets remain usable. Client rendering,
timeout policy, packet-ratio policy, and user-facing quality adaptation remain
part of the next client implementation.

## Required verification

Before marking this contract qualified, record evidence for:

- complete native round trips, repeated sequence-header agreement, profile
  metadata, manifest block mapping, and native packets larger than one MTU;
- loss of noncritical data beyond FEC recovery with successful partial native
  decoding, and separately loss of the manifest / critical fragments;
- selective FEC percentages in actual RTP/NV headers, conservative fallback,
  min-parity edge cases, encryption authentication, and IV-wrap rejection;
- reordered/duplicated datagrams, duplicate conflicts, offset/length overflow,
  inconsistent frame metadata, and allocation ceilings;
- frame-specific budgets while bitrate changes, and v1 wire-layout regression.

This document describes implemented code paths and their acceptance criteria.
Compile records for selected Linux objects and the bundled runtime are kept in
`build/pyrowave-linux-compile/`, with their source/patch identities. No new runtime,
GPU, or network tests were run for this audit, and compilation does not establish
partial decoding or platform capture behavior. Earlier v1 SDR reports retain
only their original scope.
