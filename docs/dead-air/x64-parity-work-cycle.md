# Dead Air x64 parity work cycle

## Required workflow

1. Audit the original Dead Air x86 binary, shipped configs, scripts, UI, and
   runtime evidence for missing or changed mechanics.
2. Write every unresolved discrepancy to `x64-parity-open-issues.md`.
3. Fix the complete current batch before starting another audit batch.
4. Build the x64 runtime and verify the batch against the original behavior.
5. Keep failed or unverified items in the open-issues file.
6. Once the whole batch passes, clear its issue list and record the evidence
   below.
7. Start the next audit from a different parity surface and repeat.
8. Complete at least five verified cycles before declaring the port complete.
9. Stop every hidden game, launcher, compiler, and test process after its test.
10. Do not require UAC or user interaction during autonomous testing.
11. Preserve the standard `gamedata`, XDB, Lua, JSGME, and addon pipelines.
12. Do not hardcode input keys; use the original action binding pipeline.
13. Validate Russian text through the original Windows-1251 loading path.
14. Treat a visible fatal dialog or Lua error as a failed test even when the
    process remains responsive.

## Audit surfaces

Use a different primary surface for each required cycle:

1. Input, menus, UI XML, localization, and window management.
2. Inventory, equipment slots, loot, trade, restrictions, and saves.
3. Weapons, ammunition, aiming, HUD transforms, sounds, and damage callbacks.
4. World simulation, anomalies, detectors, weather, rendering, and particles.
5. Lua bindings, callbacks, config keys, serialization, addon compatibility,
   and long-session stability.

## Cycle log

### Cycle 1: input, menus, UI, localization, and window management

Verified:

- The early detached-worktree bisection made the weapon pump/fire-mode changes
  appear causal because they changed code layout and timing. The actual
  zero-argument Lua failure was later traced to the invalid on-demand rain
  luminance sample and fixed in cycle 2.
- `snd_pump` uses the original non-exclusive flag and x86 registration order.
  Fire-mode changes respect failure bit `0x08000000`.
- The XDB compatibility payload was packed and unpacked with AXRToolset; its
  Russian strings and XML declaration decode correctly as Windows-1251.
- Escape routing, cursor preservation, actor flashlight omni suppression, and
  every magazine/shotgun ammunition lookup were checked against the x86 paths.
- Every hidden game, launcher, compiler, and linker process was stopped after
  the tests.

### Cycle 2: inventory, equipment, loot, trade, restrictions, and saves

Verified:

- `CInventory` now follows the x86 sequential
  `slot_persistent_N`/`slot_active_N` pair contract instead of the generic
  `slots_count` path.
- Trade condition pricing requires the original `buy_condition_koeff` key.
  Generic OpenXRay slot actions were removed, and the original optional
  backpack condition bar was restored.
- `CEffect_Rain` stores and clamps rain volume during `OnFrame`; the Lua getter
  returns the stable stored number instead of sampling an unready hemi cube.
- The x86 safe-map add/remove routines at `0x101B0DA0` and `0x101B14E0` were
  confirmed to ignore duplicate or missing entries. The x64 registry now does
  the same, eliminating the NPC teleport fatal without suppressing unrelated
  assertions.
- D3D deferred texture upload again uses Dead Air's flat texture list and
  dedicated joinable chunks of 100 rather than executing texture loads on the
  caller through the generic task scheduler.
- The unattended QA mode initializes `rsAlwaysActive` before `user.ltx` and
  keeps worker tasks active when that explicit mode is selected. Normal
  foreground behavior is unchanged.
- `cycle2-final-clean-20260726-180603.stdout.log` completed with zero errors.
  The clean binaries loaded `admin - quicksave9`, restored the actor and
  inventory-sort UI, and ran for 70 seconds with no fatal, assertion,
  safe-map error, rain/Lua `nil`, or leftover QA diagnostics.
- Every game, launcher, compiler, and linker process was stopped after the
  verification.

### Cycle 3: weapons, ammunition, aiming, HUD, sounds, and damage

Verified:

- The HUD FOV path again uses the x86 split between the user setting
  `psHUD_FOV_def` and the renderer ratio `psHUD_FOV`. The active first-person
  weapon computes the smoothed near-wall value, and the actor camera converts
  it with `value * 100 / Device.fFOV` every frame.
