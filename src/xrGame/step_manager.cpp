#include "StdAfx.h"
#include "step_manager.h"
#include "entity_alive.h"
#include "Include/xrRender/Kinematics.h"
#include "Level.h"
#include "GamePersistent.h"
#include "material_manager.h"
#include "xrEngine/profiler.h"
#include "IKLimbsController.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "da_water_impact.h"
#include "da_water_actor.h"

#ifdef DEBUG
BOOL debug_step_info = FALSE;
BOOL debug_step_info_load = FALSE;
#endif

extern float psHUDStepSoundVolume;

namespace
{
// How the wake fades as a foot approaches the bank. There is less water to displace in the
// shallows, so the disturbance is smaller and tighter there instead of stopping dead at the
// waterline - which is what the baked field's channel A (metres to the nearest bank) is for.
constexpr float wake_bank_range = 2.f; // full strength this far out
constexpr float wake_bank_min = 0.35f; // what is left of it right on the waterline
} // namespace

CStepManager::CStepManager() {}
CStepManager::~CStepManager() {}
IFactoryObject* CStepManager::_construct()
{
    m_object = smart_cast<CEntityAlive*>(this);
    VERIFY(m_object);
    return (m_object);
}

void CStepManager::reload(LPCSTR section)
{
    m_legs_count = pSettings->r_u8(section, "LegsCount");
    LPCSTR anim_section = pSettings->r_string(section, "step_params");

    if (!pSettings->section_exist(anim_section))
    {
#ifdef DEBUG
        Msg("! no step_params section for :%s section :s", m_object->cName().c_str(), section);
#endif
        return;
    }
    VERIFY((m_legs_count >= MIN_LEGS_COUNT) && (m_legs_count <= MAX_LEGS_COUNT));

    SStepParam param;
    param.step[0].time = 0.1f; // avoid warning

    LPCSTR anim_name, val;
    string16 cur_elem;

    IKinematicsAnimated* skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());

    VERIFY3(skeleton_animated, "object is not animated", m_object->cNameVisual().c_str());
#ifdef DEBUG
    if (debug_step_info_load)
        Msg("loading step_params for object :%s, visual: %s, section: %s, step_params section: %s  ",
            m_object->cName().c_str(), m_object->cNameVisual().c_str(), section, anim_section);
#endif

    for (u32 i = 0; pSettings->r_line(anim_section, i, &anim_name, &val); ++i)
    {
        _GetItem(val, 0, cur_elem);

        param.cycles = u8(atoi(cur_elem));
        R_ASSERT(param.cycles >= 1);

        for (u32 j = 0; j < m_legs_count; j++)
        {
            _GetItem(val, 1 + j * 2, cur_elem);
            param.step[j].time = float(atof(cur_elem));
            _GetItem(val, 1 + j * 2 + 1, cur_elem);
            param.step[j].power = float(atof(cur_elem));
            VERIFY(_valid(param.step[j].power));
        }

        MotionID motion_id = skeleton_animated->ID_Cycle_Safe(anim_name);
        if (!motion_id)
        {
#ifdef DEBUG

            IKinematicsAnimated* KA = smart_cast<IKinematicsAnimated*>(m_object->Visual());
            VERIFY(KA);

            Msg("! (CStepManager::reload) no anim :%s object:%s, visual: %s, step_params section: %s ", anim_name,
                m_object->cName().c_str(), m_object->cNameVisual().c_str(), anim_section);

#endif
            continue;
        }
#ifdef DEBUG
        if (debug_step_info_load)
        {
            IKinematicsAnimated* KA = smart_cast<IKinematicsAnimated*>(m_object->Visual());
            VERIFY(KA);
            std::pair<LPCSTR, LPCSTR> anim_name = KA->LL_MotionDefName_dbg(motion_id);
            Msg("step_params loaded for object :%s, visual: %s, motion: %s, anim set: %s  ", m_object->cName().c_str(),
                m_object->cNameVisual().c_str(), anim_name.first, anim_name.second);
        }
#endif
        m_steps_map.emplace(motion_id, param);
    }

#ifdef DEBUG
    if (m_steps_map.empty())
        Msg("! no steps info loaded for :%s, section :s, step_params section: %s ", m_object->cName().c_str(), section,
            anim_section);
