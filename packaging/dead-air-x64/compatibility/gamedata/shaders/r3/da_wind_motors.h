#ifndef DA_WIND_MOTORS_H
#define DA_WIND_MOTORS_H

// Wind motors (the Tsushima "vorticle" idea, budgeted to 8 sources): local sources of
// vegetation response, simulated on the CPU (Environment::UpdateEffectiveWind) and consumed
// here per instance root.
//  * press motors - an actor standing in the grass pushes it radially outward; when they move
//    on, the CPU drives a damped spring-back oscillation (grass overshoots and settles);
//  * impulse motors - explosions/blowouts send an expanding ring that bends everything
//    outward from the epicentre as the front passes;
//  * shot motors - a narrow line gust along a bullet trace: a brief outward shiver.
// Rows: pos = (world xyz, radius-or-length),
//       par = (signed amplitude, ring radius | dir.x, ring width | dir.z, 0 radial / 1 line).
// da_wm_info.x = number of live motors: the loop costs nothing while the world is quiet.
uniform float4x4 da_wm_pos0;
uniform float4x4 da_wm_pos1;
uniform float4x4 da_wm_par0;
uniform float4x4 da_wm_par1;
uniform float4 da_wm_info;

// Horizontal bend (world XZ) for a vegetation instance rooted at root_w (xyz - the Y matters
// for the shot-trace height gate), scaled by height so tips move and roots stay planted.
// Returns a displacement to ADD to the wind result. press_w rises to 1 inside a press motor's
// footprint: the caller suppresses the wind wave with it, because grass held down by a boot
// must not keep waving mid-air.
float2 da_wind_motors_bend(float3 root_w, float H, out float press_w)
{
    float2 bend = float2(0.0f, 0.0f);
    press_w = 0.0f;
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

        float2 d = root_w.xz - P.xz;

        [branch]
        if (A.w > 0.5f)
        {
            // Line motor: a SPHERE-TRACED turbulent tube around the 3D flight path. The
            // bullet's wake is measured to the VERTEX (root XZ for tuft coherence, vertex
            // height for the vertical) - so a burst over the grass stirs nothing, a shot
            // into a tall bush shakes the branches AT the path, and a downward shot wakes
            // the grass out where the trace actually drops into it. The push runs mostly
            // ALONG the shot (the wake drags air with the bullet) with a radial spread.
            const float2 ldir = float2(A.y, A.z);
            const float along = clamp(dot(d, ldir), 0.0f, P.w);
            d -= ldir * along;
            const float dist_xz = length(d);
            const float trace_y = P.y + (A.w - 1.0f) * along;
            const float dy = trace_y - (root_w.y + H);
            const float dist3 = sqrt(dist_xz * dist_xz + dy * dy);
            [branch]
            if (dist3 > 0.85f)
                continue;
            const float t = dist3 * (1.0f / 0.32f);
            const float2 radial = (dist_xz > 0.02f) ? (d / dist_xz) : float2(-ldir.y, ldir.x);
            const float2 push = normalize(ldir * 0.75f + radial * 0.50f);
            bend += push * (exp(-t * t) * A.x * H);
            continue;
        }

        const float dist = length(d);
        [branch]
        if (dist > P.w + A.z * 2.0f || dist < 0.001f)
            continue;

        // Ring profile: a gaussian around the current ring radius. Press motors keep ring
        // radius at 0, which turns the same formula into a bump centred on the actor.
        const float t = (dist - A.y) / A.z;
        float w = exp(-t * t);
        // Blast WAKE: behind the expanding front the radial outflow keeps blowing, fading
        // toward the epicentre (already-spent air). sqrt ramps the wake up fast just behind
        // the front - the front itself crosses a tuft in a few frames, it is the WAKE that
        // the eye actually reads as "wind rushing out of the blast". safe_r keeps the
        // division finite for zero-radius press motors (both ternary sides evaluate).
        const float safe_r = max(A.y, 0.5f);
        w = max(w, (A.y > 0.5f && dist < A.y) ? 0.75f * sqrt(dist / safe_r) : 0.0f);
        bend += (d / dist) * (w * A.x * H);
        // Press motors are the ones with a still ring (A.y == 0) and a positive amplitude
        // envelope; their footprint also flattens the wind wave.
        press_w = max(press_w, w * saturate(A.x * 2.0f) * (A.y <= 0.001f ? 1.0f : 0.0f));
    }
    return bend;
}

#endif // DA_WIND_MOTORS_H
