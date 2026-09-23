# PyroWave local dependency patches

## Bundled PyroWave 0.5.0 extensions and resource fixes

File: `pyrowave-0.5.0-nt-handle-ownership.patch`

Current SHA-256: `8f3b4ee860c114a753b3c32bff62fca44a915df94080e0916cfa494372ea36d2`.
The authoritative revision and hash values are in
[`pyrowave-pins.cmake`](../dependencies/pyrowave-pins.cmake). The filename is
retained from the original ownership fix; its scope now also includes the
extensions below.

Baseline revisions:

- PyroWave: `d2997ac172bdc00e29c58e3f2938acb7e94580bf` (C API 0.5.0).
- Granite: `9d44761debb9ac31d8d800cac8b030a7a0390b7e`.

The patch preserves existing public C ABI signatures and the pinned bitstream
syntax. It adds C API functions and changes allocation checks, device dispatch,
CPU input handling, precision configuration and sequence-header color metadata.
The host requires the bundled patched library; upstream API version 0.5.0 alone
is insufficient. Embedded shader sources/bytecode remain those of the pinned
upstream revision. The dependency manifest records the patch hash separately.

The same verified patch is applied before the isolated Vulkan build on Windows
and Linux, or the Metal build on macOS. Its 15 modified files comprise four
Granite Vulkan files, seven PyroWave Vulkan/common files and four files under
`metal/`. Granite sources are present for patch application but are not linked
into the Metal backend.

### Extension scope

| Contract component | Behavior |
| --- | --- |
| `nt-handle-ownership-v1` | Windows NT handles transfer only on C API success; failed external-memory binding releases the allocation. Details below. |
| `color-metadata-v1` | `pyrowave_vibepollo_encoder_set_color_info` validates five independent Boolean VUI fields and writes them into the native sequence header: primaries, transfer, YCbCr matrix, range and chroma siting. It describes pixels already converted by the host. |
| `multi-device-v1` | Device activation selects the correct global Vulkan instance dispatch table; per-device dispatch remains with Granite. Device identity exposes name, UUID and vendor/device IDs. The host must serialize activation and subsequent C API operations. |
| `allocation-checks-v1` | Wavelet samplers, images, views, metadata and encoder/decoder scratch allocation failures are checked and propagated. Failed initialization can reject a session without assuming a successful GPU allocation. |
| `precision-config-v1` | `pyrowave_vibepollo_configure_precision` selects 0/1/2 before precision is frozen by initialization; -1 retains the current environment/default selection. Changing frozen precision is rejected and requires a host restart. No process environment mutation is needed. |
| `cpu-planar-v1` | Vulkan CPU input accepts three Y/Cb/Cr planes in 8-bit or full-range UNORM16 storage, for 4:2:0 or 4:4:4. An additive wait function exposes bounded fence waiting before packetization. It does not make driver teardown cancellable. |
| `metal-extensions-v1` | Metal exports the same contract getter, precision configuration and five-field color metadata setter. Native header layout is preserved. The existing Metal CPU encoder still accepts 8-bit input only. |

The host's profile conversion, session policies, packet envelope and transport
are implemented outside this dependency patch. A 16-bit input sample is UNORM16,
not a P010 value shifted into a 16-bit word. Wavelet precision 0/1/2 is a separate
choice from the input sample precision.

### Runtime identity

The local C export `pyrowave_vibepollo_runtime_contract()` returns the exact
immutable value in
[`src/pyrowave_runtime_contract.h`](../../src/pyrowave_runtime_contract.h).
It combines the pinned PyroWave revision with every component listed above.
Production loaders and the reference decoder share that header. The ownership
verification script compiles against it instead of copying a contract literal.

The getter uses the existing C export macro/calling convention and performs no
allocation or GPU initialization. Exports are also listed in
`pyrowave-shared.def` and `metal/pyrowave.exports`. The host checks both the
0.5.0 API version and the exact contract before creating GPU resources. The
contract is an accidental-mismatch guard; archive/patch hashes establish the
separate build provenance. Replacing a rejected runtime requires restarting the
host before retrying its cached loader.

### NT handle defects addressed

The pinned Granite allocator closes an imported NT handle immediately after
`vkAllocateMemory`, including when allocation fails. A successful allocation can
also be followed by `vkBindImageMemory` failure. In either case the C API reports
failure after consuming a caller-owned handle. VibePollo correctly retains its
RAII handle until the C API returns success, so the unpatched dependency can
cause a second close, possibly after Windows has reused that handle value.

