# Specular

The stock materials (G-buffer codes 2-26) shade their specular after DOOM Eternal's model (`specBRDF` and its
analytic `environmentBRDF`, from the static breakdown of its shaders), on DirectX 11, DirectX 12 and Vulkan alike.
Foliage (codes 27-30) and snow (31, its glint 1) keep their own terms. Shaders `lmodel.h`, `hmodel.h`, `combine_1.ps`
and the light passes in the compatibility overlay.

## 1. Lights

- **What was there:** `s_material`, a LUT of Blinn lobes per material slice (exponents 16, 24, 128 and the metal's
  rippled 24); the gloss only scaled the result, and `combine_1` multiplied the specular by the diffuse light sum - the
  light's intensity squared, tinted by the albedo's light.
- **Now** (`da_spec_ggx`): GGX without the 1/pi the diffuse lacks too, Schlick-Smith visibility with k = alpha^2,
  Schlick Fresnel with f90 = saturate(50 f0).
  - Width: the slice's Blinn exponent n as alpha^2 = 2 / (n + 2), narrowed by the gloss the rain raises (DOOM's
    alpha^2 = (1 - 0.8 s)^4, s = 3 x gloss).
  - Strength: f0 = 0.8 x gloss, at most 0.16. The content authored its gloss as the strength of its specular - a dry
    road 0.005, a bus 0.045, a wet road 0.15 to 0.25 - and the stock lobe's energy at a gloss of 0.05 is a
    dielectric's 4 %. No physical floor: 2 % gave a dry road five times the stock sheen.
  - `combine_1` takes the lights' specular at its own strength in the lights' hue (the diffuse sum with its
    `Vibrance(2)` undone), not the gloss times the diffuse sum.
- Local lights and the sun alike (`plight_local` / `plight_infinity` with the gloss); the stock signatures stay for
  shaders that do not pass it.

## 2. The sky

- **What was there:** the blurred irradiance cube (`env_s0/1`) at its sharpest mip for every surface, through a fake
  `y * 2 - 1` remap, squared, weighted by the LUT and the gloss.
- **Now:** the sky cubes themselves (`sky_s0/1`) in the true mirror direction at mip 6 (1 - s) (DOOM's probe mip),
  tinted as the dome shows them (the fog's `da_sky_tint`, `da_fog_common.h`), weighted by DOOM's analytic
  environment BRDF and by the sky the point sees (its hemi term, so tunnels do not mirror the sky). The ray traced
  reflection of glossy surfaces still takes the cube's place where it hits (`dxr.md` 3.9), now under the same
  weight; the cone specular occlusion follows the same smoothness.

## 3. Measured

RTX 3070 Laptop, 1440p, DLSS Balanced, noon and a steady rain on the bus by the road, an interior lit by a lamp
(the A/B against `r__spec_ggx 0`, an A/A shot beside each):

| | DirectX 12 | Vulkan | DirectX 11 |
|---|---|---|---|
| Dry, noon | -0.4 of 255 | -0.3 | -0.4 |
| Wet, rain | +1.2: wet ground mirrors the sky | +1.2 | +1.5 |
| Interior, lamp | -0.2 | - | - |
| Cost | +0.1 ms in the combine, +0.01 in the lights | | |

## 4. Knobs

Look constants, re-applied by the preset sync: `r__spec_ggx` (1; 0 the stock lobe; QA: 2 shows the gloss, 3 the
lights' specular in red and the sky's in green, after the tone map), `r__spec_f0` 0.8, `r__spec_f0_max` 0.16,
`r__spec_smooth` 3.
