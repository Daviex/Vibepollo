# PyroWave v1 synthetic fixtures

These samples were generated from the original constant/gradient patterns in
VibePollo's validation harness. They contain no desktop capture, user data,
external artwork, audio or client traffic. The fixture files and manifests are
distributed under this repository's GNU GPL version 3 license; see `LICENSE`
at the repository root. The encoder dependency retains its own MIT license.

Both sets use pinned PyroWave commit
`d2997ac172bdc00e29c58e3f2938acb7e94580bf`, C API 0.5.0 and the authoritative
VibePollo profile `sdr-bt709-full-left-420`. Read `manifest.json` for dimensions,
source pattern, byte budget and SHA-256 checksums.

- `64x64-constant`: minimal exact-reconstruction example.
- `720p-gradient`: two native packets and a complete PWVF envelope.

The first synthetic frame is encoded. `frame.pwvf` contains the 32-byte PWVF
header, then repeated little-endian packet length and packet bytes. Each
`packet-N.bin` is the exact corresponding native packet, without an outer
transport header. No RTP, FEC or encryption bytes appear in these files.

Build `tools/pyrowave` with the bundled pinned headers/DLL, including the local
NT-handle-ownership contract export. The validation executable rejects an
unpatched upstream DLL even if it reports API 0.5.0. Then verify offline:

```powershell
python tools/pyrowave/verify_fixture.py --fixture-dir tools/pyrowave/fixtures/64x64-constant --decoder build/pyrowave-gpu-smoke/pyrowave-decode-fixture.exe
python tools/pyrowave/verify_fixture.py --fixture-dir tools/pyrowave/fixtures/720p-gradient --decoder build/pyrowave-gpu-smoke/pyrowave-decode-fixture.exe
```

Verification recreates the source pattern, checks the envelope and native
packet hashes, and decodes the persisted envelope in a separate process. A
compatible Vulkan GPU is required. Expected decoded hashes were checked on
NVIDIA RTX 3080 Ti; cross-vendor precision equivalence has not been established.
Re-encoding on another GPU or driver need not produce identical codec bytes.

Reproduction and measured validation scope are documented in
`docs/pyrowave-gpu-validation.md`.
