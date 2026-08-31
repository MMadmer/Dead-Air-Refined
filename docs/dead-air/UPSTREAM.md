# Source lineage and attribution

Dead Air: Refined is an independent derivative project maintained by MMadmer.
It is not presented as an engine implementation created from an empty codebase.

## Engine foundation

The engine foundation originates from the
[OpenXRay `xray-16`](https://github.com/OpenXRay/xray-16) project. Development
of the Dead Air port began from upstream commit:

```text
29030f81b137f6ea5365b3d71f2b588490832f5b
```

The corresponding upstream source and its complete authorship history remain
available at:

```text
https://github.com/OpenXRay/xray-16
```

The local Git remote named `upstream` points to this repository.

## Dead Air source reference

Lanforse provided the surviving unfinished Dead Air 1.0 source tree together
with its matching early CoC x64 engine baseline. Refined used the pair as a
comparative reference to isolate Dead Air-authored changes, with the released
0.98b binary/decompilation retained as the compatibility authority. The CoC
x64 tree was not imported as Refined's engine foundation.

## Refined history

The public Dead Air: Refined history records project-specific release states.
It does not duplicate thousands of upstream commits as if they were changes
made directly to Refined. This keeps the Refined contributor list scoped to
people who contribute to this project while retaining explicit and verifiable
source provenance.

## Reference integrations from DeadAir-x64/DeadAir-Engine-x64-OpenSource

The independent x64 port at
`https://github.com/DeadAir-x64/DeadAir-Engine-x64-OpenSource` (MIT-licensed
OpenXRay derivative) was audited at commit
`50316f59ad62f595e7d5c6bd2f6ea025027d8285`, and selected solutions were
re-implemented in Refined in target-native form (designs adapted, code
rewritten, comments and diagnostics replaced; nothing was merged wholesale):

- fixed-map node destruction on storage growth — from `f2a431a3`
  (`src/xrCore/Containers/FixedMap.h`);
- patrol-path alias ownership and duplicate-name release — from `f2a431a3`
  (`src/xrAICore/Navigation/PatrolPath/patrol_path_storage.*`);
- invalid physics pose containment in bone callbacks — from `b0b6119b`
  (`src/xrPhysics/PHElement.*`), diagnostics reworked to per-element state;
- local shadow-light budget with actor-torch privilege and admission
  hysteresis — from reference HEAD `r2_R_render.cpp`/`Torch.cpp`, with an
  unshadowed-export demotion layout; admission is per shadow-map face, so a
  shadowed point light spends six slots; parity default
  (`r__light_shadow_budget 0`);
- middle/far sun cascade cache — from `06654c8a`
  (`render_phase_sun.cpp`), parity default (`r__sun_cache_ms 0`);
- loose per-particle overrides through the VFS — from `48f732bb`
  (`src/Layers/xrRender/PSLibrary.*`), no default content shipped;
- Lua call audit tooling concept — from `00e210a8`, re-implemented as
  `tools/compat/lua_call_audit.py` on top of Refined's lua_help inventory;
- shader-script exception containment — from `c6ac5075`
  (`ResourceManager_Scripting.cpp` family, new `ResourceManager_ScriptGuard.h`);
- 3D fluid volume level ownership — from `c6ac5075` (`r2_loader.cpp`, `r2.h`);
- lazy thread-safe space-restriction borders — from `e5f89e48`
  (`space_restriction_*`), composition-initialize and inside() fixes added;
- exact indexed nearest level-graph vertex — from `e5f89e48`
  (`level_graph.*`), CSR index with load-time validation and a stricter bound;
- character description cache prewarm — from `825af97c` (`xrgame_dll_detach.cpp`);
- sustained path-failure backoff — from reference HEAD `level_path_builder.h`,
  with two reference defects fixed (overlong suppression, wait-flag stall);
- NPC post-combat hot paths — from `633aed54`: distant foot-IK gate
  (`ph_ik_dist`, parity default 0), dead-object visibility throttle
  (`ai_dead_vision_ms`, authorized default 1000 ms), exact visual-memory
  config cap (restores 0.98b parity); squad distribution dedupe rejected as
  not behavior-preserving;
- deferred ALife release by ID — from `c025a7db`
  (`alife_switch_manager.*`, `alife_update_manager.cpp`,
  `alife_simulator_script.cpp`);
- ALife/client hardening safe subset — from `80311074`: level-changer
  cross-table guard, squad roster packet bounds, net_Unregister ID-reuse
  check, camera-effector removal reentrancy, attachment-owner bone/visual
  guards; the reference's `m_da_alive` liveness marker and periodic registry
  sanitizer are rejected by design;
- script bone and monster community guards — from `49c3dde0`
  (`script_game_object.cpp`, `monster_community.cpp`), community fallback
  kept at the documented no-community state instead of the reference's
  first-known-community substitution;
- hardware grass instancing — from `a262ee42` (`DetailManager_VS.cpp`,
  `dx11DetailManager_VS.cpp`, detail vertex shaders): one stored mesh copy
  replicated by DrawIndexedInstanced instead of 61 baked copies; shaders
  rebuilt on the vanilla Dead Air base extracted from `configs.xdb0`;
- OpenGL renderer removal — layout from `71011c8d` (build/link/module
  plumbing only, sources retained in tree);
- local-light visibility stabilization — from `a2373dfe`
  (`light_vis.cpp`, `light.h`): screen-edge-clipped volumes are treated
  as visible instead of queried (edge-clipped occlusion queries report
  false zeroes), OMNIPART faces measure the parent point-light sphere,
  and a miss-streak hysteresis extinguishes a light only after three
  consecutive negative queries; the reference's water stencil rework and
  its shadow-visibility begin(graph_id) removal are not applicable here;
- temporal upscaler renderer — documented as an RFC only
  (`docs/dead-air/TEMPORAL_UPSCALER_RFC.md`), no code adapted.

Additional adaptations recorded in the reference-integration final report are
attributed the same way: design from the named reference commit,
implementation rewritten for Refined.

## Content asset provenance

Since 1.4.0 the binary assets Refined authors or borrows no longer live in this
repository. They are published as content bundles from
`https://github.com/MMadmer/Dead-Air-Refined_Assets` and are authored from
`source/gamedata/` in that repository's working copy. Moving them out did not
move their provenance, which is recorded here, because this is the file that
owns provenance for everything Refined ships.

The 1.4.0 content set is 40 files:

- **The 3D PDA** — `meshes/dynamics/devices/dev_pda/`,
  `meshes/dynamics/weapons/` (the PDA hands rig and its `.omf`),
  `anims/camera_effects/weapon/pda_*.anm`, `sounds/items/pda/`,
  `sounds/detectorshud/detector_draw.ogg`, `textures/item/item_kpk*`,
  `textures/act/act_arm_3.*`, and the `textures/ui/ui_deadpda*` and
  `textures/ui/ui_pda_loadscreen*` sequences. All from the Gunslinger mod,
  taken as assets and re-authored into this engine's shader dialect; the port
  itself is described in the 3D PDA work and its shaders are ours. Attribution
  to the Gunslinger team stands in the project README's credits.
- **The vegetation rustle voices** — `sounds/dead_air_x64/grass_rustle.ogg`
  and `leaves_rustle.ogg`, added in `59fa0baed`, recorded there only as
  "royalty-free". **That is not a provenance record.** No source, licence text
  or licence URL was kept, so nobody can now say which royalty-free library
  they came from or what its terms require. This was tolerable while they sat
  inside an engine archive; it is not tolerable now that they are published as
  standalone release assets in their own repository. Either the original source
  and its licence are found and recorded in this section, or the two files are
  replaced with recordings whose terms are known. Until one of those happens,
  treat this as an open item, not as cleared.

Everything else that ships is either the base game's own data, the
compatibility archive's configs and shaders (which are ours and stay in this
repository because they are version-coupled to the engine), or content the
player already had.

## Attribution

Original copyright notices, dependency licenses, submodule histories, and
third-party acknowledgements remain part of the source distribution. The
project README also identifies OpenXRay as the engine foundation and credits
the Dead Air developers and community, including Lanforse for preserving and
sharing the surviving source reference.
