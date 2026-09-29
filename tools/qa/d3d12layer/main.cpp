// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Entry point of the xrD3D12 differential tests: creates a Direct3D 11 device and a layer device on
// adapter 0, runs every scenario on both, compares, and runs the draw throughput benchmark.
//
//   d3d12layer_tests.exe [-gbv] [-dxil] [-nocopyqueue] [-filter <text>] [-nobench | -benchonly] [-benchframes <n>] [-benchdraws <n>]
#include "harness.h"

#include <d3d11sdklayers.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace layertest
{
namespace
{
std::atomic<int> g_layerErrors{0};
std::atomic<int> g_layerWarnings{0};
std::mutex g_printLock;
std::vector<std::string> g_layerLines; // guarded by g_printLock
xrd3d12::Device* g_layerDevice = nullptr;
IDXGIAdapter1* g_adapter = nullptr;

void LayerLog(const char* line)
{
    if (std::strstr(line, "debug layer ERROR") || std::strstr(line, "debug layer CORRUPTION"))
        ++g_layerErrors;
    else if (std::strstr(line, "debug layer WARNING"))
        ++g_layerWarnings;
    std::lock_guard lock(g_printLock);
    std::printf("      %s\n", line);
    g_layerLines.emplace_back(line);
}

// Messages of the reference device's debug layer; errors there mean the scenario itself is wrong.
int DrainReference(ID3D11InfoQueue* queue)
{
    if (!queue)
        return 0;
    int errors = 0;
    const UINT64 count = queue->GetNumStoredMessages();
    std::vector<u64> storage;
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &length)))
            continue;
        storage.resize(length / sizeof(u64) + 1);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &length)))
            continue;
        const char* severity = "?";
        switch (message->Severity)
        {
        case D3D11_MESSAGE_SEVERITY_CORRUPTION: severity = "CORRUPTION"; ++errors; break;
        case D3D11_MESSAGE_SEVERITY_ERROR: severity = "ERROR"; ++errors; break;
        case D3D11_MESSAGE_SEVERITY_WARNING: severity = "WARNING"; break;
        default: continue;
        }
        std::printf("      [d3d11 reference] %s #%d: %.*s\n", severity, int(message->ID), int(message->DescriptionByteLength),
            message->pDescription);
    }
    queue->ClearStoredMessages();
    return errors;
}

float HalfToFloat(u16 h)
{
    const u32 sign = u32(h & 0x8000) << 16;
    const u32 exponent = (h >> 10) & 0x1F;
    const u32 mantissa = h & 0x3FF;
    u32 bits = 0;
    if (!exponent)
    {
        if (mantissa)
        {
            float value = std::ldexp(float(mantissa), -24);
            return sign ? -value : value;
        }
        bits = sign;
    }
    else if (exponent == 31)
        bits = sign | 0x7F800000 | (mantissa << 13);
    else
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Components of a texel as numbers comparable with the image tolerance.
u32 Components(DXGI_FORMAT format, const u8* texel, double* out)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        for (u32 i = 0; i < 4; ++i)
            out[i] = texel[i];
        return 4;
    case DXGI_FORMAT_R8G8_UNORM:
        out[0] = texel[0];
        out[1] = texel[1];
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
        out[0] = texel[0];
        return 1;
    case DXGI_FORMAT_R16_UINT:
    {
        u16 v;
        std::memcpy(&v, texel, 2);
        out[0] = v;
        return 1;
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    {
        u32 v;
        std::memcpy(&v, texel, 4);
        out[0] = v & 1023;
        out[1] = (v >> 10) & 1023;
        out[2] = (v >> 20) & 1023;
        out[3] = v >> 30;
        return 4;
    }
    case DXGI_FORMAT_R32_UINT:
    {
        u32 v;
        std::memcpy(&v, texel, 4);
        out[0] = v;
        return 1;
    }
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    {
        const u32 n = format == DXGI_FORMAT_R32_FLOAT ? 1 : format == DXGI_FORMAT_R32G32_FLOAT ? 2 : 4;
        for (u32 i = 0; i < n; ++i)
        {
            float v;
            std::memcpy(&v, texel + 4 * i, 4);
            out[i] = v;
        }
        return n;
    }
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    {
        const u32 n = format == DXGI_FORMAT_R16_FLOAT ? 1 : format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4;
        for (u32 i = 0; i < n; ++i)
        {
            u16 v;
            std::memcpy(&v, texel + 2 * i, 2);
            out[i] = HalfToFloat(v);
        }
        return n;
    }
    default: return 0;
    }
}

