# Dead Air: Refined diagnostic reports

## Purpose

The engine creates the same network-ready report format for:

- an unhandled crash;
- a manually selected running session.

Manual capture is available through the `session_report` console command. Crash
capture is automatic. Reports are written to `$app_data_root$/session_reports`.
The directory keeps no more than 10 reports and removes the oldest report only
after a newer report is ready.

The main and in-game menus also expose a native bug-report form. It submits a
title, the baked product version and a description to the Refined Report Hub over HTTPS and can optionally
create and attach the same session-report ZIP. Submission runs on a
worker thread, so a slow or unavailable network does not stall the game loop.
The title accepts up to 200 characters and requires at least 5 non-whitespace
characters; the description accepts up to 10,000 and requires at least 20.
Both menus render `Dead Air: Refined v<version>` in the bottom-right corner
from the same native version constant used by uploads and diagnostic manifests.

After an unhandled crash, the next startup selects the newest unhandled
`dar-report-crash-*.zip` and first displays a native yes/no confirmation before
other startup dialogs. Choosing yes opens the report form with the `Отправка
crash report` caption. The crash attachment is mandatory and is the exact ZIP
that triggered the prompt. Choosing no, cancelling the form, or completing a
submission records that report as handled; failed submissions remain retryable,
and an already handled crash does not prompt again.

Public builds receive their upload credential through the ignored
`src/xrGame/ui/BugReportSecrets.local.h` build-time header. The repository
fallback deliberately contains no credential, and report contents or
authorization headers must never be written to the engine log.

## Container

Each report is a standard Deflate ZIP named:

```text
dar-report-session-<UTC>-<random>.zip
dar-report-crash-<UTC>-<random>.zip
```

The ZIP contains:

| Entry | Required | Contents |
| --- | --- | --- |
| `report.json` | yes | Stable machine-readable manifest |
| `session.dmp` | yes | Compact minidump without raw stack memory or data segments |
| `session.log` | when available | Last 4 MiB of the sanitized engine log |
| `user.ltx` | when available | Sanitized user configuration |
| `fsgame.ltx` | when available | Sanitized filesystem configuration |
| `save.scop` | when a save is active | Verbatim main save payload |
| `save.scoc` | when present | Verbatim script/custom save payload |
| `save.scov` | when present | Verbatim versioned project-extension payload |

`report.json` uses schema `dead-air-refined.session-report/1`. Metadata consumers
should interpret only fields they understand; the upload service preserves
unknown schemas and exposes unsupported or missing fields as absent. The optional
`save.files` array records the exact size and SHA-256 of every included save
component. The collector opens the complete save group before hashing or ZIP
creation so a concurrent atomic save cannot mix components from two revisions.

## Diagnostic coverage

The manifest records:

- product version, build commit, build ID, architecture and executable SHA-256;
- exception code, faulting module, module RVA and thread ID for crash reports;
- anonymous `module + RVA` stack frames;
- loaded module names, image sizes, timestamps and selected binary hashes;
- Windows version, CPU model, topology and supported instruction sets;
- GPU, driver version, D3D feature level and memory capacities;
- physical RAM, commit, disk space and system/session uptime;
- process memory, virtual-address layout, I/O totals, handles and thread count;
- current and aggregate CPU, I/O, FPS, frame-time and render-time load;
- current local and non-local GPU memory budget and usage;
- texture, model, sound-cache and Lua memory;
- ALife, online and pending-release object counts;
- XDB archives and loose content with safe relative names, sizes, timestamps and
  hashes for diagnostically important files;
- the state of the installed content set.

The developer must retain the matching PDB files for public builds. The report
contains enough `build + module + RVA` information to symbolize a stack without
shipping PDB files to players.

## Content fields

Content bundles are ordinary archives, so a mounted bundle already appears as a
row in `content.archives` like any other. What that row cannot say is which
content set the installation was supposed to have and whether it actually had it,
so the `content` object of `report.json` carries four more values:

| Field | Type | Value |
| --- | --- | --- |
| `content_id` | string | The `content-id=` line of `.dead-air-x64\content-manifest.txt`. Empty when no manifest could be read. |
| `content_manifest` | boolean | Whether the mount gate found and parsed a manifest. False means the installation cannot state what content it should have. |
| `content_incomplete` | boolean | Whether `.dead-air-x64\content-incomplete.txt` exists. This is the latch that refuses to start a level. |
| `skipped_bundles` | array of strings | File names of bundle-shaped archives the mount gate refused. Empty on a healthy installation. |

