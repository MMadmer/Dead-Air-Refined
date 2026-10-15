// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The engine side of DXC / Shader Model 6 on R5 (docs/dead-air/dx12-features/dxc.md 3.1-3.8): the
// setup after device creation, whether engine shaders are DXIL this session, the engine-shader and
// native-shader compiles with their caches, and the statistics line. The R4 module answers "FXC".
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// After the device exists (CHW::CreateDevice): shader model choice, "[dxc]" setup line, native self-test.
void da_dxc_init(ID3D11Device* device);
// Engine shaders are DXIL this session (r__caps dxc=).
bool da_dxc_active();
// Shader model of the engine shaders (0x60..0x66), 0 while they stay on FXC.
u32 da_dxc_shader_model();
// Highest shader model a native pass may target (min(device, DXC)), 0 without the DXC service.
// Independent of r__dxc: native passes compile through DXC whenever it is up.
u32 da_dxc_native_model();
// "[dxc] shaders: ..." at the end of level_Load and at shutdown.
void da_dxc_log_stats();

struct DaDxcDefine
{
    pcstr name;
    pcstr value; // null = "1"
};

enum DaDxcNativeFlags : u32
{
    DaDxcNative16BitTypes = 1u << 0, // -enable-16bit-types; ignored where the device has no native fp16
};

struct DaDxcNative
{
    pcstr path{};   // file under the shader root, e.g. "rt/rt_ao.hlsl"; names `source` when that is set
    pcstr source{}; // embedded text instead of the file (includes still resolve through the file system)
    pcstr entry{};  // null for libraries (lib_6_x)
    pcstr target{}; // "cs_6_5", "lib_6_3", "ms_6_5", ...
    const DaDxcDefine* defines{};
    u32 define_count{};
    u32 flags{}; // DaDxcNativeFlags
};

// A native pass shader: HLSL 2021, row-major matrices like every engine shader, DXC's own preprocessor
// with #include through the file system (relative to the including file first, then the shader
// root), always defined GRIZZLY_DXC, GRIZZLY_SM (of the target), GRIZZLY_WAVE_LANES (with wave ops), GRIZZLY_MIN16
// (with 16-bit min precision) and SMAP_ARRAY. Cached under
// shaders_cache_grizzly/r5_dxil/<tag>/native/<path>.<entry>.<target>/<defines hash>. Thread-safe.
// false without the DXC service, for a target above da_dxc_native_model() or on a compile error
// (logged with the first error). `reflection` receives DXC's reflection blob (xrd3d12::Reflect).
bool da_dxc_compile_native(const DaDxcNative& request, xr_vector<u8>& bytecode, xr_vector<u8>* reflection = nullptr);
// Shorthand for a file.
bool da_dxc_compile_native(pcstr path, pcstr entry, pcstr target, const DaDxcDefine* defines, u32 define_count,
    xr_vector<u8>& bytecode);

// ---- engine path (r4_shaders.cpp, CBlender_Compile::UnifyShaderFormats; RSC_FXC in ResourceManager.h) ----

// "r5_dxil\<tag>\" under shaders_cache_grizzly ("-plain" / "-arb" appended under -dxc_plain /
// -dxc_all_bound), "" while engine shaders stay on FXC.
pcstr da_dxc_cache_dir();

// One engine shader through DXC (dxc.md 3.3): FXC's preprocessor with the engine's includer and
// macros (plus the GRIZZLY_* macros), then DXC with the engine's arguments. `target` is the engine's
// ("vs_5_0", "ms_6_5"): the stage letters are kept, the model becomes the session's. Mesh and
// amplification shaders compile at HLSL 2021, everything else at 2018. On failure `error` holds the
// first error line.
bool da_dxc_compile_engine(pcstr name, const void* source, size_t size, const D3D_SHADER_MACRO* macros,
    ID3DInclude* include, pcstr entry, pcstr target, xr_vector<u8>& object, xr_vector<u8>& reflection,
    xr_string& error);

// DXIL cache file of an engine shader: object + reflection, validated by the source CRC (as the FXC
// cache) and a CRC of the contents. A hit counts as "cached".
bool da_dxc_cache_load(pcstr file, u32 source_crc, xr_vector<u8>& object, xr_vector<u8>& reflection);
void da_dxc_cache_store(pcstr file, u32 source_crc, const xr_vector<u8>& object, const xr_vector<u8>& reflection);

// Statistics of the engine path: a shader DXC could not build (it went to FXC), DXBC twins.
void da_dxc_note_fxc();
void da_dxc_note_twins(u32 stages);
// A permutation DXC refused this session (its cache file and source CRC) goes straight to FXC.
bool da_dxc_refused(pcstr file, u32 source_crc);
void da_dxc_note_refusal(pcstr file, u32 source_crc);
} // namespace xray::render::RENDER_NAMESPACE
