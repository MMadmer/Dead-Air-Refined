#include "StdAfx.h"
#include "player_hud.h"
#include "HudItem.h"
#include "xrUICore/ui_base.h"
#include "Actor.h"
#include "physic_item.h"
#include "static_cast_checked.hpp"
#include "ActorEffector.h"
#include "WeaponMagazinedWGrenade.h" // XXX: move somewhere
#include "GamePersistent.h"

player_hud* g_player_hud = nullptr;
extern ENGINE_API shared_str current_player_hud_sect;
extern ENGINE_API xr_vector<shared_str> g_player_hud_extra_omf;
extern ENGINE_API xr_vector<std::pair<shared_str, shared_str>> g_player_hud_extra_omf_variants;
extern ENGINE_API int g_player_hud_model_loading;
extern ENGINE_API xr_vector<std::pair<shared_str, shared_str>> g_player_hud_extra_omf_by_model;

// Scene knobs (console, console_commands.cpp): the scene item seat corrections laid over the
// section values, the seat slide speeds (in slow so the hand does not peck at the start, out
// fast because a finished cycle only has to return to the weapon) and the cycle trace.
Fvector g_hud_scene_item_pos_adj{};
Fvector g_hud_scene_item_rot_adj{};
float g_hud_scene_item_scale_adj = 1.f;
float g_hud_scene_seat_in = 2.5f;
float g_hud_scene_seat_out = 9.f;
int g_hud_scene_dbg = 0;

// Data-driven lists of extra hand-animation omfs. Loaded once; the render side appends them to
// every hands model created while the loading flag is raised. The 3D PDA set comes from its own
// config, the animation module's per-model sets from [da_hud_animations] in system.ltx.
static void fill_player_hud_extra_omf()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_pda3d.ltx");
    if (FS.exist(path))
    {
        CInifile ini(path, TRUE);
        if (ini.section_exist("player_hud_extra_omf"))
            for (const auto& [key, value] : ini.r_section("player_hud_extra_omf").Data)
                if (value.size())
                    g_player_hud_extra_omf.emplace_back(value);
        // Variants: "substring = path". A hands model whose path contains the substring gets
        // the matching file(s) INSTEAD of the base list - the exo rig family carries its own
        // bind pose and needs its own retargeted animations.
        if (ini.section_exist("player_hud_extra_omf_variants"))
            for (const auto& [key, value] : ini.r_section("player_hud_extra_omf_variants").Data)
                if (key.size() && value.size())
                    g_player_hud_extra_omf_variants.emplace_back(key, value);
    }
    if (pSettings->section_exist("da_hud_animations"))
        for (const auto& [key, value] : pSettings->r_section("da_hud_animations").Data)
            if (key.size() && value.size())
                g_player_hud_extra_omf_by_model.emplace_back(key, value);
    if (!g_player_hud_extra_omf.empty() || !g_player_hud_extra_omf_by_model.empty())
        Msg("* [hud-anim] %u extra hud omf(s) registered (+%u variant(s), %u per-model set(s))",
            u32(g_player_hud_extra_omf.size()), u32(g_player_hud_extra_omf_variants.size()),
            u32(g_player_hud_extra_omf_by_model.size()));
}


// --#SM+# Begin--
constexpr float PITCH_OFFSET_R    = 0.0f;   // Насколько сильно ствол смещается вбок (влево) при вертикальных поворотах камеры
constexpr float PITCH_OFFSET_N    = 0.0f;   // Насколько сильно ствол поднимается\опускается при вертикальных поворотах камеры
constexpr float PITCH_OFFSET_D    = 0.02f;  // Насколько сильно ствол приближается\отдаляется при вертикальных поворотах камеры
constexpr float PITCH_LOW_LIMIT   = -PI;    // Минимальное значение pitch при использовании совместно с PITCH_OFFSET_N
constexpr float ORIGIN_OFFSET     = -0.05f; // Фактор влияния инерции на положение ствола (чем меньше, тем масштабней инерция)
constexpr float ORIGIN_OFFSET_AIM = -0.03f; // (Для прицеливания)
constexpr float TENDTO_SPEED      = 5.f;    // Скорость нормализации положения ствола
constexpr float TENDTO_SPEED_AIM  = 8.f;    // (Для прицеливания)
// --#SM+# End--

float CalcMotionSpeed(const shared_str& anim_name, const float anim_speed)
{
    // Apply custom animation speeds / configuration only for singleplayer games.
    // Fast reloading / showing / hiding animation does not seem fair.
    if (IsGameTypeSingle())
        return anim_speed;
    else
        return (anim_name == "anm_show" || anim_name == "anm_hide") ? 2.0f : 1.0f;
}

CBlend* PlayHudCycle(
    IKinematicsAnimated& model, const u16 part, const MotionID motion, const BOOL mix_in, const float speed_scale)
{
    CMotionDef* const motion_def = model.LL_GetMotionDef(motion);
    if (!motion_def)
        Msg("! PlayHudCycle: dangling motion id slot=%u idx=%u, hands sect [%s]", u32(motion.slot),
            u32(motion.idx), current_player_hud_sect.c_str());
    R_ASSERT(motion_def);
    return model.LL_PlayCycle(part, motion, mix_in, motion_def->Accrue(), motion_def->Falloff(),
        motion_def->Speed() * speed_scale, motion_def->StopAtEnd(), nullptr, nullptr);
}

const player_hud_motion* player_hud_motion_container::find_motion(const shared_str& name) const
{
    const auto it = m_anims.find(name);
    return it != m_anims.end() ? &it->second : nullptr;
}

void player_hud_motion_container::load(IKinematicsAnimated* model, const shared_str& sect, bool lenient)
{
    const CInifile::Sect& _sect = pSettings->r_section(sect);

    for (const auto& [name, anm] : _sect.Data)
    {
        if (0 == strncmp(name.c_str(), "anm_", sizeof("anm_") - 1))
        {
            player_hud_motion pm;

            if (_GetItemCount(anm.c_str()) == 1)
            {
                pm.m_base_name = anm;
                pm.m_additional_name = anm;
                pm.m_anim_speed = 1.f;
            }
            else
            {
                R_ASSERT2(_GetItemCount(anm.c_str()) <= 3, anm.c_str());
                string512 str_item;
                _GetItem(anm.c_str(), 0, str_item);
                pm.m_base_name = str_item;

                _GetItem(anm.c_str(), 1, str_item);
                pm.m_additional_name = xr_strlen(str_item) > 0 ? str_item : pm.m_base_name;

                _GetItem(anm.c_str(), 2, str_item);
                pm.m_anim_speed = xr_strlen(str_item) > 0 ? atof(str_item) : 1.f;
            }

            // and load all motions for it
            for (u32 i = 0; i <= 8; ++i)
            {
                string512 buff;
                if (i == 0)
                    xr_strcpy(buff, pm.m_base_name.c_str());
                else
                    xr_sprintf(buff, "%s%d", pm.m_base_name.c_str(), i);

                MotionID motion_ID = model->ID_Cycle_Safe(buff);
                if (motion_ID.valid())
                {
                    pm.m_animations.emplace_back(motion_descr{ std::move(motion_ID), buff });
#ifdef DEBUG
//					Msg(" alias=[%s] base=[%s] name=[%s]",pm.m_alias_name.c_str(), pm.m_base_name.c_str(), buff);
#endif // #ifdef DEBUG
                }
            }
            if (pm.m_animations.empty() && lenient)
            {
                Msg("! [hud-scene] motion [%s] of [%s] is not in the hands model, skipped",
                    pm.m_base_name.c_str(), sect.c_str());
                continue;
            }
            R_ASSERT2(!pm.m_animations.empty(), make_string("motion not found [%s]", pm.m_base_name.c_str()).c_str());

            m_anims.emplace(name, std::move(pm));
        }
    }
}

Fvector& attachable_hud_item::hands_attach_pos() { return m_measures.m_hands_attach[0]; }
Fvector& attachable_hud_item::hands_attach_rot() { return m_measures.m_hands_attach[1]; }

