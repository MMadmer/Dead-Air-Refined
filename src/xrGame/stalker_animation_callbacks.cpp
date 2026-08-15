////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_callbacks.cpp
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager : bone callbacks
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "sight_manager.h"
#include "stalker_movement_manager_smart_cover.h"
#include "game_object_space.h"
#include "EffectorShot.h"

typedef CStalkerAnimationManager::callback_params callback_params;

// A config bone index, validated. This was not a "possible error" but a WRITE PAST THE
// ARRAY: the bone name comes from the NPC section (bone_head/bone_shoulder/bone_spin),
// i.e. mod data. When the model lacks the bone, LL_BoneID returns BI_NONE (0xFFFF) and
// LL_GetBoneInstance(0xFFFF) writes the callback pointer ~8 MB past bone_instances -
// heap corruption that crashes later somewhere else. The only check, VERIFY inside
// LL_GetBoneInstance, is empty in release.
static CBoneInstance* config_bone(IKinematics* kinematics, pcstr section, pcstr key)
{
    pcstr bone_name = pSettings->r_string(section, key);
    const u16 bone_id = kinematics->LL_BoneID(bone_name);
    if (bone_id == BI_NONE || bone_id >= kinematics->LL_BoneCount())
    {
        static u32 hits = 0;
        ++hits;
        if (hits <= 10 || (hits % 500) == 0)
            Msg("! %s: bone '%s' (%s) is not in the model, rotation not assigned (case %u)", section,
                bone_name, key, hits);
        return nullptr;
    }
    return &kinematics->LL_GetBoneInstance(bone_id);
}

static void callback_rotation(CBoneInstance* bone)
{
    R_ASSERT(_valid(bone->mTransform));
    callback_params* parameter = static_cast<callback_params*>(bone->callback_param());
    VERIFY(parameter);
    VERIFY(parameter->m_rotation);
    VERIFY(parameter->m_object);

    CAI_Stalker const* object = parameter->m_object;
    if (!object->sight().enabled())
        return;

    Fvector position = bone->mTransform.c;
    R_ASSERT(_valid(*parameter->m_rotation));
    bone->mTransform.mulA_43(*parameter->m_rotation);
    CWeaponShotEffector& effector = object->weapon_shot_effector();
    if (!effector.IsActive())
    {
        bone->mTransform.c = position;
        R_ASSERT(_valid(bone->mTransform));
        return;
    }

    Fvector angles;
    effector.GetDeltaAngle(angles);
    angles.x = angle_normalize_signed(angles.x);
    angles.y = angle_normalize_signed(angles.y);
    angles.z = angle_normalize_signed(angles.z);

    if (object->movement().current_params().cover())
        angles.mul(0.f);
    else
        angles.mul(.1f);

    Fmatrix effector_transform;
    effector_transform.setXYZ(angles);
    R_ASSERT(_valid(effector_transform));
    bone->mTransform.mulA_43(effector_transform);
    bone->mTransform.c = position;
    R_ASSERT(_valid(bone->mTransform));
}

static void callback_rotation_blend(CBoneInstance* const bone)
{
    R_ASSERT(_valid(bone->mTransform));

    callback_params* parameter = static_cast<callback_params*>(bone->callback_param());
    VERIFY(parameter);
    VERIFY(parameter->m_rotation);
    VERIFY(parameter->m_object);
    VERIFY(parameter->m_blend);
    //	VERIFY2							( *parameter->m_blend, make_string( "%d %s[%s]", Device.dwTimeGlobal,
    // parameter->m_object->cName().c_str(), parameter->m_object->g_Alive() ? "+" : "-") );

    float multiplier = 1.f;
    if (*parameter->m_blend)
    {
        CBlend const& blend = **parameter->m_blend;
        multiplier = blend.timeCurrent / blend.timeTotal;
    }

    VERIFY(multiplier >= 0.f);
    VERIFY(multiplier <= 1.f);
    multiplier = parameter->m_forward ? multiplier : (1.f - multiplier);

#if 0
    Fmatrix rotation				= *parameter->m_rotation;
    Fvector							angles;
    rotation.getXYZ					(angles);
    angles.mul						(multiplier);
    rotation.setXYZ					(angles);
#else // #if 0
    Fquaternion left;
    left.set(Fidentity);

    Fquaternion right;
    right.set(*parameter->m_rotation);

    Fquaternion result;
    result.slerp(left, right, multiplier);

    Fmatrix rotation;
    rotation.rotation(result);
#endif // #if 0

    Fvector position = bone->mTransform.c;
    R_ASSERT(_valid(rotation));
    bone->mTransform.mulA_43(rotation);
    bone->mTransform.c = position;
    R_ASSERT(_valid(bone->mTransform));
}

