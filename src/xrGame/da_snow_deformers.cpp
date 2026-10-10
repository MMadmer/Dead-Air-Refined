// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Everything that presses the snow (docs/dead-air/WINTER.md), as stamps for the renderer's trail maps
// and the grass push map (da_snow_stamp_add, SSnowStamp):
//   feet    - every creature in a 96 m window round the camera, every frame, wherever a sole is on the
//             snow surface or under it (the contact gate; the step manager's 'disable' gate skipped every
//             motion a creature has no step parameters for - all but walk and run for a tushkan, a rat, a
//             burer or a chimera). A planted foot packs the loose snow under it to its creature's density
//             (pack) and sinks until the packed column carries it: deep in fresh powder, a little in old
//             snow. A swinging foot that stays in the snow ploughs through it, so in deep snow the prints
//             join into a trail;
//   body    - a capsule under each body at its belly height: the low bodies (tushkans, dogs, boars)
//             plough deep snow and leave a trough (a tushkan in the deepest drift swims in it); every
//             body pushes the grass;
//   every stamp of a creature is the capsule its deformer swept since the last frame, so a fast leg
//   leaves no gaps, and none reaches deeper than its creature's reach under the snow surface (sink);
//   resting - a corpse, loot, an artefact or a prop that comes to rest leaves its footprint once, and
//             again whenever it comes back into the window, so nothing lies buried under a drift. A corpse
//             presses the snow by its weight (its creature's pack) into an irregular hollow, and the snow
//             round it is churned by the fall: lumps and scuffs, never a clean pit of its boxes;
//   craters - an explosion near the ground blows an irregular hole.
// Psy-dog phantoms (illusions) and poltergeists (they fly) press nothing. Radii, belly heights and sink
// depths per creature class are data ([snow_deformers] of dead_air_x64_seasons.ltx) over built-in defaults.

#include "StdAfx.h"
#include "da_snow_deformers.h"
#include "entity_alive.h"
#include "Level.h"
#include "GamePersistent.h"
#include "PhysicsShellHolder.h"
#include "xrPhysics/PhysicsShell.h"
#include "Include/xrRender/Kinematics.h"
#include "xrEngine/da_snow_field.h"
#include "ai/monsters/pseudodog/psy_dog.h"
#include "ai/monsters/poltergeist/poltergeist.h"
#include "ai/stalker/ai_stalker.h"
#include "Actor.h"
#include "actor_defs.h"
#include "ZoneCampfire.h"

