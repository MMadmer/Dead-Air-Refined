#include "StdAfx.h"
#include "Level.h"
#include "xrEngine/xr_object.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Include/xrRender/Kinematics.h"
#include "raypick.h"
#include "Level.h"

CRayPick::CRayPick()
{
    start_position.set(0, 0, 0);
    direction.set(0, 0, 0);
    range = 0;
    flags = collide::rq_target::rqtNone;
    ignore = nullptr;
};

CRayPick::CRayPick(const Fvector& P, const Fvector& D, const float R, const collide::rq_target F, CScriptGameObject* I)
{
    start_position.set(P);
    direction.set(D);
    range = R;
    flags = F;
    ignore = nullptr;
    if (I)
        ignore = smart_cast<IGameObject*>(&(I->object()));
};

bool CRayPick::query()
{
    collide::rq_result R;
    if (Level().ObjectSpace.RayPick(start_position, direction, range, flags, R, ignore))
    {
        result.set(R);
        return true;
    }

    return false;
}

void script_rq_result::set(collide::rq_result& R)
{
    IGameObject* go = R.O ? smart_cast<IGameObject*>(R.O) : nullptr;
    if (go)
        O = go->lua_game_object();
    range = R.range;
    element = R.element;

    const SGameMtl* mtl = nullptr;
    if (!R.O)
    {
        if (R.element >= 0)
        {
            const CDB::TRI& tri = Level().ObjectSpace.GetStaticTris()[R.element];
            mtl = GMLib.GetMaterialByIdx(u16(tri.material));
        }
    }
    else if (go && go->Visual())
    {
        if (IKinematics* k = go->Visual()->dcast_PKinematics())
            if (R.element >= 0 && u16(R.element) < k->LL_BoneCount())
                mtl = GMLib.GetMaterialByIdx(k->LL_GetData(u16(R.element)).game_mtl_idx);
    }
    material_name = mtl ? mtl->m_Name.c_str() : "";
    material_flags = mtl ? mtl->Flags.get() : 0;
    material_shoot_factor = mtl ? mtl->fShootFactor : 0.f;
}
