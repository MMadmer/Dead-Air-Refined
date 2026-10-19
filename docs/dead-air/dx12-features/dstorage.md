# DirectStorage texture loading (xrRender_R5)

1.6.1: `r__dstorage` and its Video row are gone. DirectStorage loads wherever its runtime is up, `-no_dstorage`
keeps the old path (the A/B switch of the sections below). The level's textures are unloaded with it always
(`r__unload_level_textures` and its row are gone too).

Status: implemented (28.09.2026). S1-S9 in the tree (textures and `level.geom`/`level.geomx` through
DirectStorage); the functional part of the final test phase passed (every verified texture and geometry buffer
equal, frames unchanged); `r__dstorage` defaults to 1. The load-time A/B of S6-S9 was not run (one-pass Perf
phase, PLAN 8.4); numbers in section 13 and "Final test phase, performance" at the end. DX12
renderer only; `xrRender_R4.dll` keeps today's path unchanged and only learns the new console variable
(so the menu row can be shown greyed).

## Integration amendments (PLAN.md)

Implemented by the `ds` lane (milestones S1-S9 of `PLAN.md` 4.5). These points supersede the text
below where they differ (ids refer to `PLAN.md` section 1):
- **Vendoring and build lists by `core`** (K1): `Externals/DirectStorage`, the
  `Microsoft::DirectStorageHeaders` target, runtime staging, `runtime-files.txt`, the
  `DEPENDENCIES.md` row and obligations.
- **C22**: the installer has no licence page. The obligations of the Microsoft binaries (unmodified
  signed DLLs, end-user terms at least as protective, no Microsoft marks, telemetry off) are recorded
  in `DEPENDENCIES.md` exactly like those of the NVIDIA binaries.
- **C28**: no own caps hook: `core`'s `da_native_caps.cpp` fills `dstorage=` from
  `da_dstorage_available()`. `core` also registers `r__dstorage` / `r__texload_stats` (both modules),
  writes the Video row, nodes and strings of section 7, and pre-places `da_dstorage_shutdown()` in
  `CHW::DestroyDevice`.
- **Layer**: `TexturePageAllocator` lives in this lane's new `d12_texheap.cpp`, reached from
  `InitTexture` through `Device::TexturePlace`, a hook the `infra` lane pre-places at I0; the layer
  scenario lives in `scenarios_texheap.cpp` (C20).
