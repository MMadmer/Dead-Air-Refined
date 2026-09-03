#ifndef DA_HUD_LIGHT_H
#define DA_HUD_LIGHT_H

// First-person geometry under local lights.
//
// The HUD is rasterized through its own narrow projection into the top slice of the depth
// range, so: (a) its pixels are told apart by the hardware depth (a copy the g-buffer left,
// s_depth_copy), and (b) their lateral eye-space position has to be rebuilt with the HUD
// field of view - the deferred decompression carries the camera's and puts a first-person
// pixel about twice as far off-axis as it is, which is why a lamp used to light the weapon
// from the wrong side. The self-shadow of the hands stays the sun's alone (hud_shadow.ps): a
// screen-space march toward each local light was tried and read as a dotted contact band.
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

#endif // DA_HUD_LIGHT_H
