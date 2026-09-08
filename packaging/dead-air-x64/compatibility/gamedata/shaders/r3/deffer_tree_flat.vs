#include "common.h"
#include "tree_instance.h"
#include "da_wind_field.h"
#include "da_tree_bend.h"

uniform float3x4 m_xform;
uniform float3x4 m_xform_v;
uniform float4 consts;
uniform float4 c_scale, c_bias, wind, wave;
uniform float4 c_sun;
uniform float4 c_tree;

v2p_flat main(v_tree I, uint instance_id : SV_InstanceID)
{
    I.Nh = unpack_D3DCOLOR(I.Nh);
    I.T = unpack_D3DCOLOR(I.T);
    I.B = unpack_D3DCOLOR(I.B);

    float3x4 local_xform = m_xform;
    float3x4 local_xform_v = m_xform_v;
    float4 local_c_scale = c_scale;
    float4 local_c_bias = c_bias;
    float4 local_c_sun = c_sun;
    float4 local_c_tree = c_tree;
    if (tree_instance_control.x > 0.5f)
    {
        local_xform = tree_instance_xform(instance_id);
        local_xform_v = tree_instance_xform_v(instance_id);
        local_c_scale = tree_instance_scale(instance_id);
        local_c_bias = tree_instance_bias(instance_id);
        local_c_sun = tree_instance_sun(instance_id);
        local_c_tree = tree_instance_tree(instance_id);
    }

    v2p_flat o;
    float3 pos = mul(local_xform, I.P);
    float base = local_xform._24;
    // Height above the root, never below it. Root and butt geometry sits under the pivot on
    // many models (spruces above all); a negative height would turn the length-keeping drop
    // below into a lift of twice the depth and float the tree on its mirrored roots.
    float H = max(pos.y - base, 0.0f);
    float frac = I.tc.z * consts.x;

    const float3 root3 = float3(local_xform._14, local_xform._24, local_xform._34);
    // The bend itself lives in da_tree_bend.h - one deformation for the crown, the trunk and
    // the shadow. c_sun.z is the crown's CPU-integrated oscillator state, c_sun.w its
    // natural-frequency factor from the model's real height; the hash is the fallback for a
    // tree that carries neither.
    da_tree_bend_in bi;
    bi.pos = pos;
    bi.root = root3;
    bi.H = H;
    bi.tree_h = local_c_tree.x;
    bi.frac = frac;
    bi.q_state = local_c_sun.z;
    bi.freq_k = local_c_sun.w > 0.01f ? local_c_sun.w : (0.82f + 0.42f * da_wf_hash(root3.xz * 0.37f));
    bi.flex_gate = da_tree_row_gate(local_c_tree.w);
    float2 result = da_tree_bend(bi, wave, wind, float3(local_c_tree.yz, da_tree_row_valid(local_c_tree.w)));
#ifdef USE_TREEWAVE
    result = 0;
#endif
    const float drop = H - sqrt(max(H * H - dot(result, result), 0.0f));
    float4 f_pos = float4(pos.x + result.x, pos.y - drop, pos.z + result.y, 1);

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
