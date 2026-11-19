// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_glows.h"

#include "Layers/xrRender/ResourceManager.h"
#include "xrCore/XMS/xms_level_things.h"
#include "xrCDB/xr_area.h"
#include "xrEngine/IGame_Level.h"

#include <algorithm>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
struct Glow
{
    Fvector position;
    float radius;
    Fvector color;
    bool fixed;
    ref_shader shader;
    // how much of it is shown, 0 to 1: it follows `seen`
    float shown{};
    bool seen{};
    collide::ray_cache cache{};
};

// Past this a glow is not drawn, and it fades over the last third of the way.
constexpr float far_distance = 90.f;
// Seconds a glow takes to come up or to go.
constexpr float fade_time = 0.25f;
// How many glows have their line of sight tested a frame; the rest keep what they had.
constexpr u32 tests_a_frame = 24;
// One draw's quads.
constexpr u32 max_drawn = 256;

xr_vector<Glow> g_glows;
ref_geom g_geom;
u32 g_next_test{};

// The shader a glow is drawn with: one of the two the game draws a picture that faces the viewer
// with - the lens flares' (added to the frame, seen through nothing but by the line of sight) and
// the sun's (the same, hidden by depth as well). The game's own `effects\glow` is made for
// vertices that are on the screen already and draws nothing of a glow in this renderer, and any
// other shader of the game is made for geometry a glow has not - it stops the game as it is
// compiled, or draws nothing: a glow that names one, or none, is drawn with the flares'.
pcstr shader_of(const xr_string& asked, bool& replaced)
{
    constexpr pcstr flare = "effects\\flare";
    constexpr pcstr sun = "effects\\sun";
    replaced = false;
    pcstr want = flare;
    if (0 == xr_stricmp(asked.c_str(), sun))
        want = sun;
    else if (!asked.empty() && 0 != xr_stricmp(asked.c_str(), flare) && 0 != xr_stricmp(asked.c_str(), "effects\\glow"))
        replaced = true;
    if (RImplementation.Resources->_FindBlender(want))
        return want;
    return RImplementation.Resources->_FindBlender(flare) ? flare : nullptr;
}
} // namespace

void xms_glows_level_unload()
{
    g_glows.clear();
    g_geom.destroy();
    g_next_test = 0;
}

