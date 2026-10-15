// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The ice under the actor and under fire (docs/dead-air/WEATHER.md, ice). See da_ice_game.h.

#include "StdAfx.h"
#include "da_ice_game.h"
#include "Actor.h"
#include "Level.h"
#include "ParticlesObject.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "xrEngine/da_ice.h"
#include "xrEngine/da_climate.h"
#include "xrEngine/da_particle_suppress.h"
#include "xrEngine/Environment.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "GamePersistent.h"

namespace
{
// A set of sounds named in a comma list of [ice] (dead_air_x64_water.ltx), created on first use.
struct SSoundSet
{
    xr_vector<ref_sound> sounds;
    bool made = false;

    void make(const shared_str& list)
    {
        if (made)
            return;
        made = true;
        if (!list.size())
            return;
        const int n = _GetItemCount(list.c_str());
        string256 item;
        for (int i = 0; i < n; ++i)
        {
            _GetItem(list.c_str(), i, item);
            sounds.emplace_back().create(item, st_Effect, sg_SourceType);
        }
    }

    void play(const Fvector& at, float volume)
    {
        if (sounds.empty())
            return;
        Fvector p = at;
        float v = volume;
        sounds[::Random.randI(int(sounds.size()))].play_no_feedback(nullptr, 0, 0.f, &p, &v);
    }
};

SSoundSet s_crack, s_break, s_bullet_thin;

void make_sounds()
{
    const SDaIceCfg& cfg = da_ice_cfg();
    s_crack.make(cfg.snd_crack);
    s_break.make(cfg.snd_break);
    s_bullet_thin.make(cfg.snd_bullet_thin);
}

void particles(const shared_str& name, const Fvector& at)
{
    if (!name.size() || da_particle_suppressed(name.c_str()))
        return;
    CParticlesObject* ps = CParticlesObject::Create(name.c_str(), TRUE);
    Fmatrix xf;
    xf.identity();
    xf.c.set(at);
    ps->UpdateParent(xf, zero_vel);
    ps->Play(false);
}

// The ice gives way at a point of the surface: a hole of open water, the crash, the shards and the splash, the ring.
void break_at(const Fvector& surface, float radius, float volume)
{
    make_sounds();
    const SDaIceCfg& cfg = da_ice_cfg();
    da_ice_hole_add(surface, radius, 0);
    s_break.play(surface, volume);
    particles(cfg.ps_break, surface);
    particles(cfg.ps_splash, surface);
    g_pGamePersistent->Environment().water_hit(surface, radius * 1.6f, CEnvironment::EWaterHit::ring);
}

// The actor on the ice: the weight on weak ice, the cracks it makes, the break; the channel a man in the water breaks.
struct SActorIce
{
    float load = 0.f;       // s of weight on weak ice
    float next_crack = 0.f; // s to the next crack while on it
    float creak = 8.f;      // s to the next creak of sound ice under a walk
    Fvector last{};         // feet last frame
    Fvector trail{};        // where the channel last broke
    bool has_last = false, has_trail = false;
    float vy = 0.f;         // vertical speed, smoothed
};
SActorIce s_actor;
} // namespace

