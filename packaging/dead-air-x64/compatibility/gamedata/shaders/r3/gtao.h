// Dead Air: Refined - GTAO bodies shared by the pixel passes (gtao_render.ps, gtao_filter.ps)
// and their compute versions (gtao_render.cs, gtao_filter.cs, the DirectX 12 async path).
//
// Both stages call the same function with the INTEGER pixel: the pixel shaders pass
// uint2(SV_Position.xy), the compute shaders SV_DispatchThreadID.xy. The texture coordinate is
// rebuilt from it with the same float operations in both, so a compute pass writes exactly the
// values its pixel pass writes (r__async_verify compares them bit for bit).

#ifndef GTAO_H
#define GTAO_H

#include "common.h"

// r__async_verify compares the pixel and the compute stage bit for bit. DXC marks every float
// operation fast, so the driver contracts each stage's pipeline its own way: a last-bit difference
// in a tap's coordinate picks the neighbouring texel (0.1 % of the GTAO pixels, up to 0.28 of
// occlusion), in a value it flips the half-float rounding. precise on a stage's output keeps both
// stages to the same operations. FXC (DirectX 11, the DirectX 12 fallback) is left as it was.
#ifndef DA_STAGE_EXACT
#ifdef GRIZZLY_DXC
#define DA_STAGE_EXACT precise
#else
#define DA_STAGE_EXACT
#endif
#endif

Texture2D s_blue_noise; // R8G8: x = slice angle jitter, y = step jitter
Texture2D<float2> t_gtao_packed; // x = view-z, y = raw AO ($user$gtao_0)

// x: world-space radius -> screen-space radius factor, height / (4 * tan(fov/2)); y: slices (the preset's tier);
// zw: this frame's turn of the noise (gtao_temporal.ps gathers the turns), set by phase_gtao every frame.
float4 gtao_parameters;

// The centre of a render-size pixel as a texture coordinate.
float2 gtao_pixel_tc(uint2 pix)
{
    return (float2(pix) + 0.5) * pos_decompression_params2.zw;
}

// The render pass reads the single-sampled G-buffer: GTAO is never created under MSAA
// (r2_rendertarget.cpp), where common.h types s_position as Texture2DMS.
#ifndef USE_MSAA

// View-space position of an arbitrary tap from its texcoord and stored view-z.
// Same math as gbuffer_load_data (common_functions.h), texcoord form: our
// pos_decompression_params carries (HorzTan, VertTan) with DX11 signs, exactly
// like the donor's, so the donor expression survives unchanged.
float3 gtao_view_pos(float2 tc, float viewz)
{
    return float3((tc * 2.0 - 1.0) * pos_decompression_params.xy * viewz, viewz);
}

// Reference normal for the horizon search. The march runs over the DEPTH buffer, which is
// flat per triangle; measuring it against the interpolated, normal-mapped shading normal
// makes every facet of a smooth-shaded low-poly mesh (an NPC's arm, face, fingers)
// occlude itself on the side where the shading normal tilts away from the facet - a
// per-triangle gradient that reads as the wireframe showing through the skin. The facet's
// own plane, taken from the depth neighbourhood, is what the march actually sees. Each
// axis takes the neighbour closer in depth so a silhouette does not pull in the surface
// behind it; the result is oriented by the shading normal and trusted only where the two
// roughly agree - thin alpha-tested geometry (grass, leaves) has no usable depth slope and
// keeps the shading normal it had.
float3 gtao_reference_normal(float2 tc, float3 P, float3 Ns)
{
    float2 dx = float2(pos_decompression_params2.z, 0.0);
    float2 dy = float2(0.0, pos_decompression_params2.w);

    float zl = s_position.SampleLevel(smp_nofilter, tc - dx, 0).z;
    float zr = s_position.SampleLevel(smp_nofilter, tc + dx, 0).z;
    float zu = s_position.SampleLevel(smp_nofilter, tc - dy, 0).z;
    float zd = s_position.SampleLevel(smp_nofilter, tc + dy, 0).z;

    float3 ex = (abs(zl - P.z) < abs(zr - P.z)) ? (P - gtao_view_pos(tc - dx, zl)) : (gtao_view_pos(tc + dx, zr) - P);
    float3 ey = (abs(zu - P.z) < abs(zd - P.z)) ? (P - gtao_view_pos(tc - dy, zu)) : (gtao_view_pos(tc + dy, zd) - P);

    float3 Ng = cross(ex, ey);
    float len = length(Ng);
    if (len < 1e-8)
        return Ns;
    Ng /= len;
    Ng = (dot(Ng, Ns) < 0.0) ? -Ng : Ng;

    // 0.85 is ~32 degrees, more than the facet-to-vertex-normal spread of any game mesh.
    return normalize(lerp(Ns, Ng, smoothstep(0.7, 0.85, dot(Ng, Ns))));
}

