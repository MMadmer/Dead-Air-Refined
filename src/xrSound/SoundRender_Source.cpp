#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"

#include "xrCore/XMS/xms_assets.h"
#include "xrServerEntities/ai_sounds.h"

#include <vorbis/vorbisfile.h>

CSoundRender_Source::~CSoundRender_Source() { unload(); }

namespace
{
bool ov_can_continue_read(long res)
{
    switch (res)
    {
    case 0:
        return false;
    // info
    case OV_HOLE:
        Msg("Vorbisfile encoutered missing or corrupt data in the bitstream. Recovery is normally automatic and this "
            "return code is for informational purposes only.");
        return true;
    case OV_EBADLINK:
        Msg("The given link exists in the Vorbis data stream, but is not decipherable due to garbage or corruption.");
        return true;
    // error
    case OV_FALSE: Msg("Not true, or no data available"); return false;
    case OV_EREAD: Msg("Read error while fetching compressed data for decode"); return false;
    case OV_EFAULT: Msg("Internal inconsistency in decode state. Continuing is likely not possible."); return false;
    case OV_EIMPL: Msg("Feature not implemented"); return false;
    case OV_EINVAL:
        Msg("Either an invalid argument, or incompletely initialized argument passed to libvorbisfile call");
        return false;
    case OV_ENOTVORBIS: Msg("The given file/data was not recognized as Ogg Vorbis data."); return false;
    case OV_EBADHEADER:
        Msg("The file/data is apparently an Ogg Vorbis stream, but contains a corrupted or undecipherable header.");
        return false;
    case OV_EVERSION: Msg("The bitstream format revision of the given stream is not supported."); return false;
    case OV_ENOSEEK: Msg("The given stream is not seekable"); return false;
    }
    return false;
}

// A module's description of a sound wins over what its .ogg states.
void apply_description(pcstr name, SoundSourceInfo& info)
{
    const XMS::Assets::Sound* s = XMS::Assets::FindSound(name);
    if (!s)
        return;
    if (s->has_min)
        info.minDist = s->min_distance;
    if (s->has_max)
        info.maxDist = s->max_distance;
    if (s->has_volume)
        info.baseVolume = s->volume;
    if (s->has_ai)
        info.maxAIDist = s->ai_distance;
    if (!s->has_type)
        return;

    // the game's sound types by their names in ai_sounds.h, without SOUND_TYPE_ and in lower case
    static constexpr std::pair<pcstr, u32> types[] = {
        {"no_sound", SOUND_TYPE_NO_SOUND}, {"weapon", SOUND_TYPE_WEAPON}, {"item", SOUND_TYPE_ITEM},
        {"monster", SOUND_TYPE_MONSTER}, {"anomaly", SOUND_TYPE_ANOMALY}, {"world", SOUND_TYPE_WORLD},
        {"picking_up", SOUND_TYPE_PICKING_UP}, {"dropping", SOUND_TYPE_DROPPING}, {"hiding", SOUND_TYPE_HIDING},
        {"taking", SOUND_TYPE_TAKING}, {"using", SOUND_TYPE_USING}, {"shooting", SOUND_TYPE_SHOOTING},
        {"empty_clicking", SOUND_TYPE_EMPTY_CLICKING}, {"bullet_hit", SOUND_TYPE_BULLET_HIT},
        {"recharging", SOUND_TYPE_RECHARGING}, {"dying", SOUND_TYPE_DYING}, {"injuring", SOUND_TYPE_INJURING},
        {"step", SOUND_TYPE_STEP}, {"talking", SOUND_TYPE_TALKING}, {"attacking", SOUND_TYPE_ATTACKING},
        {"eating", SOUND_TYPE_EATING}, {"idle", SOUND_TYPE_IDLE}, {"object_breaking", SOUND_TYPE_OBJECT_BREAKING},
        {"object_colliding", SOUND_TYPE_OBJECT_COLLIDING}, {"object_exploding", SOUND_TYPE_OBJECT_EXPLODING},
        {"ambient", SOUND_TYPE_AMBIENT}, {"item_picking_up", SOUND_TYPE_ITEM_PICKING_UP},
        {"item_dropping", SOUND_TYPE_ITEM_DROPPING}, {"item_hiding", SOUND_TYPE_ITEM_HIDING},
        {"item_taking", SOUND_TYPE_ITEM_TAKING}, {"item_using", SOUND_TYPE_ITEM_USING},
        {"weapon_shooting", SOUND_TYPE_WEAPON_SHOOTING}, {"weapon_empty_clicking", SOUND_TYPE_WEAPON_EMPTY_CLICKING},
        {"weapon_bullet_hit", SOUND_TYPE_WEAPON_BULLET_HIT}, {"weapon_recharging", SOUND_TYPE_WEAPON_RECHARGING},
        {"monster_dying", SOUND_TYPE_MONSTER_DYING}, {"monster_injuring", SOUND_TYPE_MONSTER_INJURING},
        {"monster_step", SOUND_TYPE_MONSTER_STEP}, {"monster_talking", SOUND_TYPE_MONSTER_TALKING},
        {"monster_attacking", SOUND_TYPE_MONSTER_ATTACKING}, {"monster_eating", SOUND_TYPE_MONSTER_EATING},
        {"anomaly_idle", SOUND_TYPE_ANOMALY_IDLE}, {"world_object_breaking", SOUND_TYPE_WORLD_OBJECT_BREAKING},
        {"world_object_colliding", SOUND_TYPE_WORLD_OBJECT_COLLIDING},
        {"world_object_exploding", SOUND_TYPE_WORLD_OBJECT_EXPLODING}, {"world_ambient", SOUND_TYPE_WORLD_AMBIENT},
    };
    u32 type = 0;
    for (const xr_string& word : s->type)
    {
        const auto it = std::ranges::find_if(types, [&word](const auto& t) { return word == t.first; });
        if (it != std::end(types))
            type |= it->second;
        else
            Msg("! %s: '%s' is no sound type - left out", s->file.c_str(), word.c_str());
    }
    info.gameType = type;
}
}

