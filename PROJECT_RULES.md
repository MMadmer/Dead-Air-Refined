# Dead Air: Refined — working rules

This file is the single source of the project's working rules. The technical
specifications in `docs/dead-air` extend it but must not duplicate or override
the process described here.

## 1. Project scope

- Target game: Dead Air 0.98b and Dead Air Revolution II.
- Target platform: Windows x64.
- The sources of truth for behavior are the original x86 build of Dead Air and
  the meaningful changes of the unfinished Dead Air 1.0 port relative to its
  CoC base. On disputed behavior, investigate the original x86 binary,
  configuration, script, or shader first, then the semantic diff between the
  corresponding CoC and Dead Air 1.0 sources, and only then change the x64
  implementation. When a confirmed Dead Air 1.0 decision conflicts with the
  original, 1.0 behavior wins. The Dead Air 1.0 sources are not a reference as
  a whole: code inherited unchanged from CoC is not a Dead Air decision of its
  own and must not be ported into the project merely because 1.0 contains it.
- The project preserves the savegame format, the Lua API, the ABI of public
  modules, serialization, the XDB and loose `gamedata` load order, JSGME, and
  ordinary content addons.
- The project also preserves the XMS module system contract — the formats and
  behavior that mods built in XFined Editor (`D:\Games\XFined-Editor`) rely
  on: the module manifest, mount order and conflict resolution, `.ltxp`,
  `.xmlp`, string table priorities, `.xspawn`, `.xcform`, aimap append,
  overlay_visuals, the composite `game.graph`, the game mode registry, and the
  `.scov` chunks. The game and the editor evolve independently: a Dead Air
  Refined update has no right to break a mod built by any earlier editor
  version.
- The project targets a player who installs the game, picks a graphics preset,
  and plays. Customization is minimized on purpose: setup steps, optional
  downloads, and per-feature switches are not features, they are friction, and
  every one of them is a decision the project failed to make on the player's
  behalf. See section 4 for what this means for content.
- Native x86 plugins are not declared x64-compatible without a dedicated port.

## 2. Starting work

1. Read this file, the current technical specifications, and the relevant
   history of previous project tasks.
2. Check `git status`, the current branch, the last successful commit, and the
   built and deployed versions. Do not touch unrelated or foreign changes.
3. Check the available MCP servers and skills. For native behavior and
   signatures use IDA/decompilation first; for runtime facts use logs and safe
   live diagnostics.
4. Fix the batch boundaries and success criteria before changing code.
5. If system profiling or another standard operation requires UAC, request it
   from the user. Do not substitute the required test with a knowingly less
   accurate one just to avoid UAC.

## 3. Changes and compatibility

- Before any edit, first compare the affected code and behavior with the
  original x86 version through an available primary source — binary,
  configuration, script, or shader — and check the semantic diff of the
  corresponding CoC and Dead Air 1.0 sources. For bugs both checks are
  mandatory before choosing the cause and the fix; unchanged CoC code inside
  the 1.0 sources does not confirm the desired Dead Air behavior.
- The references are untouchable: the original x86 binary (including its IDA
  database), the Dead Air 1.0 sources, and the unpacked trees in `_analysis`
  are strictly read-only. Do not edit, append to, or re-save anything there.
- Dead Air 1.0 is unfinished: do not build the engine or packs from its
  sources. 1.0 serves only as a behavioral reference; buildable code lives
  exclusively in DeadAir-x64.
- If the x64 implementation diverges from the original and Dead Air 1.0 has no
  meaningful decision of its own, restore parity with the original. If a
  confirmed Dead Air 1.0 change conflicts with the original, treat 1.0
  behavior as the reference. An original solution is allowed only when the
  task explicitly requires changing that behavior, or when the defect is
  proven to exist in the chosen reference.
- Fix the cause, do not mask the symptom or disable the check.
- Do not change game mechanics, simulation frequency, image quality, draw
  distance, LOD, culling results, or world composition unless explicitly
  requested.
- Do not edit or repack the user's `database`, `gamedata`, `appdata`, `MODS`,
  JSGME state, or savegames.
