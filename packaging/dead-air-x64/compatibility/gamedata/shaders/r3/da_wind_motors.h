#ifndef DA_WIND_MOTORS_H
#define DA_WIND_MOTORS_H

// Wind motors (the Tsushima "vorticle" idea, budgeted to 8 sources): point sources of LOCAL
// vegetation response, simulated on the CPU (Environment::UpdateEffectiveWind) and consumed
// here per instance root.
//  * press motors - an actor standing in the grass pushes it radially outward; when they move
//    on, the CPU drives a damped spring-back oscillation (grass overshoots and settles);
//  * impulse motors - explosions/blowouts send an expanding ring that bends everything
//    outward from the epicentre as the front passes.
// Rows: pos = (world xyz, radius), par = (signed bend amplitude, ring radius, ring width, 0).
// da_wm_info.x = number of live motors: the loop costs nothing while the world is quiet.
uniform float4x4 da_wm_pos0;
uniform float4x4 da_wm_pos1;
uniform float4x4 da_wm_par0;
uniform float4x4 da_wm_par1;
uniform float4 da_wm_info;

// Horizontal bend (world XZ) for a vegetation instance rooted at root_w, scaled by height so
// tips move and roots stay planted. Returns a displacement to ADD to the wind result.
float2 da_wind_motors_bend(float2 root_w, float H)
{
    float2 bend = float2(0.0f, 0.0f);
    const int count = int(da_wm_info.x);
    [loop]
    for (int i = 0; i < count; ++i)
    {
        // Both ternary sides are evaluated in HLSL, so each index must stay in range on its own.
        const int lo = min(i, 3);
        const int hi = max(i - 4, 0);
        const float4 P = (i < 4) ? da_wm_pos0[lo] : da_wm_pos1[hi];
        const float4 A = (i < 4) ? da_wm_par0[lo] : da_wm_par1[hi];
        [branch]
        if (P.w <= 0.0f || abs(A.x) <= 0.001f)
            continue;

        const float2 d = root_w - P.xz;
        const float dist = length(d);
        [branch]
        if (dist > P.w + A.z * 2.0f || dist < 0.001f)
            continue;

        // Ring profile: a gaussian around the current ring radius. Press motors keep ring
        // radius at 0, which turns the same formula into a bump centred on the actor.
        const float t = (dist - A.y) / A.z;
        const float w = exp(-t * t);
        bend += (d / dist) * (w * A.x * H);
    }
    return bend;
}

#endif // DA_WIND_MOTORS_H
