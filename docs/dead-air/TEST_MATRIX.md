# Dead Air x64 validation matrix

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Clean x64 build | `Release`, clean intermediate tree | Pass, zero build errors |
| PE architecture | 44 packaged EXE/DLL files: 41 runtime files and 3 update/maintenance programs | Pass, all AMD64 |
| XDB discovery | Existing `database/*.xdb*` | Pass |
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
| Packed addon | DA Inventory Sort XDB | Pass |
| Loose addon | DAR2 Oxygen HUD scripts/UI | Pass |
| Lua binding parity | Original x86 exports vs x64 exports | Pass, zero missing |
| Screen-space sun shafts | R4 renderer | Pass, all shader stages compiled |
| FXAA | R4 renderer | Pass, both shader stages compiled |
| Visor drops and reflections | R4 renderer, `r2_lenswater` and `r2_lensdirt` enabled | Pass only when `combine_2_naa` compiles without fallback shaders and the world remains visible |
| PDA script tabs | Widescreen PDA, relations tab and one native tab | Pass only when text placement remains stable, tabs accept input, and no Lua/UI lifecycle error is logged |
| Debug script tabs | Debug mode, spawner plus every available non-spawner tab | Pass only when each tab is created, drawn, and switched without Lua/UI lifecycle errors |
| Outfit and helmet night vision | Equipped NV-capable outfit and helmet, R4 renderer | Pass only when both activation paths start and stop the configured post-process effector |
| Third-party script compatibility | Binding parity fixture plus a representative packed or loose addon | Pass only when the addon starts without missing export, signature, or Lua ownership errors |
| Level transition | Underground to Agroprom | Pass |
| Transition-save reload | Fresh underground and Agroprom saves | Pass |
| Long-session soak | Fresh Agroprom save, packaged runtime | Pass, 15.29 minutes, responsive, 3.04 GiB private memory |
| Dependency audit | 44-file release package | Pass, no missing local DLLs |
| GUI upgrade installer | Final `Dead Air: Refined 1.1.1` package over the installed 1.1.0 root | Pass, all 46 managed payload hashes matched and 75 saves remained unchanged |
| Main-menu AtmosFear apply | `af_options_dialog:OnBtnAccept()` with no active level | Pass, settings saved without calling the runtime weather manager; process remained responsive without Lua or access-violation errors |
| Installed EXE uninstaller | Upgrade mode | Pass, 13 original runtime files restored across a 43-file scope with zero mismatches |
| Refined version rollback | Typed `refined-version` snapshot selected through the uninstaller | Pass, saved runtime restored by SHA-256 while the original x86 backup remained reserved for removal |
| Maintenance wizard | Installer-style fixed-size layout and silent action paths | Pass, standard header artwork persisted and both rollback and removal completed without leftovers |
| Patch-only distribution | Installer contents and target validation | Pass, no original Dead Air files included and an empty target is rejected |
| Raw update archive | Manifest schema, exact file set, per-file size and SHA-256 | Pass, 51 ZIP entries and 46 payload files including maintenance and uninstall components |
| Saved-game preservation | Fresh install without `appdata` plus repeat install with an existing save sentinel | Pass, `appdata\savedgames` created and sentinel SHA-256 unchanged |
| External updater | Isolated installed root, backup, apply, maintenance, cleanup | Pass, prior version snapshot created, obsolete sentinel removed, cache deleted, uninstaller regenerated |
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
| Rollback | Restore the selected version-independent snapshot by SHA-256 and remove added files | Pass, zero mismatches or leftovers |

## Runtime acceptance rules

A runtime test passes only when the intended game state is reached, the process
remains responsive after loading, and the log contains no new fatal error,
assertion, reader overflow, or Lua compatibility failure. A content bug already
present in the original x86 runtime is recorded separately and is not treated
as an x64 regression.