- Mod compatibility is two-sided and both sides are mandatory: old mods (XDB,
  loose `gamedata`, JSGME, content addons) and new XMS mods from XFined
  Editor. Any change touching XMS contract points (module registry and
  mounting, LTX/XML/string table merge order, level deltas
  `.xspawn`/`.xcform`/aimap/overlay_visuals/`game.graph`, `.scov` chunks, the
  mode registry) is made backward-compatibly only: new capabilities are added
  as new chunk/field versions with a silent fallback, and behavior already
  shipped is neither changed nor removed. A mod built by an earlier editor
  version must keep working after any game update — this is the condition of
  the two projects' independent evolution and is not negotiable within
  optimizations or refactorings.
- Savegames must not be overwritten, deleted, or included in a managed
  installer/update payload. Any format migration requires a separate explicit
  decision and a reversible plan.
- Weapons Evolution and other third-party mods are used only as compatibility
  analysis material. Mod-specific workarounds and base game changes are
  forbidden without separate direct permission.
- Errors on the shared Lua callback/API boundary are fixed centrally in the
  existing compatibility layer. Do not add full override files of a specific
  mod's scripts when the contract or the arguments can be normalized for all
  consumers of the shared extension point.
- Create a new compatibility script only for a distinct responsibility that
  cannot reasonably live in an already existing shared compatibility script.
- Public Lua names, callbacks, action IDs, serialized enum values, and the
  sizes and field order of network and save structures are preserved.
- Do not execute Lua concurrently. Do not allow concurrent writes into one
  game object or shared scratch state without an explicit ownership model and
  barrier.
- Do not hold resources of an unloaded location for the sake of speed.
- Weak hardware and old OSes stay playable but do not cap everyone else. A
  modern platform capability (an API flag, a driver extension, an output mode,
  an instruction set) is enabled when detected at runtime; in its absence the
  code silently falls back to the previous path with no functionality loss and
  no warnings to the player. The fallback branch stays working and verifiable,
  not nominal: if it cannot produce the result, the report says so instead of
  hiding it. It is equally forbidden to require new hardware where old
  hardware coped, and to degrade new hardware to match the old.
- The renderers (`R1`/`R2`/`R2.5`/`R3`/`R4`) and output modes are supported
  simultaneously: a change in one backend must not break the build or the
  behavior of the others, and shared code stays shared.

## 4. Content and assets

- Shipped content is never optional. Once an asset — a model, texture,
  animation, sound — is part of a release, every installation of that release
  has it. There is no opt-in download, no "lite" edition, no per-feature
  content toggle, and no setting whose only purpose is to avoid fetching data.
- Assets are published as versioned, hashed bundles pinned per game version and
  obtained by the installer or by the game's repair path. An installation either
  has the complete set for its version, or it is reported as incomplete and play
  is refused until it is repaired. The incomplete latch is written before the
  first change to `database` and removed only after a fresh hash-verifying pass
  over the manifest finds nothing outstanding, so an interrupted commit cannot
  present itself as healthy. An install that cannot obtain its assets is a
  failed install, not a reduced one: the fetch runs before the installer has
  touched anything, and a failure aborts with the installation as it was.
  Assets are deliberately NOT carried inside the update archive: the applier is
  bounded at 1 GiB expanded and 1024 files and its second stage has a
  five-minute wall, none of which survive a multi-gigabyte payload. An update is
  not offered while content is incomplete — the repair comes first.
- Nothing enters `database` except by renaming a file whose SHA-256 has already
  matched the manifest. Downloads land in the content cache under the hash they
  are supposed to have, a partial file carries a `.part` suffix, and installing
  a bundle is a move. A truncated download, a wrong revision, or a delta that
  reconstructed the wrong bytes therefore cannot acquire a bundle name — not
  because a check rejects them, but because nothing writes into `database`
  directly. Any future producer of bundle bytes commits through the same rename
  or it does not commit at all.