float gtao_compute(float3 view_position, float3 view_normal, float2 texcoord, float2 jitter)
{
    const float GTAO_TAU = 6.2831854;
    const float GTAO_HALF_PI = 1.5707964;

    // Donor settings: 4 steps, 0.85 m radius; the slices from the preset (3 the donor's, 1 the cheap tier the temporal
    // pass makes whole over the frames).
    const int GTAO_DIRECTIONS = max(int(gtao_parameters.y), 1);
    const int GTAO_STEPS = 4;
    const float GTAO_RADIUS = 0.85;
    const float GTAO_NEG_1_OVER_RADIUSQR = -1.0 / (GTAO_RADIUS * GTAO_RADIUS);

    // Bias the position to avoid numerical issues
    view_position *= 0.9992;

    float3 view_direction = -normalize(view_position);

    // Screen-space radius (clamped)
    float screen_radius = min((GTAO_RADIUS * gtao_parameters.x) / view_position.z, 256.0);

    // Slice scale; Y flipped: texture v grows down, view-space y grows up
    float2 slice_scale = pos_decompression_params2.zw * screen_radius * float2(1.0, -1.0);

    // We integrate AO over 2*PI
    float slice_angle = GTAO_TAU / float(GTAO_DIRECTIONS);

    // Accumulated occlusion and slice weight
    float2 occ_weight = (0.0).xx;

    [loop] for (int i = 0; i < GTAO_DIRECTIONS; i++)
    {
        float angle = (float(i) + jitter.x) * slice_angle;

        float3 slice_direction = float3(cos(angle), sin(angle), 0.0);

        // GTAO math
        float3 axis = cross(view_direction, slice_direction);
        float3 proj_normal = view_normal - axis * dot(view_normal, axis);
        float3 proj_tangent = cross(axis, proj_normal);
        float proj_normal_length = length(proj_normal);
        float sin_n = dot(proj_tangent, view_direction) * rcp(proj_normal_length);

        // Init horizon
        float max_horizon_cos = sin_n;

        for (int j = 0; j < GTAO_STEPS; j += 2)
        {
            // Ray increment, squared for more detail in crevices
            float2 increment = (j + float2(0.0, 1.0) + jitter.yy) / GTAO_STEPS;
            increment *= increment;

            float4 s_texcoord = texcoord.xyxy + slice_direction.xyxy * slice_scale.xyxy * increment.xxyy;

            // Guard band
            if (dot(s_texcoord.zw - saturate(s_texcoord.zw), 1.0) != 0.0)
                break;

            // Fetch linear view-z of both taps; a cleared (sky) texel counts as
            // infinitely far so it can never manufacture occlusion
            float2 s_depth = float2(
                s_position.SampleLevel(smp_nofilter, s_texcoord.xy, 0).z,
                s_position.SampleLevel(smp_nofilter, s_texcoord.zw, 0).z);
            s_depth = (s_depth < 0.01) ? (10000.0).xx : s_depth;

            // 1st tap (two steps per iteration, manually unrolled)
            {
                float3 s_vector = gtao_view_pos(s_texcoord.xy, s_depth.x) - view_position;

                float s_vec_length = dot(s_vector, s_vector);
                float s_horizon = dot(s_vector, view_direction) * rsqrt(s_vec_length);

                // Obscurance falloff, as in HBAO
                float falloff = saturate(s_vec_length * GTAO_NEG_1_OVER_RADIUSQR + 1.0);
                s_horizon = lerp(-1.0, s_horizon, falloff);

                max_horizon_cos = max(max_horizon_cos, s_horizon);
            }

            // 2nd tap
            {
                float3 s_vector = gtao_view_pos(s_texcoord.zw, s_depth.y) - view_position;

                float s_vec_length = dot(s_vector, s_vector);
                float s_horizon = dot(s_vector, view_direction) * rsqrt(s_vec_length);

                float falloff = saturate(s_vec_length * GTAO_NEG_1_OVER_RADIUSQR + 1.0);
                s_horizon = lerp(-1.0, s_horizon, falloff);

                max_horizon_cos = max(max_horizon_cos, s_horizon);
            }
        }

        // Importance-sampling approximation with the horizon remap baked in
        // (uniformly weighted GTAO, see the donor header for references)
        max_horizon_cos = saturate(0.5 * sin(GTAO_HALF_PI * (1.0 + sin_n) - GTAO_HALF_PI * max_horizon_cos) + 0.5);

        // Weight by projected normal length
        occ_weight += float2(1.0 - max_horizon_cos, 1.0) * proj_normal_length;
    }

    // Normalize and compensate for the missing hemisphere side
    occ_weight.x *= rcp(occ_weight.y);
    return saturate(1.0 - occ_weight.x * 2.0);
}