void da_ice_actor_update(CActor& actor)
{
    const SDaIceCfg& cfg = da_ice_cfg();
    if (!g_pGamePersistent || !da_climate_enabled() || !actor.g_Alive())
    {
        s_actor = {};
        return;
    }
    auto& env = g_pGamePersistent->Environment();
    const Fvector feet = actor.Position();
    const float dt = Device.fTimeDelta;
    if (s_actor.has_last && dt > 0.f && dt < 0.5f)
    {
        // A teleport or a load is not a fall.
        const float vy = (feet.y - s_actor.last.y) / dt;
        s_actor.vy = feet.similar(s_actor.last, 3.f) ? vy : 0.f;
    }
    s_actor.last = feet;
    s_actor.has_last = true;

    float surface, bed;
    if (!env.water_at(feet, surface, bed) || feet.y > surface + 0.4f)
    {
        s_actor.load = std::max(s_actor.load - dt, 0.f);
        s_actor.has_trail = false;
        return;
    }
    Fvector at = feet;
    at.y = surface;
    if (env.ice_at(at) <= 0.5f)
    {
        s_actor.has_trail = false;
        return;
    }
    make_sounds();
    float strength = 0.f;
    const EDaIce state = da_ice_at(at, &strength);

    // In the water under the ice's plane (a man who went through, or wading where it never held): he breaks a
    // channel as he goes, thin ice crunching, thick ice in slabs.
    if (feet.y < surface - 0.15f)
    {
        if (!s_actor.has_trail || Fvector2().set(feet.x - s_actor.trail.x, feet.z - s_actor.trail.z).magnitude() > 0.45f)
        {
            da_ice_hole_add(at, cfg.trail_radius, 0);
            s_break.play(at, 0.35f + 0.4f * clampr(strength / cfg.safe, 0.f, 1.f));
            particles(cfg.ps_break, at);
            s_actor.trail = feet;
            s_actor.has_trail = true;
        }
        s_actor.load = 0.f;
        return;
    }
    s_actor.has_trail = false;

    // A hole opened under a man standing still: his physics sleeps on the ice it stood on until something wakes it.
    const auto wake = [&actor] {
        if (actor.character_physics_support() && actor.character_physics_support()->movement())
            actor.character_physics_support()->movement()->EnableCharacter();
    };
    float speed = 0.f;
    if (actor.character_physics_support() && actor.character_physics_support()->movement())
    {
        const Fvector& v = actor.character_physics_support()->movement()->GetVelocity();
        speed = Fvector2().set(v.x, v.z).magnitude();
    }

    switch (state)
    {
    case da_ice_thin:
        // Nothing to stand on: the physics lets him through; the ice breaks where he went in.
        break_at(at, cfg.hole_radius, 1.f);
        wake();
        s_actor.load = 0.f;
        break;
    case da_ice_weak:
    {
        // A landing hits it at once; a walk loads it, a run more, and it cracks louder as it goes.
        const float limit = cfg.crack_min + (cfg.crack_max - cfg.crack_min) *
            clampr((strength - cfg.hold) / std::max(cfg.safe - cfg.hold, 1e-3f), 0.f, 1.f);
        s_actor.load += dt * (1.f + 0.25f * speed);
        if (s_actor.vy < -4.f)
            s_actor.load = limit;
        s_actor.next_crack -= dt;
        if (s_actor.next_crack <= 0.f)
        {
            const float k = clampr(s_actor.load / limit, 0.f, 1.f);
            s_crack.play(at, 0.4f + 0.6f * k);
            s_actor.next_crack = (1.2f - 0.9f * k) * ::Random.randF(0.6f, 1.3f);
        }
        if (s_actor.load >= limit)
        {
            break_at(at, cfg.hole_radius, 1.f);
            wake();
            s_actor.load = 0.f;
        }
        break;
    }
    case da_ice_solid:
        // Sound ice that is not much past what carries a man still talks under a walk: a creak now and then.
        s_actor.load = 0.f;
        if (strength < cfg.safe * 2.f && speed > 0.5f)
        {
            s_actor.creak -= dt;
            if (s_actor.creak <= 0.f)
            {
                s_crack.play(at, 0.25f);
                s_actor.creak = ::Random.randF(6.f, 18.f);
            }
        }
        break;
    default: s_actor.load = 0.f; break;
    }
}

u16 da_ice_bullet_material(u16 material, const Fvector& p)
{
    const SGameMtl* m = GMLib.GetMaterialByIdx(material);
    if (!m || !m->Flags.test(SGameMtl::flLiquid))
        return material;
    const u16 ice = da_ice_material();
    return ice != u16(GAMEMTL_NONE_IDX) && da_ice_holds(p) ? ice : material;
}

void da_ice_bullet_hit(const Fvector& p, bool solid)
{
    const SDaIceCfg& cfg = da_ice_cfg();
    make_sounds();
    if (solid)
    {
        // The ice holds: a white star of cracks where it struck (the pair plays the chips and the crack).
        da_ice_hole_add(p, cfg.bullet_radius * 4.f, 1);
        return;
    }
    // Ice too thin to stop it: a small hole, a splash of the water under it, the crack of the skin.
    da_ice_hole_add(p, cfg.bullet_radius, 0);
    da_ice_hole_add(p, cfg.bullet_radius * 5.f, 1);
    s_bullet_thin.play(p, 0.8f);
    particles(cfg.ps_bullet, p);
}

bool da_ice_blast(const Fvector& p, float blast_radius)
{
    if (!g_pGamePersistent || !da_climate_enabled() || g_pGamePersistent->Environment().ice_at(p) <= 0.5f)
        return false;
    float strength = 0.f;
    if (da_ice_at(p, &strength) == da_ice_none)
        return false;
    // A grenade breaks a hole through ice a man stands on; thick winter ice only cracks round a small one.
    const float r = clampr(0.35f * blast_radius, 0.8f, 4.f) * clampr(0.12f / std::max(strength, 0.01f), 0.25f, 1.f);
    break_at(p, r, 1.f);
    da_ice_hole_add(p, r * 2.2f, 1);
    return true;
}