- The weapon parameter panel restores Dead Air's reliability comparison,
  clamped and color-coded condition value, `loaded / magazine` display, active
  ammunition type, and localized descriptions for all 32 mechanical defect
  bits.
- Mechanical defect generation now follows the x86 actor-only path. NPC
  weapons no longer acquire the player's random condition failures.
- The optional `snd_silncer_shot_actor` entry is loaded with the original
  layered-sound flags. Belt-only ammunition, pump sounds, fire-mode failures,
  and stalker hit callbacks were separately verified against their x86
  functions and retained.
- `cycle3-clean-20260726-183921.stdout.log` rebuilt the complete x64 target
  from clean intermediates with zero errors. The resulting binaries loaded
  `admin - quicksave9`, restored the actor and inventory-sort UI, and remained
  stable for more than a minute with no fatal or assertion.
- The existing `bind_gr_gun.script:568` preload error was not introduced by
  this batch and remains reserved for the Lua compatibility audit.
- Every game, launcher, compiler, and linker process was stopped after the
  verification.

### Cycle 4: world simulation, anomalies, detectors, weather, rendering, and particles

Verified:

- Detector configuration again uses Dead Air's required contract for both
  artefact radii, every light property, the light animator, and both particle
  properties. Incomplete addon detector sections now fail like x86 instead of
  silently receiving unrelated OpenXRay defaults.
- Detector spawn no longer forces an extra full skeleton calculation before
  resolving configured effect bones. World particles retain the original
  non-empty name guard.
- HUD detector particles resolve `particles` from the item's actual section
  when the detector enters its idle HUD state, matching inherited and
  runtime-overridden x86 section behavior.
- Flashlight switching no longer plays the optional OpenXRay
  `SndTurnOn`/`SndTurnOff` aliases. Actor omni-light suppression, glow state,
  and trace-bone visibility remain identical to the x86 switch function.
- Custom-zone volumetric idle and blowout lights, detector animated BGR color,
  actor outfit backpack availability, rain volume storage, deferred texture
  chunks, and HUD-particle FOV were separately compared with their x86 paths
  and retained.
- `cycle4-clean-20260726-190142.stdout.log` rebuilt the complete x64 target
  from clean intermediates with zero errors and the same 50 external
  missing-PDB linker warnings. The tested `xrGame.dll` SHA-256 is
  `BA97F71F7429FAF4AF55759A5421502E3E8A151073D178576D5D76B77ECC7648`.
- The clean binaries loaded `admin - quicksave9`, restored the actor and
  inventory-sort UI, completed synchronization, and remained responsive for
  more than 80 seconds with no fatal, assertion, detector-config failure, or
  rendering failure.
- The existing `bind_gr_gun.script:568` preload error is isolated to the next
  Lua compatibility batch and was not treated as a pass for that batch.
- Every game, launcher, compiler, and linker process was stopped after the
  verification.

### Cycle 5: Lua bindings, configs, serialization, addons, and long-session stability

Verified:

- The complete Dead Air action range retains its original numeric ABI from
  `kLEFT = 0` through `kPDA_TAB6 = 89`. OpenXRay extensions begin at 90, and
  the runtime action table contains exactly 151 rows in matching ID order.
- `kWPN_7` is restored at ID 28 in the engine table and Lua export. Its
  inventory action toggles the original binocular slot 14, so the shipped
  `bind wpn_7 kG` and addons using the legacy ID work through the normal
  binding pipeline.
- All 12 missing DirectInput-era config key names and 14 missing Lua `DIK`
  aliases map to their SDL equivalents. `DIK_PGUP` and `DIK_PGDN` also map to
  their correct physical keys.
- The gravity-gun binder fastcall is attached to `gravi_gun_binder` instead of
  the nonexistent `ggun_binder`. The tracked UTF-8 source is converted
  deterministically to Windows-1251 while packing the compatibility XDB.
- `xtra_dead_air_x64.xdb0` packed and unpacked five files successfully. The
  verified gravity-gun script rejects strict UTF-8, decodes as Windows-1251,
  retains its Russian text, and contains only the corrected fastcall method.
  The archive SHA-256 is
  `E7B285AE398C085FF8B9FFD999E056F66318600263C615F6A4F186168FC1D4F9`.
- `cycle5-clean-20260726-193005.stdout.log` rebuilt the complete x64 target
  with zero errors and the same 50 external warnings. The ordered action-table
  correction then rebuilt incrementally with zero errors in
  `cycle5-incremental-20260726-194029.stdout.log`.
