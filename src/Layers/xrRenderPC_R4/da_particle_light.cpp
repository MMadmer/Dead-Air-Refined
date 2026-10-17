// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Lit particles, after DOOM Eternal (its particles take the sun with its shadow, the ambient and the local lights;
// shaders/r3/particle.vs): the blended particles - smoke, dust, steam, mist - were drawn at their authored colour in
// any light, so they glowed grey in the night and under every roof. This keeps what the vertex shader needs: the
// frame's nearest local lights and the froxel volume's sun visibility.

#include "stdafx.h"

#include "Layers/xrRenderPC_R4/da_particle_light.h"
#include "Layers/xrRender/Light_Package.h"
#include "Layers/xrRender/ResourceManager.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Per light: position and 1 / range^2; colour and the spot's cosine of its half cone (-2: a point light); direction.
constexpr u32 kLights = 8;
constexpr u32 kRows = 3;
// Lights farther than this from the eye (their sphere's near side) light no particle.
constexpr float kReach = 80.f;

struct ParticleLights
{
    u32 frame = 0;
    u32 count = 0;
    Fvector4 rows[kLights * kRows]{};
};
ParticleLights g_lights;
// The last frame a lit particle was drawn on: only the lit particles' shaders take this binder.
u32 g_seen = 0;

class cl_da_particle_light : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        g_seen = Device.dwFrame;
        const bool lights = g_lights.frame == Device.dwFrame;
        cmd_list.set_c(C, ps_r__particle_light ? 1.f : 0.f, da_froxel_frame().frame == Device.dwFrame ? 1.f : 0.f,
            lights ? float(g_lights.count) : 0.f, ps_r__tonemap_white > 0.01f ? ps_r__tonemap_white : 1.7f);
    }
} binder_da_particle_light;

class cl_da_particle_lights : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        for (u32 i = 0; i < g_lights.count * kRows; ++i)
            cmd_list.set_ca(C, i, g_lights.rows[i]);
    }
} binder_da_particle_lights;

// The QA views' switch for every particle; unlike da_particle_light it keeps no froxel volume alive.
class cl_da_particle_qa : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, float(ps_r__particle_light), 0.f, 0.f, 0.f); }
} binder_da_particle_qa;

// The scene's own light terms as combine_1 takes them (r4_rendertarget_phase_combine.cpp): the sun light's colour,
// the sky's tint (2 x env_color x the hemi scale, w the weathers' blend) and the ambient (2 x ambient x its scale).
class cl_da_particle_sun : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const light* sun = (light*)RImplementation.Lights.sun._get();
        const float clouds = g_pGamePersistent->Environment().cloud_sun_visibility;
        if (sun)
            cmd_list.set_c(C, sun->color.r, sun->color.g, sun->color.b, clouds);
        else
            cmd_list.set_c(C, 0.f, 0.f, 0.f, clouds);
    }
} binder_da_particle_sun;

class cl_da_particle_env : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment().CurrentEnv;
        const float k = 2.f * ps_r2_sun_lumscale_hemi;
        cmd_list.set_c(C, env.env_color.x * k, env.env_color.y * k, env.env_color.z * k, env.env_color.w);
    }
} binder_da_particle_env;

class cl_da_particle_amb : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment().CurrentEnv;
        const float k = 2.f * ps_r2_sun_lumscale_amb;
        cmd_list.set_c(C, std::max(env.ambient.x * 2.f, 0.001f) * ps_r2_sun_lumscale_amb,
            std::max(env.ambient.y * 2.f, 0.001f) * ps_r2_sun_lumscale_amb,
            std::max(env.ambient.z * 2.f, 0.001f) * ps_r2_sun_lumscale_amb, k);
    }
} binder_da_particle_amb;

// The froxel volume's mapping, for the vertex shader that reads it (the volume's own passes set it themselves).
class cl_da_froxel_grid : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_froxel_frame().grid); }
} binder_da_froxel_grid;

class cl_da_froxel_range : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_froxel_frame().range); }
} binder_da_froxel_range;

class cl_da_froxel_ray : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_froxel_frame().ray); }
} binder_da_froxel_ray;
} // namespace

bool da_particle_light_wanted()
{
    // Two seconds past the last lit particle: a puff that comes back into view finds the volume still there.
    return ps_r__particle_light && g_seen && Device.dwFrame <= g_seen + 120;
}

void da_particle_light_snapshot(const light_Package& package)
{
    // The nearest lights by the near side of their spheres, those that reach a particle near the eye first. The lights
    // come in two packages a frame (those settled and those whose occlusion query is in flight): both count.
    struct Candidate
    {
        light* L;
        float gap;
    };
    static xr_vector<Candidate> candidates;
    if (g_lights.frame != Device.dwFrame)
        candidates.clear();
    const auto consider = [&](const xr_vector<light*>& list)
    {
        for (light* L : list)
        {
            if (!L || !L->flags.bActive || L->range <= EPS_L)
                continue;
            const float gap = Device.vCameraPosition.distance_to(L->position) - L->range;
            if (gap < kReach)
                candidates.push_back({L, gap});
        }
    };
    consider(package.v_point);
    consider(package.v_spot);
    consider(package.v_shadowed);
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.gap < b.gap; });

    g_lights.frame = Device.dwFrame;
    g_lights.count = 0;
    for (const Candidate& c : candidates)
    {
        // A point light lit through shadowed faces sits in the package once per face: one entry each.
        bool seen = false;
        for (u32 i = 0; i < g_lights.count && !seen; ++i)
            seen = g_lights.rows[i * kRows].x == c.L->position.x && g_lights.rows[i * kRows].y == c.L->position.y &&
                g_lights.rows[i * kRows].z == c.L->position.z;
        if (seen)
            continue;
        if (g_lights.count == kLights)
            break;
        const light& L = *c.L;
        const float lod = L.get_LOD();
        Fvector4* row = g_lights.rows + g_lights.count * kRows;
        row[0].set(L.position.x, L.position.y, L.position.z, 1.f / (L.range * L.range));
        const bool spot = L.flags.type == IRender_Light::SPOT;
        row[1].set(L.color.r * lod, L.color.g * lod, L.color.b * lod, spot ? _cos(L.cone * 0.5f) : -2.f);
        row[2].set(L.direction.x, L.direction.y, L.direction.z, 0.f);
        ++g_lights.count;
    }
}

void da_particle_light_register(CResourceManager& resources)
{
    resources.RegisterConstantSetup("da_particle_light", &binder_da_particle_light);
    resources.RegisterConstantSetup("da_particle_lights", &binder_da_particle_lights);
    resources.RegisterConstantSetup("da_particle_qa", &binder_da_particle_qa);
    resources.RegisterConstantSetup("da_particle_sun", &binder_da_particle_sun);
    resources.RegisterConstantSetup("da_particle_env", &binder_da_particle_env);
    resources.RegisterConstantSetup("da_particle_amb", &binder_da_particle_amb);
    resources.RegisterConstantSetup("da_froxel_grid", &binder_da_froxel_grid);
    resources.RegisterConstantSetup("da_froxel_range", &binder_da_froxel_range);
    resources.RegisterConstantSetup("da_froxel_ray", &binder_da_froxel_ray);
}
} // namespace xray::render::RENDER_NAMESPACE
