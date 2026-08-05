#include "stdafx.h"
#include "Light_Render_Direct.h"

namespace xray::render::RENDER_NAMESPACE
{
void CLight_Compute_XFORM_and_VIS::compute_xf_spot(light* L)
{
    ZoneScoped;

    // Build EYE-space xform
    Fvector L_dir, L_up, L_right, L_pos;
    L_dir.set(L->direction);
    L_dir.normalize();

    if (L->right.square_magnitude() > EPS)
    {
        // use specified 'up' and 'right', just enshure ortho-normalization
        L_right.set(L->right);
        L_right.normalize();
        L_up.crossproduct(L_dir, L_right);
        L_up.normalize();
        L_right.crossproduct(L_up, L_dir);
        L_right.normalize();
    }
    else
    {
        // auto find 'up' and 'right' vectors
        L_up.set(0, 1, 0);
        if (_abs(L_up.dotproduct(L_dir)) > .99f)
            L_up.set(0, 0, 1);
        L_right.crossproduct(L_up, L_dir);
        L_right.normalize();
        L_up.crossproduct(L_dir, L_right);
        L_up.normalize();
    }
    L_pos.set(L->position);

    //
    L->X.S.posX = L->X.S.posY = 0;
    L->X.S.transluent = FALSE;
    if (L->flags.bShadow)
    {
        const int cachedSize = L->X.S.size;

        // Compute approximate screen area (treating it as an point light) - R*R/dist_sq
        // Note: we clamp screen space area to ONE, although it is not correct at all
        float dist = Device.vCameraPosition.distance_to(L->spatial.sphere.P) - L->spatial.sphere.R;
        if (dist < 0)
            dist = 0;
        const float ssa = clampr(L->range * L->range / (1.f + dist * dist), 0.f, 1.f);

        const float intensity0 = (L->color.r + L->color.g + L->color.b) / 3.f;
        const float intensity1 = L->color.r * 0.2125f + L->color.g * 0.7154f + L->color.b * 0.0721f;
        const float intensity = (intensity0 + intensity1) / 2.f;
        const float duelDot = 1.f - 0.5f * Device.vCameraDirection.dotproduct(L_dir);
        const float sizeFactor = L->range / 8.f;
        const float wideFactor = L->cone / deg2rad(90.f);
        const float factor = ps_r2_ls_squality * powf(ssa, .5f) * powf(intensity, 1.f / 16.f) *
            powf(duelDot, .25f) * powf(sizeFactor, .25f) * powf(wideFactor, .5f);

        const u32 size =
            std::clamp(u32(iFloor(factor * SMAP_adapt_optimal)), SMAP_adapt_min, SMAP_adapt_max);
        const int epsilon = iCeil(float(size) * .01f);
        L->X.S.size = _abs(int(size) - cachedSize) >= epsilon ? size : cachedSize;
    }
    else
        L->X.S.size = SMAP_adapt_max;

    // make N pixel border
    L->X.S.view.build_camera_dir(L_pos, L_dir, L_up);
    // float	n			= 2.f						;
    // float	x			= float(L->X.S.size)		;
    // float	alpha		= L->cone/2					;
    // float	tan_beta	= (x+2*n)*tanf(alpha) / x	;
    // float	g_alpha		= 2*rad2deg		(alpha);
    // float	g_beta		= 2*rad2deg		(atanf(tan_beta));
    // Msg				("x(%f) : a(%f), b(%f)",x,g_alpha,g_beta);

    // Match the original Dead Air projection: filtered cubemap faces need a stable overlap and near plane.
    const float shadowCone = std::min(L->cone + deg2rad(5.f), PI * 0.98f);
    L->X.S.project.build_projection(shadowCone, 1.f, SMAP_near_plane, L->range + EPS_S);
    L->X.S.combine.mul(L->X.S.project, L->X.S.view);
}
} // namespace xray::render::RENDER_NAMESPACE
