# PyroWave local dependency patches

## NT handle ownership and external allocation cleanup

File: `pyrowave-0.5.0-nt-handle-ownership.patch`

SHA-256: `ba9f00d2fda290d4fd93d5792b3f08100eb12e9e8fb92add5012a2e76302fc8d`

Baseline revisions:

- PyroWave: `d2997ac172bdc00e29c58e3f2938acb7e94580bf` (C API 0.5.0).
- Granite: `9d44761debb9ac31d8d800cac8b030a7a0390b7e`.

This patch changes resource ownership and error cleanup and adds one local build
identity export. Existing C ABI signatures, encoder, decoder, shaders, packet
format and negotiated bitstream revision are unchanged. The build manifest
records the local patch hash separately from those upstream revisions. The patch
must be applied before compiling the optional DLL; the runtime's existing caller
RAII must use that patched DLL.

### Runtime identity

The local C export `pyrowave_vibepollo_runtime_contract()` returns the immutable
static string
`d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1`. It uses the
existing C API export macro and calling convention, and is listed explicitly in
`pyrowave-shared.def` for the Windows linker. It performs no allocation or GPU
initialization. The host requires that export and an exact match before creating
a Vulkan device or imported resource. Upstream API version 0.5.0 alone cannot
distinguish an unpatched DLL or a different bitstream revision; it remains a
separate compatibility check. The getter is an accidental-mismatch guard, not a
cryptographic provenance check.

### Defects addressed

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

### Patch behavior

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
$source = 'build/pyrowave-server-on/_deps/pyrowave/src-ed2d2563f22d'
$patch = 'cmake/patches/pyrowave-0.5.0-nt-handle-ownership.patch'
(Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash.ToLowerInvariant()
git apply --check --verbose "--directory=$source" $patch
git apply "--directory=$source" $patch
git apply --reverse --check --verbose "--directory=$source" $patch
```

Do not rely on `git -C <source> apply` inside this repository: Git can treat the
patch paths as outside that subdirectory and skip them. The verbose check must
list all seven patched files. Normal builds use the CMake dependency patch step;
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

Validated with GCC 15.2 on 2026-09-23: **13 ownership cases passed**, plus an exact
check of the extracted runtime-contract getter and its Windows export-list entry. Cases cover
image allocation failure, binding failure, image-pool failure, a thrown host
allocation error, wrapper OOM for image/sync, semaphore creation/import failure,
successful NT imports, successful KMT imports, and invalid input/output pointers.

This is a C API ownership test with simulated Granite failures. It does not
exercise actual `vkAllocateMemory`, `vkBindImageMemory`, driver error recovery,
or the complete Granite resource-holder destructor. The external allocation
cleanup above is source-reviewed; real GPU cycles and dependency builds provide
separate validation.
