#include "stdafx.h"
#include "Layers/xrRender/DetailManager.h"
#include "Layers/xrRender/BufferUtils.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace detail_manager
{
extern const int quant;
//extern const int c_hdr;
}

void CDetailManager::hw_Load_Shaders()
{
    // Create shader to access constant storage
    ref_shader S;
    S.create("details\\set");
    R_constant_table& T0 = *(S->E[0]->passes[0]->constants);
    R_constant_table& T1 = *(S->E[1]->passes[0]->constants);
    hwc_consts = T0.get("consts");
    hwc_wave = T0.get("wave");
    hwc_wind = T0.get("dir2D");
    hwc_array = T0.get("array");
    hwc_s_consts = T1.get("consts");
    hwc_s_xform = T1.get("xform");
    hwc_s_array = T1.get("array");
}

void CDetailManager::hw_Render(CBackend& cmd_list, const bool collectStats, const CFrustum* frustum)
{
    ZoneScoped;
    using namespace detail_manager;

    // Setup geometry and DMA
    cmd_list.set_Geometry(hw_Geom);

    // r__wind_shadow 0: grass stands still in the SHADOW passes while swaying on screen.
    // A shadow map is a hard edge on a texel boundary; a blade moving by a fraction of a
    // texel flips whole shaded pixels between lit and unlit every frame, which narrow
    // specular lobes turn into colour noise on metal. Every frustum-culled detail pass
    // today is a shadow pass (sun cascades, lamp smaps) - the main pass passes none.
    const bool shadow_pass = frustum != nullptr;
    Fvector4 wind_zero;
    wind_zero.set(0.f, 0.f, 0.f, 0.f);
    const bool freeze_wind = shadow_pass && ps_r__wind_shadow == 0;
    const Fvector4& dir1 = freeze_wind ? wind_zero : m_wind_dir1;
    const Fvector4& dir2 = freeze_wind ? wind_zero : m_wind_dir2;

    // Wave0
    float scale = 1.f / float(quant);
    Fvector4 wave;
    Fvector4 consts;
    consts.set(scale, scale, ps_r__Detail_l_aniso, ps_r__Detail_l_ambient);
    // wave.set				(1.f/5.f,		1.f/7.f,	1.f/3.f,	Device.fTimeGlobal*swing_current.speed);
    wave.set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_time_pos);
    // RCache.set_c			(&*hwc_consts,	scale,		scale,		ps_r__Detail_l_aniso,	ps_r__Detail_l_ambient);
    // //
    // consts
    // RCache.set_c			(&*hwc_wave,	wave.div(PI_MUL_2));	// wave
    // RCache.set_c			(&*hwc_wind,	dir1); //
    // wind-dir
    // hw_Render_dump			(&*hwc_array,	1, 0, c_hdr );
    hw_Render_dump(cmd_list, consts, wave.div(PI_MUL_2), dir1, 1, 0, collectStats, frustum);

    // Wave1
    // wave.set				(1.f/3.f,		1.f/7.f,	1.f/5.f,	Device.fTimeGlobal*swing_current.speed);
    wave.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos);
    // RCache.set_c			(&*hwc_wave,	wave.div(PI_MUL_2));	// wave
    // RCache.set_c			(&*hwc_wind,	dir2); //
    // wind-dir
    // hw_Render_dump			(&*hwc_array,	2, 0, c_hdr );
    hw_Render_dump(cmd_list, consts, wave.div(PI_MUL_2), dir2, 2, 0, collectStats, frustum);

    // Still
    consts.set(scale, scale, scale, 1.f);
    // RCache.set_c			(&*hwc_s_consts,scale,		scale,		scale,				1.f);
    // RCache.set_c			(&*hwc_s_xform,	Device.mFullTransform);
    // hw_Render_dump			(&*hwc_s_array,	0, 1, c_hdr );
    hw_Render_dump(cmd_list, consts, wave.div(PI_MUL_2), m_wind_dir2, 0, 1, collectStats, frustum);
}