namespace
{
// Creatures and resting objects are stamped this far from the camera (horizontal metres): the renderer's
// fine trail window at its largest, with room for its coarse persistence map.
constexpr float stamp_window = 96.f;
// A sole this close above the snow surface stands on it.
constexpr float contact_slack = 0.03f;
// A sole or a body this far under the topmost surface is under a roof or a deck: the snow up there is
// not its business.
constexpr float under_roof = 0.6f;
// Rings a resting object is stamped again on crossing inward: every preset's fine window holds one.
constexpr float rest_rings[3] = {stamp_window, 44.f, 22.f};
constexpr u32 leg_max = 4;
// Most stamps one resting object leaves; most a scan may lay (snow arriving, a level loading with snow:
// every corpse, loot item and prop in the window at once), and what any resting print always leaves of the
// frame's list to the creatures that update after it.
constexpr u32 rest_stamps_max = 48;
constexpr u32 rest_scan_budget = 1024;
constexpr u32 creature_reserve = 512;
// The physics in motion: looked for this often and this far from the camera, so many objects and elements of each
// at most a frame.
constexpr float moving_scan = 0.15f;
constexpr float moving_window = 40.f;
constexpr u32 moving_max = 8;
constexpr u32 moving_elements = 16;
// A campfire's melt: its pit's thin snow out to this radius, and the thaw round a lit one out to this.
constexpr float fire_pit_r = 0.55f;
constexpr float fire_thaw_r = 2.5f;

struct SProfile
{
    float foot_r = 0.f;  // m, 0 = no feet stamps
    float body_r = 0.f;  // m, 0 = no body disc
    float belly = 0.f;   // m above the body's position: the lowest point of the trunk
    float sink = 0.f;    // m under the snow surface a foot or the body reaches at most
    float pack = 0.f;    // kg/m3 a planted foot packs loose snow to (its weight over the paw's area)
    bool skip = false;
};

struct SProfileDefault
{
    pcstr clsid;
    SProfile p;
};

// Radii from the brief's deformer table (foot spacing and body boxes of the shipped visuals); belly
// heights from the models' bind poses. Humans' discs sit at the hip: they push grass, never snow. Reach:
// about the knee. Pack: the density a planted foot packs loose snow to grows with its pressure - a rat
// or a tushkan barely sinks into settled snow (it runs over it, and swims in a drift), a dog packs fresh
// powder to a fifth of its depth and settled snow to half, a man settled snow to under half, a boar, a
// flesh, a chimera or the pseudogiant to firn and ice.
const SProfileDefault profile_defaults[] = {
    {"S_ACTOR", {0.12f, 0.30f, 0.90f, 0.50f, 520.f, false}},
    {"AI_STL_S", {0.12f, 0.30f, 0.90f, 0.50f, 520.f, false}},
    {"SM_TUSHK", {0.06f, 0.12f, 0.10f, 0.20f, 260.f, false}},
    {"SM_RAT", {0.00f, 0.08f, 0.03f, 0.08f, 220.f, false}},
    {"SM_DOG_S", {0.07f, 0.20f, 0.42f, 0.35f, 400.f, false}},
    {"SM_P_DOG", {0.08f, 0.22f, 0.44f, 0.38f, 440.f, false}},
    {"SM_DOG_P", {0.08f, 0.22f, 0.44f, 0.38f, 440.f, false}},
    {"SM_DOG_F", {0.f, 0.f, 0.f, 0.f, 0.f, true}},
    {"SM_BOARW", {0.10f, 0.35f, 0.35f, 0.40f, 580.f, false}},
    {"SM_FLESH", {0.10f, 0.40f, 0.30f, 0.40f, 600.f, false}},
    {"SM_CAT_S", {0.06f, 0.15f, 0.30f, 0.25f, 300.f, false}},
    {"SM_SNORK", {0.10f, 0.30f, 0.20f, 0.45f, 480.f, false}},
    {"SM_BLOOD", {0.12f, 0.30f, 0.90f, 0.50f, 560.f, false}},
    {"SM_CONTR", {0.12f, 0.30f, 0.90f, 0.50f, 520.f, false}},
    {"SM_BURER", {0.14f, 0.35f, 0.45f, 0.45f, 540.f, false}},
    {"SM_CHIMS", {0.12f, 0.40f, 0.40f, 0.50f, 600.f, false}},
    {"SM_GIANT", {0.25f, 0.80f, 0.45f, 0.60f, 700.f, false}},
    {"SM_IZLOM", {0.12f, 0.30f, 0.90f, 0.50f, 520.f, false}},
    {"SM_ZOMBI", {0.12f, 0.30f, 0.90f, 0.50f, 520.f, false}},
    {"SM_POLTR", {0.f, 0.f, 0.f, 0.f, 0.f, true}},
};
constexpr SProfile profile_fallback{0.10f, 0.30f, 0.60f, 0.45f, 480.f, false};
// Of the snow a deformer ploughs aside, the share that falls back into the furrow behind it: a leg's
// narrow furrow keeps most of its walls; a low body's trough fills more, the smaller the body the more (a
// tushkan in a drift leaves a shallow trough over its back, not an open trench).
constexpr float fall_leg = 0.3f;
constexpr float fall_leg_small = 0.45f;
constexpr float fall_body = 0.35f;
constexpr float fall_body_small = 0.7f;
// A foot whose horizontal speed is under this share of the body's (plus a margin for the animation's own
// sway, m/s) is planted; a swinging one is not.
constexpr float planted_share = 0.35f;
constexpr float planted_margin = 0.25f;

xr_unordered_map<CLASS_ID, SProfile> g_profiles;
bool g_profiles_loaded = false;

void load_profiles()
{
    g_profiles_loaded = true;
    g_profiles.clear();
    for (const SProfileDefault& d : profile_defaults)
        g_profiles[TEXT2CLSID(d.clsid)] = d.p;

    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_seasons.ltx");
    if (!FS.exist(path))
        return;
    const CInifile ini(path, TRUE);
    if (!ini.section_exist("snow_deformers"))
        return;
    // <class id> = foot radius, body radius, belly height [, reach [, pack]]  |  none
    for (const auto& item : ini.r_section("snow_deformers").Data)
    {
        if (!item.first.size() || !item.second.size())
            continue;
        const CLASS_ID id = TEXT2CLSID(item.first.c_str());
        SProfile p;
        // a line without a reach or a pack keeps the built-in ones of its class
        const auto known = g_profiles.find(id);
        const SProfile& base = known != g_profiles.end() ? known->second : profile_fallback;
        p.sink = base.sink;
        p.pack = base.pack;
        if (!xr_stricmp(item.second.c_str(), "none"))
            p.skip = true;
        else if (sscanf(item.second.c_str(), "%f , %f , %f , %f , %f", &p.foot_r, &p.body_r, &p.belly, &p.sink, &p.pack) < 3)
        {
            Msg("! [snow] [snow_deformers] %s: expected 'foot radius, body radius, belly height[, reach[, pack]]' or "
                "'none'",
                item.first.c_str());
            continue;
        }
        p.foot_r = clampr(p.foot_r, 0.f, 1.f);
        p.body_r = clampr(p.body_r, 0.f, 2.f);
        p.belly = clampr(p.belly, 0.f, 3.f);
        p.sink = clampr(p.sink, 0.f, 1.f);
        p.pack = clampr(p.pack, 0.f, 917.f);
        g_profiles[id] = p;
    }
}

const SProfile& profile_of(CLASS_ID id)
{
    if (!g_profiles_loaded)
        load_profiles();
    const auto it = g_profiles.find(id);
    return it != g_profiles.end() ? it->second : profile_fallback;
}

struct SCreature
{
    IKinematics* kin{};
    CLASS_ID cls{};
    u16 bone_count{};
    const SProfile* profile{};
    u16 bones[leg_max]{};
    float sole[leg_max]{}; // bind-pose height of the foot bone over the sole
    u32 legs{};
    Fvector foot[leg_max]{};
    Fvector2 foot_vel[leg_max]{};
    float body_y{}; // the belly's height on the last frame
    Fvector pos{};
    Fvector2 vel{};
    float time{-1.f};
    u32 frame{u32(-1)};
};
xr_unordered_map<u16, SCreature> g_creatures;

struct SResting
{
    Fvector pos;
    u32 scan;
    u32 ring;
};
xr_unordered_map<u16, SResting> g_resting;
u32 g_scan = 0;
float g_next_scan = 0.f;
float g_next_prune = 0.f;
bool g_was_active = false;

// What moves: each element's box bottom last frame.
struct SMoving
{
    Fvector bottom[moving_elements];
    u32 count = 0;
    bool fresh = true;
};
xr_unordered_map<u16, SMoving> g_moving;
float g_next_moving = 0.f;

// The topmost surface under a point, conservatively: the lowest of the four texels round it, so a foot
// beside a wall reads the ground and not the wall top.
float top_under(const SSnowField& f, float x, float z)
{
    const float fx = (x - f.origin.x) / f.texel - 0.5f;
    const float fz = (z - f.origin.y) / f.texel - 0.5f;
    const int x0 = clampr(iFloor(fx), 0, int(f.dim_x) - 1), x1 = clampr(iFloor(fx) + 1, 0, int(f.dim_x) - 1);
    const int z0 = clampr(iFloor(fz), 0, int(f.dim_z) - 1), z1 = clampr(iFloor(fz) + 1, 0, int(f.dim_z) - 1);
    const size_t w = f.dim_x;
    return std::min(std::min(f.top[size_t(z0) * w + x0], f.top[size_t(z0) * w + x1]),
        std::min(f.top[size_t(z1) * w + x0], f.top[size_t(z1) * w + x1]));
}

// Is a deformer's bottom at y in the snow at xz: the ground's (under the lowest top round it, the depth over that) or
// a cover's (a roof, a trailer: on its own top, under the snow the shells lay there - the gate by the ground's alone
// let no foot on a sloping roof through, its lowest neighbour lying under it).
bool in_snow(const SSnowField& f, float x, float z, float y)
{
    const float top = top_under(f, x, z);
    if (y < top - under_roof)
        return false;
    if (y <= top + da_snow_depth_at(x, z) + contact_slack)
        return true;
    if (f.cover.size() != size_t(f.dim_x) * f.dim_z)
        return false;
    const int cx = clampr(iFloor((x - f.origin.x) / f.texel), 0, int(f.dim_x) - 1);
    const int cz = clampr(iFloor((z - f.origin.y) / f.texel), 0, int(f.dim_z) - 1);
    const size_t i = size_t(cz) * f.dim_x + cx;
    // (under the metre's highest point by a slope's run over it and the beams a roof carries)
    return f.cover[i] && y >= f.top[i] - 0.8f && y <= f.top[i] + da_snow_open_depth_at(x, z) + contact_slack;
}

bool in_window(const Fvector& p, float radius = stamp_window)
{
    const Fvector& cam = Device.vCameraPosition;
    return _sqr(p.x - cam.x) + _sqr(p.z - cam.z) <= radius * radius;
}

u32 ring_of(const Fvector& p)
{
    const Fvector& cam = Device.vCameraPosition;
    const float d2 = _sqr(p.x - cam.x) + _sqr(p.z - cam.z);
    u32 ring = 0;
    for (u32 r = 0; r < 3; ++r)
        if (d2 <= rest_rings[r] * rest_rings[r])
            ring = r + 1;
    return ring; // 0 = outside, 3 = innermost
}

SSnowStamp make_stamp(
    float x, float z, float foot_y, float radius, float sharp, const Fvector2& push, u32 kind, float sink = 0.f)
{
    SSnowStamp s;
    s.xz.set(x, z);
    s.foot_y = foot_y;
    s.radius = radius;
    s.sharp = sharp;
    s.push = push;
    s.kind = kind;
    s.sink = sink;
    s.xz0 = s.xz;
    s.foot_y0 = foot_y;
    return s;
}

void stamp(float x, float z, float foot_y, float radius, float sharp, const Fvector2& push, u32 kind, float sink = 0.f)
{
    da_snow_stamp_add(make_stamp(x, z, foot_y, radius, sharp, push, kind, sink));
}

// A deformer of a living creature: the capsule swept since the last frame from `from` to `to` (their y the
// deformer's bottom), packing (pack) or ploughing (fall) the snow.
void stamp_swept(const Fvector& from, const Fvector& to, float radius, float sharp, const Fvector2& push, u32 kind,
    float sink, float pack, float fall)
{
    SSnowStamp s = make_stamp(to.x, to.z, to.y, radius, sharp, push, kind, sink);
    s.xz0.set(from.x, from.z);
    s.foot_y0 = from.y;
    s.pack = pack;
    s.fall = fall;
    da_snow_stamp_add(s);
}

// The footprint of a resting object, laid whole or not at all.
struct SRestPrint
{
    SSnowStamp s[rest_stamps_max];
    u32 count = 0;
};

// False when the frame's list ran full: a print cut off there is laid again by a later scan.
bool lay_print(const SRestPrint& print)
{
    for (u32 i = 0; i < print.count; ++i)
        if (!da_snow_stamp_add(print.s[i]))
            return false;
    return true;
}

// Where the creature's feet are: the foot bones the step manager reads ([foot_bones] in the visual's
// user data, else the section's foot_bones line), and how high each sits over its sole in the bind pose.
void resolve_feet(CEntityAlive& e, IKinematics& K, SCreature& c)
{
    c.kin = &K;
    c.cls = e.CLS_ID;
    c.bone_count = K.LL_BoneCount();
    c.legs = 0;
    const CInifile::Sect* sect = nullptr;
    CInifile* ud = K.LL_UserData();
    if (ud && ud->section_exist("foot_bones"))
        sect = &ud->r_section("foot_bones");
    else if (pSettings->line_exist(e.cNameSect().c_str(), "foot_bones"))
    {
        pcstr name = pSettings->r_string(e.cNameSect().c_str(), "foot_bones");
        if (pSettings->section_exist(name))
            sect = &pSettings->r_section(name);
    }
    if (!sect)
        return;
    xr_vector<Fmatrix> bind;
    K.LL_GetBindTransform(bind);
    for (const auto& item : sect->Data)
    {
        if (c.legs >= leg_max)
            break;
        const u16 id = K.LL_BoneID(item.second);
        if (id == BI_NONE || id >= bind.size())
            continue;
        c.bones[c.legs] = id;
        c.sole[c.legs] = bind[id].c.y;
        ++c.legs;
    }
}

// The footprint of a resting physics object: each element's box projected on the ground, as discs
// laid over its two most horizontal axes, packing the snow under it by its weight: a dropped rifle dents
// it, a crate sinks, a barrel or a wreck goes down to firm snow (carved to the box's bottom, a rifle cut two
// clean trenches into a drift). Its physics lies on the ground under the snow: a loose object lies at the
// bottom of its own hollow, a body no more than two fifths of each element's height under the snow (packed by
// weight alone, a rifle went under a drift out of sight; a hollow narrower than the snow grid's cells closed
// over it again). A corpse packs it by its creature's weight, each disc a little off its place
// and size (a body is no set of boxes), and a ring of scuffs round the body churns the snow it fell into.
void plan_resting(CPhysicsShellHolder& o, SRestPrint& out)
{
    out.count = 0;
    CPhysicsShell* shell = o.PPhysicsShell();
    if (!shell)
        return;
    const CEntityAlive* corpse = smart_cast<CEntityAlive*>(&o);
    const float pack = corpse ? profile_of(corpse->CLS_ID).pack : clampr(220.f + 3.f * shell->getMass(), 230.f, 650.f);
    // a corpse keeps room for its scuffs
    constexpr u32 scuffs = 10;
    u32 budget = corpse ? rest_stamps_max - scuffs : rest_stamps_max;
    // the same irregularity every time the body is stamped again
    u32 seed = u32(o.ID()) * 2654435761u + 0x9E3779B9u;
    const auto rnd = [&seed](float lo, float hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * float(seed >> 8) * (1.f / 16777216.f);
    };
    Fbox2 area;
    area.invalidate();
    float lowest = flt_max;
    const u16 count = shell->get_ElementsNumber();
    const Fvector2 still{0.f, 0.f};
    for (u16 i = 0; i < count && budget; ++i)
    {
        CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
        if (!e)
            continue;
        Fvector sz, c;
        e->get_Box(sz, c);
        const Fmatrix& X = e->XFORM();
        const Fvector axes[3] = {X.i, X.j, X.k};
        float bottom = c.y;
        float span[3];
        for (int k = 0; k < 3; ++k)
        {
            bottom -= _abs(axes[k].y) * sz[k] * 0.5f;
            span[k] = _sqrt(_sqr(axes[k].x) + _sqr(axes[k].z)) * sz[k];
        }
        // The two axes that lie flattest carry the footprint.
        int a = 0, b = 1;
        if (span[2] > span[a])
            a = 2;
        b = (a == 0) ? 1 : 0;
        for (int k = 0; k < 3; ++k)
            if (k != a && span[k] > span[b])
                b = k;
        // A loose object's discs lie dense enough that their flat floors (the stamp's dist^8 walls, flat out to 0.65 of
        // the radius) join over its whole box: sparse, a standing barrel rolled away left a clover of four bare holes.
        const float step = corpse ? clampr(std::min(span[a], span[b]), 0.12f, 0.6f) : clampr(0.5f * std::min(span[a], span[b]), 0.12f, 0.35f);
        const int na = clampr(iCeil(span[a] / step), 1, 8), nb = clampr(iCeil(span[b] / step), 1, 8);
        const float cell = std::max(span[a] / float(na), span[b] / float(nb));
        const float r = corpse ? std::max(0.75f * step, 0.08f) : std::max(1.1f * cell, 0.3f);
        for (int ia = 0; ia < na && budget; ++ia)
            for (int ib = 0; ib < nb && budget; ++ib, --budget)
            {
                const float ta = ((float(ia) + 0.5f) / float(na) * 2.f - 1.f) * 0.5f * sz[a];
                const float tb = ((float(ib) + 0.5f) / float(nb) * 2.f - 1.f) * 0.5f * sz[b];
                float x = c.x + axes[a].x * ta + axes[b].x * tb;
                float z = c.z + axes[a].z * ta + axes[b].z * tb;
                SSnowStamp s = make_stamp(x, z, bottom, r, 0.f, still, 2, corpse ? 0.4f * 2.f * (c.y - bottom) : 0.f);
                s.pack = pack;
                if (corpse)
                {
                    s.xz.x += rnd(-0.3f, 0.3f) * step;
                    s.xz.y += rnd(-0.3f, 0.3f) * step;
                    s.xz0 = s.xz;
                    s.radius = r * rnd(0.85f, 1.25f);
                    s.sharp = rnd(0.03f, 0.1f);
                    area.modify(Fvector2{s.xz.x, s.xz.y});
                    lowest = std::min(lowest, bottom);
                }
                out.s[out.count++] = s;
            }
    }
    if (!corpse || out.count == 0)
        return;
    // the scuffs: shallow ploughed patches round the body, out to a third of a metre past it
    area.grow(0.3f);
    for (u32 i = 0; i < scuffs; ++i)
    {
        const float x = rnd(area.min.x, area.max.x), z = rnd(area.min.y, area.max.y);
        SSnowStamp s = make_stamp(x, z, lowest + rnd(0.03f, 0.12f), rnd(0.1f, 0.24f), rnd(0.08f, 0.2f), still, 2);
        s.fall = rnd(0.35f, 0.65f);
        out.s[out.count++] = s;
    }
}

// The lowest point of the trunk over the body's position: a wounded stalker lies on the ground, a
// crouching actor kneels.
float belly_of(CEntityAlive& e, const SProfile& p)
{
    if (const CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(&e); stalker && stalker->wounded())
        return 0.12f;
    if (const CActor* actor = smart_cast<CActor*>(&e))
    {
        const u32 state = actor->MovingState();
        if (state & mcCrouch)
            return (state & mcAccel) ? 0.35f : 0.55f;
    }
    return p.belly;
}

// A loose object lying on its own: no parent, not a living creature, a physics shell in the world -
// asleep, unless the caller is the settle callback itself (the shell is still flagged active there).
bool resting_candidate(IGameObject* go, CPhysicsShellHolder*& holder, bool require_asleep = true)
{
    holder = nullptr;
    if (!go || go->H_Parent() || go->getDestroy())
        return false;
    CPhysicsShellHolder* h = smart_cast<CPhysicsShellHolder*>(go);
    if (!h)
        return false;
    // The living press the snow with their feet; their bodies come here once they are corpses.
    if (const CEntityAlive* alive = smart_cast<CEntityAlive*>(h); alive && alive->g_Alive())
        return false;
    CPhysicsShell* shell = h->PPhysicsShell();
    if (!shell || !shell->isActive() || (require_asleep && shell->isEnabled()))
        return false;
    holder = h;
    return true;
}
} // namespace

