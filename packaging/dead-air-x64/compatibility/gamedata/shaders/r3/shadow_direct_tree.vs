#include "common.h"
#include "tree_instance.h"
#include "da_wind_field.h"
#include "da_tree_bend.h"

// The tree's shadow caster. The SAME deformation as deffer_tree_flat.vs, so on the presets
// that let crowns sway in the cascades (r__tree_shadow_sway) the shadow is the crown's shadow
// and not a stiff copy standing beside it. On the lower presets the wind constant arrives
// as zero here and the whole chain collapses to the motors (a blast still shakes a bush's
// shadow - a transient, not the per-frame shimmer the freeze exists to prevent).
uniform float3x4 m_xform;
uniform float3x4 m_xform_v;
uniform float4 consts;
uniform float4 c_scale, c_bias, wind, wave;
uniform float4 c_sun;
uniform float4 c_tree;

#ifdef USE_AREF
v2p_shadow_direct_aref main(v_shadow_direct_aref I, uint instance_id : SV_InstanceID)
#else
v2p_shadow_direct main(v_shadow_direct I, uint instance_id : SV_InstanceID)
#endif
{
#ifdef USE_AREF
    v2p_shadow_direct_aref O;
#else
    v2p_shadow_direct O;
#endif

    float3x4 local_xform = m_xform;
    float4 local_c_sun = c_sun;
    float4 local_c_tree = c_tree;
    if (tree_instance_control.x > 0.5f)
    {
        local_xform = tree_instance_xform(instance_id);
        local_c_sun = tree_instance_sun(instance_id);
        local_c_tree = tree_instance_tree(instance_id);
    }

    float3 pos = mul(local_xform, I.P);
    float base = local_xform._24;
    float H = pos.y - base;
#ifdef USE_AREF
    float frac = I.tc.z * consts.x;
#else
    // Opaque caster geometry carries no flexibility channel: the trunk, which the trunk
    // profile in da_tree_bend moves; only the branch extras stay off it.
    float frac = 0.0f;
#endif

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
    float2 result = da_tree_bend(bi, wave, wind, local_c_tree.yzw);
    const float drop = H - sqrt(max(H * H - dot(result, result), 0.0f));

    float4 f_pos = float4(pos.x + result.x, pos.y - drop, pos.z + result.y, 1);
    O.hpos = mul(m_VP, f_pos);
#ifdef USE_AREF
    O.tc0 = (I.tc * consts).xy;
#endif
#ifndef USE_HWSMAP
    O.depth = O.hpos.z;
#endif
    return O;
}
FXVS;