void CSoundRender_Source::decompress(void* dest, u32 byte_offset, u32 size, OggVorbis_File* ovf) const
{
    ZoneScoped;

    // seek
    const auto sample_offset = ogg_int64_t(byte_offset / m_data_info.blockAlign);
    const u32 cur_pos = u32(ov_pcm_tell(ovf));
    if (cur_pos != sample_offset)
        ov_pcm_seek(ovf, sample_offset);

    // decompress
    if (m_data_info.format == SoundFormat::Float32)
        i_decompress(ovf, static_cast<float*>(dest), size);
    else
        i_decompress(ovf, static_cast<char*>(dest), size);
}

void CSoundRender_Source::i_decompress(OggVorbis_File* ovf, char* _dest, u32 size) const
{
    long TotalRet = 0;

    // Read loop
    while (TotalRet < static_cast<long>(size))
    {
        const auto ret = ov_read(ovf, _dest + TotalRet, size - TotalRet, 0, 2, 1, nullptr);
        if (ret <= 0 && !ov_can_continue_read(ret))
            break;
        TotalRet += ret;
    }
}

void CSoundRender_Source::i_decompress(OggVorbis_File* ovf, float* _dest, u32 size) const
{
    s32 left = s32(size / m_data_info.blockAlign);
    while (left)
    {
        float** pcm;
        long samples = ov_read_float(ovf, &pcm, left, nullptr);

        if (samples <= 0 && !ov_can_continue_read(samples))
            break;

        if (samples > left)
            samples = left;

        for (long j = 0; j < samples; j++)
            for (long i = 0; i < m_data_info.channels; i++)
                *_dest++ = clampr(pcm[i][j], -1.0f, 1.0f);

        left -= samples;
    }
}

constexpr ov_callbacks g_ov_callbacks =
{
    // read
    [](void* ptr, size_t size, size_t nmemb, void* datasource) -> size_t
    {
        auto* file = static_cast<IReader*>(datasource);
        const size_t exist_block = _max(0ul, iFloor(file->elapsed() / (float)size));
        const size_t read_block = std::min(exist_block, nmemb);
        file->r(ptr, read_block * size);
        return read_block;
    },
    // seek
    [](void* datasource, ogg_int64_t offset, int whence) -> int
    {
        //SEEK_SET 0 File beginning
        //SEEK_CUR 1 Current file pointer position
        //SEEK_END 2 End-of-file
        switch (whence)
        {
        case SEEK_SET: ((IReader*)datasource)->seek((int)offset); break;
        case SEEK_CUR: ((IReader*)datasource)->advance((int)offset); break;
        case SEEK_END: ((IReader*)datasource)->seek((int)offset + ((IReader*)datasource)->length()); break;
        }
        return 0;
    },
    // close
    [](void* datasource) -> int
    {
        auto* file = static_cast<IReader*>(datasource);
        FS.r_close(file);
        return 0;
    },
    // tell
    [](void* datasource) -> long
    {
        const auto file = static_cast<IReader*>(datasource);
        return static_cast<long>(file->tell());
    },
};

OggVorbis_File* CSoundRender_Source::open() const
{
    auto* file = FS.r_open(pname.c_str());
    R_ASSERT3(file && file->length(), "Can't open wave file:", pname.c_str());

    auto* ovf = xr_new<OggVorbis_File>();
    if (ov_open_callbacks(file, ovf, nullptr, 0, g_ov_callbacks))
    {
        FS.r_close(file);
        xr_delete(ovf);
        return nullptr;
    }

    return ovf;
}