- A bundle-shaped archive is mounted only when the installed manifest declares
  that exact name and size. A leftover revision, a truncated file, or an archive
  renamed by hand is skipped and reported instead, because content the build was
  never tested against surfaces as a missing-asset crash far from its cause. The
  gate has no opinion about any other archive: stock archives and third-party
  ones mount exactly as before.
- Content bundles are ordinary archives and get no priority. Loose `gamedata`,
  JSGME and XMS modules override them, and so does an archive that sorts after
  them — a mod overriding content is a mod working. Do not give the content
  system a privileged mount position to protect its own assets.
- A binary delta is an optimisation and never a requirement. An ineligible base,
  a missing edge, or a failed apply falls back to the full bundle silently, and
  the result is committed only after it hashes to the bundle the manifest
  declares. A delta is recorded as rejected only when its output fails that
  check with every I/O call having succeeded: a full disk, a read error, or a
  cancellation is retryable, and blacklisting one would permanently turn a small
  download into a large one for that installation.
- Add before delete. A commit installs everything the manifest declares and only
  then demotes what it no longer declares back into the cache; obsolete bundles
  are never removed as part of installing their replacements. An interrupted
  commit leaves an installation with too many bundles instead of one with too
  few: the extra ones are refused by the mount gate, reported as a notice rather
  than a fault, and demoted by the next commit — the next update, install or
  repair. Too few bundles is a broken game; too many is untidy.
- Content bundles are never managed files. They must not appear in
  `managed-files.txt`, in `runtime-files.txt`, in an update manifest, in the
  installer's file table, or in a backup scope. That list decides what is backed
  up, what is deleted when it drops out of a new version, what the updater
  snapshots, and what the uninstaller removes — and a bundle name carries the
  hash of its own bytes, so it changes whenever the content does. A bundle named
  there would be deleted from `database` the moment a version renamed it,
  outside the content commit, and no backup could restore it because the update
  manifest never declared it. It would also be copied into every backup, at
  gigabytes a time. Bundles are removed only by the content commit, the cache
  collector, and the uninstaller's own sweep, all of which delete by the
  bundle-name pattern and nothing else.
- The trust root is the installed content manifest in the control directory. It
  arrives inside a hash-verified payload and is the only thing that decides what
  an installation must contain. No manifest path is ever accepted on a command
  line, and no downloaded manifest replaces it: the assets host is pinned in
  shipped code, and the `repo=` line of a fetched manifest is informational.
  Moving the assets to another host is a code change and a release. The one
  override that exists redirects the download host to a loopback address for QA
  and cannot skip a hash, and no switch that skips content may be added beside
  it.
- Content lives outside this repository. The engine sources stay free of
  binary asset trees; released assets are published as versioned, hashed
  bundles and pinned per game version, so a version always knows exactly which
  content belongs to it. Integrity is established by hash, so the host is a
  delivery detail rather than something that has to be trusted.
- Missing or corrupt required content is a broken installation and is reported
  as one, with a repair path that re-fetches it. Silently disabling the feature
  that needed it is forbidden: it turns a fixable install into a game that is
  quietly missing parts.
- Defensive fallbacks remain correct for broken *logic* — an absent script
  bridge, a failed subsystem — and must not be reused to paper over absent
  *content*. The two failures need opposite responses: one degrades safely, the
  other demands repair.

## 5. Asynchronous saves

- The current shared main-thread budget for save preparation is `3 ms` per
  frame.
- The client part is processed in portions of at most `16` objects per step.
- ALife state capture continues on subsequent frames within the remaining
  budget; compression and atomic commit run on a background writer thread.
- The previous save file pair must survive any failure of a new transaction.
  For Refined this means the full consistent save group: the original
  `.scop`/`.scoc` and the single additional `.scov`.
- Do not extend the `.scop` and `.scoc` formats. Store all new persistent
  mechanics only as separate versioned chunks in the single `.scov`; new
  per-mechanic files are forbidden. Preserve unknown, flagged, and newer
  chunks byte-for-byte.
- The `.scov` v3 container format is frozen. Future versions add a new chunk
  ID or raise a specific chunk's version, but do not change the
  header/directory and do not downgrade an already written version.
