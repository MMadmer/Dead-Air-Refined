# Dead Air x64 validation matrix

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Clean x64 build | `Release`, clean intermediate tree | Pass, zero build errors |
| PE architecture | 43 packaged EXE/DLL files: 40 runtime files and 3 update/maintenance programs | Pass, all AMD64 |
| XDB discovery | Existing `database/*.xdb*`; discovery is conditional for names matching the content bundle shape | Pass, stock and third-party archives mount unchanged, and a bundle-shaped name mounts only when the installed content manifest declares that exact name at that exact size |
| Loose override discovery | Existing `gamedata` | Pass |
| Clean Dead Air new game | Base archives, no DAR2 archives | Pass |
| Clean Dead Air x64 save/reload | `x64_da_newgame` | Pass |
| Existing DAR2 x86 save | Latest `admin - quicksave9` | Pass |
| Original 0.98b pair without `.scov` | `legacy098.scop/.scoc`, exact x86-reference bytes | Pass, 27,634 objects loaded; new Refined save committed and loaded in a separate process; original hashes unchanged and no sidecar added |
| Refined save in original 0.98b | Isolated original x86 runtime and Refined-generated `serializer_ab_01` | Pass for load, 22,958 spawn points / 27,625 objects; the original renderer stalled after load before it could perform the requested resave |
| Lua save API compatibility | Ten legacy `before_save` mutations plus synchronous `capture_encode == false` | Pass, exact `.scoc` results, failed capture preserved the previous trio, zero transaction residue |
| XMS zero-module identity | XMS enabled, `mods/` with no valid manifest, new game | Pass, 22,958 spawn points (baseline) and not a single XMS log line — composition is a true no-op |
| XMS module composition | `xms.sample` enabled, new game | Pass, 22,959 spawn points (+1 from `demo.xspawn`), game graph 5,768 vertices (5,528 base), module level `xms_proba` joined as id 128 |
| XMS visual overlays | `overlay_visuals.ltx` with a static, a hierarchy, a mode-gated and a missing entry on `fake_start` | Pass, 2 attached, mode-gated entry skipped, missing file warned; graceful shutdown detached them without an assert |
| Malformed extension quickload | Valid active `l01_escape` world plus structurally invalid target `.scov` | Pass, load rejected before broadcast; level and actor ID remained unchanged |
| Save transaction fault models | 33 deterministic I/O/fallback cases and 10 durable crash checkpoints | Pass, production ordering verifier matched the implementation |
| NQ quest runtime in game | `tools\qa\nq\Run-NqQa.ps1 -Scenario All`, three reference quests plus two broken assets as a module in an isolated QA root | Pass, 9/9 scenarios, 83 checks, zero failures; the player's `savedgames` manifest is unchanged. `Dialog` drives the real talk window on the hidden desktop through the reply accelerator keys, so the engine walks its own phrase graph (`0>wolf_1>ask_more>wolf_rich>back>wolf_1>accept`) |
| Packed addon | DA Inventory Sort XDB | Pass |
| Loose addon | DAR2 Oxygen HUD scripts/UI | Pass |
| Lua binding parity | Original x86 exports vs x64 exports | Pass, zero missing |
| First-person self-shadow | Maximum preset, `renderer_r4` and `renderer_r3`, an outdoor save with an item in hand | Pass, `$user$smap_hud` created and `hud_shadow.ps` compiled on both renderers; the shadow term follows the model (lit top of the head, shadowed underside), the darkening lands only on HUD pixels and the background is untouched; `_preset Extreme` reports `r__hud_shadow 1` and `_preset High` reports `0` |
| Player world shadow | `r__actor_shadow` on, first person, a save with an item in hand, sun or a lamp overhead | Pass only when the caster is the full third-person model - head, arms and the held item all present in the silhouette - the shadow follows the player's stance with no detached item hanging in the air, the caster is gone from memory while the option is off, and the player's own torch is not shadowed by the body carrying it |
| Stale server-object handle | A save with squads, release one from a script and keep the handle | Pass only when every guarded accessor answers a neutral value - npc_count 0, commander_id 65535, squad_members empty, mutators ignored - logs the refusal with the script stack, and the session keeps running |
| Screen-space sun shafts | R4 renderer | Pass, all shader stages compiled |
| FXAA | R4 renderer | Pass, both shader stages compiled |
| Visor drops and reflections | R4 renderer, `r2_lenswater` and `r2_lensdirt` enabled | Pass only when `combine_2_naa` compiles without fallback shaders and the world remains visible |
| PDA script tabs | Widescreen PDA, relations tab and one native tab | Pass only when text placement remains stable, tabs accept input, and no Lua/UI lifecycle error is logged |
| Debug script tabs | Debug mode, spawner plus every available non-spawner tab | Pass only when each tab is created, drawn, and switched without Lua/UI lifecycle errors |
| Outfit and helmet night vision | Equipped NV-capable outfit and helmet, R4 renderer | Pass only when both activation paths start and stop the configured post-process effector |
| Wind service replay | `wind_seed N` twice on the same save, `wind_dbg 1` | Pass only when both logs print the same `[wind]` lines tick for tick, and `wind_force 1.0` pins the base at 1.00 within one tick |
| Tree crown oscillator | Outdoor save, `wind_dbg 1`, a gust event | Pass only when the watched tree's `[wind-tree]` line shows q overshooting the target and ringing down (damped, never clamped at the target), and the shadow of the crown moves with it on every preset but Minimum |
| Cloud deck and its shadow | Midday save, `r__clouds_quality 1` then `3`, `r__clouds_cover 0.25/0.55/0.85` | Pass only when the deck reads as masses with edges (not a haze), the ground shows the shadows of the clouds overhead, the shafts break where the deck is dense, no square edge of the cloud map shows in the sky, and tier 3 costs under 3 ms at 1440p on the rig |
| Wind on particles, bodies and water | Smoke source, a dropped can, a lake in a `storm` weather | Pass only when the smoke leans downwind and returns when the wind drops, a resting can stays put in a breeze and rolls in a gale, and the lake shows streaks along the heading that a `clear` weather does not |
| Third-party script compatibility | Binding parity fixture plus a representative packed or loose addon | Pass only when the addon starts without missing export, signature, or Lua ownership errors |
| Level transition | Underground to Agroprom | Pass |
| Transition-save reload | Fresh underground and Agroprom saves | Pass |
| Long-session soak | Fresh Agroprom save, packaged runtime | Pass, 15.29 minutes, responsive, 3.04 GiB private memory |
| Dependency audit | 43-file release package | Pass, no missing local DLLs |
| GUI upgrade installer | Final `Dead Air: Refined 1.1.1` package over the installed 1.1.0 root | Pass, all 46 managed payload hashes matched and 75 saves remained unchanged |
| Main-menu AtmosFear apply | `af_options_dialog:OnBtnAccept()` with no active level | Pass, settings saved without calling the runtime weather manager; process remained responsive without Lua or access-violation errors |
| Installed EXE uninstaller | Upgrade mode | Pass, 13 original runtime files restored across a 43-file scope with zero mismatches |
| Maintenance wizard | Installer-style fixed-size layout and silent action paths | Pass, standard header artwork persisted and the removal path completed without leftovers |
| Patch-only distribution | Installer contents and target validation | Pass, no original Dead Air files included and an empty target is rejected |
| Raw update archive | Manifest schema, exact file set, per-file size and SHA-256 | Pass, 51 ZIP entries and 46 payload files including maintenance and uninstall components |
| Saved-game preservation | Fresh install without `appdata` plus repeat install with an existing save sentinel | Pass, `appdata\savedgames` created and sentinel SHA-256 unchanged |
| External updater | Isolated installed root, backup, apply, maintenance, cleanup | Pass, in-flight snapshot created, obsolete sentinel removed, cache deleted, uninstaller regenerated. The snapshot is a transaction rollback for the update that is running and is deleted once the apply succeeds, so nothing is retained afterwards |
| Empty release list | Isolated update response with no valid stable release | Pass, accepted without an update candidate or blocking the menu |
| Final package smoke | Latest x86 save, 22,958 spawn points / 27,198 objects | Pass |
| Manual diagnostic report | Loaded `async_benchmark_1`, hidden desktop | Pass, valid anonymous ZIP with system, hardware, runtime, content, log, configuration, and minidump data |
| Automatic crash report | Gated release QA access violation | Pass, exception code, module RVA, anonymous stack, and valid `MDMP` attachment captured |
| Crash-report startup prompt | Newest unhandled `dar-report-crash-*.zip` plus a mocked 1.0.5 update | Pass only when the native yes/no confirmation opens first, yes opens the `Отправка crash report` form, and that exact ZIP remains mandatory |
| Crash-report acknowledgement | Handled marker followed by a second menu launch | Pass, the same ZIP did not prompt again and the deferred 1.0.5 update dialog opened |
| Diagnostic privacy scan | All non-save ZIP entries, ASCII and UTF-16 | Pass, no user name, computer name, profile path, game path, e-mail address, or IP address |
| Diagnostic rotation | 13 pre-existing reports plus one new report | Pass, newest report retained and total reduced to 10 |
| Native bug-report layouts | 4:3 and widescreen XML, all child bounds checked against the panel | Pass, zero out-of-panel controls |
| Bug-report upload contract | Live Report Hub, multipart title/description plus valid anonymous diagnostic ZIP | Pass, HTTP 201, attachment accepted, QA reports deleted |

