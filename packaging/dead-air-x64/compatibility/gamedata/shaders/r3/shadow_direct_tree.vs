#include "common.h"
#include "tree_instance.h"
#include "da_wind_field.h"
#include "da_wind_motors.h"

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
    if (tree_instance_control.x > 0.5f)
    {
        local_xform = tree_instance_xform(instance_id);
        local_c_sun = tree_instance_sun(instance_id);
    }

    float3 pos = mul(local_xform, I.P);
    float base = local_xform._24;
    float H = pos.y - base;
#ifdef USE_AREF
    float frac = I.tc.z * consts.x;
#else
    // Opaque caster geometry carries no flexibility channel: the trunk, which barely moves.
    float frac = 0.15f * saturate(H * 0.2f);
#endif

    const float3 root3 = float3(local_xform._14, local_xform._24, local_xform._34);
    const float freq_k = local_c_sun.w > 0.01f ? local_c_sun.w : (0.82f + 0.42f * da_wf_hash(root3.xz * 0.37f));
    const float q_state = local_c_sun.z > 0.001f ? local_c_sun.z : 1.0f;
    const float wind_k = saturate(da_wind_field.z);
    const float sway_mean = 0.45f + 0.35f * wind_k;
    float dp = sway_mean + (1.0f - sway_mean) * da_sway(wave.w * freq_k + dot(root3, (float3)wave));
    float inten = H * dp * q_state;
    float3 flow = da_wind_field_eval(root3.xz);
    const float2 wdir = da_wind_local_dir(wind.xz, flow.z);
    float2 result = calc_xz_wave(wdir * (inten * flow.x), frac);
    result += wdir * (H * flow.y * 0.5f * frac * q_state);
    float press_unused;
    result += da_wind_motors_bend(root3, H, press_unused) *
        (0.6f * saturate(frac * 2.0f) * saturate((5.0f - H) * 0.45f));
    const float axis_r = length(pos.xz - root3.xz);
    const float leaf_w = saturate((axis_r - 0.3f) * 1.1f);
    const float dp2 = da_flutter(wave.w * 2.3f * freq_k + dot(pos, (float3)wave * 3.7f));
    result += wdir * (dp2 * leaf_w * saturate(H * 1.5f) * frac * 1.2f);
    const float bend_len = length(result);
    const float bend_max = H * 0.50f;
    [branch] if (bend_len > 0.001f)
        result *= bend_max * tanh(bend_len / bend_max) / bend_len;
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