void xms_glows_level_load()
{
    xms_glows_level_unload();
    XMS::LevelThings things;
    XMS::ReadLevelThings(things);
    if (things.glows.empty())
        return;
    u32 left_out = 0;
    u32 other_shader = 0;
    for (const XMS::LevelGlow& from : things.glows)
    {
        bool replaced = false;
        pcstr shader = shader_of(from.shader, replaced);
        other_shader += replaced ? 1 : 0;
        Glow g;
        g.position = from.position;
        g.radius = from.radius;
        g.color = from.color;
        g.fixed = from.fixed;
        if (shader)
            g.shader.create(shader, from.texture.c_str());
        // a shader that made no pass has nothing to draw with
        if (!g.shader || !g.shader->E[0])
        {
            ++left_out;
            continue;
        }
        g_glows.push_back(std::move(g));
    }
    if (other_shader)
        Msg("! [level things] %u glows name a shader no glow is drawn with: they are drawn with the lens flares' "
            "(effects\\flare; effects\\sun is the other one a glow takes)",
            other_shader);
    if (left_out)
        Msg("! [level things] %u glows are left out: the game has no shader to draw them with", left_out);
    if (!g_glows.empty())
    {
        g_geom.create(FVF::F_LIT, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        Msg("* [level things] %u glows", u32(g_glows.size()));
    }
}

void xms_glows_render()
{
    if (g_glows.empty() || !g_pGameLevel || !g_geom)
        return;
    const Fvector eye = Device.vCameraPosition;
    const Fvector look = Device.vCameraDirection;

    // the line of sight of a few glows a frame, in turn: nothing of the level between the eye and
    // the glow's place. A glow too far to be drawn costs no test, so the near ones keep their
    // turn however many glows a level has.
    const u32 count = u32(g_glows.size());
    for (u32 looked = 0, tested = 0; looked < count && tested < tests_a_frame; ++looked)
    {
        Glow& g = g_glows[g_next_test % count];
        ++g_next_test;
        Fvector way;
        way.sub(g.position, eye);
        const float distance = way.magnitude();
        if (distance > far_distance)
        {
            g.seen = false;
            continue;
        }
        if (distance < 0.05f)
        {
            g.seen = true;
            continue;
        }
        way.div(distance);
        ++tested;
        // a glow stands at a lamp's glass: the last few centimetres are the lamp's own
        g.seen = !g_pGameLevel->ObjectSpace.RayTest(
            eye, way, std::max(distance - 0.1f, 0.f), collide::rqtStatic, &g.cache, nullptr);
    }

    const float step = Device.fTimeDelta / fade_time;
    struct Drawn
    {
        Glow* glow;
        float alpha;
        float distance;
    };
    svector<Drawn, max_drawn> drawn;
    for (Glow& g : g_glows)
    {
        g.shown = std::clamp(g.shown + (g.seen ? step : -step), 0.f, 1.f);
        if (g.shown <= 0.f || drawn.size() >= max_drawn)
            continue;
        Fvector way;
        way.sub(g.position, eye);
        // behind the viewer
        if (way.dotproduct(look) <= 0.f)
            continue;
        const float distance = way.magnitude();
        const float fade = std::clamp((far_distance - distance) / (far_distance / 3.f), 0.f, 1.f);
        if (fade <= 0.f)
            continue;
        drawn.push_back({&g, g.shown * fade, distance});
    }
    if (drawn.empty())
        return;
    // half the screen's height at one metre: what a glow of a fixed size is measured in
    const float screen = Device.mProject._22 > 1e-4f ? 1.f / Device.mProject._22 : 1.f;

    u32 offset = 0;
    auto* pv = static_cast<FVF::LIT*>(RImplementation.Vertex.Lock(drawn.size() * 4, g_geom.stride(), offset));
    for (const Drawn& d : drawn)
    {
        const Glow& g = *d.glow;
        const float radius = g.fixed ? g.radius * d.distance * screen : g.radius;
        Fvector x, y;
        x.mul(Device.vCameraRight, radius);
        y.mul(Device.vCameraTop, radius);
        Fcolor c;
        c.set(g.color.x, g.color.y, g.color.z, d.alpha);
        // an additive picture fades by its colour, a blended one by its alpha: both are given
        c.mul_rgb(d.alpha);
        c.r = std::min(c.r, 1.f);
        c.g = std::min(c.g, 1.f);
        c.b = std::min(c.b, 1.f);
        const u32 packed = c.get();
        const Fvector& p = g.position;
        pv->set(p.x + x.x - y.x, p.y + x.y - y.y, p.z + x.z - y.z, packed, 0, 0);
        ++pv;
        pv->set(p.x + x.x + y.x, p.y + x.y + y.y, p.z + x.z + y.z, packed, 0, 1);
        ++pv;
        pv->set(p.x - x.x - y.x, p.y - x.y - y.y, p.z - x.z - y.z, packed, 1, 0);
        ++pv;
        pv->set(p.x - x.x + y.x, p.y - x.y + y.y, p.z - x.z + y.z, packed, 1, 1);
        ++pv;
    }
    RImplementation.Vertex.Unlock(drawn.size() * 4, g_geom.stride());

    RCache.set_xform_world(Fidentity);
    RCache.set_Geometry(g_geom);
    for (u32 i = 0; i < drawn.size(); ++i)
    {
        RCache.set_Shader(drawn[i].glow->shader);
        // a picture that faces the viewer has no back: which way its corners turn decides
        // nothing. Said after the shader, whose own states it would otherwise be lost to.
        RCache.set_CullMode(CULL_NONE);
        RCache.Render(D3DPT_TRIANGLELIST, offset + i * 4, 0, 4, 0, 2);
    }
    RCache.set_CullMode(CULL_CCW);
}
} // namespace xray::render::RENDER_NAMESPACE