Checking only the allocation result before closing is insufficient: binding and
later object allocation can still fail. `GetHandleInformation`, handle-protection
flags and speculative duplicate closes do not provide an ownership contract.

The semaphore import path already waits for successful Vulkan import before
closing, but allocates its public wrapper afterwards. A wrapper allocation failure
therefore has the same ownership problem.

Separately, `ImageResourceHolder` does not assign its optional allocator pointer
in the pinned image-creation path. If binding fails after memory allocation, its
destructor destroys the image without freeing that allocation.

### NT handle patch behavior

1. An internal Windows-only `ExternalHandle` flag defaults to existing Granite
   behavior. The C API sets it to borrow the NT handle during image/semaphore
   creation; the allocator/importer then leaves that handle open.
2. Each C API function allocates its opaque wrapper with `new (std::nothrow)`
   before import. Wrapper OOM returns `PYROWAVE_ERROR_OUT_OF_HOST_MEMORY` and
   leaves the caller handle intact. Invalid input/output pointers are rejected
   before import.
3. The wrapper receives the finished resource and is published through the output
   pointer. Closing the imported NT handle is the final operation before returning
   `PYROWAVE_SUCCESS`. All prior failures, including exceptions during Granite
   work, retain the caller's NT handle. KMT handles retain their existing
   non-owning behavior; file-descriptor imports are unchanged.
4. External-memory image creation attaches its allocator to `ImageResourceHolder`.
   Its cleanup destroys the image first, then frees the allocation under the
   device memory mutex. This assignment is restricted to external-memory images.

For the existing default-view failure branch, its local `LOCK_MEMORY()` guard
exits before the outer holder destructor. Its explicit `free_immediate()` clears
the allocation base, making the later holder cleanup a no-op for memory; no
recursive lock or double free is introduced. Once ownership transfers to a valid
`ImageHandle`, `holder.owned` is false, and later failure is cleaned up by that
image handle instead.

The patch is unrelated to a possible growing process-handle count during
otherwise successful repeated Vulkan device creation. Those successful import
paths already close their shared NT handles in the original code and require
separate lifecycle measurement.

### Verify and apply manually

Run from the repository root against an unmodified extracted dependency tree.
Replace the example source directory with the configured pin directory.

```powershell
$source = 'build/pyrowave-server-on/_deps/pyrowave/src-<configured-pin-id>'
$patch = 'cmake/patches/pyrowave-0.5.0-nt-handle-ownership.patch'
(Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash.ToLowerInvariant()
git apply --check --verbose "--directory=$source" $patch
git apply "--directory=$source" $patch
git apply --reverse --check --verbose "--directory=$source" $patch
```

Do not rely on `git -C <source> apply` inside this repository: Git can treat the
patch paths as outside that subdirectory and skip them. The verbose check must
list all 15 patched files. Normal builds use the CMake dependency patch step;
manual application is only for an isolated verification tree.

### Deterministic fault injection

`verify_nt_handle_ownership.py` extracts the actual patched C API functions,
`ExternalHandle` declaration and Granite close guards, then compiles those bodies
against deterministic failure adapters. It neither loads a driver nor opens a
real NT handle. The close callback asserts that the output wrapper has already
been published, and rejects duplicate closes. The source tree is read only.

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
python cmake/patches/verify_nt_handle_ownership.py --source $source
```

Historical evidence, obtained with GCC 15.2 on 2026-09-23 for the earlier
ownership-only patch SHA-256
`ba9f00d2fda290d4fd93d5792b3f08100eb12e9e8fb92add5012a2e76302fc8d`:
**13 ownership cases passed**, plus the then-current runtime-contract getter and
Windows export-list entry. Cases cover
image allocation failure, binding failure, image-pool failure, a thrown host
allocation error, wrapper OOM for image/sync, semaphore creation/import failure,
successful NT imports, successful KMT imports, and invalid input/output pointers.

The script's shared-contract reference was subsequently updated without running
it. The historical result does not establish a pass for the current expanded
patch and does not cover HDR/4:4:4, multi-device behavior, precision settings,
the Linux CPU bridge or Metal. Runtime builds and hardware results must name the
revision/hash they actually exercised; the earlier Windows GPU evidence is
recorded separately in [`pyrowave-gpu-validation.md`](../../docs/pyrowave-gpu-validation.md).

This is a C API ownership test with simulated Granite failures. It does not
exercise actual `vkAllocateMemory`, `vkBindImageMemory`, driver error recovery,
or the complete Granite resource-holder destructor. The external allocation
cleanup above is source-reviewed; real GPU cycles and dependency builds provide
separate validation.
