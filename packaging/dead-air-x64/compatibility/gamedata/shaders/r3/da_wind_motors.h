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
            // A bullet's wake is a hand's width, not a metre: the tube stays tight (45 cm cut,
            // 16 cm sigma). Bushes react at the hit because the wake is measured to the
            // VERTEX now, not because the tube was widened - a wide tube stirred the grass
            // under shots that passed a metre above it.
            if (dist3 > 0.45f)
                continue;
            const float t = dist3 * (1.0f / 0.16f);
            const float2 radial = (dist_xz > 0.02f) ? (d / dist_xz) : float2(-ldir.y, ldir.x);
            const float2 push = normalize(ldir * 0.75f + radial * 0.50f);
            // The lever is the height up to three metres, as it always was for bushes and low
            // branches, and fades to nothing by six: the wake shakes what it passes within
            // reach, it does not swing a crown twelve metres up by twelve metres (the spike a
            // shot through a tree used to leave).
            bend += push * (exp(-t * t) * A.x * min(H, max(0.0f, 6.0f - H)));
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
        // The height lever belongs to grass, bushes and the branches a body brushes, where the
        // plant bends from its root - unchanged up to 2.2 m. Above that a press fades out and
        // is gone by 3.2 m: a stalker walking past a trunk used to throw crown cards twelve
        // metres up out by metres (H at the top times the press strength, then the 0.5 H
        // cap). A blast keeps the full lever at any height - its front bends the whole tree.
        const float reach = is_blast ? 1.0f : saturate(3.2f - H);
        bend += (d / dist) * (w * amp * H * reach);
        // Press motors are the ones with a still ring (A.y == 0) and a positive amplitude
        // envelope; their footprint also flattens the wind wave.
        press_w = max(press_w, w * saturate(A.x * 2.0f) * ((A.y <= 0.001f && !is_blast) ? 1.0f : 0.0f));
    }
    return bend;
}