void da_snow_deform_entity(CEntityAlive& e)
{
    if (!e.g_Alive())
        return;
    const SSnowField* field = da_snow_field();
    if (!field)
        return;
    const Fvector& pos = e.Position();
    if (!in_window(pos))
        return;
    // Illusions leave no tracks, and a poltergeist's "feet" are fingers a metre over the ground.
    if (smart_cast<CPsyDogPhantom*>(&e) || smart_cast<CPoltergeist*>(&e))
        return;
    IKinematics* K = smart_cast<IKinematics*>(e.Visual());
    if (!K)
        return;

    SCreature& c = g_creatures[e.ID()];
    // A new body under a recycled id, or a new visual: resolve everything again.
    if (c.kin != K || c.cls != e.CLS_ID || c.bone_count != K->LL_BoneCount())
    {
        c.profile = &profile_of(e.CLS_ID);
        resolve_feet(e, *K, c);
        c.time = -1.f;
    }
    if (c.profile->skip || c.frame == Device.dwFrame)
        return;
    // The eye's own body is not drawn in first person, so nothing else keeps its skeleton current.
    if (const IGameObject* view = Level().CurrentViewEntity(); view && view->ID() == e.ID())
        K->CalculateBones();

    const float now = Device.fTimeGlobal;
    const float dt = c.time < 0.f ? 0.f : now - c.time;
    const bool fresh = dt <= 0.f || dt > 0.5f;
    c.time = now;
    c.frame = Device.dwFrame;
    const float k = fresh ? 1.f : 1.f - std::exp(-dt * 12.f);

    // Body velocity (the grass push), by finite difference; a jump of metres is a teleport.
    Fvector2 v{0.f, 0.f};
    if (!fresh)
    {
        v.set((pos.x - c.pos.x) / dt, (pos.z - c.pos.z) / dt);
        if (v.square_magnitude() > 15.f * 15.f)
            v.set(0.f, 0.f);
    }
    c.vel.x += (v.x - c.vel.x) * k;
    c.vel.y += (v.y - c.vel.y) * k;
    const Fvector prev_pos = c.pos;
    c.pos = pos;

    const SProfile& p = *c.profile;
    const Fmatrix& X = e.XFORM();
    const float body_speed = _sqrt(v.square_magnitude());
    for (u32 l = 0; l < c.legs; ++l)
    {
        Fmatrix W;
        W.mul_43(X, K->LL_GetBoneInstance(c.bones[l]).mTransform);
        const Fvector f = W.c;
        Fvector2 fv{0.f, 0.f};
        if (!fresh)
        {
            fv.set((f.x - c.foot[l].x) / dt, (f.z - c.foot[l].z) / dt);
            if (fv.square_magnitude() > 15.f * 15.f)
                fv.set(0.f, 0.f);
        }
        c.foot_vel[l].x += (fv.x - c.foot_vel[l].x) * k;
        c.foot_vel[l].y += (fv.y - c.foot_vel[l].y) * k;
        Fvector from{c.foot[l].x, c.foot[l].y - c.sole[l], c.foot[l].z};
        c.foot[l] = f;
        if (p.foot_r <= 0.f)
            continue;

        const float sole = f.y - c.sole[l];
        if (!in_snow(*field, f.x, f.z, sole))
            continue;
        // A planted foot packs the snow under it and sinks; a swinging one still in the snow ploughs a furrow at
        // its own height. Swept from the last frame (a fresh body or a jump of a metre: a disc).
        const bool planted = _sqrt(fv.square_magnitude()) <= planted_share * body_speed + planted_margin;
        const Fvector to{f.x, sole, f.z};
        if (fresh || from.distance_to_sqr(to) > 1.f)
            from = to;
        stamp_swept(from, to, p.foot_r, 0.06f, c.foot_vel[l], 0, p.sink, planted ? p.pack : 0.f,
            planted ? 0.f : (p.body_r < 0.15f ? fall_leg_small : fall_leg));
    }

    if (p.body_r > 0.f)
    {
        const float top = top_under(*field, pos.x, pos.z);
        const float belly = pos.y + belly_of(e, p);
        if (pos.y >= top - under_roof)
        {
            Fvector from{prev_pos.x, c.body_y, prev_pos.z};
            const Fvector to{pos.x, belly, pos.z};
            if (fresh || from.distance_to_sqr(to) > 4.f)
                from = to;
            stamp_swept(from, to, p.body_r, 0.12f, c.vel, 1, p.sink, 0.f, p.body_r < 0.15f ? fall_body_small : fall_body);
        }
        c.body_y = belly;
    }
}

