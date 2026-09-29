#include "stdafx.h"
#include "Layers/xrRender/DetailManager.h"
#include "Layers/xrRender/BufferUtils.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/da_wind_history.h"

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

CDetailManager::DumpInputs CDetailManager::hw_DumpInputs(const u32 dump) const
{
    using namespace detail_manager;

    const float scale = 1.f / float(quant);
    DumpInputs in;
    // The shadow passes take the same wind as the screen: a blade's shadow follows the blade. The
    // *_prev pair is the same wave one frame back, for the motion vectors of the G-buffer pass.
    switch (dump)
    {
    case 0: // Wave0
        in.consts.set(scale, scale, ps_r__Detail_l_aniso, ps_r__Detail_l_ambient);
        in.wave.set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_time_pos);
        in.wave_prev.set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_time_pos_prev);
        in.wind = m_wind_dir1;
        in.wind_prev = m_wind_dir1_prev;
        in.var_id = 1;
        in.lod_id = 0;
        break;
    case 1: // Wave1
        in.consts.set(scale, scale, ps_r__Detail_l_aniso, ps_r__Detail_l_ambient);
        in.wave.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos);
        in.wave_prev.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos_prev);
        in.wind = m_wind_dir2;
        in.wind_prev = m_wind_dir2_prev;
        in.var_id = 2;
        in.lod_id = 0;
        break;
    default: // Still (the wave of Wave1, which the still shader ignores)
        in.consts.set(scale, scale, scale, 1.f);
        in.wave.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos);
        in.wave_prev.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos_prev);
        in.wind = m_wind_dir2;
        in.wind_prev = m_wind_dir2_prev;
        in.var_id = 0;
        in.lod_id = 1;
        break;
    }
    in.wave.div(PI_MUL_2);
    in.wave_prev.div(PI_MUL_2);
    return in;
}

void CDetailManager::hw_Render(CBackend& cmd_list, const bool collectStats, const CFrustum* frustum, const u32 dumps)
{
    ZoneScoped;

    // Setup geometry and DMA
    cmd_list.set_Geometry(hw_Geom);

    for (u32 dump = 0; dump < dump_count; ++dump)
    {
        if (!(dumps & (1u << dump)))
            continue;
        const DumpInputs in = hw_DumpInputs(dump);
        hw_Render_dump(cmd_list, in.consts, in.wave, in.wind, in.wave_prev, in.wind_prev, in.var_id, in.lod_id,
            collectStats, frustum);
    }
}

