// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "da_media.h"

#if defined(XR_PLATFORM_WINDOWS)
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace
{
template <class T>
void release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

struct Candidate
{
    pcstr ext;
    DaMediaKind kind;
};

constexpr Candidate candidates[] = {
    { ".ogm", DaMediaKind::Theora },
    { ".avi", DaMediaKind::Avi },
    { ".gif", DaMediaKind::Gif },
    { ".mp4", DaMediaKind::Video },
    { ".webm", DaMediaKind::Video },
    { ".wmv", DaMediaKind::Video },
    { ".seq", DaMediaKind::Sequence },
    { ".dds", DaMediaKind::Image },
};
} // namespace

namespace
{
struct PublishedPicture
{
    u32 width{};
    u32 height{};
    xr_vector<u8> pixels;
    u32 revision{};
};

std::mutex g_pictures_lock;
xr_map<xr_string, PublishedPicture> g_pictures;

class PictureSource final : public CDaMediaSource
{
    xr_string m_id;
    xr_vector<u8> m_copy;
    u32 m_revision{};

    // the picture's pixels when they are newer than the copy and of its size
    bool take(bool first)
    {
        std::lock_guard lock(g_pictures_lock);
        const auto it = g_pictures.find(m_id);
        if (it == g_pictures.end() || it->second.pixels.empty())
            return false;
        const PublishedPicture& picture = it->second;
        if (first)
        {
            m_width = picture.width;
            m_height = picture.height;
        }
        else if (picture.revision == m_revision || picture.width != m_width || picture.height != m_height)
            return false;
        m_copy = picture.pixels;
        m_revision = picture.revision;
        m_pixels = m_copy.data();
        return true;
    }

public:
    explicit PictureSource(pcstr id) : m_id(id) {}
    bool Open() { return take(true); }
    bool Update(u32) override { return take(false); }
};
} // namespace

void da_media_publish(pcstr id, u32 width, u32 height, const u8* rgba)
{
    std::lock_guard lock(g_pictures_lock);
    PublishedPicture& picture = g_pictures[id];
    picture.width = width;
    picture.height = height;
    picture.pixels.assign(rgba, rgba + size_t(width) * height * 4);
    ++picture.revision;
}

void da_media_forget(pcstr id)
{
    std::lock_guard lock(g_pictures_lock);
    g_pictures.erase(id);
}

DaMediaKind da_media_resolve(pcstr name, string_path& fn)
{
    if (!strncmp(name, DA_MEDIA_PICTURE, sizeof(DA_MEDIA_PICTURE) - 1))
    {
        xr_strcpy(fn, name + sizeof(DA_MEDIA_PICTURE) - 1);
        return DaMediaKind::Picture;
    }

    DaMediaKind first = DaMediaKind::None;
    DaMediaKind loose = DaMediaKind::None;
    bool firstLoose = false;
    string_path path, loosePath;
    for (const Candidate& c : candidates)
    {
        // most names are a plain picture: the .dds only matters against a moving file of the same name
        if (c.kind == DaMediaKind::Image && first == DaMediaKind::None)
            break;
        if (!FS.exist(path, "$game_textures$", name, c.ext))
            continue;
        // the file system marks a file outside the archives with the largest archive index
        const CLocatorAPI::file* desc = FS.GetFileDesc(path);
        const bool isLoose = desc && desc->vfs == std::numeric_limits<size_t>::max();
        if (first == DaMediaKind::None)
        {
            first = c.kind;
            firstLoose = isLoose;
            xr_strcpy(fn, path);
        }
        if (loose == DaMediaKind::None && isLoose)
        {
            loose = c.kind;
            xr_strcpy(loosePath, path);
        }
    }
    if (loose != DaMediaKind::None && !firstLoose)
    {
        xr_strcpy(fn, loosePath);
        return loose;
    }
    return first;
}

u32 da_media_time() { return Device.TimeContinualNow(); }

