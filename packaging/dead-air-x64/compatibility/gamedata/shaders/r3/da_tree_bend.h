#ifndef DA_TREE_BEND_H
#define DA_TREE_BEND_H

#include "da_wind_field.h"
#include "da_wind_motors.h"

// The bend of a tree under wind, shared by the three tree vertex shaders (flat, bump, shadow)
// so a crown, its trunk and its shadow are ONE deformation.
//
// The sway is the crown's: height times the waveform (a static downwind lean with harmonic
// oscillation around it, one phase per tree), times the crown's response to the gust field
// (the CPU oscillator following the tongues that pass this root), the gust lean on top.
// Who moves how far is the larger, softly, of two weights:
//  * the TRUNK - a cantilever profile of height only, (h/H)^1.5 from the root, three
//    quarters of the crown's travel at the top - so every vertex at a height moves with the
//    trunk at that height and a branch card's base rides the trunk it grows from;
//  * the authored per-vertex FLEXIBILITY (tc.z: ~0 trunk, ~1 tips) - the travel the models
//    were made for, which is the crown motion the field liked: tips out past the trunk.
// The leaf flutter rides the flexibility; motors (shot wakes, blasts, presses) follow the tree
// rule of da_tree_motors_bend: a bush leans as one plant, a tree moves only the foliage a body
// or a wake can reach, a blast bends it whole from the root.
struct da_tree_bend_in
{
    float3 pos;     // world position of the vertex, unbent
    float3 root;    // world position of the instance root
    float H;        // height of the vertex above the root
    float tree_h;   // height of the model (c_tree.x, instance row 9)
    float frac;     // authored flexibility (tc.z * consts.x)
    float q_state;  // crown oscillator state (c_sun.z); 0 = none, the field's value is used
    float freq_k;   // natural-frequency factor (c_sun.w)
    float flex_gate; // foliage at the root (c_tree.w): 0 a stump, a log or a snag, 1 a tree or a bush
};

// c_tree.w / instance row 9 w: 0 = an older producer (analytic field, the full bend);
// 1 + gate = own analytic field; 3 + gate = the shared root sample is valid. The gate says
// whether the root carries foliage: flora\trunk_wave dresses stumps, logs and snags as
// readily as the trunk under a crown, and only a plant - a root with leaf cards - moves. Wood
// reads 0 and stands still in any wind, whatever the tree format's height channel says.
float da_tree_row_valid(float w) { return w > 2.5f ? 1.0f : 0.0f; }
float da_tree_row_gate(float w) { return w > 2.5f ? saturate(w - 3.0f) : (w > 0.5f ? saturate(w - 1.0f) : 1.0f); }

float2 da_tree_bend(da_tree_bend_in I, float4 wave, float4 wind, float3 root_flow)
{
    const float hn = saturate(I.H / max(I.tree_h, 1.0f));
    // Waveform: a static downwind lean with harmonic oscillation around it, one phase per
    // tree (a per-vertex phase squashed crowns), the mean/swing split following the wind.
    const float wind_k = saturate(da_wind_field.z);
    const float sway_mean = 0.45f + 0.35f * wind_k;
    const float dp = sway_mean + (1.0f - sway_mean) * da_sway(wave.w * I.freq_k + dot(I.root, (float3)wave));
    // Local flow from the travelling gust field at the root: a tongue leans this crown while
    // the next tree stands in a lull; the deviation channel turns the local heading.
    float3 flow;
    // Native trees publish the shared field once per root and frame. Old constant layouts
    // leave valid at zero, so tools and older producers retain the analytic path.
    [branch] if (root_flow.z > 0.5f)
        flow = float3(da_wind_field_amp(root_flow.x),
            da_wind_field_lean(root_flow.x, da_wind_field.w), root_flow.y);
    else
        flow = da_wind_field_eval(I.root.xz);
    const float2 wdir = da_wind_local_dir(wind.xz, flow.z); // its length is the authored amplitude

    // The crown's response to the gust field: the oscillator state where the CPU keeps one,
    // the field's instantaneous value where it does not (impostors, tools). Never both.
    const float resp = I.q_state > 0.001f ? I.q_state : flow.x;
    const float sway = I.H * (dp * resp + flow.y * 0.5f);
    // Trunk profile against authored flexibility, soft max: a card whose base is stiff rides
    // the trunk, a tip keeps its own travel.
    // The whole sway is the plant's: the gate takes it out on wood. (The tc.z "flexibility" is
    // the compiler's height fraction, not an authored property - it is ~1 on a stump's top.)
    const float trunk_w = hn * sqrt(hn) * 0.75f;
    const float dw = trunk_w - I.frac;
    const float w = 0.5f * (trunk_w + I.frac + sqrt(dw * dw + 0.0025f));
    float2 result = wdir * (sway * w * I.flex_gate);
    // Leaf flutter: a finer, 2.3x faster wave on the outer foliage.
    //
    // Its phase used to be sampled straight off a world-space field at the VERTEX. That field
    // runs one cycle per 11.4 m, which is a fine ripple across a fifteen-metre crown and a
    // quarter of a cycle across a two-metre bush - one side leaning while the other springs
    // back. On a bush, where the flutter is as strong as the sway itself, that read as a wave
    // sliding left to right through the plant while it bent like a sheet.
    //
    // The phase now comes from the ROOT, so every plant still gets its own, and the per-vertex
    // part is scaled by the plant's own size: a crown flutters leaf by leaf, a shrub flutters
    // as one thing.
    const float axis_r = length(I.pos.xz - I.root.xz);
    const float leaf_w = saturate((axis_r - 0.3f) * 1.1f);
    const float leaf_k = saturate(I.tree_h * 0.12f); // 0 on a shrub, 1 from about 8 m up
    const float dp2 = da_flutter(wave.w * 2.3f * I.freq_k + dot(I.root, (float3)wave * 3.7f)
        + dot(I.pos - I.root, (float3)wave * 3.7f) * leaf_k);
    result += wdir * (dp2 * leaf_w * saturate(I.H * 1.5f) * I.frac * 1.2f * I.flex_gate);
    // Motors: the tree rule (da_wind_motors.h, da_tree_motors_bend); wood takes none.
    result += da_tree_motors_bend(I.pos, I.root, I.H, I.tree_h, I.frac) * I.flex_gate;
    // Progressive stiffness of a real trunk: resistance grows smoothly with the bend and the
    // limit (~0.5 H, ~30 degrees) is an asymptote nothing visibly slams into. (An 8-degree
    // cap tried here left the trees leaning less in a storm than they used to.)
    const float bend_len = length(result);
    const float bend_max = I.H * 0.50f;
    [branch] if (bend_len > 0.001f)
        result *= bend_max * tanh(bend_len / bend_max) / bend_len;
    return result;
}

// Preserve the entry point and input layout used by existing shader addons.
float2 da_tree_bend(da_tree_bend_in I, float4 wave, float4 wind)
{
    return da_tree_bend(I, wave, wind, float3(0, 0, 0));
}

#endif // DA_TREE_BEND_H