- **C23**: infra's copy queue for `InitUploader` is measured only after S8, on top of DirectStorage.
- **3.8 geometry is in scope** as S9 (needs infra's `NativeBufferRange`, I2): merged only if the
  load A/B improves and the geometry verifies.
- xrCore (`rtc_decompress_prefix`, `xrCore.def` ordinal 1519) is this lane's alone.

## 1. Goal

1. Shorter level loads and shorter in-game texture hitches on DX12: texture bytes go from the game's
   archives straight into D3D12 textures through DirectStorage (DS) instead of mapped-memory page
   faults, a heap copy, a row copy into upload pages under one lock and a copy on the graphics queue.
2. **No re-packing of shipped archives.** Everything works on the `.xdb`/`.db` files as installed
   (stock archives, DAR2, our content bundles, level archives).
3. Measurable: every texture batch and every in-game synchronous load reports its path and time, on
   both paths, so `r__dstorage 0/1` is an A/B switch.

Non-goals (measured, see 2.4 and 3.9): GDeflate GPU decompression for shipped content, asynchronous
"pop-in" streaming, DS for meshes/sounds/spawn data, DS on DX11.

## 2. Current code

### 2.1 Archives (xrCore)

- Container: chunked file; chunk 1 is the index (optionally LZHuf-compressed, `CFS_CompressMark`;
  header-less `.db` of ShoC also encrypted), `open_chunk` `LocatorAPI.cpp:398`. An index entry is
  `archive_file_header` {`u16 size`, `u32 size_real`, `u32 size_compr`, `u32 crc`, name, `u32 ptr`}
  (`LocatorAPI.h:123-139`, reader `LocatorAPI.cpp:2398`). The parsed index is cached as
  `appdata\vfs-index-%08x.cache` (`LoadVfsIndexCache`/`SaveVfsIndexCache`, `LocatorAPI.cpp:83-154`).
- **Payload: raw when `size_real == size_compressed`, otherwise LZO1X-1** (`rtc_decompress`,
  `rt_compressor.cpp:32`, the `xr_lzo::decompress_safe` decoder of
  `lzo1x_decompress_safe_fast.inl`). No per-entry alignment: of the 12,733 DDS entries of all
  archives 2 start on 4 KB and 845 more on 16 bytes.
- Mounting: archives are processed in name order (`Recurse` sorts, `LocatorAPI.cpp:1049`) and a
  later registration overwrites the entry (`Register`, `LocatorAPI.cpp:348-355`), so the last archive
  wins. On x64 the **whole archive is mapped once** (`archive::open`, `MapViewOfFile` at `:658`,
  `archive::mappedBase`); `CLocatorAPI::m_archives` and `GetFileDesc` (`:1366`, exported @507) are
  public, so the renderer can resolve `name -> {vfs, ptr, size_real, size_compressed}` and the
  archive path without new xrCore API.
- Reading: `file_from_archive(IReader*&)` (`LocatorAPI.cpp:1671-1754`): raw -> `CPackReader` straight
  over the mapping (the IO happens as page faults while the texture is built); LZO -> `xr_alloc` +
  `rtc_decompress` into the heap.

The effective texture set of the stand install (index caches, last-archive-wins applied,
`_work\dstorage-probe-20260928\probe\vfs_effective.py`):

| Source | DDS files | Raw | LZO (real / stored) |
|---|---|---|---|
| `xtra_dead_air_x64_content_textures_00..14` (our HD bundles) | 2,035 | 4.70 GiB | - |
| `levels.xdb0..4` (lightmaps, `level_lods`, details) | 562 | 0.87 GiB | - |
| `textures.xdb0..2`, `xtra.xdb0` (stock) | 5,804 | 113 files, 0.03 GiB | 5,691 files, 2.81 / 1.60 GiB |
| `xtra_dar2.xdb0..1` | 1,995 | 28 files, 0.01 GiB | 1,967 files, 2.14 / 1.09 GiB |
| `xtra_dead_air_x64.xdb0` | 5 | 0.01 GiB | - |
| **Total** | 10,401 | 2,743 files, **5.62 GiB (53%)** | 7,658 files, **4.96 / 2.68 GiB (47%)** |

99.1% of the 12,584 DDS entries of the texture, level and bundle archives (overridden ones included)
map 1:1 to a DXGI format without conversion (BC1-BC7, 32-bit RGBA/BGRA/BGRX); 110 need DirectXTex
(71 other formats, 36 BC textures whose size is not a multiple of 4, 3 volumes).

### 2.2 Renderer

- `CRender::texture_load` (`dx11Texture.cpp:240`): resolves `$level$`, `$game_saves$`,
  `$game_textures$` (bump fallbacks), `FS.r_open` (`:295`), mip skip from `get_texture_load_lod`
  (`:305`, `texture_lod` 0 on Extreme, 1 on High, 2 on the default preset), BC fast path
  `create_texture_from_block_compressed_dds` (`:104`, `IMMUTABLE` texture with initial data),
  otherwise DirectXTex `LoadFromDDSMemory` + `CreateTextureEx` (`:337`, `:375`).
- `CTexture::Load` (`dx11SH_Texture.cpp:346`, normal textures at `:509`) is synchronous and sets
  `flags.bLoaded` before it has a surface.
- Level batch: `CRender::level_Load` starts `Resources->BeginDeferredUpload()` right after the shader
  list (`r2_loader.cpp:74`); `TextureUploadPool` (`ResourceManager.cpp:68-170`, at most 4 workers,
  count from a memory budget at `:180`) calls `CTexture::Load` while the main thread loads geometry,
  visuals, details, sectors, lights. `CLevel::net_start_client5` joins it
  (`Level_network_start_client.cpp:219-221` -> `DeferredUpload`, `ResourceManager.cpp:569`).
  `UnloadLevelTextures`, `DeferredUnload`, `ShutdownTextureUploadPool` wait for the pool first.
- In game, a texture first requested after the level loaded is loaded on the caller's thread:
  `_CreateTexture` -> `T->Load()` (`ResourceManager_Resources.cpp:278`), `apply_load` at the first
  bind (`dx11SH_Texture.cpp:195`), `surface_get` (`:172`). That is the texture hitch.

Stand logs (R5, `-noprefetch`): `l01_escape` warm: 1,168 textures (`t-report` base 1.39 GiB); the
phase that ends with `Final texture upload` (`net_start_client5`) takes 295 ms. `k00_marsh`, first
load of the level: the early batch (1,161 textures, 1.17 GiB) runs beside 1.6 s of main-thread phases
and the final phase still takes **873 ms**, most of it waiting for the pool: textures are on the
critical path.

### 2.3 xrD3D12 layer

- `CreateTexture2D` -> `InitTexture` (`d12_objects.cpp:363`): one `CreateCommittedResource` per
  texture (`CreateCommitted`, `:288`) in `COPY_DEST`, then `InitUploader::UploadTexture`
  (`d12_infra.cpp:2129`) copies every row into 32 MB upload pages **while holding the uploader's one
  mutex** and records `CopyTextureRegion` on a private DIRECT list, submitted at 256 MB
  (`kInitAutoFlushBytes`, `:25`) or before the immediate context's next submission.
- A `DEFAULT` texture without initial data is created in `COMMON` (`:404`) and gets its
  COMMON -> shader-read barrier at the first bind (`EnsureShaderInputs`,
  `d12_context_state.cpp:537-553`; deferred contexts hand it off through their requirements).
  `IMMUTABLE` without data is refused (`d12_objects.cpp:1486`), as in D3D11.
- Native interop: `xrd3d12::NativeDevice`, `NativeResource` (`d12_api.h:71-75`).

### 2.4 Measurements (offline probe)

`dsprobe` (source in `_work\dstorage-probe-20260928\probe`, not in the tree yet) loads every DDS of
one archive into D3D12 textures, (a) the way the engine does today (mapped archive, LZO into the
heap, rows into upload pages under one lock, DIRECT queue), (b) through DirectStorage 1.3.0. RTX 3070
Laptop, Samsung 990 PRO, 64 GB RAM, i7-11800H; other agents shared the machine, hence ranges. "Cold"
= first touch of a copy written with unbuffered I/O (`robocopy /J`), i.e. not in the file cache.

| Batch | Path | Wall |
|---|---|---|
| HD bundle `..._textures_02`, 248 textures, 696.5 MiB raw | today, 4 threads, warm / cold | 414-670 ms / 978-1045 ms |
| | today, 8 threads, warm | 1439 ms (the uploader lock) |
| | DS, unbuffered (always from disk), committed textures | 346-421 ms |
| | DS + textures placed in heaps | 292-351 ms |
| | DS with OS file cache (`ForceFileBuffering`), warm / cold | 409-425 ms / 532 ms |
| stock `textures.xdb2`, 295 textures, 510.3 MiB (302.9 MiB stored, LZO) | today, 4 / 8 threads | 474-744 ms / 455-510 ms |
| | DS, codec on 4 / 8 threads | 500-915 ms / 389-570 ms |
| | DS + placed, 4 threads | 504-530 ms |

- `CreateCommittedResource` costs 0.64-0.81 ms per texture (158-240 ms per batch above); placing into
  256 MB heaps costs 0.15-0.24 ms (44-60 ms).
- LZO decode + re-pitch runs at 260-370 MiB/s per core (1.4-1.95 s CPU for 510 MiB): LZO textures stay
  CPU-bound on either path.
- IO ceiling of DS into memory: 2.3-2.8 GB/s for the raw bundle ranges.
- Direct path (3.4): 696.0 of 696.5 MiB were read straight into the textures by DS; the tails that
  need re-pitching were 0.44 MiB; codec CPU 0.
- Correctness: 16-24 textures per run read back and compared with the DDS body, 0 mismatches in every
  DS mode (raw codec, LZO codec, direct + tail, GDeflate), including lightmaps (`levels.xdb0`).
- GDeflate (`DStorageCreateCompressionCodec`, default level): stock textures compress to 0.533 of the
  footprint size (LZO stores them at 0.594), the HD BC7 bundle to 0.833; 48-63 s CPU to compress
  0.5-0.7 GiB. GPU decompression runs through the driver metacommand (`GetCompressionSupport` = 0x19:
  GPU optimized, compute + copy queue): 510 MiB from memory in 318 ms, 205 ms of it creating the
  committed textures.

What that means:

- Raw textures (our bundles and level lightmaps, 53% of the effective bytes, about half of a level
  batch): 1.3-2.3x shorter warm, about 3x cold, and no CPU per byte.
- LZO textures (stock + DAR2): DS cannot make LZO faster. The gain is dropping the uploader lock (the
  codec scales with threads) and cheaper resource creation: 1.0-1.3x.
- The largest non-IO cost on both paths is `CreateCommittedResource`: placed texture heaps pay off
  with or without DS.
- GDeflate needs data written in GDeflate. Shipped archives are LZO or raw, so GPU decompression is
  out without re-packing (3.9).

## 3. Design

```
TextureUploadPool worker (level batch)  /  CTexture::Load on any thread (in game)
  da_dstorage::Begin(texture, fn, lod)          -- eligible? else today's path
    resolve   FS.GetFileDesc(fn) -> archive entry (vfs, ptr, sizes); loose/XMS files: today's path
    header    raw: archive.mappedBase + ptr     LZO: rtc_decompress_prefix(256 bytes)
    resource  HW.pDevice->CreateTexture2D(DEFAULT, SRV, no data)  -> layer: placed, COMMON
    requests  raw: FILE queue, FORMAT_NONE, MULTIPLE_SUBRESOURCES_RANGE (layout-matching mips)
                   + MEMORY queue: small tail re-pitched from the mapping
              LZO: FILE queue, custom format 0x80|LZO|skip<<1 -> codec pool -> staging memory
    status    one status-array slot per texture and queue
  batch end:  EnqueueSignal(fence) + Submit
  da_dstorage::Finish (pool Wait / first bind / sync load):
    status OK  -> SRV, MemoryUsage, PostLoad; the texture becomes visible to the renderer
    failed     -> release, today's path for that texture
```

### 3.1 Runtime (`da_dstorage.{h,cpp}`, new, R4 builds stubs)

- Load `dstorage.dll` by absolute path next to `xrEngine.exe` (pattern of `da_ffx_loader.cpp`),
  `GetProcAddress` `DStorageSetConfiguration1` and `DStorageGetFactory`; no import library. Lazy: on
  the first eligible texture after the device exists; `-no_dstorage` never loads it.
- `DSTORAGE_CONFIGURATION1` (process-wide, before the first `DStorageGetFactory`):
  `DisableTelemetry = TRUE`; `NumBuiltInCpuDecompressionThreads = 0` (only GDeflate would use them);
  `DisableBypassIO = ForceFileBuffering = TRUE` when the volume of `$arch_dir$` incurs a seek penalty
  (`IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceSeekPenaltyProperty`, i.e. an HDD) or with
  `-dstorage_buffered`. Unbuffered reads ignore the file cache: on this machine they still beat warm
  mapped reads for raw data (2.4), and DS's own guidance keeps buffering for HDDs only.
- `SetStagingBufferSize(64 MB)` (largest texture today: 42.7 MiB DDS); a texture whose footprint does
  not fit takes today's path. `SetDebugFlags(SHOW_ERRORS)` with `-dxdebug`.
- Queues on `xrd3d12::NativeDevice(HW.pDevice)`: `batch` (FILE, 4096, NORMAL), `sync` (FILE, 256,
  HIGH), `tail` (MEMORY, 4096, NORMAL) and `tail_sync` (MEMORY, 256, HIGH). One `ID3D12Fence`, one
  `IDStorageStatusArray` of 8192 slots used as a ring.
- Files: one `IDStorageFile` per `vfs` index, opened on first use from `m_archives[vfs].path`
  (`CP_ACP` -> UTF-16), closed at shutdown.
- Codec pool: `clamp(hardware threads - 2, 2, 8)` threads wait on
  `IDStorageCustomDecompressionQueue1::GetEvent()` and take `GetRequests1(SELECT_CUSTOM)` batches.
- Shutdown: drain (fence), `Close` queues and files, release factory; from `CHW::DestroyDevice`
  before `da_streamline_shutdown` (`dx11HW.cpp:718`) and from `~CResourceManager` after
  `ShutdownTextureUploadPool` (`ResourceManager_Reset.cpp:115`).

### 3.2 Eligibility (per texture, else today's path, silently)

R5 device; runtime ready; `r__dstorage 1`; name resolves to an archive entry (`vfs !=
VFS_STANDARD_FILE`) whose archive is mapped - loose `gamedata` files and XMS module overlays keep
today's path (mods stay exactly as they are); not `.ogm/.avi/.seq`; header maps 1:1 (2D, array or
cube; not volume; BC dimensions of the first kept mip a multiple of 4); footprint within the staging
size.

