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
  hashes for diagnostically important files.

The developer must retain the matching PDB files for public builds. The report
contains enough `build + module + RVA` information to symbolize a stack without
shipping PDB files to players.

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

The current validated manual and crash reports are approximately 58-61 KiB.
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