void da_snow_deform_crater(const Fvector& pos, float radius)
{
    const SSnowField* field = da_snow_field();
    if (!field || !da_snow_frame().active || !in_window(pos))
        return;
    const float top = top_under(*field, pos.x, pos.z);
    const float depth = da_snow_depth_at(pos.x, pos.z);
    if (pos.y < top - under_roof || depth <= 0.f)
        return;
    const float r = clampr(0.3f * radius, 0.6f, 3.f);
    // An air burst throws nothing off the ground.
    if (pos.y > top + depth + 0.4f * r)
        return;
    const Fvector2 still{0.f, 0.f};
    // An irregular hole (the RDR2 way): a core and seven lobes of random reach and size, each a flat bare floor in
    // steep walls (a bowl from the centre read as a soft dimple, not a blast).
    stamp(pos.x, pos.z, top, 0.75f * r, 0.03f, still, 3);
    constexpr int lobes = 7;
    for (int i = 0; i < lobes; ++i)
    {
        const float ang = float(i) * (PI_MUL_2 / float(lobes)) + ::Random.randF(-0.3f, 0.3f);
        const float d = r * ::Random.randF(0.35f, 0.6f);
        stamp(pos.x + _sin(ang) * d, pos.z + _cos(ang) * d, top, r * ::Random.randF(0.3f, 0.5f), 0.03f, still, 3);
    }
}