### 3.3 Header

- Raw: `mappedBase + ptr` (touches the entry's first page, which DS would read anyway).
- LZO: new xrCore export `size_t rtc_decompress_prefix(void* dst, size_t dst_len, const void* src,
  size_t src_len)` returning the bytes produced. It is `xr_lzo::decompress_safe` with the last
  literal run clipped at the output end: the safe decoder stops *before* a run that does not fit and
  produced nothing for 12 of 9,696 LZO headers in a 64 KB window in the probe.
- Parsing: `DirectX::GetMetadataFromDDSMemory` (DirectXTex is linked already) on the prefix, after the
  legacy `dwSize == 128` fix that `texture_load` already applies (`dx11Texture.cpp:323-334`); whitelist
  of formats that `create_texture_from_block_compressed_dds`/DirectXTex create unchanged.

### 3.4 Resource and requests

- Resource: `D3D11_USAGE_DEFAULT`, `BIND_SHADER_RESOURCE`, `TEXTURECUBE` for cubes, mips/size reduced
  by the skip exactly like the fast path (`skippedMips`, cubes keep all mips); no initial data, so the
  layer leaves it in `COMMON`, the state DS requires and leaves. `xrd3d12::NativeResource` gives the
  `ID3D12Resource` for the request.
- **Raw entries, no codec.** Per array slice, `k` = the leading kept mips whose tight DDS layout
  equals `GetCopyableFootprints(desc, first, k)` (row pitch a multiple of 256, subresource offsets on
  512 - true for BC2/3/5/6/7 mips of 64 px and more, BC1/4 of 128 px and more): one FILE request,
  `FORMAT_NONE`, source `{file, ptr + dataOffset + offset of the first kept mip, bytes}`, destination
  `MULTIPLE_SUBRESOURCES_RANGE {resource, first, k}` (DS 1.3). The skipped mips are simply not read.
  The tail (a few KB) is re-pitched on the producer thread from the mapping into a pooled block and
  goes through the MEMORY queue as a second `MULTIPLE_SUBRESOURCES_RANGE`; the block returns to the
  pool at completion.
- **LZO entries, custom codec.** One FILE request: source `{file, ptr, size_compressed}`,
  `CompressionFormat = DSTORAGE_CUSTOM_COMPRESSION_0 | 1 | skip << 1` (0x81..0x87: the request carries
  everything the codec needs, the DDS header is inside the stream), destination
  `MULTIPLE_SUBRESOURCES` from subresource 0, `UncompressedSize` = footprint total. Codec thread:
  `rtc_decompress_prefix` of the header -> needed bytes = header + all mips -> `rtc_decompress_prefix`
  of that many into a thread scratch -> re-pitch the kept subresources into `DstBuffer` front to back
  (write-combined when `DEST_IN_UPLOAD_HEAP`: written once, never read) -> `SetRequestResults`. Any
  inconsistency (size, header) answers `E_FAIL`.
- Status: requests of one texture are enqueued under the queue's mutex and followed by
  `EnqueueStatus(slot)`, so a slot covers exactly that texture (DS reports a status for everything
  enqueued since the previous status entry). A raw texture with a tail holds two slots.

### 3.5 Batches, completion, visibility

- `CTexture` gets `std::atomic<u8> m_ds_state` (idle / pending) and the slot/fence of its request.
  `pSurface` and `m_pSRView` are set **only at finish**, so nothing can bind a texture DS is still
  writing; the SRV can be created at finish (descriptor creation does not read the texture).
- Level batch: on R5 with DS, a pool worker's `CTexture::Load` does `Begin` and returns; workers
  `Submit` every 32 textures; the worker that finishes the list signals the batch fence and submits.
  `TextureUploadPool::Wait()` additionally waits for that fence (30 s timeout, then device-removed
  check and today's path) and finishes every pending texture; failed ones are loaded by today's path
  on the pool. Every existing caller of `Wait()` (unload, reset, shutdown) is covered by that.
- A pending texture reached by `Load`/`apply_load`/`surface_get` on another thread calls
  `da_dstorage::Finish(texture)`: wait for its slot, finish under the finish mutex.
- In game (synchronous): same `Begin` on the HIGH queues, `Submit`, wait for the slot
  (`IsComplete` with `SwitchToThread`, `EnqueueSetEvent` for LZO), finish. The contract of
  `CTexture::Load` stays "returns with the texture ready".
- GPU ordering: finish runs after the CPU saw completion, so every later DIRECT submission that reads
  the texture is ordered after DS's copy; the layer still tracks the texture as `COMMON` and emits
  COMMON -> shader read at the first bind (existing path). No cross-queue wait is needed.

### 3.6 Placed texture heaps (layer, helps both paths)

`TexturePageAllocator` in `d12_infra.cpp`, beside `BufferPageAllocator`: 256 MB heaps with
`ALLOW_ONLY_NON_RT_DS_TEXTURES`, first-fit free list of (offset, size) with coalescing,
`GetResourceAllocationInfo` for size and alignment (64 KB; 4 KB small placement where the call allows
it), frees through `DeferredReleaser`, an empty heap is released after a level unload. Used by
`InitTexture` for `IMMUTABLE`/`DEFAULT` SRV-only textures without RT/DS/UAV binds and without
`SHARED`; everything else stays committed. Measured 0.64-0.81 -> 0.15-0.24 ms per texture.

### 3.7 Numbers and diagnostics

- One line per batch on both paths: `* [texload] batch: 1161 textures (ds raw 612, ds lzo 520, cpu
  29, failed 0), 1233 MB, wall 812 ms, create 190 ms, codec 1320 ms cpu`.
- In-game synchronous loads counted per path (count, total, max ms), printed by `r__texload_stats` and
  once per level at unload.
- `-dstorage_verify N`: after a batch, N sampled DS textures are loaded again by today's path into
  scratch textures, both are read back and compared per subresource;
  `* [dstorage] verify: N textures, M mismatches` (mismatching names logged).
- Tracy zones `dstorage.begin`, `dstorage.codec`, `dstorage.finish`.

### 3.8 Geometry (optional, later)

`level.geom`/`level.geomX` vertex and index buffers go from the stream straight into
`CreateFromData` (`r2_loader.cpp:333-407`, raw, 60-100 MB per level). DS could read them into
`DEFAULT` buffer pages with `DESTINATION_BUFFER`; it needs the native range of a suballocated buffer
(`NativeBufferRange`, `infra.md` 3.2). In scope as the last milestone (PLAN S9), after textures, with
its own load A/B; merged only if it gains.

As implemented (S9): a `DEFAULT` buffer created **without** initial data is its own committed resource in
`COMMON` in the layer (a page range exists only for buffers created with data), so `NativeResource` gives the
request's destination and no page range is needed; the price is that these buffers are layer-tracked
(`NativeBufferRange(...).tracked`), i.e. one state check per bind and a COMMON -> read barrier per
submission in which they are drawn. Details and the gate in 13 (S9).

### 3.9 GDeflate: what is and is not possible

- Shipped archives cannot use it: entries are LZO or raw, and the format of an entry is implied by
  `size_real != size_compressed`. Stock and DAR2 archives are not ours to re-pack.
- Our content bundles could be re-packed, but not worth it: the HD BC7 textures shrink only to 0.833,
  the raw direct path already reads them at disk speed without CPU, and a new entry format would need
  DS's CPU codec on DX11 (a new DX11 dependency) and new bundle hashes for every player.
- A local GDeflate cache of the LZO textures (transcoded in `appdata`, about 2.7 GB) would move the
  LZO cost to the GPU; it is a separate experiment, not planned: the LZO share of a batch overlaps
  the main thread's level parsing once the lock and committed resources are gone.

## 4. Integration points

| File | Change |
|---|---|
| `src/Layers/xrRenderPC_R4/da_dstorage.h/.cpp` (new) | runtime, queues, files, codec pool, `Begin`/`Finish`, stats, verify; `#ifdef XR_D3D12` body, stubs otherwise |
| `src/Layers/xrRenderPC_R4/sources.cmake` | the two files |
| `src/Layers/xrRenderDX11/dx11Texture.cpp` | `texture_load` split: `resolve_texture_name` (name logic, shared), DS attempt, today's path unchanged |
| `src/Layers/xrRenderDX11/dx11SH_Texture.cpp`, `xrRender/SH_Texture.h` | `m_ds_state`, finish hook in `Load`, `apply_load`, `surface_get`, `Unload` |
| `src/Layers/xrRender/ResourceManager.cpp` | `TextureUploadPool::Wait` waits for DS and finishes; batch submit by the last worker; `[texload]` line |
| `src/Layers/xrRenderDX12/d12_layer.h`, `d12_infra.cpp`, `d12_objects.cpp` | `TexturePageAllocator`, `InitTexture` uses it |
| `src/Layers/xrRenderDX11/dx11HW.cpp` | shutdown hook in `DestroyDevice` |
| `src/Layers/xrRender/xrRender_console.cpp` | `r__dstorage` (both modules), `r__texload_stats`, `dstorage` key of `r__caps` |
| `src/Layers/xrRenderPC_R4/da_upscaler.h/.cpp` | `UpscalerCaps::dstorage` filled by the DS hook (the `r__caps` plumbing; `infra.md` 3.1 reserves the key) |
| `src/xrCore/Compression/lzo1x_decompress_safe_fast.inl`, `rt_compressor.cpp/.h`, `xrCore.def` | `rtc_decompress_prefix`, exported by the next free ordinal (1519 today) |
| `cmake/XRay.Dependencies.cmake` | `Microsoft::DirectStorageHeaders` (interface target), runtime files staged by `xray_stage_windows_runtime` |
| `packaging/dead-air-x64/installer/runtime-files.txt` | `dstorage.dll`, `dstoragecore.dll` |
| video options (`dead_air_x64_video_options.ltx`, `ui_mm_opt.xml`, `ui_mm_opt_16.xml`, `ui_mm_opt_video.script`, `text\rus|eng\dead_air_x64_video.xml`) | one row, section 7 |
| `tools/qa/dstorage/` (new), `tools/qa/water/texture_load.lua` (new), `tools/qa/d3d12layer` | probe, stand scenario, layer scenario |
| `docs/dead-air/DX12_RENDERER.md`, `DEPENDENCIES.md` | DS section, dependency row and obligations |

New engine files carry the Grizzly/MIT header; the vendored Microsoft files stay as they are.

## 5. Data and resources

- `Externals/DirectStorage/` from NuGet `Microsoft.Direct3D.DirectStorage` **1.3.0** (package SHA-256
  `8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177`, downloaded to
  `_work\sdk-downloads\dstorage`): `include/dstorage.h`, `dstorageerr.h` (MIT, `LICENSE-CODE.txt`);
  `bin/x64/dstorage.dll` (206,904 bytes, 1.3.2506.2501, SHA-256 `cae65787...b272a`),
  `dstoragecore.dll` (1,482,784 bytes, SHA-256 `cf85b19a...fdd11`), both Authenticode-signed by
  Microsoft; `LICENSE.txt`, `distributable_files.txt`, `README.md`. 1.4 is preview only (Zstd, GACL)
  and not used.
- Licence (`LICENSE.txt`, Microsoft Software License Terms, DirectStorage SDK): the two DLLs are
  distributable; we must add significant primary functionality, make distributors and end users
  accept terms at least as protective (recorded in `DEPENDENCIES.md` like the NVIDIA binaries'
  obligations - the installer has no licence page, PLAN C22), not use Microsoft trademarks to suggest
  endorsement, not modify them.
  Telemetry is switched off by configuration (3.1).
- Memory: DS staging 64 MB (partly video memory), status ring 8192 entries, tail block pool up to
  8 MB, codec scratch per thread up to the largest DDS (43 MB), trimmed after a batch.
- No archive change, no new file in `appdata`, no new download.

## 6. Shaders

None. (GDeflate decompression would run inside DS's driver metacommand or its own fallback shader;
not used.)

## 7. UI and presets

- Not a quality-preset setting (it changes no pixel), not in `rspec_*.ltx`, not in the RTX category.
- `r__dstorage` (bool, default 1, saved), registered by R4 and R5; R4 ignores it. Takes effect at the
  next texture load, no restart. `r__caps` gains `dstorage=1` when the runtime is loaded and the
  queues exist on this device (0 on R4, without `dstorage.dll`, with `-no_dstorage`).
- Video tab: `dstorage = check, r__dstorage` in `[rows]`, listed in `group_quality` after
  `level_texture_unload` (the other loading setting), caption `ui_mm_da_dstorage`
  ("DirectStorage texture loading" / "Загрузка текстур через DirectStorage"), hint
  `st_da_x64_video_hint_dstorage`. The row is always shown; `InitStates` greys it with
  `st_da_x64_video_hint_dstorage_dx11` on DX11 and `st_da_x64_video_hint_dstorage_unavailable` when
  `dstorage=0` on DX12. Nodes `check_dstorage` in `ui_mm_opt.xml` and `ui_mm_opt_16.xml`
  (options_item `r__dstorage`, group `mm_opt_video`, applied at once). Strings in cp1251.
- Command line: `-no_dstorage`, `-dstorage_buffered`, `-dstorage_verify N`.

## 8. Fallback

| Condition | Result |
|---|---|
| DX11, `-no_dstorage`, `r__dstorage 0` | today's path, `dstorage=0` |
| `dstorage.dll`/`dstoragecore.dll` missing or refusing (factory/queue creation fails) | one log line, today's path for the session |
| loose file, XMS overlay, `.seq`/video, unsupported header, footprint > staging | today's path for that texture |
| request or codec failure (status slot) | texture released, today's path for it, counted as `failed` |
| fence not signalled in 30 s | device-removed check; removed -> the engine's removal handling; otherwise today's path for the pending textures, DS off for the session |
| after that timeout (fix phase, `ds-timeout-fallback`) | no second wait: every pending ticket takes one `IsComplete` look, what is not complete loads the usual way at once. Requests still in flight are **parked**, never freed under DirectStorage: the ticket's texture, re-pitched tail and status entries (a level geometry's buffers and entry) stay alive until their entries complete (swept after each batch, at level unload, by `r__texload_stats` and at shutdown) or for the process. Shutdown after a timeout waits at most 1 s per queue and stops at the first that does not finish; a runtime still busy is left alive (queues not closed, codec threads stopped) instead of blocking the exit |
| HDD | buffered DS (OS cache) |

## 9. Test plan

Build `Invoke-Ninja 'xrCore xrRenderPC_R5 xrRenderPC_R4'`; `tools\qa\d3d12layer\Run-LayerTests.ps1
-HighPerformance`: 36 scenarios unchanged plus a new one (a placed `DEFAULT` texture written by a
native copy-queue copy, left in `COMMON`, first sampled from a deferred list) - pass: all equal to
D3D11 / the reference readback.

Stand runs (`Invoke-Stand`, save `qa_pedro`, `-ExtraArgs '-r5 -always_active'` unless stated):

| # | Run | Pass |
|---|---|---|
| 1 | `-Scenario render_check -ExtraArgs '-r5 -always_active -dstorage_verify 512'` | `[dstorage] verify: 512 textures, 0 mismatches`; `[texload] batch` with ds raw and ds lzo > 0 and failed 0; `[qa] caps ... dstorage=1`; no FATAL |
| 2 | `render_check` twice: `-PreCommands 'r__dstorage 0'` and default, same weather | the `r__capture` frames differ no more than two runs of the same setting; FPS within 2% |
| 3 | `-Scenario texture_load` (new: waits for the level, prints `r__texload_stats` and the `[texload]` line, plays 30 s, prints again), 3 interleaved runs per `r__dstorage 0/1` | median batch wall at least 25% shorter and the `net_start_client5` phase shorter with DS; in-game sync loads: DS p95 <= today's p95, 0 failed |
| 4 | `render_check -ExtraArgs '-r4 -always_active'` | `dstorage=0`, no `[dstorage]` initialisation, `[texload]` path cpu, FPS as before |
| 5 | fallbacks: `-no_dstorage`; one run with `dstorage.dll` renamed in the rig (restored after); `-dstorage_buffered` | game loads, the log names the path, run 1's verify passes where DS is on |
| 6 | `upscale_check` and `motion_check` with DS on (DLSS, FSR, both frame generations) | no FATAL, no DS error lines (Streamline's device hook and DS's queues coexist) |
| 7 | level change inside a run (`texture_load` with `qa_level_change = true`: `jump_to_level` to the neighbour and back) | `Level texture unload` releases, the second batch verifies, no leak in `RM_Dump` counts |
| 8 | `-dxdebug` run of #1 | no D3D12 debug-layer errors from DS copies or the COMMON hand-off |

Offline: `tools\qa\dstorage\Run-DsProbe.ps1` (the probe, landed) on `robocopy /J` cold copies of one
raw bundle and one LZO archive: DS at least 2x today's path on the cold raw batch, verify 0
mismatches. Frame time: `ab_run.ps1` at 2560x1440 Extreme with `r__dstorage 0/1` - per-pass GPU ms
equal (DS is idle after the load).

## 10. Performance expectation

- Level load, warm cache: the texture wait at the end (295-873 ms `net_start_client5` phase on the
  stand) shrinks by 50-80%, i.e. 0.2-0.7 s per load; the batch then mostly hides behind the main
  thread's parsing.
- Cold cache (first start after boot): the raw half of a batch 2.5-3x faster; 1-2 s per first load.
- In-game synchronous loads: raw textures 2-4x faster each (no page faults, no CPU copy, placed
  resource), LZO about 1.2x (placed resource, no lock).
- Frame time: none. DS copies run on its own copy queue and only while something loads.
- Unchanged: DX11; LZO-heavy content remains CPU-bound (stock + DAR2 are 47% of the texture bytes).

## 11. Risks

- NVIDIA Streamline wraps the device; DS creating its queues on `NativeDevice` must not upset DLSS-G
  (test 6). Same for the FSR frame-generation swap chain.
- Unbuffered IO on HDDs and on some network/virtual disks is slow: seek-penalty detection +
  `-dstorage_buffered`; a request failure on odd file systems (compressed NTFS, FAT) falls back per
  texture.
- The pending state is new synchronisation around `CTexture`; `flags.bLoaded` is already set before a
  surface exists today, so every reader of `pSurface` must go through the finish hook.
- `xrCore.def` ordinal collisions with other features in flight: take the next free ordinal at merge.
- Placed-heap fragmentation across many level changes: free lists + releasing empty heaps; VRAM of
  the 8 GB laptop part is watched with `QueryVideoMemory` (`infra.md`).
- GPU removal while requests are in flight: fence timeout path (8).
- Mapped header peek on a cold cache costs one 4 KB page fault per texture on the producer (about
  0.1 ms on NVMe); acceptable, a header cache is the fallback plan if it shows up.
- Licence obligations of the Microsoft binaries (5).

## 12. Implementation order

1. Land the probe as `tools/qa/dstorage/` (+ `Run-DsProbe.ps1`), vendor `Externals/DirectStorage`,
   CMake target, runtime staging, `runtime-files.txt`, `DEPENDENCIES.md` row.
2. xrCore: `xr_lzo` prefix decode with clipped literal run, `rtc_decompress_prefix`, `xrCore.def`.
3. Measurement first: `[texload]` batch line and in-game sync-load stats on today's path (both
   modules) - the baseline for every later step.