Version rollback has no rows here any more because it no longer exists. The
updater deletes its snapshot as soon as an apply succeeds, and the uninstaller
no longer offers a restore-a-Refined-version action. Restoring the original
32-bit game from `backup-x86` is unaffected and is still covered above.

## Content system

The mount-gate cases run through `tools\qa\Run-ContentProbe.ps1`, which appends
console commands to the QA rig's `user.ltx`, boots the shipped engine and reads
the log back. Nothing is stubbed: filesystem initialisation, the mount gate and
`ContentService::Initialize` all run on a real installation. Every case also
fails on a `stack trace` or `FATAL ERROR` line, and the harness fails if the rig
is left latched. Play refusal is always driven through
`start server(all/single/alife/new)` issued from `user.ltx`, which is the route
the project's own smoke recipe uses and the one a menu-button gate would miss.

The flow cases run the shipped `DeadAirUpdater.exe` in its three content modes
against `tools\qa\Start-ContentAssetMock.ps1` on loopback. The mock is a raw
socket server so it can drop a connection mid-body and ignore `Range` on demand.
`DAR_QA_CONTENT_BASE` redirects the downloader to it and is accepted only for a
`http://` loopback host with no credentials, so a manifest or an environment
cannot point a shipped build at an arbitrary server.

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Mount gate, healthy installation | `Test-ContentGate.ps1`, QA rig, all five declared bundles correct | Pass, `5 bundle(s) present`, nothing skipped, no play refusal |
| Warm state cache | Second launch of the same installation | Pass, `5 bundle(s), 0 hashed` - every verdict came from `content-state.txt` on a matching name, size and mtime, and no bundle was read |
| Missing bundle | One bundle moved aside | Pass, reported missing, the latch is written and the level load is refused with `Cannot start a level` |
| Truncated bundle | 4 KiB removed from the tail, name unchanged | Pass, refused before the archive is opened as `size does not match the manifest` with the declared size named, play refused, no assert |
| Corrupt index at the correct size | Index chunk length overwritten, size unchanged | Pass, refused as `invalid index` before the header is parsed, play refused, and the archive reader never asserts |
| Flipped data byte | One byte inverted at 80 % of the file; size, index and header intact | Pass, mounts and survives the stat pass, and is caught only by the hash pass as `does not match the manifest` - which is why the hash pass exists |
| Stale and unrecognised leftovers | A copy under a wrong hash in a slot the manifest knows, plus a copy in a group it does not have | Pass, `stale bundle` and `unrecognised bundle-shaped archive` respectively; both are refused and reported as notices, and neither blocks play |
| Absent manifest | `content-manifest.txt` moved aside | Pass, `manifest unavailable`, `state=recovery`, play refused - not a silent boot with every bundle unmounted |
| Leftover latch on an intact installation | Hand-written `content-incomplete.txt` on a correct installation | Pass, startup reports the latch and keeps the gate shut, and the background hash pass clears it without a restart |
| Clean fetch | `Test-ContentFlow.ps1`, `--content-fetch` against the asset mock | Pass, every declared bundle lands in `.dead-air-x64\content-cache` under its own SHA-256 |
| Commit | `--content-commit` over that cache | Pass, every bundle is installed into `database\` and the latch is removed |
| Plan against a complete installation | `--content-plan` after the commit | Pass, `missing=0` - an installation that already matches its manifest fetches nothing |
| Resume through repeated drops | Mock closing the socket every quarter of the largest bundle | Pass, the transfer completes, and the mock's log shows `Range` requests from non-zero offsets, so it resumed rather than restarting from zero each time |
| Server that ignores `Range` | `-NoRange` mock answering 200 to a range request over an existing part file | Pass, the part is truncated and refetched from zero instead of having a second copy of the head appended |
| Tampered cache entry | One byte flipped in a finished cache file before the commit | Pass, the commit exits non-zero and nothing enters `database\` |
| Upgrade by delta | `Test-ContentFlow.ps1`, a v2 manifest whose `[deltas]` section carries one real `.darpatch` edge for the largest bundle, mock serving the v2 assets | Pass, the fetch pulls the patch and never the whole v2 bundle, and the locally rebuilt file survives the commit and lands in `database\` under its v2 name. Recorded as a failure when `bin\x64\Release\DarDelta.exe` is not built, because a case that cannot run is not a case that passed |
| Install-time fetch | Silent `Setup.exe` against the asset mock | Pass, five bundles fetched in `PrepareToInstall`, committed at `ssPostInstall`, latch cleared |
| Install with no asset server | The same silent Setup with the mock stopped | Pass, aborted with a message and the target directory left byte-for-byte untouched |
| Uninstall content sweep | Silent uninstall of a content-bearing installation | Pass, `database\` left holding exactly the archives that were there before the install |
| In-game repair | QA rig with a bundle deleted by hand, driven through `dar_content_repair` and through the dialog | Pass, detected, downloaded, installed and the latch cleared, after which the dialog offers a restart; the missing-bundle layout was confirmed on screen with both buttons and the problem list rendered |
| GitHub `Range` probe | A published release asset requested directly from `github.com` | Pass, 206 with a well-formed `Content-Range` on the redirect target, so resume works against the real host and not only the mock |
| Shard layout at full size | `Test-ContentScale.ps1`, 5120 MB of synthetic content spread Zipf-style over 60 directories, through the real builder in `-LayoutOnly` mode | Pass, 18 shards, largest 1094 MB, no group left undivided. It fails when a shard passes the 1536 MB ceiling, when one shard holds more than half of all content, or when a group above 500 MB is left in a single shard. It measures the placement policy only - no archive is packed, transferred or committed |

## Content coverage limits

Everything above ran against the QA fixture: five bundles totalling about 19 MB,
served from loopback. Nothing has been exercised at the size this system exists
for. `Test-ContentScale.ps1` rehearses the shard layout policy at that size and
nothing else, so the cold hash duration over several gigabytes, the growth of
the filesystem index and of `_initialize` with tens of thousands of content
files, commit duration and peak disk during a full fetch are all projections.
The 250 MB shard target and the 1536 MB ceiling in `groups.ltx` have never been
produced by a real build.

No content release exists yet. The only traffic to the real host is the `Range`
probe above; no bundle has been fetched from a published release asset, and the
pinned assets repository has never served a fetch. Update-time content is
equally unproven: the only content step on the update path is the
`--content-commit` appended to the updater's command line, which commits a cache
something else filled, and no run has taken an installation across two content
sets.

Binary deltas have shipped. `src/xrContentSync/ContentDelta.{h,cpp}` applies
them, `src/utils/DarDelta` builds them, `dead_air_x64_content_bundles.ps1` emits
the `[deltas]` section whenever it is given `-PreviousManifest`, and the manifest
parser validates the cumulative edges. One end-to-end case exercises the healthy
path: `Test-ContentFlow.ps1` overwrites 4 KiB in the middle of the largest
bundle, builds a real `.darpatch` with `DarDelta.exe`, writes a v2 manifest
carrying that single edge with a recomputed `content-id`, installs v1 for real,
then repoints the mock at the v2 assets and fetches. It asserts that the fetch
succeeds, that the mock's log shows the `.darpatch` requested, that it never
shows the whole v2 bundle requested, and that the locally rebuilt file passes the
commit and lands in `database\`. That is one hop on a healthy disk and nothing
else. When `bin\x64\Release\DarDelta.exe` is not built the case is recorded
as a failure rather than skipped, so a run that dropped the only delta
coverage cannot end by reporting that every case passed.

The failure modes are all still uncovered, and they are a pre-release blocker,
because the apply path is the only place in the system where the client
manufactures bytes it never downloaded. A corrupt patch has to be shown falling
back to the whole bundle for every malformed shape `ContentDelta::Apply`
distinguishes - bad magic or version, a body that ends early, a COPY reaching
outside the base, an INSERT larger than the target can hold, an unknown opcode,
and ops that all complete but produce the wrong length - every one of which is an
`Integrity` verdict. A patch applied against the wrong base is deliberately not
one of them: the base is re-hashed immediately before the apply rather than
trusted from the plan, and a mismatch is `Transient`, so it has to fall back
silently and must not be remembered. A cancel mid-apply is untested in both
halves: there is no cancel check inside the COPY and INSERT byte loops, only
between ops, so a single op over a large base sets the worst-case latency, and
the partial output has to be gone afterwards. The blacklist round trip has never
been walked: `rejected-deltas.txt` takes an entry only for an `Integrity`
verdict, while a disk that filled mid-apply, a cancel, a wrong base, and an
output that matches the `.darpatch` header's target hash but not the manifest's
are all retried instead, because a permanent blacklist turns a 20 MB delta into a
400 MB download for good - and neither the resolver dropping a blacklisted edge
nor `ContentCommit` clearing the file when the content-id changes has been
exercised. And no multi-hop chain has ever been built: each intermediate is freed
only once the next hop's apply returns, so peak cache use is two bundles plus the
hop's delta asset rather than the whole path, and a chain that does not end at
the bundle the manifest wants must be refused rather than committed.

Also untested: a disk that fills during a fetch or a commit; the installer's
free-space refusal on the directory page; a bundle that cannot be deleted during
uninstall, which should be named in a message rather than silently skipped; and
repair on an installation where a mod disabled automatic updates, which nothing
in `ContentService` consults but which has never been run. The diagnostic report
gained `content_id`, `content_manifest`, `content_incomplete` and
`skipped_bundles`; no report has been captured since, so the privacy scan row
above does not cover them.

## Runtime acceptance rules

A runtime test passes only when the intended game state is reached, the process
remains responsive after loading, and the log contains no new fatal error,
assertion, reader overflow, or Lua compatibility failure. A content bug already
present in the original x86 runtime is recorded separately and is not treated
as an x64 regression.

Every runtime gate in this document now presupposes a complete content set.
`CLevel::net_start1` refuses the load through `ContentService::PlayBlocked()`,
which switches on the service state and never reads the latch file: only
`Complete` and `Verifying` are let through, and `Verifying` deliberately so,
because the cheap pass has already confirmed every bundle is present and the
right size and a hash pass in flight is no reason to hold the player at the
menu. Everything else refuses - `Incomplete` and `Recovery` obviously, `Unknown`
because the service has not looked yet, `Repairing` because it is still
downloading, and `Repaired` too: the bundles are back on disk but the filesystem
was indexed without them, so a level loaded before the restart would still be
missing every asset they carry. A run that never reaches a level because content
is missing is therefore a content failure, not a runtime regression, and is
recorded as one.