Fvector& attachable_hud_item::hands_offset_pos()
{
    const u8 idx = m_parent_hud_item->GetCurrentHudOffsetIdx();
    return m_measures.m_hands_offset[0][idx];
}

Fvector& attachable_hud_item::hands_offset_rot()
{
    u8 idx = m_parent_hud_item->GetCurrentHudOffsetIdx();
    return m_measures.m_hands_offset[1][idx];
}

void attachable_hud_item::set_bone_visible(const shared_str& bone_name, BOOL bVisibility, BOOL bSilent)
{
    const u16 bone_id = m_model->LL_BoneID(bone_name);
    if (bone_id == BI_NONE)
    {
        if (bSilent)
            return;
        R_ASSERT2(false, make_string("model [%s] has no bone [%s]", m_visual_name.c_str(), bone_name.c_str()).c_str());
    }
    const BOOL bVisibleNow = m_model->LL_GetBoneVisible(bone_id);
    if (bVisibleNow != bVisibility)
        m_model->LL_SetBoneVisible(bone_id, bVisibility, TRUE);
}

void attachable_hud_item::update(bool bForce)
{
    if (bForce || m_upd_firedeps_frame != Device.dwFrame)
    {
        const bool is_16x9 = UICore::is_widescreen();

        if (m_measures.m_prop_flags.test(hud_item_measures::e_16x9_mode_now) != is_16x9)
            reload_measures();

        if (GamePersistent().GetHudTuner().is_active())
            m_measures.update(m_attach_offset);

        m_parent->calc_transform(m_attach_place_idx, m_attach_offset, m_item_transform);
        m_upd_firedeps_frame = Device.dwFrame;
    }

    if (IKinematicsAnimated* ka = m_model->dcast_PKinematicsAnimated())
    {
        ka->UpdateTracks();
        // Reference order: the tracks just advanced, a pose published earlier this frame is
        // stale - without the invalidation the attached item lags the hands it hangs on.
        ka->dcast_PKinematics()->CalculateBones_Invalidate();
        ka->dcast_PKinematics()->CalculateBones(TRUE);
    }
}

void attachable_hud_item::update_hud_additional(Fmatrix& trans) const
{
    if (m_parent_hud_item)
    {
        m_parent_hud_item->UpdateHudAdditonal(trans);
    }
}

void attachable_hud_item::setup_firedeps(firedeps& fd)
{
    update(false);
    // fire point&direction
    if (m_measures.m_prop_flags.test(hud_item_measures::e_fire_point))
    {
        Fmatrix& fire_mat = m_model->LL_GetTransform(m_measures.m_fire_bone);
        fire_mat.transform_tiny(fd.vLastFP, m_measures.m_fire_point_offset);
        m_item_transform.transform_tiny(fd.vLastFP);

        fd.vLastFD.set(0.f, 0.f, 1.f);
        m_item_transform.transform_dir(fd.vLastFD);
        VERIFY(_valid(fd.vLastFD));
        VERIFY(_valid(fd.vLastFD));

        fd.m_FireParticlesXForm.identity();
        fd.m_FireParticlesXForm.k.set(fd.vLastFD);
        Fvector::generate_orthonormal_basis_normalized(
            fd.m_FireParticlesXForm.k, fd.m_FireParticlesXForm.j, fd.m_FireParticlesXForm.i);
        VERIFY(_valid(fd.m_FireParticlesXForm));
    }

    if (m_measures.m_prop_flags.test(hud_item_measures::e_fire_point2))
    {
        Fmatrix& fire_mat = m_model->LL_GetTransform(m_measures.m_fire_bone2);
        fire_mat.transform_tiny(fd.vLastFP2, m_measures.m_fire_point2_offset);
        m_item_transform.transform_tiny(fd.vLastFP2);
        VERIFY(_valid(fd.vLastFP2));
        VERIFY(_valid(fd.vLastFP2));
    }

    if (m_measures.m_prop_flags.test(hud_item_measures::e_shell_point))
    {
        Fmatrix& fire_mat = m_model->LL_GetTransform(m_measures.m_shell_bone);
        fire_mat.transform_tiny(fd.vLastSP, m_measures.m_shell_point_offset);
        m_item_transform.transform_tiny(fd.vLastSP);
        VERIFY(_valid(fd.vLastSP));
        VERIFY(_valid(fd.vLastSP));
    }
}

bool attachable_hud_item::need_renderable() const { return m_parent_hud_item->need_renderable(); }

void attachable_hud_item::render(u32 context_id, IRenderable* root)
{
    GEnv.Render->add_Visual(context_id, root, m_model->dcast_RenderVisual(), m_item_transform);
    m_parent_hud_item->render_hud_mode();
}

bool attachable_hud_item::render_item_ui_query() const { return m_parent_hud_item->render_item_3d_ui_query(); }
void attachable_hud_item::render_item_ui() const { m_parent_hud_item->render_item_3d_ui(); }

Fmatrix hud_item_measures::load(const shared_str& sect_name, IKinematics* K)
{
    const bool is_16x9 = UICore::is_widescreen();
    string64 _prefix;
    xr_sprintf(_prefix, "%s", is_16x9 ? "_16x9" : "");
    string128 val_name;

    strconcat(val_name, "hands_position", _prefix);
    m_hands_attach[0] = pSettings->r_fvector3(sect_name, val_name);
    strconcat(val_name, "hands_orientation", _prefix);
    m_hands_attach[1] = pSettings->r_fvector3(sect_name, val_name);

    m_item_attach[0] = pSettings->r_fvector3(sect_name, "item_position");
    m_item_attach[1] = pSettings->r_fvector3(sect_name, "item_orientation");

    Fmatrix attach_offset;
    update(attach_offset);

    shared_str bone_name;
    m_prop_flags.set(e_fire_point, pSettings->line_exist(sect_name, "fire_bone"));
    if (m_prop_flags.test(e_fire_point))
    {
        bone_name = pSettings->r_string(sect_name, "fire_bone");
        m_fire_bone = K->LL_BoneID(bone_name);
        m_fire_point_offset = pSettings->r_fvector3(sect_name, "fire_point");
    }
    else
        m_fire_point_offset = {};

    m_prop_flags.set(e_fire_point2, pSettings->line_exist(sect_name, "fire_bone2"));
    if (m_prop_flags.test(e_fire_point2))
    {
        bone_name = pSettings->r_string(sect_name, "fire_bone2");
        m_fire_bone2 = K->LL_BoneID(bone_name);
        m_fire_point2_offset = pSettings->r_fvector3(sect_name, "fire_point2");
    }
    else
        m_fire_point2_offset = {};

    m_prop_flags.set(e_shell_point, pSettings->line_exist(sect_name, "shell_bone"));
    if (m_prop_flags.test(e_shell_point))
    {
        bone_name = pSettings->r_string(sect_name, "shell_bone");
        m_shell_bone = K->LL_BoneID(bone_name);
        m_shell_point_offset = pSettings->r_fvector3(sect_name, "shell_point");
    }
    else
        m_shell_point_offset = {};

    m_hands_offset[0][0] = {};
    m_hands_offset[1][0] = {};

    strconcat(val_name, "aim_hud_offset_pos", _prefix);
    m_hands_offset[0][1] = pSettings->r_fvector3(sect_name, val_name);
    strconcat(val_name, "aim_hud_offset_rot", _prefix);
    m_hands_offset[1][1] = pSettings->r_fvector3(sect_name, val_name);

    strconcat(val_name, "gl_hud_offset_pos", _prefix);
    m_hands_offset[0][2] = pSettings->r_fvector3(sect_name, val_name);
    strconcat(val_name, "gl_hud_offset_rot", _prefix);
    m_hands_offset[1][2] = pSettings->r_fvector3(sect_name, val_name);

    R_ASSERT2(pSettings->line_exist(sect_name, "fire_point") == pSettings->line_exist(sect_name, "fire_bone"),
        sect_name.c_str());
    R_ASSERT2(pSettings->line_exist(sect_name, "fire_point2") == pSettings->line_exist(sect_name, "fire_bone2"),
        sect_name.c_str());
    R_ASSERT2(pSettings->line_exist(sect_name, "shell_point") == pSettings->line_exist(sect_name, "shell_bone"),
        sect_name.c_str());

    load_inertion_params(sect_name);
    m_prop_flags.set(e_16x9_mode_now, is_16x9);

    return attach_offset;
}