void CSoundRender_Source::close(OggVorbis_File*& ovf)
{
    if (!ovf)
        return;

    ov_clear(ovf);
    xr_delete(ovf);
}

bool CSoundRender_Source::LoadWave(pcstr pName)
{
    ZoneScoped;

    pname = pName;

    // Load file into memory and parse WAV-format
    OggVorbis_File ovf;
    {
        IReader* wave = FS.r_open(pname.c_str());
        R_ASSERT3(wave && wave->length(), "Can't open wave file:", pname.c_str());
        if (ov_open_callbacks(wave, &ovf, nullptr, 0, g_ov_callbacks))
        {
            FS.r_close(wave);
            return false;
        }
    }

    const vorbis_info* ovi = ov_info(&ovf, -1);

    // verify
    R_ASSERT3_CURE(ovi, "Invalid source info:", pName,
    {
        ov_clear(&ovf);
        return false;
    });

    m_data_info = {};

    m_data_info.samplesPerSec = ovi->rate;
    m_data_info.channels = u16(ovi->channels);

    if (SoundRender->supports_float_pcm)
    {
        m_data_info.format = SoundFormat::Float32;
        m_data_info.bitsPerSample = 32;
    }
    else
    {
        m_data_info.format = SoundFormat::PCM;
        m_data_info.bitsPerSample = 16;
    }

    m_data_info.blockAlign = m_data_info.bitsPerSample / 8 * m_data_info.channels;
    m_data_info.avgBytesPerSec = m_data_info.samplesPerSec * m_data_info.blockAlign;
    m_data_info.bytesPerBuffer = sdef_target_block * m_data_info.avgBytesPerSec / 1000;

    const s64 pcm_total = ov_pcm_total(&ovf, -1);
    dwBytesTotal = u32(pcm_total * m_data_info.blockAlign);
    fTimeTotal = dwBytesTotal / float(m_data_info.avgBytesPerSec);

    m_info = {};

    const vorbis_comment* ovm = ov_comment(&ovf, -1);
    if (ovm->comments)
    {
        IReader F(ovm->user_comments[0], ovm->comment_lengths[0]);
        const u32 vers = F.r_u32();
        if (vers == 0x0001)
        {
            m_info.minDist = F.r_float();
            m_info.maxDist = F.r_float();
            m_info.baseVolume = 1.f;
            m_info.gameType = F.r_u32();
            m_info.maxAIDist = m_info.maxDist;
        }
        else if (vers == 0x0002)
        {
            m_info.minDist = F.r_float();
            m_info.maxDist = F.r_float();
            m_info.baseVolume = F.r_float();
            m_info.gameType = F.r_u32();
            m_info.maxAIDist = m_info.maxDist;
        }
        else if (vers == OGG_COMMENT_VERSION)
        {
            m_info.minDist = F.r_float();
            m_info.maxDist = F.r_float();
            m_info.baseVolume = F.r_float();
            m_info.gameType = F.r_u32();
            m_info.maxAIDist = F.r_float();
        }
        else
        {
#ifndef MASTER_GOLD
            Log("! Invalid ogg-comment version, file: ", pName);
#endif
        }
    }
    else
    {
#ifndef MASTER_GOLD
        Log("! Missing ogg-comment, file: ", pName);
#endif
    }

    apply_description(fname.c_str(), m_info);

    R_ASSERT3_CURE(m_info.maxAIDist >= 0.1f && m_info.maxDist >= 0.1f, "Invalid max distance.", pName,
    {
        ov_clear(&ovf);
        return false;
    });

    ov_clear(&ovf);
    return true;
}

bool CSoundRender_Source::load(pcstr name)
{
    // Optional dialogue sounds use an empty name in the original game data.
    if (!name || !*name)
        return false;

    string_path fn, N;
    xr_strcpy(N, name);
#ifdef XR_PLATFORM_WINDOWS
    xr_strlwr(N);
#endif

    if (strext(N))
        *strext(N) = 0;

    fname = N;

    strconcat(fn, N, ".ogg");
    if (!FS.exist("$level$", fn))
        FS.update_path(fn, "$game_sounds$", fn);

#ifndef MASTER_GOLD
    if (!FS.exist(fn))
    {
        Msg("~ %s: Can't find sound '%s'", __FUNCTION__, name);
#   ifdef _EDITOR
        FS.update_path(fn, "$game_sounds$", "$no_sound.ogg");
#   endif
    }
#endif

    if (FS.exist(fn))
    {
        if (LoadWave(fn))
            return true;
    }

    return false;
}

void CSoundRender_Source::unload()
{
    fTimeTotal = 0.0f;
    dwBytesTotal = 0;
}
