#include "StdAfx.h"
#include "shell_litter.h"

#include "Level.h"
#include "xrEngine/Render.h"
#include "xrEngine/xr_collide_form.h"
#include "xrCDB/xr_collide_defs.h"

extern ENGINE_API int ps_r__shell_decals;

namespace shell_litter
{
namespace
{
struct SPending
{
    Fvector start;
    float land_time;
};
xr_vector<SPending> pending;
// Alive accounting: the wallmark engine owns the visuals and fades them by its own TTL;
// this only keeps the SPAWN rate inside the cap. Conservative fixed lifetime.
constexpr float shell_ttl = 300.f;
xr_vector<float> death_times;

// Function-local statics: a FactoryPtr constructs its render object immediately, so these
// must not be born at DLL load - only on the first landing, when the renderer is alive.
wm_shader* shell_shaders()
{
    static wm_shader shells[4];
    static bool ready = false;
    if (!ready)
    {
        ready = true;
        string64 tex;
        for (u32 i = 0; i < 4; ++i)
        {
            xr_sprintf(tex, "wm" DELIMITER "wm_da_shell_%u", i);
            shells[i]->create("effects" DELIMITER "wallmark", tex);
        }
    }
    return shells;
}

u32 alive_count(float now)
{
    // Compact expired entries in place - the vector stays tiny (cap-bounded).
    size_t w = 0;
    for (size_t r = 0; r < death_times.size(); ++r)
        if (death_times[r] > now)
            death_times[w++] = death_times[r];
    death_times.resize(w);
    return u32(w);
}
} // namespace

void queue(const Fvector& eject_pos, bool hud_mode)
{
    if (ps_r__shell_decals <= 0 || !g_pGameLevel || GEnv.isDedicatedServer)
        return;
    SPending p;
    // HUD weapon particles live in hud space - land the decal from the camera instead;
    // world weapons (NPC, mounted) use the real ejection point.
    if (hud_mode)
        p.start.mad(Device.vCameraPosition, Device.vCameraDirection, 0.25f);
    else
        p.start = eject_pos;
    p.land_time = Device.fTimeGlobal + ::Random.randF(0.25f, 0.55f);
    pending.push_back(p);
}

void update()
{
    if (pending.empty() || !g_pGameLevel)
        return;
    const float now = Device.fTimeGlobal;
    u32 landed = 0;
    for (size_t i = 0; i < pending.size() && landed < 3;)
    {
        if (pending[i].land_time > now)
        {
            ++i;
            continue;
        }
        const SPending p = pending[i];
        pending[i] = pending.back();
        pending.pop_back();
        ++landed;

        if (alive_count(now) >= u32(ps_r__shell_decals))
            continue;

        // Sideways scatter (shells bounce), then a short ray to the floor.
        const float a = ::Random.randF(0.f, PI_MUL_2);
        const float d = _sqrt(::Random.randF()) * ::Random.randF(0.25f, 0.85f);
        Fvector from{p.start.x + _cos(a) * d, p.start.y + 0.2f, p.start.z + _sin(a) * d};
        collide::rq_result rq;
        if (!Level().ObjectSpace.RayPick(
                from, Fvector().set(0.f, -1.f, 0.f), 3.0f, collide::rqtStatic, rq, nullptr))
            continue;

        Fvector point;
        point.mad(from, Fvector().set(0.f, -1.f, 0.f), rq.range - 0.01f);
        CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + rq.element;
        Fvector* verts = Level().ObjectSpace.GetStaticVerts();
        // Steep surfaces do not hold a lying shell.
        Fvector n;
        n.mknormal(verts[tri->verts[0]], verts[tri->verts[1]], verts[tri->verts[2]]);
        if (n.y < 0.55f)
            continue;

        wm_shader& sh = shell_shaders()[::Random.randI(4)];
        GEnv.Render->add_StaticWallmark(sh, point, ::Random.randF(0.075f, 0.095f), tri, verts);
        death_times.push_back(now + shell_ttl);
    }
}
} // namespace shell_litter
