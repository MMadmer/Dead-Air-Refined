#ifndef DA_HUD_LIGHT_H
#define DA_HUD_LIGHT_H

// First-person geometry under local lights.
//
// The HUD is rasterized through its own narrow projection into the top slice of the depth
// range, so: (a) its pixels are told apart by the hardware depth (a copy the g-buffer left,
// s_depth_copy), (b) their lateral eye-space position has to be rebuilt with the HUD field of
// view - the deferred decompression carries the camera's and puts a first-person pixel about
// twice as far off-axis as it is, which is why a lamp used to light the weapon from the wrong
// side, and (c) no light's shadow map holds the hands: the self-shadow is marched in screen
// space against the HUD's own depth, toward the light, for every local light - a lamp, a
// fire, the torch in the player's own hand.
Texture2D s_depth_copy;
uniform float4 da_hud_light;   // HorzTan, VertTan, 2*HorzTan/w, 2*VertTan/h
uniform float4 da_hud_light2;  // hud depth limit, enabled, normal offset (m), local-light shadow on

bool da_hud_pixel(float2 pos2d)
{
    [branch] if (da_hud_light2.y < 0.5f)
        return false;
    return s_depth_copy.Load(int3(pos2d, 0)).x < da_hud_light2.x;
}

// Eye-space position of a first-person pixel from its (trusted) eye-space depth.
float3 da_hud_position(float2 pos2d, float Pz)
{
    return float3(Pz * (pos2d * da_hud_light.zw - da_hud_light.xy), Pz);
}

// Eye space -> the pixel the HUD projection puts it on.
float2 da_hud_project(float3 Pe)
{
    return (Pe.xy / Pe.z + da_hud_light.xy) / da_hud_light.zw;
}

// Screen-space shadow toward a local light: walk from the surface toward the light through
// the HUD's own depth; a first-person pixel closer than the ray is the hand or the item in
// the way. World pixels never count - the world is never nearer than the hands. The bias
// grows with the distance walked (a curved surface seen along the ray drifts in depth by
// more than any fixed epsilon), the first centimetres are skipped (they are the surface
// itself), and the result is a soft darkening, never black.
float da_hud_ss_shadow(float3 Pe, float3 N, float3 Lpos, float2 pos2d)
{
    float3 to_l = Lpos - Pe;
    const float dist = length(to_l);
    [branch] if (dist < 0.04f)
        return 1.0f;
    to_l /= dist;
    // The hands and the item: half a metre of march is the whole first-person scene.
    const float len = min(dist, 0.45f);
    const int steps = 10;
    const float t_start = 0.025f;
    const float3 start = Pe + N * (da_hud_light2.z + 0.004f);
    // No per-pixel jitter: on a surface this small it reads as dither, not as softness.
    float hit = 0.0f;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float t = t_start + (float(i) + 0.5f) / float(steps) * (len - t_start);
        const float3 p = start + to_l * t;
        [branch] if (p.z <= 0.03f)
            break;
        const float2 sp = da_hud_project(p);
        [branch] if (any(sp < 0.0f) || any(sp >= 2.0f * da_hud_light.xy / da_hud_light.zw))
            break;
        [branch] if (!da_hud_pixel(sp))
            continue;
        const float zs = s_position.Load(int3(sp, 0)).z;
        const float dz = p.z - zs;
        const float bias = 0.008f + 0.06f * t;
        // A soft edge: the occlusion ramps in over a centimetre instead of switching.
        [branch] if (dz < 0.12f + bias)
            hit = max(hit, saturate((dz - bias) * 100.0f));
        [branch] if (hit >= 0.999f)
            break;
    }
    return 1.0f - 0.7f * hit;
}

#endif // DA_HUD_LIGHT_H
