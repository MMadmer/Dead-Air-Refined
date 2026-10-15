# DXC / Shader Model 6 for R5

Status: implemented. D1 (audit tools, 6.1), D2 (shader fixes of 6.2, the constant-buffer
sharing fix, `Test-Shaders.ps1` / `Check-ShaderLinkage.py`), D3a (`da_dxc`, native compiles), D3b
(the engine DXIL path), D4 (perf DXC vs FXC: no pass regresses, nothing pinned to FXC) and D5 (payoff
edits of 6.4: the TAA quad moments kept, -3 to -5 % on the TAA pass; GTAO quad depths, PCSS and cloud
march in min16 and the `da_h` types measured and removed; `-all-resources-bound` behind the start-up
switch `-dxc_all_bound`, left as an unmeasured QA switch) are implemented (28.09.2026, numbers in 6.2,
6.4, 9 and 13); the final test phase passed (functional part and performance at the end). Scope: `xrRender_R5.dll`; `xrRender_R4.dll` keeps FXC, and its output
changes only where a shared shader fix of 6.2 says so. Everything below was measured on the development machine (RTX 3070
Laptop, driver with SM 6.8, Windows 11 26200) with DXC 1.9.2607 (`_work/sdk-downloads/dxc-1.9.2607`,
zip SHA-256 `a1dfb116...cd241`). Probe sources, audit script, patches and raw outputs are in
`<scratchpad>/dxc/` (`Check-DxcShaders.py`, `pso_probe.cpp`, `probe_mix.cpp`, `probe_typename.cpp`,
`probe_caps.cpp`, `shader-fixes.patch`, `payoff-candidates.patch`, `audit*.txt`, `pso*.txt`), where
`<scratchpad>` is `C:\Users\admin\AppData\Local\Temp\claude\D--Games-Dead-Air-DeadAir-x64\663ffd4e-302f-4d92-9244-b15c665cf3ee\scratchpad`.

Relation to the other feature designs: `infra.md` 3.6 (loader, `CompileShader`, native shaders,
opt-in `//grizzly:dxil`) and `dxr.md` 3.2 (`da_dxc`) describe parts of the same compiler path. This
document owns it; where it differs from them it says so (3.1, 3.4, 3.6) and the difference wins.

## Integration amendments (PLAN.md)

Implemented by the `dxc` lane (milestones D1-D5 of `PLAN.md` 3.3/4.7) except the layer service.
These points supersede the text below where they differ (ids refer to `PLAN.md` section 1):
- **Split of work**: the layer service `d12_dxc.cpp` and the layer's DXIL acceptance (`Shader::Reflect`,
  `ProgramType`, `Shader::dxil`, mix guard, harness `-Dxil` mode and the two `dxil_*` scenarios in
  `scenarios_dxil.cpp`) are built by the `infra` lane to this document's 3.1/3.4/3.6 (milestone I1).
  `Externals/dxc`, `XRAY_DXC_RUNTIME_FILES`, `runtime-files.txt`, `DEPENDENCIES.md` and the `r__dxc`
  registration / `dxc=` key are done by `core` (K1/K2); `dx11HW.cpp` calls `da_dxc_init` from a hook
  `core` pre-places. This lane owns `da_dxc.*`, the engine compile path, reflection aliases, twins,
  the shader fixes, the audit tools, `Test-Shaders.ps1` and `Check-ShaderLinkage.py`.
- **D3a before D3b**: `da_dxc_compile_native` and its cache
  (`shaders_cache_grizzly/r5_dxil/<tag>/native/<path>.<entry>.<target>/<defines hash>`) land first, right
  after I1, because the RT lane waits for them.
- **C5, mesh shaders**: `.ms` and `.as` files go through the engine compile path (FXC preprocessing,
  engine includes, the option macros, `SMAP_ARRAY`) with `-HV 2021` - they are new-only extensions
  written for DXC - and targets `ms_6_<m>` / `as_6_<m>` (`m` >= 5 required). A pass that contains a
  mesh or amplification stage is exempt from `UnifyShaderFormats`: if its PS fell back to FXC, the
  pass is reported unavailable (mesh grass then uses the vertex path). The `#dxil` suffix is not used.
- **C6**: `R_constant_table::parse` takes the constant-buffer slot from `BindPoint` for DXIL and
  asserts index == `BindPoint` for DXBC (one debug log line if not).
- **C4**: new engine-path shaders of other features (async/VRS compute shaders, GD tree VS variants)
  follow the session's mode like every engine shader; no FXC-only assumption remains.
- **C15, payoff edits**: the TAA edit stays in `da_taa.ps`; the GTAO and cloud edits are made after
  the `async` lane moved those bodies into `gtao.h` and `da_clouds_march.h` (shared with compute
  shaders). Their compute variants use a quad-swizzled thread-to-pixel mapping (lanes 4k..4k+3 form
  one 2x2 block) or keep the non-quad path under `DA_COMPUTE`; every payoff edit must keep
  `r__async_verify` stage port at 0 differences.
- **C25**: this lane owns `Test-Shaders.ps1` (`.cs` as `cs_5_0`, DXC mode incl. `.ms/.as`, skips
  `r3/rt/` and `r3/native/`) and `Check-ShaderLinkage.py` (DXIL rule, MS->PS pairs, GD permutations);
  other lanes add extra pipelines inside their `[dx12:<lane>]` blocks.
- **D4 rule** (PLAN 4.7): if DXIL loses more than 1 % against FXC, the regressing passes are fixed or
  pinned to FXC through the twin mechanism by an explicit list in `da_dxc.cpp`; the DXC path stays.
  Measured at D4 (section 9, test 12): no pass loses, so there is no pin list.
- **D5 rule** (PLAN 4.7, 7.2): each payoff edit is kept only with >= 0.05 ms or >= 3 % on its pass, test
  7's capture diff and `r__async_verify` port 0. Measured in one batch through the new `-dxc_plain`
  switch (section 7): only the TAA quad moments pass; the others are removed with their numbers in 6.4.
  Nothing of D5 is left in the async lane's `gtao.h` / `da_clouds_march.h`. The optional
  `-all-resources-bound` needs a batch of its own (PLAN 0.1b: all measurements in the final test phase):
  it is in `da_dxc.cpp` behind `-dxc_all_bound` and is decided there by the same rule (6.4).

## 1. Goal

- R5 compiles every engine shader (the `.vs/.ps/.gs/.hs/.ds/.cs` of `gamedata/shaders/r3`, all
  permutations) with DXC to signed DXIL at runtime, Shader Model 6.0..6.6 by device, instead of FXC
  DXBC. Same look, same bindings, same constant layout.
- One compiler service for every SM 6 consumer: engine shaders (this doc), DXR `lib_6_x` / inline
  `RayQuery` `cs_6_5` (`dxr.md`), mesh/amplification `ms_6_5`/`as_6_5` (`mesh.md`), native passes
  (`infra.md`).
- Robust: no DXC, an old device, a shader DXC refuses, a crash inside DXC - the game still renders
  (FXC), and a pipeline never mixes DXIL and DXBC stages.
- Payoff: first-run shader compilation (the heavy shaders compile 14-16x faster), and SM 6 wave /
  quad intrinsics and 16-bit math in the passes where they measurably help.
- Not goals: HLSL 2021 for the legacy shader base, the Agility SDK, any preset or menu change.

## 2. Current code

**Compile path** - `src/Layers/xrRenderPC_R4/r4_shaders.cpp` (compiled by both modules):
- `CRender::shader_compile` (250-712): builds the macro list (`options`, 253-575: external
  `m_ShaderOptions` from `addShaderOption`, then preset/HW options, `SMAP_ARRAY=1` under `XR_D3D12`
  at 570-575) and the permutation name `sh_name`; cache path
  `appdata/shaders_cache_grizzly/r5/<name>.<ext>/<sh_name>` (583-609, `r5` under `XR_D3D12`); `fileCrc` of
  the file and its includes (`getFileCrc32`, 611-616); cache read (618-642: u32 dx9compat, u32
  fileCrc, u32 bytecodeCrc, bytecode); `HW.D3DCompile` with the `includer` (644-667, X3523 retry with
  `D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY`); cache write (669-692); failure log with every define
  (694-705).
- `includer` (151-185): `FS.r_open("$game_shaders$", "r3\" + name)`, then `name` - always from the
  r3 root, never relative to the including file.
- `create_shader` (15-51, 53-149): `CreateXShader`, debug name for the layer (26-32), `D3DReflect` ->
  `constants.parse` (34-48); VS: `D3DGetInputSignatureBlob` -> `_CreateInputSignature` (70-77) and a
  second `D3DReflect` for `position_only` (79-105, `D3D11_SIGNATURE_PARAMETER_DESC`); `-disasm` via
  `D3DDisassemble` (133-146).
- Targets and flags: `ShaderTypeTraits<>::GetCompilationTarget` (`xrRender/ShaderResourceTraits.h`
  191-216, 262-301, 529-555; profiles from `dx11HWCaps.cpp` 159-209, `vs_5_0`/`ps_5_0`), legacy
  entries `main_vs_1_1`...; flags `PACK_MATRIX_ROW_MAJOR | OPTIMIZATION_LEVEL3` (692-704);
  `CResourceManager::CreateShader` (616-720) - map by name, `stub_default` fallback.
- Name keys: `_CreateVS` appends the skinning suffix, `_CreatePS` the MSAA sample
  (`xrRenderDX11/dx11ResourceManager_Resources.cpp` 43-67, 93-126); GS/HS/DS/CS 131-142.
- Pass assembly: `CBlender_Compile::r_Pass` (`xrRenderDX11/Blender_Recorder_R3.cpp` 133-176: PS
  first, a PS that needed DX9 compatibility forces the flag on its VS, 150-156), `r_TessPass`
  (179-189), `r_ComputePass` (191-196); `PassSET_Shaders` (`xrRender/Blender_Recorder.cpp` 205-232);
  `PassEnd` builds the constant table and the `SPass` (176-203).

**Engine reflection** - `xrRenderDX11/dx11r_constants.cpp`: `parseConstants` (11-181; FLOAT/BOOL/INT
only, anything else is `fatal`, 44-50; MATRIX_ROWS with 4 columns only), `parseResources` (183-256;
texture/sampler/RW typed, `VERIFY(BindCount == 1)`), `parse` (288-350) binds constant buffer number
`iBuf` of the reflection **at slot `iBuf`** (317-333, `rebuildConstantBufferBindings`,
`xrRender/r_constants.cpp` 179-208). `dx11ConstantBuffer` (ctor 17-68, `Similar` 70-97) shares
buffers of equal name/layout. Types come from `xrRenderDX11/CommonTypes.h` 243-254
(`ID3DShaderReflection = ID3D11ShaderReflection`, D3D11 desc structs).