Fmatrix hud_item_measures::load_monolithic(const shared_str& sect_name, IKinematics* K, CHudItem* owner)
{
    m_item_attach[0] = pSettings->r_fvector3(sect_name, "position");
    m_item_attach[1] = pSettings->r_fvector3(sect_name, "orientation");

    Fmatrix attach_offset;
    update(attach_offset);

    // fire bone
    if (auto* wpn = smart_cast<CWeapon*>(owner))
    {
        cpcstr fire_bone = pSettings->r_string(sect_name, "fire_bone");
        m_fire_bone = K->LL_BoneID(fire_bone);
        if (m_fire_bone >= K->LL_BoneCount())
            xrDebug::Fatal(DEBUG_INFO, "There is no '%s' bone for weapon '%s'.", fire_bone, sect_name.c_str());
        m_fire_bone2 = m_fire_bone;
        m_shell_bone = m_fire_bone;

        m_fire_point_offset = pSettings->r_fvector3(sect_name, "fire_point");
        m_fire_point2_offset = pSettings->read_if_exists<Fvector3>(sect_name, "fire_point2", m_fire_point_offset);

        if (pSettings->line_exist(owner->object().cNameSect(), "shell_particles"))
            m_shell_point_offset = pSettings->r_fvector3(sect_name, "shell_point");
        else
            m_shell_point_offset.set(0, 0, 0);

        m_hands_offset[0][0] = {};
        m_hands_offset[1][0] = {};

        if (wpn->IsZoomEnabled())
        {
            const auto load_zoom_offsets = [&](pcstr prefix, Fvector3& position, Fvector3& rotation)
            {
                string256 full_name;
                position = pSettings->r_fvector3(sect_name, strconcat(full_name, prefix, "zoom_offset"));
                rotation.x = pSettings->r_float(sect_name, strconcat(full_name, prefix, "zoom_rotate_x"));
                rotation.y = pSettings->r_float(sect_name, strconcat(full_name, prefix, "zoom_rotate_y"));
                rotation.z = pSettings->read_if_exists<float>(sect_name, strconcat(full_name, prefix, "zoom_rotate_z"), 0.f);
            };
            load_zoom_offsets("", m_hands_offset[0][1], m_hands_offset[1][1]);
            if (smart_cast<CWeaponMagazinedWGrenade*>(wpn))
            {
                load_zoom_offsets("grenade_", m_hands_offset[0][2], m_hands_offset[1][2]);
                if (wpn->GrenadeLauncherAttachable())
                    load_zoom_offsets("grenade_normal_", m_hands_offset[0][1], m_hands_offset[1][1]);
            }
        }
    }
    else
    {
        m_fire_bone  = BI_NONE;
        m_fire_bone2 = BI_NONE;
        m_shell_bone = BI_NONE;

        m_fire_point_offset  = {};
        m_fire_point2_offset = {};
        m_shell_point_offset = {};
    }

    load_inertion_params(sect_name);
    m_prop_flags.set(e_16x9_mode_now, UICore::is_widescreen());

    return attach_offset;
}

void hud_item_measures::load_inertion_params(const shared_str& sect_name)
{
    //Загрузка параметров инерции --#SM+# Begin--
    m_inertion_params.m_pitch_offset_r = READ_IF_EXISTS(pSettings, r_float, sect_name, "pitch_offset_right", PITCH_OFFSET_R);
    m_inertion_params.m_pitch_offset_n = READ_IF_EXISTS(pSettings, r_float, sect_name, "pitch_offset_up", PITCH_OFFSET_N);
    m_inertion_params.m_pitch_offset_d = READ_IF_EXISTS(pSettings, r_float, sect_name, "pitch_offset_forward", PITCH_OFFSET_D);
    m_inertion_params.m_pitch_low_limit = READ_IF_EXISTS(pSettings, r_float, sect_name, "pitch_offset_up_low_limit", PITCH_LOW_LIMIT);

    m_inertion_params.m_origin_offset = READ_IF_EXISTS(pSettings, r_float, sect_name, "inertion_origin_offset", ORIGIN_OFFSET);
    m_inertion_params.m_origin_offset_aim = READ_IF_EXISTS(pSettings, r_float, sect_name, "inertion_origin_aim_offset", ORIGIN_OFFSET_AIM);
    m_inertion_params.m_tendto_speed = READ_IF_EXISTS(pSettings, r_float, sect_name, "inertion_tendto_speed", TENDTO_SPEED);
    m_inertion_params.m_tendto_speed_aim = READ_IF_EXISTS(pSettings, r_float, sect_name, "inertion_tendto_aim_speed", TENDTO_SPEED_AIM);
    //--#SM+# End--
}

void hud_item_measures::update(Fmatrix& attach_offset)
{
    Fvector ypr = m_item_attach[1];
    ypr.mul(PI / 180.f);
    attach_offset.setHPB(ypr.x, ypr.y, ypr.z);
    attach_offset.translate_over(m_item_attach[0]);
}

attachable_hud_item::~attachable_hud_item()
{
    IRenderVisual* v = m_model->dcast_RenderVisual();
    GEnv.Render->model_Delete(v);
}

attachable_hud_item::attachable_hud_item(player_hud* parent, const shared_str& sect_name, IKinematicsAnimated* hands_model)
    : m_parent(parent), m_sect_name(sect_name)
{
    // Visual
    if (pSettings->line_exist(m_sect_name, "item_visual"))
    {
        m_monolithic = false;
        m_visual_name = pSettings->r_string(m_sect_name, "item_visual");
    }
    else if (pSettings->line_exist(m_sect_name, "visual"))
    {
        m_monolithic = true;
        m_visual_name = pSettings->r_string(m_sect_name, "visual");
    }
    R_ASSERT3(!m_visual_name.empty(), "Missing 'item_visual' from weapon hud section.", m_sect_name.c_str());

    m_model = smart_cast<IKinematics*>(GEnv.Render->model_Create(m_visual_name.c_str()));

    m_attach_place_idx = pSettings->read_if_exists<u16>(m_sect_name, "attach_place_idx", 0);

    IKinematicsAnimated* animatedHudItem;
    if (!m_monolithic && hands_model)
        animatedHudItem = hands_model;
    else
        animatedHudItem = smart_cast<IKinematicsAnimated*>(m_model);

    m_hand_motions.load(animatedHudItem, m_sect_name);
    reload_measures();
}

void attachable_hud_item::reload_measures()
{
    if (m_monolithic)
        m_attach_offset = m_measures.load_monolithic(m_sect_name, m_model, m_parent_hud_item);
    else
        m_attach_offset = m_measures.load(m_sect_name, m_model);
}

