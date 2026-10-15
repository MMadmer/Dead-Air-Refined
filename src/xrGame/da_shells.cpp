// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_shells.h"

#include "Level.h"
#include "Actor.h"
#include "entity_alive.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "da_script_cam.h"
#include "xrEngine/IRenderable.h"
#include "xrCDB/ISpatial.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Include/xrRender/RenderVisual.h"

// Cases that may exist at once; the quality preset sets it (xrRender_sync_preset_derived).
extern ENGINE_API int ps_shells_max;

namespace da_shells
{
namespace
{
enum ESurface : u8
{
    surface_hard,
    surface_soft,
    surface_metal,
    surface_count
};

struct Casing
{
    shared_str section;
    shared_str visual_name;
    // The first instance: proves the model loads and stands in for the renderable's own visual. Every case has an
    // instance of its own besides - the renderer takes a visual into a pass once a frame, so cases sharing one drew
    // a single case. Instances of cases gone wait in the spare list.
    IRenderVisual* visual{};
    xr_vector<IRenderVisual*> spare;
    bool missing{};
    float radius{0.005f};
    float half_length{0.01f};
    float mass{0.005f};
    Fvector2 speed{2.f, 3.5f};
    Fvector2 up{0.8f, 1.6f};
    Fvector2 back{-0.4f, 0.2f};
    Fvector2 spin{15.f, 40.f};
    u32 sound_class{};
};

struct SoundClass
{
    shared_str name;
    // per surface: the variants, each with its own voices so one bounce does not cut another short
    xr_vector<ref_sound> voices[surface_count];
};

struct Config
{
    bool loaded{};
    bool enabled{};
    float render_distance{25.f};
    float spawn_distance{40.f};
    float sound_distance{20.f};
    float own_ignore_s{0.4f};
    float gravity{9.81f};
    float drag{0.1f};
    float kick_radius{0.3f};
    float kick_speed{0.8f};
    float kick_max{1.8f};
    float kick_mass{0.01f};
    float sound_volume{0.6f};
    float shadow_distance{6.f};
    u32 shadow_max{48};
    Fvector eject_dir{1.f, 0.55f, -0.15f}; // right, up, forward of the weapon
    xr_vector<Casing> casings;
    xr_vector<std::pair<shared_str, u16>> by_particles;
    xr_vector<std::pair<shared_str, u16>> by_ammo; // prefix -> casing
    xr_vector<std::pair<shared_str, Fvector>> eject_by_weapon; // section prefix -> right/up/forward
    xr_vector<SoundClass> sounds;
};
Config cfg;

struct Shell
{
    Fvector pos{};
    Fvector vel{};
    Fvector spin{}; // world angular velocity, rad/s
    Fquaternion rot{};
    Fvector ground_n{0.f, 1.f, 0.f};
    float born{};
    float kicked{};
    float landed{};
    IRenderVisual* visual{};
    // A body or a prop under the case, and where it stood: when it moves or goes, the case falls.
    Fvector support_at{};
    u16 support{u16(-1)};
    u16 casing{};
    u16 shooter{u16(-1)};
    u8 bounces{};
    u8 ground_surface{surface_hard};
    bool ground{}; // sliding or rolling on a surface
    bool resting{};
};
xr_vector<Shell> shells;

// Feet on the move this frame, with the speed of the body's own movement: positions of the others come
// interpolated and jitter by metres a second while they stand.
struct Walker
{
    Fvector pos;
    Fvector vel;
};
xr_vector<Walker> walkers;
u32 kicks_total{};
xr_vector<IGameObject*> nearest;
collide::rq_results ray_results;

constexpr float k_rest_speed = 0.08f;
constexpr float k_ground_enter_speed = 0.6f; // normal speed below which a hit no longer bounces
constexpr float k_ground_max_speed = 3.f;
constexpr float k_ground_max_time = 8.f; // a roll down a long ramp is let go of after this

float roll(const Fvector2& range) { return ::Random.randF(range.x, range.y); }

Fvector2 read_range(const CInifile& ini, pcstr section, pcstr key, Fvector2 fallback)
{
    if (!ini.line_exist(section, key))
        return fallback;
    return ini.r_fvector2(section, key);
}

ESurface surface_of(u16 material)
{
    const SGameMtl* mtl = GMLib.GetMaterialByIdx(material);
    if (!mtl)
        return surface_hard;
    if (mtl->Flags.test(SGameMtl::flLiquid))
        return surface_soft;
    pcstr name = mtl->m_Name.c_str();
    if (strstr(name, "metal") || strstr(name, "iron") || strstr(name, "tin") || strstr(name, "pipe"))
        return surface_metal;
    for (pcstr soft : {"earth", "grass", "dirt", "sand", "soil", "mud", "bush", "leaf", "tree", "flesh", "cloth",
             "water", "swamp", "moss", "straw", "carpet"})
    {
        if (strstr(name, soft))
            return surface_soft;
    }
    return surface_hard;
}

void load()
{
    if (cfg.loaded)
        return;
    cfg.loaded = true;
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_shells.ltx");
    if (!FS.exist(path))
    {
        Msg("! [shells] dead_air_x64_shells.ltx not found - the sprite shells stay");
        return;
    }
    const CInifile ini(path, TRUE);
    constexpr pcstr main = "shells";
    if (!ini.section_exist(main))
        return;
    cfg.enabled = ini.line_exist(main, "enabled") ? ini.r_bool(main, "enabled") : true;
    const auto rf = [&](pcstr key, float& v) {
        if (ini.line_exist(main, key))
            v = ini.r_float(main, key);
    };
    rf("render_distance", cfg.render_distance);
    rf("spawn_distance", cfg.spawn_distance);
    rf("sound_distance", cfg.sound_distance);
    rf("own_ignore_s", cfg.own_ignore_s);
    rf("gravity", cfg.gravity);
    rf("drag", cfg.drag);
    rf("kick_radius", cfg.kick_radius);
    rf("kick_speed", cfg.kick_speed);
    rf("kick_max", cfg.kick_max);
    rf("kick_mass", cfg.kick_mass);
    rf("sound_volume", cfg.sound_volume);
    rf("shadow_distance", cfg.shadow_distance);
    if (ini.line_exist(main, "shadow_max"))
        cfg.shadow_max = ini.r_u32(main, "shadow_max");
    if (ini.line_exist(main, "eject_dir"))
        cfg.eject_dir = ini.r_fvector3(main, "eject_dir");

    // Sound classes first: a casing names one.
    if (ini.section_exist("shell_sounds"))
    {
        for (const auto& [name, prefix] : ini.r_section("shell_sounds").Data)
        {
            SoundClass& sc = cfg.sounds.emplace_back();
            sc.name = name;
            static constexpr pcstr surface_names[surface_count] = {"hard", "soft", "metal"};
            for (u32 s = 0; s < surface_count; ++s)
            {
                for (int variant = 1; variant <= 8; ++variant)
                {
                    string_path file, probe;
                    xr_sprintf(file, "%s%s_%d", prefix.c_str(), surface_names[s], variant);
                    if (!FS.exist(probe, "$game_sounds$", file, ".ogg"))
                        break;
                    // two voices per variant: a burst throws cases faster than one rings out
                    for (int voice = 0; voice < 2; ++voice)
                        sc.voices[s].emplace_back().create(file, st_Effect, sg_SourceType);
                }
            }
        }
    }
    const auto sound_class = [&](pcstr name) -> u32 {
        for (u32 i = 0; i < cfg.sounds.size(); ++i)
            if (cfg.sounds[i].name == name)
                return i;
        return u32(-1);
    };

    const auto casing_index = [&](const shared_str& section) -> u16 {
        for (u16 i = 0; i < cfg.casings.size(); ++i)
            if (cfg.casings[i].section == section)
                return i;
        if (!ini.section_exist(section) || !ini.line_exist(section, "visual"))
        {
            Msg("! [shells] casing [%s] has no visual", section.c_str());
            return u16(-1);
        }
        Casing& c = cfg.casings.emplace_back();
        c.section = section;
        c.visual_name = ini.r_string(section, "visual");
        if (ini.line_exist(section, "radius"))
            c.radius = ini.r_float(section, "radius");
        if (ini.line_exist(section, "half_length"))
            c.half_length = ini.r_float(section, "half_length");
        if (ini.line_exist(section, "mass"))
            c.mass = ini.r_float(section, "mass");
        c.speed = read_range(ini, section.c_str(), "eject_speed", c.speed);
        c.up = read_range(ini, section.c_str(), "eject_up", c.up);
        c.back = read_range(ini, section.c_str(), "eject_back", c.back);
        c.spin = read_range(ini, section.c_str(), "spin", c.spin);
        c.sound_class = ini.line_exist(section, "sound") ? sound_class(ini.r_string(section, "sound")) : u32(-1);
        return u16(cfg.casings.size() - 1);
    };

    const auto read_map = [&](pcstr section, xr_vector<std::pair<shared_str, u16>>& out) {
        if (!ini.section_exist(section))
            return;
        for (const auto& [key, value] : ini.r_section(section).Data)
        {
            const u16 index = casing_index(value);
            if (index != u16(-1))
                out.emplace_back(key, index);
        }
    };
    read_map("shells_by_ammo", cfg.by_ammo);
    read_map("shells_by_particles", cfg.by_particles);
    // the longest prefix wins
    std::sort(cfg.by_ammo.begin(), cfg.by_ammo.end(),
        [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    if (ini.section_exist("shells_eject"))
    {
        for (const auto& [key, value] : ini.r_section("shells_eject").Data)
        {
            Fvector v{};
            sscanf_s(value.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z);
            cfg.eject_by_weapon.emplace_back(key, v);
        }
        std::sort(cfg.eject_by_weapon.begin(), cfg.eject_by_weapon.end(),
            [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    }

    Msg("* [shells] %u casing(s), %u ammo and %u particle mapping(s), %u sound class(es)", u32(cfg.casings.size()),
        u32(cfg.by_ammo.size()), u32(cfg.by_particles.size()), u32(cfg.sounds.size()));
}

bool starts_with(const shared_str& s, const shared_str& prefix)
{
    return s.size() >= prefix.size() && 0 == strncmp(s.c_str(), prefix.c_str(), prefix.size());
}

u16 casing_for(const Ejection& shot)
{
    if (shot.ammo.size())
        for (const auto& [prefix, index] : cfg.by_ammo)
            if (starts_with(shot.ammo, prefix))
                return index;
    if (shot.particles.size())
        for (const auto& [name, index] : cfg.by_particles)
            if (name == shot.particles)
                return index;
    return u16(-1);
}

IRenderVisual* visual_of(Casing& c)
{
    if (c.visual || c.missing)
        return c.visual;
    string_path file;
    if (!FS.exist(file, "$game_meshes$", c.visual_name.c_str(), ".ogf"))
    {
        Msg("! [shells] model [%s] of casing [%s] not found - the sprite stays for it", c.visual_name.c_str(),
            c.section.c_str());
        c.missing = true;
        return nullptr;
    }
    c.visual = GEnv.Render->model_Create(c.visual_name.c_str());
    c.missing = !c.visual;
    return c.visual;
}

IRenderVisual* acquire(Casing& c)
{
    if (!c.spare.empty())
    {
        IRenderVisual* v = c.spare.back();
        c.spare.pop_back();
        return v;
    }
    return GEnv.Render->model_Create(c.visual_name.c_str());
}

void release(Shell& s)
{
    if (s.visual)
        cfg.casings[s.casing].spare.push_back(s.visual);
    s.visual = nullptr;
}

void remove_at(u32 index)
{
    release(shells[index]);
    shells[index] = shells.back();
    shells.pop_back();
}

// ---- the one renderable that draws every case ------------------------------------------------

class ShellsRenderable final : public SpatialBase, public RenderableBase
{
public:
    struct Item
    {
        IRenderVisual* visual;
        Fmatrix xform;
        float dist2;
    };
    // Written on the main thread in update, read by the render tasks of the frame that follows. Items go nearest
    // first; the shadow passes take the first shadow_count of them.
    xr_vector<Item> items[2];
    u32 shadow_count[2]{};
    std::atomic<u32> front{0};
    std::atomic<u32> render_calls{0}; // qa_shells: passes that asked for the cases

    ShellsRenderable() : SpatialBase(g_pGamePersistent->SpatialSpace)
    {
        renderable.pROS_Allowed = true;
        renderable.visual = nullptr;
        renderable.xform.identity();
        spatial.type = 0;
    }
    ~ShellsRenderable() override
    {
        if (spatial.type)
            spatial_unregister();
        // the probe belongs to its casing: not for the base to delete
        renderable.visual = nullptr;
    }

    IRenderable* dcast_Renderable() override { return this; }

    void renderable_Render(u32 context_id, IRenderable* root) override
    {
        const u32 index = front.load(std::memory_order_acquire);
        auto& list = items[index];
        render_calls.fetch_add(1, std::memory_order_relaxed);
        // Lights and cascades get only the nearest cases: farther off a centimetre of brass is below a shadow texel.
        const u32 count = root ? u32(list.size()) : _min(shadow_count[index], u32(list.size()));
        for (u32 i = 0; i < count; ++i)
            GEnv.Render->add_Visual(context_id, root, list[i].visual, list[i].xform);
    }

    void place(const Fvector& center, float radius)
    {
        if (!spatial.type)
        {
            spatial.type = STYPE_RENDERABLE;
            spatial.sphere.set(center, radius);
            spatial_register();
            return;
        }
        if (!center.similar(spatial.sphere.P, 0.5f) || !fsimilar(radius, spatial.sphere.R))
        {
            spatial.sphere.set(center, radius);
            spatial_move();
        }
    }
};
ShellsRenderable* g_renderable{};

// ---- collision --------------------------------------------------------------------------------

struct Hit
{
    float range{flt_max};
    Fvector normal{0.f, 1.f, 0.f};
    ESurface surface{surface_hard};
    IGameObject* object{};
};

struct TraceParams
{
    Hit* hit;
    Fvector dir;
    const IGameObject* skip;
};

bool trace_callback(collide::rq_result& result, LPVOID params)
{
    TraceParams& p = *static_cast<TraceParams*>(params);
    if (result.range >= p.hit->range)
        return true;
    if (result.O == p.skip && p.skip)
        return true;
    if (result.O)
    {
        // a body or a prop: bounced back the way it came
        p.hit->range = result.range;
        p.hit->object = result.O;
        p.hit->normal.invert(p.dir);
        p.hit->surface = surface_soft;
        if (p.dir.y < -0.5f)
            p.hit->normal.set(0.f, 1.f, 0.f);
        return true;
    }
    const CDB::TRI& tri = Level().ObjectSpace.GetStaticTris()[result.element];
    const SGameMtl* mtl = GMLib.GetMaterialByIdx(tri.material);
    // bushes and the like let a case through, as they let the player
    if (mtl && mtl->Flags.test(SGameMtl::flPassable))
        return true;
    const Fvector* v = Level().ObjectSpace.GetStaticVerts();
    Fvector e1, e2, n;
    e1.sub(v[tri.verts[1]], v[tri.verts[0]]);
    e2.sub(v[tri.verts[2]], v[tri.verts[0]]);
    n.crossproduct(e1, e2);
    if (n.square_magnitude() < EPS_S)
        return true;
    n.normalize();
    if (n.dotproduct(p.dir) > 0.f)
        n.invert();
    p.hit->range = result.range;
    p.hit->object = nullptr;
    p.hit->normal = n;
    p.hit->surface = surface_of(u16(tri.material));
    return true;
}

bool trace(const Fvector& from, const Fvector& dir, float range, IGameObject* ignore, Hit& hit)
{
    hit = {};
    hit.range = range;
    // The player's own body is not drawn from the eyes: nothing there to bounce off.
    CActor* actor = Actor();
    const IGameObject* skip = actor && actor->active_cam() == eacFirstEye && actor != ignore ? actor : nullptr;
    TraceParams params{&hit, dir, skip};
    collide::ray_defs rd(from, dir, range, CDB::OPT_CULL, collide::rqtBoth);
    ray_results.r_clear();
    Level().ObjectSpace.RayQuery(ray_results, rd, trace_callback, &params, nullptr, ignore);
    return hit.range < range;
}

// ---- sound ------------------------------------------------------------------------------------

void ring(const Shell& s, ESurface surface, float impact)
{
    const Casing& c = cfg.casings[s.casing];
    if (c.sound_class >= cfg.sounds.size() || impact < 0.35f)
        return;
    if (s.pos.distance_to_sqr(Device.vCameraPosition) > _sqr(cfg.sound_distance))
        return;
    auto& voices = cfg.sounds[c.sound_class].voices[surface];
    if (voices.empty())
        return;
    // a free voice from a random start, so a burst does not play one variant over and over
    const u32 count = u32(voices.size());
    const u32 start = u32(::Random.randI(int(count)));
    for (u32 i = 0; i < count; ++i)
    {
        ref_sound& v = voices[(start + i) % count];
        if (v._feedback())
            continue;
        v.play_at_pos(nullptr, s.pos, 0);
        v.set_volume(cfg.sound_volume * clampr(impact / 3.f, 0.15f, 1.f));
        v.set_frequency(::Random.randF(0.92f, 1.08f));
        v.set_range(0.5f, cfg.sound_distance);
        return;
    }
}

// ---- simulation -------------------------------------------------------------------------------

Fvector axis_of(const Shell& s)
{
    Fmatrix m;
    m.rotation(s.rot);
    return m.k;
}

// A world-space turn of the case. The rows of an Fmatrix are the basis and Fmatrix::rotation reads the
// quaternion the other way round, so the turn goes on the right, inverted.
void rotate(Shell& s, const Fvector& w, float dt)
{
    const float angle = w.magnitude() * dt;
    if (angle < EPS_S)
        return;
    Fvector axis;
    axis.div(w, angle / dt);
    Fquaternion dq;
    dq.rotation(axis, -angle);
    Fquaternion r;
    r.mul(s.rot, dq);
    s.rot = r;
    s.rot.normalize();
}

// Lay the case on its side on the surface, keeping its heading.
void lay_down(Shell& s, const Fvector& n)
{
    Fvector axis = axis_of(s);
    axis.mad(n, -axis.dotproduct(n));
    // standing on its end: falls over any way
    if (axis.square_magnitude() < 0.01f)
    {
        axis.random_dir();
        axis.mad(n, -axis.dotproduct(n));
    }
    axis.normalize_safe();
    Fmatrix m;
    m.identity();
    m.k = axis;
    m.j = n;
    m.i.crossproduct(m.j, m.k);
    s.rot.set(m);
    s.rot.normalize();
}

// What the case lies on from here: the surface for its friction, a body or a prop to watch.
void land_on(Shell& s, const Hit& hit)
{
    s.ground_n = hit.normal;
    s.ground_surface = hit.surface;
    s.support = hit.object ? hit.object->ID() : u16(-1);
    if (hit.object)
        s.support_at = hit.object->Position();
}

void step_air(Shell& s, const Casing& c, float dt, float now)
{
    s.vel.y -= cfg.gravity * dt;
    s.vel.mul(1.f - cfg.drag * dt);
    Fvector move;
    move.mul(s.vel, dt);
    const float len = move.magnitude();
    rotate(s, s.spin, dt);
    if (len < EPS_S)
        return;
    Fvector dir;
    dir.div(move, len);
    IGameObject* ignore = (now - s.born < cfg.own_ignore_s) ? Level().Objects.net_Find(s.shooter) : nullptr;
    Hit hit;
    if (!trace(s.pos, dir, len + c.radius, ignore, hit))
    {
        s.pos.add(move);
        return;
    }
    s.pos.mad(dir, _max(hit.range - c.radius, 0.f));
    s.pos.mad(hit.normal, 0.001f);
    const float vn = s.vel.dotproduct(hit.normal);
    if (vn >= 0.f)
        return;
    Fvector v_n, v_t;
    v_n.mul(hit.normal, vn);
    v_t.sub(s.vel, v_n);
    // brass rings off stone and steel, thuds into earth
    const float restitution = hit.surface == surface_soft ? 0.15f : (hit.surface == surface_metal ? 0.45f : 0.35f);
    const float friction = hit.surface == surface_soft ? 0.55f : 0.25f;
    v_t.mul(1.f - friction);
    v_n.mul(-restitution);
    s.vel.add(v_t, v_n);
    ++s.bounces;
    ring(s, hit.surface, -vn);
    // a fresh tumble from the knock
    Fvector kick;
    kick.random_dir();
    s.spin.mul(0.4f);
    s.spin.mad(kick, -vn * ::Random.randF(6.f, 14.f));
    // standing surface and the bounce all but gone: slide or roll from here on
    if (hit.normal.y > 0.55f && -vn < k_ground_enter_speed)
    {
        s.ground = true;
        s.landed = now;
        land_on(s, hit);
        s.vel.mad(hit.normal, -s.vel.dotproduct(hit.normal));
        lay_down(s, hit.normal);
    }
}

void step_ground(Shell& s, const Casing& c, float dt, float now)
{
    const Fvector& n = s.ground_n;
    s.vel.mad(n, -s.vel.dotproduct(n));
    // a case on its side rolls across its axis and slides along it
    const Fvector axis = axis_of(s);
    float along = s.vel.dotproduct(axis);
    Fvector across;
    across.mad(s.vel, axis, -along);
    const auto damp = [&](float v, float decel) {
        const float m = _abs(v) - decel * dt;
        return m <= 0.f ? 0.f : (v > 0.f ? m : -m);
    };
    // Gravity along the surface against what the surface holds back: earth and grass keep a case on a slope it
    // rolls down on concrete. It slides along its axis and rolls across it, a thin tube with half the pull.
    Fvector slope;
    slope.set(0.f, -cfg.gravity, 0.f).mad(n, cfg.gravity * n.y);
    const float slope_along = slope.dotproduct(axis);
    Fvector slope_across;
    slope_across.mad(slope, axis, -slope_along);
    const float press = cfg.gravity * n.y;
    const float slide_drag =
        (s.ground_surface == surface_soft ? 0.8f : (s.ground_surface == surface_metal ? 0.35f : 0.5f)) * press;
    const float roll_drag =
        (s.ground_surface == surface_soft ? 0.45f : (s.ground_surface == surface_metal ? 0.06f : 0.12f)) * press;
    along = damp(along + slope_along * dt, slide_drag);
    across.mad(slope_across, 0.5f * dt);
    const float across_speed = across.magnitude();
    if (across_speed > EPS_S)
        across.mul(damp(across_speed, roll_drag) / across_speed);
    s.vel.mad(across, axis, along);

    float speed = s.vel.magnitude();
    if (speed > k_ground_max_speed)
    {
        s.vel.mul(k_ground_max_speed / speed);
        speed = k_ground_max_speed;
    }
    const bool holds = _abs(slope_along) <= slide_drag && 0.5f * slope_across.magnitude() <= roll_drag;
    if ((speed < k_rest_speed && holds) || now - s.landed > k_ground_max_time)
    {
        s.vel.set(0.f, 0.f, 0.f);
        s.spin.set(0.f, 0.f, 0.f);
        s.resting = true;
        return;
    }
    // roll about the axis as fast as it travels across it
    Fvector roll_w;
    roll_w.crossproduct(n, across);
    roll_w.mul(1.f / _max(c.radius, 0.002f));
    rotate(s, roll_w, dt);

    Fvector next;
    next.mad(s.pos, s.vel, dt);
    // something in the way: knocked back
    Hit hit;
    const float travel = speed * dt;
    if (travel > EPS_S)
    {
        Fvector dir;
        dir.div(s.vel, speed);
        if (trace(s.pos, dir, travel + c.radius, nullptr, hit) && hit.normal.y < 0.55f)
        {
            const float vn = s.vel.dotproduct(hit.normal);
            s.vel.mad(hit.normal, -1.3f * vn);
            next = s.pos;
            ring(s, hit.surface, -vn);
        }
    }
    // stay on the ground, or fall off its edge
    Fvector probe = next;
    probe.mad(n, 0.05f);
    const Fvector down{-n.x, -n.y, -n.z};
    if (trace(probe, down, 0.05f + c.radius + 0.06f, nullptr, hit) && hit.normal.y > 0.55f)
    {
        s.pos.mad(probe, down, _max(hit.range - c.radius, 0.f));
        land_on(s, hit);
    }
    else
    {
        s.pos = next;
        s.ground = false;
    }
}

void step(Shell& s, float dt, float now)
{
    const Casing& c = cfg.casings[s.casing];
    if (s.ground)
        step_ground(s, c, dt, now);
    else
        step_air(s, c, dt, now);
    // nothing under it at all: gone
    if (s.pos.y < Level().ObjectSpace.GetBoundingVolume().vMin.y - 5.f)
        s.born = -1.f;
}

// Feet on the move nudge the cases they walk through; a light case goes further than a heavy one,
// none takes off.
void kick(Shell& s, const Walker& w, float now)
{
    const Fvector& feet_velocity = w.vel;
    const Casing& c = cfg.casings[s.casing];
    const float walker_speed = feet_velocity.magnitude();
    Fvector away;
    away.sub(s.pos, w.pos);
    away.y = 0.f;
    if (away.square_magnitude() < EPS_S)
        away.set(::Random.randF(-1.f, 1.f), 0.f, ::Random.randF(-1.f, 1.f));
    away.normalize_safe();
    away.mad(feet_velocity, 0.5f / _max(walker_speed, EPS_S));
    away.y = 0.f;
    away.normalize_safe();
    const float speed = _min(walker_speed * cfg.kick_speed * _sqrt(cfg.kick_mass / _max(c.mass, 0.001f)), cfg.kick_max);
    s.vel.mul(away, speed);
    s.vel.y = ::Random.randF(0.2f, 0.6f) * _min(speed, 1.f);
    Fvector spin;
    spin.random_dir();
    s.spin.mul(spin, speed * ::Random.randF(8.f, 20.f));
    s.resting = false;
    s.ground = false;
    s.support = u16(-1);
    s.kicked = now;
    s.bounces = 0;
    ++kicks_total;
}

// The case to give up for a new one: the resting one farthest from the camera, else the oldest.
u32 victim()
{
    u32 best = u32(-1);
    float best_d = -1.f;
    for (u32 i = 0; i < shells.size(); ++i)
    {
        if (!shells[i].resting)
            continue;
        const float d = shells[i].pos.distance_to_sqr(Device.vCameraPosition);
        if (d > best_d)
        {
            best_d = d;
            best = i;
        }
    }
    if (best != u32(-1))
        return best;
    float oldest = flt_max;
    for (u32 i = 0; i < shells.size(); ++i)
    {
        if (shells[i].born < oldest)
        {
            oldest = shells[i].born;
            best = i;
        }
    }
    return best;
}

} // namespace

bool eject(const Ejection& shot)
{
    load();
    if (!cfg.enabled || ps_shells_max <= 0 || !g_pGameLevel)
        return false;
    const u16 index = casing_for(shot);
    if (index == u16(-1))
        return false;
    Casing& c = cfg.casings[index];
    if (!visual_of(c))
        return false;

    // The hud weapon hangs where the arms would hold it, so its port is a world point already. Drawn with the
    // narrower hud field of view it shows farther out on screen; moved along the screen ray to match, the case
    // started most of a metre to the side, over the railing of a tower.
    Fvector origin = shot.position;
    if (origin.distance_to_sqr(Device.vCameraPosition) > _sqr(cfg.spawn_distance))
        return true;

    // The frame of the weapon: right, up and forward.
    Fvector forward = shot.forward;
    if (shot.hud)
        forward = Device.vCameraDirection;
    forward.normalize_safe();
    Fvector up{0.f, 1.f, 0.f}, right;
    right.crossproduct(up, forward);
    if (right.square_magnitude() < EPS_S)
        right = Device.vCameraRight;
    right.normalize();
    up.crossproduct(forward, right);

    if (shot.hud)
    {
        // out of the wall it is pressed against: not behind it
        Fvector dir;
        dir.sub(origin, Device.vCameraPosition);
        const float len = dir.magnitude();
        Hit hit;
        if (len > EPS_S && trace(Device.vCameraPosition, dir.div(len), len, Level().CurrentEntity(), hit))
            origin.mad(Device.vCameraPosition, dir, _max(hit.range - 0.05f, 0.f));
    }

    Fvector weights = cfg.eject_dir;
    for (const auto& [prefix, w] : cfg.eject_by_weapon)
    {
        if (starts_with(shot.weapon, prefix))
        {
            weights = w;
            break;
        }
    }

    if (shells.size() >= u32(ps_shells_max))
    {
        const u32 v = victim();
        if (v != u32(-1))
            remove_at(v);
    }

    IRenderVisual* instance = acquire(c);
    if (!instance)
        return false;
    Shell& s = shells.emplace_back();
    s.visual = instance;
    s.casing = index;
    s.shooter = shot.shooter ? shot.shooter->ID() : u16(-1);
    s.born = Device.fTimeGlobal;
    s.pos = origin;
    const float speed = roll(c.speed);
    Fvector dir;
    dir.mul(right, weights.x).mad(up, weights.y).mad(forward, weights.z);
    dir.normalize_safe();
    s.vel.mul(dir, speed);
    s.vel.mad(up, roll(c.up) * 0.5f);
    s.vel.mad(forward, roll(c.back));
    s.vel.add(shot.carrier_velocity);
    // The case leaves along the bore and tumbles end over end about the weapon's up axis.
    Fmatrix m;
    m.identity();
    m.k = forward;
    m.j = up;
    m.i = right;
    s.rot.set(m);
    s.spin.mul(up, roll(c.spin) * (::Random.randI(2) ? 1.f : -1.f));
    Fvector wobble;
    wobble.random_dir();
    s.spin.mad(wobble, roll(c.spin) * 0.25f);
    return true;
}

void update()
{
    load();
    if (!cfg.enabled || shells.empty() || !g_pGameLevel)
    {
        if (g_renderable && !g_renderable->items[g_renderable->front.load()].empty())
        {
            const u32 back = g_renderable->front.load() ^ 1;
            g_renderable->items[back].clear();
            g_renderable->shadow_count[back] = 0;
            g_renderable->front.store(back, std::memory_order_release);
        }
        return;
    }
    // A preset lowered under the cases that are already lying about.
    while (shells.size() > u32(_max(ps_shells_max, 0)))
        remove_at(victim());

    const float now = Device.fTimeGlobal;
    const float dt = _min(Device.fTimeDelta, 0.05f);

    walkers.clear();
    nearest.clear();
    if (dt > EPS_S)
        Level().ObjectSpace.GetNearest(nearest, Device.vCameraPosition, cfg.render_distance, nullptr);
    for (IGameObject* o : nearest)
    {
        CEntityAlive* alive = smart_cast<CEntityAlive*>(o);
        if (!alive || !alive->g_Alive())
            continue;
        CCharacterPhysicsSupport* support = alive->character_physics_support();
        if (!support || !support->movement())
            continue;
        Fvector v = support->movement()->GetVelocity();
        v.y = 0.f;
        if (v.square_magnitude() > _sqr(0.6f))
            walkers.push_back({o->Position(), v});
    }

    // Small steps: a case leaves the port at a few metres a second.
    const int substeps = iCeil(dt / (1.f / 90.f));
    const float h = substeps ? dt / substeps : 0.f;
    for (Shell& s : shells)
    {
        // a case lying on a body or a prop falls when that moves or goes
        if (s.resting && s.support != u16(-1))
        {
            const IGameObject* under = Level().Objects.net_Find(s.support);
            if (!under || under->Position().distance_to_sqr(s.support_at) > _sqr(0.02f))
            {
                s.resting = false;
                s.ground = false;
                s.support = u16(-1);
            }
        }
        if (!s.resting)
        {
            for (int i = 0; i < substeps && !s.resting && s.born >= 0.f; ++i)
                step(s, h, now);
        }
        if (s.born < 0.f || walkers.empty() || now - s.kicked < 0.4f || !(s.resting || s.ground))
            continue;
        for (const Walker& w : walkers)
        {
            Fvector d;
            d.sub(s.pos, w.pos);
            if (_abs(d.y) < 0.35f && d.x * d.x + d.z * d.z < _sqr(cfg.kick_radius))
            {
                kick(s, w, now);
                break;
            }
        }
    }
    for (u32 i = 0; i < shells.size();)
    {
        if (shells[i].born < 0.f)
            remove_at(i);
        else
            ++i;
    }

    // What the camera sees this frame, near enough to be more than a pixel.
    if (!g_renderable)
        g_renderable = xr_new<ShellsRenderable>();
    const u32 back = g_renderable->front.load() ^ 1;
    auto& list = g_renderable->items[back];
    list.clear();
    // This frame's camera: the device puts its own full transform together only after the level's frame.
    Fmatrix full;
    full.mul(Device.mProject, Device.mView);
    CFrustum view;
    view.CreateFromMatrix(full, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    const float far2 = _sqr(cfg.render_distance);
    for (const Shell& s : shells)
    {
        const float dist2 = s.pos.distance_to_sqr(Device.vCameraPosition);
        if (dist2 > far2)
            continue;
        const Casing& c = cfg.casings[s.casing];
        if (!view.testSphere_dirty(s.pos, c.half_length + c.radius))
            continue;
        auto& item = list.emplace_back();
        item.visual = s.visual;
        item.xform.mk_xform(s.rot, s.pos);
        item.dist2 = dist2;
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.dist2 < b.dist2; });
    const float shadow2 = _sqr(cfg.shadow_distance);
    u32 shadowed = 0;
    while (shadowed < list.size() && shadowed < cfg.shadow_max && list[shadowed].dist2 < shadow2)
        ++shadowed;
    g_renderable->shadow_count[back] = shadowed;
    g_renderable->front.store(back, std::memory_order_release);
    // The main pass tests a renderable's own visual against the occluders before it asks for the leaves: one of the
    // cases, half a metre behind the eye - a box behind the near plane always passes, while one in front of it hid
    // every case with the camera against a wall.
    for (const Casing& c : cfg.casings)
    {
        if (c.visual)
        {
            g_renderable->renderable.visual = c.visual;
            break;
        }
    }
    g_renderable->renderable.xform.translate(Fvector().mad(Device.vCameraPosition, Device.vCameraDirection, -0.5f));
    if (g_renderable->renderable.visual)
        g_renderable->place(Device.vCameraPosition, cfg.render_distance);
}

void look(float height)
{
    if (shells.empty())
        return;
    Fvector centre{};
    for (const Shell& s : shells)
        centre.add(s.pos);
    centre.div(float(shells.size()));
    Fvector pos = centre;
    Fvector back = Device.vCameraDirection;
    back.y = 0.f;
    back.normalize_safe();
    pos.mad(back, -height * 0.7f);
    pos.y += height;
    Fvector dir;
    dir.sub(centre, pos).normalize();
    float h, p;
    dir.getHP(h, p);
    if (CActor* actor = Actor())
    {
        CDaScriptCamEffector* cam = actor->script_cam_init();
        cam->m_position = pos;
        cam->m_hpb.set(h, p, 0.f);
    }
    Msg("* [shells] the camera looks at %.2f %.2f %.2f from %.2f %.2f %.2f", centre.x, centre.y, centre.z, pos.x, pos.y,
        pos.z);
}

void approach(float distance)
{
    CActor* actor = Actor();
    if (!actor)
        return;
    Fvector centre{};
    u32 count = 0;
    for (const Shell& s : shells)
    {
        if (s.shooter == actor->ID())
        {
            centre.add(s.pos);
            ++count;
        }
    }
    if (!count)
        return;
    centre.div(float(count));
    Fvector dir;
    dir.sub(centre, actor->Position());
    dir.y = 0.f;
    if (dir.square_magnitude() < EPS_S)
        dir = Device.vCameraDirection;
    dir.y = 0.f;
    dir.normalize_safe();
    Fvector pos;
    pos.mad(centre, dir, -distance);
    pos.y = actor->Position().y;
    // MoveActor takes the heading and the pitch, not a direction
    float h, p;
    dir.getHP(h, p);
    actor->MoveActor(pos, Fvector{p, h, 0.f});
    Msg("* [shells] the player stands %.1f m before the cases at %.2f %.2f %.2f", distance, centre.x, centre.y, centre.z);
}

void report()
{
    u32 flying = 0, ground = 0, resting = 0, own = 0;
    const u16 actor = Actor() ? Actor()->ID() : u16(-1);
    for (const Shell& s : shells)
    {
        (s.resting ? resting : (s.ground ? ground : flying))++;
        own += s.shooter == actor;
    }
    Msg("* [shells] %u case(s) of %d (%u the player's): %u flying, %u sliding, %u lying; %u drawn, %u render call(s)",
        u32(shells.size()), ps_shells_max, own, flying, ground, resting,
        g_renderable ? u32(g_renderable->items[g_renderable->front.load()].size()) : 0u,
        g_renderable ? g_renderable->render_calls.load() : 0u);
    Msg("* [shells] %u kick(s) by feet so far", kicks_total);
    u32 shown = 0;
    for (const Shell& s : shells)
    {
        if (shown++ >= 10)
            break;
        const Fvector axis = axis_of(s);
        Msg("* [shells]   [%s] at %.2f %.2f %.2f, %.1f m from the camera, axis %.2f %.2f %.2f, %u bounce(s)%s",
            cfg.casings[s.casing].section.c_str(), s.pos.x, s.pos.y, s.pos.z, s.pos.distance_to(Device.vCameraPosition),
            axis.x, axis.y, axis.z, u32(s.bounces), s.resting ? ", lying" : (s.ground ? ", sliding" : ""));
    }
}

void level_unload()
{
    walkers.clear();
    xr_delete(g_renderable);
    for (Shell& s : shells)
        release(s);
    shells.clear();
    for (Casing& c : cfg.casings)
    {
        for (IRenderVisual*& v : c.spare)
            GEnv.Render->model_Delete(v);
        c.spare.clear();
        if (c.visual)
            GEnv.Render->model_Delete(c.visual);
        c.visual = nullptr;
    }
    // The voices go now, while the sound engine is there: at exit they would outlive it. The next level reads the
    // config anew.
    cfg = Config{};
    kicks_total = 0;
}
} // namespace da_shells