#endif
    // reload foot bones
    for (u32 i = 0; i < MAX_LEGS_COUNT; i++)
        m_foot_bones[i] = BI_NONE;
    reload_foot_bones();

    m_time_anim_started = 0;
    m_blend = 0;
}

void CStepManager::on_animation_start(MotionID motion_id, CBlend* blend)
{
    m_blend = blend;
    if (!m_blend)
        return;

    if (m_object->character_ik_controller())
        m_object->character_ik_controller()->PlayLegs(blend);

    m_time_anim_started = Device.dwTimeGlobal;

    // искать текущую анимацию в STEPS_MAP
    STEPS_MAP_IT it = m_steps_map.find(motion_id);
    if (it == m_steps_map.end())
    {
#ifdef DEBUG
        if (debug_step_info)
        {
            IKinematicsAnimated* KA = smart_cast<IKinematicsAnimated*>(m_object->Visual());
            VERIFY(KA);
            std::pair<LPCSTR, LPCSTR> anim_name = KA->LL_MotionDefName_dbg(motion_id);
            Msg("! no step_params found for object :%s, visual: %s, motion: %s, anim set: %s  ",
                m_object->cName().c_str(), m_object->cNameVisual().c_str(), anim_name.first, anim_name.second);
        }
#endif
        m_step_info.disable = true;
        return;
    }

    m_step_info.disable = false;
    m_step_info.params = it->second;
    m_step_info.cur_cycle = 1; // all cycles are 1-based

    for (u32 i = 0; i < m_legs_count; i++)
    {
        m_step_info.activity[i].handled = false;
        m_step_info.activity[i].cycle = m_step_info.cur_cycle;
    }

    VERIFY(m_blend);
}

