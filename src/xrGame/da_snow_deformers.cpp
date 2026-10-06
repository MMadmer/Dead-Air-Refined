// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Everything that presses the snow (docs/dead-air/WINTER.md), as stamps for the renderer's trail maps
// and the grass push map (da_snow_stamp_add, SSnowStamp):
//   feet    - every creature in a 96 m window round the camera, every frame, wherever a planted sole is
//             on the snow surface or under it (the contact gate; the step manager's 'disable' gate
//             skipped every motion a creature has no step parameters for - all but walk and run for a
//             tushkan, a rat, a burer or a chimera); a foot swinging through the snow leaves no
//             furrow, so the prints read one by one (it still brushes the grass);
//   body    - a disc under each body at its belly height: the low bodies (tushkans, dogs, boars) plough
//             deep snow, every body pushes the grass;
//   feet and bodies sink no deeper than the creature's weight packs the snow (sink: a rat 1.2 cm, a
//   dog 4-5 cm, a man 6.5 cm, a boar or a flesh 8 cm, a pseudogiant 15 cm), whatever depth they reach;
//   resting - a corpse, loot, an artefact or a prop that comes to rest leaves its footprint once, and
//             again whenever it comes back into the window, so nothing lies buried under a drift;
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

struct SProfile
{
    float foot_r = 0.f;  // m, 0 = no feet stamps
    float body_r = 0.f;  // m, 0 = no body disc
    float belly = 0.f;   // m above the body's position: the lowest point of the trunk
    float sink = 0.f;    // m under the snow surface a foot or the body packs the snow down to
    bool skip = false;
};

struct SProfileDefault
{
    pcstr clsid;
    SProfile p;
};

// Radii from the brief's deformer table (foot spacing and body boxes of the shipped visuals); belly
// heights from the models' bind poses. Humans' discs sit at the hip: they push grass, never snow. Sink
// depths by weight over paw area: rats and tushkans 1-2 cm, dogs 3-6 cm, people 5-8 cm, boars and fleshes
// 6-10 cm, the pseudogiant up to 15 cm.
const SProfileDefault profile_defaults[] = {
    {"S_ACTOR", {0.12f, 0.30f, 0.90f, 0.065f, false}},
    {"AI_STL_S", {0.12f, 0.30f, 0.90f, 0.065f, false}},
    {"SM_TUSHK", {0.06f, 0.12f, 0.10f, 0.015f, false}},
    {"SM_RAT", {0.00f, 0.08f, 0.03f, 0.012f, false}},
    {"SM_DOG_S", {0.07f, 0.20f, 0.42f, 0.04f, false}},
    {"SM_P_DOG", {0.08f, 0.22f, 0.44f, 0.05f, false}},
    {"SM_DOG_P", {0.08f, 0.22f, 0.44f, 0.05f, false}},
    {"SM_DOG_F", {0.f, 0.f, 0.f, 0.f, true}},
    {"SM_BOARW", {0.10f, 0.35f, 0.35f, 0.085f, false}},
    {"SM_FLESH", {0.10f, 0.40f, 0.30f, 0.08f, false}},
    {"SM_CAT_S", {0.06f, 0.15f, 0.30f, 0.025f, false}},
    {"SM_SNORK", {0.10f, 0.30f, 0.20f, 0.06f, false}},
    {"SM_BLOOD", {0.12f, 0.30f, 0.90f, 0.08f, false}},
    {"SM_CONTR", {0.12f, 0.30f, 0.90f, 0.07f, false}},
    {"SM_BURER", {0.14f, 0.35f, 0.45f, 0.07f, false}},
    {"SM_CHIMS", {0.12f, 0.40f, 0.40f, 0.09f, false}},
    {"SM_GIANT", {0.25f, 0.80f, 0.45f, 0.15f, false}},
    {"SM_IZLOM", {0.12f, 0.30f, 0.90f, 0.065f, false}},
    {"SM_ZOMBI", {0.12f, 0.30f, 0.90f, 0.065f, false}},
    {"SM_POLTR", {0.f, 0.f, 0.f, 0.f, true}},
};
constexpr SProfile profile_fallback{0.10f, 0.30f, 0.60f, 0.06f, false};
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
    // <class id> = foot radius, body radius, belly height [, sink]  |  none
    for (const auto& item : ini.r_section("snow_deformers").Data)
    {
        if (!item.first.size() || !item.second.size())
            continue;
        const CLASS_ID id = TEXT2CLSID(item.first.c_str());
        SProfile p;
        // a line without a sink keeps the built-in one of its class
        const auto known = g_profiles.find(id);
        p.sink = known != g_profiles.end() ? known->second.sink : profile_fallback.sink;
        if (!xr_stricmp(item.second.c_str(), "none"))
            p.skip = true;
        else if (sscanf(item.second.c_str(), "%f , %f , %f , %f", &p.foot_r, &p.body_r, &p.belly, &p.sink) < 3)
        {
            Msg("! [snow] [snow_deformers] %s: expected 'foot radius, body radius, belly height[, sink]' or 'none'",
                item.first.c_str());
            continue;
        }
        p.foot_r = clampr(p.foot_r, 0.f, 1.f);
        p.body_r = clampr(p.body_r, 0.f, 2.f);
        p.belly = clampr(p.belly, 0.f, 3.f);
        p.sink = clampr(p.sink, 0.f, 0.6f);
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
    return s;
}