DaUIShaderState g_da_ui_shader;

#if defined(XR_PLATFORM_WINDOWS)
namespace
{
// A read-only IStream over a file the file system opened: Media Foundation reads a video from an archive
// through it. The stream owns the reader; the last reference closes it.
class ReaderStream final : public IStream
{
    std::atomic<ULONG> m_refs{ 1 };
    IReader* m_reader;
    const u8* m_data;
    u64 m_size;
    u64 m_pos{};

    ~ReaderStream() { FS.r_close(m_reader); }

public:
    explicit ReaderStream(IReader* reader)
        : m_reader(reader), m_data(static_cast<const u8*>(reader->pointer())), m_size(u64(reader->length())) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream))
        {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (!left)
            delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* read) override
    {
        const u64 n = std::min<u64>(cb, m_pos < m_size ? m_size - m_pos : 0);
        CopyMemory(pv, m_data + m_pos, size_t(n));
        m_pos += n;
        if (read)
            *read = ULONG(n);
        return n == cb ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override
    {
        s64 base = 0;
        if (origin == STREAM_SEEK_CUR)
            base = s64(m_pos);
        else if (origin == STREAM_SEEK_END)
            base = s64(m_size);
        else if (origin != STREAM_SEEK_SET)
            return STG_E_INVALIDFUNCTION;
        const s64 to = base + move.QuadPart;
        if (to < 0)
            return STG_E_INVALIDFUNCTION;
        m_pos = u64(to);
        if (result)
            result->QuadPart = m_pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override
    {
        if (!stat)
            return E_POINTER;
        ZeroMemory(stat, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = m_size;
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};

u16 read_u16(IWICMetadataQueryReader* query, LPCWSTR name, u16 fallback)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    u16 result = fallback;
    if (SUCCEEDED(query->GetMetadataByName(name, &value)))
    {
        if (value.vt == VT_UI2)
            result = value.uiVal;
        else if (value.vt == VT_UI1)
            result = value.bVal;
    }
    PropVariantClear(&value);
    return result;
}

// An animated GIF through WIC: each frame composed over the ones before it the way its disposal says,
// at its own delay. The composed frames are kept while they fit in a budget, so only the first loop of
// a small GIF decodes; a large one decodes as it plays.
class GifSource final : public CDaMediaSource
{
    struct Frame
    {
        u32 left, top, width, height;
        u32 start; // ms from the start of a loop
        u8 disposal;
    };

    static constexpr size_t cache_budget = 96u << 20;

    IReader* m_file{};
    IWICImagingFactory* m_factory{};
    IWICStream* m_stream{};
    IWICBitmapDecoder* m_decoder{};
    xr_vector<Frame> m_frames;
    u32 m_loop{}; // ms
    xr_vector<u8> m_canvas;
    xr_vector<u8> m_saved; // the canvas a "restore to previous" frame puts back
    xr_vector<u8> m_scratch;
    xr_vector<xr_vector<u8>> m_cache;
    bool m_caching{};
    int m_shown{ -1 }; // the frame Pixels shows
    int m_composed{ -1 }; // the frame the canvas holds
    u32 m_start{};

    // Composing goes forward from the frame the canvas holds; going back starts it over.
    void compose_to(int target)
    {
        const int from = target < m_composed ? -1 : m_composed;
        for (int i = from + 1; i <= target; ++i)
        {
            compose(i);
            if (m_caching && m_cache[i].empty())
                m_cache[i] = m_canvas;
        }
        m_composed = target;
    }

    void compose(int index)
    {
        if (index == 0)
            std::fill(m_canvas.begin(), m_canvas.end(), u8(0));
        else
        {
            // what the frame before leaves behind
            const Frame& prev = m_frames[index - 1];
            if (prev.disposal == 2)
                clear_rect(prev);
            else if (prev.disposal == 3 && m_saved.size() == m_canvas.size())
                m_canvas = m_saved;
        }

        const Frame& f = m_frames[index];
        if (f.disposal == 3)
            m_saved = m_canvas;

        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(m_decoder->GetFrame(index, &frame)) && SUCCEEDED(m_factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)))
        {
            UINT w = 0, h = 0;
            converter->GetSize(&w, &h);
            m_scratch.resize(size_t(w) * h * 4);
            if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(m_scratch.size()), m_scratch.data())))
            {
                // a GIF's pixel is there or not: the transparent ones leave the canvas as it is
                const u32 rw = std::min(w, m_width > f.left ? m_width - f.left : 0);
                const u32 rh = std::min(h, m_height > f.top ? m_height - f.top : 0);
                for (u32 y = 0; y < rh; ++y)
                {
                    const u8* src = &m_scratch[size_t(y) * w * 4];
                    u8* dst = &m_canvas[(size_t(f.top + y) * m_width + f.left) * 4];
                    for (u32 x = 0; x < rw; ++x, src += 4, dst += 4)
                        if (src[3])
                            CopyMemory(dst, src, 4);
                }
            }
        }
        release(converter);
        release(frame);
    }

    void clear_rect(const Frame& f)
    {
        const u32 rw = std::min(f.width, m_width > f.left ? m_width - f.left : 0);
        const u32 rh = std::min(f.height, m_height > f.top ? m_height - f.top : 0);
        for (u32 y = 0; y < rh; ++y)
            ZeroMemory(&m_canvas[(size_t(f.top + y) * m_width + f.left) * 4], size_t(rw) * 4);
    }