- The save group commits atomically: companions first, `.scop` last. Crash
  recovery is allowed only from a valid durable transaction marker; orphan
  `.bak` files without a marker are not restored automatically.
- These limits and this order must not be changed as a micro-optimization. A
  change is allowed only after a dedicated profile and checks of freezes,
  save/load, level transitions, and aborted writes.

The full binary contract and evolution rules are described in
`docs/dead-air/SAVE_COMPATIBILITY.md`.

## 6. Code

- Use C++20 and language features when they do not hurt logic, compatibility,
  or performance.
- For new associative lookup containers default to the project Swiss-table
  types `xr_flat_hash_map` or `xr_node_hash_map` when safe. Keep `xr_map` when
  sorted order, address or iterator stability, a compatible public type,
  key-ordered serialization, or other `std::map` behavior is required.
- Follow the local style of the file; the base format is 4 spaces, UTF-8, a
  final empty line, and up to 120 characters per line.
- Check plain pointers with the implicit boolean conversion: `if (pointer)`
  and `if (!pointer)`, no comparisons with `nullptr`.
- Add comments only where the reason for a decision, a lifetime, a
  synchronization, or a compatibility constraint is unclear without them.
  Comments must be short and in English only.
- Do not add diagnostic code, telemetry, or temporary console commands to the
  final batch.
- The Release build must pass with the active warnings-as-errors policy.

## 7. Optimization

1. First obtain a profile on the real `xrEngine.exe` and name the specific hot
   path, function, shader, object, or wait.
2. Prioritize constant FPS losses, freezes, and broad user scenarios over
   micro-optimizations.
3. Preserve the current successful source, build, and deployed state before
   the experiment.
4. Execute a whole coherent batch. Full in-game and A/B testing happens after
   the iteration is complete, not after every small change.
5. Compare baseline/candidate under identical conditions. Treat a difference
   below 1% as noise unless another statistical justification exists.
6. For general performance QA use exactly three locations in godmode: light,
   medium, and heavy by the last measured rating. Report the chosen locations,
   save, renderer, resolution, weather, warm-up, and duration.
7. For lighting, verify indoors and outdoors separately, sources held and in
   the world, camera tilts, a source leaving the viewport, and shadow motion.
8. Check average, p50/p95/p99/max frame time, CPU/GPU bottleneck, private
   memory, handles, threads, and resource growth across level transitions.
9. Do not keep a change with a visible regression, a leak, an unstable result,
   or no reproducible win. Remove a failed experiment completely before the
   next attempt.

## 8. Build and deployment

- The only supported pipeline: CMake presets and Ninja Multi-Config. Legacy
  Visual Studio projects, XMake, and parallel alternative pipelines are not
  supported.
- The canonical clean build:

  ```powershell
  tools\build\build_x64.ps1 -Configuration Release -Clean
  ```

- For small local source edits default to the incremental build. A clean build
  is mandatory after changing CMake, the toolchain, dependencies, generated
  files, or on suspicion of stale artifacts.
- After a clean or a successful incremental build, run another incremental
  build and confirm no work or errors remain.
- Deploy the full list from
  `packaging/dead-air-x64/installer/runtime-files.txt`, not a single DLL, when
  the batch touches the shared version or dependencies.
- Before deployment make sure the game is closed. Afterwards compare the
  SHA-256 of every file from the current runtime manifest with
  `bin/x64/Release`.
- The main game installation receives only a confirmed candidate. Place test
  roots and temporary files separately and remove them after verification.

## 8.1. Player bug report handling

- Do not take reports at face value: the player's description is a symptom and
  a hypothesis, not a diagnosis. Expected behavior is checked against the
  original x86 version and Dead Air 1.0 by the rules of section 3 before
  choosing the cause and the fix; behavior matching the reference is not a bug
  and is closed with an explanation, not a change.
- Triage starts from the attached diagnostics (`session.log`, `session.dmp`,
  `user.ltx`, the save group), not from the description text. The reported
  version is checked against the fix history: an issue already fixed in a
  newer version is closed as outdated with the commit reference, without a
  repeated fix.