void CStepManager::update(bool b_hud_view)
{
    START_PROFILE("Step Manager")

    if (m_step_info.disable)
        return;
    if (!m_blend)
        return;

    float dist_sqr = m_object->Position().distance_to_sqr(Device.vCameraPosition);
    bool b_play = dist_sqr < 400.0f; // 20m

    // Continuous wake: a foot pushing through water disturbs it for as long as it keeps
    // moving, so this is fed EVERY frame - water_wake refreshes the slot the emitter already
    // owns rather than taking a new one - and not once per step event like the ring below.
    // It runs only while a stepping animation is playing, which is what "moving" means here.
    UpdateWaterWake(dist_sqr);

    // получить параметры шага
    SStepParam& step = m_step_info.params;
    u32 cur_time = Device.dwTimeGlobal;

    // время одного цикла анимации
    float cycle_anim_time = get_blend_time() / step.cycles;

    // пройти по всем ногам и проверить время
    SGameMtlPair* mtl_pair = 0;
    bool material_picked = false;

    for (u32 i = 0; i < m_legs_count; i++)
    {
        // если событие уже обработано для этой ноги, то skip
        if (m_step_info.activity[i].handled && (m_step_info.activity[i].cycle == m_step_info.cur_cycle))
            continue;

        // вычислить смещённое время шага в соответствии с параметрами анимации ходьбы
        u32 offset_time = m_time_anim_started +
            u32(1000 * (cycle_anim_time * (m_step_info.cur_cycle - 1) + cycle_anim_time * step.step[i].time));
        if (offset_time <= cur_time)
        {
            if (!material_picked)
            {
                mtl_pair = m_object->material().get_current_pair();

                material_picked = true;
            }

            if (!mtl_pair)
                break;

            // Играть звук
            if (b_play && is_on_ground())
                m_step_sound.play_next(mtl_pair, m_object, m_step_info.params.step[i].power, b_hud_view);

            // Играть партиклы
            if (b_play && !mtl_pair->CollideParticles.empty())
            {
                LPCSTR ps_name = mtl_pair->CollideParticles[::Random.randI(0, mtl_pair->CollideParticles.size())].c_str();

                //отыграть партиклы столкновения материалов
                CParticlesObject* ps = CParticlesObject::Create(ps_name, TRUE);

                // вычислить позицию и направленность партикла
                Fmatrix pos;

                // установить направление
                pos.k.set(Fvector().set(0.0f, 1.0f, 0.0f));
                Fvector::generate_orthonormal_basis(pos.k, pos.j, pos.i);

                // установить позицию
                pos.c.set(get_foot_position(ELegType(i)));

                ps->UpdateParent(pos, Fvector().set(0.f, 0.f, 0.f));
                GamePersistent().ps_schedule_play(ps);
            }

            // Play Camera FXs
            event_on_step();

            // Ripples: a foot landing in water or in a rain puddle spreads a ring on it - the
            // same ring a bullet makes (Environment::water_hit) - within the distance a ring
            // can be seen from. The pair's ground material tells open water, the puddle mask
            // tells the rain layer.
            {
                const auto& wcfg = da_water_impact_cfg();
                if (wcfg.enabled && dist_sqr < wcfg.ring_distance * wcfg.ring_distance)
                {
                    const Fvector foot = get_foot_position(ELegType(i));
                    Fvector surface;
                    if (da_water_surface(foot, mtl_pair->GetMtl0(), 1.f, surface) ||
                        da_water_surface(foot, mtl_pair->GetMtl1(), 1.f, surface))
                    {
                        g_pGamePersistent->Environment().water_hit(surface, wcfg.ring_radius_step, CEnvironment::EWaterHit::ring);

                        // ...and the splash that goes with it, but only where the material
                        // pair could not throw one itself. A step that landed on the water
                        // material plays the pair's own hit_fx through CollideParticles above;
                        // a step on a submerged bed reports the bed's pair, which knows nothing
                        // about the water standing over it.
                        const auto liquid = [](int idx) {
                            if (idx < 0 || idx >= int(GMLib.CountMaterial()))
                                return false;
                            const SGameMtl* m = GMLib.GetMaterialByIdx(u16(idx));
                            return m && m->Flags.test(SGameMtl::flLiquid);
                        };
                        const auto& acfg = da_water_actor_cfg();
                        if (b_play && !liquid(mtl_pair->GetMtl0()) && !liquid(mtl_pair->GetMtl1()) &&
                            surface.y - foot.y > acfg.step_splash_depth)
                            da_water_splash(surface, acfg.ps_step);
                    }
                }
            }

            // обновить поле handle
            m_step_info.activity[i].handled = true;
            m_step_info.activity[i].cycle = m_step_info.cur_cycle;
        }
    }

    // определить текущий цикл
    if (m_step_info.cur_cycle < step.cycles)
        m_step_info.cur_cycle = 1 + u8(float(cur_time - m_time_anim_started) / (1000.f * cycle_anim_time));

    // если анимация циклическая...
    u32 time_anim_end = m_time_anim_started + u32(get_blend_time() * 1000); // время завершения работы анимации
    if (!m_blend->stop_at_end && (time_anim_end < cur_time))
    {
        m_time_anim_started = time_anim_end;
        m_step_info.cur_cycle = 1;

        for (u32 i = 0; i < m_legs_count; i++)
        {
            m_step_info.activity[i].handled = false;
            m_step_info.activity[i].cycle = m_step_info.cur_cycle;
        }
    }
    STOP_PROFILE
}

void CStepManager::UpdateWaterWake(float dist_sqr)
{
    const auto& wcfg = da_water_impact_cfg();
    if (!wcfg.enabled || !g_pGamePersistent)
        return;
    if (dist_sqr >= wcfg.ring_distance * wcfg.ring_distance)
        return;

    auto& env = g_pGamePersistent->Environment();
    // The baked field only: this runs per frame per body, so it has to stay a table lookup.
    // A level without a field simply makes no wakes, which is the behaviour before this work.
    if (!env.water_field_valid())
        return;

    CCharacterPhysicsSupport* cps = m_object->character_physics_support();
    if (!cps || !cps->movement())
        return;

    // The wake is a rate: it grows with how hard the body is pushing the water, and a body
    // that has stopped stops feeding its slot and lets it fade.
    const auto& acfg = da_water_actor_cfg();
    const float k = clampr(cps->movement()->GetXZVelocityActual() / _max(acfg.wake_speed_ref, EPS_S), 0.f, 1.f);
    if (k < 0.05f)
        return;

    for (u32 i = 0; i < m_legs_count; ++i)
    {
        const Fvector foot = get_foot_position(ELegType(i));
        float surface, bed;
        if (!env.water_at(foot, surface, bed) || surface <= foot.y)
            continue;
        Fvector at = foot;
        at.y = surface;
        // water_shore_dist is defined on every texel of the field, water or bank, and the foot
        // is inside it here (water_at just succeeded), so this cannot be the -FLT_MAX miss.
        const float bank = wake_bank_min +
            (1.f - wake_bank_min) * clampr(env.water_shore_dist(foot) / wake_bank_range, 0.f, 1.f);
        env.water_wake(at, acfg.wake_radius * bank, acfg.wake_strength * k * bank);
    }
}