u32 attachable_hud_item::anim_play(const shared_str& anm_name_b, BOOL bMixIn, const CMotionDef*& md, u8& rnd_idx)
{
    string256 anim_name_r;
    const bool is_16x9 = UICore::is_widescreen();
    xr_sprintf(anim_name_r, "%s%s", anm_name_b.c_str(), m_attach_place_idx == 1 && is_16x9 ? "_16x9" : "");

    const player_hud_motion* anm = m_hand_motions.find_motion(anim_name_r);
    R_ASSERT2(anm, make_string("model [%s] has no motion alias defined [%s]", m_sect_name.c_str(), anim_name_r).c_str());
    R_ASSERT2(anm->m_animations.size(), make_string("model [%s] has no motion defined in motion_alias [%s]",
                                            m_visual_name.c_str(), anim_name_r)
                                            .c_str());

    // Draw and holster animations run at their configured speed. Scaling them by the item's
    // control inertion made heavy weapons slow to raise and, with that, slow to fire; the
    // balance of that Dead Air 1.0 mechanic does not hold up, so it is not applied here.
    const float speed = CalcMotionSpeed(anm->m_base_name, anm->m_anim_speed);

    rnd_idx = (u8)Random.randI(anm->m_animations.size());
    const motion_descr& M = anm->m_animations[rnd_idx];

    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(m_model);

    if (g_hud_scene_dbg)
        Msg("~ [hud-scene] item [%s] plays [%s] (place %u, mix %d, scene %s)", m_sect_name.c_str(), anim_name_r,
            u32(m_attach_place_idx), int(bMixIn), m_parent->scene_active() ? "ON" : "off");

    const u32 ret = m_parent->anim_play(m_attach_place_idx, M.mid, bMixIn, md, speed, m_monolithic ? ka : nullptr);

    if (ka)
    {
        shared_str item_anm_name;
        if (anm->m_base_name != anm->m_additional_name)
            item_anm_name = anm->m_additional_name;
        else
            item_anm_name = M.name;

        MotionID M2 = ka->ID_Cycle_Safe(item_anm_name);
        if (!M2.valid())
            M2 = ka->ID_Cycle_Safe("idle");
        else if (bDebug)
            Msg("playing item animation [%s]", item_anm_name.c_str());

        R_ASSERT3(M2.valid(), "model has no motion [idle] ", m_visual_name.c_str());

        if (!m_monolithic)
        {
            const u16 root_id = m_model->LL_GetBoneRoot();
            CBoneInstance& root_binst = m_model->LL_GetBoneInstance(root_id);
            root_binst.set_callback_overwrite(TRUE);
            root_binst.mTransform.identity();
        }

        const u16 pc = ka->partitions().count();
        for (u16 pid = 0; pid < pc; ++pid)
        {
            CBlend* B = PlayHudCycle(*ka, pid, M2, bMixIn, speed);
            R_ASSERT(B);
        }

        m_model->CalculateBones_Invalidate();
    }

    R_ASSERT2(m_parent_hud_item, "parent hud item is NULL");
    CPhysicItem& parent_object = m_parent_hud_item->object();
    // R_ASSERT2		(parent_object, "object has no parent actor");
    // IGameObject*		parent_object = static_cast_checked<IGameObject*>(&m_parent_hud_item->object());

    if (IsGameTypeSingle() && parent_object.H_Parent() == Level().CurrentControlEntity())
    {
        CActor* current_actor = static_cast_checked<CActor*>(Level().CurrentControlEntity());
        VERIFY(current_actor);

        string_path ce_path;
        string_path anm_name;
        strconcat(anm_name, "camera_effects" DELIMITER "weapon" DELIMITER, M.name.c_str(), ".anm");
        if (FS.exist(ce_path, "$game_anims$", anm_name))
        {
            CEffectorCam* ec = current_actor->Cameras().GetCamEffector(eCEWeaponAction);
            if (ec)
                current_actor->Cameras().RemoveCamEffector(eCEWeaponAction);

            CAnimatorCamEffector* e = xr_new<CAnimatorCamEffector>();
            e->SetType(eCEWeaponAction);
            e->SetHudAffect(false);
            e->SetCyclic(false);
            e->Start(anm_name);
            current_actor->Cameras().AddCamEffector(e);
        }
    }
    return ret;
}

player_hud::~player_hud()
{
    scene_item_release();

    if (m_model)
    {
        IRenderVisual* v = m_model->dcast_RenderVisual();
        GEnv.Render->model_Delete(v);
    }

    if (m_model_2)
    {
        IRenderVisual* v = m_model_2->dcast_RenderVisual();
        GEnv.Render->model_Delete(v);
    }

    for (auto& [name, item] : m_pool)
    {
        xr_delete(item);
    }
    m_pool.clear();
}

void player_hud::load(const shared_str& player_hud_sect)
{
    if (player_hud_sect == m_sect_name)
        return;

    m_sect_name = player_hud_sect;

    const bool b_reload = m_model != nullptr;
    if (m_model)
    {
        IRenderVisual* v = m_model->dcast_RenderVisual();
        GEnv.Render->model_Delete(v);
    }

    if (m_model_2)
    {
        IRenderVisual* v = m_model_2->dcast_RenderVisual();
        GEnv.Render->model_Delete(v);
        m_model_2 = nullptr;
    }

    if (!pSettings->section_exist(m_sect_name))
    {
        if (b_reload)
        {
            if (m_attached_items[1])
                m_attached_items[1]->m_parent_hud_item->on_a_hud_attach();

            if (m_attached_items[0])
                m_attached_items[0]->m_parent_hud_item->on_a_hud_attach();
        }

        return;
    }

    const shared_str& model_name = pSettings->r_string(m_sect_name, "visual");
    fill_player_hud_extra_omf();
    // Both copies are created under the loading flag: the extra motion sets must reach the
    // left half as well, or a scene cycle exists on one hand only.
    g_player_hud_model_loading = 1;
    m_model = smart_cast<IKinematicsAnimated*>(GEnv.Render->model_Create(model_name.c_str()));
    m_model_2 = smart_cast<IKinematicsAnimated*>(GEnv.Render->model_Create(model_name.c_str()));
    g_player_hud_model_loading = 0;

    // Same model, same bone and motion ids: a cycle found through one copy plays on the other.
    // The clavicle name differs between rigs (l_clavicle, bip01_l_clavicle) - both are tried,
    // and a miss is said out loud: without the hiding the arms double on screen.
    if (m_model && m_model_2)
    {
        const auto hide_arm = [&](IKinematicsAnimated* model, pcstr n1, pcstr n2, pcstr side)
        {
            IKinematics* k = model->dcast_PKinematics();
            u16 id = k->LL_BoneID(n1);
            if (id == BI_NONE)
                id = k->LL_BoneID(n2);
            if (id == BI_NONE)
            {
                Msg("! [hud-scene] hands [%s]: no %s clavicle bone (%s / %s), the arms will double",
                    model_name.c_str(), side, n1, n2);
                return;
            }
            k->LL_SetBoneVisible(id, FALSE, TRUE);
        };
        hide_arm(m_model, "l_clavicle", "bip01_l_clavicle", "left");
        hide_arm(m_model_2, "r_clavicle", "bip01_r_clavicle", "right");
    }

    load_ancors();
    // Msg("hands visual changed to [%s] [%s] [%s]", model_name.c_str(), b_reload ? "R" : "", m_attached_items[0] ? "Y" : "");

    if (!b_reload)
    {
        m_model->PlayCycle("hand_idle_doun");
        if (m_model_2)
            m_model_2->PlayCycle("hand_idle_doun");
    }
    else
    {
        if (m_attached_items[1])
            m_attached_items[1]->m_parent_hud_item->on_a_hud_attach();

        if (m_attached_items[0])
            m_attached_items[0]->m_parent_hud_item->on_a_hud_attach();
    }
    m_model->dcast_PKinematics()->CalculateBones_Invalidate();
    m_model->dcast_PKinematics()->CalculateBones(TRUE);
    if (m_model_2)
    {
        m_model_2->dcast_PKinematics()->CalculateBones_Invalidate();
        m_model_2->dcast_PKinematics()->CalculateBones(TRUE);
    }
}

Fvector player_hud::attach_pos(u8 part) const
{
    if (m_attached_items[part])
        return m_attached_items[part]->hands_attach_pos();
    if (m_attached_items[part ? 0 : 1])
        return m_attached_items[part ? 0 : 1]->hands_attach_pos();
    return m_scene_hands_pos;
}

Fvector player_hud::attach_rot(u8 part) const
{
    if (m_attached_items[part])
        return m_attached_items[part]->hands_attach_rot();
    if (m_attached_items[part ? 0 : 1])
        return m_attached_items[part ? 0 : 1]->hands_attach_rot();
    return m_scene_hands_rot;
}

void player_hud::load_ancors()
{
    const CInifile::Sect& _sect = pSettings->r_section(m_sect_name);
    for (const auto& [name, bone] : _sect.Data)
    {
        if (0 == strncmp(name.c_str(), "ancor_", sizeof("ancor_") - 1))
        {
            m_ancors.emplace_back(m_model->dcast_PKinematics()->LL_BoneID(bone));
        }
    }
}