bool CompareImages(const Image& a, const Image& b, std::string& why)
{
    char text[512];
    if (a.width != b.width || a.height != b.height || a.depth != b.depth || a.format != b.format ||
        a.texels.size() != b.texels.size() || !a.texelSize)
    {
        std::snprintf(text, sizeof(text), "%s: shape differs (%ux%ux%u fmt %u vs %ux%ux%u fmt %u)", a.label.c_str(), a.width,
            a.height, a.depth, unsigned(a.format), b.width, b.height, b.depth, unsigned(b.format));
        why = text;
        return false;
    }
    u32 mismatches = 0;
    std::string first;
    const size_t count = a.texels.size() / a.texelSize;
    // -dxil: DXC contracts a * b + c into mad where FXC rounds twice; float results may differ in
    // the last bits (a few ulp, relative to the value).
    const bool floats = a.format == DXGI_FORMAT_R32_FLOAT || a.format == DXGI_FORMAT_R32G32_FLOAT ||
        a.format == DXGI_FORMAT_R32G32B32A32_FLOAT || a.format == DXGI_FORMAT_R16_FLOAT ||
        a.format == DXGI_FORMAT_R16G16_FLOAT || a.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const double relative = g_dxil && floats ? 1e-5 : 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        double va[4]{}, vb[4]{};
        const u32 n = Components(a.format, a.texels.data() + i * a.texelSize, va);
        Components(b.format, b.texels.data() + i * b.texelSize, vb);
        bool same = true;
        for (u32 c = 0; c < n; ++c)
        {
            const bool nan = std::isnan(va[c]) || std::isnan(vb[c]);
            const double tolerance = (std::max)(a.tolerance, relative * (std::max)(1.0, std::fabs(va[c])));
            if (nan ? std::isnan(va[c]) != std::isnan(vb[c]) : std::fabs(va[c] - vb[c]) > tolerance)
                same = false;
        }
        if (same)
            continue;
        if (!mismatches)
        {
            const u32 x = u32(i % a.width), y = u32((i / a.width) % a.height), z = u32(i / (size_t(a.width) * a.height));
            std::snprintf(text, sizeof(text), "(%u,%u,%u) d3d11 [%g %g %g %g] layer [%g %g %g %g]", x, y, z, va[0], va[1], va[2],
                va[3], vb[0], vb[1], vb[2], vb[3]);
            first = text;
        }
        ++mismatches;
    }
    if (!mismatches)
        return true;
    std::snprintf(text, sizeof(text), "%s: %u of %zu texels differ, first %s", a.label.c_str(), mismatches, count, first.c_str());
    why = text;
    return false;
}

// Distinct texel values of an image: shows that a comparison had something to compare.
size_t DistinctTexels(const Image& image)
{
    if (!image.texelSize)
        return 0;
    std::vector<std::string> values;
    const size_t count = image.texels.size() / image.texelSize;
    values.reserve(count);
    for (size_t i = 0; i < count; ++i)
        values.emplace_back(reinterpret_cast<const char*>(image.texels.data() + i * image.texelSize), image.texelSize);
    std::sort(values.begin(), values.end());
    return size_t(std::unique(values.begin(), values.end()) - values.begin());
}

struct Options
{
    bool gbv = false;
    bool bench = true;
    bool benchOnly = false;
    const char* filter = nullptr;
    bool highPerformance = false; // the discrete GPU on hybrid laptops instead of adapter 0
    bool copyQueue = true;        // creation-time uploads through the layer's copy queue (-nocopyqueue: -dx12_no_copy_queue)
    u32 benchFrames = 12;
    u32 benchDraws = 20000;
};

