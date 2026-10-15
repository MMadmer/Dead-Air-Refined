// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The DXC shader compiler service of xrD3D12 (docs/dead-air/dx12-features/dxc.md 3.1, 3.4): loads
// dxcompiler.dll + dxil.dll by absolute path, compiles, reflects and disassembles. The only code
// of the renderer that touches DXC.
#include "d12_layer.h"

#include <d3d12shader.h>
#include <dxcapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace xrd3d12
{
namespace
{
constexpr u32 FourCC(char a, char b, char c, char d)
{
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

// Whether a DXBC-format container (FXC and DXC write the same header) has a part.
bool HasPart(const void* blob, size_t size, u32 fourcc)
{
    const u8* bytes = static_cast<const u8*>(blob);
    if (!bytes || size < 32 || std::memcmp(bytes, "DXBC", 4) != 0)
        return false;
    u32 count;
    std::memcpy(&count, bytes + 28, 4);
    if (count > (size - 32) / 4)
        return false;
    for (u32 i = 0; i < count; ++i)
    {
        u32 offset, part;
        std::memcpy(&offset, bytes + 32 + 4 * i, 4);
        if (offset > size - 8)
            continue;
        std::memcpy(&part, bytes + offset, 4);
        if (part == fourcc)
            return true;
    }
    return false;
}

// A bare part outside a container: CompileShader's DXC_OUT_REFLECTION is the 'STAT' part ('RDAT' for
// libraries) - fourcc, size, then the part itself.
bool IsBareReflectionPart(const void* blob, size_t size)
{
    const u8* bytes = static_cast<const u8*>(blob);
    if (!bytes || size < 8)
        return false;
    u32 fourcc, length;
    std::memcpy(&fourcc, bytes, 4);
    std::memcpy(&length, bytes + 4, 4);
    return (fourcc == FourCC('S', 'T', 'A', 'T') || fourcc == FourCC('R', 'D', 'A', 'T')) && u64(length) + 8 == size;
}

// ---- the loaded compiler (decided once per process) ----

struct Compiler
{
    std::once_flag once;
    bool loaded = false;
    char reason[256]{};
    HMODULE dxil = nullptr;
    HMODULE dxcompiler = nullptr;
    DxcCreateInstanceProc create = nullptr;
    UINT32 major = 0, minor = 0;
    char version[32]{};
    UINT shaderModel = 0;
    // Pipeline check on a device: 0 not run, 1 passed, -1 failed.
    std::mutex checkLock;
    int checked = 0;
} g_compiler;

void SetReason(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(g_compiler.reason, sizeof(g_compiler.reason), fmt, args);
    va_end(args);
}

bool LoadModules(const wchar_t* dir)
{
    std::wstring folder;
    if (dir && *dir)
        folder = dir;
    else
    {
        wchar_t exe[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!length || length >= MAX_PATH)
        {
            SetReason("the executable's folder is unknown");
            return false;
        }
        folder.assign(exe, length);
        folder.resize(folder.find_last_of(L"\\/") + 1);
    }
    if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
        folder += L'\\';

    // Absolute paths only: a dxcompiler.dll found through the search path could be any version.
    // dxcompiler binds to the dxil.dll loaded first (the signer; unsigned DXIL is refused).
    constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    g_compiler.dxil = LoadLibraryExW((folder + L"dxil.dll").c_str(), nullptr, flags);
    if (!g_compiler.dxil)
    {
        SetReason("dxil.dll not loadable (error %lu)", GetLastError());
        return false;
    }
    g_compiler.dxcompiler = LoadLibraryExW((folder + L"dxcompiler.dll").c_str(), nullptr, flags);
    if (!g_compiler.dxcompiler)
    {
        SetReason("dxcompiler.dll not loadable (error %lu)", GetLastError());
        return false;
    }
    g_compiler.create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(g_compiler.dxcompiler, "DxcCreateInstance"));
    if (!g_compiler.create)
    {
        SetReason("dxcompiler.dll has no DxcCreateInstance");
        return false;
    }

    IDxcCompiler3* compiler = nullptr;
    if (FAILED(g_compiler.create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler)
    {
        SetReason("no DXC compiler instance");
        return false;
    }
    IDxcVersionInfo* info = nullptr;
    const bool versioned = SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(&info))) && info &&
        SUCCEEDED(info->GetVersion(&g_compiler.major, &g_compiler.minor));
    UINT32 commits = 0;
    if (info)
    {
        IDxcVersionInfo2* info2 = nullptr;
        char* hash = nullptr;
        if (SUCCEEDED(info->QueryInterface(IID_PPV_ARGS(&info2))) && info2)
        {
            if (SUCCEEDED(info2->GetCommitInfo(&commits, &hash)) && hash)
                CoTaskMemFree(hash);
            info2->Release();
        }
        info->Release();
    }
    compiler->Release();
    if (!versioned)
    {
        SetReason("the DXC version is unknown");
        return false;
    }
    if (g_compiler.major < 1 || (g_compiler.major == 1 && g_compiler.minor < 8))
    {
        SetReason("DXC %u.%u is too old (1.8 or newer is needed)", g_compiler.major, g_compiler.minor);
        return false;
    }
    // The file version of the release DLLs: major.minor.0.commits.
    if (commits)
        std::snprintf(g_compiler.version, sizeof(g_compiler.version), "%u.%u.0.%u", g_compiler.major, g_compiler.minor, commits);
    else
        std::snprintf(g_compiler.version, sizeof(g_compiler.version), "%u.%u", g_compiler.major, g_compiler.minor);
    // DXC 1.x targets shader model 6.x; capped by what the SDK headers name.
    g_compiler.shaderModel = g_compiler.major > 1 ? UINT(D3D_HIGHEST_SHADER_MODEL) :
                                                    (std::min)(UINT(0x60 + g_compiler.minor), UINT(D3D_HIGHEST_SHADER_MODEL));
    return true;
}

// ---- per-thread instances ----
// One compiler and one utils object per thread. They are never released: threads outlive nothing
// here but the process, and dxcompiler.dll stays loaded until the process ends.
struct ThreadInstances
{
    IDxcCompiler3* compiler = nullptr;
    IDxcUtils* utils = nullptr;
};
thread_local ThreadInstances t_dxc;

bool Instances(ThreadInstances*& out)
{
    out = &t_dxc;
    if (!g_compiler.loaded)
        return false;
    if (!t_dxc.compiler && FAILED(g_compiler.create(CLSID_DxcCompiler, IID_PPV_ARGS(&t_dxc.compiler))))
        t_dxc.compiler = nullptr;
    if (!t_dxc.utils && FAILED(g_compiler.create(CLSID_DxcUtils, IID_PPV_ARGS(&t_dxc.utils))))
        t_dxc.utils = nullptr;
    return t_dxc.compiler && t_dxc.utils;
}

// DXC reads UTF-8. Text that is not (cp1251 comments in shader files) gets every byte >= 0x80
// replaced, independent of the machine's code page; identifiers are ASCII.
bool IsUtf8(const char* text, size_t size)
{
    const u8* p = reinterpret_cast<const u8*>(text);
    for (size_t i = 0; i < size;)
    {
        const u8 c = p[i];
        u32 extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 4;
        if (extra == 4 || i + extra >= size)
            return false;
        for (u32 k = 1; k <= extra; ++k)
        {
            if ((p[i + k] >> 6) != 0x2)
                return false;
        }
        i += extra + 1;
    }
    return true;
}

void MakeUtf8(std::string& text)
{
    if (IsUtf8(text.data(), text.size()))
        return;
    for (char& c : text)
    {
        if (u8(c) >= 0x80)
            c = '?';
    }
}

std::string Narrow(const wchar_t* text)
{
    std::string out;
    if (!text)
        return out;
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length > 1)
    {
        out.resize(size_t(length - 1));
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    }
    return out;
}