bool player_hud::render_item_ui_query() const
{
    bool res = false;
    if (m_attached_items[0])
        res |= m_attached_items[0]->render_item_ui_query();

    if (m_attached_items[1])
        res |= m_attached_items[1]->render_item_ui_query();

    return res;
}

void player_hud::render_item_ui() const
{
    if (m_attached_items[0])
        m_attached_items[0]->render_item_ui();

    if (m_attached_items[1])
        m_attached_items[1]->render_item_ui();
}

void player_hud::render_hud(u32 context_id, IRenderable* root)
{
    attachable_hud_item* item0 = m_attached_items[0];
    attachable_hud_item* item1 = m_attached_items[1];

    // A scene draws the hands by itself: it parks the weapon first and then asks for a cycle, so
    // "no item, nothing to draw" would leave a playing scene invisible.
    const bool scene = scene_active();

    if (!item0 && !item1 && !scene)
        return;

    const bool b_r0 = item0 && item0->need_renderable();
    const bool b_r1 = item1 && item1->need_renderable();

    if (!b_r0 && !b_r1 && !scene)
        return;

    if (m_model)
        GEnv.Render->add_Visual(context_id, root, m_model->dcast_RenderVisual(), m_transform);

    if (m_model_2)
        GEnv.Render->add_Visual(context_id, root, m_model_2->dcast_RenderVisual(), m_transform_2);

    // A two-hand scene owns the hands whole: the weapon is put away logically but its hud item
    // stays attached, and drawing it would put the knife over the scene. A one-hand scene keeps
    // the weapon in the other hand, so it is drawn. need_renderable is not the test - it is
    // about the scope zoom, not about a holstered item.
    if (!scene || m_scene_one_hand)
    {
        if (item0)
            item0->render(context_id, root);

        if (item1)
            item1->render(context_id, root);
    }

    if (m_scene_item_visual && scene)
        GEnv.Render->add_Visual(context_id, root, m_scene_item_visual, m_scene_item_transform);
}

void player_hud::render_shadow(u32 context_id, IRenderable* root)
{
    attachable_hud_item* item0 = m_attached_items[0];
    attachable_hud_item* item1 = m_attached_items[1];
    const bool scene = scene_active();

    if (!item0 && !item1 && !scene)
        return;

    const bool b_r0 = item0 && item0->need_renderable();
    const bool b_r1 = item1 && item1->need_renderable();

    if (!b_r0 && !b_r1 && !scene)
        return;

    // Visuals only, with the same transforms the main pass uses. attachable_hud_item::render
    // is deliberately not reused: it also runs render_hud_mode(), which registers the item's
    // light, and that must happen once per frame, not once per pass.
    if (m_model)
        GEnv.Render->add_Visual(context_id, root, m_model->dcast_RenderVisual(), m_transform);

    if (m_model_2)
        GEnv.Render->add_Visual(context_id, root, m_model_2->dcast_RenderVisual(), m_transform_2);

    if (!scene || m_scene_one_hand)
    {
        if (item0 && item0->m_model)
            GEnv.Render->add_Visual(context_id, root, item0->m_model->dcast_RenderVisual(), item0->m_item_transform);

        if (item1 && item1->m_model)
            GEnv.Render->add_Visual(context_id, root, item1->m_model->dcast_RenderVisual(), item1->m_item_transform);
    }

    if (m_scene_item_visual && scene)
        GEnv.Render->add_Visual(context_id, root, m_scene_item_visual, m_scene_item_transform);
}

#include "xrCore/Animation/Motion.hpp"

u32 player_hud::motion_length(const shared_str& anim_name, const shared_str& hud_name, const CMotionDef*& md)
{
    const float speed = CalcMotionSpeed(anim_name, 1.0f);
    attachable_hud_item* pi = create_hud_item(hud_name);
    const player_hud_motion* pm = pi->m_hand_motions.find_motion(anim_name);

    if (!pm)
        return 100; // ms TEMPORARY
    R_ASSERT2(pm,
        make_string("hudItem model [%s] has no motion with alias [%s]", hud_name.c_str(), anim_name.c_str()).c_str());
    IKinematicsAnimated* model = pi->m_monolithic ? smart_cast<IKinematicsAnimated*>(pi->m_model) : nullptr;
    return motion_length(pm->m_animations[0].mid, md, speed, model);
}

u32 player_hud::motion_length(const MotionID& M, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel) const
{
    IKinematicsAnimated* model = itemModel ? itemModel : m_model;
    md = model->LL_GetMotionDef(M);
    VERIFY(md);
    if (md->flags & esmStopAtEnd)
    {
        CMotion* motion = model->LL_GetRootMotion(M);
        return iFloor(0.5f + 1000.f * motion->GetLength() / (md->Dequantize(md->speed) * speed));
    }
    return 0;
}

