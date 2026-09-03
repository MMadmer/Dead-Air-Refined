#ifndef DA_CLOUDS_H
#define DA_CLOUDS_H

// The cloud deck: one coverage field, three consumers.
//
// The deck is a world object - a layer of cloud at a fixed altitude above the level, drifting
// with the wind aloft - and everything that involves clouds evaluates the SAME field at the
// same world position:
//   * the visible deck (clouds.ps) intersects the view ray with the deck plane and reads it,
//   * the cloud shadow (shadow.h sunmask) projects the receiver up the sun ray to the deck
//     plane and reads it - so the shadow on the ground is the shadow of the cloud overhead,
//   * the sun shafts read the same shadow.
// That correlation is the whole point; the stock dome scrolled two textures in UV space with
// no world-metre meaning, and its shadow was an unrelated noise on an unrelated clock.
//
// The field is rendered once per frame into the cloud map (da_cloud_map.ps -> $user$cloud_map,
// a 16 km square around the camera at ~16 m per texel). Readers fetch it; across the outer band
// of the square the map fades into the noise evaluated directly, so the far deck near the
// horizon continues it without a visible edge.
//
// da_cloud_params  (cl_da_cloud_params):  x = coverage 0..1 from the weather, y = deck base
//                   altitude above the level's ground (m), zw = world-space drift of the field
//                   (m) - the aloft wind integrated by the service.
// da_cloud_params2 (cl_da_cloud_params2): x = quality tier 0..3, y = deck thickness (m),
//                   z = service clock (s), w = shadow density (0 = no shadow).
// da_cloud_map     (cl_da_cloud_map):     xy = map centre (world XZ), z = edge length, w = 1/z.
uniform float4 da_cloud_params;
uniform float4 da_cloud_params2;
uniform float4 da_cloud_map;

// ---- Noise ----------------------------------------------------------------------------------
float da_cl_hash(float2 p)
{
    p = frac(p * float2(0.1031f, 0.1030f));
    p += dot(p, p.yx + 33.33f);
    return frac((p.x + p.y) * p.x);
}