// ---- QA ------------------------------------------------------------------------------------------------------

namespace
{
// qa_ice_goto [depth=0.5]: the actor onto the water's surface (on its ice) where the water is at least that deep, facing
// the most water; qa_ice_probe: the ice under the actor; qa_ice_bullet <0|1>: the view's hit on the water as a bullet
// that the ice stopped (1) or that went through it (0).
class CCC_QaIceGoto final : public IConsole_Command
{
public:
    CCC_QaIceGoto(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        if (!actor)
            return;
        float depth = 0.5f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &depth);
        const CEnvironment& env = GamePersistent().Environment();
        if (!env.water_field_valid())
        {
            Msg("! [qa] qa_ice_goto: this level has no water field");
            return;
        }
        // The texel of the field farthest from any bank (the middle of the largest water) at least that deep.
        constexpr int dim = CEnvironment::water_field_dim;
        int best = -1;
        float best_shore = -1.f;
        for (int i = 0; i < dim * dim; ++i)
        {
            const CEnvironment::SWaterTexel& t = env.water_field[i];
            if (!t.mask || t.surface - t.bed < depth || t.shore <= best_shore)
                continue;
            if (size_t(i) < env.water_class.size() && env.water_class[i] == CEnvironment::water_class_indoor)
                continue;
            best_shore = t.shore;
            best = i;
        }
        if (best < 0)
        {
            Msg("! [qa] qa_ice_goto: no water %.2f m deep on the level", depth);
            return;
        }
        const Fvector ext = Fvector().sub(env.water_field_bounds.vMax, env.water_field_bounds.vMin);
        Fvector at;
        at.set(env.water_field_bounds.vMin.x + (float(best % dim) + 0.5f) * ext.x / float(dim),
            env.water_field[best].surface + 0.05f,
            env.water_field_bounds.vMin.z + (float(best / dim) + 0.5f) * ext.z / float(dim));
        Msg("* [qa] ice_goto: the water's middle, %.1f m from its banks, %.2f m deep", best_shore,
            env.water_field[best].surface - env.water_field[best].bed);
        actor->MoveActor(at, Fvector{-0.5f, 0.f, 0.f});
        float strength = 0.f;
        const EDaIce st = da_ice_at(at, &strength);
        Msg("* [qa] ice_goto (%.1f, %.2f, %.1f): ice %.2f state %u strength %.3f m", at.x, at.y, at.z, env.ice_at(at),
            u32(st), strength);
    }
};

class CCC_QaIceProbe final : public IConsole_Command
{
public:
    CCC_QaIceProbe(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        if (!actor)
            return;
        const CEnvironment& env = GamePersistent().Environment();
        const Fvector feet = actor->Position();
        float surface = 0.f, bed = 0.f;
        const bool water = env.water_at(feet, surface, bed);
        Fvector at = feet;
        at.y = surface;
        float strength = 0.f;
        const EDaIce st = water ? da_ice_at(at, &strength) : da_ice_none;
        Msg("* [qa] ice_probe feet (%.2f, %.3f, %.2f) water %d%s surface %.3f bed %.3f shore %.1f ice %.2f state %u "
            "strength %.3f holes %u material %u",
            feet.x, feet.y, feet.z, water ? 1 : 0, env.water_flowing_at(at) ? " running" : "", surface, bed,
            env.water_shore_dist(at), water ? env.ice_at(at) : 0.f, u32(st), strength, u32(da_ice_holes().size()),
            u32(da_ice_material()));
    }
};

class CCC_QaIceBullet final : public IConsole_Command
{
public:
    CCC_QaIceBullet(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
            return;
        int solid = 1;
        if (args && xr_strlen(args))
            sscanf(args, "%d", &solid);
        const Fvector& eye = Device.vCameraPosition;
        const Fvector& dir = Device.vCameraDirection;
        collide::rq_result R;
        if (!Level().ObjectSpace.RayPick(eye, dir, 100.f, collide::rqtStatic, R, nullptr))
        {
            Msg("! [qa] ice_bullet: nothing in view");
            return;
        }
        Fvector p;
        p.mad(eye, dir, R.range);
        da_ice_bullet_hit(p, solid != 0);
        Msg("* [qa] ice_bullet %s at (%.2f, %.2f, %.2f), holes %u", solid ? "stopped" : "through", p.x, p.y, p.z,
            u32(da_ice_holes().size()));
    }
};
} // namespace

void da_ice_register_console()
{
    CMD1(CCC_QaIceGoto, "qa_ice_goto");
    CMD1(CCC_QaIceProbe, "qa_ice_probe");
    CMD1(CCC_QaIceBullet, "qa_ice_bullet");
}
