// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "ph_ice.h"
#include "xrEngine/da_ice.h"
#include "xrMaterialSystem/GameMtlLib.h"

namespace
{
// How far a body may reach under the surface and still be lying on the ice: a boot's sole on it, a body's curve. Deeper
// is a body standing in the water through it.
constexpr float ice_contact_depth = 0.25f;
} // namespace

u16 ph_contact_material(u16 material, const float* pos, float depth)
{
    const SGameMtl* m = GMLib.GetMaterialByIdx(material);
    if (!m || !m->Flags.test(SGameMtl::flLiquid))
        return material;
    const u16 ice = da_ice_material();
    if (ice == u16(GAMEMTL_NONE_IDX) || depth > ice_contact_depth)
        return material;
    return da_ice_holds(Fvector().set(pos[0], pos[1], pos[2])) ? ice : material;
}