void player_hud::update(const Fmatrix& cam_trans)
{
    Fmatrix trans = cam_trans;
    if (psHUD_Flags.test(HUD_LEFT_HANDED))
    {
        // faster than multiplication by flip matrix
        trans.m[0][0] = -trans.m[0][0];
        trans.m[0][1] = -trans.m[0][1];
        trans.m[0][2] = -trans.m[0][2];
        trans.m[0][3] = -trans.m[0][3];
    }

    update_inertion(trans);

    attachable_hud_item* item0 = m_attached_items[0];
    attachable_hud_item* item1 = m_attached_items[1];

    // Each item's additions (sway, recoil) go to its own half: the weapon drives the right
    // matrix, the left-hand item the left one. trans_b is the state before any addition - the
    // hand a scene owns returns to it.
    const Fmatrix trans_b = trans;
    Fmatrix trans_2 = trans;

    if (item0)
        item0->update_hud_additional(trans);
    if (item1)
        item1->update_hud_additional(trans_2);

    if (item0 && !item1)
        trans_2 = trans;
    else if (item1 && !item0)
        trans = trans_2;

    const bool monolithic = item0 && item0->m_monolithic || item1 && item1->m_monolithic;
    if (!m_model || monolithic)
    {
        m_transform = trans;
        m_transform_2 = trans_2;
    }
    else
    {
        Fvector m1pos = attach_pos(0), m2pos = attach_pos(1);
        Fvector m1rot = attach_rot(0), m2rot = attach_rot(1);

        const bool scene = scene_active();

        if (scene && (m_scene_hand == 2 || (!item0 && !item1)))
        {
            // The scene owns both hands (or nothing is held): the seat is the scene's.
            m1pos = m2pos = m_scene_hands_pos;
            m1rot = m2rot = m_scene_hands_rot;
            trans = trans_b;
            trans_2 = trans_b;
        }
        else if (m_scene_seat_k > 0.f)
        {
            // One-hand scene: the owned hand slides to the scene seat, the other stays on its
            // item. The factor itself is driven at the end of update.
            const bool right = (m_scene_hand_seat == 0);
            Fvector& hp = right ? m1pos : m2pos;
            Fvector& hr = right ? m1rot : m2rot;
            hp.lerp(hp, m_scene_hands_pos, m_scene_seat_k);
            hr.lerp(hr, m_scene_hands_rot, m_scene_seat_k);

            Fmatrix tb = trans_b;
            if (right)
            {
                tb.inertion(trans, m_scene_seat_k);
                trans = tb;
            }
            else
            {
                tb.inertion(trans_2, m_scene_seat_k);
                trans_2 = tb;
            }
        }

        m1rot.mul(PI / 180.f);
        m_attach_offset.setHPB(m1rot.x, m1rot.y, m1rot.z);
        m_attach_offset.translate_over(m1pos);

        m2rot.mul(PI / 180.f);
        m_attach_offset_2.setHPB(m2rot.x, m2rot.y, m2rot.z);
        m_attach_offset_2.translate_over(m2pos);

        m_transform.mul(trans, m_attach_offset);
        m_transform_2.mul(trans_2, m_attach_offset_2);

        m_model->UpdateTracks();
        m_model->dcast_PKinematics()->CalculateBones_Invalidate();
        m_model->dcast_PKinematics()->CalculateBones(TRUE);

        if (m_model_2)
        {
            m_model_2->UpdateTracks();
            m_model_2->dcast_PKinematics()->CalculateBones_Invalidate();
            m_model_2->dcast_PKinematics()->CalculateBones(TRUE);
        }
    }

    // The scene item moves here, once per frame, right after the hands: the grip bone is fresh.
    // Not in render_hud - that runs once per context and the animation would run ahead.
    if (m_scene_item_visual)
    {
        // The seat matrix is rebuilt every frame so the console corrections lie on top of the
        // section values while tuning.
        {
            Fvector ypr = m_scene_item_rot;
            ypr.add(g_hud_scene_item_rot_adj);
            ypr.mul(PI / 180.f);
            m_scene_item_offset.setHPB(ypr.x, ypr.y, ypr.z);

            Fvector pos = m_scene_item_pos;
            pos.add(g_hud_scene_item_pos_adj);
            m_scene_item_offset.translate_over(pos);

            const float sc = m_scene_item_scale * g_hud_scene_item_scale_adj;
            if (!fsimilar(sc, 1.f))
            {
                Fmatrix S;
                S.scale(sc, sc, sc);
                m_scene_item_offset.mulB_43(S);
            }
        }

        if (m_scene_item_attached)
        {
            // The item hangs on the same half as the playing hand, or it would follow the right
            // matrix while the hand lives on the left. lh_lead_gun asks for the weapon grip
            // (anchor 0, the right half) whatever hand plays.
            const u16 idx = m_scene_item_lead_gun ? u16(0) : m_scene_item_attach;
            const bool left = !m_scene_item_lead_gun && (m_scene_hand == 1) && m_model_2;
            IKinematicsAnimated* model = left ? m_model_2 : m_model;
            const Fmatrix& base = left ? m_transform_2 : m_transform;

            IKinematics* k = model ? model->dcast_PKinematics() : nullptr;
            if (k && idx < m_ancors.size())
            {
                const Fmatrix ancor = k->LL_GetTransform(m_ancors[idx]);
                m_scene_item_transform.mul(base, ancor);
                m_scene_item_transform.mulB_43(m_scene_item_offset);
            }
            else
                m_scene_item_transform.mul(base, m_scene_item_offset);
        }
        else
            m_scene_item_transform.mul(m_transform, m_scene_item_offset);

        if (m_scene_item_model)
        {
            m_scene_item_model->UpdateTracks();
            m_scene_item_model->dcast_PKinematics()->CalculateBones_Invalidate();
            m_scene_item_model->dcast_PKinematics()->CalculateBones(TRUE);
        }
    }

    if (item0)
        item0->update(true);

    if (item1)
        item1->update(true);

    // A one-hand scene gives the hand back the moment its own cycle is over, without waiting
    // for the script's stop: between the two the hand hung in the last pose. The scene itself
    // (item, drawing) stays until the stop.
    if (m_scene_hand != u8(-1) && m_scene_one_hand && m_scene_end && Device.dwTimeGlobal >= m_scene_end)
    {
        m_scene_hand = u8(-1);
        if (attachable_hud_item* hi = m_attached_items[0])
            if (hi->m_parent_hud_item)
                hi->m_parent_hud_item->PlayAnimIdle();
    }

    if (m_scene_hand != u8(-1))
        m_scene_seat_k += Device.fTimeDelta * g_hud_scene_seat_in;
    else
        m_scene_seat_k -= Device.fTimeDelta * g_hud_scene_seat_out;

    clamp(m_scene_seat_k, 0.f, 1.f);
}

// Right copy: partitions 0 and 2 (partition 1 is its hidden left arm). Left copy: 0, 1 and 2.
// The ownership lock: while a scene holds a hand, an item's cycle skips that copy - with one
// model the root partition went to whoever played last, and the weapon's idle overrode the scene.
void player_hud::play_blend(u16 pid, const MotionID& M, BOOL bMixIn, float speed, bool script_anim)
{
    switch (pid)
    {
    case 0: // both hands
    {
        if (!script_anim && m_scene_hand == 2)
            return;
        // Down without the scene flag: each half checks its own lock, otherwise a two-hand scene
        // would unlock a hand another scene owns.
        play_blend(1, M, bMixIn, speed, false);
        play_blend(2, M, bMixIn, speed, false);
        break;
    }
    case 1: // left
    {
        if (!script_anim && m_scene_hand == 1)
            return;
        if (!m_model_2)
            return;
        const u16 pc = m_model_2->partitions().count();
        for (u16 i = 0; i < pc; ++i)
            PlayHudCycle(*m_model_2, i, M, bMixIn, speed);
        m_model_2->dcast_PKinematics()->CalculateBones_Invalidate();
        break;
    }
    case 2: // right
    {
        if (!script_anim && m_scene_hand == 0)
            return;
        if (!m_model)
            return;
        const u16 pc = m_model->partitions().count();
        for (u16 i = 0; i < pc; ++i)
        {
            if (i == 1)
                continue;
            PlayHudCycle(*m_model, i, M, bMixIn, speed);
        }
        m_model->dcast_PKinematics()->CalculateBones_Invalidate();
        break;
    }
    default:
        break;
    }
}

player_hud_motion_container& player_hud::scene_motions(const shared_str& sect)
{
    auto it = m_scene_motions.find(sect);
    if (it == m_scene_motions.end())
    {
        player_hud_motion_container container;
        container.load(m_model, sect, true);
        it = m_scene_motions.emplace(sect, std::move(container)).first;
    }
    return it->second;
}

u32 player_hud::scene_motion_length(pcstr section, pcstr anim, float speed)
{
    if (!m_model || !section || !anim)
        return 0;

    const shared_str sect(section);
    if (!pSettings->section_exist(sect))
        return 0;

    const player_hud_motion* motion = scene_motions(sect).find_motion(anim);
    if (!motion || motion->m_animations.empty())
        return 0;

    const CMotionDef* md = nullptr;
    return motion_length(motion->m_animations[0].mid, md, speed > 0.f ? speed : 1.f, nullptr);
}

