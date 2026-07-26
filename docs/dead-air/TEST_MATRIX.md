# Dead Air x64 validation matrix

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Clean x64 build | `Release`, clean intermediate tree | Pass, zero build errors |
| PE architecture | 42 packaged EXE/DLL files | Pass, all AMD64 |
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
| Dependency audit | 42-file release runtime | Pass, no missing local DLLs |
| Installer | Final `1.0.0` package over an isolated existing x86 root | Pass |
| Final package smoke | Latest x86 save, 22,958 spawn points / 27,198 objects | Pass |
| Rollback | Restore 28 original x86 files by SHA-256 and remove added files | Pass, zero mismatches or leftovers |

## Runtime acceptance rules

A runtime test passes only when the intended game state is reached, the process
remains responsive after loading, and the log contains no new fatal error,
assertion, reader overflow, or Lua compatibility failure. A content bug already
present in the original x86 runtime is recorded separately and is not treated
as an x64 regression.

## Known content issue

`bind_gr_gun.script` fails because the shipped script references an undefined
`ggun_binder`. The same script fails at the same stage in the original x86
runtime, so this is not introduced by the port.
