#include "common.h"
#include "tree_instance.h"

uniform float3x4 m_xform;
uniform float3x4 m_xform_v;
uniform float4 consts;
uniform float4 c_scale, c_bias, wind, wave;
uniform float2 c_sun;

v2p_flat main(v_tree I, uint instance_id : SV_InstanceID)
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

    v2p_flat o;
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
    float4 f_pos = float4(pos.x + result.x, pos.y, pos.z + result.y, 1);

    float3 Pe = mul(m_V, f_pos);
    float hemi = I.Nh.w * local_c_scale.w + local_c_bias.w;
    o.hpos = mul(m_VP, f_pos);
    o.N = mul((float3x3)local_xform_v, unpack_bx2(I.Nh));
    o.tcdh = float4((I.tc * consts).xyyy);
    o.position = float4(Pe, hemi);

#if defined(USE_R2_STATIC_SUN) && !defined(USE_LM_HEMI)
    float suno = I.Nh.w * local_c_sun.x + local_c_sun.y;
    o.tcdh.w = suno;
#endif

#ifdef USE_TDETAIL
    o.tcdbump = o.tcdh * dt_params;
#endif

    return o;
}
FXVS;