`content_id` is read by a short line scan rather than the manifest parser,
because the collector runs while the process is already dying; the scan stops at
the first `content-id=` line or at `[bundles]`, whichever comes first. It is
therefore possible for `content_id` to be non-empty while `content_manifest` is
false, and that combination is itself diagnostic: the manifest is present but
malformed, so every bundle was refused.

The reason each bundle was refused is not in the manifest. The mount gate writes
it to the engine log, which arrives with the report as `session.log`.

The schema string `dead-air-refined.session-report/1` is unchanged. The object
gained fields, and consumers already interpret only the fields they understand.

## Privacy contract

The sender-facing report excludes:

- user and computer names;
- user profile, installation and application-data paths;
- command-line and environment contents;
- player identity outside the verbatim save payload;
- raw stack memory and module data segments;
- e-mail addresses, network addresses and credential-like configuration values.

Save attachments are included unchanged and can contain player-created data;
they are therefore not anonymous. Other text attachments preserve structure while replacing sensitive values with
markers. Module, PDB and content paths retain only safe file names or relative
content paths. The minidump keeps thread contexts and module metadata but not
raw stack pages.

The four content fields carry nothing specific to the installation that sent
them:

- `content_id` is a SHA-256 over the manifest's bundle names and hashes in
  ordinal order. Every installation on the same content set reports the same
  value, which is what makes it useful for triage and harmless for privacy. It
  is the one field copied out of a file verbatim rather than composed by the
  collector, so it is exactly as safe as the manifest: a manifest the build
  produced holds a digest, and a hand-edited one holds whatever was typed there.
- `content_manifest` and `content_incomplete` are booleans and carry nothing.
- `skipped_bundles` holds bare archive file names. The mount gate is given the
  file name with every directory component already stripped, and each name is
  then run through the same sanitizer as every other name in the report, so no
  path can reach it even when the bundle was found through an aliased root.

The archive rows the bundles add to `content.archives` follow the existing rule
for archives: file name, size and timestamp, no path.

## Go receiver contract

The receiver treats every ZIP as untrusted input without restricting its payload:

1. Limit the compressed upload to 5 MiB and the expanded total to 16 MiB.
2. Accept arbitrary entry names, nested paths, duplicates and payload types; the
   server never extracts entries to the filesystem.
3. Stream every entry once and reject invalid ZIP structure, unsupported
   compression, size mismatches or checksum failures.
4. Scan a root `report.json` on a best-effort basis when present. Missing,
   malformed, partial and unknown-schema manifests remain valid uploads.
5. Bound every scanned metadata value before storing or displaying it.
6. Derive a crash category only from a recognized `type: crash`; otherwise use
   the backward-compatible manual category.
7. Store the original ZIP unchanged so future inspection and symbolization can
   be repeated.

The current validated manual and crash reports are approximately 58-61 KiB. That
range was measured on an installation without content bundles. Every mounted
bundle adds one `content.archives` row of roughly 110 bytes before Deflate, and
the rows differ only in their hash, size and timestamp, so they compress well: a
content set in the tens of bundles moves the packed report by well under a
kilobyte, and the range above still describes it. Re-measure on a content-bearing
installation before quoting a figure, rather than adjusting this one by
arithmetic.

The upper limits leave room for a much larger real-world log while remaining
small enough for Discord transport.

## Validation

The release implementation passed:

- a hidden manual report from a loaded Dead Air save;
- a real unhandled access violation in a release build;
- ZIP and JSON parsing;
- `MDMP` signature and dump SHA-256 validation;
- ASCII and UTF-16 privacy scans of every non-save ZIP entry;
- 13-to-10 report rotation while retaining the newest report;
- a CMake x64 `Release` build with warnings treated as errors.

Every entry above was recorded before the content fields existed. The privacy
scan and the size measurement have not been re-run against a content-bearing
installation. The run that matters is one with a refused bundle, so that
`skipped_bundles` is not empty: an empty array proves nothing about how a name
is sanitized.