4. Layer: `TexturePageAllocator` + `InitTexture`; layer scenario; stand #1-#3 on today's path.
5. `da_dstorage`: runtime, configuration, queues, files, status ring, codec pool; `texture_load` split
   and eligibility; raw direct + tail; LZO codec; `CTexture` pending state; `TextureUploadPool`
   integration; verify mode.
6. In-game synchronous path on the HIGH queues.
7. `r__dstorage`, `r__caps` key, `r__texload_stats`, menu row and strings.
8. `texture_load.lua`, the full test plan, A/B numbers into `DX12_RENDERER.md`.
9. `level.geom` through `DESTINATION_BUFFER` (3.8), with its own load A/B; merged only if it gains
   (PLAN S9).

## 13. Implementation log (measured)

### S1 - probe in the tree

`tools/qa/dstorage/dsprobe.cpp` + `Run-DsProbe.ps1` (cl.exe /W4 over `Externals/DirectStorage`, output in
`build\qa\dstorage`; holds `stand.lock`, `-Lock all` adds `ninja.lock` for timed batches). Variants `cpu`,
`ds`, `ds-direct`, `ds-placed`, `dsbuf`, `dsmem`, `gdeflate`; `-Cold` reads `robocopy /J` copies;
`-PrefixCheck` is the S2 unit check. Gate run `ds-s1-probe` (machine shared with eight lanes): every
variant verified 24 read-back textures with 0 mismatches and 0 request/codec failures.