**Layer** - `src/Layers/xrRenderDX12`:
- `d12_device.cpp`: `FindChunk` (140-163) walks any `DXBC` container (DXIL containers use the same
  header); `ReadInputSignature` (176-213) already reads `ISG1` (185-189); `ProgramType` (216-226) reads
  `SHEX/SHDR` only; `CreateShader<>` (571-610) copies the bytecode and calls `Shader::Reflect`;
  `CreateInputLayout` (897-1000).
- `d12_objects.cpp`: `Shader::Reflect` (1982-2035) = `D3DReflect` -> `ID3D11ShaderReflection`, fills
  `ShaderBindings` through `BindSlot` (1099-1145) and the VS input semantics.
- `d12_layer.h`: limits 58-70 (14 CB, 32 SRV, 16 samplers, 8 UAV), `GfxRoot`/`CsRoot` 447-473,
  `ShaderBindings` 941-953, `Shader` 955-971.
- `d12_infra.cpp`: one fixed graphics and one compute root signature, register space 0
  (`RootLayout` 352-400, `RootSignatures::Init` 2302-2346); PSO key from shader content hashes
  (`GraphicsPsoDesc::Hash` 2358-2382); `PipelineCache::Graphics` (2507-2593) logs and caches a refused
  PSO as null; `Compute` (2595-2646).
- Device creation from the engine: `xrRenderDX11/dx11HW.cpp` 193-226 (`xrd3d12::CreateDevice`).
- Loading SDK DLLs by absolute path: pattern of `xrRenderPC_R4/da_ffx_loader.cpp` 13-44.

**QA already there**: `tools/qa/Check-ShaderLinkage.py` enumerates every pipeline the R4/R5 sources
can build (C++ `r_Pass`, `.s` scripts, `uber_deffer`) in every preset permutation x bare/+game x
skinning, compiles with d3dcompiler_47 in process; `tools/qa/Test-Shaders.ps1`; layer tests
`tools/qa/d3d12layer` (`harness.cpp` 81-120 compiles with FXC).

**What the probes showed** (all reproducible from `<scratchpad>/dxc`):
- A graphics PSO with FXC and DXC stages is refused: `E_INVALIDARG`, D3D12 #1046 "Graphics pipeline
  state mixes shader model 6.0 or above with 5.1 or below" (both directions; `probe_mix.cpp`).
- `D3DReflect` on DXIL: `E_INVALIDARG`. `D3DReflect(DXBC, IID_ID3D12ShaderReflection)` works.
  `D3DGetInputSignatureBlob` on DXIL works (returns a container with `ISG1`).
  `IDxcUtils::CreateReflection` on a `-Qstrip_reflect` object gives bindings (type, slot, count,
  dimension) and signatures without names; on `DXC_OUT_REFLECTION` the full data (names, cbuffer
  variables). FXC and DXC reflect `$Globals` including unused globals, same offsets.
- Adapter: highest SM 6.8, wave ops 32..32 lanes, native 16-bit ops, `MinPrecisionSupport` = 16-bit,
  VRS tier 2 (`probe_caps.cpp`).
- `dx11ConstantBuffer::Similar` memcmps `D3D_SHADER_TYPE_DESC` including its `Name` **pointer**;
  identical types in two reflections have different pointers under FXC and DXC alike
  (`probe_typename.cpp`), so equal buffers are shared only when a freed reflection's memory happens to
  be reused. Pre-existing; fixed in step 3 of section 12.

## 3. Design

### 3.1 Components

- **Layer `d12_dxc.cpp` (new, xrD3D12)** - the only code that touches DXC:
  - `LoadShaderCompiler(const wchar_t* dir, char* reason, size_t)`: `LoadLibraryExW` of
    `<dir>\dxil.dll` then `<dir>\dxcompiler.dll` (absolute paths, `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
    LOAD_LIBRARY_SEARCH_SYSTEM32`; dxcompiler binds to the dxil.dll already loaded), `DxcCreateInstance`
    by `GetProcAddress` (no import lib), `IDxcVersionInfo` >= 1.8 required. `dir` = folder of the exe
    (`GetModuleFileNameW(nullptr)`, as `da_ffx_loader.cpp`). Result and reason are sticky.
  - `CompileShader(const ShaderCompile&, ShaderOutput&)` - `infra.md`'s struct plus `hlsl` (2018 or
    2021), `args` (extra wide arguments) and `preprocessed` (source needs no include handler);
    output = object (stripped), reflection blob, messages, `signed` flag. Thread-safe: one
    `IDxcCompiler3`/`IDxcUtils` per thread (`thread_local`). The DXC call sits in a
    `__try/__except` helper without C++ objects: an access violation inside DXC becomes a failed
    compile. Signing: `dxil.dll` must be loaded (unsigned DXIL is refused by the runtime); the
    loader's self-check compiles a one-line VS/PS pair and creates a PSO from it on the native device
    (`xrd3d12::NativeDevice`) - if that fails, DXC is off for the session (about 40 ms, once).
  - `Reflect(const void*, size_t, REFIID, void**)` -> `ID3D12ShaderReflection` for both formats (DXBC:
    `D3DReflect` with the D3D12 IID; DXIL: `CreateReflection`); `ID3D12LibraryReflection` for `lib_*`.
    This replaces the `ID3D11ShaderReflection` adapter of `infra.md` 3.6: no adapter object is needed.
  - `Disassemble(...)`, `IsDxil(bytecode, size)` (has a `DXIL` part), `DxcVersion()`.
- **Layer `d12_api.h`** exports the above in namespace `xrd3d12`; `ShaderCompilerCaps` =
  `NativeCaps.dxcShaderModel` + `waveOps`, `waveLanesMin/Max`, `native16Bit`, and new
  `minPrecision16` (`OPTIONS.MinPrecisionSupport & 16_BIT`).