void CDetailManager::hw_Render_dump(CBackend& cmd_list,
    const Fvector4& consts, const Fvector4& wave, const Fvector4& wind, const Fvector4& wave_prev,
    const Fvector4& wind_prev, u32 var_id, u32 lod_id, const bool collectStats, const CFrustum* frustum, const bool hybrid)
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
    static shared_str strWavePrev("wave_prev");
    static shared_str strDir2DPrev("dir2D_prev");
    static shared_str strWindPrevInfo("da_wm_info_prev");

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

    // Instances per draw. 61 comes from the DX9 vertex register file (256 registers, four
    // per instance, minus a header) and is what every detail shader's array declares. A
    // bigger array was tried: the constant buffer is re-uploaded WHOLE on every flush, so a
    // batch of 512 pays for 512 instances' worth of bytes to draw the 90 a slot group has.
    // 256 and 512 both measured slower than 61 on l01_escape.
    const u32 batch_limit = static_cast<u32>(hw_BatchSize);

    const R_constant_table* resolved_table = nullptr;
    R_constant *cConsts = nullptr, *cWave = nullptr, *cDir2D = nullptr, *cXForm = nullptr;
    R_constant *cSFade = nullptr, *cSFadeEye = nullptr, *cGrassTint = nullptr;
    R_constant *cWavePrev = nullptr, *cDir2DPrev = nullptr, *cWindPrevInfo = nullptr;

    vis_list& list = m_visibles[var_id];

    // Iterate
    for (const u8 objectId : m_visibleObjectIds[var_id])
    {
#if RENDER == R_R4
        if (hybrid && !ms_HasHybrid(var_id, objectId))
            continue;
#endif
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
            // Seven names resolved per object per pass per dump, for constants that
            // belong to the element's table and move only when the element does. Detail
            // objects share shaders, so one remembered table answers for most of them.
            if (cmd_list.get_ctable() != resolved_table)
            {
                resolved_table = cmd_list.get_ctable();
                cConsts = cmd_list.get_c(strConsts)._get();
                cWave = cmd_list.get_c(strWave)._get();
                cDir2D = cmd_list.get_c(strDir2D)._get();
                cXForm = cmd_list.get_c(strXForm)._get();
                cSFade = cmd_list.get_c(strSFade)._get();
                cSFadeEye = cmd_list.get_c(strSFadeEye)._get();
                cGrassTint = cmd_list.get_c(strGrassTint)._get();
                cWavePrev = cmd_list.get_c(strWavePrev)._get();
                cDir2DPrev = cmd_list.get_c(strDir2DPrev)._get();
                cWindPrevInfo = cmd_list.get_c(strWindPrevInfo)._get();
            }

            cmd_list.set_c(cConsts, consts);
            cmd_list.set_c(cWave, wave);
            cmd_list.set_c(cDir2D, wind);
            cmd_list.set_c(cXForm, cmd_list.xforms.m_wvp);
            cmd_list.set_c(cSFade, sfade);
            cmd_list.set_c(cSFadeEye, eye);
            cmd_list.set_c(cGrassTint, tint);
            cmd_list.set_c(cWavePrev, wave_prev);
            cmd_list.set_c(cDir2DPrev, wind_prev);
            // A shadow map has no motion vectors: its grass skips the second, previous-frame bend.
            // Set here rather than left to the binder, which runs only when the table changes.
            if (cWindPrevInfo)
            {
                const bool prev_bend = !shadow_pass && (da_motion_vectors_active() || ps_r__motion_vectors);
                cmd_list.set_c(cWindPrevInfo, da_wind_prev().wm_active, prev_bend ? 1.f : 0.f, 0.f, 0.f);
            }

            // ref_constant constArray = RCache.get_c(strArray);
            // VERIFY(constArray);

            // u32			c_base				= x_array->vs.index;
            // Fvector4*	c_storage			= RCache.get_ConstantCache_Vertex().get_array_f().access(c_base);
            //	Map constants to memory directly
            void* pVData = nullptr;
            cmd_list.get_ConstantDirect(strArray, batch_limit * sizeof(Fvector4) * 4, &pVData, 0, 0);
            // A detail shader whose instance array is smaller than the batch draws nothing
            // rather than writing through a null pointer, which is what release used to do.
            if (!pVData)
                continue;
            Fvector4* c_storage = (Fvector4*)pVData;

            u32 dwBatch = 0;

            for (const VisiblePart& part : vis)
            {
                // Distance first, frustum second. In a shadow pass most parts are past the
                // grass shadow radius, and this test is three subtractions against a frustum
                // test that walks up to six planes - so the cheap one gets to reject them.
                // Recomputed per frame on purpose: the slot-refresh distance in UpdateVisibleM
                // is amortised over 15-30 frames and would make the shadow radius lag in steps.
                if (shadow_pass && ep.distance_to_sqr(part.slot->vis.sphere.P) > grass_shadow_dist_sq)
                    continue;

                if (!IsPartVisible(part, frustum, cmd_list.context_id))
                    continue;
#if RENDER == R_R4
                if (hybrid && ms_Resident(objectId, part))
                    continue;
#endif

                // The upload bytes are already packed, in draw order, by the slot refresh
                // (UpdateVisibleM). What used to be a SlotItem pointer dereference and a
                // 64-byte gather per instance is now a straight run of memory, so the copy
                // goes out in batch-sized pieces and the prefetcher sees every one coming.
                const InstanceRows* src = part.rows->data();
                u32 left = static_cast<u32>(part.rows->size());
                while (left)
                {
                    const u32 take = _min(left, batch_limit - dwBatch);
                    std::memcpy(c_storage + dwBatch * 4, src, take * sizeof(InstanceRows));
                    src += take;
                    left -= take;
                    dwBatch += take;

                    if (dwBatch == batch_limit)
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
                            void* pNext = nullptr;
                            cmd_list.get_ConstantDirect(strArray, batch_limit * sizeof(Fvector4) * 4, &pNext, 0, 0);
                            c_storage = (Fvector4*)pNext;
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