// A bullet's strike on the ground under the snow: the channel it bored from where it went in to where it struck, a
// few centimetres wide, with a dent where it went in; a load of shot lands as a cluster of them.
bool da_snow_deform_bullet(const Fvector& point, const Fvector& dir, float wallmark)
{
    const SSnowField* field = da_snow_field();
    if (!field || !da_snow_frame().active || !in_window(point, 60.f))
        return false;
    // (the surface bilinear at the very point: the lowest of the texels round it lay well under a slope's ground, and
    // every strike into shallow snow on a slope read as over the snow and left nothing)
    const float depth = da_snow_depth_at(point.x, point.z);
    const float surface = da_snow_surface_y(point.x, point.z);
    if (depth < 0.02f || point.y < surface - depth - under_roof || point.y > surface + 0.08f)
        return false;
    // where it went in: back along its flight to the snow's surface, a metre at most
    const float t = std::min(std::max(surface - point.y, 0.f) / std::max(-dir.y, 0.05f), 1.f);
    Fvector in;
    in.mad(point, dir, -t);
    const float r = clampr(0.5f * wallmark, 0.035f, 0.06f);
    const Fvector2 still{0.f, 0.f};
    SSnowStamp s = make_stamp(point.x, point.z, point.y, r, 0.05f, still, 3);
    s.xz0.set(in.x, in.z);
    s.foot_y0 = in.y;
    da_snow_stamp_add(s);
    return da_snow_stamp_add(make_stamp(in.x, in.z, in.y - 0.04f, 1.6f * r, 0.05f, still, 3));
}