// The render pass for one pixel: (view-z, raw occlusion) for rt_gtao (R16G16_FLOAT).
float2 gtao_render_pixel(uint2 pix)
{
    const float2 tc0 = gtao_pixel_tc(pix);
    float4 P = s_position.SampleLevel(smp_nofilter, tc0, 0);

    // Sky / cleared texel: park it far away, fully lit - the guided filter then
    // rejects it for real geometry by depth variance (the donor writes 1000/1.0
    // into its packed target for the same reason)
    if (P.z < 0.01)
        return float2(10000.0, 1.0);

    // Both channels of the blue-noise tile, tiled by pixel position (static
    // slice, as the donor default)
    float2 jitter = frac(s_blue_noise.Load(int3(pix % 128u, 0)).xy + gtao_parameters.zw);

    // Unpack the G-buffer
    float3 Point = gtao_view_pos(tc0, P.z);
#ifdef GBUFFER_OPTIMIZATION
    float3 Normal = gbuf_unpack_normal(P.xy);
#else
    float3 Normal = s_normal.SampleLevel(smp_nofilter, tc0, 0).xyz;
#endif

    // Don't render GTAO past 60 units - it becomes a noisy mess. The view
    // position is shifted along the normal to kill self-occlusion.
    float3 Ref = gtao_reference_normal(tc0, Point, Normal);
    float occlusion = Point.z > 60.0
        ? 1.0
        : gtao_compute(Point + Ref * 0.0035, Ref, tc0, jitter);

    // R16G16_FLOAT: x = view-z (filter guide), y = raw occlusion
    return float2(Point.z, occlusion);
}

#endif // USE_MSAA

// The guided 8x8 filter for one pixel: the final AO for rt_ssao_temp.
float gtao_filter_pixel(uint2 pix)
{
    // Texel-corner-snapped coordinates for Gather4 (fixes grid-like artifacts, see
    // reedbeta.com/blog/texture-gathers-and-coordinate-precision): the corner below-right of
    // the pixel's centre, from the integer pixel. The interpolated form this replaced,
    // floor(tc * size - 0.5) + 1, meant the same corner but could round to the neighbour's.
    float2 gather_texcoord = (float2(pix) + 1.0) * pos_decompression_params2.zw;

    // 16 gather quads = the 8x8 footprint, offsets as in the donor.
    // Depth (R) guides the fit, AO (G) is the filtered signal.
    float4 depth_taps[16] =
    {
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 0,  0)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 0,  2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 2,  2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 2,  0)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-2, -2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 0, -2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-2,  0)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-4, -4)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-2, -4)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 0, -4)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 2, -4)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-4, -2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-4,  0)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2( 2, -2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-4,  2)),
        t_gtao_packed.GatherRed(smp_nofilter, gather_texcoord, int2(-2,  2))
    };

    float4 occ_taps[16] =
    {
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 0,  0)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 0,  2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 2,  2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 2,  0)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-2, -2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 0, -2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-2,  0)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-4, -4)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-2, -4)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 0, -4)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 2, -4)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-4, -2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-4,  0)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2( 2, -2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-4,  2)),
        t_gtao_packed.GatherGreen(smp_nofilter, gather_texcoord, int2(-2,  2))
    };

    // Only the depth of the center pixel guides the reconstruction
    float center_tap = t_gtao_packed.Load(int3(pix, 0)).x;

    // Accumulated moments: E[x], E[x^2], E[y], E[xy] where x = depth, y = AO
    float4 x_x2_y_xy = (0.0).xxxx;

    [loop]
    for (int i = 0; i < 16; i++)
    {
        float4 depth_tap = depth_taps[i];
        float4 occ_tap = occ_taps[i];

        x_x2_y_xy += float4(
            dot(depth_tap, (1.0).xxxx),
            dot(depth_tap, depth_tap),
            dot(occ_tap, (1.0).xxxx),
            dot(occ_tap, depth_tap));
    }

    // 1/64
    x_x2_y_xy *= 0.015625;

    // Local linear model AO ~= beta * depth + alpha over the window
    float cyx = (x_x2_y_xy.w - x_x2_y_xy.x * x_x2_y_xy.z);
    float vx = (x_x2_y_xy.y - x_x2_y_xy.x * x_x2_y_xy.x) + 1e-4; // bias

    float beta = cyx * rcp(vx);
    float alpha = x_x2_y_xy.z - beta * x_x2_y_xy.x;

    // Final, filtered value
    return saturate(beta * center_tap + alpha);
}

#endif // GTAO_H