std::wstring Widen(const char* text)
{
    std::wstring out;
    if (!text)
        return out;
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length > 1)
    {
        out.resize(size_t(length - 1));
        MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
    }
    return out;
}

// DXC offers include paths like ".\dir\.\file.h": forward slashes, no "./" parts.
std::string NormalizePath(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\')
            c = '/';
    }
    for (size_t at; (at = path.find("/./")) != std::string::npos;)
        path.erase(at, 2);
    while (path.size() > 2 && path[0] == '.' && path[1] == '/')
        path.erase(0, 2);
    return path;
}

// #include through the caller's callback. Lives on the compiling thread's stack for one compile.
class IncludeHandler final : public IDxcIncludeHandler
{
public:
    IncludeHandler(const ShaderCompile& request, IDxcUtils* utils) : m_request(request), m_utils(utils) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** includeSource) override
    {
        if (!includeSource)
            return E_POINTER;
        *includeSource = nullptr;
        try
        {
            const std::string path = NormalizePath(Narrow(fileName));
            std::string text;
            if (path.empty() || !m_request.include(m_request.user, path.c_str(), text))
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            MakeUtf8(text);
            IDxcBlobEncoding* blob = nullptr;
            const HRESULT hr = m_utils->CreateBlob(text.data(), UINT32(text.size()), DXC_CP_UTF8, &blob);
            if (FAILED(hr))
                return hr;
            *includeSource = blob;
            return S_OK;
        }
        catch (...)
        {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler))
        {
            *object = static_cast<IDxcIncludeHandler*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // Stack object: DXC's references do not own it.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

private:
    const ShaderCompile& m_request;
    IDxcUtils* m_utils;
};

