#include "StdAfx.h"

#include "MMSound.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "xms_screens.h"

CMMSound::CMMSound() {}
CMMSound::~CMMSound() { all_Stop(); }
void CMMSound::Init(CUIXml& xml_doc, LPCSTR path)
{
    string256 _path;
    m_bRandom = xml_doc.ReadAttribInt(path, 0, "random") ? true : false;

    int nodes_num = xml_doc.GetNodesNum(path, 0, "menu_music");

    XML_NODE tab_node = xml_doc.NavigateToNode(path, 0);
    xml_doc.SetLocalRoot(tab_node);
    const XmsScreens::Choice& choice = XmsScreens::CurrentMenu();
    m_play_list.clear();
    if (choice.game_music)
        for (int i = 0; i < nodes_num; ++i)
            m_play_list.push_back({ xml_doc.Read("menu_music", i, "") });
    for (const XmsScreens::Track& track : choice.tracks)
        m_play_list.push_back({ track.name, track.weight, track.volume });
    m_next_track = choice.next_track;
    xml_doc.SetLocalRoot(xml_doc.GetRoot());

    strconcat(sizeof(_path), _path, path, ":whell_sound");
    if (check_file(xml_doc.Read(_path, 0, "")))
        m_whell.create(xml_doc.Read(_path, 0, ""), st_Effect, sg_SourceType);

    strconcat(sizeof(_path), _path, path, ":whell_click");
    if (check_file(xml_doc.Read(_path, 0, "")))
        m_whell_click.create(xml_doc.Read(_path, 0, ""), st_Effect, sg_SourceType);
}

bool CMMSound::check_file(LPCSTR fname)
{
    string_path _path;
    strconcat(sizeof(_path), _path, fname, ".ogg");
    return FS.exist("$game_sounds$", _path) ? true : false;
}

void CMMSound::whell_Play()
{
    if (m_whell._handle() && !m_whell._feedback())
        m_whell.play(NULL, sm_Looped | sm_2D);
}

void CMMSound::whell_Stop()
{
    if (m_whell._feedback())
        m_whell.stop();
}

void CMMSound::whell_Click()
{
    if (m_whell_click._handle())
        m_whell_click.play(NULL, sm_2D);
}

void CMMSound::whell_UpdateMoving(float frequency) { m_whell.set_frequency(frequency); }
void CMMSound::music_Play()
{
    if (m_play_list.empty())
        return;

    float total = 0.f;
    for (const Entry& entry : m_play_list)
        total += entry.weight;
    float roll = Random.randF(total);
    const Entry* picked = &m_play_list.back();
    for (const Entry& entry : m_play_list)
    {
        roll -= entry.weight;
        if (roll <= 0.f)
        {
            picked = &entry;
            break;
        }
    }

    string_path stereo;
    strconcat(stereo, picked->name.c_str(), ".ogg");

    if (!m_music[0].create(stereo, st_Music, sg_SourceType))
    {
        string_path left, right;
        strconcat(left, picked->name.c_str(), "_l.ogg");
        strconcat(right, picked->name.c_str(), "_r.ogg");
        m_music[0].create(left, st_Effect, sg_Undefined);
        m_music[1].create(right, st_Effect, sg_Undefined);
    }

    const u32 flags = m_next_track ? sm_2D : sm_Looped | sm_2D;
    if (m_music[0] && m_music[1])
    {
        m_music[0].play_at_pos(nullptr, Fvector().set(-0.5f, 0.f, 0.3f), flags);
        m_music[1].play_at_pos(nullptr, Fvector().set(+0.5f, 0.f, 0.3f), flags);
        m_music[1].set_volume(picked->volume);
    }
    else
    {
        m_music[0].play(nullptr, flags);
        m_music[1].destroy();
    }
    m_music[0].set_volume(picked->volume);
}

void CMMSound::music_Update()
{
    // a menu pauses the game, so a list that moves on has to look regardless - a second apart, a sound that
    // will not start must not be tried every frame
    if (m_next_track)
    {
        if (Device.dwTimeContinual < m_next_try)
            return;
        m_next_try = Device.dwTimeContinual + 1000;
    }
    else if (Device.Paused())
        return;

    if (!m_music[0]._feedback() || (m_music[1]._handle() && !m_music[1]._feedback()))
        music_Play();
}

void CMMSound::music_Stop()
{
    for (ref_sound& channel : m_music)
        channel.stop();
}
void CMMSound::all_Stop()
{
    music_Stop();
    m_whell.stop();
    m_whell_click.stop();
}