public:
    ~GifSource() override
    {
        release(m_decoder);
        release(m_stream);
        release(m_factory);
        if (m_file)
            FS.r_close(m_file);
    }

    bool Open(pcstr fn)
    {
        // WIC needs COM; a thread that has none joins the process's multithreaded apartment, whichever
        // thread loads the texture, without being initialised for good in one model or the other
        static const bool mta = [] {
            // (Windows 8's: the engine's headers are set for 7, so it is looked up)
            using Increment = HRESULT(WINAPI*)(void**);
            HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            const auto increment = combase ? reinterpret_cast<Increment>(GetProcAddress(combase, "CoIncrementMTAUsage")) : nullptr;
            void* cookie = nullptr;
            return increment && SUCCEEDED(increment(&cookie));
        }();
        UNUSED(mta);
        m_file = FS.r_open(fn);
        if (!m_file || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_factory))) ||
            FAILED(m_factory->CreateStream(&m_stream)) ||
            FAILED(m_stream->InitializeFromMemory(static_cast<BYTE*>(m_file->pointer()), DWORD(m_file->length()))) ||
            FAILED(m_factory->CreateDecoderFromStream(m_stream, nullptr, WICDecodeMetadataCacheOnLoad, &m_decoder)))
            return false;

        UINT count = 0;
        if (FAILED(m_decoder->GetFrameCount(&count)) || !count)
            return false;

        IWICMetadataQueryReader* global = nullptr;
        if (SUCCEEDED(m_decoder->GetMetadataQueryReader(&global)))
        {
            m_width = read_u16(global, L"/logscrdesc/Width", 0);
            m_height = read_u16(global, L"/logscrdesc/Height", 0);
            release(global);
        }

        m_frames.resize(count);
        u32 time = 0;
        for (UINT i = 0; i < count; ++i)
        {
            Frame& f = m_frames[i];
            f = {};
            f.start = time;
            IWICBitmapFrameDecode* frame = nullptr;
            if (FAILED(m_decoder->GetFrame(i, &frame)))
                return false;
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            f.width = w;
            f.height = h;
            u32 delay = 0;
            IWICMetadataQueryReader* query = nullptr;
            if (SUCCEEDED(frame->GetMetadataQueryReader(&query)))
            {
                f.left = read_u16(query, L"/imgdesc/Left", 0);
                f.top = read_u16(query, L"/imgdesc/Top", 0);
                delay = read_u16(query, L"/grctlext/Delay", 0) * 10u;
                f.disposal = u8(read_u16(query, L"/grctlext/Disposal", 0));
                release(query);
            }
            release(frame);
            // as the browsers do: a delay under 20 ms is played at 100 ms
            time += delay < 20 ? 100 : delay;
            if (!m_width || !m_height)
            {
                m_width = std::max(m_width, f.left + f.width);
                m_height = std::max(m_height, f.top + f.height);
            }
        }
        m_loop = std::max(time, 1u);
        if (!m_width || !m_height)
            return false;

        m_canvas.assign(size_t(m_width) * m_height * 4, 0);
        m_caching = count > 1 && m_canvas.size() * count <= cache_budget;
        if (m_caching)
            m_cache.resize(count);
        compose_to(0);
        m_shown = 0;
        m_pixels = m_canvas.data();
        return true;
    }

    bool Update(u32 time) override
    {
        int target = 0;
        if (!m_started)
        {
            m_started = true;
            m_start = time;
        }
        else if (m_frames.size() > 1)
        {
            const u32 t = (time - m_start) % m_loop;
            // the last frame that has started by t
            const auto it = std::upper_bound(m_frames.begin(), m_frames.end(), t,
                [](u32 value, const Frame& f) { return value < f.start; });
            target = int(it - m_frames.begin()) - 1;
        }
        if (target == m_shown)
            return false;

        if (m_caching && !m_cache[target].empty())
            m_pixels = m_cache[target].data();
        else
        {
            compose_to(target);
            m_pixels = m_canvas.data();
        }
        m_shown = target;
        return true;
    }
};

