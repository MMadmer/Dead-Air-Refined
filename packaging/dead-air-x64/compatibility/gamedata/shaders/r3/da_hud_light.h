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
uniform float4 da_hud_light2;  // hud depth limit, enabled, normal offset (m), -

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
// the way. World pixels never count - the world is never nearer than the hands.
float da_hud_ss_shadow(float3 Pe, float3 N, float3 Lpos, float2 pos2d)
{
    float3 to_l = Lpos - Pe;
    const float dist = length(to_l);
    [branch] if (dist < 0.02f)
        return 1.0f;
    to_l /= dist;
    // The hands and the item: half a metre of march is the whole first-person scene.
    const float len = min(dist, 0.5f);
    const int steps = 12;
    const float3 start = Pe + N * (da_hud_light2.z + 0.003f);
    // A per-pixel start jitter turns the step planes into grain.
    const float jitter = frac(52.9829189f * frac(0.06711056f * pos2d.x + 0.00583715f * pos2d.y));
    float shadow = 1.0f;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float t = (float(i) + 0.5f + (jitter - 0.5f)) / float(steps) * len;
        const float3 p = start + to_l * t;
        [branch] if (p.z <= 0.03f)
            break;
        const float2 sp = da_hud_project(p);
        // The screen size, from the decompression params: w = 2*HorzTan/z, h = 2*VertTan/w.
        [branch] if (any(sp < 0.0f) || any(sp >= 2.0f * da_hud_light.xy / da_hud_light.zw))
            break;
        [branch] if (!da_hud_pixel(sp))
            continue;
        const float zs = s_position.Load(int3(sp, 0)).z;
        const float dz = p.z - zs;
        // In front of the ray by more than the bias, and not so far that it is another finger
        // seen past the edge of the hand: an occluder of a plausible thickness.
        [branch] if (dz > 0.004f && dz < 0.10f)
        {
            shadow = 0.0f;
            break;
        }
    }
    return shadow;
}

#endif // DA_HUD_LIGHT_H
