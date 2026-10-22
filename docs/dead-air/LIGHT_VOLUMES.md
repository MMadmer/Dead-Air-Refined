# Local lamps: instanced volumes and parallel shadow faces

DirectX 11, DirectX 12 and Vulkan (`xrRender_R4/R5/R6`). The OpenGL renderer keeps the stock path.

## 1. What was there

- Every lamp was three draws, each with its own pipeline: two stencil passes over its volume mesh (back faces, then
  front faces, Carmack's reverse) and the lighting pass over the marked pixels. The `light_test` hall at night has
  101 shadowed point-lamp faces, a point and two spots: over 300 draws and as many pipeline changes, the cost a
  DirectX 12 or Vulkan driver charges most for.
- The shadow faces were culled on worker threads and drawn on the main thread one by one, into the immediate
  context: 5.5 ms of the DirectX 12 render thread there, a quarter of it.

## 2. Instanced volumes (`da_light_volumes.cpp`, shaders `r3\da_light_volume*`)

- A lamp is a 256-byte record in one buffer per frame (`r3\da_light_volumes.h`): the view-space position and falloff,
  the colour with the lamp's LOD, the cone or cube face, the shadow map and projector rows of its atlas rect, the
  volume's own transform (`light::xform_calc`). An atlas batch draws its records as at most six instanced draws: the
  sphere, the cone and the cube face's cap (the stock `du_*` meshes), each split by the near plane.
- A volume the eye sees from outside draws its front faces with the scene's depth test: what stands before it fails,
  and the coarse depth culling keeps working. One the near plane cuts (a sphere about the lamp holding its mesh,
  with a margin) draws its back faces with the test inverted: what lies behind it fails, the sky too. The other side
  is the shader's reach test, and the spot's cone and the point lamp's face are tested exactly there, which the
  tessellated meshes only approximate. No stencil masks.
- One pixel shader for every kind of lamp, as `accum_base.ps` shaded them: the light model with the GGX lobe and the
  falloff, the 9-tap shadow, the projector (14 textures a batch, t18..t31), the contact shadow, the first-person
  position. 14 registers against the stock's 10; the light pass costs the GPU what the stock one did.
- The unshadowed lamps light with the last batch; a shadowed face whose map had no casters lights nothing, as before.
  `lmodel.h`'s light model reads its one-mip LUT without gradients so the shader can call it after its early outs.

## 3. Shadow faces in parallel (`r2_R_lights.cpp`)

- The casters draw with the slope-scaled bias (`r__smap_slope_bias`) clamped at 0.0005 of depth (1.6.2,
  `kLampSlopeBiasClamp`): unclamped, a wall a lamp saw edge-on (a flashlight lying beside it) went back a metre and
  more in the map, and the ground behind took the light through the wall.
- A lamp the actor carries (never-demote) takes his shadow model's body but not its weapon and gear
  (`CActor::render_shadow_caster`): those sit centimetres off the lamp. 1.6.1 kept the whole model out of them and the
  player lost his shadow on the walls around.

- The frame's faces are culled and recorded up front on worker threads, every worker's share of each atlas batch
  closed into a command list of its own (`FinishCommandList`); the main thread runs the lists in batch order between
  the atlas clears and the batches' lighting - the sun cascades' way. A list takes four faces at least: on DirectX 11
  each costs its execution (two measured the same on DirectX 12). The translucent casters (`-tsh`) lock the shared
  vertex stream: with them the faces go one by one.
- Gone with it on these renderers: the shadow casters' occlusion tests (`light::svis`, one query and one extra draw
  per face per frame - a query cannot be issued in a deferred list on DirectX 12, and the lamp shadow cache keeps
  the faces' statics instead) and the light list they needed (`Lights_LastFrame`). The cache's copy quad has vertices
  of its own: the shared dynamic stream cannot be locked from a worker.

## 4. Measured

`light_test` at night, RTX 3070 Laptop, 2560x1440, interleaved runs, `r__framegen st_fg_off`:

| Renderer | FPS before | after | GPU lights before | after |
|---|---|---|---|---|
| DirectX 11 | 47.6 | 55.8-60.0 | 2.25 ms | 2.38-2.47 ms |
| DirectX 12 | 40.7-42.0 | 47.4-52.0 | 2.13 ms | 2.30-2.44 ms |
| Vulkan | 39.4 | 44.8-51.7 | 1.98 ms | 2.29-2.44 ms |

The spread is the laptop's heat between runs. The frame is the stock one (0.4-0.9/255 apart, the NPCs moving; the
torch's pool 0.8/255). The game thread's light phase went 6.3 to about 3 ms on DirectX 12. The GPU's shadow maps take 0.1-0.2 ms more than before: the casters the occlusion tests used to
skip on the faces the cache does not hold, and the lists' boundaries.

## 5. Tried and dropped: tiled lighting

The lamps culled by a compute pass into 16-pixel tiles and every tile shaded with its lamps in one draw per atlas
batch. The frame matched and the driver calls went, but its pixel pass held 26 registers against the stock's 10 and
nine batches walked every tile: 0.6 ms more GPU than the stock lights.

## 6. Mods and what went

The stock accumulation shaders (`accum_spot_*`, `accum_omni_*`, `accum_base.ps`) light nothing on these renderers:
`da_light_volume.ps` shades every lamp. A lamp's projector texture (`spot_texture`) still applies. Gone from these
renderers with the stock path: its spot and point blenders and shaders (`s_accum_spot`, `s_accum_point`, a lamp's
`s_spot` / `s_point`, which `set_texture` compiled per projector), `accum_spot` / `accum_point`, the translucent shadow
fill (`phase_smap_spot_tsh`: the volumes read no translucency, the translucent casters write their depth like the
rest), the overlay's `accum_base.ps` and `accum_omni_unshadowed.ps`. The torch's warmup takes its projector texture
and its volumetric shaders.
