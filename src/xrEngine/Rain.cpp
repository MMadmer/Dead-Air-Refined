#include "stdafx.h"

#include "Rain.h"
#include "IGame_Persistent.h"
#include "Environment.h"

// Rain knobs, see xr_ioc_cmd.cpp.
extern ENGINE_API float ps_r__rain_splash;
extern ENGINE_API float ps_r__rain_splash_time;
// Rain ladder: 1 base, 2 oriented splashes, 3 +streak lighting, 4 full.
extern ENGINE_API int ps_r__rain_quality;

#ifdef _EDITOR
#include "ui_toolscustom.h"
#else
#include "Render.h"
#include "IGame_Level.h"
#include "xrCDB/xr_area.h"
#include "xr_object.h"
#include "xrMaterialSystem/GameMtlLib.h"
#endif

namespace
{
// Rings a landed drop may hand to the eight shared water-impact slots per second, per mm/h of
// rain, and how many the bank may hold. The slots belong to bullets, blasts and bodies as
// well; the bulk of the rain's contribution to the water surface is the ripple field's own
// rate-driven seeding, not this.
} // namespace

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEffect_Rain::CEffect_Rain()
{
    state = stIdle;
    rain_volume = 0.f;
    rain_volume_snd = 0.f;
    rain_exposure = 0.f;
    hemi_factor = 0.f;

    // Open sky until the probe says otherwise: a level that starts in the rain must not have
    // its first second silent while the rays come in.
    cover_factor = 1.f;
    cover_ray = 0;
    cover_lean = 0.f;
    cover_heading = 0.f;
    for (float& r : cover_range)
        r = -1.f;
    for (s32& m : cover_mtl)
        m = -1;
    for (bool& open : cover_open)
        open = true;

    snd_Ambient.create("ambient" DELIMITER "rain", st_Effect, sg_Undefined);

    p_create();
}

CEffect_Rain::~CEffect_Rain()
{
    snd_Ambient.destroy();
    rain_volume = 0.f;
    rain_volume_snd = 0.f;
    rain_exposure = 0.f;

    // Cleanup
    p_destroy();
}

void CEffect_Rain::InvalidateState()
{
    state = stIdle;
    rain_volume = 0.f;
    rain_volume_snd = 0.f;
    rain_exposure = 0.f;
    hemi_factor = 0.f;
    snd_Ambient.stop();

    cover_factor = 1.f;
    cover_ray = 0;
    cover_lean = 0.f;
    cover_heading = 0.f;
    for (float& r : cover_range)
        r = -1.f;
    for (s32& m : cover_mtl)
        m = -1;
    for (bool& open : cover_open)
        open = true;

    for (Item& item : items)
        item.invalidate();

    p_destroy();
    p_create();
}

