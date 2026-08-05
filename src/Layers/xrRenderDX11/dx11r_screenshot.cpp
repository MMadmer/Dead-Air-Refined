#include "stdafx.h"

//#include "xr_effgamma.h"

#include "xrCore/Media/Image.hpp"
#include "xrEngine/xrImage_Resampler.h"

#include <DirectXTex.h>
#include <wincodec.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace xray::render::RENDER_NAMESPACE
{
#define GAMESAVE_SIZE 128

namespace
{
struct GamesaveScreenshotJob
{
    DirectX::ScratchImage image;
    std::string name;
    u64 id{};
};

struct GamesaveGpuCapture
{
    ~GamesaveGpuCapture()
    {
        _RELEASE(ready);
        _RELEASE(staging);
    }

    ID3D11Texture2D* staging{};
    ID3D11Query* ready{};
    std::string name;
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    u32 width{};
    u32 height{};
};

class GamesaveScreenshotQueue
{
public:
    GamesaveScreenshotQueue() : worker(&GamesaveScreenshotQueue::run, this) {}

    ~GamesaveScreenshotQueue()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        condition.notify_one();
        worker.join();
    }

    void enqueue(DirectX::ScratchImage&& image, pcstr name)
    {
        {
            std::lock_guard lock(mutex);
            jobs.push_back({std::move(image), name, nextId++});
        }
        condition.notify_one();
    }

    bool enqueue_gpu_capture(ID3D11Resource* source, pcstr name)
    {
        ID3D11Texture2D* sourceTexture = nullptr;
        if (FAILED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sourceTexture))))
            return false;

        D3D11_TEXTURE2D_DESC sourceDescription;
        sourceTexture->GetDesc(&sourceDescription);

        auto capture = std::make_unique<GamesaveGpuCapture>();
        capture->name = name;
        capture->format = sourceDescription.Format;
        capture->width = sourceDescription.Width;
        capture->height = sourceDescription.Height;

        D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
        stagingDescription.MipLevels = 1;
        stagingDescription.ArraySize = 1;
        stagingDescription.SampleDesc.Count = 1;
        stagingDescription.SampleDesc.Quality = 0;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;

        HRESULT result = HW.pDevice->CreateTexture2D(&stagingDescription, nullptr, &capture->staging);
        ID3D11Texture2D* resolvedTexture = nullptr;
        if (SUCCEEDED(result) && sourceDescription.SampleDesc.Count > 1)
        {
            D3D11_TEXTURE2D_DESC resolvedDescription = stagingDescription;
            resolvedDescription.Usage = D3D11_USAGE_DEFAULT;
            resolvedDescription.CPUAccessFlags = 0;
            result = HW.pDevice->CreateTexture2D(&resolvedDescription, nullptr, &resolvedTexture);
        }

        D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(result))
            result = HW.pDevice->CreateQuery(&queryDescription, &capture->ready);

        ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
        if (SUCCEEDED(result))
        {
            if (resolvedTexture)
            {
                context->ResolveSubresource(resolvedTexture, 0, sourceTexture, 0, sourceDescription.Format);
                context->CopyResource(capture->staging, resolvedTexture);
            }
            else
                context->CopyResource(capture->staging, sourceTexture);

            // The event completes only after the staging copy reaches the GPU command stream.
            context->End(capture->ready);
            gpuCaptures.emplace_back(std::move(capture));
        }

        _RELEASE(resolvedTexture);
        _RELEASE(sourceTexture);
        return SUCCEEDED(result);
    }

    void process_gpu_captures()
    {
        if (gpuCaptures.empty())
            return;

        ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
        GamesaveGpuCapture& capture = *gpuCaptures.front();
        const HRESULT state =
            context->GetData(capture.ready, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (state == S_FALSE)
            return;
        if (FAILED(state))
        {
            gpuCaptures.pop_front();
            return;
        }

        complete_front_gpu_capture(context);
    }

    void flush_gpu_captures()
    {
        if (gpuCaptures.empty())
            return;

        ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
        context->Flush();
        while (!gpuCaptures.empty())
            complete_front_gpu_capture(context);
    }

private:
    void complete_front_gpu_capture(ID3D11DeviceContext* context)
    {
        GamesaveGpuCapture& capture = *gpuCaptures.front();

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(capture.staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            gpuCaptures.pop_front();
            return;
        }

        DirectX::ScratchImage image;
        const HRESULT initialized =
            image.Initialize2D(capture.format, capture.width, capture.height, 1, 1);
        if (SUCCEEDED(initialized))
        {
            const DirectX::Image* destination = image.GetImage(0, 0, 0);
            const size_t rowSize = std::min(destination->rowPitch, static_cast<size_t>(mapped.RowPitch));
            for (u32 row = 0; row < capture.height; ++row)
            {
                memcpy(destination->pixels + row * destination->rowPitch,
                    static_cast<const u8*>(mapped.pData) + row * mapped.RowPitch, rowSize);
            }
        }
        context->Unmap(capture.staging, 0);

        if (SUCCEEDED(initialized))
            enqueue(std::move(image), capture.name.c_str());
        gpuCaptures.pop_front();
    }

    static bool write_file(pcstr name, const void* data, size_t size)
    {
        const HANDLE file = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        const auto* cursor = static_cast<const u8*>(data);
        size_t remaining = size;
        bool succeeded = true;
        while (remaining)
        {
            const DWORD blockSize =
                static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(file, cursor, blockSize, &written, nullptr) || written != blockSize)
            {
                succeeded = false;
                break;
            }
            cursor += written;
            remaining -= written;
        }

        succeeded = succeeded && FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        return succeeded;
    }

    static void execute(GamesaveScreenshotJob&& job)
    {
        DirectX::ScratchImage resized;
        if (FAILED(Resize(*job.image.GetImage(0, 0, 0), GAMESAVE_SIZE, GAMESAVE_SIZE,
                DirectX::TEX_FILTER_BOX, resized)))
            return;

        DirectX::ScratchImage compressed;
        if (FAILED(Compress(*resized.GetImage(0, 0, 0), DXGI_FORMAT_BC1_UNORM,
                DirectX::TEX_COMPRESS_DEFAULT | DirectX::TEX_COMPRESS_PARALLEL, 0.f, compressed)))
            return;

        DirectX::Blob saved;
        if (FAILED(SaveToDDSMemory(
                *compressed.GetImage(0, 0, 0), DirectX::DDS_FLAGS_FORCE_DX9_LEGACY, saved)))
            return;

        const std::string temporaryName =
            job.name + "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(job.id) + ".tmp";
        if (!write_file(temporaryName.c_str(), saved.GetBufferPointer(), saved.GetBufferSize()) ||
            !MoveFileExA(temporaryName.c_str(), job.name.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileA(temporaryName.c_str());
        }
    }

    void run()
    {
        for (;;)
        {
            GamesaveScreenshotJob job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty())
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            execute(std::move(job));
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<GamesaveScreenshotJob> jobs;
    std::deque<std::unique_ptr<GamesaveGpuCapture>> gpuCaptures;
    std::thread worker;
    u64 nextId{};
    bool stopping{};
};

std::unique_ptr<GamesaveScreenshotQueue> gamesaveScreenshotQueue;

GamesaveScreenshotQueue& gamesave_screenshot_queue()
{
    if (!gamesaveScreenshotQueue)
        gamesaveScreenshotQueue = std::make_unique<GamesaveScreenshotQueue>();
    return *gamesaveScreenshotQueue;
}
}

