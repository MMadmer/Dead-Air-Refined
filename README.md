# Dead Air: Refined

Dead Air: Refined is a comprehensive technical update for Dead Air 0.98b. The
project provides a native 64-bit Windows runtime, improves stability and
performance, modernizes the engine dependency stack, and adds integrated
installation, diagnostics, bug reporting, and automatic updates.

Current release: **1.4.0**

Required game: **Dead Air 0.98b or Dead Air Revolution II**

Supported platform: **Windows x64**

The release is distributed as a patch for an existing game installation. The
base game's content is not included. Refined's own assets are not inside the
download either: they are published separately as versioned, hash-named bundles
and fetched during installation, so an installation always holds exactly the
content its version declares.

## Project lineage

Dead Air: Refined is an independent derivative project maintained by MMadmer.
Its engine is called **XFined-Ray**, and that is the name it reports in logs,
diagnostics and packaging. The foundation it descends from is the
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
- Updated third-party libraries and a warning-clean x64 `Release` build.
- Improved windowed, borderless, and exclusive-fullscreen display modes.
- Engine-native anonymous diagnostic reports and a bug-report form available in
  both the main menu and the in-game menu.
- Automatic update checks against GitHub releases, verified update archives,
  download progress, restart-based installation, and automatic cleanup.
- Versioned content bundles pinned to the game version and verified by hash at
  every start: an installation is either complete or reported as broken, with a
  repair that re-fetches exactly what is missing.
- Patch installer that fetches and verifies the release's content before it
  changes anything, and restores the original 32-bit runtime during removal.

Development and validation rules are defined in
[`PROJECT_RULES.md`](PROJECT_RULES.md). Technical specifications are available
in [`docs/dead-air`](docs/dead-air).

## Installation

Download only one release asset. Most users need
`Dead-Air-Refined-1.4.0-Setup.exe`; it supports both first-time installation
and manual upgrades from an earlier Refined version, and it is the only asset
that installs content on a machine that has none. Use
`Dead-Air-Refined-1.4.0-Update.zip` when you prefer to upgrade an
existing Refined installation by hand: extract it into the game root and
replace the existing files. It carries the runtime and the content manifest but
not the content itself, so it can upgrade an installation that already has its
bundles; extracted into a folder Refined was never installed into, it produces a
game that knows what content it is missing and offers to fetch it, not one that
is ready to play.
The built-in updater downloads the patch archive on its own. You do not need
more than one file.

1. Install Dead Air 0.98b or Dead Air Revolution II.
2. Close the game and any tools that may keep its files open.
3. Download `Dead-Air-Refined-1.4.0-Setup.exe` from the latest release.
4. Select the root game directory containing `xrEngine.exe`, `fsgame.ltx`, and
   the `database` directory.
5. Let the content download finish. It runs before anything on disk is
   changed, so cancelling it leaves the installation exactly as it was, and
   what was already downloaded is kept and reused on the next attempt.
6. Complete the wizard and start the game normally.

The installer updates both an original 32-bit installation and an earlier
Dead Air: Refined installation. An installation consists of three parts:

- the x64 runtime in the game root;
- `database`, holding the game's own archives, the Refined compatibility
  archive, and the content bundles for the installed version;
- `.dead-air-x64`, holding the content manifest that pins this version to its
  bundles, the download cache, the file lists the installer and the updater
  maintain, and a backup of the original 32-bit runtime.

Those are the only places inside the game folder the installer writes; outside
it, it creates Start-menu shortcuts, an optional desktop shortcut, and the
standard Windows uninstall entry, all of which the uninstaller removes. The
game's own archives are never replaced, and `gamedata`, `appdata`, saves,
`MODS`, and JSGME state are left alone.

If content ever goes missing or is damaged, the game reports the installation
as incomplete, refuses to load a level, and offers to repair it from the main
menu. A repair downloads only what is missing and ends with a restart.

## Automatic updates

The game checks this repository once after the main menu appears. When a newer
stable version is available, the update dialog displays the installed version,
the available version, and the download size. Nothing is offered while the
installation's content is incomplete — the repair comes first.

Downloaded archives are validated by version, file manifest, size, and SHA-256.
After confirmation, the updater closes the game, snapshots the files it is about
to replace, replaces the runtime files, updates the maintenance utility, removes
its cache, and starts the updated game. The snapshot exists only to undo a
failed update and is deleted as soon as one succeeds.

Update archives never carry content bundles. The update payload is deliberately
capped far below what a content set weighs, and content is versioned and
verified on its own, so an update replaces the runtime and leaves the content
where it is.