| Batch | cpu | ds | ds-direct | ds-placed |
|---|---|---|---|---|
| HD bundle `_textures_02`, 248 textures, 696.5 MiB raw | 594 ms | 398 ms | 383 ms (696.0 MiB direct, 0.44 MiB tails) | 392 ms (creation 48 ms vs 121-147 committed) |
| `textures.xdb2`, 295 textures, 510 MiB (303 MiB LZO) | 487 ms | 480 ms | 692 ms | 643 ms (codec 2.1 s CPU over 4 threads) |

### S2 - `rtc_decompress_prefix` (xrCore ordinal 1519)

`xr_lzo::decompress<Prefix>` in `lzo1x_decompress_safe_fast.inl`: the safe decoder is the `<false>`
instantiation (only its output-overrun labels differ, so its loop is unchanged); the prefix decoder clips
the literal run or the match that crosses the window end instead of refusing it. Exported as
`size_t rtc_decompress_prefix(void*, size_t, const void*, size_t)` (bytes produced, 0 for a broken stream)
at ordinal 1519, declared in `Compression/rt_compressor_prefix.h` (not `rt_compressor.h`, which every
module's precompiled header includes). Unit check `ds-s2-prefix`: all 9,696 LZO `.dds` entries of the 45
stand archives, windows 1/37/148/256/4097/65536 bytes, inline decoder and the xrCore.dll export by
ordinal: exactly min(window, size) bytes equal to the full decode, no write past the window, 0
mismatches. The safe decoder stops short of a DDS header for 2,689 of the 9,696 entries in a 256-byte
window (9,277 at 148 bytes); the "12 at 64 KB" of 3.3 did not reproduce (0 at 64 KB and at 4 KB), so the
check covers every entry. Decode speed of the safe decoder unchanged within the machine's noise
(`textures.xdb0`, 1446 MB, best-of interleaved: 2191-2685 ms new vs 2327-2716 ms old).

### S3 - `[texload]` numbers on today's path (both modules)

`texture_load` notes every load (wall, read = archive open + LZO inflate, create = the resource
creation, which also pages raw entries in; bytes); the upload pool reports one line per batch
(`* [texload] batch early|final: N textures queued, M loads (ds raw, ds lzo, cpu, failed), MB, wall, wait,
create, read, codec ms cpu, workers`), `DeferredUpload` prints `final upload call` and the level's
synchronous loads are kept apart in two groups: while the level still loads (until `bReady` and the
precache frames are over) and in play (count, total, max, p95 per path). `r__texload_stats` prints the
last batches and both groups; a level unload (or the next level load) prints them once. Scenario
`tools/qa/water/texture_load.lua` (stats, twelve stock items spawned next to the player, 20 s, stats).

Baseline (1920x1080, the stand's Extreme settings, warm file cache, runs `ds-s3-texload-*`):

| Level (save) | API | early batch: loads, MB, wall, wait, create CPU | final upload call | sync loads while loading | in play (after the spawn) |
|---|---|---|---|---|---|
| l01_escape (`qa_pedro`) | R5 | 1139, 1419 MB, 1268-1571 ms, 391-461 ms, 4.6-5.7 s | 445-564 ms | 758, 589 MB, 2225-2326 ms, p95 10.8-11.0 ms | 69, 98 MB, 296-307 ms, p95 13.6-15.0, max 22-24 ms |
| l01_escape (`qa_pedro`) | R4 | 1139, 1419 MB, 630 ms, 0 ms, 2.0 s | 82 ms | 758, 589 MB, 1310 ms, p95 8.5 ms | 69, 98 MB, 235 ms, p95 12.3, max 24 ms |
| k00_marsh (`fogqa`) | R5 | 1205, 1454 MB, 1371-1449 ms, 391-557 ms, 5.0-5.3 s | 443-629 ms | 623-631, 480 MB, 1829-1956 ms, p95 10.8 ms | 67, 95 MB, 280-294 ms, p95 14.1-14.2, max 26-28 ms |
| k00_marsh (`fogqa`) | R4 | 1205, 1454 MB, 656 ms, 0 ms, 2.0 s | 46 ms | 623, 479 MB, 1140 ms, p95 8.2 ms | 67, 95 MB, 199 ms, p95 12.5, max 19 ms |

What the baseline says:
- On DX12 today a texture costs about 2.5x the CPU of DX11 to create (4.6-5.7 s vs 2.0 s for the same
  1139-1205 loads): the layer's committed resource per texture and the row copy under the uploader's one
  lock (2.3). The early batch no longer hides behind the main thread on R5 (wait 0.4-0.6 s, R4 0).
- The largest texture cost of a load is not the batch: 620-760 textures are created synchronously on the
  main thread after the final upload (objects coming online in `net_start_client6` and the first frames,
  `bDeferredLoad` is already off): 1.8-2.3 s on R5, 1.1-1.3 s on R4. S6's synchronous DS path addresses
  them; S8's A/B reports this group next to the batch.
- R4 is measured on the same code (the numbers are logging only; `ds-s3-render-pedro-r4` equals Gate 0).

### S4 - placed texture heaps (`d12_texheap.cpp`)

`TexturePageAllocator`: 256 MB heaps with `ALLOW_ONLY_NON_RT_DS_TEXTURES`, first fit with coalescing, 64 KB
placement, 4 KB (`D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT`) where the most detailed mip spans at most 16 standard
tiles - checked before asking `GetResourceAllocationInfo`, which the debug layer reports for an ineligible
description. Textures above 64 MB stay committed (a few per level; they would fragment the heaps). The resource and
its range are released together through the releaser once the frame fence passed; a heap that empties is released
when another empty heap is kept, so a level change does not create and destroy heaps back to back. Placed:
SRV-only `IMMUTABLE`/`DEFAULT` textures created with initial data, and textures created while the thread set
`xrd3d12::SetStreamedTextures(true)` (DirectStorage writes every subresource before first use; any other
texture without data stays committed, because a placed range keeps what an earlier texture left there).
`xrd3d12::GetTextureHeapStats` feeds `r__texload_stats`. Layer scenarios (`scenarios_texheap.cpp`): textures
with data placed (RGBA8 with 9 mips, BC1, a 16x16 at 4 KB, a cube) and read back through draws against D3D11, with
the ranges of a first round freed and reused by a second; a streamed texture placed without data, written by a
native COPY-queue copy while in COMMON and first sampled from a deferred context list. Layer tests: **70 passed,
0 failed**, 0 debug-layer warnings (every other scenario now runs on placed textures too).

Stand (R5 `qa_pedro`, today's path with `-no_dstorage`): the early batch's creation CPU fell to 4084-4534 ms
from 4.6-5.7 s committed (S3), the final upload call to **50-53 ms** from 445-564 ms, and the wait of the joining
thread to 0 from 391-461 ms. Frames: `ds-s4-render-pedro-g0ovl` (tree binaries, Gate-0 overlay, every other lane
masked by `-dx12_no_rt/vrs/ms/async/sm6` and `r__gpu_driven 0`) equals Gate 0 with the control
`ds-s4-ctl-pedro` (99.95 % within, largest cluster 9 px, 58.5 dB). With the working tree's overlay the frame
fails Gate 0 through other lanes' shader edits (a uniform speckle, no texture-shaped difference), which is why
parity is judged on the Gate-0 overlay.

### S5 - DirectStorage textures (`da_dstorage.cpp`)

- Runtime: `dstorage.dll` by absolute path next to the executable (`LOAD_WITH_ALTERED_SEARCH_PATH`, so
  `dstoragecore.dll` comes from the same folder), `DStorageSetConfiguration1` (telemetry off, no built-in CPU
  decompression threads, `DisableBypassIO`/`ForceFileBuffering` with `-dstorage_buffered` or on a volume
  with a seek penalty), staging 64 MB, `SHOW_ERRORS` with `-dxdebug`. Loaded on the first
  `da_dstorage_available()` (the `r__caps` hook at renderer start) or the first eligible texture, whether
  `r__dstorage` is ticked or not, so the Video row can be ticked again; `-no_dstorage` never loads it.
- Two queue pairs on the native device: batch (NORMAL, 8192) and synchronous (HIGH, 1024), each a FILE queue and a
  MEMORY queue (re-pitched tails) with a fence. One status array of 8192 entries with a free list; a texture's
  requests and its status entries are enqueued under the pair's lock, so an entry covers exactly that texture.
- Eligibility (else today's path, silently): R5, runtime up, `r__dstorage 1`, the name resolves (the shared
  `texture_resolve_name`, split out of `texture_load`) to an archive entry whose archive is mapped (loose
  files and module overlays keep today's path), the header parses to a layout both of today's paths create
  without converting texels (BC1-7 incl. sRGB, RGBA8/BGRA8/BGRX8; legacy RGB masks only where DirectXTex copies
  them unchanged; no volumes; block-compressed top mip after the skip a multiple of 4; DirectXTex's mip-count
  rule), every request within the staging size. The mip skip is today's (`get_texture_load_lod`, cubes keep all
  mips), `MemoryUsage` today's `calc_texture_size`.
- Header: raw entries from the mapping, LZO entries through `rtc_decompress_prefix` (256 bytes).
- Raw entries: per slice the leading kept mips whose tight layout equals `GetCopyableFootprints` go in one FILE
  request (`MULTIPLE_SUBRESOURCES_RANGE`) straight into the texture; the rest of the slice (a few KB) is
  re-pitched from the mapping into the ticket's block and goes through the MEMORY queue. LZO entries: one FILE
  request with custom format `0x80 | 1 | skip << 1`; a dispatcher thread hands the codec's requests to
  `clamp(threads, 4, 10) - 2` codec threads (8 here) that inflate header + body with `rtc_decompress_prefix`
  and re-pitch the kept subresources front to back into DirectStorage's staging memory.
- Textures: `DEFAULT`, SRV, no initial data, created with `SetStreamedTextures(true)` (placed, COMMON). The
  surface and view of `CTexture` are set only when it finishes: on an upload-pool worker `Load` keeps the
  ticket in `CTexture::ds_ticket` and returns; the pool's last worker drains the batch pair (fences), runs the
  verify pass and finishes every texture of the batch before the batch counts as done, so `Wait()` (and every
  caller of it: final upload, unload, reset, shutdown) sees them ready. A thread that needs a pending texture
  first (`Load`, `apply_load`, `surface_get`, `desc_enshure`, `GetImTextureID`, `Unload`) finishes it
  itself under the finish lock. A failed status entry releases the texture and today's path loads it (counted as
  `failed`). A wait that exceeds 30 s logs the device state and turns DirectStorage off for the session (what
  happens to the requests still in flight: section 8 and the fix-phase note at the end of 13).
- `-dstorage_verify N`: N tickets spread over each batch are read back through a COPY queue of our own (before
  their textures become visible) and compared per row with the DDS body read by today's path (`FS.r_open`);
  synchronous loads are all verified in that mode. `* [dstorage] verify: ...` per batch and per level. (Fix
  phase: the batch tickets are picked when they are created and verified by `da_dstorage_end`, see the end of 13.)

Gate run `ds-s5-verify-pedro` (R5 `qa_pedro`, `-dstorage_verify 512`, Gate-0 overlay, other lanes masked):
`[dstorage] verify: 512 textures, 0 mismatches (batch of 1086)`, the final batch and the synchronous loads
0 mismatches; early batch **ds raw 478, ds lzo 608, cpu 53, failed 0** (the 53 are formats DirectStorage leaves to
today's path); `r__caps ... dstorage=1`; no FATAL; frames equal Gate 0 with the control `ds-s4-ctl-pedro`
(99.94 %, largest cluster 56 px within the calibrated limit, 56.8 dB). Creation CPU of the batch: 653 ms (placed
without data) against 4.1-4.5 s with data; the verify pass (512 readbacks and re-reads) runs inside the batch,
so this run's wall (1901 ms) is not a timing - S8 times the batch without it.

### S6 - synchronous loads

A texture loaded outside the upload pool (`CTexture::Load` on the main or render thread) takes the synchronous
queue pair (HIGH priority): its requests are submitted at once and the thread waits for its status entries
(spin, then `SwitchToThread`/`Sleep(0)`, 30 s limit), so `Load` still returns with the texture ready. Run
`ds-s6-texload-pedro` (R5 `qa_pedro`, `texture_load`, the working tree with every lane at its defaults)
against the S3 baseline of today's path:

| | S3 today (R5) | S6, every eligible texture through DirectStorage |
|---|---|---|
| early batch wall / wait / creation CPU | 1268-1571 / 391-461 / 4607-5674 ms | **617 / 0 / 666 ms** (codec 461 ms CPU on 8 threads) |
| final upload call | 445-564 ms | **81 ms** |
| sync loads while loading | 758, 2226-2326 ms, p95 10.8-11.0 | 758, 2339 ms: raw 125 p95 **7.0**, LZO 598 p95 **14.5**, cpu 35 |
| sync loads in play | 69, 296-307 ms, p95 13.6-15.0 | 69 (all LZO), 384 ms, p95 **15.7** |

Raw entries win synchronously (no page faults, no row copy); a single LZO texture loses, because it waits for an
unbuffered read, the codec thread hand-off and a copy on DirectStorage's queue one texture at a time, where
today's path inflates it straight from the warm mapping. Decision (tuning within the design): synchronous loads
take DirectStorage for **raw entries only**; LZO entries load synchronously the usual way (inline, from the
mapping, into a placed texture). The upload batch keeps DirectStorage for both, where the codec threads run in
parallel and the uploader lock is gone.

Code after the decision (implemented, untested - final test phase):
- `da_dstorage_begin` returns no ticket for an LZO entry outside a batch worker; the raw sync path is unchanged.
- A thread that needs a batch texture before its batch completed (`ds_finish` from `Load`/`apply_load`/
  `surface_get`/`desc_enshure`/`Unload`) first submits the batch queue pair when the ticket's status entries are
  not complete yet: a batch submits only every 32 textures, and a wait on requests still sitting unsubmitted would
  have run into the 30 s timeout and switched DirectStorage off for the session.
- `texture_resolve_name(name, fn, quiet)`: DirectStorage resolves quietly and leaves a missing texture or bump map
  to today's path, which logs its fallback once (before, a declined texture logged it twice).

Final test phase (PLAN S6 gate, budget 0.1a): R5 `qa_pedro` `texture_load` with the tree at its defaults, one run
per `r__dstorage 1` / `0` (the S3 rows are the older baseline): in-play `ds raw` p95 <= the `cpu` p95 of the
`r__dstorage 0` run, `failed 0`, no `! [dstorage] ... did not complete` line.

### S7 - `r__dstorage`, `da_dstorage_available`, `r__texload_stats`

Implemented, untested.
- `r__dstorage` (core registers it in both modules: saved, default 1): read at every `da_dstorage_begin` and
  `da_dstorage_load_buffers`, so it applies to the next texture load and the next level's geometry without a
  restart; R4 never takes the DirectStorage path whatever it says.
- `da_dstorage_available()` (the `r__caps dstorage=` key through core's caps hook): loads the runtime on the first
  call (R5, not with `-no_dstorage`) whether `r__dstorage` is ticked or not, and answers whether DirectStorage can
  run this session: runtime + queues up and no wait timed out (a timeout turns it off for the session, and the
  Video row then greys itself with the "unavailable" hint instead of offering a switch that does nothing).
- `r__texload_stats` (`da_dstorage_log_stats`): DirectStorage state (on/off, buffered or not, loads raw/LZO/failed,
  MB direct and tails, codec failures), texture heaps (heaps, placed textures and MB, placed and too-big totals),
  `live textures` (every `CTexture` alive; the leak check of a level change), the last four batch lines and the
  level's synchronous loads (while loading / in play).

Final test phase (dstorage 9 runs 2, 4, 5 within 0.1a): run 2 = the S6 pair above (frames of `render_check` are
not needed again: DS changes no pixel, S4/S5 proved parity); run 4 = one R4 `render_check` (`dstorage=0`, no
`[dstorage]` line, batch `cpu` only); run 5 = one R5 run with `-no_dstorage` (log `off (-no_dstorage)`, `dstorage=0`)
and one with `dstorage.dll` renamed in the rig (`! [dstorage] not available (dstorage.dll not loaded)`, game loads);
`-dstorage_buffered` goes into the S8 runs' command line of one pair member, not a run of its own.

### S8 - `texture_load.lua` and the A/B

Implemented, untested. `tools/qa/water/texture_load.lua`: caps, `r__texload_stats`, twelve stock items spawned next to
the player, `qa_texload_ms` (20 s) of play with an FPS window, stats again. With `qa_level_change = true` it jumps
to `qa_level_to` (default `l02_garbage`) and back and prints the stats after `qa_texload_level_ms` (8 s) on each
level. The level change reconnects and restarts the script engine (a new ALife simulator), so the phase and the
first level's name live in the actor's saved variables (`save_var`/`load_var`, carried by the level-change
autosave); the file's locals start over on every level.

A/B switch: `r__dstorage` through `-PreCommands 'r__dstorage 0'` (it is saved, read at the load), everything
else equal. Final test phase (runs 3, 6, 7, 8 + the offline cold probe, within 0.1a):
- run 3: `texture_load` on `qa_pedro`, `r__dstorage 1` vs `0`, one pair (the perf rule's single batch if the
  pair is inconclusive: `Invoke-Dx12Perf -Pairs 3` with `r__dstorage` as the A/B). Pass: early batch wall at least
  25 % shorter with DS, `final upload call` shorter, in-play sync p95 not worse, `failed 0`.
- run 6: `upscale_check` once with DS on (DLSS + frame generation): no FATAL, no `! [dstorage]` line.
- run 7: `texture_load` with `-Globals 'qa_level_change = true'` and `-dstorage_verify 256`: each batch
  `verify: N textures, 0 mismatches`, `live textures` of the two `l01_escape` visits within a few (no growth per
  visit), texture heaps back to the same count.
- run 8: run 1 with `-dxdebug`: no D3D12 debug-layer errors from DirectStorage copies or the COMMON hand-off.
- offline: `Run-DsProbe.ps1 -Cold` on one raw bundle and one LZO archive: DS >= 2x today's path on the cold raw
  batch, verify 0 mismatches.
- frame time: covered by S9's frame A/B (the same switch).

### S9 - `level.geom` through `DESTINATION_BUFFER`

Implemented, untested. `da_dstorage_load_buffers` (called by one guarded hook block at the top of
`CRender::LoadBuffers`, `r2_loader.cpp`): when DirectStorage runs, `r__dstorage` is 1, the build is not a DEBUG
build and `level.geom`/`level.geomx` is a raw (not LZO) entry of an archive, it walks the file's top-level chunks
to the vertex and index buffer chunks, parses them exactly like `LoadBuffers` (declarations, counts, the same
`[Loading VB/IB]` lines), creates every buffer through `CreateFromData(nullptr, size)` - on R5 a `DEFAULT` buffer
without data is its own committed resource in `COMMON` - and reads each buffer's bytes from the archive into it
with `DESTINATION_BUFFER` requests of at most 16 MB on a queue pair of its own (NORMAL priority, so its wait does
not include the texture batch running beside it). The loading thread waits for the pair's fence (30 s limit);
a failed request, a timeout or a verify mismatch releases every buffer and today's path loads them again from the
stream. `-dstorage_verify N` reads every geometry buffer back through the verify copy queue and compares it with
the stream (`[dstorage] verify: N geometry buffers, M mismatches`). One line per file on either path:
`* [texload] geometry level.geom: <n> buffers, <MB> MB, <ms> ms (ds)` or `* [texload] geometry level.geom: <ms> ms (cpu)`.

Decisions:
- `NativeResource` instead of `NativeBufferRange`: a buffer created without data has its own resource, so
  there is no page range to address; creating them without data is what lets DirectStorage write them.
- DEBUG builds (Debug, Mixed) keep today's path: the engine's staging buffers `VERIFY` initial data in
  `CreateFromData` (`dx11BufferUtils.cpp`, not this lane's file); Release is what ships and what the gates run.
- The buffers are layer-tracked (one state check per bind, a COMMON -> vertex/index barrier in every submission
  that draws them). Code that reads them natively must honour `NativeBufferRange(...).tracked` and `Require` them
  (rt told: BLAS inputs).

Final test phase (keep/remove rule, PLAN S9 "merged only if the load A/B improves and the geometry verifies"):
- `-dstorage_verify 64` in the S8 run 7: `verify: N geometry buffers, 0 mismatches` for `level.geom` and
  `level.geomx` of both levels;
- the S8 run 3 pair gives the `[texload] geometry` line on both paths: DS must be shorter for `level.geom` +
  `level.geomx` together (median of the pair members; one repeat pair if they are within 10 %);
- frame time: one `Invoke-Dx12Perf` batch on `qa_pedro`, `-Pairs 3`, A = `r__dstorage 1`, B = `r__dstorage 0`
  (both at 2560x1440 Extreme per 0.5): mean frame time and the `[gpu]` fields equal within the batch's noise
  (the tracked buffers must not cost CPU/GPU time per frame); render-thread time equal within 1 %.
- If any of the three fails: remove S9 (the hook block in `r2_loader.cpp`, `da_dstorage_load_buffers`,
  `DaGeometryTimer`, the geometry queue pair) and write the numbers here; the texture path stays.

### Fix phase (28.09, review findings; code + build only, checks in the final test phase)

- **`ds-verify-uaf`** (real): `da_dstorage_batch_complete` verified tickets through the raw `(CTexture*, DaDsTicket*)`
  pairs of the batch list after the drain, without the finish lock, while any thread that needed one of those
  textures first could already have ended and deleted its ticket (`ds_finish` -> `da_dstorage_end` ->
  `ReleaseTicket`): a use-after-free in exactly the `-dstorage_verify` runs. It also checked only `IsComplete`, so a
  request that failed (and correctly fell back to today's path) counted as a mismatch. Now the batch list holds
  textures only (`da_dstorage_batch_add(CTexture*)`), the ticket's single owner is `CTexture::ds_ticket`.
  `da_texload_batch_begin` sets the stride (`max(1, queued / N)`) and resets a counter; `da_dstorage_begin` marks
  every stride-th batch ticket, at most N, as `verify`; `da_dstorage_end` (which owns the ticket, under the finish
  lock for batch textures) verifies it when its requests completed **and** succeeded, before the texture is
  published, into per-batch counters that `da_dstorage_batch_complete` prints after its finish loop in the same
  `* [dstorage] verify: N textures, M mismatches (batch of K)` line. The number picked depends on the queued count
  (1139 queued -> stride 2 -> 512 of 1086 DirectStorage tickets on `qa_pedro`).
- **`ds-timeout-fallback`** (real): after the first 30 s timeout every other pending ticket waited its own 30 s (a
  level batch of ~1100 textures would hang the load for hours), and a timed-out ticket's tail, texture and status
  entries - and the level geometry's buffers and entry - were freed while DirectStorage could still read or write
  them (the placed range of a released texture returns to the free list after a direct-queue fence that does not
  cover DirectStorage's copy queue). Now (section 8): once disabled, `da_dstorage_end` and the batch finish never
  wait again (one `IsComplete` look; the batch pair is only submitted); an incomplete ticket goes to the parked
  list with its texture, tail and status entries; a geometry drain that timed out parks the buffers (one reference
  each) and its entry; parked loads are released once their entries complete. `da_dstorage_shutdown` waits 30 s
  per pair normally, 1 s after a timeout, and leaves a still busy runtime alive. A verify readback that times out
  leaves its readback buffer to the copy queue and stops further verifies (`verifyStuck`). `Disable` logs once
  per session (an atomic exchange), whichever wait timed out first, also when several threads time out together.
- **`ds-nullptr-compare`** (real): the three `!= nullptr` tests in `da_dstorage.cpp` and `ds_pending()` in
  `SH_Texture.h` use the implicit bool conversion (`!!`).
- Final-phase checks unchanged: runs 1/7/8 still expect `verify: ... 0 mismatches` per batch; the timeout path
  cannot be provoked on the stand within the budget and is covered by review only.

## Final test phase, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

- `final-dsverify-pedro` (every feature on, `-dstorage_verify 4000`): `verify: 1086 textures, 0 mismatches (batch of
  1086)`, final batch 0 mismatches, `verify: 46 geometry buffers, 0 mismatches` (level.geom) and `18` (level.geomx);
  early batch `ds raw 478, ds lzo 608, cpu 53, failed 0`. Its frame against the same config with `-no_dstorage` +
  the Gate-0 references: calibrated "equal" (99.86 % within against the no-DS frame, largest cluster 10 px):
  DirectStorage changes no pixel.
- Level change with DirectStorage on (`final-robust-level-b/-c`): k00_marsh early batch `ds raw 452, ds lzo 285, cpu
  126, failed 0`, geometry `55 + 27 buffers (ds)`, no `! [dstorage]` line.
- R4: `dstorage=0`, batch cpu only (`final-r4-render-pedro`). The S6-S9 timing A/Bs are the Perf phase's.

## Final test phase, performance (Perf phase, 28.09.2026)

At the user's request the Perf phase ran one measurement pass instead of the `texture_load` pairs (PLAN 8.4), so
there is no DirectStorage on/off load-time verdict; the numbers below are what the path does with everything on.
- `final-onepass-foliage` (every DX12 feature on, `r__dstorage 1`): `[dstorage] runtime ready: unbuffered reads,
  staging 64 MB, 8 codec threads`; `batch early: 1181 textures queued, 1138 loads (ds raw 478, ds lzo 607, cpu 53,
  failed 0), 1417 MB, wall 678 ms, wait 0 ms`; geometry `level.geom: 46 buffers, 88 MB, 256 ms (ds)`,
  `level.geomx: 18 buffers, 21 MB, 58 ms (ds)`; `0 sync loads` until the final upload.
- Frame cost: DirectStorage works only while textures and geometry load (and changes no pixel, see the functional
  part); no per-frame A/B was run.
- Decision: DirectStorage stays on by default for textures and level geometry (S9 kept as built; its keep/remove A/B
  was not run). `r__dstorage 0` or `-no_dstorage` give the old path back.
