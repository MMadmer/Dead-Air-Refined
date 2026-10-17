# Lit particles

The blended particles (smoke, dust, steam, mist; blend mode BLEND) take the scene's light after DOOM Eternal's
lit particles, on DirectX 11, DirectX 12 and Vulkan alike. Additive, alpha-additive and multiplying particles (fire,
sparks, glows, decals of light) are light themselves and stay as they were. Shaders `particle.vs`,
`particle_unlit.vs`, `particle.ps` and `da_particle_light.h` in the compatibility overlay; engine
`src/Layers/xrRenderPC_R4/da_particle_light.cpp` and `Blender_Particle_deferred.cpp`.

## 1. What was there

Every particle was drawn at its authored colour in any light: night smoke glowed grey-blue, dust under a roof was as
bright as in the noon sun. The deferred particle blender also gave every blend mode the BLEND pixel shader, so the
fog-by-blend split of the first DOOM wave (additive particles lose their light in the fog, blended ones take the
fog's colour) never reached the game; it now picks `particle`, `particle_add`, `particle_mul`, `particle_mul2` by
the blend.

## 2. The light, per vertex

`da_particle_lighting` in the vertex shader, the light a white surface at the vertex would show:

- **Sun:** the sun light's colour at the diffuse's 0.35 of a puff lit on one half, times its visibility - the
  froxel volume's sun term (the cascades and the cloud shadow, `VOLUMETRIC_LIGHT.md`), read at the vertex. Outside
  the volume, or on a frame without it, the clouds' share of the sun.
- **Sky:** the irradiance cubes `env_s0/1` looked up and to the side, tinted and squared as `hmodel.h` does.
- **Ambient:** the weather's ambient at the scene's scale.
- **Local lights:** the frame's eight nearest lights (by the near side of their spheres, within 80 m of the eye),
  taken as the lights are rendered: the stock attenuation, spots with their cone.
- Through the scene's exposure (`s_tonemap`) and the tone curve's shoulder at its white point, relative to a white
  surface at the exposure's white - the light the effects were authored for - and capped at 1.5.

## 3. Rain, snow and splashes

The rain's streaks, the snowflakes and the splash crowns (`da_rain.vs`, `da_rain_splash.vs`) take the same light in
their vertex shaders. They kept a constant floor of half their authored grey whatever the light, so a night's rain,
a downpour under a roof's edge and the crowns in a dark yard stood out at the brightness of the day's. A drop and a
flake are lenses: on the High rung of the rain's ladder (`r__rain_quality`) the sun and the local lights add a
forward lobe toward the eye, up to four times their light where the eye looks through the drop at them (it was the
sun's alone). With `r__particle_light 0` they keep the old terms.

## 4. Near the eye

Blended and additive particles also fade over the last 40 cm before the near plane (DOOM Eternal's camera fade, on
the soft particles' path): a puff the camera walks through no longer shows the plane's straight cut. A muzzle's
smoke from 0.6 m on stays whole. Multiplying particles now fade toward white (MUL) and grey (MUL_2X), the colours
that leave the frame as it is, at the soft edge and near the eye alike; fading them toward black darkened what they
crossed.

## 5. Flipbooks

An animated sequence (the effect's frames played at its speed: fireballs, flames, anomalies) blends into its next
frame by the particle's progress through the current one, as DOOM Eternal's particles do, instead of stepping from
frame to frame: a 77-frame fireball played at 39 frames a second no longer judders at 60 or 144 Hz. The particle's
vertex carries the same corner in the next frame and the blend (`FVF::LIT_FLIP`, `ParticleEffect.cpp`); a sequence
played backwards blends toward the frame before. Random still frames and single images take one fetch as before,
the blending ones a second (`particle.ps`). QA: `r__particle_light 3` paints the blend over every particle (red
toward the next frame, blue none).

## 6. Cost

The particles need the froxel volume's sun term. With the glow on it is there anyway; with the glow off (overcast,
`r__froxel_strength 0`) the injection runs only while lit particles were drawn in the last two seconds, and without
its wisps' noise. The additive and multiplying particles take `particle_unlit.vs`, so they neither light themselves
nor keep the volume alive.

RTX 3070 Laptop, 1440p, DLSS Balanced, noon, glow off: the injection for the particles alone +0.2 ms in the sun slot.
The per-vertex light itself is below the timer's noise.

## 7. Measured

Heli smoke and a storm haze 8 m ahead (the A/B against `r__particle_light 0`):

| | DirectX 12 | DirectX 11 | Vulkan |
|---|---|---|---|
| Noon | -0.9 of 255: the smoke takes the warm sun and the overcast sky instead of its blue-grey | -0.8 | -0.5 |
| Night | -1.3: the smoke no longer glows | -0.4 | -1.7 |

## 8. Knobs

A look constant, re-applied by the preset sync: `r__particle_light` (1; 0 the stock unlit particles; QA: 2 shows
the light itself at half strength over the particles' coverage, 3 the flipbooks' blend).