The installer remains the recommended option. The `Update.zip` asset can
also be extracted manually; the integrated updater uses the patch archive.

## Removal

`Uninstall Dead Air Refined.exe` removes Dead Air: Refined and restores the
original 32-bit runtime when its backup is present. It also removes the content
the installation downloaded: the bundles its manifest names, any bundle-shaped
file left over from an earlier revision, and the download cache. Anything it
cannot delete is reported by name instead of being quietly left behind.

Rolling back to an earlier Refined version is not offered. User saves,
configuration files, `gamedata`, `MODS`, and JSGME state are not removed.

## Bug reports and diagnostics

The main menu and in-game menu include an integrated bug-report form. A report
contains a title, a description, the exact Refined version, and, when selected,
an anonymous diagnostic archive.

Diagnostic archives exclude player identity, command-line data, environment
contents, save payloads, installation paths, and raw stack memory. They retain
the build identifier, module offsets, hardware and runtime information, sanitized
logs, and content metadata required to investigate a problem: the installed
content identifier, whether the installation is currently marked incomplete, the
mounted archives, and every bundle the engine refused to mount with the reason
it was refused.

A diagnostic archive can also be created without submitting a report by running
`session_report` in the game console. Reports are stored under
`$app_data_root$/session_reports`. After an unhandled crash, the next startup
opens the report form before any update notification and attaches that exact
anonymous crash report. Sending or declining it marks the crash as handled.

See [`docs/dead-air/DIAGNOSTIC_REPORTS.md`](docs/dead-air/DIAGNOSTIC_REPORTS.md)
for the report schema and privacy contract.

## Compatibility

Dead Air: Refined preserves the established content loading order and supports
packed and loose addons that use the standard `database`, `gamedata`, and `MODS`
paths. Existing 32-bit saves remain compatible.

Content bundles are ordinary archives in `database` and get no special
priority: loose `gamedata`, JSGME, and XMS modules override them, and so does
an archive that sorts after them. Verification looks at the bundle files
themselves and never at the paths a mod resolves, so overriding content does
not make an installation look broken. See
[`docs/dead-air/CONTENT_BUNDLES.md`](docs/dead-air/CONTENT_BUNDLES.md) for the
bundle and manifest contract.

Refined-specific persistent state is stored in one optional, forward-compatible
`.scov` companion. The original `.scop` and `.scoc` formats remain unchanged,
and unknown chunks are preserved across saves. See
[`docs/dead-air/SAVE_COMPATIBILITY.md`](docs/dead-air/SAVE_COMPATIBILITY.md).

Native 32-bit plugins and addons that replace engine executables or DLL files
cannot run inside the 64-bit process and require an x64 build.

## Building from source

Requirements:

- Windows x64;
- CMake 3.23 or newer;
- Ninja;
- Visual Studio Build Tools with the Desktop development with C++ workload;
- PowerShell 7 or Windows PowerShell;
- Git.

Build the runtime from the repository root:

```powershell
tools\build\build_x64.ps1
```

That is the whole procedure for a fresh clone. The wrapper initializes any
missing Git submodule, applies the required dependency patches from `patches`,
enters the MSVC x64 developer environment, and runs the canonical `windows-x64`
CMake preset with Ninja Multi-Config. Binaries land in `bin\x64\<configuration>`.

`-Configuration` selects `Debug`, `Mixed`, `Release` (the default), or
`ReleaseMasterGold`. `-Clean` discards the build and output directories first; it
is only needed after changing CMake files, the toolchain, or dependencies, since
an ordinary rebuild is incremental and a repeated run does no work.

A release is built in two steps, content first, because the installer refuses to
be built without a content manifest:

```powershell
tools\package\dead_air_x64_content_bundles.ps1 -SourceRoot <authored gamedata> `
    -BundleCache <bundle cache> -OutputManifest <manifest> `
    -PortVersion 1.4.0 -ReleaseTag <content tag>
tools\package\build_dead_air_x64_installer.ps1 -PortVersion 1.4.0 `
    -ContentManifest <manifest>