// Motors on tree-shader geometry: crowns, trunks and the bushes (Dead Air's bushes are tree
// models under flora\leaf_wave). The grass rule above - strength times height, measured at
// the tuft root - does not carry over. A boot flattens a blade of grass, a shoulder does not
// fold a branch, and a crown card is metres wide where a tuft is a hand: a footprint narrower
// than the card tears it into spikes, and a lever of the crown's height throws it out by
// metres. Two regimes, blended on the model height (c_tree.x - the height of the tallest part
// sharing the root):
//  * BUSH (under ~3 m): one plant. A press and a blast are measured at the root with the
//    canopy radius taken off the distance, so a body inside the bush leans the whole bush
//    away from itself and every card moves together; a shot through the canopy at card
//    height shivers the whole bush, softly in the vertical, so a card a metre tall does not
//    tear.
//  * TREE (over ~4.5 m): the trunk is stiff and a body reaches no higher than it stands. A
//    press moves only the flexible foliage (authored frac) within 2.2 m above the presser's
//    feet, fading out by 3.2 m, with a body-wide footprint and a bounded travel; a shot gives
//    the low foliage a gentle, wide shiver (nothing above six metres); a blast bends the
//    whole tree from its root through the authored flexibility, the way the wind does.
float2 da_tree_motors_bend(float3 pos, float3 root, float H, float tree_h, float frac)
{
    float2 bend = float2(0.0f, 0.0f);
    const int count = int(da_wm_info.x);
    const float bush_k = saturate((4.5f - tree_h) * (1.0f / 1.5f));
    const float canopy = 0.4f * min(tree_h, 4.5f);
    // Foliage keeps trunks and thick branches (frac under 0.3) out of a press or a wake;
    // flex is the plain authored flexibility, what the wind and a blast bend by.
    const float foliage = saturate((frac - 0.3f) * 2.5f);
    const float flex = saturate(frac * 2.0f);
    const float vy = root.y + H;
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

        const float2 d_root = root.xz - P.xz;
        const float2 d_vert = pos.xz - P.xz;

        [branch]
        if (A.w > 0.5f)
        {
            // Shot trace. Bush: the root against the trace, canopy taken off, the plant's own
            // height as the vertical width. Tree: the vertex against the trace, a wide soft
            // wake (a card's corners are a metre apart) on the low foliage only.
            const float2 ldir = float2(A.y, A.z);
            float2 dr = d_root;
            const float along_r = clamp(dot(dr, ldir), 0.0f, P.w);
            dr -= ldir * along_r;
            const float dist_r = max(length(dr) - canopy, 0.0f);
            float2 dvt = d_vert;
            const float along_v = clamp(dot(dvt, ldir), 0.0f, P.w);
            dvt -= ldir * along_v;
            const float dist_v = length(dvt);
            [branch]
            if (min(dist_r, dist_v) > 1.2f)
                continue;
            const float dy_r = P.y + (A.w - 1.0f) * along_r - vy;
            const float dy_v = P.y + (A.w - 1.0f) * along_v - vy;
            const float sig_yb = max(0.30f, 0.35f * tree_h);
            const float tr = dist_r * (1.0f / 0.16f);
            const float tyb = dy_r / sig_yb;
            const float w_bush = exp(-tr * tr - tyb * tyb) * (0.8f * min(H, 1.2f));
            const float tv = dist_v * (1.0f / 0.45f);
            const float tyv = dy_v * (1.0f / 0.6f);
            const float w_tree = exp(-tv * tv - tyv * tyv) * (0.25f * min(H, 2.2f)) * foliage * saturate((6.0f - H) * 0.5f);
            const float2 off = lerp(dvt, dr, bush_k);
            const float off_len = length(off);
            const float2 radial = (off_len > 0.02f) ? (off / off_len) : float2(-ldir.y, ldir.x);
            const float2 push = normalize(ldir * 0.75f + radial * 0.50f);
            bend += push * (A.x * lerp(w_tree, w_bush, bush_k));
            continue;
        }

        const bool is_blast = (A.w < -0.5f);
        const float dist_r = length(d_root);
        [branch]
        if (is_blast)
        {
            // The whole tree from its root: one distance, one phase for every card of it. The
            // shaping is the grass one (Kinney-Graham falloff, edge fade, one-time front with
            // the per-root spring-back behind it), the travel the authored flexibility.
            [branch]
            if (dist_r > P.w + A.z * 2.0f || dist_r < 0.001f)
                continue;
            const float amp = A.x * min(1.1f, (0.25f * P.w) / max(dist_r, 0.5f)) * saturate((P.w - dist_r) / (0.30f * P.w));
            const float t = (dist_r - A.y) / A.z;
            float w = exp(-t * t);
            [branch]
            if (dist_r < A.y)
            {
                const float tau = (A.y - dist_r) * (1.0f / 22.0f);
                w = exp(-tau * 3.5f) * cos(tau * 9.0f);
            }
            bend += (d_root / dist_r) * (w * amp * H * 0.8f * flex);
            continue;
        }

        // Press. Bush: the root against the body, canopy taken off - a body in the bush leans
        // the plant as one, the boot-sized footprint (A.z) only softens the edge. Tree: the
        // vertex against the body - a shoulder's reach rather than a boot's, no higher than
        // the body stands, foliage only, the travel bounded (0.22 x height up to 2.2 m).
        const float dist_rb = max(dist_r - canopy, 0.0f);
        const float dist_v = length(d_vert);
        [branch]
        if (dist_rb > P.w + A.z * 2.0f && dist_v > 1.6f)
            continue;
        const float tb = dist_rb / A.z;
        const float w_bush = exp(-tb * tb) * (0.7f * H) * flex;
        const float tv = max(dist_v - 0.35f, 0.0f) * (1.0f / 0.45f);
        const float h_body = vy - P.y;
        const float reach = saturate(3.2f - h_body) * saturate(h_body + 1.0f);
        const float w_tree = exp(-tv * tv) * (0.22f * min(H, 2.2f)) * reach * foliage;
        const float2 dir_b = (dist_r > 0.02f) ? (d_root / dist_r) : float2(1.0f, 0.0f);
        // A soft centre: a card that straddles the body is not torn in two directions.
        const float2 dir_v = d_vert / max(dist_v, 0.5f);
        bend += lerp(dir_v * w_tree, dir_b * w_bush, bush_k) * A.x;
    }
    return bend;
}

#endif // DA_WIND_MOTORS_H
