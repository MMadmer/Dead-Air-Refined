#include "common.h"
#include "tree_instance.h"
#include "da_wind_field.h"
#include "da_wind_motors.h"

uniform float3x4 m_xform;
uniform float3x4 m_xform_v;
uniform float4 consts;
uniform float4 c_scale, c_bias, wind, wave;
uniform float2 c_sun;

v2p_bumped main(v_tree I, uint instance_id : SV_InstanceID)
{
    I.Nh = unpack_D3DCOLOR(I.Nh);
    I.T = unpack_D3DCOLOR(I.T);
    I.B = unpack_D3DCOLOR(I.B);

    float3x4 local_xform = m_xform;
    float3x4 local_xform_v = m_xform_v;
    float4 local_c_scale = c_scale;
    float4 local_c_bias = c_bias;
    float2 local_c_sun = c_sun;
    if (tree_instance_control.x > 0.5f)
    {
        local_xform = tree_instance_xform(instance_id);
        local_xform_v = tree_instance_xform_v(instance_id);
        local_c_scale = tree_instance_scale(instance_id);
        local_c_bias = tree_instance_bias(instance_id);
        local_c_sun = tree_instance_sun(instance_id);
    }

    float3 pos = mul(local_xform, I.P);
    float base = local_xform._24;
    float H = pos.y - base;
    float frac = I.tc.z * consts.x;
    // Root phase + per-tree natural frequency + wind-dependent mean/swing split + local
    // heading + flexibility rule + total-bend cap: all identical to deffer_tree_flat.vs -
    // see the notes there.
    const float3 root3 = float3(local_xform._14, local_xform._24, local_xform._34);
    const float freq_k = 0.82f + 0.42f * da_wf_hash(root3.xz * 0.37f);
    const float wind_k = saturate(da_wind_field.z);
    const float sway_mean = 0.45f + 0.35f * wind_k;
    float dp = sway_mean + (1.0f - sway_mean) * da_sway(wave.w * freq_k + dot(root3, (float3)wave));
    float inten = H * dp;
    float3 flow = da_wind_field_eval(root3.xz);
    const float2 wdir = da_wind_local_dir(wind.xz, flow.z);
    float2 result = calc_xz_wave(wdir * (inten * flow.x), frac);
    result += wdir * (H * flow.y * 0.5f * frac);
    // Motors reach bushes only - see deffer_tree_flat.vs.
    float press_unused;
    result += da_wind_motors_bend(root3.xz, H, press_unused) *
        (0.6f * saturate(frac * 2.0f) * saturate((3.5f - H) * 0.5f));
    const float axis_r = length(pos.xz - root3.xz);
    const float leaf_w = saturate((axis_r - 0.3f) * 1.1f);
    const float dp2 = da_flutter(wave.w * 2.3f * freq_k + dot(pos, (float3)wave * 3.7f));
    result += wdir * (dp2 * leaf_w * saturate(H * 1.5f) * frac * 1.2f);
    // Soft tanh saturation - see deffer_tree_flat.vs.
    const float bend_len = length(result);
    const float bend_max = H * 0.50f;
    [branch] if (bend_len > 0.001f)
        result *= bend_max * tanh(bend_len / bend_max) / bend_len;
#ifdef USE_TREEWAVE
    result = 0;
#endif
    // Arc-length correction: displaced tips drop, the bend reads as a bend.
    const float drop = H - sqrt(max(H * H - dot(result, result), 0.0f));
    float4 w_pos = float4(pos.x + result.x, pos.y - drop, pos.z + result.y, 1);
    float2 tc = (I.tc * consts).xy;
    float hemi = I.Nh.w * local_c_scale.w + local_c_bias.w;

    v2p_bumped O;
    float3 Pe = mul(m_V, w_pos);
    O.tcdh = float4(tc.xyyy);
    O.hpos = mul(m_VP, w_pos);
    O.position = float4(Pe, hemi);

#if defined(USE_R2_STATIC_SUN) && !defined(USE_LM_HEMI)
    float suno = I.Nh.w * local_c_sun.x + local_c_sun.y;
    O.tcdh.w = suno;
#endif

    float3 N = unpack_bx4(I.Nh);
    float3 T = unpack_bx4(I.T);
    float3 B = unpack_bx4(I.B);
    float3x3 xform = mul((float3x3)local_xform_v, float3x3(
        T.x, B.x, N.x,
        T.y, B.y, N.y,
        T.z, B.z, N.z));

    O.M1 = xform[0];
    O.M2 = xform[1];
    O.M3 = xform[2];

#ifdef USE_TDETAIL
    O.tcdbump = O.tcdh * dt_params;
#endif

    return O;
}
FXVS;