Options ParseOptions(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "-gbv"))
            o.gbv = true;
        else if (!std::strcmp(argv[i], "-nobench"))
            o.bench = false;
        else if (!std::strcmp(argv[i], "-benchonly"))
            o.benchOnly = true;
        else if (!std::strcmp(argv[i], "-profile"))
            g_profileBenchmark = true;
        else if (!std::strcmp(argv[i], "-highperf"))
            o.highPerformance = true;
        else if (!std::strcmp(argv[i], "-dxil"))
            g_dxil = true;
        else if (!std::strcmp(argv[i], "-nocopyqueue"))
            o.copyQueue = false;
        else if (!std::strcmp(argv[i], "-filter") && i + 1 < argc)
            o.filter = argv[++i];
        else if (!std::strcmp(argv[i], "-benchframes") && i + 1 < argc)
            o.benchFrames = u32(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "-benchdraws") && i + 1 < argc)
            o.benchDraws = u32(std::atoi(argv[++i]));
    }
    return o;
}
} // namespace

bool g_profileBenchmark = false;

xrd3d12::Device* LayerDevice()
{
    return g_layerDevice;
}

int LayerLogCount(const char* text)
{
    std::lock_guard lock(g_printLock);
    int count = 0;
    for (const std::string& line : g_layerLines)
        count += line.find(text) != std::string::npos;
    return count;
}

IDXGIAdapter1* Adapter()
{
    return g_adapter;
}

void EndFrame(Gpu& g)
{
    if (g.layer)
        xrd3d12::Device::From(g.dev)->ForceFrameBoundary();
    else
        g.ctx->Flush();
}

void WaitIdle(Gpu& g)
{
    if (g.layer)
    {
        xrd3d12::Flush(g.ctx, true);
        return;
    }
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(g.dev->CreateQuery(&desc, &query)))
        return;
    g.ctx->End(query.Get());
    BOOL done = FALSE;
    while (g.ctx->GetData(query.Get(), &done, sizeof(done), 0) == S_FALSE)
        SwitchToThread();
}
} // namespace layertest

using namespace layertest;

namespace
{
const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

std::wstring PipelineCachePath()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring path = exePath;
    return path.substr(0, path.find_last_of(L"\\/") + 1) + L"pipelines.bin";
}

// The D3D12 debug layer, once enabled, is on for every later device of the process: the benchmark
// runs in a process of its own that never enables it (-benchonly).
int RunBenchmark(const Options& options)
{
    const std::wstring cachePath = PipelineCachePath();
    ComPtr<ID3D11Device> plainDevice;
    ComPtr<ID3D11DeviceContext> plainContext;
    ID3D11Device* plainLayer = nullptr;
    ID3D11DeviceContext* plainLayerContext = nullptr;
    xrd3d12::DeviceCreateInfo info;
    info.adapter = g_adapter;
    info.pipelineCachePath = cachePath.c_str();
    info.copyQueueUploads = options.copyQueue;
    info.log = LayerLog;
    if (FAILED(D3D11CreateDevice(g_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels, UINT(std::size(kLevels)),
            D3D11_SDK_VERSION, &plainDevice, nullptr, &plainContext)) ||
        FAILED(xrd3d12::CreateDevice(info, &plainLayer, &plainLayerContext, nullptr)))
    {
        std::printf("benchmark skipped: the devices could not be created\n");
        return 2;
    }
    std::printf("---- benchmark: %u draws per frame, each with a DISCARD constant buffer update and 2 texture binds "
                "(no debug layers)\n",
        options.benchDraws);
    Gpu ref{"d3d11", false, plainDevice.Get(), plainContext.Get()};
    Gpu layer{"d3d12 layer", true, plainLayer, plainLayerContext};
    g_layerDevice = xrd3d12::Device::From(plainLayer);
    const double refMs = BenchmarkFrameMs(ref, options.benchFrames, options.benchDraws);
    const double layerMs = BenchmarkFrameMs(layer, options.benchFrames, options.benchDraws);
    std::printf("BENCH d3d11 %.3f ms/frame (%.2f us/draw), layer %.3f ms/frame (%.2f us/draw): %.2fx\n", refMs,
        refMs * 1000.0 / options.benchDraws, layerMs, layerMs * 1000.0 / options.benchDraws, layerMs / refMs);
    g_layerDevice = nullptr;
    plainLayerContext->Release();
    plainLayer->Release();
    return 0;
}

