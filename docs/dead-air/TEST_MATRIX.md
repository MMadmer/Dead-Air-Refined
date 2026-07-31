# Dead Air x64 validation matrix

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Clean x64 build | `Release`, clean intermediate tree | Pass, zero build errors |
| PE architecture | 41 packaged EXE/DLL files | Pass, all AMD64 |
| XDB discovery | Existing `database/*.xdb*` | Pass |
| Loose override discovery | Existing `gamedata` | Pass |
| Clean Dead Air new game | Base archives, no DAR2 archives | Pass |
| Clean Dead Air x64 save/reload | `x64_da_newgame` | Pass |
| Existing DAR2 x86 save | Latest `admin - quicksave9` | Pass |
| Packed addon | DA Inventory Sort XDB | Pass |
| Loose addon | DAR2 Oxygen HUD scripts/UI | Pass |
| Lua binding parity | Original x86 exports vs x64 exports | Pass, zero missing |
| Screen-space sun shafts | R4 renderer | Pass, all shader stages compiled |
| FXAA | R4 renderer | Pass, both shader stages compiled |
| Level transition | Underground to Agroprom | Pass |
| Transition-save reload | Fresh underground and Agroprom saves | Pass |
| Long-session soak | Fresh Agroprom save, packaged runtime | Pass, 15.29 minutes, responsive, 3.04 GiB private memory |
| Dependency audit | 41-file release runtime | Pass, no missing local DLLs |
| GUI upgrade installer | Final `Dead Air: Refined 0.9.0` package over an isolated x86 root | Pass, all 41 runtime hashes matched |
| Installed EXE uninstaller | Upgrade mode | Pass, 13 original runtime files restored across a 43-file scope with zero mismatches |
| Refined version rollback | Typed `refined-version` snapshot selected through the uninstaller | Pass, saved runtime restored by SHA-256 while the original x86 backup remained reserved for removal |
| Patch-only distribution | Installer contents and target validation | Pass, no original Dead Air files included and an empty target is rejected |
| Final package smoke | Latest x86 save, 22,958 spawn points / 27,198 objects | Pass |
| Manual diagnostic report | Loaded `async_benchmark_1`, hidden desktop | Pass, valid anonymous ZIP with system, hardware, runtime, content, log, configuration, and minidump data |
| Automatic crash report | Gated release QA access violation | Pass, exception code, module RVA, anonymous stack, and valid `MDMP` attachment captured |
| Diagnostic privacy scan | All ZIP entries, ASCII and UTF-16 | Pass, no user name, computer name, profile path, game path, e-mail address, or IP address |
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
