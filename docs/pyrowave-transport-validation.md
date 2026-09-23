# PyroWave transport validation

> **PW-X extension status (23 September 2026):** This document records the original SDR 4:2:0 phase at commit `205c510c`. Its limits, hashes and test results are historical. For the extended implementation, use the [capability matrix](pyrowave-capability-matrix.md), [protocol v2](pyrowave-protocol-v2.md) and [extension build report](pyrowave-extension-build.md). The earlier results do not qualify HDR, 4:4:4 or the new transport/runtime paths.


Validated on Windows x64, 2026-09-23: **8/8 transport component tests passed**
with MSYS2 UCRT64 GCC 15.2 and the bundled GoogleTest sources. The final direct
run took 15 ms after compilation. These results concern the server transport
functions and a bounded test receiver; they do not claim a completed client.

## Production code exercised

`tests/unit/test_pyrowave_transport.cpp` directly links:

- `src/pyrowave_transport.cpp`: short header, RTP/NV slots, FEC metadata,
  padding, AES-GCM prefix and encryption.
- `src/pyrowave_protocol.cpp`: PWVF serialization, parsing and transport plan.
- `src/stream_protocol.cpp`: the shared `concat_and_insert` implementation.
- `src/rswrapper.c`: the same Reed–Solomon implementation used by the server.
- `src/crypto.cpp`: the same OpenSSL AES-GCM implementation used by the server.

`stream.cpp` calls these transport functions for PyroWave, preserving its shared
pacing, batching and socket-send code. The tests do not reproduce packetization
or FEC encoding in a second implementation. The receiver independently derives
shard counts from the transmitted headers, decrypts, deduplicates, invokes the
production Reed–Solomon decoder, strips headers/padding and parses PWVF.

The receiver is a test fixture, not a production client decoder. Native packet
contents in this component are deterministic test bytes, not GPU output.

## Coverage

| Case | Evidence and boundary |
|---|---|
| Exact layout | Literal 32-byte RTP/NV fixture, 8-byte short header, zero padding, 16-bit RTP sequence wrap, 24-bit stream sequence field, deterministic IV/domain and frame number. Decrypted ciphertext equals the plaintext fixture. |
| Layout drift | Compile-time assertions compare every used RTP/NV field offset, transport flags and encryption-prefix offsets with the bundled Moonlight structures. Production `stream.cpp` also asserts aggregate header sizes. |
| UDP loopback | Actual IPv4 and IPv6 UDP sockets, each with plaintext and AES-GCM; one datagram sent and received at a time. Reassembled PWVF exactly equals the original. |
| MTU | All four IP/encryption combinations choose packet size so datagram plus base IP/UDP headers equals 1280 bytes. Sum of those bytes equals the production transport plan. |
| Four FEC blocks | An 800000-byte native payload produces four blocks. Remove the first data shard from each block, reverse block/shard order, duplicate each survivor, then reconstruct exact PWVF bytes. This loss/reorder injection uses the receiver directly. |
| FEC matrix | Percentages 0, 1, 20, 100 and 255 crossed with minimum parity 0, 2 and 16. A lost data shard is recovered whenever FEC is enabled; FEC zero is tested without recoverable parity. |
| Unrecoverable loss | Dropping data with FEC disabled, or dropping more shards than parity permits, yields no reconstructed frame. |
| Payload boundaries | Native lengths 964, 968 and 100 exercise a full payload boundary, a new final shard, and minimum parity 2. |
| Encryption and truncation | Reject corrupted IV, domain byte, prefix frame number, tag or ciphertext; reject a different key and truncated datagrams. |
| Encoder API guards | Reject missing cipher for encrypted transport, IV counter wrap, and a partial shard too short to contain its header. |

The separate protocol component exercises the broader threshold matrix,
including minimum parity 254, all one-to-four-block transitions, frame limits,
IPv4/IPv6, encryption and maximal wire byte budgets. Negotiation additionally
rejects a minimum-parity setting if a smallest one-shard frame cannot be
represented, even when a large frame could fit.

### Authentication boundary

The legacy 32-byte encrypted prefix contains IV (12 bytes), frame number
(4 bytes), and tag (16 bytes). The prefix frame number is **not AES-GCM AAD**.
The receiver must compare it with the authenticated NV frame number and the
expected frame identity. The corruption test at prefix offset 12 passes because
of this explicit identity check; it does not imply that GCM authenticates the
entire prefix. Each shard is authenticated before FEC/reassembly.

Plain transport has no codec-level authenticity guarantee. FEC repairs loss; it
does not replace authentication. This component does not change the existing
host encryption policy.

## Reproduce the component

Run from the repository root in PowerShell. The direct build uses a temporary
directory and does not modify either configured host build directory.

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
$testDir = Join-Path $env:TEMP 'vibepollo-pyrowave-identity'
New-Item -ItemType Directory -Force -Path $testDir | Out-Null

& 'C:/msys64/ucrt64/bin/gcc.exe' -std=c11 -O2 `
  -I third-party/nanors/deps/obl -c src/rswrapper.c `
  -o "$testDir/rswrapper.o"
if ($LASTEXITCODE -ne 0) { throw 'Reed-Solomon compilation failed' }

& 'C:/msys64/ucrt64/bin/g++.exe' -std=c++23 -O2 -pthread -I . `
  -I third-party/libdisplaydevice/third-party/googletest/googletest/include `
  -I third-party/libdisplaydevice/third-party/googletest/googletest `
  tests/unit/test_pyrowave_transport.cpp `
  src/pyrowave_transport.cpp src/pyrowave_protocol.cpp `
  src/stream_protocol.cpp src/crypto.cpp "$testDir/rswrapper.o" `
  third-party/libdisplaydevice/third-party/googletest/googletest/src/gtest-all.cc `
  third-party/libdisplaydevice/third-party/googletest/googletest/src/gtest_main.cc `
  -lcrypto -lws2_32 -o "$testDir/pyrowave-transport-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Transport test compilation failed' }

& "$testDir/pyrowave-transport-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Transport tests failed' }
```

The existing `rswrapper.c` third-party amalgamation emits macro-redefinition
warnings with this direct command. The transport test compilation completes
without errors. The executable needs the UCRT64 runtime DLL directory on PATH.

For configured CMake test builds, the registered target is
`test_component_pyrowave_transport`. Build and run it through the repository's
component-test workflow; coordinate access if another build already owns that
Ninja directory.

## Limits of these results

- This component does not start `video::capture_pyrowave`,
  `videoBroadcastThread`, RTSP, the session lifecycle, or the GPU runtime.
  Successful function-level packet tests alone do not validate the complete
  sender thread, its pacing, stats, batching or teardown.
- MTU checks use base IPv4/IPv6 header sizes and the declared path MTU. Loopback
  does not validate a physical network, PMTU discovery, IP options, tunnels,
  congestion, Ethernet overhead or NIC offloads.
- Loss/reorder/duplicate tests inject controlled datagrams; they are not a
  sustained lossy-link performance or congestion test.
- These tests do not decode a native PyroWave image, establish image quality,
  measure GPU latency, or qualify another GPU/driver combination.

See [Protocol v1](pyrowave-protocol-v1.md),
[GPU validation](pyrowave-gpu-validation.md),
[Build validation](pyrowave-build-validation.md) and
[Client handoff](pyrowave-client-handoff.md) for the corresponding contracts and
separately recorded evidence.