- Several reports with one signature are one defect: the root is fixed once
  and all duplicates are closed onto it. One report with several problems is
  split into separate defects.
- Mod script crashes are fixed by the rules of section 3: the engine contract
  or the shared compatibility layer, not an override for a specific mod.
- The outcome of every report is recorded: confirmed and fixed (commit),
  already fixed (version/commit), not reproducible, matches the reference, or
  rejected with a reason. A report is not handled without one of these
  verdicts.

## 9. Runtime QA

- Test the `xrEngine.exe` from the configured main game root, not a separate
  harness or a random binary copy.
- Do not take over the computer, move the character, or automate input.
  Allowed: hidden launch, log reading, and stopping only the process we
  started.
- A runtime test counts as successful only after the required game state is
  reached. A startup smoke must not be passed off as a level load.
- Mandatory checks by risk: an existing save, a new game, save/load, a level
  transition, UI/PDA/inventory, the relevant addon, and an active soak.
- Check the log for fatals, assertions, access violations, reader overflows,
  Lua/UI lifecycle errors, and new recurring warnings.
- After every test verify that the game, launcher, updater, debugger,
  profiler, compiler, linker, CMake, and Ninja have exited. A shell timeout
  does not mean the child processes have finished.

### 8.1. Render tests

1. Run render tests on the main Windows Desktop, not a hidden or virtual one.
   Foreground and background runs are both valid, but within one A/B the
   window visibility, focus, and minimized state must match exactly and be
   recorded with the result; compare foreground only with foreground and
   background only with background. `-always_active` does not make a mixed
   comparison valid. Do not automate input, the cursor, or character control;
   rotate the camera only through standard runtime code.
2. Create a separate `fsltx`, `appdata`, and Lua QA script. Copy the whole
   source save group into the test `savedgames` without modifying the files;
   pass the save name to `-start`, not a `.ltx` path and not a name with a
   save-file extension.
3. Launch the engine as
   `xrEngine.exe -fsltx <qa.ltx> -always_active -silent_error_mode
   -force_flushlog -r4 -start server(<save>/single/alife/load)`.
4. Attach the QA script through `[common]` in `script.ltx`. In
   `on_game_start` register `actor_on_first_update` and start measuring only
   after the first actor update and the actual level load.
5. Right after `actor_on_first_update` execute `g_pause_in_background 0`,
   `main_menu off`, and `device():pause(false)`. Then call
   `device():pause(false)` every frame until the test ends. On a loaded level
   there must be no red pause caption; the loading screen does not count for
   this check.
6. Preserve the original pitch and the initial horizontal camera angle. Rotate
   yaw only, at the given positive or negative speed; do not point the actor
   at the floor and do not change its position. For continuous rotation use
   duration `-1`, otherwise stop the rotation at the given time.
7. Before measuring, set fullscreen and `vid_mode 2560x1440`, then verify the
   physical DWM bounds of the window: origin `0,0`, size exactly `2560x1440`.
   `user.ltx` alone is not enough: DPI virtualization at Windows 150% scale
   can turn the mode into logical `1707x960`.
8. If the test `xrEngine.exe` is not at the main path, temporarily copy the
   AppCompat DPI layer of the main exe (`HIGHDPIAWARE`) for it before launch.
   After QA remove only the registry entry the test created. A measurement
   taken before the physical DWM bounds are confirmed is invalid.
9. After a short stabilization of the loaded scene, capture about 15 seconds
   of rendering; this is a guideline with reasonable variance, not a hard
   millisecond timer. Log elapsed time, yaw, and FPS at most once per second;
   limit the system profiler to the same measurement window.
10. On completion execute `quit`, wait for exit, and verify the absence of
    `xrEngine`, the launcher, debugger, `wpr`, `xperf`, `wpaexporter`,
    compiler, linker, CMake, and Ninja. Separately terminate the
    `pwsh`/`powershell` instances the test created, including those launched
    through UAC, without touching the persistent Codex and user processes. If
    the normal exit did not happen, kill only this test's PID. Remove the
    temporary QA files and fat Windows dumps.