- The final runtime loaded `admin - quicksave9`, restored the actor and
  inventory-sort UI, and remained responsive for 620 seconds. Its 1,479-line
  log has SHA-256
  `78658E8AD29ECA86581C0729588E2A1EC4A34C4DA641C2EE34D5573D0420A6EC`
  and contains no fatal, assertion, stack trace, Lua runtime error,
  `ActionNameToPtr`, or `KeynameToPtr` failure.
- The tested runtime hashes are
  `F76FB9513AC864C255EC5F9565D6F3DD5C69C385E2EA4783A30DA55FD721170C`
  for `xrEngine.exe`,
  `17B7626CD0CFECF86749375769DF5D6F1C919786F32CAE05443E2B6D1A000E20`
  for `xrEngine.dll`, and
  `33DA3566FCE839384D714226D0EF0C63065C6C6E27C5C37B5A068156166FD4D8`
  for `xrGame.dll`.
- Every game, launcher, compiler, and linker process was stopped after the
  verification.

### Final cross-surface audit and delivery verification

Verified:

- The final physical-key audit restored the original `kAT` and `kAX` aliases.
  Mouse buttons 6 through 8 are represented in the SDL input state, config
  name table, Lua exports, and bounded event remap table. Legacy config input
  names therefore have no remaining gaps against the original x86 strings.
- The final clean build log
  `final-clean-20260726-200141.stdout.log` contains zero build errors and the
  same 50 external missing-PDB warnings. The final binary SHA-256 values are
  `819ED0BED51BEFA9D0661466C868C0508E5E6E049D0ACD3D03F5269B3ED105C0`
  for `xrEngine.exe`,
  `27F5866B77BF4D70C831FD2D87504C6F50CD7F727195F8AAE9BEB55AB39A863D`
  for `xrEngine.dll`, and
  `51455ABCECC914E02CE7D596697A791B23FF4763A607615DF783845B11926FCF`
  for `xrGame.dll`.
- A clean standalone runtime accepted temporary bindings for `kAT`, `kAX`,
  and `mouse8`, loaded `x64_da_newgame`, and remained responsive for 148
  seconds. Its log SHA-256 is
  `E077C17FF5FB437A89D0BE21EE8594AB0A17EC176ED7D63E63E330E2355A1E9E`
  and contains no fatal, assertion, Lua, action-name, or key-name failure.
- The original x86 binary-string ABI audit found zero missing symbols across
  all 257 legacy `k`/`DIK` names and zero missing identifiers across the 5,623
  lowercase config-like strings used for the broader compatibility check.
- The final compatibility archive SHA-256 is
  `E7B285AE398C085FF8B9FFD999E056F66318600263C615F6A4F186168FC1D4F9`.
  The final installer package rebuilt without errors, all eight published
  checksum entries match, and the 10,520,061,865-byte Zip64 archive passed
  `7z t`. Its SHA-256 is
  `15629A8F103AEC51CCD60D8DACDF8F10F108BC9ABF9EF1BB1767F9EC2BBC09DA`.
- Upgrade installation over an isolated original x86 root installed all 42
  x64 runtime files with the final hashes. Its independent snapshot preserved
  13 pre-existing runtime files across a 43-file restore scope.
- Rollback restored every present snapshot file byte-for-byte, removed every
  x64-only file in the restore scope, retained the selected snapshot, and left
  the maintenance uninstaller available. The verification found zero
  mismatches.
- Reinstalling in upgrade mode with backup disabled did not create another
  snapshot. Removing the patch deleted all 42 x64 runtime files and the
  compatibility archive while preserving the original `configs.xdb0`,
  `fsgame.ltx`, and the earlier independent backup.
- A clean standalone installation matched all 42 runtime files, all 14 base
  archives, `fsgame.ltx`, and the compatibility archive by SHA-256. The
  installed executable loaded `x64_da_newgame`, accepted two Escape presses,
  and remained responsive for 120 seconds with zero fatal, assertion, Lua,
  action-name, key-name, or reader-overflow failures. The 1,343-line runtime
  log SHA-256 is
  `5D425106FAE3C93846EDB9DC1AFF89F56E49B49BED0F681B7993EE4AAE372FF8`.
- Removing the standalone installation deleted the runtime and all 14 base
  archives while preserving the test `appdata` directory and its saved game.
- Every game, setup, uninstaller, launcher, compiler, and linker process was
  stopped after the final verification.