- **Renderer `xrRenderPC_R4/da_dxc.h/.cpp` (new, bodies under `XR_D3D12`)** - the engine glue that
  `dxr.md` calls `da_dxc`: loading at device creation, the SM choice, the engine-shader compile
  (argument list, macros, preprocessing through the engine's `includer`), the DXIL cache files, the
  per-session statistics, `r__dxc`. `r4_shaders.cpp` calls it; RT/mesh/native passes call its
  `da_dxc_compile_native(...)` (DXC's own preprocessor with an FS include handler, HLSL 2021 - new
  shaders are written for it).

### 3.2 Setup and shader model

At the end of the `XR_D3D12` branch of `CHW::CreateDevice` (`dx11HW.cpp` after 226):
`da_dxc_init(pDevice)`. Engine shaders use DXC when all hold: DLLs load, the device reports SM >= 6.0,
`r__dxc 1` (saved, default 1), no `-dx12_no_sm6` (`infra.md`'s switch; `da_dxc` reads it itself, so it
works before `infra.md` lands). Target
`sm = min(device highest, DXC highest, 6.6)`: 6.6 because 6.7+ brings nothing the engine shaders use
and each target is a cache directory and an audit run. Log once:
`* [dxc] dxcompiler 1.9.0.5402, engine shaders SM 6.6 (device 6.8), waves 32-32, min16 yes` or
`* [dxc] engine shaders stay on FXC: <reason>`.

### 3.3 Compiling an engine shader

`shader_compile` keeps building `options`/`sh_name` exactly as today. Under `XR_D3D12`, when DXC is
active and the flags do not carry `RSC_FXC` (3.6):
1. **Preprocess with FXC's preprocessor**: `D3DPreprocess(source, name, options, &includer)` - the
   same `includer` (151-185) and the same macro set. DXC then compiles one self-contained buffer.
   Measured on the whole tree: the DXIL is byte-identical to DXC preprocessing the raw file with an
   include handler in 2530 of 2531 permutations (the odd one is a dead archive file DXC's
   preprocessor refuses); cost 2-5 ms per shader. Reason: DXC resolves a nested `#include "x.h"` in
   `shared/y.h` as `shared/x.h` first, FXC (the engine's includer) as `r3/x.h`, and a handler cannot
   tell the two requests apart - `shared/wmark.h` including `common.h` with both `r3/common.h` and
   `r3/shared/common.h` present is such a case in the shipped tree.
2. **Source bytes**: text that is not valid UTF-8 (25 files carry cp1251 comments) gets every byte
   >= 0x80 replaced by `?` before DXC sees it - independent of the machine's code page; identifiers
   are ASCII.
3. **Target**: first two characters of `pTarget` + `_6_<m>` (`vs_5_0` -> `vs_6_6`; legacy
   `main_vs_1_1`-style entries keep their entry point).
4. **Arguments**: `<name>.<ext> -E <entry> -T <target> -HV 2018 -Zpr -O3 -flegacy-resource-reservation
   -Qstrip_debug -Qstrip_reflect -Wno-conversion -Wno-for-redefinition -Wno-effects-syntax`, plus
   `-WX` with `-shader_warnings_as_errors`; debug builds `-Od -Zi -Qembed_debug` instead of `-O3`;
   `-dxc_debug` (command line) adds `-Zi -Qembed_debug` for PIX/Nsight source-level debugging.
   - `-HV 2018`: under 2021, 261 of 2477 compiled permutations are refused (209 vector conditions of
     `?:`, 36 `for`-scope redefinitions of `dir` in combine_1/ssao_calc, 12 loop variables used after
     the loop in the fluid shaders, 4 vector `&&`). New shaders (RT, native) use 2021.
   - `-Zpr` = `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`: reflection gives `MATRIX_ROWS`, same offsets.
   - `-flegacy-resource-reservation`: shaders bind unused resources explicitly
     (`Texture2D s_smap : register(ps,t0)`); FXC reserves those registers, DXC by default does not and
     shifts the automatic ones (s_position t1->t0, s_material t3->t2 in accum_base,
     accum_spot_unshadowed*; samplers of combine_2_NAA*; SunShaftsDisplay). With the flag every
     binding equals FXC's.
   - Warnings silenced: 26 101 implicit truncations, 488 `for` redefinitions, 368 legacy
     `register(ps,..)`/effect syntax - all legacy idioms FXC accepts silently.
   - `D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY` has no DXC equivalent; no shipped shader needs it
     (the audit's X3523 list is empty), a mod shader that does falls back to FXC.
5. **Macros**: the `options` list unchanged, plus `GRIZZLY_DXC=1`, `GRIZZLY_SM=<66>` (as `infra.md`),
   `GRIZZLY_WAVE_LANES=<waveLanesMin>` only when wave ops are supported, `GRIZZLY_MIN16=1` only when the device
   honours 16-bit min precision. Shaders guard SM 6 code with these; FXC never sees them.
6. **Output**: stripped object (to the layer) + `DXC_OUT_REFLECTION` (to the engine). A compile
   failure or a failed reflection: one log line with the first error, the define dump as today, then
   the FXC path below runs for this shader (3.6).

### 3.4 Reflection

- `CommonTypes.h` under `XR_D3D12`: `ID3DShaderReflection*` = the `ID3D12ShaderReflection*`
  interfaces, `D3D_SHADER_DESC`/`BUFFER`/`VARIABLE`/`TYPE`/`INPUT_BIND` = the D3D12 structs (same
  fields; the bind desc adds `Space`/`uID`), `IID_ID3DShaderReflection` = `IID_ID3D12ShaderReflection`,
  plus a `D3D_SIGNATURE_PARAMETER_DESC` alias for `r4_shaders.cpp` 93. `parse`, `parseConstants`,
  `parseResources`, `dx11ConstantBuffer` compile unchanged for both formats. R4 keeps the D3D11
  types.
- `create_shader` gets the reflection object from its caller: DXBC -> `xrd3d12::Reflect(bytecode)`,
  DXIL -> `xrd3d12::Reflect(reflection blob)`; the VS `position_only` loop uses that same object
  instead of a second `D3DReflect`. `D3DGetInputSignatureBlob` stays (works on DXIL).
- Layer: `Shader::Reflect` uses `xrd3d12::Reflect` (stripped DXIL is enough: slots, types,
  dimensions, input semantics); `Shader::dxil` is set; `ProgramType` also reads the `DXIL` part (its
  first u32 >> 16 uses the same 0..5 stage numbering). `BindSlot` refuses a binding in a register
  space other than 0 for engine shaders (log once).
- Audit result (6.1): for all 2531 unique permutations the DXC reflection matches FXC's
  in constant buffers (names, sizes, order, every variable's offset/size/class/type/rows/columns) and
  bindings (name, type, slot, count, dimension); nothing `parse` would `fatal` on, no constant
  buffer whose reflection index differs from its register. The one difference: `mark_msaa_edges.ps`
  with `MSAA_SAMPLES=1`, where DXC folds `P == P0` (x/1) and drops the loads - the pass exists only
  under MSAA.
- 16-bit types stay out of constant buffers: `parseConstants` `fatal`s on `min16float`/`float16`
  members (and so does FXC's reflection today).

### 3.5 Cache

- Directory `appdata/shaders_cache_grizzly/r5_dxil/<tag>/<name>.<ext>/<sh_name>` with
  `<tag> = sm66-w32-m16-<crc32 of DXC version string + the fixed argument list, 8 hex>`; the FXC cache
  (`r5/`) is untouched, so switching `r__dxc` or updating DXC never serves stale bytecode and never
  deletes the other format.
- File: `u32 'DXL1'`, `u32 fileCrc` (as today), `u32 objectSize`, `u32 reflectionSize`,
  `u32 crc32(object + reflection)`, object, reflection. Written with `FS.w_open` like today.
- The PSO library (`r5/pipelines.bin`) needs nothing: PSO keys hash the bytecode, DXIL and DXBC keys
  never collide.

### 3.6 DXIL and DXBC in one pipeline

- Rule: a graphics pipeline is all DXIL or all DXBC (D3D12 #1046, probe). Compute pipelines stand
  alone.
- Global mode decides the format of every engine shader. The exception is a shader DXC cannot build:
  it becomes DXBC (the existing FXC code path, `r5/` cache) and its **pass** is unified to DXBC:
  - `SVS..SCS` get `bool dxil` and `shared_str source` (the file name `CreateShader` opened, before
    the skin/sample suffixes).
  - `CResourceManager::_CreateVS/_PS/_GS/_HS/_DS(name, flags)` accept `RSC_FXC` (an engine-private
    flag bit, 1u << 31, never passed to D3DCompile): the map key gets `|fxc`, `shader_compile` skips
    DXC.
  - `CBlender_Compile::UnifyShaderFormats()` right after the stages of a pass exist and before any
    `ctable.merge` (`r_Pass` after the `_Create*` calls at 150-155; `r_TessPass` after creating
    HS/DS, rebuilding the table from scratch - no `r_Constant` has run yet; `PassSET_Shaders`
    reordered to create first, merge after): if the pass mixes formats, every DXIL stage is replaced by `_CreateX(source, RSC_FXC)`.
    Log: `! [dxc] pass deffer_base_bump/deffer_base_bump: DXBC twins for vs (the ps fell back to FXC)`.
  - This is the generic form of `infra.md`'s `//grizzly:dxil` pairing plumbing; the marker is not
    needed, since all R5 engine shaders are DXIL.
- Layer guard: `PipelineCache::Graphics` checks the stages' `dxil` flags before
  `CreateGraphicsPipelineState`; a mix is logged once with the shader names
  (`graphics pipeline %016llx mixes DXIL (vs 'a') and DXBC (ps 'b') stages`) and cached as a refused
  PSO (draw skipped, as for any refused pipeline). `-dxstats` counts it.
- Shaders created outside `CResourceManager` (ImGui's own pair) are one format per pipeline already.
  A feature adding engine-path shaders (`gpudriven.md` 410) creates them through `CResourceManager`
  so their format follows the mode; an FXC-only assumption holds only with `r__dxc 0`.

### 3.7 Other consumers

`CompileShader` takes any target: `lib_6_3..6_6` (no `-E`; `-exports` from the request),
`cs_6_5` with `RayQuery`, `as_6_5`/`ms_6_5`; reflection of libraries through
`ID3D12LibraryReflection`. Their root signatures and PSOs are theirs (`dxr.md`, `infra.md`); native
pass files use `da_dxc_compile_native` (HLSL 2021, DXC preprocessor, FS include handler that resolves
relative to the includer first, then the r3 root - new files are written for it), cached under
`r5_dxil/<tag>/native/`. Mesh and amplification shaders are engine-path shaders (see the amendments:
engine compile path with `-HV 2021`).

### 3.8 Statistics and diagnostics

`da_dxc` counts compiled / cache hits / FXC fallbacks / twins / milliseconds per session and logs
e.g. `* [dxc] shaders: 412 compiled (23.1 s), 2051 cached, 0 on FXC, 0 twins` at the end of
`CRender::level_Load` (`xrRender_R2/r2_loader.cpp` 27) and at shutdown. `-disasm` writes
`disasm/<name>.<ext>.dxil.txt` through `xrd3d12::Disassemble`.

## 4. Integration points

| File | Change |
|---|---|
| `src/Layers/xrRenderDX12/d12_dxc.cpp` (new, Grizzly/MIT header) | loader, caps, `CompileShader`, `Reflect`, `Disassemble`, `IsDxil` |
| `src/Layers/xrRenderDX12/d12_api.h` | declarations (3.1); `NativeCaps.minPrecision16` |
| `src/Layers/xrRenderDX12/d12_objects.cpp` 1982-2035 | `Shader::Reflect` through `xrd3d12::Reflect` (D3D12 descs), `Shader::dxil` |
| `src/Layers/xrRenderDX12/d12_device.cpp` 216-226 | `ProgramType` for the `DXIL` part |
| `src/Layers/xrRenderDX12/d12_layer.h` 955-971 | `Shader::dxil` |
| `src/Layers/xrRenderDX12/d12_infra.cpp` 2507-2593 | format-mix guard, stats counter |
| `src/Layers/xrRenderDX12/CMakeLists.txt` | `d12_dxc.cpp` (Windows SDK `dxcapi.h`/`d3d12shader.h`, no import lib) |
| `src/Layers/xrRenderPC_R4/da_dxc.h/.cpp` (new) | engine glue (3.1-3.3, 3.5, 3.8) |
| `src/Layers/xrRenderPC_R4/sources.cmake` | the new files |
| `src/Layers/xrRenderPC_R4/r4_shaders.cpp` 15-149, 250-712 | DXC branch under `XR_D3D12`, reflection object passed in, `RSC_FXC`, disasm |
| `src/Layers/xrRenderDX11/CommonTypes.h` 243-254 | D3D12 reflection aliases under `XR_D3D12` |
| `src/Layers/xrRenderDX11/dx11ConstantBuffer.cpp` 44-50 | type `Name` pointer cleared before CRC/memcmp (both modules) |
| `src/Layers/xrRender/SH_Atomic.h` 33-120 | `dxil`, `source` on `SVS..SCS` |
| `src/Layers/xrRender/ShaderResourceTraits.h` 616-720 | store `source`; pass `RSC_FXC` through |
| `src/Layers/xrRenderDX11/dx11ResourceManager_Resources.cpp` 43-142, `xrRender/ResourceManager.h` | `flags` on `_CreatePS/_GS/_HS/_DS`, `|fxc` key |
| `src/Layers/xrRenderDX11/Blender_Recorder_R3.cpp` 133-196, `xrRender/Blender_Recorder.cpp` 205-232, `xrRender/Blender_Recorder_R2.cpp` (two-shader `r_Pass`), `xrRender/blender_recorder.h` | `UnifyShaderFormats` |
| `src/Layers/xrRenderDX11/dx11HW.cpp` after 226 | `da_dxc_init` |
| `src/Layers/xrRender/xrRender_console.cpp` 833-852, near 1959 | `r__dxc` (saved, both modules, R4 ignores it); `dxc=` in `r__caps` |
| `src/Layers/xrRender_R2/r2_loader.cpp` 27 | statistics line |
| `src/Layers/xrRender_R2/r2.cpp` 768 | `-sjitter` becomes a no-op (the accum shaders lost `tcJ`; both compilers fail, see 6.2) |
| `cmake/XRay.Dependencies.cmake` 59-153 | `XRAY_DXC_RUNTIME_FILES`, staged by `xray_stage_windows_runtime` |
| `packaging/dead-air-x64/installer/runtime-files.txt` | `dxcompiler.dll`, `dxil.dll` |
| `packaging/dead-air-x64/compatibility/gamedata/shaders/r3/` | fixes of 6.2, payoff edits of 6.4 |
| `tools/qa/Check-DxcShaders.py`, `tools/qa/dxc/pso_probe.cpp` + `Run-DxcAudit.ps1` (new) | the audit (6.1) |
| `tools/qa/d3d12layer/harness.cpp`, `main.cpp`, `Run-LayerTests.ps1`, `Build-LayerTests.ps1` | `-Dxil` mode, two scenarios, DLLs copied next to the exe |
| `docs/dead-air/DX12_RENDERER.md`, `DEPENDENCIES.md`, `TEST_MATRIX.md` | compiler section, notice, rows |

## 5. Data and resources

- `Externals/dxc/`: DXC v1.9.2607 (GitHub release "DX Compiler Release for July 2026", zip SHA-256
  `a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241`): `bin/x64/dxcompiler.dll`
  (28.1 MB, SHA-256 `9a510051...0e9b`), `bin/x64/dxil.dll` (3.8 MB, `feb57253...c2d8`),
  `LICENSE-MS.txt` (dxil.dll and the distributables, redistribution allowed with an application),
  `LICENSE-LLVM.txt`, `LICENCE-MIT.txt`, `README.md` (version, source URL, hashes). Headers come from
  the Windows SDK 10.0.26100 (`dxcapi.h`, `d3d12shader.h`), whose COM interfaces match. Same vendoring
  as `infra.md` 3.6 - one copy.
- Runtime files: +31.9 MB uncompressed in the game folder (the release zip compresses its binaries to
  about 38 %, so roughly 12 MB in a patch archive).
- Memory: the DLL images (high addresses, ASLR) plus DXC's transient heap during compiles; the
  per-thread compiler instances live for the session. Shader bytecode in memory: stripped DXIL is
  smaller than DXBC (13.7 MB vs 16.5 MB for all 2531 permutations). Reflection blobs are read,
  parsed and freed.
- Disk: the `r5_dxil/<tag>/` cache, about the size of the DXBC cache.

## 6. Shaders

### 6.1 Audit

`tools/qa/Check-DxcShaders.py` imports `tools/qa/Check-ShaderLinkage.py`
and takes the same inputs: the archive layers under `_work/da_unpack` + the loose overlay, every
pipeline of the R5 sources (472), every other shader file of the tree (70, compute included), every
preset permutation x bare/+game x skinning mode with the DirectX 12 define set - 21 040 jobs,
2531 unique preprocessed shaders; `--console-extras` adds the console-only switches one at a time
(HDAO, HBAO 1/2, min-max SM, sun jitter/filter, T-shadows, motion blur, forced gloss, skin colour,
static sun, sun/shaft/SSAO qualities, SSAO blur, technicolor/vignette/grain, SM 4.1, doubles: 42 644
jobs, 3540 unique; since D2 without sun jitter and skin colour - `-sjitter` and `-skinw` are no-ops -
and static sun, which only R4's R2A mode sets). Per unique shader: FXC compile (the engine's flags and X3523 retry) and DXC
compile in process (`dxcompiler.dll` through ctypes), both reflected (`ID3D12ShaderReflection`),
compared field by field and run through the rules of `parse`/`BindSlot`; stage linkage of every
pipeline with the DXIL signatures (`Check-ShaderLinkage.link`); compile times; optionally the DXIL
blobs + a pipeline list for `pso_probe.exe`, which creates every pipeline on the real device with the
layer's root signature layout and reports refusals with the debug layer's reason.

In the tree (D1): the defaults are the engine's setup of 3.3 - `Externals/dxc`, FXC preprocessing with
the engine's includer, `-HV 2018 -Zpr -O3 -flegacy-resource-reservation -Qstrip_debug -Qstrip_reflect`
with the three legacy warnings silenced, the `GRIZZLY_*` macros of a 32-lane / min16 device (`--wave-lanes`,
`--no-min16`); `--preprocess dxc`, `--no-engine-args`, `--dxc-arg` for experiments. The DXC wrapper
itself lives in `Check-ShaderLinkage.py` (one copy of the arguments for both tools). `.ms`/`.as` files
compile at HLSL 2021 from SM 6.5 on; `r3/rt/` and `r3/native/` are left out (they compile through
`da_dxc_compile_native`). Exit code 1 on a DXC-only failure, a new reflection problem or a pipeline
outside the gated set that does not link. `tools/qa/dxc/pso_probe.cpp` (Grizzly/MIT header) and
`tools/qa/dxc/Run-DxcAudit.ps1 [-Sm 6_6] [-ConsoleExtras] [-Loose <overlay>]` build the probe with cl.exe
into `build\qa\dxc`, run the audit with `--out` and the probe; the probe has graphics, compute and mesh
root signatures shaped like the layer's (mesh: the VS parameters at MESH, the GS parameters at
AMPLIFICATION visibility, `mesh.md` 3.5) and lists refusals of pipelines built behind a legacy switch
("(gated)") apart. PASS = no DXC-only failure and refusals only in the gated set.
Measured with the probe: D3D12 links a mesh shader to the pixel shader by semantic over its vertex and
primitive outputs (reordered inputs link; a missing semantic is #666, a wider mask #662), so the
linkage rule for an `ms` producer has no register check.

The two everyday tools (D2, PLAN C25): `tools/qa/Test-Shaders.ps1` compiles the overlay's top-level
`.vs/.ps/.cs` (and `.gs/.hs/.ds`), `-Dxc [-Sm 6_6]` the way R5 does (fxc `/P` with the engine's options,
`SMAP_ARRAY` and the `GRIZZLY_*` macros, then `dxc.exe` of the vendored release with the engine's
arguments; `.ms/.as` at HLSL 2021 from SM 6.5), skips `rt\` and `native\`, and uses one temp tree per
process (lanes run it in parallel). `tools/qa/Check-ShaderLinkage.py --compiler dxc [--sm 6_6]` links
the R5 pipelines with DXIL signatures: a stage DXC refuses falls back to FXC and the pass is linked (and
listed) as DXBC twins (3.6); mesh names `name#ms` / `name#as` exist only in this mode (a mesh pipeline
with a non-DXIL stage is listed as unavailable); options in a name (`file(OPT_A,OPT_B,)`) become
defines. Both scripts carry `[dx12:<lane>]` blocks for the lanes' own permutations, known-unbuildable
entries and code-built pipelines (`EXTRA_PIPELINES`).

### 6.2 Results and fixes

| SM | DXC-only failures before fixes | after fixes |
|---|---|---|
| 6.0 | 20: overlapping `SV_Position` (19), `#unfdef` in a dead file (1) | 0 |
| 6.6 | 56: + immediate texel offsets in the Ultra 7x7 PCF (36) | 0 |
| 6.6 + console extras | + `uniform texture2D` in the HBAO path (8) | 0 |

"After" is with the fixes below and FXC preprocessing (3.3); the dead file fails only under DXC's own
preprocessor. The chosen setup (FXC preprocessing, `-flegacy-resource-reservation`) was re-run at
SM 6.0, 6.5 and 6.6 on the fixed tree.

Measured with the tools in the tree (28.09.2026; outputs in `_work/dx12-features-20260928/dxc/d1`,
`.../d2`). D1 ran on the Gate-0 overlay snapshot (the tree dxc.md was written against; the working
tree already carried other lanes' shader edits), D2 on that snapshot plus the four fixed files:

| Run | D1 (before), DXC-only failures | D2 (after) |
|---|---|---|
| `--sm 6_0` | 19 duplicate `SV_Position` (2531 unique); 20 with `--preprocess dxc` (+ the dead file) | 0 (2646 unique: today's sources build more pipelines) |
| `--sm 6_5` | 19 | 0 |
| `Run-DxcAudit -Sm 6_6` | 55 (+ 36 non-immediate offsets); probe 1694 pipelines, refused only gated | 0; probe 1725 pipelines, refused only the 31 gated old-cascade permutations |
| `Run-DxcAudit -Sm 6_6 -ConsoleExtras` | 73 (+ 8 `uniform texture2D`; offsets 42 and `SV_Position` 23 in the extra permutations); probe 2022, refused gated + the two `-sjitter` pairs | 0 (2748 unique; `-sjitter`, `-skinw` and static sun no longer in the extras); probe 1742, refused only the 34 gated permutations |

Everywhere: no reflection problem the engine or the layer would have, reflection differences only in
`mark_msaa_edges.ps`, no non-linking DXIL pipeline outside the gated old cascades.

Fixes (in `<scratchpad>/dxc/shader-fixes.patch`, overlay files; all four landed in D2):
1. `lod.ps`, `water.ps` (`water_soft.ps`/`water_green.ps` include it): the pixel shader takes
   `float4 pos2d : SV_Position` next to an input struct that already has `hpos : SV_Position`; DXC
   refuses the duplicate. Drop the parameter, use `I.hpos` (the same pixel position). Also valid FXC.
   In the tree: `lod.ps` (the `MSAA_ALPHATEST_DX10_1_ATOC` signature and its `alpha_to_coverage`
   call), `water.ps` (the `GBUFFER_OPTIMIZATION` signature and `gbuffer_load_data`); the `#ifdef`
   pairs that only differed by the parameter are gone.
2. `shadow.h` `dx10_0_hw_hq_7x7` (SUN_QUALITY 3): `SampleCmpLevelZero(..., int2(col,row))` inside
   loops; at SM 6.6+ DXC merges branches and the offset stops being an immediate
   (`[unroll]` does not help, tried). Fold the offset into the coordinate:
   `SMAP_TC(tc.xy + float2(col,row) * (1.0/SMAP_size) + ...)` - the same texel (SMAP_size is a power of
   two), compiles at 6.0/6.6/6.7/6.8 and on FXC. DX11 output changes only by float rounding.
   In the tree: one `const float2 tco = tc.xy + float2( col, row ) * ( 1.0/SMAP_size )` per loop
   step, `SMAP_TC(tco + ...)` in the nine taps (the same expression order as the audited patch).
3. `ssao_hbao.ps` (archive only, copied into the overlay): `uniform texture2D jitter4;` (a DX9 effect
   type FXC maps to `Texture2D`) -> `Texture2D jitter4;`. HBAO is selectable through `r2_ssao_mode`
   (`ui_mm_opt.xml` 722/744). In the tree: the `configs` layer's file, byte for byte, with that line
   changed. It has no `main()` (an include of `combine_1`/`ssao_calc` under `USE_HBAO`), so
   `Test-Shaders.ps1` lists it as known-unbuildable on its own; the audit checks it through its
   includers.
4. `dx11ConstantBuffer` (both modules): the constructor keeps only the layout fields of each member's
   `D3D_SHADER_TYPE_DESC` (class, type, rows, columns, elements, members; padding zeroed, no type-name
   pointer) and stores the variable's start offset in `Offset` (a type's own offset is 0 outside
   structs, which the engine's buffers never hold); `Similar()` also compares the buffer size. Equal
   buffers are now shared as upstream intended (2, `probe_typename.cpp`), and explicit packing can
   never make two different layouts look equal, which the old comparison did not check either.
- Not DXC regressions, found on the way: `-sjitter` (`r2.cpp` 768) makes `accum_sun_near/far` fail on
  both compilers (`tcJ` is gone from `v2p_volume`) and breaks VS/PS linkage of `accum_volume.vs` with
  `accum_indirect`/`accum_omni_unshadowed` on DX12 - made a no-op. `accum_volumetric_sun_normal .ps`
  (a space in the name, `#unfdef`) is never loaded. The command-line debug switch `-skinw`
  (`SKIN_COLOR`) broke every model vertex shader on both compilers (`skin.h` writes `rgb_tint`,
  `v_model` has none) - made a no-op like `-sjitter` (`core`, `r2.cpp`, 28.09.2026);
  `USE_R2_STATIC_SUN` breaks the bump/steep shaders on both compilers but only R4's R2A mode sets it.
  The only pipelines that do not link or create are
  the known old-cascade ones (`r2_shadow_cascede_old`, gated in `Check-ShaderLinkage.py`) - the same
  set as with FXC.
- `pso_probe` on the fixed tree: SM 6.6 1690 pipelines (preset permutations) and 2055 (with console
  extras, 35 compute) created; refused only the gated old cascades (+2 with `-sjitter`).

### 6.3 Compile time (payoff 1)

Single-threaded, same machine: `accum_sun_near.ps` (Extreme) FXC 2425 ms / DXC 151 ms,
`da_clouds_march.ps` 2079 / 150, `combine_1.ps` 42 / 39, `deffer_base_bump.ps` 14 / 20,
`deffer_model_bump.vs` 9 / 22 (DXC has ~15 ms of fixed cost: validation and signing). Whole audit set:
FXC 320-415 s vs DXC 165-215 s of compile time summed over threads (1.9x). The first level load after
an install, an update or a preset change is dominated by the sun and cloud shaders, which drop by
~2 s each.

### 6.4 Wave, quad and 16-bit edits (payoff 2)

Frame at 2560x1440 Extreme, DX12, this GPU (`_work/dx12-perf-20260927/qa/q4-r5c`): 16.2 ms; scene 3.0,
sun 4.5 (cascades + accumulation), clouds march 1.85, lights 0.45, combine 5.35 (AO 1.57, forward 1.31,
AA 0.71, post 1.09). Candidates (`<scratchpad>/dxc/payoff-candidates.patch`; each compiles on FXC with
the guards off and on DXC 6.6 with them on, verified):
1. **`da_taa.ps` 3x3 neighbourhood through quad reads** (`#ifdef GRIZZLY_WAVE_LANES`): each lane samples
   the three texels on its outer side (`o = (pixel & 1) * 2 - 1`), the other five come from
   `QuadReadAcrossX/Y/Diagonal` - 3 samples instead of 8; the pass-through tests (history reset, PDA
   box, `P.z < 1.10`) are first decided for the whole quad so the quad reads run with all four lanes.
   DXIL sample instructions 17 -> 12. Values are the same texels, summed in another order.
2. **GTAO reference normal** (in `gtao.h` once `async.md` moved the body there; compute variant
   quad-swizzled, PLAN C15): the in-quad horizontal and vertical neighbours' depths come
   from `QuadReadAcrossX/Y(P.z)` read before the sky early-out, one sample per axis instead of two:
   7 -> 5 sample instructions, bit-exact.
3. **`shadow.h` PCSS (SUN_QUALITY 4) in `min16float`** (`da_h*` types in `common_defines.h`,
   `min16float` only under `GRIZZLY_DXC && GRIZZLY_MIN16`, `float` elsewhere): the 0/1 comparison masks
   `v1[]/v0[]`, the kernel weights `Fw()` and the fraction terms; depths, blocker average and the
   final sum stay 32-bit. The Extreme `accum_sun_near` DXIL goes from 0 to 917 half-precision
   lines.
4. **Cloud march lighting in `min16float`** (in `da_clouds_march.h` after `async.md`'s move):
   `acc`, `lit`, `amb`, colour terms; positions,
   density, transmittance and the haze blend stay 32-bit.
- Rejected: `-enable-16bit-types` (makes the 595 `1.0h` literals of 42 files real fp16 and the
  shipped `half` types fp16 unless remapped by macro; compiles, but changes the look everywhere);
  GTAO filter in 16-bit (z^2 sums overflow fp16); quad sharing in the shadow filters (neighbour pixels
  map to unrelated shadow texels).
- Ampere runs FP16 at the FP32 rate: 3 and 4 can only win through register pressure; on RDNA and
  Turing they double the math rate. Each edit is kept only if its pass gains >= 0.05 ms (or >= 3 %)
  at equal clocks and the capture diff is invisible (section 9); otherwise it is reverted.
- Optional A/B after that: `-all-resources-bound` (the layer fills every table entry, unbound slots
  with null descriptors).

**D5 result (28.09.2026, numbers in 9 "D5 result").** The four candidates went in together, each behind
`GRIZZLY_WAVE_LANES` or `GRIZZLY_MIN16`, and were measured in one batch against `-dxc_plain` (section 7), which
compiles the same tree without those two macros. Offline first (`_work/dx12-features-20260928/dxc/d5/`):
`Test-Shaders.ps1` FXC / `-Dxc` / `-Dxc -Sm 6_0` on every touched pass and `Check-DxcShaders.py --sm 6_6
--filter accum_sun` (SUN_QUALITY 4 included): 0 failures; `identity.py` over 55600 permutations (every
pipeline and shader file, both APIs, every preset permutation) against the tree with the edits reversed:
FXC bytecode identical in all of them (DirectX 11 and the FXC fallback unchanged) and the DXIL without the
two macros identical too (so `-dxc_plain` is exactly the pre-D5 DXIL); `dxil_counts.py` (Extreme): TAA 19
-> 14 texture instructions (+19 quad ops), GTAO render 10 -> 8, `accum_sun_near` 1468 lines with `half`,
the march 45.
1. **TAA quad moments - kept** in `da_taa.ps`: -3.2 to -5.0 % of the TAA pass (-0.024 to -0.038 ms at
   1440p) in both measured pairs, however the clock difference is taken out. A probe build of the pass
   (the quad-built moments compared with the loop's on every blending pixel, magenta above 1e-4;
   `make_probe.py`, verified in the rig's DXIL cache: 22 texture ops, 19 quad ops) showed 0 magenta pixels
   in `dxc-d5-probe-pedro`; test 7 against the same binary with `-dxc_plain` under TAA
   (`dxc-d5-plain-pedro`): PSNR 61.0 dB, worst 8x8 block 1.3/255, 0 blocks > 4/255 - PASS. The pass has no
   compute stage, so `r__async_verify` does not see it.
2. **GTAO quad depths - removed** (`gtao.h` is the async lane's 14:50 text again): ao -0.5 % at equal
   clocks, +0.1 / +2.2 % by the unedited passes - two of 18 texture reads per pixel do not show in a pass
   bound by its 12 horizon taps and their math.
3. **PCSS in min16 - removed**: the sun pass got slower, +0.29 / +0.44 ms raw, +6-7 % by the unedited
   passes. Ampere runs FP16 at the FP32 rate, and the conversions between the 32-bit depths/sum and the
   16-bit weights cost more than the registers saved.
4. **Cloud march lighting in min16 - removed**: march -1.0 % at equal clocks, -0.9 / +2.2 % by the
   unedited passes, i.e. nothing. With 3 and 4 gone the `da_h*` types left `common_defines.h` too.
- **`-all-resources-bound` - behind `-dxc_all_bound`, decided in the final test phase**: on seven heavy
  passes (`accum_sun_near` SUN_QUALITY 4, `combine_1`, `deffer_base_bump` ps/vs, `da_taa`, `gtao_render`,
  `da_clouds_march`) its DXIL is the default DXIL plus the AllResourcesBound flag bit, not one instruction
  different (`arb_check.py`), so any effect sits in the driver (descriptor prefetch) and needs a batch of
  its own. The flag is safe with the layer: every table it binds holds a valid descriptor in each declared
  slot (SRV tables up to the shader's `srvCount` with null SRVs of the declared dimension, the UAV table
  all `kMaxUAVs`, CBV tables the zero buffer, sampler tables the null sampler; `d12_context_state.cpp`
  `ApplyStage`, `BuildUavTable`, `BuildCbvTable`). The start-up switch `-dxc_all_bound` (R5) adds the
  argument to every engine compile, puts the engine DXIL into its own cache folder (`r5_dxil\<tag>-arb\`,
  `-plain-arb` with `-dxc_plain`) and logs `* [dxc] -dxc_all_bound: ...`; native passes are unchanged.
  Final-phase rule: one `Invoke-Dx12Perf` batch (`qa_pedro`, 2560x1440 Extreme TAA, `-Pairs 3`,
  `r__gpu_log 5`, A = `-r5 -always_active`, B = `-r5 -always_active -dxc_all_bound`); kept only if a pass
  gains >= 0.05 ms or >= 3 % in both measured pairs and no pass loses as much - then the argument moves
  into `k_engine_args` (a new cache tag) and the switch goes; otherwise the switch and its three lines
  are removed from `da_dxc.cpp`. Frames need no parity run (identical instructions); the B runs' logs must
  show the setup line and no `! [dxc]`.

## 7. UI / presets

- No preset touches it (it does not change the look) and there is no menu row.
- `r__dxc` (console, saved, default 1, applies at the next start): engine shaders through DXC.
  `-dx12_no_sm6` (`infra.md`) turns every SM 6 path off for the run; `-dxc_debug` embeds debug info.
  `-dxc_plain` (start-up switch, D5) keeps DXC but compiles the engine shaders without
  `GRIZZLY_WAVE_LANES` / `GRIZZLY_MIN16`, i.e. without the wave, quad and min16 paths of 6.4, into their own
  cache folder (`r5_dxil\<tag>-plain\`); native passes keep both macros. It is the A/B of those paths
  inside one binary and the way around a driver that mishandles them. `-dxc_all_bound` (start-up
  switch, D5, QA only until the final A/B of 6.4 decides it) compiles the engine shaders with
  `-all-resources-bound` into `r5_dxil\<tag>-arb\`.
- `r__caps` gains `dxc=1` (engine shaders are DXIL this session); `sm`, `wave`, `fp16` come from
  `infra.md`. The RTX category rows (`dxr.md`) read `sm`/`rt`: without DXC they are shown inactive
  with their hint, like frame generation on DX11.

## 8. Fallback

| Situation | Result |
|---|---|
| `dxcompiler.dll`/`dxil.dll` missing or not loadable, version < 1.8, device SM < 6.0, `r__dxc 0`, `-dx12_no_sm6` | every engine shader on FXC (today's R5), `dxc=0`, one log line with the reason |
| the loader's self-check PSO fails (dxil.dll broken, unsigned DXIL, driver) | DXC off for the session, as above |
| one shader: DXC error, DXC crash (SEH), reflection failure | that shader on FXC, its passes unified to DXBC twins (3.6), log lines |
| stages of different formats reach the layer anyway | PSO refused with a named log line, draw skipped - never a device error |
| a driver miscompiles DXIL (not seen here) | `r__dxc 0`; the report's log carries the `[dxc]` setup and statistics lines |
| a driver mishandles only the quad or min16 paths of 6.4 (edge artefacts, wrong shadows) | `-dxc_plain`: DXIL without them, the frame of the pre-D5 shaders (`* [dxc] -dxc_plain` line) |
| DXIL cache damaged or from another DXC | CRC/tag mismatch -> recompiled |

## 9. Test plan

Offline (no game):
1. `python tools\qa\Check-DxcShaders.py --sm 6_0`, `--sm 6_5`, `--sm 6_6`, `--sm 6_6 --console-extras`
   (defaults: `Externals/dxc`, FXC preprocessing, `-flegacy-resource-reservation`). Pass: "DXC only 0";
   no "mishandle" section; reflection differences only `mark_msaa_edges.ps`; non-linking pipelines
   only the gated old cascades (and the `-sjitter` pair if the extras still define it). Exit code 0.
2. `tools\qa\dxc\Run-DxcAudit.ps1 -Sm 6_6 [-ConsoleExtras]` (audit with `--out` + `pso_probe.exe`).
   Pass: refused = gated set ("PASS" line, exit 0).
3. `python tools\qa\Check-ShaderLinkage.py` (FXC, both APIs), `python tools\qa\Check-ShaderLinkage.py
   --compiler dxc --sm 6_6` and `--sm 6_0` (DXIL signatures, twins listed), `tools\qa\Test-Shaders.ps1`,
   `-Dxc` and `-Dxc -Sm 6_0` after the shader fixes. Pass: 0 refused, no twins, no new compile
   failures.
4. Build: `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` (locked.ps1). Pass: both link, no new warnings.
5. Layer: `tools\qa\d3d12layer\Run-LayerTests.ps1 -HighPerformance` (36/36 as today) and
   `-HighPerformance -Dxil` (the layer side builds every scenario's shaders with DXC SM 6.0, D3D11 keeps
   DXBC) plus the scenarios `dxil_bindings` (same draw with the DXBC and the DXIL build on the layer:
   identical readback) and `dxil_dxbc_mix` (mixed PSO: refused with the log line, no debug-layer
   message). Pass: all pass, debug layer silent, also with `-Gbv`.

Stand (hidden desktop, `Invoke-Stand`, `-ExtraArgs '-r5 -always_active'`, short runs):
6. `Invoke-Stand -Name dxc-render -Scenario render_check -Save qa_pedro`. Pass: log has the `[dxc]`
   setup line with SM 6.6, `[qa] caps` contains `dxc=1`, the statistics line shows 0 on FXC and 0 twins,
   no `! [dxc]`, no `graphics pipeline ... refused`, no `mixes DXIL`, an FPS line.
7. The same with `-ExtraArgs '-r5 -always_active -dx12_no_sm6'` (name `fxc-render`) and
   `-Globals 'qa_render_cmds = {"r__aa st_aa_smaa"}'` in both runs; compare the two `r__capture` images of the
   same frame index: PSNR >= 40 dB and no 8x8 block with a mean difference > 4/255. Repeat on
   `-Save foliage` (grass and trees: vertex wind, both shadow techniques).
8. `-Scenario upscale_check` and `-Scenario motion_check` with DXC: pass criteria of those scenarios
   (DLSS/FSR contexts come up, velocity capture valid) plus the log checks of 6.
9. Fallbacks: (a) rename `dxcompiler.dll` in the rig -> `dxc=0`, reason logged, render_check passes;
   (b) an overlay copy of `da_taa.ps` with a duplicate `SV_Position` parameter -> `! [dxc]` fallback
   line, `DXBC twins` line for its pass, TAA capture valid, no refused pipeline; (c) `r__dxc 0` in
   `user.ltx` -> FXC. Restore the rig after each.
10. Cold compile time: back up `appdata/shaders_cache_grizzly` of the rig, run render_check once with an
    empty `r5_dxil/` and once with an empty `r5/` (`-dx12_no_sm6`), restore the backup. Pass: time from
    start to the first FPS line with DXC <= FXC; the statistics line gives the DXC compile seconds.
11. DX11 regression: `Invoke-Stand -Name dx11-render -Scenario render_check` (no `-r5`). Pass: no new
    log errors, capture identical to a run before the change except the float rounding of fix 2 in
    Ultra shadows (PSNR >= 45 dB).
    D2 result (R4 `render_check -Tga`, SMAA + frozen wind, runs under `_work/dx12-features-20260928/qa`):
    the shared tree carries other lanes' changes and the loaded machine shoots at later game times than
    the Gate-0 references, so each D2 change was isolated in a same-period A/B.
    - Shader fixes alone at High (the folded 7x7 PCF runs there): Gate-0 binaries + D2 overlay
      (`dxc-d2-high-trees-basebin-r4`) against Gate-0 binaries + Gate-0 overlay
      (`dxc-d2-ref-high-trees-{a,b,c}-r4`): calibrated "equal" passes (99.86 % within, largest cluster
      54 px; references 99.58 %, 319 px), PSNR 57.5 dB - the rounding of fix 2 is invisible.
    - Constant-buffer sharing alone, qa_pedro Extreme: today's tree built with and without fix 4
      (`dxc-d2-cbon-pedro-r4` vs `dxc-d2-cboff-pedro-{a,b}-r4` + core's control `core-k2-ctl-pedro-r4`):
      calibrated "equal" passes (99.67 % within, 5 clusters, largest 60 px; references 99.05 %, 5,
      56 px), PSNR 55.5 dB; against the Gate-0 references 52.3 dB.
    - The same on `trees` (Extreme, weather clock pinned 9:00): `dxc-d2-cbon-trees-r4` vs
      `dxc-d2-cboff-trees-{a,b}-r4` + `core-k2-ctl-trees-r4`: calibrated "equal" passes (99.69 % within,
      5 clusters, largest 79 px; references 99.37 %, 8, 237 px), PSNR 61.9 dB; against the Gate-0
      references (+ control) calibrated "equal" passes, 54.4 dB.
    - Today's binaries + D2 overlay against the Gate-0 references (other lanes' changes included):
      foliage 54.2 dB, qa_pedro 51.1 dB (>= 45).
12. Perf (holds `ninja.lock`, 2560x1440 Extreme): `ab_run.ps1 -Runs r5a,r5b,r5c -Prefix dxc` and
    `ab_run.ps1 -Runs r5a,r5b,r5c -Prefix fxc -R5Extra '-dx12_no_sm6'`, interleaved (one run of each in
    turn), `analyze.ps1` on all six. Pass: frame time DXC <= FXC + 1 % at equal clocks; per-pass
    numbers recorded. Then each payoff edit of 6.4 alone, the same A/B against the DXC baseline, kept
    only when its pass gains >= 0.05 ms or 3 % and test 7's capture diff passes.

**D3b results (tests 4-11, 28.09.2026; runs under `_work/dx12-features-20260928/qa`, scripts and heat
maps in `.../dxc/d3`).** Every R5 run below is `render_check` on `qa_pedro` unless named otherwise.
- Test 4: `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` exit 0 (last 14:19), no warning in a touched file.
- Test 5: `Run-LayerTests.ps1 -HighPerformance` and `-HighPerformance -Dxil`: 68 passed, 0 failed each
  (floor 60), debug layer silent.
- Test 6: `dxc-d3b-render-pedro` and `-foliage`: `* [dxc] dxcompiler 1.9.0.5402, engine shaders SM 6.6
  (device 6.8), waves 32-32, min16 yes`, caps `dxc=1;sm=68`, `* [dxc] shaders: 0 compiled, 200 cached,
  0 on FXC, 0 twins`, no `refused` / `mixes DXIL` / other `!` line, FPS lines (52.5 / 33.3 under the
  night's stand load).
- Test 7 (the same binaries, `-dx12_no_sm6` = FXC, parity recipe): `qa_pedro` DXC vs FXC PSNR 56.4 dB,
  worst 8x8 block 2.02/255 - PASS. `foliage` PSNR 49.9 dB; unmasked, one 8x8 block reaches 4.13/255 in
  moving grass, while FXC against itself (`fxc-d3b-render-foliage-b`, `-c`) differs by up to 11.6/255
  per block (4 blocks > 4/255) - with FXC's own run-to-run motion masked (PLAN 0.6) the worst block is
  2.68/255, 0 blocks > 4/255 - PASS. The calibrated "equal" of DXC against the three FXC runs misses
  only on the share within 2/255 (99.35 % vs 99.58 %; clusters 5, largest 32 px vs the references'
  109 px): the two compilers are not bit-exact (contracted `mad`, reassociation), as expected; the heat
  map (`d3b-dxc-vs-fxc3-foliage-heat.png`) has no structure outside moving foliage.
  Against Gate 0 both paths of today's tree miss "equal" by the same margin (qa_pedro DXC 99.10 %,
  FXC 99.19 %; foliage 93.7 % / 94.0 %): the other lanes' overlay edits (the GTAO filter corner,
  PLAN 6.4), not the compiler.
- Test 8: `dxc-d3b-upscale` all 12 steps (SMAA, TAA, vectors, FSR Quality/Performance/FG, DLSS Quality,
  DLAA, DLSS + FSR FG; DLSS FG skipped: no DLSS-G on this GPU, as at Gate 0); `dxc-d3b-motion-vec`
  (TAA + `r__motion_vectors 1`) 20 frames of a valid vector field; against `fxc-d3b-motion-vec` frames
  5/10/19 are 39/30/38 dB apart, inside the spread of the Gate-0 pair `core-g0-motion-a`/`-b` under the
  same camera turn (42/36/31 dB; frame 0, the switch frame, differs in every run); log checks of test 6
  clean.
- Test 9: (a) `dxc-d3b-fb-nodll` (no `dxcompiler.dll` in the rig): `* [dxc] engine shaders stay on FXC:
  the DXC service is not up`, `dxc=0;sm=50`, renders. (b) `dxc-d3b-fb-taa2` (full overlay with a
  duplicate `SV_Position` in `da_taa.ps`): `! [dxc] ...da_taa.ps...: da_taa.ps:85:1: error: Parameter with
  semantic SV_Position has overlapping semantic index at 0.`, one `! [dxc] pass da_fullscreen/da_taa:
  DXBC twins for vs (ps fell back to FXC)`, statistics `1 on FXC, 4 twins, 1 refused by DXC`, no refused
  pipeline, a valid TAA frame, 78 FPS. (c) `dxc-d3b-fb-rdxc0b` (`r__dxc 0` in `user.ltx`): `* [dxc] engine
  shaders stay on FXC: r__dxc 0 (native passes: dxcompiler 1.9.0.5402, SM 6.8)`, the native self-test
  still passes, `dxc=0`. The first (c) run `dxc-d3b-fb-rdxc0` appended the line to the rig's `user.ltx`,
  which `Run-ContentProbe.ps1` rebuilds from its backup - the engine never saw it; `-PreCommands` is
  the way to put a start-up setting into a run.
- Test 10 (cold caches, `dxc-d3b-cold-pedro` / `fxc-d3b-cold-pedro`): DXC compiled 198 shaders in 15.2 s
  (sum over threads); renderer resources 16.0 s vs 20.5 s; start to the scenario's end 61 s vs 108 s
  (FXC keeps compiling after the load: 5.5 FPS over the measuring window). PASS (DXC <= FXC).
- Test 11 (DX11): the engine files of D3b compile into R4 too, so the R4 engine was isolated with the
  Gate-0 overlay: `vrs-v2-render-pedro-r4` (today's R4 incl. every D3b edit, staged 14:02) is **equal** to
  Gate 0 with the same-period control `dxc-d3b-ctl-pedro-r4` (99.80 % within, 1 cluster > 16 px;
  references 99.70 %, 1), PSNR 59.4 dB; `ds-s3-render-pedro-r4` (13:48) likewise. With the tree
  overlay (`dxc-d3b-render-pedro-r4` 54.9 dB, `-foliage-r4` 47.1 dB) only the overlay's GTAO filter
  corner remains (async's attribution, PLAN 6.4 known delta, >= 45 dB).

**D4 result (test 12, 28.09.2026).** One `Invoke-Dx12Perf` batch (PLAN 0.1a): `dxc-d4-perf-pedro`,
`qa_pedro`, 2560x1440 Extreme TAA, `-Pairs 3` (pair 0 warm-up), A = FXC (`-r5 -always_active
-dx12_no_sm6`), B = DXC, one R5 binary for both (`50CFA239...`, the post-D3b tree build), the tree
overlay. Both variants report the same frame features (`[preset] ... vrs=0`, VRS mode 0 at Extreme,
`gd=0`, no mesh grass, async on the pixel path); `-dx12_no_sm6` additionally masks `ms`, `wave`,
`fp16`, which nothing in that frame reads. The DXC runs: `0 compiled, 203 cached, 0 on FXC, 0 twins`.
Analysis `_work/dx12-features-20260928/dxc/d4/analyze_d4.py` (`analyze_d4.txt`).

| Field (ms, mean of the run's lines) | Pair 1 FXC | DXC | | Pair 2 FXC | DXC | |
|---|---|---|---|---|---|---|
| FPS (12 s window) | 51.5 | 51.5 | 0.0 % | 53.3 | 52.6 | -1.3 % |
| clock, FPS window mean (all samples) | 1336 | 1331 | -0.4 % | 1310 | 1268 | -3.2 % |
| clock, util >= 85 % samples (helper) | 1241 | 1200 | -3.3 % | 1281 | 1236 | -3.5 % |
| scene | 3.311 | 2.865 | -13.5 % | 3.425 | 3.120 | -8.9 % |
| sun | 5.225 | 4.872 | -6.8 % | 5.242 | 5.311 | +1.3 % |
| lights | 0.504 | 0.459 | -8.9 % | 0.499 | 0.496 | -0.7 % |
| clouds + march | 0.325 | 0.287 | -11.7 % | 0.334 | 0.305 | -8.7 % |
| combine (ao, forward, aa, post inside) | 6.194 | 5.500 | -11.2 % | 6.082 | 5.940 | -2.3 % |
| grass (inside scene) | 0.570 | 0.514 | -9.7 % | 0.606 | 0.534 | -12.0 % |
| timed passes (sum, incl. ripple) | 15.559 | 14.125 | -9.2 % | 15.583 | 15.329 | -1.6 % |
| `frame` span | 16.165 | 18.688 | +15.6 % | 16.233 | 15.904 | -2.0 % |
| idle inside the span | 0.607 | 4.562 | | 0.650 | 0.575 | |

- Verdict: **pass**, nothing pinned to FXC. Every timed pass of DXC is lower or equal in both pairs; the
  one raw increase (sun +1.3 % in pair 2) came at a 3.2 % lower clock (-1.9 % at equal clocks). At equal
  clocks (pair 1, window clocks 0.4 % apart) the GPU work is 9.2 % lower with DXC, in pair 2 4.8 % lower
  scaled to equal clocks. The `frame` span of `dxc-d4-perf-pedro-dxc-1` (+15.6 %) is not GPU work: the
  stand frame is CPU-bound (51.5 FPS = 19.4 ms per frame in both runs of the pair, GPU utilisation
  50-100 %), and in that run the GPU waited 4.6 ms inside the span in every logged frame (0.6 ms in the
  other three runs, including the second DXC run with the same binary and shaders), i.e. the CPU's
  second submission came late; the span then covers 96 % of the frame period. The helper's own
  verdict line says "no valid pair" (its clock rule uses only util >= 85 % samples, which in this
  power-limited, CPU-bound frame are a handful of seconds and sit lower for the variant with less GPU
  work) and its information line "frame at 1300 MHz: fxc 15.715, dxc 16.183 ms" carries that one span.
- Precision: a `[gpu]` line is a single frame (`dx11GpuTimers` prints the latest resolved frame), so
  each run's numbers are the mean of 2 frames and runs of the same variant differ by 5-10 % per pass;
  the batch shows the direction (DXIL not slower on this GPU and driver, faster in most passes), not a
  1 % figure. Reported to `core` (tool owner) with a fix suggestion; D5 got that resolution from the
  protocol itself instead (`r__gpu_log 5`, below).

**D5 result (test 12 for the payoff edits, 28.09.2026).** One `Invoke-Dx12Perf` batch
(`dxc-d5-perf-foliage`, `foliage`, 2560x1440 Extreme TAA, `-Pairs 3`, pair 0 warm-up), one R5 binary and
one overlay with every candidate of 6.4 in it: A = `-r5 -always_active -dxc_plain` (the DXIL without
`GRIZZLY_WAVE_LANES` / `GRIZZLY_MIN16`, byte-identical to the pre-D5 shaders, below), B = the payoff paths. Both
variants add `r__gpu_log 5` (a `[gpu]` line every 5 frames: 90-110 frames in each run's FPS window
instead of 2) and `r__async_compute st_async_off` (GTAO and the cloud march as the pixel passes the
edits target). `foliage` rather than D4's `qa_pedro`: the march (0.14 ms on qa_pedro after the early-out)
and the PCSS sun term are only measurable with sky and shadowed vegetation in the frame. Analysis
`_work/dx12-features-20260928/dxc/d5/analyze_d5.py` (`analyze_d5.txt`); the window clocks of the pairs
differ by +2.8 % and -5.5 %, so each edit is judged three ways: raw, scaled to equal clocks, and scaled by
the passes without an edit (scene, lights, clouds composite, forward, bloom, post: payoff -1.8 % / +1.3 %).

| Pass (edit) | plain ms (pair 1 / 2) | payoff raw | equal clocks | by the unedited passes | Verdict |
|---|---|---|---|---|---|
| `aa` (TAA quad moments) | 0.752 / 0.755 | -0.040 / -0.015 | -0.020 / -0.055 (-5.0 %) | -0.027 / -0.024 (-3.6 / -3.2 %) | kept (>= 3 % every way) |
| `ao` (GTAO quad depths) | 1.216 / 1.219 | -0.021 / +0.043 | +0.013 / -0.026 (-0.5 %) | +0.001 / +0.027 | removed |
| `sun` (PCSS min16) | 5.719 / 5.934 | +0.286 / +0.438 | +0.454 / +0.088 (+4.7 %) | +0.396 / +0.355 (+6.9 / +6.0 %) | removed (slower) |
| `march` (cloud march min16) | 1.702 / 1.714 | -0.045 / +0.061 | +0.001 / -0.037 (-1.0 %) | -0.015 / +0.038 | removed |

Standard error of a run's mean: 0.005-0.007 ms on `aa`, 0.008-0.012 on `ao`, 0.04-0.07 on `sun`,
0.014-0.021 on `march`. FPS (44.9 / 37.8 and 44.2 / 45.8) and the `frame` span follow the CPU in this
frame, not the GPU: the timed passes of pair 1 differ by +0.16 ms (the PCSS edit) while its span grew by
2.7 ms of GPU idle (the CPU's second submission came late, as in D4's `dxc-1`), and pair 2 goes the other
way; no measured run compiled a shader (`0 compiled, 206 cached`). The helper's own verdict line (FPS
-15.8 % over its one clock-valid pair) is that CPU-side span.

## 10. Perf expectation

- GPU: DXIL vs DXBC within +-2 % per frame on this GPU (same HLSL, driver back ends differ per pass;
  measured, not assumed - test 12). Measured (D4): DXIL not slower in any pass, the timed passes
  1.6-9.2 % lower at 2560x1440 Extreme; FPS equal within noise (the stand frame is CPU-bound).
- First-run compile: the sun accumulation and cloud march shaders 14-16x faster (2.1-2.4 s -> 0.15 s
  each), small shaders ~10 ms slower; a first level load with the Extreme preset several seconds
  shorter; the audit set 1.9x less compile time.
- Payoff edits on the RTX 3070 Laptop at 1440p Extreme: TAA -0.05..-0.1 ms (5 of 8 neighbourhood
  samples gone), GTAO render -0.03..-0.05 ms, PCSS and clouds in min16 0..-0.2 ms (register pressure
  only on Ampere; up to 2x math rate on RDNA/Turing). Total realistic 0.1-0.3 ms (1-2 %).
  Measured (D5, `foliage`): TAA -0.024..-0.038 ms (-3.2..-5.0 %), kept; GTAO and the march within
  noise, PCSS in min16 +0.29..+0.44 ms - all three removed. The payoff that stays is the TAA pass and
  the compile time of 6.3.
- CPU: unchanged per frame; fewer constant-buffer objects after the `Similar` fix (fewer maps/uploads,
  both renderers, counted with `-dxstats`).

## 11. Risks

- Driver DXIL back ends on AMD/Intel are untested here: `r__dxc 0` and the `[dxc]` log lines; the
  PSO probe can be run by any tester with the audit output.
- Mods with FXC-only HLSL: handled per pass (twins), costs FXC compile time only for those passes.
- DXC 1.9 is 28 MB: patch size; an update of DXC changes the cache tag (one recompilation).
- Quad intrinsics need quad-uniform control flow; the TAA/GTAO edits decide branches per quad first.
  A wrong edit shows as edge artefacts: captures around HUD, PDA and sky edges (test 7).
- `min16float` results differ between vendors (precision is a minimum): only 32-bit-safe terms were
  converted; any visible difference reverts the edit.
- Windows 10 in-box runtimes stop below SM 6.6 (`dxr.md`: 6.5 from 2004): covered by the 6.5 audit
  run; SM 6.0-only devices by the 6.0 run.
- `CommonTypes.h` aliasing touches every reflection use of R5: the audit's reflection comparison and
  layer test 5 cover it; R4 is untouched.
- Constant-buffer sharing fix: buffers that were accidentally separate become shared -
  intended upstream behaviour; stand captures + `-dxstats` confirm.

## 12. Implementation order

1. Vendor DXC 1.9.2607 into `Externals/dxc` (DLLs, licences, README with hashes);
   `XRAY_DXC_RUNTIME_FILES` + staging; `runtime-files.txt`; `DEPENDENCIES.md` notice.
2. Tools: `tools/qa/Check-DxcShaders.py` (default DXC dir, FXC preprocessing, known-unbuildable and
   gated lists shared with `Check-ShaderLinkage.py`), `tools/qa/dxc/pso_probe.cpp` +
   `Run-DxcAudit.ps1`. Run tests 1-2 on the current tree: they must report exactly 6.2's failures.
   Done (D1, 6.1/6.2).
3. Shader fixes of 6.2 in the overlay; `-sjitter` no-op; `dx11ConstantBuffer` type-name fix. Tests
   1-3, then R4 build + test 11. Done (D2; the `-sjitter` no-op is `core`'s, K2).
4. Layer: `d12_dxc.cpp`, `d12_api.h`, `Shader::Reflect`/`ProgramType`/`dxil`, mix guard, stats;
   CMake. Test 5 (both modes, new scenarios).
5. Engine: `CommonTypes.h` aliases, `da_dxc`, `r4_shaders.cpp` DXC branch + reflection hand-over +
   cache + disasm, `dxil`/`source`/`RSC_FXC`, `UnifyShaderFormats`, `dx11HW.cpp` init, `r__dxc`,
   `r__caps`, statistics. Test 4, then 6-11. Done (D3a, D3b; section 13).
6. Perf baseline, test 12 (DXC vs FXC). Done (D4, section 9: no pass regresses, no FXC pin list).
7. Payoff edits of 6.4 one at a time (`common_defines.h` types first, then TAA, GTAO, PCSS, clouds),
   each with tests 1, 7 and 12; keep or revert per the thresholds. Done (D5, 6.4: the four in one batch
   through `-dxc_plain`, each on its own `[gpu]` field; TAA kept, the rest removed). The optional
   `-all-resources-bound` is written behind `-dxc_all_bound`; its batch runs in the final test phase (6.4).
8. Docs: `DX12_RENDERER.md` (compiler, cache, console, caps, fallback), `TEST_MATRIX.md` rows,
   `infra.md`/`dxr.md` cross-references to this document.

## 13. Implementation record (D3a, D3b)

**D3a - `da_dxc` (28.09.2026).** `xrRenderPC_R4/da_dxc.h/.cpp` (bodies under `XR_D3D12`, R4 stubs):
- `da_dxc_init(device)` from `dx11HW.cpp` (after the caps; the caps are read again after it):
  engine model `min(device, DXC, 6.6)`; `da_dxc_native_model()` = `min(device, DXC)` for native passes,
  independent of `r__dxc` (native passes have no FXC path); reasons in the order `-dx12_no_sm6`, not a
  DirectX 12 device, no DXC service, no SM 6 on the device, `r__dxc 0`. Cache tag
  `sm66-w32-m16-<crc32 of the DXC version, the fixed engine and native arguments and the flags>`.
  `-dxc_debug` adds `-Zi -Qembed_debug`, debug builds compile `-Od`, `-shader_warnings_as_errors`
  adds `-WX`, `-log_shader_warnings` logs DXC's warnings.
- `da_dxc_compile_native(DaDxcNative, bytecode, reflection*)`: a file under the shader root or an
  embedded `source`; HLSL 2021, `-Zpr -O3`, DXC's preprocessor with the file system include handler
  (includer's folder, then the root through `-I .`), macros `GRIZZLY_DXC`, `GRIZZLY_SM` (of the target),
  `GRIZZLY_WAVE_LANES`, `GRIZZLY_MIN16`, `SMAP_ARRAY`, optional `-enable-16bit-types` where the device has
  native fp16; cache `r5_dxil/<tag>/native/<path>.<entry>.<target>/<crc of target, entry, defines>`
  validated by the CRC of the source and of every include it resolves.
- DXC's `DXC_OUT_REFLECTION` is a bare `STAT` part (fourcc, size, module), not a container:
  `xrd3d12::Reflect` recognises DXC blobs by the container's parts and sent it to `D3DReflect`, which
  fails (the first self-test run reported 0 bindings). `da_dxc` wraps every reflection blob into a
  one-part container before it is cached or handed out (`da_dxc_contain`), which both reflectors take.
- Statistics at the end of `level_Load` and in `CHW::DestroyDevice`; an embedded self-test at init
  (a cs_6_0 with an include, a wave op under `GRIZZLY_WAVE_LANES` and one UAV) logs
  `* [dxc] native self-test: native/dxc_selftest.hlsl cs_6_0 compiled, 1684 bytes of DXIL, 1 binding, 17.3 ms`.

**D3b - the engine path.**
- `CommonTypes.h`: the D3D12 reflection interfaces and descs under `XR_D3D12` (plus
  `D3D_SIGNATURE_PARAMETER_DESC`); R4 keeps the D3D11 ones.
- `SH_Atomic.h`: `dxil` and `source` on `SVS..SCS`; `r_constants.h`: `R_constant_table::dxil`;
  `ResourceManager.h`: `RSC_FXC` (bit 31, never reaches D3DCompile) and `flags` on
  `_CreatePS/_GS/_HS/_DS/_CS`.
- `ShaderResourceTraits.h` `CreateShader`: the map key gets `|fxc` for `RSC_FXC`; `source` is kept;
  `name#ms` (vertex slot) / `name#as` (geometry slot) open `name.ms` / `name.as` with targets
  `ms_6_5` / `as_6_5` and no `stub_default` stand-in (a failure leaves `sh` null for the caller).
- `r4_shaders.cpp`: `compile_dxil` under `XR_D3D12` when `da_dxc_active()` and no `RSC_FXC`: cache
  `shaders_cache_grizzly/r5_dxil/<tag>/<name>.<ext>/<sh_name>`, else `da_dxc_compile_engine` (FXC's
  preprocessor with the engine includer and macros + the `GRIZZLY_*` macros, target letters of the
  engine's target with the session's model, HLSL 2018 or 2021 for `.ms/.as`, the arguments of 3.3);
  a failure logs `! [dxc] <shader>: <first error>` and the defines and falls through to FXC
  (`ms`/`as` have no FXC path). `create_shader` takes the reflection object (DXIL: the reflection
  blob, DXBC: the bytecode, both through `xrd3d12::Reflect`; R4 `D3DReflect`), reuses it for the VS
  `position_only` test, routes `ms` into `SVS` and `as` into `SGS`, releases the HW shader when it
  cannot reflect it, and `-disasm` writes `<name>.<ext>.dxil.txt` through `xrd3d12::Disassemble`.
- `dx11r_constants.cpp` (C6): the constant-buffer slot is the binding's `BindPoint` for DXIL; for DXBC
  the reflection index, with one log line if it ever differs from the register.
- `CBlender_Compile::UnifyShaderFormats()` (`Blender_Recorder_R3.cpp`): called in both `r_Pass`
  overloads (`Blender_Recorder_R3.cpp`, and `xrRender/Blender_Recorder_R2.cpp` - the two-shader pass the
  `.s` scripts use), in `r_TessPass` (table rebuilt when it
  replaced a stage) and `PassSET_Shaders` (create first, merge after); a pass with DXIL and DXBC
  stages gets `_CreateX(source, RSC_FXC)` twins for its DXIL stages and
  `! [dxc] pass vs/ps: DXBC twins for vs (ps fell back to FXC)` once per stage combination (blenders
  build the same pass for many elements); a pass with an `#ms`/`#as` stage is left alone and logged as
  not usable (C5). `_DeleteVS` tolerates a VS without input signature.
- A shader DXC refused is remembered for the session by cache file and source CRC: later requests of
  the same permutation go straight to FXC without a second error line (the check comes before the cache
  probe). An object the layer rejects (`CreateHWShader` or reflection fails after a successful compile)
  is refused the same way and never written to the cache: the cache file is stored only after the
  layer accepted the object (final-phase fix DXC-R2). The statistics line counts DXC's refusals:
  `* [dxc] shaders: 0 compiled (0.0 s), 202 cached, 1 on FXC, 4 twins, 1 refused by DXC; native ...`.
- Error lines name the file relative to the shader root (`da_taa.ps:85:1: error: ...`), not the rig's
  absolute path DXC reports for FXC's `#line` markers.
- Measured (section 9, D3b results): cold start with DXC 61 s vs FXC 108 s on `qa_pedro` Extreme
  (198 DXIL shaders in 15.2 s of compile time), frames of the DXC and FXC paths within PSNR 50-56 dB,
  DX11 frames equal to Gate 0.

**D5 - payoff (28.09.2026).**
- `da_dxc.cpp`: `-dxc_plain` (`DaDxcSetup::plain`): `da_dxc_compile_engine` leaves `GRIZZLY_WAVE_LANES` and
  `GRIZZLY_MIN16` out, the engine cache folder gets a `-plain` suffix, one setup line; native compiles
  unchanged. `-dxc_all_bound` (`DaDxcSetup::all_bound`): `-all-resources-bound` on every engine compile,
  cache suffix `-arb`, one setup line; native compiles unchanged.
- `da_taa.ps`: under `GRIZZLY_WAVE_LANES` the pass-through tests are decided per quad first
  (`QuadReadAcrossX/Y/Diagonal` of the lane's flag), a quad with any blending pixel builds its 3x3
  moments in `taa_moments` (three own samples on the lane's outer side, five from the quad), and the
  8-sample loop is compiled only without the macro. FXC, DirectX 11 and `-dxc_plain` compile the loop.

## Final test phase, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

- Every R5 run of the final stand (23 logs) prints `* [dxc] dxcompiler 1.9.0.5402, engine shaders SM 6.6 (device 6.8),
  waves 32-32, min16 yes` and `* [dxc] shaders: ... 0 on FXC, 0 twins`; none has `! [dxc]`, `refused` of a DXC
  permutation or `mixes DXIL`.
- The DXC-only tree against Gate 0, re-measured with the final code: qa_pedro 98.95 % within, 51.8 dB; foliage
  93.4 %, 46.7 dB (`final-dxconly-<save>`) - the margin D3b recorded (99.10 % / 93.7 %: compilers not bit-exact plus
  the GTAO filter corner, >= 45 dB). It served as the control that attributed the other features' parity.
- The one refused pipeline of the stand (`shadow_direct_tree.vs` + `shadow_direct_base_aref.ps`) was not a DXC
  linkage problem: a stale pixel shader of a pooled backend (final\stand-fixes.md S2, fixed).

## Final test phase, performance (Perf phase, 28.09.2026)

At the user's request the Perf phase ran one measurement pass instead of on/off batches (PLAN 8.4): DXC against FXC
(`-dx12_no_sm6`) and the `-dxc_all_bound` A/B were not run.
- DXC at SM 6.6 is the R5 shader path: mesh-shader grass and ray tracing need DXIL, and D4 (section 9) measured no
  pass slower than under FXC; the cold start with an empty cache was 61 s against 108 s (D3b). FXC stays the
  fallback (`r__dxc 0`, `-dx12_no_sm6`, a shader DXC refuses).
- `final-onepass-foliage` (everything on): `* [dxc] dxcompiler 1.9.0.5402, engine shaders SM 6.6 (device 6.8), waves
  32-32, min16 yes`, `shaders: 1 compiled (0.2 s), 223 cached, 0 on FXC, 0 twins; native 0 compiled (0.0 s), 13
  cached`; no `! [dxc]`, refused or `mixes DXIL` line.
- `-dxc_all_bound` stays a start-up QA switch, off by default and unmeasured; `-dxc_plain` stays the way around a
  driver that mishandles the wave, quad or 16-bit paths.
