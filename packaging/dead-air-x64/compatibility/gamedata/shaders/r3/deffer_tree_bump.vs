#include "common.h"
#include "tree_instance.h"

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
    float dp = calc_cyclic(wave.w + dot(pos, (float3)wave));
    float H = pos.y - base;
    float frac = I.tc.z * consts.x;
    float inten = H * dp;
    float2 result = calc_xz_wave(wind.xz * inten, frac);
#ifdef USE_TREEWAVE
    result = 0;
#endif
    float4 w_pos = float4(pos.x + result.x, pos.y, pos.z + result.y, 1);
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