float da_cl_noise(float2 p)
{
    const float2 i = floor(p);
    float2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const float a = da_cl_hash(i);
    const float b = da_cl_hash(i + float2(1.0f, 0.0f));
    const float c = da_cl_hash(i + float2(0.0f, 1.0f));
    const float d = da_cl_hash(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Three octaves, rotated between them so the lattice never shows. Returns ~0..1.
float da_cl_fbm3(float2 p)
{
    const float2x2 rot = float2x2(0.80f, 0.60f, -0.60f, 0.80f);
    float v = 0.0f;
    float a = 0.5f;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        v += a * da_cl_noise(p);
        p = mul(rot, p) * 2.03f + 17.0f;
        a *= 0.5f;
    }
    return v * (1.0f / 0.875f);
}

// ---- The coverage field at the deck plane -----------------------------------------------------
// Large cells (~2 km) shaped by the weather's coverage: 0 leaves clear sky, 1 closes the deck.
// The edge is narrow on purpose - a cloud has an edge, a haze does not.
float da_cloud_coverage(float2 xz)
{
    const float2 q = (xz - da_cloud_params.zw) * (1.0f / 2200.0f);
    const float n = da_cl_fbm3(q);
    const float c = saturate(da_cloud_params.x);
    // Calibrated to the fbm: the covered fraction of the sky tracks c itself.
    const float thr = 0.55f - 0.28f * c;
    return smoothstep(thr, thr + 0.14f, n);
}

// Fine erosion (~300 m), drifting a touch faster than the cells: shear between the layers is
// what makes a deck read as weather rather than a stamp.
float da_cloud_detail(float2 xz)
{
    const float2 q = (xz - da_cloud_params.zw * 1.18f) * (1.0f / 310.0f);
    return da_cl_fbm3(q + float2(5.3f, 9.1f));
}

// Coverage carved by detail, the rim only: inside a cell the density stays 1, at the edge the
// detail decides where the cloud ends (Schneider's remap).
float da_cloud_erode(float cov, float det)
{
    [branch] if (cov <= 0.001f)
        return 0.0f;
    const float e = det * 0.55f * (1.0f - cov);
    return saturate((cov - e) / max(1.0f - e, 0.05f));
}

// Fine erosion (~140 m) the map is too coarse to hold; both deck paths apply it per pixel.
float da_cloud_fine(float2 xz, float h01)
{
    return da_cl_noise((xz - da_cloud_params.zw * 1.3f) * (1.0f / 140.0f) + h01 * 3.7f);
}

// Flat-deck density evaluated from the noise (the map's own source; readers use the map).
float da_cloud_density2d(float2 xz)
{
    return da_cloud_erode(da_cloud_coverage(xz), da_cloud_detail(xz));
}

// ---- The map --------------------------------------------------------------------------------
float2 da_cloud_map_uv(float2 xz) { return (xz - da_cloud_map.xy) * da_cloud_map.w + 0.5f; }

// Distance from the map centre in half-widths: 0 at the centre, 1 on the square's edge.
float da_cloud_map_edge(float2 uv)
{
    const float2 e = abs(uv - 0.5f) * 2.0f;
    return max(e.x, e.y);
}

// x = density, y = coverage, z = detail. From the map inside its square; across the outer
// fifth the map fades into the noise evaluated directly, so the square never shows.
float3 da_cloud_field(Texture2D map, float2 xz)
{
    const float2 uv = da_cloud_map_uv(xz);
    const float edge = da_cloud_map_edge(uv);
    float3 m = 0;
    [branch] if (edge < 1.0f)
        m = map.SampleLevel(smp_rtlinear, uv, 0).xyz;
    [branch] if (edge < 0.8f)
        return m;
    const float cov = da_cloud_coverage(xz);
    const float det = da_cloud_detail(xz);
    float3 n = float3(da_cloud_erode(cov, det), cov, det);
    // A NaN here poisons every probe sum and comparison downstream of it (the march skipped
    // whole rays); the noise is finite for finite input, so this only guards the input.
    [flatten] if (any(isnan(n)) || any(isinf(n)))
        n = m;
    return lerp(m, n, smoothstep(0.8f, 1.0f, edge));
}

// Cloud top as a fraction of the slab: dense columns tower, thin ones stay low.
float da_cloud_top(float d2) { return 0.4f + 0.6f * d2; }

// Volumetric density inside the slab: h01 = 0 at the base, 1 at the top. x = the density
// here, y = the column's flat density - the lighting needs it to know how much cloud stands
// above the sample. Flat base, erosion growing toward the top: cauliflower, not a slab.
float2 da_cloud_density3d(Texture2D map, float2 xz, float h01)
{
    const float d2 = da_cloud_field(map, xz).x;
    [branch] if (d2 <= 0.001f)
        return 0;
    const float top = da_cloud_top(d2);
    const float bottom = smoothstep(0.0f, 0.10f, h01);
    const float cap = 1.0f - smoothstep(top - 0.30f, top, h01);
    const float erode = da_cloud_fine(xz, h01) * (0.15f + 0.45f * h01);
    return float2(saturate(d2 * bottom * cap - erode), d2);
}

// ---- The shadow ------------------------------------------------------------------------------
// Sun transmittance for a world receiver: walk up the sun ray to the deck plane and read the
// column there. One fetch - it runs for every lit pixel of both cascades and every sample of
// the shafts.
float da_cloud_transmittance(Texture2D map, float3 Pw)
{
    const float density = da_cloud_params2.w;
    [branch] if (density < 0.003f)
        return 1.0f;
    const float3 to_sun = -L_sun_dir_w;
    [branch] if (to_sun.y < 0.05f)
        return 1.0f;
    const float t = (da_cloud_params.y - Pw.y) / to_sun.y;
    [branch] if (t < 0.0f)
        return 1.0f;
    const float2 hit = Pw.xz + to_sun.xz * t;
    // The deck's own thickness under the sun angle sets how dark a full cloud gets; a low sun
    // crosses more of it. Cells are large, the penumbra at a few km is metres - the field's
    // own softness is the softness.
    const float d = da_cloud_field(map, hit).x;
    const float path = 1.0f + 0.6f * (1.0f - to_sun.y);
    return 1.0f - density * saturate(d * path);
}

#endif // DA_CLOUDS_H