```

The first script packs the authored content into bundles, reuses every bundle
whose files did not change, and writes the manifest that pins this version to
exactly those bundles. The second builds the compatibility archive, the
installer and the manual archive against that manifest, plus the patch archive
when it is given the previous release to diff against. Generated release files
are written to `artifacts` and are not tracked by Git.

The content release is published before the game release, and a published
bundle is never replaced or deleted: a bundle name carries the hash of its own
bytes, so overwriting one under that name makes every installed manifest wrong.
The publication rules are in [`PROJECT_RULES.md`](PROJECT_RULES.md); dependency
versions and compatibility pins are recorded in
[`docs/dead-air/DEPENDENCIES.md`](docs/dead-air/DEPENDENCIES.md).

## Project documentation

- [`PROJECT_RULES.md`](PROJECT_RULES.md) — authoritative development, QA,
  compatibility, commit, and release rules.
- [`TEST_MATRIX.md`](docs/dead-air/TEST_MATRIX.md) — release validation matrix.
- [`AUTO_UPDATE.md`](docs/dead-air/AUTO_UPDATE.md) — update archive and client
  protocol.
- [`CONTENT_BUNDLES.md`](docs/dead-air/CONTENT_BUNDLES.md) — content bundle,
  manifest, delta, and repair contract.
- [`DIAGNOSTIC_REPORTS.md`](docs/dead-air/DIAGNOSTIC_REPORTS.md) — diagnostic
  archive format and privacy guarantees.
- [`SAVE_COMPATIBILITY.md`](docs/dead-air/SAVE_COMPATIBILITY.md) — original-save
  compatibility, extension chunks, and atomic transaction format.
- [`DEPENDENCIES.md`](docs/dead-air/DEPENDENCIES.md) — dependency versions and
  build policy.
- [`MODDING.md`](docs/dead-air/MODDING.md) — the compatibility contract for
  addons: XMS modules, content bundles and load order, loose particle
  overrides.
- [`UPSTREAM.md`](docs/dead-air/UPSTREAM.md) — source lineage and attribution.

## Credits

Dead Air: Refined is built on other people's work. Everyone below has code,
assets, or solutions in what ships here.

To the **Dead Air** developers, for the game and the systems this project
extends. To **Lanforse**, for preserving and sharing the surviving Dead Air 1.0
source reference. To the **Dead Air community**, for years of testing, addons,
research, and technical documentation.

To **[GSC Game World](https://www.gsc-game.com/)**, for the X-Ray Engine that
every fork here descends from.

To the **[OpenXRay](https://github.com/OpenXRay/xray-16)** team, for the engine
foundation this port is built on.

To the **[IX-Ray](https://github.com/ixray-team/ixray-1.6-stcop)** team, for
much of the current graphics stack and a long list of engine fixes — their
shader code and lookup data ship inside this build. Particular thanks to
**LVutner**, named in those shaders themselves.

To the **[Gunslinger](https://github.com/gunslingermod)** team, for the 3D PDA:
the models, animations, textures, sounds, and shaders it is built from come from
their mod.

To **Akinaro**, for the **[Stalker Two-K](https://www.moddb.com/mods/stalker-two-k)**
texture set (one texture, the moss on the boulders, recoloured for Dead Air's
autumn palette), and to **Cromm Cruac**, for the tree and foliage textures of
**[Absolute Nature 4](http://absolute.crommcruac.com)** (two dead-branch textures
recoloured from yellow-brown to Dead Air's grey-green). Both sets are free, for
S.T.A.L.K.E.R. games only, and stay that way here.

To **Feel_Fried** (FDDA), **themrdemonized** (Ledge Grabbing), **Mirrowel**
(FDDA Enhanced Animations), **lizzardman** (Headgear Animations, FDDA Redone)
and **ZeburG** with the **MFS team** (bread and sausage animations), for the
first-person animations of the animation module; and to **DanesCrail**'s
DeadAir-x64 project for collecting them. Who made what, on what terms, is in
[`ASSET_CREDITS.md`](docs/dead-air/ASSET_CREDITS.md).

To **DanesCrail**, author of
**[DeadAir-Engine-x64-OpenSource](https://github.com/DeadAir-x64/DeadAir-Engine-x64-OpenSource)**
— an independent x64 port of the same mod — for solutions adopted here.

To **Jorge Jimenez** and the authors of
**[SMAA](https://github.com/iryoku/smaa)**, for the anti-aliasing library.

To **Manuel (ascii1457)** and **Screen Space Shaders for Anomaly**, for a
rendering technique reimplemented here.

To **Cromm Cruac**, **Peacemaker**, **Alundaio**, **DoctorX**,
**LostAlphaRus**, and **Andrey Fidrya (Zmey)**, whose gamedata scripts ship in
the compatibility overlay.

Dead Air: Refined also builds on a number of open-source libraries, listed with
their versions in [`DEPENDENCIES.md`](docs/dead-air/DEPENDENCIES.md). Individual
third-party components retain their respective copyright notices and licenses.

## License

Source-code licensing terms are provided in [`License.txt`](License.txt).
Third-party components remain subject to their own licenses.
