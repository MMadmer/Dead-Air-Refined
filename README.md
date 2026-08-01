# Dead Air: Refined

Dead Air: Refined is a comprehensive technical update for Dead Air 0.98b. The
project provides a native 64-bit Windows runtime, improves stability and
performance, modernizes the engine dependency stack, and adds integrated
installation, diagnostics, bug reporting, and automatic updates.

Current release: **1.0.1**

Required game: **Dead Air 0.98b or Dead Air Revolution II**

Supported platform: **Windows x64**

The release is distributed as a patch for an existing game installation. Game
content is not included.

## Project lineage

Dead Air: Refined is an independent derivative project maintained by MMadmer.
Its engine foundation is based on the
[OpenXRay `xray-16`](https://github.com/OpenXRay/xray-16) project, with the
initial Dead Air port derived from upstream commit
[`29030f81b137f6ea5365b3d71f2b588490832f5b`](https://github.com/OpenXRay/xray-16/commit/29030f81b137f6ea5365b3d71f2b588490832f5b).

The Refined repository maintains its own release history because it targets a
specific game, runtime, installer, update service, diagnostics stack, and
compatibility contract. Upstream authorship is preserved through the linked
source history, copyright notices, and third-party licenses rather than being
represented as Refined-specific contributions.

See [`docs/dead-air/UPSTREAM.md`](docs/dead-air/UPSTREAM.md) for the complete
provenance statement.

## Highlights

- Native AMD64 executable and runtime libraries without the 32-bit address-space
  limit.
- Compatibility with existing Dead Air XDB archives, loose `gamedata` overrides,
  save files, Lua addons, JSGME workflows, and the standard directory layout.
- Optimized loading, archive access, texture processing, geometry upload, save
  decompression, and runtime lookup structures.
- Expanded multicore execution for independent AI, pathfinding, physics,
  particle, sound, and renderer work.
- Updated third-party libraries and a warning-clean `Release|x64` build.
- Improved windowed, borderless, and exclusive-fullscreen display modes.
- Engine-native anonymous diagnostic reports and a bug-report form available in
  both the main menu and the in-game menu.
- Automatic update checks against GitHub releases, verified update archives,
  download progress, restart-based installation, and automatic cleanup.
- Patch installer with versioned backups, rollback to an earlier Refined build,
  and restoration of the original 32-bit runtime during removal.

Detailed implementation and validation records are available in
[`docs/dead-air`](docs/dead-air).

## Installation

1. Install Dead Air 0.98b or Dead Air Revolution II.
2. Close the game and any tools that may keep its files open.
3. Download `Dead-Air-Refined-1.0.1-Setup.exe` from the latest release.
4. Select the root game directory containing `xrEngine.exe`, `fsgame.ltx`, and
   the `database` directory.
5. Keep backup creation enabled unless the current Refined installation is
   already backed up separately.
6. Complete the wizard and start the game normally.

The installer updates both an original 32-bit installation and an earlier
Dead Air: Refined installation. It does not replace `database`, `gamedata`,
`appdata`, saves, `MODS`, or JSGME state.

## Automatic updates

The game checks this repository once after the main menu appears. When a newer
stable version is available, the update dialog displays the installed version,
the available version, and the download size.

Downloaded archives are validated by version, file manifest, size, and SHA-256.
After confirmation, the updater closes the game, creates a versioned backup,
replaces the runtime files, updates the maintenance utility, removes its cache,
and starts the updated game.

The installer remains the recommended option for manual installation. The
`Update.zip` asset is intended for the integrated updater.

## Backups, rollback, and removal

`Uninstall Dead Air Refined.exe` provides two maintenance operations:

- remove Dead Air: Refined and restore the original 32-bit runtime;
- restore a selected backup of an earlier Dead Air: Refined version.

The original 32-bit backup is reserved for removal and is not presented as a
normal Refined rollback target. User saves and configuration files are not
removed automatically.

## Bug reports and diagnostics

The main menu and in-game menu include an integrated bug-report form. A report
contains a title, a description, the exact Refined version, and, when selected,
an anonymous diagnostic archive.

Diagnostic archives exclude player identity, command-line data, environment
contents, save payloads, installation paths, and raw stack memory. They retain
the build identifier, module offsets, hardware and runtime information, sanitized
logs, and content metadata required to investigate a problem.

A diagnostic archive can also be created without submitting a report by running
`session_report` in the game console. Reports are stored under
`$app_data_root$/session_reports`.

See [`docs/dead-air/DIAGNOSTIC_REPORTS.md`](docs/dead-air/DIAGNOSTIC_REPORTS.md)
for the report schema and privacy contract.

## Compatibility

Dead Air: Refined preserves the established content loading order and supports
packed and loose addons that use the standard `database`, `gamedata`, and `MODS`
paths. Existing 32-bit saves remain compatible.

Native 32-bit plugins and addons that replace engine executables or DLL files
cannot run inside the 64-bit process and require an x64 build.

## Building from source

Requirements:

- Windows x64;
- Visual Studio with the Desktop development with C++ workload;
- PowerShell 7 or Windows PowerShell;
- initialized Git submodules.

Build the runtime from the repository root:

```powershell
tools\build\build_x64.ps1
```

Build the patch installer and automatic-update archive:

```powershell
tools\package\build_dead_air_x64_installer.ps1 -PortVersion 1.0.1
```

Generated release files are written to `artifacts` and are not tracked by Git.
Dependency versions and compatibility pins are recorded in
[`docs/dead-air/DEPENDENCIES.md`](docs/dead-air/DEPENDENCIES.md).

## Project documentation

- [`PORT_STATUS.md`](docs/dead-air/PORT_STATUS.md) — port scope and completed
  compatibility work.
- [`TEST_MATRIX.md`](docs/dead-air/TEST_MATRIX.md) — release validation matrix.
- [`MULTICORE_OPTIMIZATION_PLAN.md`](docs/dead-air/MULTICORE_OPTIMIZATION_PLAN.md)
  — multicore audit and implemented task batches.
- [`DIAGNOSTIC_REPORTS.md`](docs/dead-air/DIAGNOSTIC_REPORTS.md) — diagnostic
  archive format and privacy guarantees.
- [`DEPENDENCIES.md`](docs/dead-air/DEPENDENCIES.md) — dependency versions and
  build policy.
- [`UPSTREAM.md`](docs/dead-air/UPSTREAM.md) — source lineage and attribution.

## Credits

Special thanks to the Dead Air developers for creating the game and its systems,
and to the Dead Air community for long-term testing, addons, research, and
technical documentation.

Dead Air: Refined also incorporates work from the OpenXRay project and its
contributors. Individual third-party components retain their respective
copyright notices and licenses.

## License

Source-code licensing terms are provided in [`License.txt`](License.txt).
Third-party components remain subject to their own licenses.
