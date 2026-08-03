# Dead Air: Refined automatic updates

## Release contract

Each published version uses one GitHub Release whose tag is a strict Semantic
Version in `MAJOR.MINOR.PATCH` form. The release contains both distribution
formats:

- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Setup.exe` for the recommended guided
  installation;
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Update.zip` for manual extraction or
  automatic updating.

The game reads the public release list for
`MMadmer/Dead-Air-Refined`. Drafts, prereleases, malformed tags and releases
without the exact update archive are ignored. No separate release is required
for the installer: GitHub Releases support multiple assets under one tag.

The update archive must have a non-zero size and a GitHub-provided SHA-256
digest. The archive contains the same installed payload as the setup program,
including the updater, maintenance program and uninstaller launcher.

References:

- [GitHub REST API for releases](https://docs.github.com/en/rest/releases/releases)
- [GitHub REST API for release assets](https://docs.github.com/en/rest/releases/assets)
- [About GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)

## Client behavior

The check starts once per game process after the main menu has initialized. An
empty release list, a network error or the absence of a valid newer release does
not open a dialog and does not block the menu.

When a newer version is available, the native C++ dialog displays the installed
version, available version and download size. Declining the offer dismisses it
until the next game launch. During download, the dialog displays transferred and
total mebibytes together with a progress bar. Installation remains unavailable
until the complete archive has passed size and SHA-256 verification.

An unhandled-crash report prompt has priority over the update dialog. The update
check may finish in the background, but its dialog remains queued until the
crash report has been submitted successfully or explicitly declined.

The version comparison uses the product version compiled into the engine. The
updater does not modify or infer that version independently.

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

`update-manifest.txt` uses schema `dead-air-refined.update/1`:

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

Publication and verification rules are defined in
[`PROJECT_RULES.md`](../../PROJECT_RULES.md).