u32 player_hud::scene_play(u8 hand, pcstr section, pcstr anim, bool mix_in, float speed, u32 target_ms)
{
    if (!m_model || !section || !anim)
        return 0;

    const shared_str sect = section;
    if (!pSettings->section_exist(sect))
    {
        Msg("! [hud-scene] section [%s] does not exist", section);
        return 0;
    }

    const player_hud_motion* motion = scene_motions(sect).find_motion(anim);
    if (!motion || motion->m_animations.empty())
    {
        // The usual case with a ported addon: the cycle sits in an omf for another rig and the
        // hands model knows nothing of it. Said out loud, or it looks like a broken scene.
        Msg("! [hud-scene] cycle [%s] of [%s] is not in the hands model", anim, section);
        return 0;
    }

    {
        const Fvector zero{ 0.f, 0.f, 0.f };
        pcstr key = UICore::is_widescreen() ? "hands_position_16x9" : "hands_position";
        m_scene_hands_pos = pSettings->read_if_exists<Fvector>(sect, key, zero);
        pcstr rkey = UICore::is_widescreen() ? "hands_orientation_16x9" : "hands_orientation";
        m_scene_hands_rot = pSettings->read_if_exists<Fvector>(sect, rkey, zero);
    }

    const motion_descr& M = motion->m_animations[Random.randI(motion->m_animations.size())];

    // Stretch the cycle to the scene length when one is given: a cycle marked stop-at-end
    // freezes on its last frame and the hands stand still for the rest of the scene otherwise.
    float eff_speed = speed;
    if (target_ms > 0)
    {
        const CMotionDef* base_md = nullptr;
        const u32 base = motion_length(M.mid, base_md, 1.f, nullptr);
        if (base > 0)
            eff_speed = float(base) / float(target_ms);
    }

    scene_item_release();
    if (pSettings->line_exist(sect, "item_visual"))
    {
        pcstr visual = pSettings->r_string(sect, "item_visual");
        m_scene_item_visual = GEnv.Render->model_Create(visual);
        m_scene_item_model = smart_cast<IKinematicsAnimated*>(m_scene_item_visual);
        // A static model is the norm for things not drawn for a scene (a backpack that was
        // made to hang on the back): it is held still.
        if (m_scene_item_visual && !m_scene_item_model && g_hud_scene_dbg)
            Msg("~ [hud-scene] model [%s] has no motions, held still", visual);
    }

    if (m_scene_item_visual)
    {
        const Fvector zero{ 0.f, 0.f, 0.f };
        m_scene_item_pos = pSettings->read_if_exists<Fvector>(sect, "item_position", zero);
        m_scene_item_rot = pSettings->read_if_exists<Fvector>(sect, "item_orientation", zero);
        m_scene_item_scale = pSettings->read_if_exists<float>(sect, "item_scale", 1.f);
        m_scene_item_attach = pSettings->read_if_exists<u16>(sect, "attach_place_idx", 0);
        // item_attached: on the grip bone (default) or by its own matrix before the camera.
        // item_root_lock: the root of a bottle carries a world-space travel that has to be
        // dropped, while a harvest bag carries its whole staging in the root and needs it.
        m_scene_item_attached = pSettings->read_if_exists<bool>(sect, "item_attached", true);
        m_scene_item_root_lock = pSettings->read_if_exists<bool>(sect, "item_root_lock", true);
        m_scene_item_lead_gun = pSettings->read_if_exists<bool>(sect, "lh_lead_gun", false);

        // The item's cycle is the second name of `anm_xxx = hands, item`; with one name the
        // item takes the hands' name.
        const shared_str item_anim =
            (motion->m_base_name != motion->m_additional_name) ? motion->m_additional_name : M.name;

        MotionID mid;
        if (m_scene_item_model)
            mid = m_scene_item_model->ID_Cycle_Safe(item_anim);
        if (m_scene_item_model && !mid.valid())
            mid = m_scene_item_model->ID_Cycle_Safe("idle");

        if (mid.valid())
        {
            IKinematics* k_item = m_scene_item_model->dcast_PKinematics();
            if (k_item && m_scene_item_root_lock)
            {
                const u16 root_id = k_item->LL_GetBoneRoot();
                CBoneInstance& root = k_item->LL_GetBoneInstance(root_id);
                root.set_callback_overwrite(TRUE);
                root.mTransform.identity();
            }

            const u16 pc = m_scene_item_model->partitions().count();
            for (u16 pid = 0; pid < pc; ++pid)
                PlayHudCycle(*m_scene_item_model, pid, mid, mix_in ? TRUE : FALSE, eff_speed);
            m_scene_item_model->dcast_PKinematics()->CalculateBones_Invalidate();
        }
        else if (m_scene_item_model)
            Msg("! [hud-scene] item [%s] has neither cycle [%s] nor idle", pSettings->r_string(sect, "item_visual"),
                item_anim.c_str());
    }

    // hand: 0 right, 1 left, 2 both. A one-hand scene lays its cycle on its hand only; the other
    // keeps holding and drawing the weapon (render_hud, the seat, play_blend all read this).
    m_scene_one_hand = (hand != 2);
    m_scene_hand = hand;
    m_scene_hand_seat = (hand == 0) ? u8(0) : u8(1);
    const u16 part = (hand == 2) ? u16(0) : (hand == 0 ? u16(2) : u16(1));

    if (g_hud_scene_dbg)
        Msg("~ [hud-scene] play: section [%s], cycle [%s] -> motion [%s], hand %u, target %u ms, attached [%s] + [%s]",
            section, anim, M.name.c_str(), u32(hand), target_ms,
            m_attached_items[0] ? m_attached_items[0]->m_sect_name.c_str() : "-",
            m_attached_items[1] ? m_attached_items[1]->m_sect_name.c_str() : "-");

    const CMotionDef* md = nullptr;
    play_blend(part, M.mid, mix_in ? TRUE : FALSE, eff_speed, true);
    const u32 length = motion_length(M.mid, md, eff_speed, nullptr);

    m_scene_end = length ? (Device.dwTimeGlobal + length) : 0;
    m_scene_on = true;

    return length;
}

void player_hud::scene_item_release()
{
    if (!m_scene_item_visual)
        return;

    IRenderVisual* v = m_scene_item_visual;
    m_scene_item_visual = nullptr;
    m_scene_item_model = nullptr;
    GEnv.Render->model_Delete(v);
}

void player_hud::scene_stop()
{
    scene_item_release();

    // After a one-hand scene the weapon replays its idle: our cycle sat on the partition of the
    // other hand and does not go away by itself.
    if (m_scene_one_hand)
    {
        if (attachable_hud_item* hi = m_attached_items[0])
            if (hi->m_parent_hud_item)
                hi->m_parent_hud_item->PlayAnimIdle();
    }
    m_scene_one_hand = false;
    m_scene_hand = u8(-1);
    m_scene_on = false;
    m_scene_end = 0;
}

bool player_hud::scene_active() const
{
    if (!m_scene_on)
        return false;
    // A scene ends by time as well as by the script's stop: the addons rely on the engine
    // closing it when the cycle is over, and a flag alone kept the scene item in the hands
    // forever with the real weapon undrawn.
    if (m_scene_end && Device.dwTimeGlobal >= m_scene_end)
        return false;
    return true;
}

u32 player_hud::anim_play(u16 part, const MotionID& M, BOOL bMixIn, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel)
{
    if (!itemModel && m_model)
    {
        // One item drives BOTH hands (it is held with two). Two items - each its own half.
        const u16 pid = (attached_item(0) && attached_item(1)) ? ((part == 0) ? u16(2) : u16(1)) : u16(0);
        play_blend(pid, M, bMixIn, speed, false);
    }

    return motion_length(M, md, speed, itemModel);
}

void player_hud::update_additional(Fmatrix& trans) const
{
    if (m_attached_items[0])
        m_attached_items[0]->update_hud_additional(trans);

    if (m_attached_items[1])
        m_attached_items[1]->update_hud_additional(trans);
}

