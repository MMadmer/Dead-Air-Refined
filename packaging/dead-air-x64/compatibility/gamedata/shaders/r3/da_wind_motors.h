#ifndef DA_WIND_MOTORS_H
#define DA_WIND_MOTORS_H

// Wind motors (the Tsushima "vorticle" idea, budgeted to 8 sources): local sources of
// vegetation response, simulated on the CPU (Environment::UpdateEffectiveWind) and consumed
// here per instance root.
//  * press motors - an actor standing in the grass pushes it radially outward; when they move
//    on, the CPU drives a damped spring-back oscillation (grass overshoots and settles);
//  * impulse motors - explosions/blowouts send an expanding ring: the front hits each root
//    ONCE as it passes, and behind it every root relaxes on its own (a source only owns a
//    root while the root is inside its active zone - never the whole disc);
//  * shot motors - a narrow line gust along a bullet trace: a brief outward shiver.
// Rows: pos = (world xyz, radius-or-length),
//       par = (signed amplitude, ring radius | dir.x, ring width | dir.z,
//              kind: 0 press / -1 blast / 1+slope line).
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
            if (dist3 > 0.45f)
                continue;
            const float t = dist3 * (1.0f / 0.16f);
            const float2 radial = (dist_xz > 0.02f) ? (d / dist_xz) : float2(-ldir.y, ldir.x);
            const float2 push = normalize(ldir * 0.75f + radial * 0.50f);
            bend += push * (exp(-t * t) * A.x * H);
            continue;
        }

        const float dist = length(d);
        [branch]
        if (dist > P.w + A.z * 2.0f || dist < 0.001f)
            continue;

        // Radial motors. A.w = -1 marks a BLAST: A.x is only the peak strength, and the
        // whole spatial shape is computed from this root's own distance - the Kinney-Graham
        // ~1/R particle-velocity falloff (full flatten only near the epicentre, anchored at
        // a quarter of the reach) plus an edge fade, so the front visibly weakens as it
        // travels and arrives at the rim with nothing left. Presses (A.w = 0) keep their
        // CPU-driven envelope untouched.
        const bool is_blast = (A.w < -0.5f);
        float amp = A.x;
        [flatten]
        if (is_blast)
            amp *= min(1.1f, (0.25f * P.w) / max(dist, 0.5f)) *
                saturate((P.w - dist) / (0.30f * P.w));
        // Ring FRONT: a gaussian around the current ring radius. Press motors keep ring
        // radius at 0, which turns the same formula into a bump centred on the actor.
        const float t = (dist - A.y) / A.z;
        float w = exp(-t * t);
        [branch]
        if (is_blast && dist < A.y)
        {
            // The front already passed this root, so the ring no longer owns it: the hit was
            // a ONE-TIME push and from then on the root springs back to rest by itself. A
            // stateless VS still gets per-root memory for free, because the moment the front
            // crossed HERE is a pure function of distance: tau = seconds since the hit
            // (front expands at 22 m/s, kept in sync with Environment.cpp). Damped cosine =
            // ease back with one soft overshoot past vertical; tau 0 matches the gaussian
            // peak, so the handover at the front is seamless - and because the falloff above
            // is per-root too, every root finishes its OWN oscillation even after the ring
            // edge has gone quiet.
            const float tau = (A.y - dist) * (1.0f / 22.0f);
            w = exp(-tau * 3.5f) * cos(tau * 9.0f);
        }
        bend += (d / dist) * (w * amp * H);
        // Press motors are the ones with a still ring (A.y == 0) and a positive amplitude
        // envelope; their footprint also flattens the wind wave.
        press_w = max(press_w, w * saturate(A.x * 2.0f) * ((A.y <= 0.001f && !is_blast) ? 1.0f : 0.0f));
    }
    return bend;
}

#endif // DA_WIND_MOTORS_H
