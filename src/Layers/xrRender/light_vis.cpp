#include "stdafx.h"
#include "Layers/xrRender/light.h"
#include "Layers/xrRender/r__occlusion.h"
#include "xrCDB/Intersect.hpp"

namespace xray::render::RENDER_NAMESPACE
{
const u32 delay_small_min = 1;
const u32 delay_small_max = 3;
const u32 delay_large_min = 10;
const u32 delay_large_max = 20;
const u32 cullfragments = 4;

void light::vis_prepare(CBackend& cmd_list)
{
    if (int(indirect_photons) != ps_r2_GI_photons)
        gi_generate();

    //	. test is sheduled for future	= keep old result
    //	. test time comes :)
    //		. camera inside light volume	= visible,	shedule for 'small' interval
    //		. perform testing				= ???,		pending

    u32 frame = Device.dwFrame;
    if (frame < vis.frame2test)
        return;

    float safe_area = VIEWPORT_NEAR;
    {
        float a0 = deg2rad(Device.fFOV * Device.fASPECT / 2.f);
        float a1 = deg2rad(Device.fFOV / 2.f);
        float x0 = VIEWPORT_NEAR / _cos(a0);
        float x1 = VIEWPORT_NEAR / _cos(a1);
        float c = _sqrt(x0 * x0 + x1 * x1);
        safe_area = _max(_max(VIEWPORT_NEAR, _max(x0, x1)), c);
    }

    // Msg	("sc[%f,%f,%f]/c[%f,%f,%f] - sr[%f]/r[%f]",VPUSH(spatial.center),VPUSH(position),spatial.radius,range);
    // Msg	("dist:%f, sa:%f",Device.vCameraPosition.distance_to(spatial.center),safe_area);
    bool skiptest = false;
    if (ps_r2_ls_flags.test(R2FLAG_EXP_DONT_TEST_UNSHADOWED) && !flags.bShadow)
        skiptest = true;
    if (ps_r2_ls_flags.test(R2FLAG_EXP_DONT_TEST_SHADOWED) && flags.bShadow)
        skiptest = true;

    // OMNIPART faces are slices of one point-light volume and carry no bounds of their own,
    // so both edge and inside checks measure the parent sphere.
    Fvector queryCenter = spatial.sphere.P;
    float queryRadius = spatial.sphere.R;
    if (flags.type == IRender_Light::OMNIPART)
    {
        queryCenter = position;
        queryRadius = range;
    }

    // A volume clipped by the screen edge yields unreliable zeroes (the sampling misses all
    // pixels while the light still reaches the frame), so treat it as visible and skip the query.
    u32 planeMask = 0xffffffff;
    const EFC_Visible viewVisibility = RImplementation.ViewBase.testSphere(queryCenter, queryRadius, planeMask);
    const bool screenClippedVolume = viewVisibility == fcvPartial;

    const bool cameraInsideVolume =
        Device.vCameraPosition.distance_to(queryCenter) <= (queryRadius * 1.01f + safe_area);
    if (skiptest || screenClippedVolume || cameraInsideVolume)
    { // small error
        if (vis.pending)
            RImplementation.occq_cancel(vis.query_id);
        vis.visible = true;
        vis.pending = false;
        vis.frame2test = frame + ::Random.randI(delay_small_min, delay_small_max);
        return;
    }

    // A query stays owned by this light until its GPU result becomes available.
    if (vis.pending)
        return;

    // testing
    vis.pending = true;
    vis.query_frame = frame;
    vis.query_camera_position.set(Device.vCameraPosition);
    vis.query_camera_direction.set(Device.vCameraDirection);
    xform_calc();
    cmd_list.set_xform_world(m_xform);
    vis.query_order = RImplementation.occq_begin(vis.query_id);
    //	Hack: Igor. Light is visible if it's frutum is visible. (Only for volumetric)
    //	Hope it won't slow down too much since there's not too much volumetric lights
    //	TODO: sort for performance improvement if this technique hurts
    if ((flags.type == IRender_Light::SPOT) && flags.bShadow && flags.bVolumetric)
        cmd_list.set_Stencil(FALSE);
    else
        cmd_list.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00);
    RImplementation.Target->draw_volume(cmd_list, this);
    RImplementation.occq_end(vis.query_id);
}

void light::vis_update()
{
    //	. not pending	->>> return (early out)
    //	. test-result:	visible:
    //		. shedule for 'large' interval
    //	. test-result:	invisible:
    //		. shedule for 'next-frame' interval

    if (!vis.pending)
        return;

    const u32 frame = Device.dwFrame;
    R_occlusion::occq_result fragments{};
    if (!RImplementation.occq_try_get(vis.query_id, fragments))
        return;
    const bool recentView = (frame - vis.query_frame) <= 2 &&
        Device.vCameraPosition.similar(vis.query_camera_position, 0.1f) &&
        Device.vCameraDirection.dotproduct(vis.query_camera_direction) >= _cos(deg2rad(1.f));

    // A delayed zero is only valid for the view that issued the query.
    const bool seen = (fragments > cullfragments) || !recentView;
    vis.pending = false;

    if (seen)
    {
        vis.miss_streak = 0;
        vis.visible = true;
        vis.frame2test = frame + ::Random.randI(delay_large_min, delay_large_max);
        return;
    }

    if (vis.miss_streak < 255)
        ++vis.miss_streak;

    // Hysteresis: a light dies only after several consecutive misses, because a single
    // near-threshold zero (a few fragments through a doorway) is jitter, not disappearance.
    if (vis.miss_streak < 3)
    {
        vis.frame2test = frame + 1;
        return;
    }

    vis.visible = false;
    vis.frame2test = frame + 1;
}
} // namespace xray::render::RENDER_NAMESPACE