## 10. Commits

- A commit is created after a large successful and verified batch.
- Do not commit failed experiments, temporary telemetry, profilers, logs,
  builds, or attempts being rolled back.
- Before committing: remove garbage, run `git diff --check`, review the whole
  diff, and make sure the working tree contains only the current batch.
- The commit message must briefly describe the result, not list files.
- If an attempt is abandoned, return the branch to the last successful state
  without keeping fictitious intermediate commits.

## 11. Release and updates

- The version follows SemVer and is updated simultaneously in the product
  version, packaging scripts, Inno Setup, compatibility metadata, and user
  documentation.
- A release contains alternative installation options and the user needs
  exactly one of them: `Dead-Air-Refined-VERSION-Setup.exe` is the recommended
  path and the only one that installs content on a machine that has none,
  `Dead-Air-Refined-VERSION-Update.zip` is the same runtime payload for a
  manual installation, and `Dead-Air-Refined-VERSION-Update_Patch.zip` carries
  only what changed since the previous version. None of them contains content
  bundles, so a manual archive upgrades an installation that already has its
  content and does not create one from nothing. All of them carry the content
  manifest, which is what lets an installation that was updated by hand say what
  it is missing and repair itself instead of landing in recovery.
- The full archive keeps the `Update.zip` name for the whole 1.x line: it is
  the asset every client since 1.0 looks for, and a release without it is
  invisible to every installation in the field. Beyond those, a release carries
  the content manifest `Dead-Air-Refined-VERSION-content-manifest.txt` as the
  published record of which bundles that version pins. The patch is cut only
  when there is a previous release to cut it against.
- The Update ZIP must contain an empty `appdata/savedgames`, and the manifest
  must not contain a single save file. The installer and uninstaller do not
  delete existing saves.
- Package building is a local operation. Push, tag, GitHub Release, and asset
  upload happen only after the user's direct permission.
- The content release is published BEFORE the game release: a game release whose
  content manifest points at assets that do not exist yet is broken for every
  installation made in that window. A content release with no game release after
  it is harmless — an orphan tag that is simply never referenced.
- **A published content release tag is never deleted, and no asset inside one is
  ever replaced.** Bundle and delta names are content-addressed, so replacing
  bytes under an existing name makes every installed manifest wrong and is
  unrecoverable in the field: the clients that already hold the old bytes see a
  hash mismatch they cannot repair, and the clients that fetch the new ones are
  told their manifest lies. Deleting an asset is worse, because the manifests
  that name it stay valid forever. Republish under a new tag instead. This
  applies to the game releases too — an asset a shipped client can ask for is
  not editable after publication, only superseded.
- Address all GitHub commands explicitly to the `MMadmer/Dead-Air-Refined`
  repository, and every content-asset command to
  `MMadmer/Dead-Air-Refined_Assets`; do not rely on auto-detection while an
  `upstream` exists.
- A release cannot be built without a content manifest. The packaging script
  refuses a manifest that is missing or declares no bundles, and the installer
  script refuses to compile without one, because a Setup that installs no
  content is exactly the optional-content build section 4 forbids.
- Before publishing verify a clean HEAD, the binary version, the package
  contents, hashes, install/update, and save-file preservation.
- After publishing verify the tag target, the release status, the names and
  SHA-256 of every asset, and the release list the updater actually reads:
  `repos/MMadmer/Dead-Air-Refined/releases?per_page=30`. The client picks the
  highest parseable version on that page, so the GitHub "Latest" badge is
  cosmetic for updating; a tag it cannot parse, a draft or a pre-release is
  invisible to every installed client.

### 10.1. Mandatory GitHub Release text

- Keep the release text short and only about the confirmed changes of the
  current version. Do not include internal task numbers, hashes, unconfirmed
  promises, or a long changelog.
- Use the same structure for every release: changes first, then a short
  installation instruction.
- In `Changes` list only the user-visible outcome: fixes, improvements, and
  compatibility. If a category is empty, do not add it.