void CStalkerAnimationManager::assign_bone_callbacks()
{
    IKinematics* kinematics = smart_cast<IKinematics*>(m_visual);
    VERIFY(kinematics);

#ifdef DEBUG
    m_head_params.invalidate();
    m_shoulder_params.invalidate();
    m_spine_params.invalidate();
#endif // #ifdef DEBUG

    LPCSTR section = object().cNameSect().c_str();

    m_head_params.m_rotation = &object().sight().current_head_rotation();
    m_head_params.m_object = &object();
    m_head_params.m_blend = 0;
    m_head_params.m_forward = true;

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_head"))
        bone->set_callback(bctCustom, &callback_rotation, &m_head_params);

    m_shoulder_params.m_rotation = &object().sight().current_shoulder_rotation();
    m_shoulder_params.m_object = &object();
    m_shoulder_params.m_blend = 0;
    m_shoulder_params.m_forward = true;

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_shoulder"))
        bone->set_callback(bctCustom, &callback_rotation, &m_shoulder_params);

    m_spine_params.m_rotation = &object().sight().current_spine_rotation();
    m_spine_params.m_object = &object();
    m_spine_params.m_blend = 0;
    m_spine_params.m_forward = true;

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_spin"))
        bone->set_callback(bctCustom, &callback_rotation, &m_spine_params);

    //	remove_bone_callbacks	();
}

void CStalkerAnimationManager::assign_bone_blend_callbacks(bool const& forward_direction)
{
    IKinematics* kinematics = smart_cast<IKinematics*>(m_visual);
    VERIFY(kinematics);

#ifdef DEBUG
    m_head_params.invalidate();
    m_shoulder_params.invalidate();
    m_spine_params.invalidate();
#endif // #ifdef DEBUG

    LPCSTR section = object().cNameSect().c_str();

    m_head_params.m_rotation = &object().sight().current_head_rotation();
    m_head_params.m_object = &object();
    m_head_params.m_blend = &global().blend();
    m_head_params.m_forward = forward_direction;

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_head"))
        bone->set_callback(bctCustom, &callback_rotation_blend, &m_head_params);

    m_shoulder_params.m_rotation = &object().sight().current_shoulder_rotation();
    m_shoulder_params.m_object = &object();
    m_shoulder_params.m_blend = &global().blend();
    m_shoulder_params.m_forward = forward_direction;

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_shoulder"))
        bone->set_callback(bctCustom, &callback_rotation_blend, &m_shoulder_params);

    m_spine_params.m_rotation = &object().sight().current_spine_rotation();
    m_spine_params.m_object = &object();
    m_spine_params.m_blend = &global().blend();
    m_spine_params.m_forward = forward_direction;

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_spin"))
        bone->set_callback(bctCustom, &callback_rotation_blend, &m_spine_params);

    //	remove_bone_callbacks	();
}

void CStalkerAnimationManager::remove_bone_callbacks()
{
    IKinematics* kinematics = smart_cast<IKinematics*>(m_visual);
    VERIFY(kinematics);

#ifdef DEBUG
    m_head_params.invalidate();
    m_shoulder_params.invalidate();
    m_spine_params.invalidate();
#endif // #ifdef DEBUG

    LPCSTR section = object().cNameSect().c_str();

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_head"))
        bone->set_callback(bctCustom, 0, 0);

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_shoulder"))
        bone->set_callback(bctCustom, 0, 0);

    if (CBoneInstance* bone = config_bone(kinematics, section, "bone_spin"))
        bone->set_callback(bctCustom, 0, 0);
}

bool CStalkerAnimationManager::forward_blend_callbacks() const
{
    if (!m_head_params.m_blend)
        return (false);

    return (m_head_params.m_forward);
}

bool CStalkerAnimationManager::backward_blend_callbacks() const
{
    if (!m_head_params.m_blend)
        return (false);

    return (!m_head_params.m_forward);
}

void CStalkerAnimationManager::clear_unsafe_callbacks()
{
    if (!m_head_params.m_blend)
        return;

    assign_bone_callbacks();
}