// Born
void CEffect_Rain::Born(Item& dest, float radius)
{
    ZoneScoped;

    Fvector axis;
    axis.set(0, -1, 0);
    // Slant follows the effective-wind service, so the rain leans exactly where the grass bends
    // and the puddle ripples drift - and it breathes with the same lulls and gusts. The angle
    // is the physical one: a falling drop drifts sideways at the wind speed, so the streak
    // tilts by atan(wind / fall). eff_wind_norm maps to ~11 m/s at full storm - the same
    // scale the bullet wind-drift uses.
    const auto& env = g_pGamePersistent->Environment();
    const float wind_ms = env.eff_wind_norm * 11.f * (0.55f + 0.45f * env.eff_wind_gust);
    float slant = atanf(wind_ms / da_rain::fall_ms);
    clamp(slant, 0.f, da_rain::max_slant);
    // Downwind is (sin dir, 0, cos dir) - the convention the service (WindAt), the grass, the
    // trees, the puddles and the clouds share. setHP's heading runs the other way round on x,
    // so the rain fell MIRRORED: dead on at north and south, ninety degrees off on the
    // diagonals, against the wind at east and west.
    const float sl = _sin(slant), cl = _cos(slant);
    axis.set(_sin(env.eff_wind_dir) * sl, -cl, _cos(env.eff_wind_dir) * sl);

    // Landing-disc spawn (the NVIDIA rain-SDK camera-volume idea): pick where the streak's
    // LINE crosses eye level inside the radius, then walk back up the fall axis. That keeps
    // the player inside the sheet at ANY slant - the old top-disc spawn built a vertical
    // column whose sheared edge read as a rain shaft in the sky the moment the slant grew
    // real.
    Fvector& view = Device.vCameraPosition;
    const float angle = ::Random.randF(0, PI_MUL_2);
    const float dist = _sqrt(::Random.randF()) * radius;
    dest.D.random_dir(axis, da_rain::spread);
    Fvector land;
    land.set(view.x + dist * _cos(angle), view.y, view.z + dist * _sin(angle));

    const float inv_fall = 1.f / std::max(-dest.D.y, 0.4f);
    const float up_range = da_rain::source_offset * inv_fall;
    dest.fSpeed = ::Random.randF(da_rain::speed_min, da_rain::speed_max);

    // THE cover test. Probe the whole column, and probe it from the CLOUD END down: the first
    // thing the ray meets is the roof over this spot, and everything below that is indoors.
    // The old code drew the spawn height first and probed from there, so a drop born under a
    // ceiling never learned there was one - that, and nothing else, is the indoor drizzle.
    // It is the same single ray the birth always paid for.
    Fvector top;
    top.mad(land, dest.D, -up_range);
    float range = up_range + (da_rain::max_distance - da_rain::source_offset) * inv_fall;
    const bool hit = RayPickEx(top, dest.D, range, collide::rqtBoth, dest.Nhit, dest.mtl_hit);

    // Blocked at the very top means the column's own start is inside geometry - deep indoors,
    // or under the floor above. Park the item rather than burn a ray on it every single frame.
    dest.sheltered = hit && range < 0.5f;
    if (dest.sheltered)
    {
        dest.P = top;
        dest.Phit = top;
        dest.uv_set = 0;
        dest.dwTime_Life = Device.dwTimeGlobal + ::Random.randI(150, 450);
        dest.dwTime_Hit = dest.dwTime_Life + 1000;
        return;
    }

    // Spawn uniformly in the CLEAR part of the column. Under open sky that is the same
    // eye-to-forty-metres spread as before; under a roof it is the slice above the roof, so
    // the sheet keeps its density outdoors and simply stops existing indoors.
    const float clear = std::min(range, up_range);
    const float down = ::Random.randF(0.f, clear);
    dest.P.mad(top, dest.D, down);
    RenewItem(dest, range - down, hit);
}

bool CEffect_Rain::RayPick(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt)
{
    Fvector n;
    s32 mtl;
    return RayPickEx(s, d, range, tgt, n, mtl);
}

// What the rain goes through. A tree crown, a bush, an occluder, a kill volume, an invisible
// wall: every one of them is PASSABLE in the game's own material terms - a bullet does not stop
// there - and neither does a drop. Water is passable too and stays a landing, because a drop
// that reaches it rings it. No material is named here; the flag is the game's, so a mod's own
// foliage answers the same way as the stock poplar whose crown is a "bush".
static bool da_rain_passes(s32 mtl)
{
    if (mtl < 0 || mtl >= s32(GMLib.CountMaterial()))
        return false;
    const SGameMtl* m = GMLib.GetMaterialByIdx(u16(mtl));
    return m && m->Flags.test(SGameMtl::flPassable) && !m->Flags.test(SGameMtl::flLiquid);
}

// The query the drop always ran, keeping what it always threw away. There is no second ray
// here: the collider filled the normal's triangle and the material in on the way past - except
// through foliage, where the ray carries on from the far side, because what stopped it was not
// a surface the water lands on. A player standing next to a poplar in a storm watched the
// visor dry and the rain in the air vanish whenever the wind leaned the sky probe into its
// crown: five rays, all "sheltered", by a "bush" sixteen metres up.
bool CEffect_Rain::RayPickEx(
    const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt, Fvector& normal, s32& material)
{
    ZoneScoped;
#ifdef _EDITOR
    normal.set(0.f, 1.f, 0.f);
    material = -1;
    Tools->RayPick(s, d, range);
    return true;
#else
    return RayPickThrough(s, d, range, tgt, normal, material, g_pGameLevel->CurrentViewEntity());
#endif
}

