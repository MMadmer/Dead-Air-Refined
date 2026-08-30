# Dead Air: Refined automatic updates

## Release contract

Each published version uses one GitHub Release whose tag is a strict Semantic
Version in `MAJOR.MINOR.PATCH` form. The release contains these assets:

- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Setup.exe` for the recommended guided
  installation;
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Setup_Manual.zip` — the complete payload,
  for manual extraction and as the update every installation can always take;
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Update_Patch.zip` — optional. Carries only
  the files that differ from the previous release, so an up-to-date player
  downloads a fraction of the full archive;
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Update.zip` — optional compatibility
  alias, byte-identical to `Setup_Manual.zip`. Clients built before the rename
  look for this exact name and see no update at all without it, so a release
  should keep publishing it until that generation is no longer in the field.

The game reads the public release list for
`MMadmer/Dead-Air-Refined`. Drafts, prereleases, malformed tags and releases
without a usable full archive are ignored. No separate release is required
for the installer: GitHub Releases support multiple assets under one tag.

Every archive must have a non-zero size and a GitHub-provided SHA-256 digest.
The full archive contains the same installed payload as the setup program,
including the updater, maintenance program and uninstaller launcher.

The release description follows the bilingual template in `PROJECT_RULES.md`.
The English `## Changes` list must be inside `## EN`, and the Russian
`## Изменения` list must be inside `## RU`. Only bullet items from those exact
sections are shown by the client; installation instructions and the other
language are never included in the update dialog.

References:

- [GitHub REST API for releases](https://docs.github.com/en/rest/releases/releases)
- [GitHub REST API for release assets](https://docs.github.com/en/rest/releases/assets)
- [About GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)

## Client behavior

The check starts once per game process after the main menu has initialized, and
only when update checking is enabled — the Game options carry a `Проверять
обновления` switch, backed by the `dar_update_check` console command and
persisted in `user.ltx` like any other setting. Turning it off stops the check
before it reaches the network. An empty release list, a network error or the
absence of a valid newer release does not open a dialog and does not block the
menu.

The ordinary offer is confined to the installed MAJOR line: a build on 1.9.0 is
offered 1.9.1 and 1.12.0, never 2.x. Crossing a major is not an update — saves
and mods do not carry over — so a release from a higher major line is announced
by its own dialog, which links to the release page and asks the player to
install it into a separate folder, leaving the current build in place. That
notice is decided independently of the ordinary offer and both can be pending at
once: the notice comes first, the update dialog behind it. It reappears on every
launch whose check succeeds; the checkbox inside it is the same
`dar_update_check` switch as in the options.

When a release publishes a patch and the installed version is the release
immediately below it, the client downloads the patch instead of the full
archive. The applier verifies the result either way (see below); a patch that
does not fit is refused without touching the installation, and the refusal is
recorded in `.dead-air-x64/patch-rejected.txt` so the client falls back to the
full archive rather than offering the same patch again.

When a newer version is available, the native C++ dialog displays the installed
version, available version, download size and the release changes in a scroll
view. It selects Russian changes only when the active string-table language is
`rus` or `ru`; every other language uses the English section. Declining the
offer dismisses it until the next game launch. During download, the dialog
displays transferred and total mebibytes together with a progress bar.
Installation remains unavailable until the complete archive has passed size
and SHA-256 verification.

An unhandled-crash report prompt has priority over the update dialog. The update
check may finish in the background, but its dialog remains queued until the
crash report has been submitted successfully or explicitly declined.

The version comparison uses the product version compiled into the engine. The
updater does not modify or infer that version independently.

## Mod opt-out

Mods can take the installation out of the update flow entirely by declaring themselves in
`[auto_update_opt_out]` (in `configs/dead_air_x64_mod_opt_out.ltx` or in `system.ltx` via an
XMS `.ltxp` patch), or by calling `main_menu.disable_auto_update("<name>")` from Lua. The
check then never starts, the bug report menu entry disappears, and the menu shows
`Автообновление отключено модами:` followed by the declared names. See
`docs/dead-air/MODDING.md` for the full contract.

## Update application

The game copies the external updater into the version-specific cache and exits.
The updater then:

1. waits for the game process to finish;
2. validates the downloaded archive and every entry in `update-manifest.txt`;
3. rejects absolute paths, parent traversal, duplicate paths, excessive file
   counts and excessive expanded size;
4. creates a `refined-version` snapshot using the existing backup format;
5. applies the payload with atomic file replacement;
6. refreshes the installed maintenance program and uninstaller metadata;
7. starts a cleanup instance, removes the downloaded cache and restarts the
   original game command line.

If payload application or maintenance fails, the saved snapshot is restored.
The original x86 snapshot remains reserved for complete removal and is not used
as an update rollback target.

## Archive manifest

A full archive uses schema `dead-air-refined.update/1`:

```text
schema=dead-air-refined.update/1
version=MAJOR.MINOR.PATCH
<sha256><TAB><size><TAB><relative-path>
```

The manifest must list every regular payload file exactly once. The ZIP may
contain only those files, directory entries and the manifest itself. Every
archive also contains empty `appdata/` and `appdata/savedgames/` directory
entries. The updater and installer create this user-data path when absent and
never include, replace or remove existing save files.

A patch uses schema `dead-air-refined.update/2`:

```text
schema=dead-air-refined.update/2
version=MAJOR.MINOR.PATCH
kind=patch
base=MAJOR.MINOR.PATCH
<sha256><TAB><size><TAB><relative-path>
```

The manifest still lists **every** file of the target version — that is what
lets the applier reach exactly the state a full install would reach, and what
tells it which files the new version dropped. Only the payload is trimmed: a
file the manifest declares and the ZIP omits must already exist in the
installation with that exact hash, which is checked before anything is written.
So a patch is not a blind overlay; it either reconstructs the full version or
refuses.

Schema `/1` is deliberately frozen. The updater that applies an archive is the
one already installed, so an older updater must keep working on the full archive
it downloads. Only patches use `/2`, and only a client new enough to choose a
patch will ever hand one to an updater new enough to read it.

Build both with `tools/package/build_dead_air_x64_installer.ps1`: pass
`-PreviousFullArchive <path to the previous Setup_Manual.zip>` to cut the patch,
`-NoLegacyUpdateAlias` once the `-Update.zip` alias is no longer needed, and
`-PatchOnly` to re-cut a patch against a different base without rebuilding the
release. `tools/qa/Test-UpdatePatchFlow.ps1` exercises the applier end to end on
a synthetic installation, and `tools/qa/Start-UpdateApiMock.ps1` serves a fake
release list on loopback for the in-game flow (`-qa_update` plus
`DAR_QA_UPDATE_API`).

Publication and verification rules are defined in
[`PROJECT_RULES.md`](../../PROJECT_RULES.md).
