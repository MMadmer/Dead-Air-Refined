#pragma once

class CUIXml;

class CMMSound
{
public:
    CMMSound();
    ~CMMSound();
    void Init(CUIXml& xml_doc, LPCSTR path);
    void whell_Play();
    void whell_Stop();
    void whell_Click();
    void whell_UpdateMoving(float frequency);

    void music_Play();
    void music_Stop();
    void music_Update();

    void all_Stop();

protected:
    IC bool check_file(LPCSTR fname);

    static constexpr size_t channels_count = 2;
    xr_array<ref_sound, channels_count> m_music;

    ref_sound m_whell;
    ref_sound m_whell_click;
    bool m_bRandom;
    struct Entry
    {
        xr_string name;
        float weight{1.f};
        float volume{1.f};
    };
    xr_vector<Entry> m_play_list;
    bool m_next_track{}; // a track that ends gives way to another
    bool m_silent{};     // the last try started nothing: the next one waits a second
    u32 m_next_try{};
};