//////////////////////////////////////////////////////////////////////////
// Function for foot processing
//////////////////////////////////////////////////////////////////////////
Fvector CStepManager::get_foot_position(ELegType leg_type)
{
    R_ASSERT2(m_foot_bones[leg_type] != BI_NONE, "foot bone had not been set");

    IKinematics* pK = smart_cast<IKinematics*>(m_object->Visual());
    const Fmatrix& bone_transform = pK->LL_GetBoneInstance(m_foot_bones[leg_type]).mTransform;

    Fmatrix global_transform;
    global_transform.mul_43(m_object->XFORM(), bone_transform);

    return global_transform.c;
}

void CStepManager::load_foot_bones(CInifile::Sect& data)
{
    for (auto I = data.Data.cbegin(); I != data.Data.cend(); ++I)
    {
        const CInifile::Item& item = *I;

        u16 index = smart_cast<IKinematics*>(m_object->Visual())->LL_BoneID(item.second.c_str());
        VERIFY3(index != BI_NONE, "foot bone not found", item.second.c_str());

        if (xr_strcmp(item.first.c_str(), "front_left") == 0)
            m_foot_bones[eFrontLeft] = index;
        else if (xr_strcmp(item.first.c_str(), "front_right") == 0)
            m_foot_bones[eFrontRight] = index;
        else if (xr_strcmp(item.first.c_str(), "back_right") == 0)
            m_foot_bones[eBackRight] = index;
        else if (xr_strcmp(item.first.c_str(), "back_left") == 0)
            m_foot_bones[eBackLeft] = index;
    }
}

void CStepManager::reload_foot_bones()
{
    CInifile* ini = smart_cast<IKinematics*>(m_object->Visual())->LL_UserData();
    if (ini && ini->section_exist("foot_bones"))
    {
        load_foot_bones(ini->r_section("foot_bones"));
    }
    else
    {
        if (!pSettings->line_exist(m_object->cNameSect().c_str(), "foot_bones"))
            R_ASSERT2(false, "section [foot_bones] not found in monster user_data");
        load_foot_bones(pSettings->r_section(pSettings->r_string(m_object->cNameSect().c_str(), "foot_bones")));
    }

    // проверка на соответсвие
    int count = 0;
    for (u32 i = 0; i < MAX_LEGS_COUNT; i++)
        if (m_foot_bones[i] != BI_NONE)
            count++;

    VERIFY(count == m_legs_count);
}

float CStepManager::get_blend_time() { return (m_blend->timeTotal / m_blend->speed); }
void CStepManager::material_sound::play_next(
    SGameMtlPair* mtl_pair, CEntityAlive* object, float volume, bool b_hud_mode)
{
    if (mtl_pair->StepSounds.empty())
        return;

    Fvector sound_pos = object->Position();
    sound_pos.y += 0.5;

    if (last_mtl_pair != mtl_pair || m_last_step_sound_played == u8(-1))
    {
        m_last_step_sound_played = u8(Random.randI(mtl_pair->StepSounds.size()));
        last_mtl_pair = mtl_pair;
    }
    else
    {
        u8 new_played = u8((m_last_step_sound_played + 1 + Random.randI(mtl_pair->StepSounds.size() - 1)) %
            mtl_pair->StepSounds.size());

        m_last_step_sound_played = new_played;
    }

    float vol = (b_hud_mode) ? volume * psHUDStepSoundVolume : volume;
    if (b_hud_mode)
        sound_pos.set(0, 0, 0);

    mtl_pair->StepSounds[m_last_step_sound_played].play_no_feedback(
        object, b_hud_mode ? sm_2D : 0, 0, &sound_pos, &vol);
}
