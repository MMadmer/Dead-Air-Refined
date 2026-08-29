#include "stdafx.h"
#include "dxRainRender.h"

// Rain sizes and density as knobs instead of 2007 constants - see xr_ioc_cmd.cpp.
// Declared before the namespace opens: inside it the extern would bind to render_r4::.
extern ENGINE_API float ps_r__rain_len;
extern ENGINE_API float ps_r__rain_width;
extern ENGINE_API int ps_r__rain_drops;
extern ENGINE_API float ps_r__rain_radius;
extern ENGINE_API float ps_r__rain_bright;
extern ENGINE_API float ps_r__rain_splash_bright;

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Rain.h"

namespace xray::render::RENDER_NAMESPACE
{
//	Warning: duplicated in rain.cpp
static const float source_offset = 40.f;
// Drops fall near-vertically now, so the kill plane needs far less headroom below.
static const float max_distance = source_offset * 1.25f;
static const float sink_offset = -(max_distance - source_offset);

const int max_particles = 1000;
const int particles_cache = 1500; // was 400 - splashes cut off in dense rain
const float particles_time = .3f;

dxRainRender::dxRainRender()
{
    IReader* F = FS.r_open("$game_meshes$", "dm" DELIMITER "rain.dm");
    VERIFY3(F, "Can't open file.", "dm" DELIMITER "rain.dm");

    DM_Drop = RImplementation.model_CreateDM(F);

    //
    SH_Rain.create("effects" DELIMITER "rain", "fx" DELIMITER "fx_rain");
    hGeom_Rain.create(FVF::F_LIT, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    hGeom_Drops.create(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, RImplementation.Vertex.Buffer(), RImplementation.Index.Buffer());

    FS.r_close(F);
}

dxRainRender::~dxRainRender() { RImplementation.model_Delete(DM_Drop); }
void dxRainRender::Copy(IRainRender& _in) { *this = *(dxRainRender*)&_in; }

void dxRainRender::Render(CEffect_Rain& owner)
{
    float factor = g_pGamePersistent->Environment().CurrentEnv.rain_density;
    if (factor < EPS_L)
        return;

    const u32 desired_items = iFloor(0.5f * (1.f + factor) * float(_max(ps_r__rain_drops, 1)));

    // born _new_ if needed
    if (owner.items.size() < desired_items)
    {
        owner.items.reserve(desired_items);
        while (owner.items.size() < desired_items)
        {
            CEffect_Rain::Item one;
            owner.Born(one, ps_r__rain_radius);
            owner.items.push_back(one);
        }
    }

    // visual
    const float factor_visual = factor;
    const float visual_length = ps_r__rain_len * factor_visual;
    const float visual_half_length = visual_length * .5f;
    // Drop colour times r__rain_bright: the weather configs paint drops dark grey-brown,
    // and a thin drop vanished against a storm sky. Real rain catches skylight and reads
    // LIGHTER than the background. Alpha is per-drop now - see the vertex loop.
    const Fvector3 f_rain_color = g_pGamePersistent->Environment().CurrentEnv.rain_color;
    const float rain_bright = ps_r__rain_bright;
    const float rain_r = _min(1.f, f_rain_color.x * rain_bright);
    const float rain_g = _min(1.f, f_rain_color.y * rain_bright);
    const float rain_b = _min(1.f, f_rain_color.z * rain_bright);

    // Ground splashes get their OWN brightness: sharing the drop colour turned them into
    // white grit on dark ground ("like hail"). A flying drop reads lighter than the
    // background; a splash lying on wet ground does not - it is in shade and soaked.
    const float splash_bright = ps_r__rain_splash_bright;
    const u32 u_splash_color = color_rgba_f(_min(1.f, f_rain_color.x * splash_bright),
        _min(1.f, f_rain_color.y * splash_bright), _min(1.f, f_rain_color.z * splash_bright), factor_visual);

    const float b_radius_wrap_sqr = _sqr((ps_r__rain_radius + .5f));

    const Fvector& vEye = Device.vCameraPosition;
    const float dt = Device.fTimeDelta;

    for (u32 I = 0; I < desired_items; I++)
    {
        CEffect_Rain::Item& one = owner.items[I];

        if (one.dwTime_Hit < Device.dwTimeGlobal)
            owner.Hit(one.Phit);
        if (one.dwTime_Life < Device.dwTimeGlobal)
            owner.Born(one, ps_r__rain_radius);

        one.P.mad(one.D, one.fSpeed * dt);
        // Membership in the SLANT-ALIGNED cylinder: test where the streak's LINE crosses eye
        // level, not where the drop is right now - the old vertical test kept the population
        // in an upright column whose sheared edge showed as a rain shaft once the wind
        // slanted the fall axis. Drops that leave the volume (the camera moved, the wind
        // turned) are simply reborn into the landing disc: Born costs the same ray as the
        // old edge-teleport machinery did.
        const float t_eye = (vEye.y - one.P.y) / std::min(one.D.y, -0.05f);
        Fvector wdir;
        wdir.set(one.P.x + one.D.x * t_eye - vEye.x, 0, one.P.z + one.D.z * t_eye - vEye.z);
        if (wdir.square_magnitude() > b_radius_wrap_sqr || (one.P.y - vEye.y) < sink_offset)
            one.invalidate();
    }

    u32 vOffset;
    FVF::LIT* verts = (FVF::LIT*)RImplementation.Vertex.Lock(desired_items * 4, hGeom_Rain->vb_stride, vOffset);
    FVF::LIT* start = verts;
    const float fade_start = ps_r__rain_radius * 0.45f;
    const float fade_len = std::max(ps_r__rain_radius * 0.75f, 1.f);
    for (u32 I = 0; I < desired_items; I++)
    {
        CEffect_Rain::Item& one = owner.items[I];

        // Build line
        Fvector& pos_head = one.P;
        Fvector pos_trail;
        pos_trail.mad(pos_head, one.D, -visual_length);

        // Culling
        Fvector sphere_center;
        sphere_center.mad(pos_head, one.D, -visual_half_length);
        if (!RImplementation.ViewBase.testSphere_dirty(sphere_center, visual_half_length))
            continue;

        static Fvector2 UV[2][4] = {{{0, 1}, {0, 0}, {1, 1}, {1, 0}}, {{1, 0}, {1, 1}, {0, 0}, {0, 1}}};

        // Everything OK - build vertices
        Fvector P, lineTop, camDir;
        camDir.sub(sphere_center, vEye);
        const float cam_dist = camDir.magnitude();
        camDir.div(std::max(cam_dist, 0.01f));
        lineTop.crossproduct(camDir, one.D);
        // |cross| = sin of the view/streak angle: the quad is left UNnormalized on purpose,
        // so a head-on streak thins to a sliver by itself. The same sine drives the alpha.
        const float sin_view = lineTop.magnitude();
        // Streak shaping instead of one flat alpha for the whole sheet (the big-game recipe:
        // camera-volume particles + per-drop fades):
        //  * distance dissolve - a far streak is sub-pixel in reality, and a full-alpha far
        //    wall was most of the old smear (floor 0.15 keeps a hint of the distant curtain);
        //  * head-on dimming - a streak seen along its own axis is a brief glint, not a rod
        //    (this is exactly the shaft-glow in the look-up screenshot);
        //  * per-drop brightness jitter - breaks the uniform curtain into individual drops;
        //  * the trail end fades like real motion blur, the head stays solid.
        float a = factor_visual;
        a *= 1.f - clampr((cam_dist - fade_start) / fade_len, 0.f, 0.85f);
        a *= 0.30f + 0.70f * sin_view;
        a *= 0.65f + 0.35f * float((I * 2654435761u) >> 29) * (1.f / 7.f);
        const u32 c_head = color_rgba_f(rain_r, rain_g, rain_b, _min(a, 1.f));
        const u32 c_tail = color_rgba_f(rain_r, rain_g, rain_b, _min(a, 1.f) * 0.35f);
        float w = ps_r__rain_width;
        u32 s = one.uv_set;
        P.mad(pos_trail, lineTop, -w);
        verts->set(P, c_tail, UV[s][0].x, UV[s][0].y);
        verts++;
        P.mad(pos_trail, lineTop, w);
        verts->set(P, c_tail, UV[s][1].x, UV[s][1].y);
        verts++;
        P.mad(pos_head, lineTop, -w);
        verts->set(P, c_head, UV[s][2].x, UV[s][2].y);
        verts++;
        P.mad(pos_head, lineTop, w);
        verts->set(P, c_head, UV[s][3].x, UV[s][3].y);
        verts++;
    }
    u32 vCount = (u32)(verts - start);
    RImplementation.Vertex.Unlock(vCount, hGeom_Rain->vb_stride);

    // Render if needed
    if (vCount)
    {
        // HW.pDevice->SetRenderState	(D3DRS_CULLMODE,D3DCULL_NONE);
        RCache.set_CullMode(CULL_NONE);
        RCache.set_xform_world(Fidentity);
        RCache.set_Shader(SH_Rain);
        RCache.set_Geometry(hGeom_Rain);
        RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, vCount, 0, vCount / 2);
        // HW.pDevice->SetRenderState	(D3DRS_CULLMODE,D3DCULL_CCW);
        RCache.set_CullMode(CULL_CCW);
    }

    // Particles
    CEffect_Rain::Particle* P = owner.particle_active;
    if (!P)
    {
        return;
    }

    {
        float dt = Device.fTimeDelta;
        _IndexStream& _IS = RImplementation.Index;
        RCache.set_Shader(DM_Drop->shader);

        Fmatrix mXform, mScale;
        int pcount = 0;
        u32 v_offset, i_offset;
        u32 vCount_Lock = particles_cache * DM_Drop->number_vertices;
        u32 iCount_Lock = particles_cache * DM_Drop->number_indices;
        IRender_DetailModel::fvfVertexOut* v_ptr =
            (IRender_DetailModel::fvfVertexOut*)RImplementation.Vertex.Lock(vCount_Lock, hGeom_Drops->vb_stride, v_offset);
        u16* i_ptr = _IS.Lock(iCount_Lock, i_offset);
        while (P)
        {
            CEffect_Rain::Particle* next = P->next;

            // Update
            // P can be zero sometimes and it crashes
            P->time -= dt;
            if (P->time < 0)
            {
                owner.p_free(P);
                P = next;
                continue;
            }

            // Render
            if (RImplementation.ViewBase.testSphere_dirty(P->bounds.P, P->bounds.R))
            {
                // Build matrix
                float scale = P->time / particles_time;
                mScale.scale(scale, scale, scale);
                mXform.mul_43(P->mXForm, mScale);

                // XForm verts
                DM_Drop->transfer(mXform, v_ptr, u_splash_color, i_ptr, pcount * DM_Drop->number_vertices);
                v_ptr += DM_Drop->number_vertices;
                i_ptr += DM_Drop->number_indices;
                pcount++;

                if (pcount >= particles_cache)
                {
                    // flush
                    u32 dwNumPrimitives = iCount_Lock / 3;
                    RImplementation.Vertex.Unlock(vCount_Lock, hGeom_Drops->vb_stride);
                    _IS.Unlock(iCount_Lock);
                    RCache.set_Geometry(hGeom_Drops);
                    RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, vCount_Lock, i_offset, dwNumPrimitives);

                    v_ptr = (IRender_DetailModel::fvfVertexOut*)RImplementation.Vertex.Lock(
                        vCount_Lock, hGeom_Drops->vb_stride, v_offset);
                    i_ptr = _IS.Lock(iCount_Lock, i_offset);

                    pcount = 0;
                }
            }

            P = next;
        }

        // Flush if needed
        vCount_Lock = pcount * DM_Drop->number_vertices;
        iCount_Lock = pcount * DM_Drop->number_indices;
        u32 dwNumPrimitives = iCount_Lock / 3;
        RImplementation.Vertex.Unlock(vCount_Lock, hGeom_Drops->vb_stride);
        _IS.Unlock(iCount_Lock);
        if (pcount)
        {
            RCache.set_Geometry(hGeom_Drops);
            RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, vCount_Lock, i_offset, dwNumPrimitives);
        }
    }

}

const Fsphere& dxRainRender::GetDropBounds() const { return DM_Drop->bv_sphere; }
} // namespace xray::render::RENDER_NAMESPACE
