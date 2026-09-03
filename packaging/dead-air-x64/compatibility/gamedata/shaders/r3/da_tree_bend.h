#ifndef DA_TREE_BEND_H
#define DA_TREE_BEND_H

#include "da_wind_field.h"
#include "da_wind_motors.h"

// The bend of a tree under wind, shared by the three tree vertex shaders (flat, bump, shadow)
// so a crown, its trunk and its shadow are ONE deformation.
//
// Two profiles, two jobs:
//  * the TRUNK bend - a function of height only, (h/H)^2 from the root - moves every vertex at
//    a height the same way. The trunk bends with the crown, and a branch card's base stays on
//    the trunk it grows from. Amplitude: the authored wind amplitude times the height, softly
//    capped near 8 degrees at the top - a real trunk in a gale;
//  * the BRANCH term - the authored per-vertex flexibility (tc.z: ~0 trunk, ~1 tips) - adds
//    the extra travel of branch tips and the leaf flutter on top, small enough that a card
//    never leaves what it grows on. (The old chain put the WHOLE bend behind that channel:
//    the trunk stood still while the cards leaned metres away from it.)
// Motors (shot wakes, blasts, presses) are measured at the VERTEX: a bullet through a bush
// shakes the branches at the trace, not only a bush shot at its root.
struct da_tree_bend_in
{
    float3 pos;     // world position of the vertex, unbent
    float3 root;    // world position of the instance root
    float H;        // height of the vertex above the root
    float tree_h;   // height of the model (c_tree.x, instance row 9)
    float frac;     // authored flexibility (tc.z * consts.x)
    float q_state;  // crown oscillator state (c_sun.z)
    float freq_k;   // natural-frequency factor (c_sun.w)
};

float2 da_tree_bend(da_tree_bend_in I, float4 wave, float4 wind)
{
    const float hn = saturate(I.H / max(I.tree_h, 1.0f));
    // Waveform: a static downwind lean with harmonic oscillation around it, one phase per
    // tree (a per-vertex phase squashed crowns), the mean/swing split following the wind.
    const float wind_k = saturate(da_wind_field.z);
    const float sway_mean = 0.45f + 0.35f * wind_k;
    const float dp = sway_mean + (1.0f - sway_mean) * da_sway(wave.w * I.freq_k + dot(I.root, (float3)wave));
    // Local flow from the travelling gust field at the root: a tongue leans this crown while
    // the next tree stands in a lull; the deviation channel turns the local heading.
    const float3 flow = da_wind_field_eval(I.root.xz);
    const float2 wdir = da_wind_local_dir(wind.xz, flow.z); // its length is the authored amplitude

    // Trunk: the whole tree from the root up, sway plus the gust lean.
    const float trunk = I.H * hn * I.q_state;
    float2 result = wdir * (trunk * (dp * flow.x * 0.8f + flow.y * 0.4f));
    // Branches: the extra travel of the flexible tips over the trunk they sit on.
    result += wdir * (I.H * dp * I.q_state * flow.x * I.frac * 0.35f);
    // Leaf flutter: a finer, 2.3x faster wave on the outer foliage, per-vertex phase.
    const float axis_r = length(I.pos.xz - I.root.xz);
    const float leaf_w = saturate((axis_r - 0.3f) * 1.1f);
    const float dp2 = da_flutter(wave.w * 2.3f * I.freq_k + dot(I.pos, (float3)wave * 3.7f));
    result += wdir * (dp2 * leaf_w * saturate(I.H * 1.5f) * I.frac * 1.2f);
    // Motors at the vertex. The trunk is stiff (frac), the foliage is not.
    float press_unused;
    result += da_wind_motors_bend(float3(I.pos.x, I.root.y, I.pos.z), I.H, press_unused) * (0.8f * saturate(I.frac * 2.0f));
    // Progressive stiffness: the top of a tree leans a few degrees, it never folds.
    const float bend_len = length(result);
    const float bend_max = I.H * 0.14f;
    [branch] if (bend_len > 0.001f)
        result *= bend_max * tanh(bend_len / bend_max) / bend_len;
    return result;
}

#endif // DA_TREE_BEND_H