std::mutex mf_lock;
int mf_users = 0;

bool mf_start()
{
    std::lock_guard guard(mf_lock);
    // Media Foundation is loaded late: a Windows N without its Media Feature Pack has none, and the game runs
    // without videos there
    if (!mf_users && (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))))
        return false;
    ++mf_users;
    return true;
}

void mf_stop()
{
    std::lock_guard guard(mf_lock);
    if (mf_users && !--mf_users)
        MFShutdown();
}

// A video through Media Foundation's source reader, decoded on a thread of its own a few frames ahead
// of the clock; at its end it starts over.
class VideoSource final : public CDaMediaSource
{
    struct Decoded
    {
        s64 time; // ms from the start of the playback, across loops
        xr_vector<u8> pixels;
    };

    static constexpr size_t queue_depth = 3;

    xr_string m_path;
    std::thread m_thread;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::deque<Decoded> m_queue;
    xr_vector<xr_vector<u8>> m_free;
    xr_vector<u8> m_shown;
    bool m_stop{};
    bool m_opened{};
    bool m_failed{};
    bool m_started_mf{};
    u32 m_start{};

    // the picture inside the decoded frame (a decoder pads 1080 rows to 1088), and the frame's rows
    u32 m_cropX{}, m_cropY{};
    u32 m_bufferHeight{};
    LONG m_stride{};