// No C++ objects with destructors here: an access violation inside DXC becomes a failed compile.
HRESULT CompileGuarded(IDxcCompiler3* compiler, const DxcBuffer* source, LPCWSTR* args, UINT32 count,
    IDxcIncludeHandler* include, IDxcResult** result, DWORD* exception)
{
    __try
    {
        return compiler->Compile(source, args, count, include, __uuidof(IDxcResult), reinterpret_cast<void**>(result));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *exception = GetExceptionCode();
        return E_FAIL;
    }
}

template <class T>
void ReleaseCom(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

void CopyBlob(IDxcBlob* blob, std::vector<unsigned char>& out)
{
    out.clear();
    if (blob && blob->GetBufferSize())
    {
        const auto* bytes = static_cast<const unsigned char*>(blob->GetBufferPointer());
        out.assign(bytes, bytes + blob->GetBufferSize());
    }
}

// dxil.dll writes the container's digest when it validated and signed the module.
bool IsSigned(const std::vector<unsigned char>& object)
{
    if (object.size() < 20)
        return false;
    for (size_t i = 4; i < 20; ++i)
    {
        if (object[i])
            return true;
    }
    return false;
}

// A one-line VS/PS pair built into a pipeline on the device: the runtime accepts DXC's DXIL here.
bool CheckPipeline(ID3D12Device* device, char* why, size_t whySize)
{
    static const char vsText[] = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(id & 1, id >> 1, 0, 1); }";
    static const char psText[] = "float4 main() : SV_Target { return float4(1, 0.5, 0.25, 1); }";
    ShaderOutput vs, ps;
    ShaderCompile request;
    request.entry = "main";
    request.preprocessed = true;
    request.source = vsText;
    request.size = sizeof(vsText) - 1;
    request.name = "selfcheck.vs";
    request.target = "vs_6_0";
    if (!CompileShader(request, vs))
    {
        std::snprintf(why, whySize, "the check shader does not compile: %.160s", vs.messages.c_str());
        return false;
    }
    request.source = psText;
    request.size = sizeof(psText) - 1;
    request.name = "selfcheck.ps";
    request.target = "ps_6_0";
    if (!CompileShader(request, ps))
    {
        std::snprintf(why, whySize, "the check shader does not compile: %.160s", ps.messages.c_str());
        return false;
    }
    if (!vs.isSigned || !ps.isSigned)
    {
        std::snprintf(why, whySize, "%s", "dxil.dll did not sign the check shaders");
        return false;
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    ID3DBlob* serialized = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = g_d3d12.SerializeRootSignature ?
        g_d3d12.SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors) : E_NOTIMPL;
    ReleaseCom(errors);
    ID3D12RootSignature* root = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
    ReleaseCom(serialized);
    if (FAILED(hr))
    {
        std::snprintf(why, whySize, "no root signature for the check (0x%08X)", unsigned(hr));
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root;
    d.VS = {vs.object.data(), vs.object.size()};
    d.PS = {ps.object.data(), ps.object.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xffffffff;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    ReleaseCom(pso);
    ReleaseCom(root);
    if (FAILED(hr))
    {
        std::snprintf(why, whySize, "the runtime refused a DXIL pipeline (0x%08X)", unsigned(hr));
        return false;
    }
    return true;
}
} // namespace

// ============================================================================================
// Public API (d12_api.h)
// ============================================================================================

bool LoadShaderCompiler(const wchar_t* dir, char* reason, size_t reasonSize)
{
    std::call_once(g_compiler.once, [dir] {
        try
        {
            g_compiler.loaded = LoadModules(dir);
        }
        catch (...)
        {
            SetReason("out of memory");
            g_compiler.loaded = false;
        }
    });
    if (reason && reasonSize)
        std::snprintf(reason, reasonSize, "%s", g_compiler.loaded ? "" : g_compiler.reason);
    return g_compiler.loaded;
}

const char* DxcVersion()
{
    return g_compiler.loaded ? g_compiler.version : "";
}

UINT DxcShaderModel()
{
    return g_compiler.loaded ? g_compiler.shaderModel : 0;
}

UINT InitShaderCompiler(ID3D12Device* device)
{
    char reason[256]{};
    if (!LoadShaderCompiler(nullptr, reason, sizeof(reason)))
    {
        LogOnce(&g_compiler, "* shader compiler: not available (%s)", reason);
        return 0;
    }
    std::lock_guard lock(g_compiler.checkLock);
    if (!g_compiler.checked && device)
    {
        char why[256]{};
        g_compiler.checked = CheckPipeline(device, why, sizeof(why)) ? 1 : -1;
        if (g_compiler.checked > 0)
            Log("* shader compiler: dxcompiler %s, shader model %X.%X", g_compiler.version, g_compiler.shaderModel >> 4,
                g_compiler.shaderModel & 0xF);
        else
            Log("! shader compiler: dxcompiler %s is off for this session: %s", g_compiler.version, why);
    }
    return g_compiler.checked > 0 ? g_compiler.shaderModel : 0;
}

bool CompileShader(const ShaderCompile& request, ShaderOutput& output)
{
    output = ShaderOutput{};
    ThreadInstances* dxc = nullptr;
    if (!Instances(dxc))
    {
        output.messages = g_compiler.loaded ? "no DXC instance on this thread" : "the shader compiler is not loaded";
        return false;
    }
    if (!request.source || !request.target)
    {
        output.messages = "no source or target";
        return false;
    }
    try
    {
        std::string source(request.source, request.size);
        MakeUtf8(source);

        std::vector<std::wstring> text;
        text.reserve(24 + 2 * request.defineCount + request.argCount);
        text.push_back(Widen(request.name ? request.name : "shader.hlsl"));
        if (request.entry)
        {
            text.push_back(L"-E");
            text.push_back(Widen(request.entry));
        }
        text.push_back(L"-T");
        text.push_back(Widen(request.target));
        text.push_back(L"-HV");
        text.push_back(request.hlsl == 2018 ? L"2018" : L"2021");
        for (UINT i = 0; i < request.defineCount; ++i)
        {
            const ShaderDefine& define = request.defines[i];
            if (!define.name)
                continue;
            text.push_back(L"-D");
            text.push_back(Widen(define.name) + L"=" + (define.value ? Widen(define.value) : std::wstring(L"1")));
        }
        if (request.flags & CompileDebugInfo)
        {
            text.push_back(L"-Zi");
            text.push_back(L"-Qembed_debug");
        }
        else
            text.push_back(L"-Qstrip_debug");
        if (!(request.flags & CompileKeepReflection))
            text.push_back(L"-Qstrip_reflect");
        if (request.flags & Compile16BitTypes)
            text.push_back(L"-enable-16bit-types");
        if (request.flags & CompileWarningsAsErrors)
            text.push_back(L"-WX");
        if (request.flags & CompileSkipOptimization)
            text.push_back(L"-Od");
        for (UINT i = 0; i < request.argCount; ++i)
        {
            if (request.args[i])
                text.emplace_back(request.args[i]);
        }
        std::vector<LPCWSTR> args;
        args.reserve(text.size());
        for (const std::wstring& arg : text)
            args.push_back(arg.c_str());

        DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
        IncludeHandler handler(request, dxc->utils);
        IDxcIncludeHandler* include = request.include && !request.preprocessed ? &handler : nullptr;
        IDxcResult* result = nullptr;
        DWORD exception = 0;
        HRESULT hr = CompileGuarded(dxc->compiler, &buffer, args.data(), UINT32(args.size()), include, &result, &exception);
        if (exception)
        {
            // The instance may be broken after a crash inside it.
            t_dxc.compiler = nullptr;
            char message[96];
            std::snprintf(message, sizeof(message), "DXC crashed (exception 0x%08lX)", exception);
            output.messages = message;
            ReleaseCom(result);
            return false;
        }
        if (FAILED(hr) || !result)
        {
            char message[64];
            std::snprintf(message, sizeof(message), "DXC failed (0x%08X)", unsigned(hr));
            output.messages = message;
            ReleaseCom(result);
            return false;
        }

        IDxcBlobUtf8* errors = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors && errors->GetStringLength())
            output.messages.assign(errors->GetStringPointer(), errors->GetStringLength());
        ReleaseCom(errors);
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        if (SUCCEEDED(status))
        {
            IDxcBlob* object = nullptr;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)))
                CopyBlob(object, output.object);
            ReleaseCom(object);
            IDxcBlob* reflection = nullptr;
            if (result->HasOutput(DXC_OUT_REFLECTION) &&
                SUCCEEDED(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflection), nullptr)))
                CopyBlob(reflection, output.reflection);
            ReleaseCom(reflection);
        }
        ReleaseCom(result);
        output.isSigned = IsSigned(output.object);
        return SUCCEEDED(status) && !output.object.empty();
    }
    catch (const std::bad_alloc&)
    {
        output.messages = "out of memory";
        return false;
    }
}

