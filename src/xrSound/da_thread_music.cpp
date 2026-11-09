// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "Sound.h"

#if defined(XR_PLATFORM_WINDOWS)
// The project targets an older Windows than this header allows; the library is looked up at run time.
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <xaudio2.h>
#pragma pop_macro("_WIN32_WINNT")
#include <vorbis/vorbisfile.h>

#include <atomic>
#include <mutex>
#include <random>
#include <thread>

namespace
{
constexpr u32 BufferCount = 4;
constexpr u32 BufferBytes = 64 * 1024;
constexpr u32 FadeMs = 250;

const ov_callbacks s_callbacks = {
    [](void* ptr, size_t size, size_t nmemb, void* source) -> size_t {
        auto* file = static_cast<IReader*>(source);
        const size_t blocks = std::min(size_t(file->elapsed()) / size, nmemb);
        file->r(ptr, u32(blocks * size));
        return blocks;
    },
    [](void* source, ogg_int64_t offset, int whence) -> int {
        auto* file = static_cast<IReader*>(source);
        switch (whence)
        {
        case SEEK_SET: file->seek(int(offset)); break;
        case SEEK_CUR: file->advance(int(offset)); break;
        case SEEK_END: file->seek(int(offset) + file->length()); break;
        default: break;
        }
        return 0;
    },
    nullptr,
    [](void* source) -> long { return long(static_cast<IReader*>(source)->tell()); },
};

// One list of tracks, played until it is told to go: a thread, its own XAudio2 and voice, nothing shared with the
// sound engine, which stands still for as long as a loading step takes.
class Player
{
    xr_vector<DaThreadMusicTrack> m_tracks;
    bool m_next;
    std::atomic<u32> m_fade_at{ 0 };
    std::atomic<bool> m_quit{ false };
    std::thread m_thread;

    float fade() const
    {
        const u32 from = m_fade_at.load(std::memory_order_relaxed);
        if (!from)
            return 1.f;
        const u32 passed = GetTickCount() - from;
        return passed >= FadeMs ? 0.f : 1.f - float(passed) / float(FadeMs);
    }

    size_t pick(std::minstd_rand& random, size_t avoid) const
    {
        float total = 0.f;
        for (size_t i = 0; i < m_tracks.size(); ++i)
            total += i == avoid ? 0.f : m_tracks[i].weight;
        if (total <= 0.f)
            return avoid < m_tracks.size() ? avoid : 0;
        float roll = std::uniform_real_distribution<float>(0.f, total)(random);
        for (size_t i = 0; i < m_tracks.size(); ++i)
        {
            if (i == avoid)
                continue;
            roll -= m_tracks[i].weight;
            if (roll <= 0.f)
                return i;
        }
        return 0;
    }

    // false when playing should end: told to, or the track cannot be played
    bool play(IXAudio2* xaudio, const DaThreadMusicTrack& track)
    {
        string_path name;
        strconcat(name, track.name.c_str(), ".ogg");
        IReader* file = FS.r_open("$game_sounds$", name);
        if (!file)
        {
            Msg("! [screens] no sound %s", name);
            return false;
        }

        OggVorbis_File vorbis{};
        if (ov_open_callbacks(file, &vorbis, nullptr, 0, s_callbacks) != 0)
        {
            Msg("! [screens] %s: not an Ogg Vorbis file", name);
            FS.r_close(file);
            return false;
        }

        Msg("* [screens] music: %s", name);
        const vorbis_info* info = ov_info(&vorbis, -1);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = WORD(info->channels);
        format.nSamplesPerSec = DWORD(info->rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * 2;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        bool go_on = false;
        IXAudio2SourceVoice* voice = nullptr;
        if (SUCCEEDED(xaudio->CreateSourceVoice(&voice, &format)) && voice)
        {
            xr_vector<u8> memory(BufferCount * BufferBytes);
            u32 next = 0;
            bool ended = false;
            voice->SetVolume(0.f);
            voice->Start();
            while (!m_quit.load(std::memory_order_relaxed))
            {
                const float k = fade();
                if (k <= 0.f)
                    break;

                XAUDIO2_VOICE_STATE state{};
                voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                while (!ended && state.BuffersQueued < BufferCount)
                {
                    u8* at = memory.data() + next * BufferBytes;
                    u32 filled = 0;
                    while (filled < BufferBytes)
                    {
                        int section = 0;
                        const long got = ov_read(&vorbis, reinterpret_cast<char*>(at) + filled, int(BufferBytes - filled), 0, 2, 1, &section);
                        if (got <= 0)
                        {
                            ended = true;
                            break;
                        }
                        filled += u32(got);
                    }
                    if (filled)
                    {
                        XAUDIO2_BUFFER buffer{};
                        buffer.AudioBytes = filled;
                        buffer.pAudioData = at;
                        buffer.Flags = ended ? XAUDIO2_END_OF_STREAM : 0;
                        voice->SubmitSourceBuffer(&buffer);
                        next = (next + 1) % BufferCount;
                        ++state.BuffersQueued;
                    }
                }
                if (ended && !state.BuffersQueued)
                {
                    go_on = true;
                    break;
                }
                voice->SetVolume(std::clamp(psSoundVFactor * psSoundVMusic, 0.f, 1.f) * track.volume * k);
                Sleep(10);
            }
            voice->Stop();
            voice->DestroyVoice();
        }
        ov_clear(&vorbis);
        FS.r_close(file);
        return go_on;
    }

    void run()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // by hand: no Windows without it is worth refusing to start for a loading screen's music
        using Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        HMODULE library = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create = library ? reinterpret_cast<Create>(GetProcAddress(library, "XAudio2Create")) : nullptr;
        IXAudio2* xaudio = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        if (create && SUCCEEDED(create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) && SUCCEEDED(xaudio->CreateMasteringVoice(&master)))
        {
            std::minstd_rand random{ GetTickCount() };
            size_t current = pick(random, m_tracks.size());
            while (play(xaudio, m_tracks[current]))
                if (m_next && m_tracks.size() > 1)
                    current = pick(random, current);
        }
        else
            Msg("! [screens] the loading screen's music has no XAudio2 to play through");
        if (master)
            master->DestroyVoice();
        if (xaudio)
            xaudio->Release();
        if (library)
            FreeLibrary(library);
        if (SUCCEEDED(com))
            CoUninitialize();
    }

public:
    Player(const xr_vector<DaThreadMusicTrack>& tracks, bool next) : m_tracks(tracks), m_next(next), m_thread([this] { run(); }) {}
    ~Player()
    {
        m_quit = true;
        if (m_thread.joinable())
            m_thread.join();
    }
    void fade_out()
    {
        u32 expected = 0;
        const u32 now = GetTickCount();
        m_fade_at.compare_exchange_strong(expected, now ? now : 1);
    }
};

std::mutex s_lock;
std::unique_ptr<Player> s_player;
} // namespace

void da_thread_music_play(const xr_vector<DaThreadMusicTrack>& tracks, bool next_track)
{
    std::lock_guard guard(s_lock);
    s_player.reset();
    if (!tracks.empty())
        s_player = std::make_unique<Player>(tracks, next_track);
}

void da_thread_music_stop()
{
    std::lock_guard guard(s_lock);
    if (s_player)
        s_player->fade_out();
}

void da_thread_music_shutdown()
{
    std::lock_guard guard(s_lock);
    s_player.reset();
}
#else
void da_thread_music_play(const xr_vector<DaThreadMusicTrack>&, bool) {}
void da_thread_music_stop() {}
void da_thread_music_shutdown() {}
#endif