    bool configure(IMFSourceReader* reader)
    {
        IMFMediaType* type = nullptr;
        if (FAILED(MFCreateMediaType(&type)))
            return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        const HRESULT set = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
        release(type);
        if (FAILED(set) || FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)))
            return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        m_bufferHeight = h;
        m_stride = LONG(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, w * 4));
        MFVideoArea area{};
        UINT32 got = 0;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &got)) &&
            got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0)
        {
            m_cropX = u32(area.OffsetX.value);
            m_cropY = u32(area.OffsetY.value);
            w = std::min<UINT32>(UINT32(area.Area.cx), w - m_cropX);
            h = std::min<UINT32>(UINT32(area.Area.cy), h - m_cropY);
        }
        release(type);
        if (!w || !h)
            return false;
        m_width = w;
        m_height = h;
        return true;
    }

    void copy_frame(IMFSample* sample, xr_vector<u8>& out)
    {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return;
        out.resize(size_t(m_width) * m_height * 4);

        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        IMF2DBuffer* buffer2d = nullptr;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && FAILED(buffer2d->Lock2D(&scan0, &pitch)))
        {
            scan0 = nullptr;
            release(buffer2d);
        }
        if (!buffer2d)
        {
            // a bottom-up picture's first row is the last one in memory
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
            {
                pitch = m_stride;
                const size_t needed = size_t(std::abs(pitch)) * m_bufferHeight;
                if (needed <= length && size_t(m_cropX + m_width) * 4 <= size_t(std::abs(pitch)) && m_cropY + m_height <= m_bufferHeight)
                    scan0 = pitch < 0 ? data + size_t(-pitch) * (m_bufferHeight - 1) : data;
                else
                    buffer->Unlock();
            }
        }

        if (scan0)
        {
            // RGB32 is B, G, R, unused: the texture wants R, G, B and an opaque alpha
            for (u32 y = 0; y < m_height; ++y)
            {
                const u8* src = scan0 + s64(y + m_cropY) * pitch + size_t(m_cropX) * 4;
                u8* dst = &out[size_t(y) * m_width * 4];
                for (u32 x = 0; x < m_width; ++x, src += 4, dst += 4)
                {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 255;
                }
            }
            if (buffer2d)
                buffer2d->Unlock2D();
            else
                buffer->Unlock();
        }
        release(buffer2d);
        release(buffer);
    }

    void run(IMFSourceReader* reader)
    {
        s64 loopBase = 0; // ms the loops before this one took
        s64 lastTime = 0;
        s64 frameStep = 33;
        while (true)
        {
            {
                std::unique_lock guard(m_lock);
                m_wake.wait(guard, [&] { return m_stop || m_queue.size() < queue_depth; });
                if (m_stop)
                    return;
            }

            DWORD flags = 0;
            LONGLONG stamp = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &stamp, &sample)))
            {
                release(sample);
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                release(sample);
                PROPVARIANT start;
                PropVariantInit(&start);
                start.vt = VT_I8;
                start.hVal.QuadPart = 0;
                const HRESULT seek = reader->SetCurrentPosition(GUID_NULL, start);
                PropVariantClear(&start);
                if (FAILED(seek) || lastTime <= 0)
                    break;
                loopBase += lastTime + frameStep;
                lastTime = 0;
                continue;
            }
            if (flags & MF_SOURCE_READERF_ERROR)
            {
                release(sample);
                break;
            }
            // A stream that changes its frame mid-way: the new layout, and an end where the size moved.
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            {
                const u32 width = m_width, height = m_height;
                if (!configure(reader) || m_width != width || m_height != height)
                {
                    release(sample);
                    break;
                }
            }
            if (!sample)
                continue;

            const s64 time = stamp / 10000;
            if (time > lastTime)
                frameStep = std::clamp<s64>(time - lastTime, 1, 200);
            lastTime = time;

            xr_vector<u8> pixels;
            {
                std::lock_guard guard(m_lock);
                if (!m_free.empty())
                {
                    pixels = std::move(m_free.back());
                    m_free.pop_back();
                }
            }
            copy_frame(sample, pixels);
            release(sample);
            std::lock_guard guard(m_lock);
            m_queue.push_back({ loopBase + time, std::move(pixels) });
        }
        Msg("! [media] %s: the video stopped playing", m_path.c_str());
    }

    void thread_main(IStream* stream)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMFByteStream* bytes = nullptr;
        IMFAttributes* attributes = nullptr;
        IMFSourceReader* reader = nullptr;
        bool ok = SUCCEEDED(MFCreateMFByteStreamOnStream(stream, &bytes)) && SUCCEEDED(MFCreateAttributes(&attributes, 1)) &&
            SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
            SUCCEEDED(MFCreateSourceReaderFromByteStream(bytes, attributes, &reader)) &&
            SUCCEEDED(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE)) &&
            SUCCEEDED(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE)) &&
            configure(reader);
        release(attributes);
        stream->Release();
        {
            std::lock_guard guard(m_lock);
            m_opened = true;
            m_failed = !ok;
        }
        m_wake.notify_all();
        if (ok)
            run(reader);
        release(reader);
        release(bytes);
        CoUninitialize();
    }