void stamp(float x, float z, float foot_y, float radius, float sharp, const Fvector2& push, u32 kind, float sink = 0.f)
{
    da_snow_stamp_add(make_stamp(x, z, foot_y, radius, sharp, push, kind, sink));
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
// laid over its two most horizontal axes, carved down to the box's bottom.
void plan_resting(CPhysicsShellHolder& o, SRestPrint& out)
{
    out.count = 0;
    CPhysicsShell* shell = o.PPhysicsShell();
    if (!shell)
        return;
    u32 budget = rest_stamps_max;
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
        const float step = clampr(std::min(span[a], span[b]), 0.12f, 0.6f);
        const int na = clampr(iCeil(span[a] / step), 1, 8), nb = clampr(iCeil(span[b] / step), 1, 8);
        const float r = std::max(0.75f * step, 0.08f);
        for (int ia = 0; ia < na && budget; ++ia)
            for (int ib = 0; ib < nb && budget; ++ib, --budget)
            {
                const float ta = ((float(ia) + 0.5f) / float(na) * 2.f - 1.f) * 0.5f * sz[a];
                const float tb = ((float(ib) + 0.5f) / float(nb) * 2.f - 1.f) * 0.5f * sz[b];
                const float x = c.x + axes[a].x * ta + axes[b].x * tb;
                const float z = c.z + axes[a].z * ta + axes[b].z * tb;
                out.s[out.count++] = make_stamp(x, z, bottom, r, 0.03f, still, 2);
            }
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
        c.foot[l] = f;
        if (p.foot_r <= 0.f)
            continue;

        const float sole = f.y - c.sole[l];
        const float top = top_under(*field, f.x, f.z);
        if (sole < top - under_roof)
            continue;
        if (sole > top + da_snow_depth_at(f.x, f.z) + contact_slack)
            continue;
        // A swinging foot brushes the grass but passes through the snow without a print.
        const bool planted = _sqrt(fv.square_magnitude()) <= planted_share * body_speed + planted_margin;
        stamp(f.x, f.z, sole, p.foot_r, 0.06f, c.foot_vel[l], 0, planted ? p.sink : -1.f);
    }

    if (p.body_r > 0.f)
    {
        const float top = top_under(*field, pos.x, pos.z);
        if (pos.y >= top - under_roof)
            stamp(pos.x, pos.z, pos.y + belly_of(e, p), p.body_r, 0.12f, c.vel, 1, p.sink);
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
    // An irregular hole (the RDR2 way): a core and seven lobes of random reach and size.
    stamp(pos.x, pos.z, top, 0.75f * r, 0.25f * r, still, 3);
    constexpr int lobes = 7;
    for (int i = 0; i < lobes; ++i)
    {
        const float ang = float(i) * (PI_MUL_2 / float(lobes)) + ::Random.randF(-0.3f, 0.3f);
        const float d = r * ::Random.randF(0.35f, 0.6f);
        stamp(pos.x + _sin(ang) * d, pos.z + _cos(ang) * d, top, r * ::Random.randF(0.3f, 0.5f), 0.2f * r, still, 3);
    }
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
    g_scan = 0;
    g_next_scan = 0.f;
    g_next_prune = 0.f;
    g_was_active = false;
    g_profiles_loaded = false;
}