void player_hud::update_inertion(Fmatrix& trans) const
{
    if (inertion_allowed())
    {
        attachable_hud_item* pMainHud = m_attached_items[0];

        Fmatrix xform;
        Fvector& origin = trans.c;
        xform = trans;

        static Fvector st_last_dir = {0, 0, 0};

        // load params
        hud_item_measures::inertion_params inertion_data;
        if (pMainHud)
        { // Загружаем параметры инерции из основного худа
            inertion_data.m_pitch_offset_r = pMainHud->m_measures.m_inertion_params.m_pitch_offset_r;
            inertion_data.m_pitch_offset_n = pMainHud->m_measures.m_inertion_params.m_pitch_offset_n;
            inertion_data.m_pitch_offset_d = pMainHud->m_measures.m_inertion_params.m_pitch_offset_d;
            inertion_data.m_pitch_low_limit = pMainHud->m_measures.m_inertion_params.m_pitch_low_limit;
            inertion_data.m_origin_offset = pMainHud->m_measures.m_inertion_params.m_origin_offset;
            inertion_data.m_origin_offset_aim = pMainHud->m_measures.m_inertion_params.m_origin_offset_aim;
            inertion_data.m_tendto_speed = pMainHud->m_measures.m_inertion_params.m_tendto_speed;
            inertion_data.m_tendto_speed_aim = pMainHud->m_measures.m_inertion_params.m_tendto_speed_aim;
        }
        else
        { // Загружаем дефолтные параметры инерции
            inertion_data.m_pitch_offset_r = PITCH_OFFSET_R;
            inertion_data.m_pitch_offset_n = PITCH_OFFSET_N;
            inertion_data.m_pitch_offset_d = PITCH_OFFSET_D;
            inertion_data.m_pitch_low_limit = PITCH_LOW_LIMIT;
            inertion_data.m_origin_offset = ORIGIN_OFFSET;
            inertion_data.m_origin_offset_aim = ORIGIN_OFFSET_AIM;
            inertion_data.m_tendto_speed = TENDTO_SPEED;
            inertion_data.m_tendto_speed_aim = TENDTO_SPEED_AIM;
        }

        // calc difference
        Fvector diff_dir;
        diff_dir.sub(xform.k, st_last_dir);

        // clamp by PI_DIV_2
        Fvector last;
        last.normalize_safe(st_last_dir);
        float dot = last.dotproduct(xform.k);
        if (dot < EPS)
        {
            Fvector v0;
            v0.crossproduct(st_last_dir, xform.k);
            st_last_dir.crossproduct(xform.k, v0);
            diff_dir.sub(xform.k, st_last_dir);
        }

        // tend to forward
        float _tendto_speed, _origin_offset;
        if (pMainHud && pMainHud->m_parent_hud_item->GetCurrentHudOffsetIdx() > 0)
        { // Худ в режиме "Прицеливание"
            float factor = pMainHud->m_parent_hud_item->GetInertionFactor();
            _tendto_speed = inertion_data.m_tendto_speed_aim - (inertion_data.m_tendto_speed_aim - inertion_data.m_tendto_speed) * factor;
            _origin_offset =
                inertion_data.m_origin_offset_aim - (inertion_data.m_origin_offset_aim - inertion_data.m_origin_offset) * factor;
        }
        else
        { // Худ в режиме "От бедра"
            _tendto_speed = inertion_data.m_tendto_speed;
            _origin_offset = inertion_data.m_origin_offset;
        }

        // Фактор силы инерции
        if (pMainHud)
        {
            float power_factor = pMainHud->m_parent_hud_item->GetInertionPowerFactor();
            _tendto_speed *= power_factor;
            _origin_offset *= power_factor;
        }

        st_last_dir.mad(diff_dir, _tendto_speed * Device.fTimeDelta);
        origin.mad(diff_dir, _origin_offset);

        // pitch compensation
        float pitch = angle_normalize_signed(xform.k.getP());

        if (pMainHud)
            pitch *= pMainHud->m_parent_hud_item->GetInertionFactor();

        // Отдаление\приближение
        origin.mad(xform.k, -pitch * inertion_data.m_pitch_offset_d);

        // Сдвиг в противоположную часть экрана
        origin.mad(xform.i, -pitch * inertion_data.m_pitch_offset_r);

        // Подьём\опускание
        clamp(pitch, inertion_data.m_pitch_low_limit, PI);
        origin.mad(xform.j, -pitch * inertion_data.m_pitch_offset_n);
    }
}

attachable_hud_item* player_hud::create_hud_item(const shared_str& sect)
{
    current_player_hud_sect = sect;
    auto& item = m_pool[sect];

    if (!item)
        item = xr_new<attachable_hud_item>(this, sect, m_model);

    return item;
}

bool player_hud::allow_activation(CHudItem* item) const
{
    if (m_attached_items[1])
        return m_attached_items[1]->m_parent_hud_item->CheckCompatibility(item);
    else
        return true;
}

void player_hud::attach_item(CHudItem* item)
{
    attachable_hud_item* pi = create_hud_item(item->HudSection());
    const int item_idx = pi->m_attach_place_idx;

    if (m_attached_items[item_idx] != pi || pi->m_parent_hud_item != item)
    {
        if (m_attached_items[item_idx])
            m_attached_items[item_idx]->m_parent_hud_item->on_b_hud_detach();

        m_attached_items[item_idx] = pi;
        pi->m_parent_hud_item = item;
        pi->reload_measures();

        if (item_idx == 0 && m_attached_items[1])
            m_attached_items[1]->m_parent_hud_item->CheckCompatibility(item);

        item->on_a_hud_attach();
    }
    pi->m_parent_hud_item = item;
}

void player_hud::detach_item_idx(u16 idx)
{
    if (nullptr == attached_item(idx))
        return;

    m_attached_items[idx]->m_parent_hud_item->on_b_hud_detach();

    m_attached_items[idx]->m_parent_hud_item = nullptr;
    m_attached_items[idx] = nullptr;

    if (idx == 1 && attached_item(0))
    {
        u16 part_idR = m_model->partitions().part_id("right_hand");
        u32 bc = m_model->LL_PartBlendsCount(part_idR);
        for (u32 bidx = 0; bidx < bc; ++bidx)
        {
            CBlend* BR = m_model->LL_PartBlend(part_idR, bidx);
            if (!BR)
                continue;

            MotionID M = BR->motionID;

            u16 pc = m_model->partitions().count();
            for (u16 pid = 0; pid < pc; ++pid)
            {
                if (pid != part_idR)
                {
                    CBlend* B = m_model->PlayCycle(pid, M, TRUE); // this can destroy BR calling UpdateTracks !
                    if (BR->blend_state() != CBlend::eFREE_SLOT)
                    {
                        u16 bop = B->bone_or_part;
                        *B = *BR;
                        B->bone_or_part = bop;
                    }
                }
            }

            // The left copy follows the weapon too, at the same point of the cycle: otherwise
            // it keeps the detached item's last pose until the weapon's next cycle.
            if (m_model_2 && BR->blend_state() != CBlend::eFREE_SLOT)
            {
                const u16 pc2 = m_model_2->partitions().count();
                for (u16 pid = 0; pid < pc2; ++pid)
                {
                    if (CBlend* B2 = m_model_2->PlayCycle(pid, M, TRUE))
                    {
                        u16 bop = B2->bone_or_part;
                        *B2 = *BR;
                        B2->bone_or_part = bop;
                    }
                }
            }
        }
    }
    else if (idx == 0 && attached_item(1))
    {
        OnMovementChanged(mcAnyMove);
    }
}

void player_hud::detach_item(CHudItem* item)
{
    if (nullptr == item->HudItemData())
        return;

    const u16 item_idx = item->HudItemData()->m_attach_place_idx;

    if (m_attached_items[item_idx] == item->HudItemData())
    {
        detach_item_idx(item_idx);
    }
}

void player_hud::calc_transform(u16 attach_slot_idx, const Fmatrix& offset, Fmatrix& result) const
{
    // An item is placed from ITS half: anchor 0 lives on the right copy, anchor 1 on the left.
    const bool left = (attach_slot_idx != 0) && (m_model_2 != nullptr);
    IKinematicsAnimated* model = left ? m_model_2 : m_model;
    const Fmatrix& base = left ? m_transform_2 : m_transform;

    const attachable_hud_item* item = m_attached_items[attach_slot_idx];
    if (item && !item->m_monolithic && model && attach_slot_idx < m_ancors.size())
    {
        IKinematics* k = model->dcast_PKinematics();
        const Fmatrix ancor_m = k->LL_GetTransform(m_ancors[attach_slot_idx]);
        result.mul(base, ancor_m);
        result.mulB_43(offset);
    }
    else
    {
        result.mul(base, offset);
        VERIFY(!fis_zero(DET(result)));
    }
}

bool player_hud::inertion_allowed() const
{
    if (const attachable_hud_item* hi = m_attached_items[0])
    {
        return hi->m_parent_hud_item->HudInertionEnabled() && hi->m_parent_hud_item->HudInertionAllowed();
    }
    return true;
}

void player_hud::OnMovementChanged(ACTOR_DEFS::EMoveCommand cmd) const
{
    CHudItem* hudItem0 = m_attached_items[0] ? m_attached_items[0]->m_parent_hud_item : nullptr;
    CHudItem* hudItem1 = m_attached_items[1] ? m_attached_items[1]->m_parent_hud_item : nullptr;

    if (cmd == 0)
    {
        if (hudItem0 && hudItem0->GetState() == CHUDState::eIdle)
            hudItem0->PlayAnimIdle();

        if (hudItem1 && hudItem1->GetState() == CHUDState::eIdle)
            hudItem1->PlayAnimIdle();
    }
    else
    {
        if (hudItem0)
            hudItem0->OnMovementChanged(cmd);

        if (hudItem1)
            hudItem1->OnMovementChanged(cmd);
    }
}