namespace
{
// The physics in motion (an enabled shell, no parent, no living creature) round the camera: each element's box
// bottom swept from the last frame ploughs a furrow at its own height, a third of the snow over it falling back -
// a barrel rolling, a crate pushed, a body dragged or kicked. At rest again, the settle callback lays its print.
void moving_frame(const SSnowField& field)
{
    const float now = Device.fTimeGlobal;
    if (now >= g_next_moving && g_pGamePersistent)
    {
        g_next_moving = now + moving_scan;
        static xr_vector<ISpatial*> found;
        found.clear();
        g_pGamePersistent->SpatialSpace.q_box(found, 0, STYPE_COLLIDEABLE, Device.vCameraPosition,
            Fvector{moving_window, 30.f, moving_window});
        for (auto& [id, m] : g_moving)
            m.count = u32(-1); // not seen yet in this scan
        for (ISpatial* s : found)
        {
            IGameObject* go = s->dcast_GameObject();
            CPhysicsShellHolder* h = go ? smart_cast<CPhysicsShellHolder*>(go) : nullptr;
            if (!h || go->H_Parent() || go->getDestroy())
                continue;
            if (const CEntityAlive* alive = smart_cast<CEntityAlive*>(h); alive && alive->g_Alive())
                continue;
            CPhysicsShell* shell = h->PPhysicsShell();
            if (!shell || !shell->isActive() || !shell->isEnabled())
                continue;
            if (g_moving.size() >= moving_max && !g_moving.count(h->ID()))
                continue;
            SMoving& m = g_moving[h->ID()];
            if (m.count == u32(-1))
                m.count = 0;
        }
        for (auto it = g_moving.begin(); it != g_moving.end();)
            it = it->second.count == u32(-1) ? g_moving.erase(it) : std::next(it);
    }
    for (auto it = g_moving.begin(); it != g_moving.end();)
    {
        CPhysicsShellHolder* h = smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(it->first));
        CPhysicsShell* shell = h ? h->PPhysicsShell() : nullptr;
        if (!shell || !shell->isActive() || !shell->isEnabled() || h->H_Parent())
        {
            it = g_moving.erase(it);
            continue;
        }
        SMoving& m = it->second;
        const u16 count = std::min<u16>(shell->get_ElementsNumber(), u16(moving_elements));
        const bool fresh = m.fresh || m.count != count;
        m.count = count;
        m.fresh = false;
        for (u16 i = 0; i < count && da_snow_stamps_room() > creature_reserve / 2; ++i)
        {
            CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
            if (!e)
                continue;
            Fvector sz, c;
            e->get_Box(sz, c);
            const Fmatrix& X = e->XFORM();
            const Fvector axes[3] = {X.i, X.j, X.k};
            float bottom = c.y, half = flt_max;
            for (int k = 0; k < 3; ++k)
            {
                bottom -= _abs(axes[k].y) * sz[k] * 0.5f;
                // the footprint's half width: the narrower of the flattest axes
                if (_abs(axes[k].y) < 0.7f)
                    half = std::min(half, 0.5f * _sqrt(_sqr(axes[k].x) + _sqr(axes[k].z)) * sz[k]);
            }
            const Fvector to{c.x, bottom, c.z};
            Fvector from = fresh ? to : m.bottom[i];
            m.bottom[i] = to;
            if (from.distance_to_sqr(to) > 1.f)
                from = to;
            if (from.distance_to_sqr(to) < 1e-6f)
                continue;
            if (!in_snow(field, to.x, to.z, bottom))
                continue;
            const Fvector2 push{(to.x - from.x) / std::max(Device.fTimeDelta, 1e-3f), (to.z - from.z) / std::max(Device.fTimeDelta, 1e-3f)};
            stamp_swept(from, to, clampr(half == flt_max ? 0.15f : half, 0.06f, 0.4f), 0.1f, push, 1, 0.f, 0.f, 0.35f);
        }
        ++it;
    }
}

