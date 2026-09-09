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
// Rain ladder: 1 base, 2 oriented splashes, 3 +streak lighting, 4 full.
extern ENGINE_API int ps_r__rain_quality;

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrEngine/Rain.h"

namespace xray::render::RENDER_NAMESPACE
{
// Splash batch size - was 400, and splashes cut off in dense rain.
const int particles_cache = 1500;

dxRainRender::dxRainRender()
{
    IReader* F = FS.r_open("$game_meshes$", "dm" DELIMITER "rain.dm");
    VERIFY3(F, "Can't open file.", "dm" DELIMITER "rain.dm");

    DM_Drop = RImplementation.model_CreateDM(F);

    //
    SH_Rain.create("effects" DELIMITER "rain", "fx" DELIMITER "fx_rain");
    // The crown's own blender, when the loose override ships it. Guarded rather than assumed:
    // a missing shader script is fatal at create time, and the drops must not take the game
    // down with them if the compatibility gamedata is out of step with the binary.
    if (RImplementation.Resources->_lua_HasShader("effects" DELIMITER "rain_splash"))
        SH_Splash.create("effects" DELIMITER "rain_splash", "fx" DELIMITER "fx_rainsplash1");

    hGeom_Rain.create(FVF::F_LIT, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    hGeom_Drops.create(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, RImplementation.Vertex.Buffer(), RImplementation.Index.Buffer());

    FS.r_close(F);
}

dxRainRender::~dxRainRender() { RImplementation.model_Delete(DM_Drop); }
void dxRainRender::Copy(IRainRender& _in) { *this = *(dxRainRender*)&_in; }

void dxRainRender::Render(CEffect_Rain& owner)
{
    CEnvironment& env = g_pGamePersistent->Environment();
    const float factor = env.CurrentEnv.rain_density;
    const float dt = Device.fTimeDelta;
    const Fvector& vEye = Device.vCameraPosition;
    const int tier = ps_r__rain_quality;

    // The rain RATE is what everything is sized on now. rain_density stays exactly what it
    // always was - the authored 0..1 keyframe, and a public Lua signal half the mod reads -
    // but "how much rain" is a rate in mm/h, and drop count, streak alpha and splash
    // brightness all answer to that instead of to a number with no physical meaning.
    const float rate01 = clampr(powf(env.RainRateMmh() * (1.f / 25.f), 0.7f), 0.f, 1.f);

    // The sheet stops when the weather does; the splash pool must NOT. That early-out used to
    // sit above the particle loop, which is the only place a crown ages and is freed, so every
    // splash alive at the moment the rain stopped was stranded in the active list until the
    // level changed. A few rain cycles bled the four thousand slots dry.
    const bool raining = factor >= EPS_L;

    if (raining)
    {
        // Drop count on the preset ladder - the one rain knob that was never on one, and the
        // most expensive there is: every birth is a level raycast. The authored count is what
        // High draws; Minimum and Low run a thinner sheet, Extreme a slightly denser one.
        static const float tier_drops[4] = {0.45f, 0.70f, 1.00f, 1.15f};
        const float tier_k = tier_drops[clampr(tier, 1, 4) - 1];
        const u32 desired_items =
            iFloor(float(_max(ps_r__rain_drops, 1)) * (0.20f + 0.80f * rate01) * tier_k);

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
        // ... and shrink when the weather eases. Only the grow path existed, so a storm left
        // six thousand Items resident through the drizzle that followed, with the tail past
        // desired_items holding frozen positions and expired timestamps for good.
        else if (owner.items.size() > desired_items)
            owner.items.resize(desired_items);

        // visual
        // Drop colour times r__rain_bright: the weather configs paint drops dark grey-brown,
        // and a thin drop vanished against a storm sky. Real rain catches skylight and reads
        // LIGHTER than the background. Alpha is per-drop - see the vertex loop.
        const Fvector3 f_rain_color = env.CurrentEnv.rain_color;
        const float rain_bright = ps_r__rain_bright;
        const float rain_r = _min(1.f, f_rain_color.x * rain_bright);
        const float rain_g = _min(1.f, f_rain_color.y * rain_bright);
        const float rain_b = _min(1.f, f_rain_color.z * rain_bright);
        // A drop is about as visible in a drizzle as in a storm - what changes is how MANY
        // there are. The stock sheet was drawn at the density's own alpha, so light rain read
        // as a grey wash rather than as separate drops.
        const float sheet_alpha = 0.55f + 0.45f * rate01;

        const float b_radius_wrap_sqr = _sqr((ps_r__rain_radius + .5f));

        // Wind steers a drop in FLIGHT, not only at birth. Sampled once at the camera and
        // shared by the whole sheet: the gust field varies over tens of metres while the sheet
        // is thirty across, so a per-drop sample would buy nothing for six thousand noise
        // evaluations a frame. What a drop gets of its own is a fixed jitter keyed on its
        // index, so the sheet shimmers under a gust instead of sliding as one rigid slab.
        const Fvector wind_v = env.WindAt(vEye, 6.f);
        const float wind_a = 1.f - expf(-dt / da_rain::wind_tau);
        const float slant_sin = _sin(da_rain::max_slant);

        for (u32 I = 0; I < desired_items; I++)
        {
            CEffect_Rain::Item& one = owner.items[I];

            if (one.dwTime_Hit < Device.dwTimeGlobal)
                owner.HitItem(one);
            if (one.dwTime_Life < Device.dwTimeGlobal)
                owner.Born(one, ps_r__rain_radius);
            // Parked under a roof: no motion, no membership test, and below it no quad.
            if (one.sheltered)
                continue;

            // The wind's pull, expressed in the drop's own units: the slant is
            // atan(wind / terminal), so at the speed the streak is DRAWN at the matching
            // horizontal velocity is that ratio times fSpeed. Relaxed toward rather than
            // snapped to, over the drop's response time.
            Fvector vel;
            vel.mul(one.D, one.fSpeed);
            const float jitter = 0.75f + 0.5f * float((I * 2654435761u) >> 29) * (1.f / 7.f);
            const float k = one.fSpeed * jitter / da_rain::fall_ms;
            float tx = wind_v.x * k, tz = wind_v.z * k;
            // Same readability cap the birth slant has, or a squall lays the sheet flat.
            const float hmax = one.fSpeed * slant_sin;
            const float h = _sqrt(tx * tx + tz * tz);
            if (h > hmax)
            {
                const float s = hmax / h;
                tx *= s;
                tz *= s;
            }
            vel.x += (tx - vel.x) * wind_a;
            vel.z += (tz - vel.z) * wind_a;
            one.D.set(vel);
            one.D.normalize_safe(Fvector().set(0.f, -1.f, 0.f));

            one.P.mad(one.D, one.fSpeed * dt);
            // Membership in the SLANT-ALIGNED cylinder: test where the streak's LINE crosses
            // eye level, not where the drop is right now - the old vertical test kept the
            // population in an upright column whose sheared edge showed as a rain shaft once
            // the wind slanted the fall axis. Drops that leave the volume (the camera moved,
            // the wind turned) are simply reborn into the landing disc: Born costs the same
            // ray as the old edge-teleport machinery did.
            const float t_eye = (vEye.y - one.P.y) / std::min(one.D.y, -0.05f);
            Fvector wdir;
            wdir.set(one.P.x + one.D.x * t_eye - vEye.x, 0, one.P.z + one.D.z * t_eye - vEye.z);
            if (wdir.square_magnitude() > b_radius_wrap_sqr || (one.P.y - vEye.y) < da_rain::sink_offset)
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

            // A drop the membership test just killed, and one parked under a roof, are both
            // gone. The stock loop drew them anyway - the invalidated one for a whole extra
            // frame at its stale position, since rebirth only happens on the next pass.
            if (one.sheltered || 0 == one.dwTime_Life)
                continue;

            // Build line. Streak length is exposure times SPEED, not intensity: the stock
            // `len * density` drew twenty-centimetre stubs in a drizzle and only reached the
            // authored two metres in a full storm, which is backwards - motion blur does not
            // know how hard it is raining. Per drop, so a fast one draws a longer streak.
            const float visual_length = ps_r__rain_len * (one.fSpeed / da_rain::speed_mean);
            const float visual_half_length = visual_length * .5f;
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
            float a = sheet_alpha;
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
            RCache.set_CullMode(CULL_NONE);
            RCache.set_xform_world(Fidentity);
            RCache.set_Shader(SH_Rain);
            RCache.set_Geometry(hGeom_Rain);
            RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, vCount, 0, vCount / 2);
            RCache.set_CullMode(CULL_CCW);
        }
    }

    // Particles
    CEffect_Rain::Particle* P = owner.particle_active;
    if (!P)
        return;

    {
        // Ground splashes get their OWN brightness: sharing the drop colour turned them into
        // white grit on dark ground ("like hail"). A flying drop reads lighter than the
        // background; a splash lying on wet ground does not - it is in shade and soaked.
        const Fvector3 f_rain_color = env.CurrentEnv.rain_color;
        const float splash_bright = ps_r__rain_splash_bright;
        const float splash_r = _min(1.f, f_rain_color.x * splash_bright);
        const float splash_g = _min(1.f, f_rain_color.y * splash_bright);
        const float splash_b = _min(1.f, f_rain_color.z * splash_bright);
        // Held above zero when the rain has stopped, so the last crowns dissolve instead of
        // snapping off with the sheet.
        const float splash_alpha = 0.5f + 0.5f * rate01;

        _IndexStream& _IS = RImplementation.Index;
        // transfer() writes world-space vertices, and the sheet's own draw is what used to
        // leave the identity world transform behind. It no longer always runs - the crowns
        // outlive the rain now - so this block sets its own.
        RCache.set_xform_world(Fidentity);
        ref_shader& splash_shader = SH_Splash ? SH_Splash : DM_Drop->shader;
        RCache.set_Shader(splash_shader);

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
                // Build matrix.
                //
                // A real crown throws up in the first few milliseconds and is gone inside a
                // tenth of a second: the sheet of water thrown out of the impact spreads as
                // sqrt(t), and what is left collapses back. The stock crown was born at FULL
                // size, held it for three tenths of a second and then vanished - which is a
                // decal with a timer, and is why splashes never read as water.
                const float age = clampr(1.f - P->time / std::max(P->life, 0.001f), 0.f, 1.f);
                float scale = P->size * (0.25f + 0.75f * _sqrt(age));
                // The splash model is 18 x 22 cm and lands anywhere in the disc, the spot under
                // the player's own boot included: seen from half a metre it read as a soft
                // light blob by the foot. A crown that close is a few centimetres.
                const float eye_dist = P->bounds.P.distance_to(Device.vCameraPosition);
                scale *= clampr((eye_dist - 0.6f) / 2.4f, 0.f, 1.f);
                mScale.scale(scale, scale, scale);
                mXform.mul_43(P->mXForm, mScale);

                // Alpha is per crown now, not one constant for the whole sheet: it holds while
                // the crown climbs and goes as it falls back.
                const u32 c_splash =
                    color_rgba_f(splash_r, splash_g, splash_b, splash_alpha * (1.f - age * age));

                // XForm verts
                DM_Drop->transfer(mXform, v_ptr, c_splash, i_ptr, pcount * DM_Drop->number_vertices);
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