HRESULT Reflect(const void* bytecode, size_t size, REFIID riid, void** reflection)
{
    if (!reflection)
        return E_POINTER;
    *reflection = nullptr;
    if (!bytecode || !size)
        return E_INVALIDARG;
    // DXIL objects and DXC reflection blobs (a STAT part with the DXIL module, no DXBC program; in a
    // container or bare) go through DXC; DXBC (FXC writes a STAT part too) through D3DReflect, which
    // answers the D3D12 interface for it as well.
    const bool dxcBlob = IsDxil(bytecode, size) || IsBareReflectionPart(bytecode, size) ||
        (HasPart(bytecode, size, FourCC('S', 'T', 'A', 'T')) && !HasPart(bytecode, size, FourCC('S', 'H', 'E', 'X')) &&
            !HasPart(bytecode, size, FourCC('S', 'H', 'D', 'R')) && !HasPart(bytecode, size, FourCC('R', 'D', 'E', 'F')));
    if (!dxcBlob)
        return D3DReflect(bytecode, size, riid, reflection);
    ThreadInstances* dxc = nullptr;
    if (!Instances(dxc))
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    const DxcBuffer buffer{bytecode, size, 0};
    return dxc->utils->CreateReflection(&buffer, riid, reflection);
}

HRESULT Disassemble(const void* bytecode, size_t size, std::string& text)
{
    text.clear();
    if (!bytecode || !size)
        return E_INVALIDARG;
    try
    {
        if (!IsDxil(bytecode, size))
        {
            ID3DBlob* blob = nullptr;
            const HRESULT hr = D3DDisassemble(bytecode, size, 0, nullptr, &blob);
            if (SUCCEEDED(hr) && blob)
                text.assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
            ReleaseCom(blob);
            return hr;
        }
        ThreadInstances* dxc = nullptr;
        if (!Instances(dxc))
            return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        const DxcBuffer buffer{bytecode, size, 0};
        IDxcResult* result = nullptr;
        HRESULT hr = dxc->compiler->Disassemble(&buffer, IID_PPV_ARGS(&result));
        if (SUCCEEDED(hr) && result)
        {
            IDxcBlobUtf8* disassembly = nullptr;
            hr = result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&disassembly), nullptr);
            if (SUCCEEDED(hr) && disassembly)
                text.assign(disassembly->GetStringPointer(), disassembly->GetStringLength());
            ReleaseCom(disassembly);
        }
        ReleaseCom(result);
        return hr;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

bool IsDxil(const void* bytecode, size_t size)
{
    return HasPart(bytecode, size, FourCC('D', 'X', 'I', 'L'));
}
} // namespace xrd3d12