// The campfires: the pit itself never holds more than a dusting (the fire can be lit, its logs and stones show), and
// a lit one thaws the snow round it over a minute and a half - bare in the pit, thinner and thinner out to two and a
// half metres, a bowl of melt with no edge. What melted comes back only with fresh snow.
void campfires_frame(const SSnowField& field)
{
    const Fvector2 still{0.f, 0.f};
    for (CZoneCampfire* fire : CZoneCampfire::all())
    {
        const Fvector& p = fire->Position();
        if (!in_window(p, 60.f))
            continue;
        const float top = top_under(field, p.x, p.z);
        const float depth = da_snow_depth_at(p.x, p.z);
        if (depth < 0.01f || p.y < top - under_roof)
            continue;
        const float melt = fire->snow_melt();
        const float dust = std::min(depth, 0.03f);
        // (sink 1: a bowl, the thaw's soft slope - not a crater's steep walls)
        stamp(p.x, p.z, top + dust * (1.f - melt), fire_pit_r + 0.35f * melt, 0.05f, still, 3, 1.f);
        if (melt > 0.f)
            stamp(p.x, p.z, top + depth * (1.f - 0.7f * melt), fire_pit_r + (fire_thaw_r - fire_pit_r) * melt, 0.02f, still, 3, 1.f);
    }
}
} // namespace