bool CEffect_Rain::SkyOpen(const Fvector& from, const Fvector& dir, float range)
{
#ifdef _EDITOR
    return true;
#else
    if (!g_pGameLevel)
        return true;
    Fvector n;
    s32 mtl = -1;
    return !RayPickThrough(from, dir, range, collide::rqtStatic, n, mtl, nullptr);
#endif
}

bool CEffect_Rain::RayPickThrough(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt,
    Fvector& normal, s32& material, IGameObject* ignore)
{
    normal.set(0.f, 1.f, 0.f);
    material = -1;
#ifdef _EDITOR
    return false;
#else
    IGameObject* E = ignore;
    Fvector from = s;
    float left = range, gone = 0.f;
    // A crown is one hull and a copse is several; past this many the column counts as open.
    for (int pass = 0; pass < 4; ++pass)
    {
        collide::rq_result RQ;
        if (!g_pGameLevel->ObjectSpace.RayPick(from, d, left, tgt, RQ, E))
            return false;

        // A dynamic object reports a bone, not a triangle, and has no cheap normal - a crown on
        // an NPC's shoulder keeps the world-up default.
        if (!RQ.O)
        {
            const CDB::TRI* T = g_pGameLevel->ObjectSpace.GetStaticTris() + RQ.element;
            const Fvector* V = g_pGameLevel->ObjectSpace.GetStaticVerts();
            normal.mknormal(V[T->verts[0]], V[T->verts[1]], V[T->verts[2]]);
            // Winding is the level compiler's business, not ours: the face the ray met is the one
            // turned toward it.
            if (normal.dotproduct(d) > 0.f)
                normal.invert();
            material = s32(T->material);

            if (da_rain_passes(material))
            {
                // Carry on from just past the face the water does not land on.
                const float step = RQ.range + 0.05f;
                gone += step;
                left -= step;
                if (left <= 0.f)
                    return false;
                from.mad(from, d, step);
                normal.set(0.f, 1.f, 0.f);
                material = -1;
                continue;
            }
        }
        range = gone + RQ.range;
        return true;
    }
    return false;
#endif
}

