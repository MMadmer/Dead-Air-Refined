// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// DXC / Shader Model 6 engine glue (da_dxc.h, docs/dead-air/dx12-features/dxc.md 3.2-3.8). The
// compiler itself is the layer's service (xrRenderDX12/d12_dxc.cpp); this file decides what the
// session compiles with, builds the argument lists, caches and counts.

#include "stdafx.h"

#include "da_dxc.h"
#include "da_native_caps.h"
#include "r2.h"

#if defined(XR_NATIVE)
// The DXC service of xrD3D12 serves both native modules; Vulkan reflects the DXIL twin of every shader.
#include "Layers/xrRenderDX12/d12_api.h"
#if defined(XR_VULKAN)
#include "Layers/xrRenderVK/vk_api.h"
#endif

#include <d3d12shader.h>

#include <atomic>
#include <chrono>
#include <mutex>
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(XR_NATIVE)
namespace
{
#if defined(XR_VULKAN)
// The shader caches of the Vulkan module: containers (DXIL twin + SPIR-V + registers), not plain DXIL.
constexpr char k_cache_root[] = "r6_spirv";
#else
constexpr char k_cache_root[] = "r5_dxil";
#endif
// Engine shaders never target more: 6.7+ brings nothing they use, and every model is a cache
// directory and an audit run (dxc.md 3.2).
constexpr u32 k_engine_model_cap = 0x66;
constexpr u32 k_cache_magic = 0x314C5844; // "DXL1"

// dxc.md 3.3 step 4. -E, -T, -HV and the strip flags come from xrd3d12::CompileShader.
constexpr const wchar_t* k_engine_args[] = {
    L"-Zpr", L"-flegacy-resource-reservation", L"-Wno-conversion", L"-Wno-for-redefinition", L"-Wno-effects-syntax"};
// Native passes: DXC's preprocessor; "-I ." makes the shader root the fallback after the includer's folder.
constexpr const wchar_t* k_native_args[] = {L"-Zpr", L"-I", L"."};

struct DaDxcSetup
{
    bool engine{};         // engine shaders through DXC this session
    u32 engine_model{};    // 0x60..0x66
    u32 native_model{};    // min(device, DXC), 0 = no service
    u32 wave_lanes{};      // GRIZZLY_WAVE_LANES, 0 without wave ops
    bool min16{};          // GRIZZLY_MIN16
    bool plain{};          // -dxc_plain: engine shaders without GRIZZLY_WAVE_LANES / GRIZZLY_MIN16
    bool all_bound{};      // -dxc_all_bound: engine shaders with -all-resources-bound
    bool fp16{};           // native 16-bit types
    u32 flags{};           // xrd3d12::ShaderCompileFlags of every compile
    bool optimize{};       // -O3 (not in debug builds)
    string32 tag{};        // sm66-w32-m16-<crc>
    string64 cache_dir{};  // "r5_dxil\<tag>[-plain][-arb]\" while engine shaders are DXIL ("r6_spirv\..." on Vulkan)
    string16 engine_sm{};  // GRIZZLY_SM of engine shaders ("66")
    string16 wave{};       // GRIZZLY_WAVE_LANES value
};
DaDxcSetup g_dxc;

struct DaDxcStats
{
    std::atomic<u32> compiled{}, cached{}, fxc{}, twins{}, failed{};
    std::atomic<u32> native_compiled{}, native_cached{}, native_failed{};
    std::atomic<u64> compile_us{}, native_us{};
};
DaDxcStats g_stats;

u64 da_dxc_now_us()
{
    using namespace std::chrono;
    return u64(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// "D:\game\dir\file.ps:85:1: error: ..." -> "file.ps:85:1: error: ...": FXC's #line markers name the
// file relative to the working directory, which DXC makes absolute.
xr_string da_dxc_short_position(const std::string& line)
{
    for (size_t colon = line.find(':', 2); colon != std::string::npos; colon = line.find(':', colon + 1))
    {
        if (colon + 1 < line.size() && isdigit(u8(line[colon + 1])))
        {
            const size_t slash = line.find_last_of("\\/", colon);
            return (slash == std::string::npos ? line : line.substr(slash + 1)).c_str();
        }
    }
    return line.c_str();
}

// The first line that reports an error, else the first line.
xr_string da_dxc_first_error(const std::string& messages)
{
    xr_string first;
    size_t begin = 0;
    while (begin < messages.size())
    {
        size_t end = messages.find('\n', begin);
        if (end == std::string::npos)
            end = messages.size();
        const std::string line = messages.substr(begin, end - begin);
        if (first.empty() && !line.empty())
            first = da_dxc_short_position(line);
        if (line.find("error") != std::string::npos)
            return da_dxc_short_position(line);
        begin = end + 1;
    }
    return first.empty() ? xr_string("compilation failed") : first;
}

// Shaders DXC refused this session, by cache file and source CRC: they go to FXC again without a
// second attempt and a second error line.
std::mutex g_refusals_lock;
xr_vector<u64> g_refusals;

u64 da_dxc_refusal_key(pcstr file, u32 source_crc) { return (u64(crc32(file, u32(xr_strlen(file)))) << 32) | source_crc; }

// "cs_6_5" -> 0x65; 0 when the target names no shader model 6.
u32 da_dxc_target_model(pcstr target)
{
    pcstr us = target ? strchr(target, '_') : nullptr;
    if (!us || us[1] != '6' || us[2] != '_' || us[3] < '0' || us[3] > '9')
        return 0;
    return 0x60 + u32(us[3] - '0');
}

// ---- files under the shader root ----

// Forward slashes, no "./" parts, "a/../" collapsed.
xr_string da_dxc_normalize(pcstr path)
{
    xr_vector<xr_string> parts;
    xr_string part;
    for (pcstr p = path;; ++p)
    {
        if (*p == '/' || *p == '\\' || !*p)
        {
            if (part == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            part.clear();
            if (!*p)
                break;
        }
        else
            part += *p;
    }
    xr_string out;
    for (const xr_string& p : parts)
    {
        if (!out.empty())
            out += '/';
        out += p;
    }
    return out;
}

IReader* da_dxc_open(const xr_string& path)
{
    string_path name;
    strconcat(sizeof(name), name, RImplementation.getShaderPath(), path.c_str());
    for (char& c : name)
    {
        if (c == '/')
            c = '\\';
    }
    return FS.r_open("$game_shaders$", name);
}

xr_string da_dxc_folder(const xr_string& path)
{
    const size_t slash = path.rfind('/');
    return slash == xr_string::npos ? xr_string() : path.substr(0, slash + 1);
}

// The quoted or angled name of an #include line, empty for any other line.
xr_string da_dxc_include_name(pcstr line, pcstr end)
{
    while (line < end && (*line == ' ' || *line == '\t'))
        ++line;
    if (line >= end || *line != '#')
        return {};
    ++line;
    while (line < end && (*line == ' ' || *line == '\t'))
        ++line;
    if (end - line < 7 || strncmp(line, "include", 7) != 0)
        return {};
    line += 7;
    while (line < end && (*line == ' ' || *line == '\t'))
        ++line;
    if (line >= end || (*line != '"' && *line != '<'))
        return {};
    const char close = *line == '"' ? '"' : '>';
    pcstr name = ++line;
    while (line < end && *line != close)
        ++line;
    return line < end ? xr_string(name, line) : xr_string();
}

// crc32 of a native source and every file it includes, resolved like the compile does: the
// including file's folder first, then the shader root. Missing files are left to DXC to report.
void da_dxc_native_crc(const xr_string& path, pcstr text, size_t size, u32& crc, xr_vector<xr_string>& visited)
{
    if (visited.size() > 256 || std::find(visited.begin(), visited.end(), path) != visited.end())
        return;
    visited.push_back(path);
    crc = crc32(text, u32(size), crc);
    const xr_string folder = da_dxc_folder(path);
    for (pcstr line = text, end = text + size; line < end;)
    {
        pcstr eol = line;
        while (eol < end && *eol != '\n')
            ++eol;
        const xr_string include = da_dxc_include_name(line, eol);
        line = eol + 1;
        if (include.empty())
            continue;
        for (const xr_string& candidate : {da_dxc_normalize((folder + include).c_str()), da_dxc_normalize(include.c_str())})
        {
            if (IReader* file = da_dxc_open(candidate))
            {
                da_dxc_native_crc(candidate, static_cast<pcstr>(file->pointer()), file->length(), crc, visited);
                FS.r_close(file);
                break;
            }
        }
    }
}

bool da_dxc_native_include(void* /*user*/, const char* file, std::string& text)
{
    const xr_string path = da_dxc_normalize(file);
    IReader* reader = path.empty() ? nullptr : da_dxc_open(path);
    if (!reader)
        return false;
    text.assign(static_cast<const char*>(reader->pointer()), reader->length());
    FS.r_close(reader);
    return true;
}

#if defined(XR_VULKAN)
// The registers the DXIL reflection gave every resource of space 0: the layer binds the SPIR-V twin's
// resources of the same names there, so the engine's D3D11 slots mean the same for both.
bool da_dxc_binding_table(const xr_vector<u8>& reflection, xr_vector<xrvk::ShaderBinding>& table)
{
    table.clear();
    ID3D12ShaderReflection* shader = nullptr;
    if (FAILED(xrd3d12::Reflect(reflection.data(), reflection.size(), __uuidof(ID3D12ShaderReflection),
            reinterpret_cast<void**>(&shader))) ||
        !shader)
        return false;
    D3D12_SHADER_DESC desc{};
    shader->GetDesc(&desc);
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(shader->GetResourceBindingDesc(i, &b)) || !b.Name || b.Space != 0 || !b.BindCount)
            continue;
        xrvk::ShaderBinding e{};
        switch (b.Type)
        {
        case D3D_SIT_CBUFFER: e.cls = xrvk::ShaderBindingCB; break;
        case D3D_SIT_SAMPLER: e.cls = xrvk::ShaderBindingSampler; break;
        case D3D_SIT_UAV_RWTYPED:
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_RWBYTEADDRESS:
        case D3D_SIT_UAV_APPEND_STRUCTURED:
        case D3D_SIT_UAV_CONSUME_STRUCTURED:
        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        case D3D_SIT_UAV_FEEDBACKTEXTURE: e.cls = xrvk::ShaderBindingUAV; break;
        default: e.cls = xrvk::ShaderBindingSRV; break;
        }
        strncpy_s(e.name, b.Name, sizeof(e.name) - 1);
        e.reg = u16(b.BindPoint);
        e.count = u8(std::min(b.BindCount, 255u));
        table.push_back(e);
    }
    shader->Release();
    return true;
}

// The SPIR-V twin of a DXIL compile (same source, defines and target) and the container the layer
// takes: DXIL for the signature and reflection, SPIR-V for the device, the registers of the resources.
bool da_dxc_spirv(const xrd3d12::ShaderCompile& base, const xr_vector<u8>& dxil, const xr_vector<u8>& reflection,
    xr_vector<u8>& object, xr_string& error)
{
    xr_vector<xrvk::ShaderBinding> table;
    if (!da_dxc_binding_table(reflection, table))
    {
        error = "the DXIL reflection of the shader cannot be read";
        return false;
    }
    UINT spirv_count = 0;
    const wchar_t* const* spirv_args = xrvk::SpirvCompileArgs(spirv_count);
    xr_vector<const wchar_t*> args;
    args.reserve(base.argCount + spirv_count);
    for (UINT i = 0; i < base.argCount; ++i)
    {
        // DXIL-only switches: register reservation and resource binding assumptions mean nothing in SPIR-V.
        if (base.args[i] && wcscmp(base.args[i], L"-flegacy-resource-reservation") &&
            wcscmp(base.args[i], L"-all-resources-bound"))
            args.push_back(base.args[i]);
    }
    args.insert(args.end(), spirv_args, spirv_args + spirv_count);
    // The SPIR-V-only paths of the shaders (da_raw_buffer.h); the DXIL twin gives the reflection without them.
    args.push_back(L"-D");
    args.push_back(L"GRIZZLY_SPIRV=1");
    // RWTexture2D<float> is any single-channel UAV in D3D11 (R8, R16F, R32F...): storage images take the view's
    // format. Image atomics need theirs in the shader: such a shader compiles again with the HLSL types' formats.
    args.push_back(L"-fspv-use-unknown-image-format");
    xrd3d12::ShaderCompile compile = base;
    compile.args = args.data();
    compile.argCount = UINT(args.size());
    compile.flags = (compile.flags & ~u32(xrd3d12::CompileKeepReflection)) | xrd3d12::CompileSpirv;
    xrd3d12::ShaderOutput output;
    bool ok = xrd3d12::CompileShader(compile, output);
    if (!ok && output.messages.find("OpImageTexelPointer") != std::string::npos)
    {
        compile.argCount = UINT(args.size() - 1);
        ok = xrd3d12::CompileShader(compile, output);
    }
    if (!ok)
    {
        error = "SPIR-V: ";
        error += da_dxc_first_error(output.messages);
        return false;
    }
    std::vector<unsigned char> container;
    xrvk::BuildShaderContainer(dxil.data(), dxil.size(), output.object.data(), output.object.size(), table.data(),
        table.size(), container);
    object.assign(container.begin(), container.end());
    return true;
}
#endif

// DXC_OUT_REFLECTION is a bare STAT part (fourcc, size, module). Wrapped into a one-part container
// it is what xrd3d12::Reflect and every other reflector takes.
void da_dxc_contain(xr_vector<u8>& reflection)
{
    constexpr u32 header = 36; // magic, digest, version, size, part count, one part offset
    if (reflection.size() < 8 || !memcmp(reflection.data(), "DXBC", 4))
        return;
    const u32 version = 1, size = header + u32(reflection.size()), parts = 1;
    xr_vector<u8> container(size, 0);
    memcpy(container.data(), "DXBC", 4);
    memcpy(&container[20], &version, 4);
    memcpy(&container[24], &size, 4);
    memcpy(&container[28], &parts, 4);
    memcpy(&container[32], &header, 4);
    memcpy(&container[header], reflection.data(), reflection.size());
    reflection = std::move(container);
}

// ---- cache files: u32 'DXL1', u32 source crc, u32 object size, u32 reflection size,
// u32 crc32(object + reflection), object, reflection ----

bool da_dxc_cache_read(pcstr file, u32 source_crc, xr_vector<u8>& object, xr_vector<u8>& reflection)
{
    if (!FS.exist(file))
        return false;
    IReader* reader = FS.r_open(file);
    if (!reader)
        return false;
    bool ok = false;
    if (reader->length() > 20 && reader->r_u32() == k_cache_magic && reader->r_u32() == source_crc)
    {
        const u32 object_size = reader->r_u32();
        const u32 reflection_size = reader->r_u32();
        const u32 saved_crc = reader->r_u32();
        if (object_size && u64(object_size) + reflection_size == u64(reader->elapsed()) &&
            crc32(reader->pointer(), object_size + reflection_size) == saved_crc)
        {
            const u8* data = static_cast<const u8*>(reader->pointer());
            object.assign(data, data + object_size);
            reflection.assign(data + object_size, data + object_size + reflection_size);
            da_dxc_contain(reflection);
            ok = true;
        }
    }
    FS.r_close(reader);
    return ok;
}

void da_dxc_cache_write(pcstr file, u32 source_crc, const xr_vector<u8>& object, const xr_vector<u8>& reflection)
{
    IWriter* writer = FS.w_open(file);
    if (!writer)
        return;
    u32 crc = crc32(object.data(), u32(object.size()));
    if (!reflection.empty())
        crc = crc32(reflection.data(), u32(reflection.size()), crc);
    writer->w_u32(k_cache_magic);
    writer->w_u32(source_crc);
    writer->w_u32(u32(object.size()));
    writer->w_u32(u32(reflection.size()));
    writer->w_u32(crc);
    writer->w(object.data(), object.size());
    if (!reflection.empty())
        writer->w(reflection.data(), reflection.size());
    FS.w_close(writer);
}

void da_dxc_selftest()
{
    // Exercises the include handler (the shader root after the includer's folder), the macros, a
    // wave intrinsic where the device has them, the cache and the reflection of a native pass.
    static constexpr char text[] = "#include \"common_defines.h\"\n"
                                   "RWByteAddressBuffer result : register(u0);\n"
                                   "[numthreads(64, 1, 1)]\n"
                                   "void main(uint3 id : SV_DispatchThreadID)\n"
                                   "{\n"
                                   "    float luminance = dot(float3(id), LUMINANCE_VECTOR);\n"
                                   "#if defined(GRIZZLY_WAVE_LANES)\n"
                                   "    luminance = WaveActiveSum(luminance);\n"
                                   "#endif\n"
                                   "    if (id.x == 0)\n"
                                   "        result.Store(0, asuint(luminance));\n"
                                   "}\n";
    DaDxcNative request;
    request.path = "native/dxc_selftest.hlsl";
    request.source = text;
    request.entry = "main";
    request.target = "cs_6_0";
    const u32 cached = g_stats.native_cached;
    const u64 start = da_dxc_now_us();
    xr_vector<u8> object, reflection;
    if (!da_dxc_compile_native(request, object, &reflection))
    {
        Msg("! [dxc] native self-test failed: %s %s", request.path, request.target);
        return;
    }
    const float ms = float(da_dxc_now_us() - start) / 1000.f;
    UINT bindings = 0;
    ID3D12ShaderReflection* shader = nullptr;
    if (SUCCEEDED(xrd3d12::Reflect(reflection.data(), reflection.size(), __uuidof(ID3D12ShaderReflection),
            reinterpret_cast<void**>(&shader))) && shader)
    {
        D3D12_SHADER_DESC desc{};
        if (SUCCEEDED(shader->GetDesc(&desc)))
            bindings = desc.BoundResources;
        shader->Release();
    }
#if defined(XR_VULKAN)
    xrvk::ShaderContainerView view;
    const bool dxil = xrvk::ParseShaderContainer(object.data(), object.size(), view) && view.dxil && view.spirv &&
        xrd3d12::IsDxil(view.dxil, view.dxilSize);
#else
    const bool dxil = xrd3d12::IsDxil(object.data(), object.size());
#endif
    if (!dxil || bindings != 1)
    {
        Msg("! [dxc] native self-test: %s %s gave %s with %u binding(s)", request.path, request.target,
            dxil ? "DXIL" : "no DXIL", bindings);
        return;
    }
    Msg("* [dxc] native self-test: %s %s %s, %u bytes of %s, %u binding, %.1f ms", request.path, request.target,
        g_stats.native_cached != cached ? "from the cache" : "compiled", u32(object.size()),
#if defined(XR_VULKAN)
        "DXIL + SPIR-V",
#else
        "DXIL",
#endif
        bindings, ms);
}
} // namespace

void da_dxc_init(ID3D11Device* device)
{
    g_dxc = DaDxcSetup{};
    const CHWNativeCaps& native = HW.native;
#if defined(XR_VULKAN)
    // No FXC on Vulkan: the compiler is the only way to SPIR-V, loaded here (the DX12 layer loads it
    // with its device).
    char load_reason[256]{};
    const bool loaded = xrd3d12::LoadShaderCompiler(nullptr, load_reason, sizeof(load_reason));
    const xrvk::NativeCaps* caps = device ? xrvk::GetNativeCaps(device) : nullptr;
    if (loaded && caps)
        g_dxc.native_model = std::min(k_engine_model_cap, xrd3d12::DxcShaderModel());
    if (!loaded)
        Msg("! [dxc] the shader compiler did not load: %s - the Vulkan renderer cannot build shaders", load_reason);
#else
    const xrd3d12::NativeCaps* caps = device ? xrd3d12::GetNativeCaps(device) : nullptr;
    // HW.native carries the masks of the -dx12_no_* switches.
    if (native.shaderModel >= 0x60 && native.dxcShaderModel >= 0x60)
        g_dxc.native_model = std::min(native.shaderModel, native.dxcShaderModel);
#endif

    pcstr reason = nullptr;
#if defined(XR_VULKAN)
    if (!caps)
        reason = "not a Vulkan device";
    else if (!loaded)
        reason = "the DXC service is not up";
#else
    if (da_native_disabled_features() & DaNativeDisableSM6)
        reason = "-dx12_no_sm6";
    else if (!caps)
        reason = "not a DirectX 12 device";
    else if (!native.dxcShaderModel)
        reason = "the DXC service is not up (see the shader compiler line)";
    else if (!native.shaderModel)
        reason = "the device has no shader model 6";
    else if (!ps_r__dxc)
        reason = "r__dxc 0";
#endif

    if (!g_dxc.native_model)
    {
        Msg("* [dxc] engine shaders stay on FXC: %s", reason ? reason : "no shader model 6");
        return;
    }

    g_dxc.engine_model = std::min(g_dxc.native_model, k_engine_model_cap);
    g_dxc.wave_lanes = native.waveOps ? native.waveLanesMin : 0;
#if defined(XR_VULKAN)
    // min16float is RelaxedPrecision in SPIR-V: drivers keep full precision where 16 bits do not pay.
    g_dxc.min16 = caps != nullptr;
#else
    g_dxc.min16 = caps && caps->minPrecision16;
#endif
    g_dxc.fp16 = native.native16Bit;
    g_dxc.optimize = true;
#ifdef DEBUG
    g_dxc.flags |= xrd3d12::CompileSkipOptimization | xrd3d12::CompileDebugInfo;
    g_dxc.optimize = false;
#endif
    // Source-level debugging in PIX / Nsight.
    if (strstr(Core.Params, "-dxc_debug"))
        g_dxc.flags |= xrd3d12::CompileDebugInfo;
    if (strstr(Core.Params, "-shader_warnings_as_errors"))
        g_dxc.flags |= xrd3d12::CompileWarningsAsErrors;
    // The wave, quad and min16 paths of the engine shaders off (dxc.md 6.4): their A/B inside one
    // binary, and the way around a driver that mishandles them. Native passes keep the macros.
    if (strstr(Core.Params, "-dxc_plain"))
        g_dxc.plain = true;
    // The layer fills every declared table slot (null descriptors for unbound ones), so the driver may
    // take all resources as bound; its A/B against the default (dxc.md 6.4).
    if (strstr(Core.Params, "-dxc_all_bound"))
        g_dxc.all_bound = true;
    xr_sprintf(g_dxc.engine_sm, "%X", g_dxc.engine_model);
    if (g_dxc.wave_lanes)
        xr_sprintf(g_dxc.wave, "%u", g_dxc.wave_lanes);

    // The tag names everything that changes the bytecode: compiler version, arguments, flags.
    pcstr version = xrd3d12::DxcVersion();
    u32 crc = crc32(version, u32(xr_strlen(version)));
    for (const wchar_t* arg : k_engine_args)
        crc = crc32(arg, u32(wcslen(arg) * sizeof(wchar_t)), crc);
    for (const wchar_t* arg : k_native_args)
        crc = crc32(arg, u32(wcslen(arg) * sizeof(wchar_t)), crc);
#if defined(XR_VULKAN)
    {
        UINT spirv_count = 0;
        const wchar_t* const* spirv_args = xrvk::SpirvCompileArgs(spirv_count);
        for (UINT i = 0; i < spirv_count; ++i)
            crc = crc32(spirv_args[i], u32(wcslen(spirv_args[i]) * sizeof(wchar_t)), crc);
        const u32 container = xrvk::kShaderContainerVersion;
        crc = crc32(&container, sizeof(container), crc);
        constexpr char vulkan_define[] = "GRIZZLY_VULKAN -fspv-use-unknown-image-format";
        crc = crc32(vulkan_define, sizeof(vulkan_define) - 1, crc);
    }
#endif
    crc = crc32(&g_dxc.flags, sizeof(g_dxc.flags), crc);
    xr_sprintf(g_dxc.tag, "sm%X-w%u-m%u-%08x", g_dxc.engine_model, g_dxc.wave_lanes, g_dxc.min16 ? 16 : 32, crc);

#if defined(XR_VULKAN)
    const u32 device_model = g_dxc.native_model;
#else
    const u32 device_model = native.shaderModel;
#endif
    if (reason)
    {
        Msg("* [dxc] engine shaders stay on FXC: %s (native passes: dxcompiler %s, SM %u.%u)", reason, version,
            g_dxc.native_model >> 4, g_dxc.native_model & 0xF);
    }
    else
    {
        g_dxc.engine = true;
        xr_sprintf(g_dxc.cache_dir, "%s" DELIMITER "%s%s%s" DELIMITER, k_cache_root, g_dxc.tag, g_dxc.plain ? "-plain" : "",
            g_dxc.all_bound ? "-arb" : "");
        if (g_dxc.wave_lanes)
        {
            Msg("* [dxc] dxcompiler %s, engine shaders SM %u.%u (device %u.%u), waves %u-%u, min16 %s", version,
                g_dxc.engine_model >> 4, g_dxc.engine_model & 0xF, device_model >> 4, device_model & 0xF,
                native.waveLanesMin, native.waveLanesMax, g_dxc.min16 ? "yes" : "no");
        }
        else
        {
            Msg("* [dxc] dxcompiler %s, engine shaders SM %u.%u (device %u.%u), no wave ops, min16 %s", version,
                g_dxc.engine_model >> 4, g_dxc.engine_model & 0xF, device_model >> 4, device_model & 0xF,
                g_dxc.min16 ? "yes" : "no");
        }
        if (g_dxc.plain)
            Msg("* [dxc] -dxc_plain: engine shaders without the wave, quad and min16 paths");
        if (g_dxc.all_bound)
            Msg("* [dxc] -dxc_all_bound: engine shaders compiled with -all-resources-bound");
    }
    da_dxc_selftest();
}

bool da_dxc_active() { return g_dxc.engine; }

u32 da_dxc_shader_model() { return g_dxc.engine ? g_dxc.engine_model : 0; }

u32 da_dxc_native_model() { return g_dxc.native_model; }

pcstr da_dxc_cache_dir() { return g_dxc.cache_dir; }

void da_dxc_log_stats()
{
    const u32 compiled = g_stats.compiled, cached = g_stats.cached, fxc = g_stats.fxc, twins = g_stats.twins,
              failed = g_stats.failed;
    const u32 native_compiled = g_stats.native_compiled, native_cached = g_stats.native_cached,
              native_failed = g_stats.native_failed;
    string64 refused{}, native_errors{};
    if (failed)
        xr_sprintf(refused, ", %u refused by DXC", failed);
    if (native_failed)
        xr_sprintf(native_errors, ", %u failed", native_failed);
    if (g_dxc.engine)
    {
        Msg("* [dxc] shaders: %u compiled (%.1f s), %u cached, %u on FXC, %u twins%s; native %u compiled (%.1f s), "
            "%u cached%s",
            compiled, float(g_stats.compile_us) / 1e6f, cached, fxc, twins, refused, native_compiled,
            float(g_stats.native_us) / 1e6f, native_cached, native_errors);
    }
    else if (native_compiled || native_cached || native_failed)
    {
        Msg("* [dxc] native shaders: %u compiled (%.1f s), %u cached%s", native_compiled,
            float(g_stats.native_us) / 1e6f, native_cached, native_errors);
    }
}

bool da_dxc_compile_native(const DaDxcNative& request, xr_vector<u8>& bytecode, xr_vector<u8>* reflection)
{
    bytecode.clear();
    if (reflection)
        reflection->clear();
    if (!request.path || !request.target)
        return false;
    if (!g_dxc.native_model)
    {
        Msg("! [dxc] native %s: no DXC this session", request.path);
        return false;
    }
    const u32 model = da_dxc_target_model(request.target);
    if (!model || model > g_dxc.native_model)
    {
        Msg("! [dxc] native %s: target %s is above the session's SM %u.%u", request.path, request.target,
            g_dxc.native_model >> 4, g_dxc.native_model & 0xF);
        ++g_stats.native_failed;
        return false;
    }

    const xr_string path = da_dxc_normalize(request.path);
    xr_string text;
    if (request.source)
        text = request.source;
    else if (IReader* file = da_dxc_open(path))
    {
        text.assign(static_cast<pcstr>(file->pointer()), file->length());
        FS.r_close(file);
    }
    else
    {
        Msg("! [dxc] native %s: file not found", path.c_str());
        ++g_stats.native_failed;
        return false;
    }

    u32 source_crc = 0;
    {
        xr_vector<xr_string> visited;
        da_dxc_native_crc(path, text.c_str(), text.size(), source_crc, visited);
    }
    const bool fp16 = (request.flags & DaDxcNative16BitTypes) && g_dxc.fp16;
    u32 key = crc32(request.target, u32(xr_strlen(request.target)));
    if (request.entry)
        key = crc32(request.entry, u32(xr_strlen(request.entry)), key);
    key = crc32(&fp16, sizeof(fp16), key);
    for (u32 i = 0; i < request.define_count; ++i)
    {
        const DaDxcDefine& define = request.defines[i];
        if (!define.name)
            continue;
        key = crc32(define.name, u32(xr_strlen(define.name)), key);
        pcstr value = define.value ? define.value : "1";
        key = crc32("=", 1, key);
        key = crc32(value, u32(xr_strlen(value)), key);
    }

    string_path cache_file;
    {
        xr_string relative = path;
        for (char& c : relative)
        {
            if (c == '/')
                c = '\\';
        }
        string_path file;
        xr_sprintf(file, "shaders_cache_grizzly" DELIMITER "%s" DELIMITER "%s" DELIMITER "native" DELIMITER "%s.%s.%s" DELIMITER "%08x",
            k_cache_root, g_dxc.tag, relative.c_str(), request.entry ? request.entry : "lib", request.target, key);
        FS.update_path(cache_file, "$app_data_root$", file);
    }

    xr_vector<u8> cached_reflection;
    if (da_dxc_cache_read(cache_file, source_crc, bytecode, cached_reflection))
    {
        ++g_stats.native_cached;
        if (reflection)
            *reflection = std::move(cached_reflection);
        return true;
    }

    string16 sm;
    xr_sprintf(sm, "%X", model);
    xr_vector<xrd3d12::ShaderDefine> defines;
    defines.reserve(request.define_count + 5);
    for (u32 i = 0; i < request.define_count; ++i)
    {
        if (request.defines[i].name)
            defines.push_back({request.defines[i].name, request.defines[i].value});
    }
    defines.push_back({"GRIZZLY_DXC", "1"});
#if defined(XR_VULKAN)
    defines.push_back({"GRIZZLY_VULKAN", "1"});
#endif
    defines.push_back({"GRIZZLY_SM", sm});
    if (g_dxc.wave_lanes)
        defines.push_back({"GRIZZLY_WAVE_LANES", g_dxc.wave});
    if (g_dxc.min16)
        defines.push_back({"GRIZZLY_MIN16", "1"});
    defines.push_back({"SMAP_ARRAY", "1"});

    xr_vector<const wchar_t*> args(std::begin(k_native_args), std::end(k_native_args));
    if (g_dxc.optimize)
        args.push_back(L"-O3");

    xrd3d12::ShaderCompile compile;
    compile.source = text.c_str();
    compile.size = text.size();
    compile.name = path.c_str();
    compile.entry = request.entry;
    compile.target = request.target;
    compile.defines = defines.data();
    compile.defineCount = UINT(defines.size());
    compile.flags = g_dxc.flags | (fp16 ? xrd3d12::Compile16BitTypes : 0u);
    compile.hlsl = 2021;
    compile.args = args.data();
    compile.argCount = UINT(args.size());
    compile.include = &da_dxc_native_include;

    const u64 start = da_dxc_now_us();
    xrd3d12::ShaderOutput output;
    const bool ok = xrd3d12::CompileShader(compile, output);
    g_stats.native_us += da_dxc_now_us() - start;
    if (!ok)
    {
        ++g_stats.native_failed;
        Msg("! [dxc] native %s %s: %s", path.c_str(), request.target, da_dxc_first_error(output.messages).c_str());
        return false;
    }
    if (!output.messages.empty() && strstr(Core.Params, "-log_shader_warnings"))
        Msg("! [dxc] native %s %s warnings:\n%s", path.c_str(), request.target, output.messages.c_str());
    ++g_stats.native_compiled;
    bytecode.assign(output.object.begin(), output.object.end());
    xr_vector<u8> blob(output.reflection.begin(), output.reflection.end());
    da_dxc_contain(blob);
#if defined(XR_VULKAN)
    {
        // Libraries (DXR state objects) have no Vulkan twin: the RT passes are compute shaders.
        xr_string error;
        if (!request.entry || !da_dxc_spirv(compile, bytecode, blob, bytecode, error))
        {
            ++g_stats.native_failed;
            Msg("! [dxc] native %s %s: %s", path.c_str(), request.target, request.entry ? error.c_str() : "libraries have no SPIR-V twin");
            bytecode.clear();
            return false;
        }
    }
#endif
    da_dxc_cache_write(cache_file, source_crc, bytecode, blob);
    if (reflection)
        *reflection = std::move(blob);
    return true;
}

bool da_dxc_compile_engine(pcstr name, const void* source, size_t size, const D3D_SHADER_MACRO* macros,
    ID3DInclude* include, pcstr entry, pcstr target, xr_vector<u8>& object, xr_vector<u8>& reflection,
    xr_string& error)
{
    object.clear();
    reflection.clear();
    error.clear();
    if (!g_dxc.engine || !target || xr_strlen(target) < 2)
    {
        error = "DXC is off";
        return false;
    }
    const bool mesh = (target[0] == 'm' || target[0] == 'a') && target[1] == 's';
    if (mesh && g_dxc.engine_model < 0x65)
    {
        ++g_stats.failed;
        error = "mesh and amplification shaders need shader model 6.5";
        return false;
    }
    const u64 start = da_dxc_now_us();

    // 1. FXC's preprocessor with the engine's includer and macros (dxc.md 3.3 step 1): a nested
    // include resolves from the shader root exactly as under FXC.
    xr_vector<D3D_SHADER_MACRO> defines;
    for (const D3D_SHADER_MACRO* macro = macros; macro && macro->Name; ++macro)
        defines.push_back(*macro);
    defines.push_back({"GRIZZLY_DXC", "1"});
#if defined(XR_VULKAN)
    // What has no Vulkan equivalent (pow2 tessellation partitioning) picks its nearest under it.
    defines.push_back({"GRIZZLY_VULKAN", "1"});
#endif
    defines.push_back({"GRIZZLY_SM", g_dxc.engine_sm});
    if (g_dxc.wave_lanes && !g_dxc.plain)
        defines.push_back({"GRIZZLY_WAVE_LANES", g_dxc.wave});
    if (g_dxc.min16 && !g_dxc.plain)
        defines.push_back({"GRIZZLY_MIN16", "1"});
    defines.push_back({nullptr, nullptr});
    ID3DBlob* text = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT hr = D3DPreprocess(source, size, name, defines.data(), include, &text, &messages);
    if (FAILED(hr) || !text)
    {
        error = messages ? da_dxc_first_error(std::string(static_cast<pcstr>(messages->GetBufferPointer()),
                                              messages->GetBufferSize())).c_str()
                         : "preprocessing failed";
        _RELEASE(text);
        _RELEASE(messages);
        ++g_stats.failed;
        return false;
    }
    _RELEASE(messages);
    pcstr preprocessed = static_cast<pcstr>(text->GetBufferPointer());
    size_t length = text->GetBufferSize();
    while (length && !preprocessed[length - 1])
        --length;

    // 2. Stage letters of the engine's target, the session's model (vs_5_0 -> vs_6_6).
    string16 dxc_target;
    xr_sprintf(dxc_target, "%c%c_6_%u", target[0], target[1], g_dxc.engine_model & 0xF);

    xr_vector<const wchar_t*> args(std::begin(k_engine_args), std::end(k_engine_args));
    if (g_dxc.optimize)
        args.push_back(L"-O3");
    if (g_dxc.all_bound)
        args.push_back(L"-all-resources-bound");

    xrd3d12::ShaderCompile compile;
    compile.source = preprocessed;
    compile.size = length;
    compile.name = name;
    compile.entry = entry;
    compile.target = dxc_target;
    compile.flags = g_dxc.flags;
    // New-only extensions (mesh, amplification) are written for 2021; the legacy base needs 2018.
    compile.hlsl = mesh ? 2021 : 2018;
    compile.args = args.data();
    compile.argCount = UINT(args.size());
    compile.preprocessed = true;

    xrd3d12::ShaderOutput output;
    const bool ok = xrd3d12::CompileShader(compile, output);
    if (!ok || output.reflection.empty())
    {
        _RELEASE(text);
        g_stats.compile_us += da_dxc_now_us() - start;
        ++g_stats.failed;
        error = ok ? "no reflection" : da_dxc_first_error(output.messages);
        return false;
    }
    if (!output.messages.empty() && strstr(Core.Params, "-log_shader_warnings"))
        Msg("! [dxc] %s warnings:\n%s", name, output.messages.c_str());
    object.assign(output.object.begin(), output.object.end());
    reflection.assign(output.reflection.begin(), output.reflection.end());
    da_dxc_contain(reflection);
#if defined(XR_VULKAN)
    // The same preprocessed text once more, to SPIR-V; the container carries both.
    const bool twin = da_dxc_spirv(compile, object, reflection, object, error);
    _RELEASE(text);
    g_stats.compile_us += da_dxc_now_us() - start;
    if (!twin)
    {
        ++g_stats.failed;
        object.clear();
        reflection.clear();
        return false;
    }
#else
    _RELEASE(text);
    g_stats.compile_us += da_dxc_now_us() - start;
#endif
    ++g_stats.compiled;
    return true;
}

bool da_dxc_cache_load(pcstr file, u32 source_crc, xr_vector<u8>& object, xr_vector<u8>& reflection)
{
    if (!da_dxc_cache_read(file, source_crc, object, reflection))
        return false;
    ++g_stats.cached;
    return true;
}

void da_dxc_cache_store(pcstr file, u32 source_crc, const xr_vector<u8>& object, const xr_vector<u8>& reflection)
{
    da_dxc_cache_write(file, source_crc, object, reflection);
}

void da_dxc_note_fxc() { ++g_stats.fxc; }
void da_dxc_note_twins(u32 stages) { g_stats.twins += stages; }

bool da_dxc_refused(pcstr file, u32 source_crc)
{
    const u64 key = da_dxc_refusal_key(file, source_crc);
    std::lock_guard lock(g_refusals_lock);
    return std::find(g_refusals.begin(), g_refusals.end(), key) != g_refusals.end();
}

void da_dxc_note_refusal(pcstr file, u32 source_crc)
{
    const u64 key = da_dxc_refusal_key(file, source_crc);
    std::lock_guard lock(g_refusals_lock);
    if (std::find(g_refusals.begin(), g_refusals.end(), key) == g_refusals.end())
        g_refusals.push_back(key);
}

#else // R4: every engine shader is FXC's

void da_dxc_init(ID3D11Device* /*device*/) {}
bool da_dxc_active() { return false; }
u32 da_dxc_shader_model() { return 0; }
u32 da_dxc_native_model() { return 0; }
pcstr da_dxc_cache_dir() { return ""; }
void da_dxc_log_stats() {}

bool da_dxc_compile_native(const DaDxcNative& /*request*/, xr_vector<u8>& bytecode, xr_vector<u8>* reflection)
{
    bytecode.clear();
    if (reflection)
        reflection->clear();
    return false;
}

bool da_dxc_compile_engine(pcstr /*name*/, const void* /*source*/, size_t /*size*/, const D3D_SHADER_MACRO* /*macros*/,
    ID3DInclude* /*include*/, pcstr /*entry*/, pcstr /*target*/, xr_vector<u8>& object, xr_vector<u8>& reflection,
    xr_string& error)
{
    object.clear();
    reflection.clear();
    error = "DXC is off";
    return false;
}

bool da_dxc_cache_load(pcstr /*file*/, u32 /*source_crc*/, xr_vector<u8>& /*object*/, xr_vector<u8>& /*reflection*/)
{
    return false;
}
void da_dxc_cache_store(pcstr /*file*/, u32 /*source_crc*/, const xr_vector<u8>& /*object*/, const xr_vector<u8>& /*reflection*/) {}
void da_dxc_note_fxc() {}
void da_dxc_note_twins(u32 /*stages*/) {}
bool da_dxc_refused(pcstr /*file*/, u32 /*source_crc*/) { return false; }
void da_dxc_note_refusal(pcstr /*file*/, u32 /*source_crc*/) {}
#endif

bool da_dxc_compile_native(pcstr path, pcstr entry, pcstr target, const DaDxcDefine* defines, u32 define_count,
    xr_vector<u8>& bytecode)
{
    DaDxcNative request;
    request.path = path;
    request.entry = entry;
    request.target = target;
    request.defines = defines;
    request.define_count = define_count;
    return da_dxc_compile_native(request, bytecode);
}
} // namespace xray::render::RENDER_NAMESPACE