void da_snow_probe(const Fvector& p)
{
    const SSnowField* f = da_snow_field();
    if (!f)
    {
        Msg("* [qa] snow probe: no field");
        return;
    }
    const int cx = clampr(iFloor((p.x - f->origin.x) / f->texel), 0, int(f->dim_x) - 1);
    const int cz = clampr(iFloor((p.z - f->origin.y) / f->texel), 0, int(f->dim_z) - 1);
    const size_t i = size_t(cz) * f->dim_x + cx;
    Msg("* [qa] snow probe at %.2f %.2f %.2f: top under %.2f, texel top %.2f ground %.2f cover %u, depth %.3f open %.3f, in snow %s",
        p.x, p.y, p.z, top_under(*f, p.x, p.z), f->top[i], f->ground[i], f->cover.empty() ? 0u : u32(f->cover[i]),
        da_snow_depth_at(p.x, p.z), da_snow_open_depth_at(p.x, p.z), in_snow(*f, p.x, p.z, p.y) ? "yes" : "no");
}

void da_snow_deform_settled(CPhysicsShellHolder& object)
{
    if (!da_snow_field() || !da_snow_frame().active)
        return;
    CPhysicsShellHolder* h;
    if (!resting_candidate(&object, h, false) || !in_window(h->Position()))
        return;
    SRestPrint print;
    plan_resting(*h, print);
    // Not past the creatures' share of the frame's list, and recorded only when laid whole: otherwise the
    // next scan lays it.
    if (print.count + creature_reserve > da_snow_stamps_room() || !lay_print(print))
        return;
    g_resting[h->ID()] = {h->Position(), g_scan, ring_of(h->Position())};
}

void da_snow_deform_frame()
{
    const float now = Device.fTimeGlobal;
    if (now >= g_next_prune)
    {
        g_next_prune = now + 2.f;
        for (auto it = g_creatures.begin(); it != g_creatures.end();)
            it = (it->second.time < 0.f || now - it->second.time > 5.f) ? g_creatures.erase(it) : std::next(it);
    }

    const bool active = da_snow_field() && da_snow_frame().active;
    if (active)
    {
        moving_frame(*da_snow_field());
        campfires_frame(*da_snow_field());
    }
    // The snow came (a new month, a QA sweep): whatever lies in the window is stamped afresh.
    if (active && !g_was_active)
    {
        g_resting.clear();
        g_next_scan = 0.f;
    }
    g_was_active = active;
    if (!active || now < g_next_scan || !g_pGamePersistent)
        return;
    g_next_scan = now + 0.5f;
    ++g_scan;

    static xr_vector<ISpatial*> found;
    found.clear();
    const Fvector half{stamp_window, 60.f, stamp_window};
    g_pGamePersistent->SpatialSpace.q_box(found, 0, STYPE_COLLIDEABLE, Device.vCameraPosition, half);
    struct SPending
    {
        float d2;
        u32 ring;
        CPhysicsShellHolder* h;
    };
    static xr_vector<SPending> pending;
    pending.clear();
    const Fvector& cam = Device.vCameraPosition;
    for (ISpatial* s : found)
    {
        CPhysicsShellHolder* h;
        if (!resting_candidate(s->dcast_GameObject(), h))
            continue;
        const Fvector& p = h->Position();
        const u32 ring = ring_of(p);
        if (!ring)
            continue;
        const auto it = g_resting.find(h->ID());
        if (it != g_resting.end() && it->second.pos.similar(p, 0.2f) && ring <= it->second.ring)
        {
            it->second.scan = g_scan;
            it->second.ring = ring;
            continue;
        }
        pending.push_back({_sqr(p.x - cam.x) + _sqr(p.z - cam.z), ring, h});
    }
    // Nearest first, within the scan's share of the list; whatever does not fit waits for the next scan.
    std::sort(pending.begin(), pending.end(), [](const SPending& a, const SPending& b) { return a.d2 < b.d2; });
    const u32 room = da_snow_stamps_room();
    u32 budget = std::min(rest_scan_budget, room > creature_reserve ? room - creature_reserve : 0u);
    SRestPrint print;
    for (const SPending& item : pending)
    {
        plan_resting(*item.h, print);
        if (print.count > budget)
            continue;
        budget -= print.count;
        if (lay_print(print))
            g_resting[item.h->ID()] = {item.h->Position(), g_scan, item.ring};
    }
    // What left the window (or was not laid) is forgotten, so it is stamped again when it comes back.
    for (auto it = g_resting.begin(); it != g_resting.end();)
        it = it->second.scan != g_scan ? g_resting.erase(it) : std::next(it);
}

void da_snow_deform_reset()
{
    g_creatures.clear();
    g_resting.clear();
    g_moving.clear();
    g_next_moving = 0.f;
    g_scan = 0;
    g_next_scan = 0.f;
    g_next_prune = 0.f;
    g_was_active = false;
    g_profiles_loaded = false;
}
