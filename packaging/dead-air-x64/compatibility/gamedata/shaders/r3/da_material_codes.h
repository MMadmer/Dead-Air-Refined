// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Material codes of the packed G-buffer above the stock range (docs/dead-air/GRASS.md, SNOW.md).
// gbuf_pack_hemi_mtl() stores uint((mtl / 1.333333333) * 31): stock textures write (m + 0.5) / 4 with
// m in [0, 4], i.e. codes 2..26. Codes 27-31 are unreachable by stock content and survive the FP16 pack
// bit-exactly: 27-30 = two-sided foliage with translucency 0.30 / 0.517 / 0.733 / 0.95, 31 = snow. Code 1,
// below the stock range, is a glint of the snow (da_snow_layer.ps): snow whose normal mirrors the sun.
// Never round-trip .w through an unpacked float: a naive re-pack turns 27 into 26.
#ifndef DA_MATERIAL_CODES_H
#define DA_MATERIAL_CODES_H

#define DA_MTL_FOLIAGE0 27u
#define DA_MTL_SNOW 31u
#define DA_MTL_GLINT 1u
#define DA_FOLIAGE_TR_MIN 0.30
#define DA_FOLIAGE_TR_STEP 0.2166667

// The mtl value that packs to `code` (safe under the truncating pack).
float da_mtl_from_code(uint code) { return (float(code) + 0.5) * (1.333333333 / 31.0); }
uint da_code_from_mtl(float m) { return uint(m * 23.25 + 0.25); }
bool da_is_foliage(float m) { return m > 1.14 && m < 1.32; } // stock max is 1.125
bool da_is_snow(float m) { return m >= 1.32; }
bool da_is_glint(float m) { return m > 0.03 && m < 0.08; } // code 1
// Snow, its glints included: the light and the white balance of snow.
bool da_is_snowy(float m) { return da_is_snow(m) || da_is_glint(m); }
float da_foliage_tr(float m) { return DA_FOLIAGE_TR_MIN + DA_FOLIAGE_TR_STEP * float(min(da_code_from_mtl(m) - DA_MTL_FOLIAGE0, 3u)); }
// Translucency tr in [0.30, 0.95] -> one of the four codes, dithered by interleaved gradient noise so the
// expectation equals tr (TAA resolves it).
float da_pack_foliage_mtl(float tr, float2 pix, uint frame)
{
    const float x = saturate((tr - DA_FOLIAGE_TR_MIN) / (3.0 * DA_FOLIAGE_TR_STEP)) * 3.0;
    const float ign = frac(52.9829189 * frac(dot(pix + 5.588238 * float(frame & 63u), float2(0.06711056, 0.00583715))));
    return da_mtl_from_code(DA_MTL_FOLIAGE0 + min(uint(x + ign), 3u));
}
// The s_material LUT slice for a material: foliage and snow light as Oren-Nayar diffuse (slice 0) plus
// their own transmission terms; every stock material keeps its own slice.
float da_lut_z(float m) { return (m > 1.14 || da_is_glint(m)) ? 0.125 : m; }

#endif // DA_MATERIAL_CODES_H
