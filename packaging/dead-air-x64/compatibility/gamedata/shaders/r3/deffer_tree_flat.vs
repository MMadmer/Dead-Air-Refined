#include "common.h"
#include "tree_instance.h"
#include "da_wind_field.h"
#include "da_wind_motors.h"

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
    float H = pos.y - base;
    float frac = I.tc.z * consts.x;
    // Wave phase from the INSTANCE ROOT, not the vertex position. The stock per-vertex phase
    // was fine at the authored 0.05 amplitude (a millimetre shimmer of shape), but scaled up
    // it desynchronises halves of one crown - one side still leaning while the other springs
    // back, and the crown visibly SQUASHES every cycle. One phase per tree: the crown moves
    // as a whole, the arc comes from the baked flexibility, the shiver below keeps the
    // fine per-vertex life.
    //
    // Each tree also gets its OWN natural frequency (mass and stiffness differ), so the
    // forest drifts out of step instead of swaying like a drilled parade; and the waveform
    // is da_sway - a static downwind lean with smooth harmonic oscillation around it - in
    // place of the stock parabola that ricocheted off its peak and swung crowns as far
    // upwind as downwind.
    // The mean/swing split follows the wind strength: light air = shallow mean lean with wide
    // swings almost back to upright; a steady gale = deep stable lean with only an elastic
    // spring around it - a plant under CONSTANT wind never straightens back up, the full
    // lean-recover cycle belongs to gusts (which the field's tongues add on top).
    const float3 root3 = float3(local_xform._14, local_xform._24, local_xform._34);
    const float freq_k = 0.82f + 0.42f * da_wf_hash(root3.xz * 0.37f);
    const float wind_k = saturate(da_wind_field.z);
    const float sway_mean = 0.45f + 0.35f * wind_k;
    float dp = sway_mean + (1.0f - sway_mean) * da_sway(wave.w * freq_k + dot(root3, (float3)wave));
    float inten = H * dp;
    // Local flow from the travelling gust field at the TREE ROOT: a gust tongue leans this
    // crown while the next tree over stands in a lull. The z channel turns the LOCAL heading:
    // neighbouring trees in a meander lean slightly different ways, and the swirl travels.
    float3 flow = da_wind_field_eval(root3.xz);
    const float2 wdir = da_wind_local_dir(wind.xz, flow.z);
    // frac (tc.z) is the AUTHORED per-vertex flexibility baked into the model: ~0 on the
    // trunk, ~1 at branch tips. The stock wave respects it via calc_xz_wave = dir * frac.
    // Every term we add must respect it too - a lean or shiver applied without frac moves
    // the rigid trunk sideways as a whole ("the tree slides on XY"), which is exactly what
    // a field test showed. Roots are anchored; everything bends as an arc from them.
    float2 result = calc_xz_wave(wdir * (inten * flow.x), frac);
    result += wdir * (H * flow.y * 0.5f * frac);
    // Blast rings rock the flexible parts too (press motors are too small to reach trees).
    float press_unused;
    result += da_wind_motors_bend(root3.xz, H, press_unused) * (0.35f * saturate(frac * 2.0f));
    // Foliage shiver (bushes live on this): a finer, 2.3x faster wave for the OUTER foliage.
    // Per-vertex phase is CORRECT here - leaves flutter independently - and the amplitude is
    // small enough to read as rustle, not shape distortion. Double-gated so the trunk stays
    // dead: by authored flexibility AND by radial distance from the instance axis.
    const float axis_r = length(pos.xz - root3.xz);
    const float leaf_w = saturate((axis_r - 0.3f) * 1.1f);
    const float dp2 = da_flutter(wave.w * 2.3f * freq_k + dot(pos, (float3)wave * 3.7f));
    result += wdir * (dp2 * leaf_w * saturate(H * 1.5f) * frac * 1.2f);
    // Soft saturation of the TOTAL bend. Storm weathers author amplitude 0.10 (double the
    // usual), and the envelope on top once folded a crown into a half-circle; a HARD cap
    // fixed that but pinned the treetop against an invisible wall while the lower crown kept
    // moving (the tip has the highest flexibility, so it hit the limit first). tanh is the
    // progressive stiffness of a real trunk: resistance grows smoothly with the bend and the
    // limit (~0.5*H, ~30 degrees) is an asymptote nothing ever visibly slams into.
    const float bend_len = length(result);
    const float bend_max = H * 0.50f;
    [branch] if (bend_len > 0.001f)
        result *= bend_max * tanh(bend_len / bend_max) / bend_len;
#ifdef USE_TREEWAVE
    result = 0;
#endif
    // Arc-length correction, same as the grass: a branch keeps its length, so a displaced
    // tip drops instead of sliding sideways at constant height - the bend reads as a BEND.
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