void CDetailManager::hw_Render_dump(CBackend& cmd_list,
    const Fvector4& consts, const Fvector4& wave, const Fvector4& wind, u32 var_id, u32 lod_id,
    const bool collectStats, const CFrustum* frustum)
{
    ZoneScoped;

    static shared_str strConsts("consts");
    static shared_str strWave("wave");
    static shared_str strDir2D("dir2D");
    static shared_str strArray("array");
    static shared_str strXForm("xform");
    static shared_str strSFade("grass_sfade");
    static shared_str strSFadeEye("grass_sfade_eye");
    static shared_str strGrassTint("grass_tint");

    // Every frustum-culled detail pass today is a shadow pass (see hw_Render).
    const bool shadow_pass = frustum != nullptr;

    // Grass shadow fade band (start, end) in metres from the camera. STRICTLY zero in the
    // normal pass: the shader reads zero as "nothing to fade" and behaves exactly as before.
    // No pass define exists for this on purpose - one would double the grass shader cache.
    Fvector4 sfade;
    if (shadow_pass && ps_r__grass_shadow_fade > 0)
    {
        const float e = float(ps_r__grass_shadow_dist);
        sfade.set(_max(e - float(ps_r__grass_shadow_fade), 0.f), e, 0.f, 0.f);
    }
    else
        sfade.set(0.f, 0.f, 0.f, 0.f);

    // The camera position is handed over SEPARATELY: in the shadow pass m_WV belongs to the
    // SUN, so a view-space distance in the shader would measure the wrong thing. This is the
    // same distance the CPU cull below uses, so the fade band lines up with the cut.
    const Fvector& ep = Device.vCameraPosition;
    Fvector4 eye;
    eye.set(ep.x, ep.y, ep.z, 0.f);

    // World-position brightness variation: strength, 1/patch size, base boost.
    Fvector4 tint;
    tint.set(ps_r__grass_tint, 1.f / _max(ps_r__grass_tint_scale, 0.1f), ps_r__grass_tint_base, 0.f);

    // Grass beyond this radius skips the shadow maps entirely - stock fed ALL visible grass
    // (hundreds of metres) into the 20 m near cascade and let the GPU discard it after
    // vertex work. Compared per part against the slot bounds below.
    const float grass_shadow_dist_sq =
        float(ps_r__grass_shadow_dist) * float(ps_r__grass_shadow_dist);

    vis_list& list = m_visibles[var_id];

    // Iterate
    for (const u8 objectId : m_visibleObjectIds[var_id])
    {
        CDetail& Object = *objects[objectId];
        VisiblePartVec& vis = list[objectId];
        const u32 vOffset = m_objectVertexOffsets[objectId];
        const u32 iOffset = m_objectIndexOffsets[objectId];
        for (u32 iPass = 0; iPass < Object.shader->E[lod_id]->passes.size(); ++iPass)
        {
            // Setup matrices + colors (and flush it as necessary)
            // RCache.set_Element				(Object.shader->E[lod_id]);
            cmd_list.set_Element(Object.shader->E[lod_id], iPass);
            cmd_list.apply_lmaterial();

            //	This could be cached in the corresponding consatant buffer
            //	as it is done for DX9
            cmd_list.set_c(strConsts, consts);
            cmd_list.set_c(strWave, wave);
            cmd_list.set_c(strDir2D, wind);
            cmd_list.set_c(strXForm, cmd_list.xforms.m_wvp);
            cmd_list.set_c(strSFade, sfade);
            cmd_list.set_c(strSFadeEye, eye);
            cmd_list.set_c(strGrassTint, tint);

            // ref_constant constArray = RCache.get_c(strArray);
            // VERIFY(constArray);

            // u32			c_base				= x_array->vs.index;
            // Fvector4*	c_storage			= RCache.get_ConstantCache_Vertex().get_array_f().access(c_base);
            Fvector4* c_storage = 0;
            //	Map constants to memory directly
            {
                void* pVData;
                cmd_list.get_ConstantDirect(strArray, hw_BatchSize * sizeof(Fvector4) * 4, &pVData, 0, 0);
                c_storage = (Fvector4*)pVData;
            }
            VERIFY(c_storage);

            u32 dwBatch = 0;

            for (const VisiblePart& part : vis)
            {
                if (!IsPartVisible(part, frustum))
                    continue;

                // Recomputed per frame on purpose: the slot-refresh distance in UpdateVisibleM
                // is amortised over 15-30 frames and would make the shadow radius lag in steps.
                if (shadow_pass && ep.distance_to_sqr(part.bounds->sphere.P) > grass_shadow_dist_sq)
                    continue;

                for (SlotItem* item : *part.items)
                {
                    SlotItem& Instance = *item;
                    u32 base = dwBatch * 4;

                    // The instance never moves: mRotY/c_hemi/c_sun are set once at slot
                    // decompression, scale/height once per 15-30 frames per slot. Rebuilding
                    // 12 multiplies per instance per frame for ~47k instances was pure waste.
                    if (!Instance.cache_valid)
                    {
                        // Build matrix ( 3x4 matrix, last row - color ). Height is scaled
                        // SEPARATELY: the local-height contribution is the second element of
                        // each row, so scaling just those lays the blade flat without
                        // touching its ground footprint (r__grass_fade_flat).
                        const float scale = Instance.scale_calculated;
                        const float hs = scale * Instance.height_calculated;
                        Fmatrix& M = Instance.mRotY;
                        Instance.cached_out[0].set(M._11 * scale, M._21 * hs, M._31 * scale, M._41);
                        Instance.cached_out[1].set(M._12 * scale, M._22 * hs, M._32 * scale, M._42);
                        Instance.cached_out[2].set(M._13 * scale, M._23 * hs, M._33 * scale, M._43);

                        // Build color (R2 only needs hemisphere)
                        const float h = Instance.c_hemi;
                        const float s = Instance.c_sun;
                        Instance.cached_out[3].set(s, s, s, h);
                        Instance.cache_valid = true;
                    }
                    c_storage[base + 0] = Instance.cached_out[0];
                    c_storage[base + 1] = Instance.cached_out[1];
                    c_storage[base + 2] = Instance.cached_out[2];
                    c_storage[base + 3] = Instance.cached_out[3];
                    dwBatch++;
                    if (dwBatch == hw_BatchSize)
                    {
                        // flush
                        if (collectStats)
                            RImplementation.BasicStats.DetailCount += dwBatch;
                        // One stored copy drawn dwBatch times instead of dwBatch baked
                        // copies: the shader takes the matrix index from SV_InstanceID.
                        u32 dwCNT_verts = dwBatch * Object.number_vertices;
                        cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, vOffset, 0,
                            Object.number_vertices, iOffset, Object.number_indices / 3, dwBatch);
                        cmd_list.stat.r.s_details.add(dwCNT_verts);

                        // restart
                        dwBatch = 0;

                        //	Remap constants to memory directly (just in case anything goes wrong)
                        {
                            void* pVData;
                            cmd_list.get_ConstantDirect(strArray, hw_BatchSize * sizeof(Fvector4) * 4, &pVData, 0, 0);
                            c_storage = (Fvector4*)pVData;
                        }
                        VERIFY(c_storage);
                    }
                }
            }
            // flush if necessary
            if (dwBatch)
            {
                if (collectStats)
                    RImplementation.BasicStats.DetailCount += dwBatch;
                u32 dwCNT_verts = dwBatch * Object.number_vertices;
                cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, vOffset, 0,
                    Object.number_vertices, iOffset, Object.number_indices / 3, dwBatch);
                cmd_list.stat.r.s_details.add(dwCNT_verts);
            }
        }
    }
}
} // namespace xray::render::RENDER_NAMESPACE