void RunBenchmarkProcess(const Options& options)
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exePath) + L"\" -benchonly -benchframes " + std::to_wstring(options.benchFrames) +
        L" -benchdraws " + std::to_wstring(options.benchDraws) + (g_profileBenchmark ? L" -profile" : L"") +
        (options.highPerformance ? L" -highperf" : L"") + (options.copyQueue ? L"" : L" -nocopyqueue");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
    {
        std::printf("benchmark skipped: its process could not start (%lu)\n", GetLastError());
        return;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
} // namespace

int main(int argc, char** argv)
{
    const Options options = ParseOptions(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && options.highPerformance && SUCCEEDED(factory.As(&factory6)))
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&g_adapter));
    if (!factory || (!g_adapter && FAILED(factory->EnumAdapters1(0, &g_adapter))))
    {
        std::printf("FATAL no DXGI adapter\n");
        return 2;
    }
    DXGI_ADAPTER_DESC1 adapterDesc{};
    g_adapter->GetDesc1(&adapterDesc);
    std::printf("adapter: %ls\n", adapterDesc.Description);
    if (options.benchOnly)
    {
        const int code = RunBenchmark(options);
        g_adapter->Release();
        return code;
    }

    // -dxil: DXC next to the executable (Build-LayerTests.ps1 copies it there).
    if (g_dxil)
    {
        char reason[256]{};
        if (!xrd3d12::LoadShaderCompiler(nullptr, reason, sizeof(reason)))
        {
            std::printf("FATAL -dxil: the shader compiler does not load: %s\n", reason);
            return 2;
        }
        std::printf("layer shaders: DXIL (dxcompiler %s)\n", xrd3d12::DxcVersion());
    }
    if (!options.copyQueue)
        std::printf("layer creation-time uploads: direct queue (-nocopyqueue)\n");

    // Reference: real Direct3D 11 with its debug layer when installed.
    ComPtr<ID3D11Device> refDevice;
    ComPtr<ID3D11DeviceContext> refContext;
    HRESULT hr = D3D11CreateDevice(g_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG, kLevels,
        UINT(std::size(kLevels)), D3D11_SDK_VERSION, &refDevice, nullptr, &refContext);
    if (FAILED(hr))
        hr = D3D11CreateDevice(g_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels, UINT(std::size(kLevels)), D3D11_SDK_VERSION,
            &refDevice, nullptr, &refContext);
    if (FAILED(hr))
    {
        std::printf("FATAL D3D11CreateDevice failed (0x%08X)\n", unsigned(hr));
        return 2;
    }
    ComPtr<ID3D11InfoQueue> refQueue;
    if (SUCCEEDED(refDevice.As(&refQueue)))
    {
        D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
        D3D11_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = UINT(std::size(deny));
        filter.DenyList.pSeverityList = deny;
        refQueue->PushStorageFilter(&filter);
        std::printf("reference: Direct3D 11 with the debug layer\n");
    }
    else
        std::printf("reference: Direct3D 11 (no debug layer installed)\n");

    // The layer, with the D3D12 debug layer (and GPU-based validation on request).
    const std::wstring cachePath = PipelineCachePath();
    xrd3d12::DeviceCreateInfo info;
    info.adapter = g_adapter;
    info.debugLayer = true;
    info.gpuValidation = options.gbv;
    info.dred = true;
    info.pipelineCachePath = cachePath.c_str();
    info.copyQueueUploads = options.copyQueue;
    info.log = LayerLog;
    ID3D11Device* layerDevice = nullptr;
    ID3D11DeviceContext* layerContext = nullptr;
    hr = xrd3d12::CreateDevice(info, &layerDevice, &layerContext, nullptr);
    if (FAILED(hr))
    {
        std::printf("FATAL xrd3d12::CreateDevice failed (0x%08X)\n", unsigned(hr));
        return 2;
    }
    g_layerDevice = xrd3d12::Device::From(layerDevice);

    Gpu ref{"d3d11", false, refDevice.Get(), refContext.Get()};
    Gpu layer{"d3d12 layer", true, layerDevice, layerContext};

    int passed = 0, failed = 0;
    for (const Scenario& s : Scenarios())
    {
        if (options.filter && !std::strstr(s.name, options.filter))
            continue;
        std::printf("---- %s\n", s.name);
        Result refResult, layerResult;
        const int errorsBefore = g_layerErrors.load();
        int refErrors = 0;
        if (!s.layerOnly)
        {
            s.fn(ref, refResult);
            WaitIdle(ref);
            refErrors = DrainReference(refQueue.Get());
        }
        s.fn(layer, layerResult);
        EndFrame(layer);
        WaitIdle(layer);
        g_layerDevice->DrainDebugMessages();
        const int layerErrors = g_layerErrors.load() - errorsBefore;

        std::vector<std::string> why;
        for (const std::string& f : refResult.failures)
            why.push_back("d3d11: " + f);
        for (const std::string& f : layerResult.failures)
            why.push_back("layer: " + f);
        if (refErrors && !s.referenceErrorsExpected)
            why.push_back("the reference D3D11 debug layer reported errors (the scenario is wrong)");
        if (layerErrors)
            why.push_back(std::to_string(layerErrors) + " D3D12 debug layer error(s)");
        if (!s.layerOnly)
        {
            if (refResult.images.size() != layerResult.images.size())
                why.push_back("different image counts");
            else
            {
                for (size_t i = 0; i < refResult.images.size(); ++i)
                {
                    std::string text;
                    if (!CompareImages(refResult.images[i], layerResult.images[i], text))
                        why.push_back(text);
                }
            }
            if (refResult.values.size() != layerResult.values.size())
                why.push_back("different value counts");
            else
            {
                for (size_t i = 0; i < refResult.values.size(); ++i)
                {
                    const auto& a = refResult.values[i];
                    const auto& b = layerResult.values[i];
                    if (a.first != b.first || a.second != b.second)
                    {
                        why.push_back(a.first + ": d3d11 " + std::to_string(a.second) + ", layer " + b.first + " " +
                            std::to_string(b.second));
                    }
                }
            }
        }
        if (why.empty())
        {
            ++passed;
            std::string distinct;
            for (const Image& image : layerResult.images)
                distinct += (distinct.empty() ? "" : " ") + std::to_string(DistinctTexels(image));
            std::printf("PASS %s (%zu images [distinct texels: %s], %zu values)\n", s.name, layerResult.images.size(),
                distinct.c_str(), layerResult.values.size());
        }
        else
        {
            ++failed;
            std::printf("FAIL %s\n", s.name);
            for (const std::string& w : why)
                std::printf("     %s\n", w.c_str());
        }
    }

    std::printf("==== %d passed, %d failed, %d debug layer warnings\n", passed, failed, g_layerWarnings.load());

    const int errorsBeforeShutdown = g_layerErrors.load();
    layerContext->Release();
    layerDevice->Release();
    g_layerDevice = nullptr;
    if (g_layerErrors.load() != errorsBeforeShutdown)
    {
        std::printf("FAIL shutdown: %d debug layer error(s)\n", g_layerErrors.load() - errorsBeforeShutdown);
        ++failed;
    }
    refContext.Reset();
    refDevice.Reset();

    if (options.bench && !options.filter)
        RunBenchmarkProcess(options);
    g_adapter->Release();
    return failed ? 1 : 0;
}