public:
    ~VideoSource() override
    {
        {
            std::lock_guard guard(m_lock);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        if (m_started_mf)
            mf_stop();
    }

    bool Open(pcstr fn)
    {
        m_path = fn;
        IReader* file = FS.r_open(fn);
        if (!file)
            return false;
        if (!mf_start())
        {
            FS.r_close(file);
            Msg("! [media] %s: Media Foundation is not available on this system", fn);
            return false;
        }
        m_started_mf = true;
        IStream* stream = new ReaderStream(file);
        m_thread = std::thread([this, stream] { thread_main(stream); });

        std::unique_lock guard(m_lock);
        m_wake.wait(guard, [&] { return m_opened; });
        if (m_failed)
            return false;
        // black until the first frame
        m_shown.assign(size_t(m_width) * m_height * 4, 0);
        for (size_t i = 3; i < m_shown.size(); i += 4)
            m_shown[i] = 255;
        m_pixels = m_shown.data();
        return true;
    }

    bool Update(u32 time) override
    {
        std::lock_guard guard(m_lock);
        if (!m_started)
        {
            if (m_queue.empty())
                return false;
            m_started = true;
            m_start = time - u32(std::max<s64>(m_queue.front().time, 0));
        }
        const s64 clock = s64(time - m_start);
        s64 shownTime = -1;
        while (!m_queue.empty() && m_queue.front().time <= clock)
        {
            if (!m_shown.empty())
                m_free.push_back(std::move(m_shown));
            shownTime = m_queue.front().time;
            m_shown = std::move(m_queue.front().pixels);
            m_queue.pop_front();
        }
        if (shownTime < 0)
            return false;
        // A texture nobody drew for a while finds its decoder waiting far behind the clock: the video
        // goes on from there instead of racing to catch up.
        if (clock - shownTime > 250)
            m_start = time - u32(shownTime);
        m_wake.notify_all();
        m_pixels = m_shown.data();
        return true;
    }
};
} // namespace

CDaMediaSource* CDaMediaSource::Open(DaMediaKind kind, pcstr fn)
{
    if (kind == DaMediaKind::Picture)
    {
        auto* picture = xr_new<PictureSource>(fn);
        if (picture->Open())
            return picture;
        xr_delete(picture);
        Msg("! [media] picture %s: nothing published under it", fn);
    }
    else if (kind == DaMediaKind::Gif)
    {
        auto* gif = xr_new<GifSource>();
        if (gif->Open(fn))
            return gif;
        xr_delete(gif);
        Msg("! [media] %s: not a GIF the game can play", fn);
    }
    else if (kind == DaMediaKind::Video)
    {
        auto* video = xr_new<VideoSource>();
        if (video->Open(fn))
            return video;
        xr_delete(video);
        Msg("! [media] %s: Media Foundation cannot play this video (its codec may be missing)", fn);
    }
    return nullptr;
}

IStream* da_media_open_stream(pcstr fn)
{
    IReader* file = FS.r_open(fn);
    return file ? new ReaderStream(file) : nullptr;
}
#else
CDaMediaSource* CDaMediaSource::Open(DaMediaKind kind, pcstr fn)
{
    if (kind != DaMediaKind::Picture)
        return nullptr;
    auto* picture = xr_new<PictureSource>(fn);
    if (picture->Open())
        return picture;
    xr_delete(picture);
    return nullptr;
}
#endif
