// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "Actor.h"
#include "attachable_item.h"
#include "CustomOutfit.h"
#include "HudItem.h"
#include "Include/xrRender/Kinematics.h"
#include "Inventory.h"
#include "Level.h"

namespace
{
// The caster is a different mesh on the same rig, so every bone takes its matrix straight from
// the actor's own skeleton. No second animation is played and the two poses cannot drift apart.
void shadow_bone_callback(CBoneInstance* bone)
{
    const auto* link = static_cast<const CActor::shadow_bone*>(bone->callback_param());
    bone->mTransform.set(link->source->LL_GetTransform(link->source_id));
}

// Visual names reach the engine both with and without the extension, so comparing two of them
// needs one common form.
shared_str normalized_visual(const shared_str& name)
{
    if (!name.size())
        return name;

    string_path stripped;
    xr_strcpy(stripped, name.c_str());
    if (pstr extension = strext(stripped))
        *extension = 0;
    xr_strlwr(stripped);
    return stripped;
}
} // namespace

// The model that should be casting. What the player wears in first person is a legs-only mesh -
// with an outfit on it comes from the outfit, and with nothing on it is still the legs model the
// character description names - so the caster is the third-person model of the same look. The
// rule follows the one the corpse uses in CActor::Die: the outfit's own npc_visual, else the
// neutral stalker. A mod that needs something else points [actor] shadow_visual at its model.
shared_str CActor::shadow_caster_visual() const
{
    shared_str name;

    const CCustomOutfit* outfit = GetOutfit();
    if (outfit && pSettings->line_exist(outfit->cNameSect(), "npc_visual"))
        name = pSettings->r_string(outfit->cNameSect(), "npc_visual");
    else if (pSettings->line_exist(cNameSect(), "shadow_visual"))
        name = pSettings->r_string(cNameSect(), "shadow_visual");
    else
        name = "actors\\stalker_neutral\\stalker_neutral_1";

    // Nothing to duplicate when the visible model is the complete one already.
    if (!name.size() || normalized_visual(name) == normalized_visual(cNameVisual()))
        return nullptr;

    return name;
}

void CActor::bind_shadow_caster(IKinematics* body)
{
    IKinematics* caster = smart_cast<IKinematics*>(m_shadow_caster);
    if (!caster)
    {
        drop_shadow_caster();
        return;
    }

    const u16 count = caster->LL_BoneCount();
    m_shadow_bones.resize(count);
    for (u16 bone_id = 0; bone_id < count; ++bone_id)
        m_shadow_bones[bone_id] = { body, body->LL_BoneID(caster->LL_BoneName_dbg(bone_id)) };

    // The links are handed out by address, so the callbacks are only assigned once the vector
    // holds its final storage. A bone the actor's rig does not have keeps its own bind pose and
    // simply follows its parent.
    for (u16 bone_id = 0; bone_id < count; ++bone_id)
    {
        if (m_shadow_bones[bone_id].source_id == BI_NONE)
            continue;

        caster->LL_GetBoneInstance(bone_id).set_callback(
            bctCustom, shadow_bone_callback, &m_shadow_bones[bone_id], TRUE);
    }
}

void CActor::collect_shadow_items()
{
    m_shadow_items.clear();

    // Item transforms are baked here rather than inside the passes: shadow maps are built from
    // several contexts at once and a pass must not write to shared object state.
    if (PIItem active = inventory().ActiveItem())
    {
        CHudItem* hud_item = active->cast_hud_item();
        if (hud_item && !hud_item->IsHidden())
        {
            hud_item->UpdateXForm();
            if (IRenderVisual* visual = active->object().Visual())
                m_shadow_items.push_back({ visual, active->object().XFORM() });
        }
    }

    // Attached gear rides on bones the refresh above has just recalculated, so its world
    // transform is current and nothing is left hanging at a stale pose.
    for (const auto* attached : attached_objects())
    {
        IGameObject& object = attached->item().object();
        if (IRenderVisual* visual = object.Visual())
            m_shadow_items.push_back({ visual, object.XFORM() });
    }
}

void CActor::update_shadow_caster(bool enabled)
{
    const bool needed =
        enabled && g_Alive() && !m_holder && Level().CurrentViewEntity() == this && HUDview();
    if (!needed)
    {
        drop_shadow_caster();
        return;
    }

    IKinematics* body = smart_cast<IKinematics*>(Visual());
    if (!body)
    {
        drop_shadow_caster();
        return;
    }

    // The caster, the weapon transform and the attachment callbacks all read the actor's pose,
    // and the main pass builds its graph in parallel with the shadow maps. So it is refreshed
    // here instead: once per frame, on the main thread, before any graph is built.
    body->CalculateBones(TRUE);

    const shared_str name = shadow_caster_visual();
    if (name != m_shadow_caster_name)
        drop_shadow_caster();

    if (!m_shadow_caster && name.size())
    {
        m_shadow_caster = GEnv.Render->model_Create(name.c_str());
        m_shadow_caster_name = name;
        bind_shadow_caster(body);
    }

    collect_shadow_items();
}

void CActor::render_shadow_caster(u32 context_id, IRenderable* root, const Fvector& source)
{
    // A light sitting on the actor itself - the torch above all - cannot be shadowed by the body
    // carrying it, and letting it try only paints the beam with the player's own silhouette. The
    // sun and every lamp in the world are far outside this radius.
    Fvector center;
    Center(center);
    if (center.distance_to(source) < Radius())
        return;

    IRenderVisual* body = m_shadow_caster ? m_shadow_caster : Visual();
    if (body)
        GEnv.Render->add_Visual(context_id, root, body, XFORM());

    for (auto& item : m_shadow_items)
        GEnv.Render->add_Visual(context_id, root, item.visual, item.xform);
}

void CActor::drop_shadow_caster()
{
    m_shadow_bones.clear();
    m_shadow_items.clear();
    m_shadow_caster_name = nullptr;

    if (m_shadow_caster)
    {
        GEnv.Render->model_Delete(m_shadow_caster);
        m_shadow_caster = nullptr;
    }
}