void CRender::Screenshot(ScreenshotMode mode /*= SM_NORMAL*/, pcstr name /*= nullptr*/)
{
    ID3DResource* pSrcTexture;
    Target->get_base_rt()->GetResource(&pSrcTexture);

    if (!pSrcTexture)
    {
        Log("! Failed to make a screenshot: couldn't obtain base RT resource");
        return;
    }

    if (mode == IRender::SM_FOR_GAMESAVE)
    {
        VERIFY(name);
        if (!gamesave_screenshot_queue().enqueue_gpu_capture(pSrcTexture, name))
            Log("! Failed to queue the gamesave screenshot");
        _RELEASE(pSrcTexture);
        return;
    }

    // Load source texture
    DirectX::ScratchImage image;
    if (FAILED(CaptureTexture(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID), pSrcTexture, image)))
    {
        Log("! Failed to make a screenshot: couldn't capture texture");
        _RELEASE(pSrcTexture);
        return;
    }

    // Save
    switch (mode)
    {
    case IRender::SM_FOR_GAMESAVE:
        break;
    case IRender::SM_NORMAL:
    {
        string64 t_stemp;
        string_path buf;
        xr_sprintf(buf, "ss_%s_%s_(%s).jpg", Core.UserName, timestamp(t_stemp), g_pGameLevel ? g_pGameLevel->name().c_str() : "mainmenu");

        DirectX::Blob saved;
        auto hr = SaveToWICMemory(*image.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatJpeg, saved);
        if (SUCCEEDED(hr))
        {
            if (IWriter* fs = FS.w_open("$screenshots$", buf))
            {
                fs->w(saved.GetBufferPointer(), saved.GetBufferSize());
                FS.w_close(fs);
            }
        }

        // hq
        if (strstr(Core.Params, "-ss_tga"))
        {
            xr_sprintf(buf, "ssq_%s_%s_(%s).tga", Core.UserName, timestamp(t_stemp), g_pGameLevel ? g_pGameLevel->name().c_str() : "mainmenu");

            hr = SaveToTGAMemory(*image.GetImage(0, 0, 0), saved);
            if (FAILED(hr))
                goto _end_;

            if (IWriter* fs = FS.w_open("$screenshots$", buf))
            {
                fs->w(saved.GetBufferPointer(), saved.GetBufferSize());
                FS.w_close(fs);
            }
        }
        break;
    }
    case IRender::SM_FOR_LEVELMAP:
    case IRender::SM_FOR_CUBEMAP:
    {
        string_path buf;
        VERIFY(name);
        strconcat(sizeof(buf), buf, name, ".tga");

        ID3DTexture2D* pTex = Target->t_ss_async;
        HW.get_context(CHW::IMM_CTX_ID)->CopyResource(pTex, pSrcTexture);

        D3D_MAPPED_TEXTURE2D MappedData;
        HW.get_context(CHW::IMM_CTX_ID)->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);
        // Swap r and b, but don't kill alpha
        {
            u32* pPixel = (u32*)MappedData.pData;
            u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);

            for (; pPixel != pEnd; pPixel++)
            {
                u32 p = *pPixel;
                *pPixel = color_argb(color_get_A(p), color_get_B(p), color_get_G(p), color_get_R(p));
            }
        }
        // save
        u32* data = (u32*)xr_malloc(Device.dwHeight * Device.dwHeight * 4);
        imf_Process(data, Device.dwHeight, Device.dwHeight, (u32*)MappedData.pData, Device.dwWidth, Device.dwHeight, imf_lanczos3);
        HW.get_context(CHW::IMM_CTX_ID)->Unmap(pTex, 0);

        if (IWriter* fs = FS.w_open("$screenshots$", buf))
        {
            XRay::Media::Image img{ Device.dwHeight, Device.dwHeight, data, XRay::Media::ImageDataFormat::RGBA8 };
            img.SaveTGA(*fs, true);
            FS.w_close(fs);
        }
        xr_free(data);
        break;
    }
    } // switch (mode)

_end_:
    _RELEASE(pSrcTexture);
}

void CRender::ProcessGamesaveScreenshots()
{
    if (gamesaveScreenshotQueue)
        gamesaveScreenshotQueue->process_gpu_captures();
}

void flush_gamesave_screenshots()
{
    if (!gamesaveScreenshotQueue)
        return;

    gamesaveScreenshotQueue->flush_gpu_captures();
    gamesaveScreenshotQueue.reset();
}
} // namespace xray::render::RENDER_NAMESPACE