- In `Installation` state explicitly that Setup, the manual ZIP and the patch
  ZIP are alternatives and only one is needed; the ZIP is allowed for manual
  installation. Warn separately that existing saves must not be clobbered.
- Before publishing run a clean build if one has not been done yet, and verify
  the version number, the asset names, and the text against the actually built
  packages.
- Any release whose content set changed must state, in `Installation`, the size
  of the content download and the free space Setup will require. The packaging
  README tells the player to look for that number on the release page, so a
  release that omits it leaves the one figure that decides whether the install
  can succeed nowhere at all. Take it from the `ContentBytes` the installer
  build prints - the sum of every bundle the manifest declares plus a tenth for
  filesystem overhead, which is exactly the bar Setup checks the volume against
  before it spends any bandwidth.

Release body template:

`Theme` (`## Theme` / `## Тема`, one short headline the update dialog prints
in bold above the change list) exists in the client from 1.4.0 on, but **must
not be used while 1.3.x installations are still updating**: their changelog
parser stops at any `## ` heading other than `## Changes` that follows the
language heading, so a theme placed before the changes leaves the 1.3.x update
dialog with an empty change list (the 1.4.0 release shipped that way and had
to be edited in place). Until that generation is gone, a release body carries
no theme section at all - and if one is ever added, it goes AFTER the change
list, never before it.

```markdown
## EN

## Changes

* [Short user-visible fix or improvement]
* [Short user-visible fix or improvement]

## Installation

Choose one option:

* **Setup** — recommended guided installation.
* **Update ZIP** — the complete payload for manual installation.
* **Update_Patch ZIP** — only what changed since the previous version; the
  in-game updater picks it automatically when it fits.

Setup downloads [N] GB of game content and needs [N] GB free on the target
drive. Do not install both. Existing saves are preserved.
---
## RU

## Изменения

* [Краткое описание исправления или улучшения, заметного пользователю]
* [Краткое описание исправления или улучшения, заметного пользователю]

## Установка

Выберите один вариант:

* **Setup** — рекомендуемая пошаговая установка.
* **Update ZIP** — полный архив для ручной установки.
* **Update_Patch ZIP** — только изменения относительно прошлой версии;
  встроенное обновление выбирает его само, когда он подходит.

Установщик загружает [N] ГБ игрового контента и требует [N] ГБ свободного
места на выбранном диске. Не устанавливайте оба варианта. Существующие сохранения
будут сохранены.
```

## 12. Documentation

- The README and packaging README contain only current user information.
  Forbidding manual archive installation is not allowed; saying what the manual
  archive does not carry — content bundles and the content manifest — and what
  that means for a first-time installation is required.
- `docs/dead-air` holds only active technical specifications, currently open
  problems, and the release validation matrix.
- Finished plans, temporary iteration reports, local absolute paths, old
  binary hashes, and already closed task lists do not remain working
  documentation. The needed shared conclusion is moved here or into the
  current specification, after which the temporary document is deleted.
- When a contract changes, update the corresponding specification and the
  README references in the same batch.

## 13. Active technical specifications

- `docs/dead-air/AUTO_UPDATE.md` — release/update protocol.
- `docs/dead-air/CONTENT_BUNDLES.md` — the content bundle, manifest, delta and
  repair contract.
- `docs/dead-air/DEPENDENCIES.md` — pinned dependencies and build policy.
- `docs/dead-air/DIAGNOSTIC_REPORTS.md` — report schema and privacy contract.
- `docs/dead-air/MODDING.md` — compatibility contract for addons.
- `docs/dead-air/NQ_RUNTIME.md` — the `.nqasset` format and NQ runtime contract.
- `docs/dead-air/SAVE_COMPATIBILITY.md` — the binary save-group contract.
- `docs/dead-air/TEMPORAL_UPSCALER_RFC.md` — an open proposal, not implemented.
- `docs/dead-air/TEST_MATRIX.md` — mandatory release gates.
- `docs/dead-air/UPSTREAM.md` — provenance and attribution.
- `docs/dead-air/x64-parity-open-issues.md` — genuinely open problems only.
