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
  - Width: DOOM's alpha^2 = (1 - 0.8 s)^4 with s = 3 x gloss, the smoothness the sky's term takes too (section 2) -
    a dull surface is a rough one, a wet one narrows. The first version held it at the stock slice's lobe (Blinn
    exponent n as alpha^2 = 2 / (n + 2), 0.111 for soil): dry ground seen at a grazing angle against a sun behind the
    clouds mirrored it at 15 times its diffuse light, and a distant hill or a village road looked under snow.
  - Strength: f0 = 0.8 x gloss, at most 0.16. The content authored its gloss as the strength of its specular - a dry
    road 0.005, a bus 0.045, a wet road 0.15 to 0.25 - and the stock lobe's energy at a gloss of 0.05 is a
    dielectric's 4 %. No physical floor: 2 % gave a dry road five times the stock sheen.
  - `combine_1` takes the lights' specular at its own strength in the lights' hue (the diffuse sum with its
    `Vibrance(2)` undone), not the gloss times the diffuse sum.
- Local lights and the sun alike (`plight_local` / `plight_infinity` with the gloss); the stock signatures stay for
  shaders that do not pass it. The light blenders bind the albedo target for the gloss (they named none, so the
  first version read whatever an earlier pass left in that slot). Every lamp, a point light without a shadow map
  too, shades through one shader since the light volumes (`da_light_volume.ps`, LIGHT_VOLUMES.md).
- Geometric specular antialiasing (Kaplanyan & Tokuyoshi 2019; DOOM has none): what the G-buffer normal turns
  across the pixel's quad widens alpha^2 by its variance (at most 0.18), on the smooth surfaces only (weighted by
  2 x smoothness) - a wet road's or a car's highlight thinner than a pixel stops flickering, while a rough surface,
  which would only gain a grazing sheen, keeps its lobe (`da_spec_aa`, called once per pixel by the light passes).
- A dielectric's Fresnel rises toward f90 at grazing angles (f90 = saturate(50 f0)); on dry ground (f0 0.004, f90
  0.2) the rough lobe keeps that sheen under its diffuse light, on a wet road it is the glare toward the sun.

## 2. The sky

- **What was there:** the blurred irradiance cube (`env_s0/1`) at its sharpest mip for every surface, through a fake
  `y * 2 - 1` remap, squared, weighted by the LUT and the gloss.
- **Now:** the sky cubes themselves (`sky_s0/1`) in the true mirror direction at mip 6 (1 - s) (DOOM's probe mip),
  tinted as the dome shows them (the fog's `da_sky_tint`, `da_fog_common.h`), weighted by DOOM's analytic
  environment BRDF and by the sky the point sees (its hemi term, so tunnels do not mirror the sky). The ray traced
  reflection of glossy surfaces still takes the cube's place where it hits (`dxr.md` 3.9), and without ray tracing
  the screen-space one (`SSR.md`), now under the same weight; the cone specular occlusion follows the same
  smoothness.

## 3. Measured

RTX 3070 Laptop, 1440p, DLSS Balanced, noon and a steady rain on the bus by the road, an interior lit by a lamp
(the A/B against `r__spec_ggx 0`, an A/A shot beside each):

| | DirectX 12 | Vulkan | DirectX 11 |
|---|---|---|---|
| Dry, noon | +1.3 of 255: the sun's grazing sheen | +1.3 | +1.3 |
| Wet, rain | +1.4: wet ground mirrors the sky and the sun | +1.3 | +1.4 |
| Cost | +0.1 ms in the combine, +0.01 in the lights | | |

(Before the light blenders bound the gloss, the lights' share was missing: dry -0.4, wet +1.2, the sky's alone.)

## 4. Knobs

Look constants, re-applied by the preset sync: `r__spec_ggx` (1; 0 the stock lobe; QA: 2 shows the gloss, 3 the
lights' specular in red and the sky's in green, after the tone map), `r__spec_f0` 0.8, `r__spec_f0_max` 0.16,
`r__spec_smooth` 3.

## 5. Glass

The blended env-mapped models - windows of vehicles and machines (`models_xwindows.s`), the EbB blender's
transparent models - are drawn forward over the tone-mapped frame (`model_env_lq.vs/.ps`), after DOOM Eternal's
glass:

- **What was there:** a fixed daylight cube (`sky\sky_5_cube`) blended into the texture by its alpha, lit by the R1
  hemisphere: a car's windows shone with the day at night and never took the weather's sky.
- **Now:** the weather's own sky in the mirror direction as the dome shows it (`hmodel.h`), through Schlick with a
  glass's f0 of 4 %; the sun's glint where the froxel volume lets the sun reach the glass; the dirt the texture
  paints at its alpha, lit like the lit particles (`LIT_PARTICLES.md`) - all through the scene's exposure. The frame
  behind shows through what the dirt and the mirror leave.
- `r__particle_light 0` brings the stock glass back.