void CEffect_Rain::RenewItem(Item& dest, float height, bool bHit)
{
    dest.uv_set = Random.randI(2);
    if (bHit)
    {
        dest.dwTime_Life = Device.dwTimeGlobal + iFloor(1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.dwTime_Hit = Device.dwTimeGlobal + iFloor(1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.Phit.mad(dest.P, dest.D, height);
    }
    else
    {
        dest.dwTime_Life = Device.dwTimeGlobal + iFloor(1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.dwTime_Hit = Device.dwTimeGlobal + iFloor(2 * 1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.Phit.set(dest.P);
    }
}

// Liquid by the game material the birth ray reported, or inside the level's baked water field.
// Two lookups and no query - the ray already answered the first and the field is an index.
bool CEffect_Rain::SurfaceIsWater(const Fvector& pos, s32 mtl) const
{
#ifndef _EDITOR
    if (mtl >= 0 && mtl < s32(GMLib.CountMaterial()))
        if (const SGameMtl* m = GMLib.GetMaterialByIdx(u16(mtl)); m && m->Flags.test(SGameMtl::flLiquid))
            return true;

    const auto& env = g_pGamePersistent->Environment();
    if (env.water_field_valid())
    {
        const float surface = env.water_surface_at(pos);
        // Just under the surface counts: the water mesh is passable, so a drop that reaches
        // the bed reports the bed's material.
        if (surface > -flt_max && pos.y < surface + 0.25f)
            return true;
    }
#endif
    return false;
}

// The sky-cover probe.
//
// The stock oracle was the view entity's LIGHTING hemi-cube. It is accumulated from every lamp
// and campfire nearby as well as from the sky, and it only re-traces when the player moves -
// once you stand still it holds whatever it had for sixteen to thirty-three seconds. So the
// rain grew louder in a lit bunker, went quiet outdoors at night, and froze the moment you
// stepped into a doorway.
//
// This asks the question the rain actually has: can water reach me from the sky, along the path
// it takes. Five static rays up the fall axis, one per frame, round robin - five a second beats
// twenty-six every half minute for an answer that changes when you walk through a door.
void CEffect_Rain::CoverTick(float dt)
{
#ifndef _EDITOR
    if (!g_pGameLevel)
        return;

    const auto& env = g_pGamePersistent->Environment();
    const float wind_ms = env.eff_wind_norm * 11.f * (0.55f + 0.45f * env.eff_wind_gust);
    float slant = atanf(wind_ms / da_rain::fall_ms);
    clamp(slant, 0.f, da_rain::cover_slant);
    const float sl = _sin(slant), cl = _cos(slant);
    // The fall axis reversed: where the drops come FROM, leaned less than the drops are.
    Fvector sky;
    sky.set(-_sin(env.eff_wind_dir) * sl, cl, -_cos(env.eff_wind_dir) * sl);

    Fvector start = Device.vCameraPosition;
    start.y += 0.2f;

    const u32 idx = cover_ray % da_rain::cover_rays;
    Fvector dir = sky;
    if (idx)
    {
        // Four rays spread off the axis so a doorway, an archway or a window reads as partial
        // cover instead of a hard on/off.
        Fvector a, b;
        Fvector::generate_orthonormal_basis(sky, a, b);
        const float ang = PI_DIV_2 * float(idx - 1);
        Fvector off;
        off.set(0.f, 0.f, 0.f);
        off.mad(a, _cos(ang));
        off.mad(b, _sin(ang));
        dir.mad(sky, off, da_rain::cover_spread);
        dir.normalize_safe(sky);
    }

    // The drop's own query, so the visor and the sheet agree on what a roof is: foliage is not
    // one, and the ray goes on through it.
    float reach = da_rain::cover_range;
    Fvector n;
    s32 mtl = -1;
    const bool blocked = RayPickEx(start, dir, reach, collide::rqtStatic, n, mtl);
    cover_open[idx] = !blocked;
    cover_range[idx] = blocked ? reach : -1.f;
    cover_mtl[idx] = blocked ? mtl : -1;
    cover_lean = slant;
    cover_heading = env.eff_wind_dir;
    cover_ray++;

    // The axis itself counts double - that is the direction the water actually arrives from.
    float open = cover_open[0] ? 2.f : 0.f;
    for (int i = 1; i < da_rain::cover_rays; ++i)
        open += cover_open[i] ? 1.f : 0.f;
    const float target = open / float(da_rain::cover_rays + 1);

    float t = dt;
    clamp(t, 0.001f, 1.0f);
    cover_factor = cover_factor * (1.0f - t) + target * t;
#endif
}

// The lighting hemi-cube of the view entity, exactly as the stock rain read it: the maximum
// over the five upper faces, smoothed with a one-pole on the frame time. It is a bad cover
// oracle - every lamp and campfire feeds it, and it only re-traces when the player moves - so
// nothing in the engine uses it any more. It survives because level.get_rain_volume() is a
// shipped script export whose consumers were tuned against it; see rain_volume in Rain.h.
void CEffect_Rain::HemiTick(float dt)
{
#ifndef _EDITOR
    IGameObject* E = g_pGameLevel->CurrentViewEntity();
    if (!E || !E->renderable_ROS())
        return;

    const float* hemi_cube = E->renderable_ROS()->get_luminocity_hemi_cube();
    float hemi_val = _max(hemi_cube[0], hemi_cube[1]);
    hemi_val = _max(hemi_val, hemi_cube[2]);
    hemi_val = _max(hemi_val, hemi_cube[3]);
    hemi_val = _max(hemi_val, hemi_cube[5]);
    clamp(hemi_val, 0.f, 1.f);

    float t = dt;
    clamp(t, 0.001f, 1.0f);
    hemi_factor = hemi_factor * (1.0f - t) + hemi_val * t;
#endif
}

void CEffect_Rain::OnFrame()
{
    ZoneScoped;

#ifndef _EDITOR
    if (!g_pGameLevel)
        return;
#endif

    if (GEnv.isDedicatedServer)
        return;

    auto& env = g_pGamePersistent->Environment();
    const float factor = env.CurrentEnv.rain_density;

    if (factor >= EPS_L)
    {
        CoverTick(Device.fTimeDelta);
        HemiTick(Device.fTimeDelta);
    }

    switch (state)
    {
    case stIdle:
        if (factor < EPS_L)
            return;
        state = stWorking;
        snd_Ambient.play(0, sm_Looped);
        snd_Ambient.set_position(Fvector().set(0, 0, 0));
        snd_Ambient.set_range(da_rain::source_offset, da_rain::source_offset * 2.f);
        break;
    case stWorking:
        if (factor < EPS_L)
        {
            state = stIdle;
            snd_Ambient.stop();
            rain_volume = 0.f;
            rain_volume_snd = 0.f;
            rain_exposure = 0.f;
            return;
        }
        break;
    }

    // The rain at the head, for what the rain physically wets. Not gated on the sound: a bed
    // that has lost its feedback must not leave the visor dry.
    rain_exposure = clampr(factor * cover_factor, 0.f, 1.f);

    // ambient sound
    if (snd_Ambient._feedback())
    {
        const float cover_snd = da_rain::cover_snd_floor + (1.f - da_rain::cover_snd_floor) * cover_factor;
        rain_volume_snd = factor * cover_snd;
        clamp(rain_volume_snd, 0.f, 1.f);
        snd_Ambient.set_volume(rain_volume_snd);

        // The script-facing value keeps the daylight term the old one had, and the raw cover
        // with it - the floor above belongs to the ambient bed alone. Indoors it is strictly
        // lower (cover closes), outdoors in daylight it is unchanged, and at night it still
        // falls away - so no shipped script sees a storm it has never seen before.
        rain_volume = factor * cover_factor * hemi_factor;
        clamp(rain_volume, 0.f, 1.f);
    }
}

void CEffect_Rain::Render()
{
#ifndef _EDITOR
    if (!g_pGameLevel)
        return;
#endif

    m_pRender->Render(*this);
}

// The stock entry point: a splash at a point with nothing known about what is under it. Kept
// because that is what the exported symbol means; the simulation goes through HitItem.
void CEffect_Rain::Hit(Fvector& pos)
{
    Fvector up;
    up.set(0.f, 1.f, 0.f);
    Splash(pos, up, -1);
}

void CEffect_Rain::HitItem(Item& src)
{
    // Where the drop's CURRENT line crosses the surface the birth ray found. Two corrections in
    // one: the frame the timer fires in has already carried the drop up to a metre past the
    // ground, and the wind steers it in flight now, so its path is no longer the straight line
    // the landing point was solved on. A grazing angle falls back to that landing point.
    Fvector pos = src.Phit;
    const float denom = src.D.dotproduct(src.Nhit);
    if (denom < -0.1f)
    {
        Fvector to_hit;
        to_hit.sub(src.Phit, src.P);
        pos.mad(src.P, src.D, to_hit.dotproduct(src.Nhit) / denom);
    }
    Splash(pos, src.Nhit, src.mtl_hit);
}

// startup _new_ particle system
void CEffect_Rain::Splash(const Fvector& pos, const Fvector& n, s32 mtl)
{
    // Was a hard `if (0 != Random.randI(2)) return;` - half the drops landed without a
    // trace, for no reason and with no way to change it. The share is a knob now, default
    // every drop: splashes, not the drops themselves, are what shows rain hitting GROUND.
    if (::Random.randF() > ps_r__rain_splash)
        return;

    const int tier = ps_r__rain_quality;

    if (tier >= 2)
    {
        // Water takes a ring, not a crown: the twelve-triangle cone is what water does when it
        // is a few millimetres deep over dirt. The ring itself is NOT fed from here. It used to
        // go through Environment::water_hit, and there are eight of those slots for the whole
        // level: a shower filled them several times a second, and every bullet's ring was
        // evicted mid-life to make room for a drop - a wave that simply vanished. Rain on open
        // water rings through the ripple field's own rate-driven seeding and the rain layer of
        // the surface normal; rain on a puddle rings through the puddle shader's drop atlas.
        // Both are driven by the rate in mm/h and neither costs a slot.
        if (SurfaceIsWater(pos, mtl))
            return;
        // A crown needs ground under it. On a wall or a steep roof pitch the drop runs off; the
        // stock code stood one up regardless, and world-up, wherever the ray happened to graze.
        if (n.y < da_rain::crown_min_ny)
            return;
    }

    Particle* P = p_allocate();
    if (0 == P)
        return;

    const Fsphere& bv_sphere = m_pRender->GetDropBounds();

    // The knob trims inside the physical envelope rather than setting it: a crown that stands
    // at full size for a third of a second is a decal, whatever the console says.
    P->life = clampr(ps_r__rain_splash_time, da_rain::crown_life_min, da_rain::crown_life_max) *
        ::Random.randF(0.85f, 1.2f);
    P->time = P->life;
    P->size = ::Random.randF(0.70f, 1.35f);

    if (tier >= 2)
    {
        // Stand the crown ON the surface it hit, with a random yaw about that surface's normal.
        Fvector up = n;
        up.normalize_safe(Fvector().set(0.f, 1.f, 0.f));
        Fvector a, b;
        Fvector::generate_orthonormal_basis(up, a, b);
        const float yaw = ::Random.randF(PI_MUL_2);
        Fvector x, z;
        x.set(0.f, 0.f, 0.f);
        x.mad(a, _cos(yaw));
        x.mad(b, _sin(yaw));
        z.crossproduct(x, up);
        P->mXForm.identity();
        P->mXForm.i.set(x);
        P->mXForm.j.set(up);
        P->mXForm.k.set(z);
    }
    else
        P->mXForm.rotateY(::Random.randF(PI_MUL_2));

    P->mXForm.translate_over(pos);
    P->mXForm.transform_tiny(P->bounds.P, bv_sphere.P);
    P->bounds.R = bv_sphere.R;
}

// initialize particles pool
void CEffect_Rain::p_create()
{
    // pool
    particle_pool.resize(da_rain::max_particles);
    for (size_t it = 0; it < particle_pool.size(); it++)
    {
        Particle& P = particle_pool[it];
        P.prev = it ? (&particle_pool[it - 1]) : 0;
        P.next = (it < (particle_pool.size() - 1)) ? (&particle_pool[it + 1]) : 0;
        P.time = 0.f;
        P.life = 1.f;
        P.size = 1.f;
    }

    // active and idle lists
    particle_active = 0;
    particle_idle = &particle_pool.front();
}

// destroy particles pool
void CEffect_Rain::p_destroy()
{
    // active and idle lists
    particle_active = 0;
    particle_idle = 0;

    // pool
    particle_pool.clear();
}

// _delete_ node from _list_
void CEffect_Rain::p_remove(Particle* P, Particle*& LST)
{
    VERIFY(P);
    Particle* prev = P->prev;
    P->prev = NULL;
    Particle* next = P->next;
    P->next = NULL;
    if (prev)
        prev->next = next;
    if (next)
        next->prev = prev;
    if (LST == P)
        LST = next;
}

// insert node at the top of the head
void CEffect_Rain::p_insert(Particle* P, Particle*& LST)
{
    VERIFY(P);
    P->prev = 0;
    P->next = LST;
    if (LST)
        LST->prev = P;
    LST = P;
}

// determine size of _list_
int CEffect_Rain::p_size(Particle* P)
{
    if (0 == P)
        return 0;
    int cnt = 0;
    while (P)
    {
        P = P->next;
        cnt += 1;
    }
    return cnt;
}

// alloc node
CEffect_Rain::Particle* CEffect_Rain::p_allocate()
{
    Particle* P = particle_idle;
    if (0 == P)
        return NULL;
    p_remove(P, particle_idle);
    p_insert(P, particle_active);
    return P;
}

// xr_free node
void CEffect_Rain::p_free(Particle* P)
{
    p_remove(P, particle_active);
    p_insert(P, particle_idle);
}
