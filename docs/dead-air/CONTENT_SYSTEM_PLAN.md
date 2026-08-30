# PART 1 — COMPLETENESS CRITIQUE: what the plan never mentions

Verified against the real tree (`D:\Games\Dead Air\DeadAir-x64`) and the real install (`D:\Games\Dead Air\database` — 18 archives, 12 GB).

---

### G1. An entire second install/uninstall system exists and is not in the plan

`packaging/dead-air-x64/Install-DeadAir-x64.ps1` (229 lines), `packaging/dead-air-x64/Uninstall-DeadAir-x64.ps1` (135 lines), and `tools/package/build_dead_air_x64_package.ps1` (103 lines) form a complete parallel installer with its own state file `.dead-air-x64\install-state.json`, its own backup directory, and — critically — its own `package-manifest.json` with a `DatabaseFiles` array that today carries `xtra_dead_air_x64.xdb0` and verifies it by SHA-256 on install *and* on uninstall.

Consequences the plan never addresses:
- Owner rule 9 ("the uninstaller must also remove content") has **two** uninstallers. Only one is in the plan.
- An installation made by `Install-DeadAir-x64.ps1` has no `port-version.txt`, no `managed-files.txt`, and would get no `content-manifest.txt`. It is invisible to every mechanism the plan designs, and would boot with the mount gate refusing everything.
- `Uninstall-DeadAir-x64.ps1` throws when a `DatabaseFiles` entry's hash changed — content bundles change *name* per version, so this path would either orphan them or refuse.

Separately, `Setup_Manual.zip` (built by `New-UpdateArchive`, `build_dead_air_x64_installer.ps1:190-244`) is documented in `AUTO_UPDATE.md:8-18` as a first-class manual install. Extracting it produces a runtime + a `content-manifest.txt` + **zero bundles**, with no installer to fetch them. The plan never says what that install does on first launch, nor how big the first-launch download is, nor that `README_RU.md` must warn about it.

### G2. `ManagedFiles` is a live tripwire and the plan walks past it

`BuildManagedFiles` (`DeadAir-x64.iss:378-394`) feeds three consumers:
- `RemoveObsoleteManagedFiles` (`:401-432`) — deletes previous-install files the new list omits;
- `PrepareOriginalX86Backup` (`:471-522`) — **copies every managed file that exists** into `.dead-air-x64\backup-x86`;
- `create_backup` in the updater (`DeadAirUpdater.cpp:596-646`) — copies the whole scope on every update.

The plan adds `content-manifest.txt` to the managed list (fine, 2 KB) and says nothing about bundles. If bundles ever land in `managed-files.txt`, the first x86 backup copies 3-5 GB and every update copies 3-5 GB. If they never do, `RemoveObsoleteManagedFiles` can never touch them — which is correct, but must be *stated as a rule*, because the natural instinct is to add them.

### G3. `src/xrCore/xrCore.def` — the build breaks and nobody noticed

`xrCore.dll` is a shipped runtime file (`packaging/dead-air-x64/installer/runtime-files.txt`, line 28) and its exports are a **hand-maintained 1515-line ordinal list** (`LIBRARY xrCore / EXPORTS / <decorated-name> @<ordinal>`), applied when `WIN32 AND BUILD_SHARED_LIBS` (`src/xrCore/CMakeLists.txt:3-5`). Every new `ContentPin::` symbol that `xrGame` calls needs a hand-written `.def` entry with a fresh ordinal, or the link fails. The plan, all three reviews, and correctness-S15's "one .cpp compiled into both targets" fix all ignore this. (S15's fix is also incomplete: compiling a source file into xrCore does not *export* it.)

### G4. There is no release runbook

The plan lists new scripts and never writes the ordered sequence. The ordering is not obvious and is load-bearing:

- The **content release must be published before the game release**. A game release whose `content-manifest.txt` points at assets that do not exist yet is a broken release for every install made in that window — and `PROJECT_RULES.md` §11 requires the user's permission for each publish, so the window is real, not theoretical.
- `build_dead_air_x64_installer.ps1:38-43` refuses to build when `-PortVersion` disagrees with `ProductVersion.h`. There is no equivalent gate tying `content-manifest.txt`'s `version=` to either.
- Nothing says what to do when content publishes and the game release is then abandoned (answer: nothing — the tag is orphaned and, per the never-delete rule, stays).

### G5. No QA story for the 5 GB you cannot test

`Test-ContentFlow.ps1` is synthetic — correct, but insufficient. Missing entirely:
- a **scale rehearsal**: N GB of incompressible synthetic bundles through the real code once, to measure cold-hash time, FS metadata growth (`FS: 63627 files cached 36 archives, 92088Kb` today → +30-60 MB and proportional `_initialize` time at 20-40k content files), commit time, and peak disk;
- a **real-GitHub probe** — one small bundle in a throwaway tag to verify `Range`/302/`digest`, which the plan defers to "first task of Batch 3" without saying *how*;
- the explicit statement that the full multi-GB path is never exercised end-to-end in CI and what stands in for it.

### G6 / G7. No migration story for the existing install base, and no version floor

Every player is on 1.4.x with the 40 binary files inside `xtra_dead_air_x64.xdb0`. The first content release moves them *out*. Between `synchronize_payload` writing the smaller compat archive and the content commit, the installation is genuinely missing assets. And `RestartAndApply` (`UpdateService.cpp:1319-1331`) runs the **installed, pre-content** `DeadAirUpdater.exe`, whose `parse_arguments` (`:135-180`) silently ignores unknown arguments — so `--content-commit` is a no-op on the first content release for everyone. The plan has no floor rule, no "commit before arming", and no note in the release text or `README_RU.md` that this one update is larger.

### G8. `appdata\vfs-index-*.cache` leaks, measurably

Real state on this machine: **108 cache files, 27 MB**, keyed by `path_crc32(A.path)` (`LocatorAPI.cpp:594`), never deleted. Every renamed bundle mints one and abandons the old. Neither `RemoveControlMetadata` (`iss:697-706`) nor `[UninstallDelete]` (`iss:108-112`) touches `appdata\` at all.

### G9. QA fixtures beyond one line-edit

`tools/qa/Test-UpdatePatchFlow.ps1` tree-compares a synthetic installation; a content-bearing install changes what "equal trees" means. `tools/qa/Run-SerializerCompatibilityQa.ps1` and the `_qa\lightdiag` runbook both build isolated roots that will now need a content pin or the mount gate silently empties `database\`.

### G10. Diagnostic reports

`capture_content_snapshot` (`CrashReport.cpp:1683-1698`) is extended in §G4 of the plan, but `docs/dead-air/DIAGNOSTIC_REPORTS.md` is never updated, and the privacy QA case ("no user name, computer name, profile path, game path") is not re-run. Bundle names are safe; the manifest *path* and skipped-reason strings must go through the same `Sanitizer`.

### G11. Localisation

One string key (`ui_mm_content_incomplete`) is named. A repair dialog needs ~14: title, body, two buttons, progress line, "bundle N of M", disk-space, network, hash-mismatch, delta-rejected, manifest-missing recovery text, restart-required, and the play-refusal console line. All cp1251 in `configs/text/rus/dead_air_x64.xml`, transcoded by the compat script (`dead_air_x64_compatibility_archive.ps1:58-69`). And the project ships only `rus/` here — the same single-language rule as `ui_update.xml` must be stated.

### G12. No console commands, no QA switches

Every comparable subsystem has them (`dar_update_check`, `dar_major_update_notice`). Content has none — no way to force a re-verify or open repair for a support case without deleting files.

### G13. The compat archive ↔ bundle relationship is asserted, not specified

The whole split rests on an asymmetry nobody wrote down: the compat archive is **version-coupled to the engine** (`build_dead_air_x64_package.ps1:28-29` says so outright — shaders and scripts), bundles are **not**. `level_ver = 1.4.0` stays hand-bumped in `xdb_userdata.ltx` while bundle headers are derived. Disjointness is a build-time assertion only; the runtime consequence of a violation (content silently wins, it sorts later) gets no log line.

### G14. Cache-before-network is implied, never stated

E3's "if the user quits, nothing is lost" only works if the resolver consults `content-cache\<sha256>` before opening a socket. That rule appears nowhere.

### G15-G18. Smaller, all real

- **G15.** The play gate is placed on menu buttons; `execUserScript()` (`x_ray.cpp:300`, called at `:389`) runs `user.ltx` as console commands, and the project's own smoke recipe uses `server(...)` — which never touches a button. Gate belongs at `CLevel::net_Start` (`xrGame/Level_start.cpp:26`), the single chokepoint for `CCC_Start` (`xrEngine/xr_ioc_cmd.cpp:247`), `-start`, new game, load and transition.
- **G16.** No `MODDING.md` content section; no reparse-point guard before the wildcard sweep of `database\` (a junctioned `database\` shared between two installs loses both).
- **G17.** No running-game check on the Inno uninstall path. `[Setup]` has no `AppMutex`/`CheckForMutexes`; `CloseApplications=yes` only drives Restart Manager over `[Files]` entries and bundles are not `[Files]` entries. The *manual* PS1 scripts do check for a running `xrEngine` — the Inno path does not.
- **G18.** §H says a published content tag is never deleted; it never says an asset is never *replaced*. That omission is exactly the S1/S16 permanent-redownload disaster. No append-only hash ledger either.

### G19 / G20 / G23 / G24. Rules, disk, checksums, backups

- `PROJECT_RULES.md:125` ("Assets travel inside the install and update payload") is contradicted by the design, and the plan claims §4 needs no change. §11's asset list is stale (two assets; reality is four). Both need proposed wording, not a flag.
- `[Setup]` has **no** `ExtraDiskSpaceRequired` and `Compression=none`. The wizard will show ~200 MB required and then die 25 minutes into a 4 GB fetch.
- `SHA256SUMS.txt` (`build_dead_air_x64_installer.ps1:440-455`) covers the installer-files artifact. Bundles are not in it and cannot be — say so, and say what covers them instead.
- Nothing tells the owner what must now be backed up forever: the bundle cache (losing it plus an unreachable release is a *correctness* event under the reuse ladder) and the authoring tree.

---

# PART 2 — FINAL CONSOLIDATED IMPLEMENTATION PLAN

## 0. Invariants

1. `{app}\database\` contains only complete, hash-verified bundle files whose names the installed manifest declares. Nothing partial, nothing intermediate, nothing from another version.
2. A bundle's **filename carries the SHA-256 of the packed file**, first 16 hex. Name ↔ bytes is 1:1, forever. (Fixes correctness-S16 / ops-S1. Constraint 4 survives: unchanged content → cache hit → identical bytes → identical name → no re-upload.)
3. Bundles enter `database\` only by `MoveFileExW` from `content-cache\`, after the SHA-256 matched the manifest. A delta therefore cannot produce a `database\` file — constraint 8 is structurally unreachable, not merely checked.
4. **Add before delete, always.** Obsolete bundles are demoted to the cache, never deleted inside a commit.
5. **Bundles are never `ManagedFiles`.** Their entire lifecycle belongs to the content commit and GC. (G2)
6. Content has no opt-out, no skip switch, and no build flag that omits it. (Constraints #1, #2)
7. The trust root is the installed `{app}\.dead-air-x64\content-manifest.txt`, which arrived inside a hash-verified payload. No manifest path is ever accepted on a command line. (Correctness-S2)

---

## A. Formats

### A1. `content-manifest.txt` — schema `dead-air-refined.content/1`

Location: `{app}\.dead-air-x64\content-manifest.txt`, shipped inside the game payload; also published as the release asset `Dead-Air-Refined-<V>-content-manifest.txt` on the main repo.

```
schema=dead-air-refined.content/1
version=1.5.0
content-id=<64 lowercase hex>
repo=MMadmer/Dead-Air-Refined_Assets
[bundles]
<sha256>\t<size>\t<bundle-file-name>\t<release-tag>
[deltas]
<sha256>\t<size>\t<delta-asset>\t<release-tag>\t<base-bundle>\t<base-sha256>\t<base-size>\t<target-sha256>
```

Rules (parser shape copied from the update manifest deliberately):
- UTF-8 no BOM, `\n`, trailing newline, written with `[Text.UTF8Encoding]::new($false)`.
- Header lines 1-4 in exactly this order, exact-prefix match, no whitespace tolerance.
- `version=` obeys `valid_version()` (`DeadAirUpdater.cpp:104-121`).
- Hashes: exactly 64 hex, **lowercase enforced on parse** (closes the `matches_payload:492` class of bug).
- Sizes via `std::from_chars`, whole field consumed.
- Bundle names must match `^xtra_dead_air_x64_content_[a-z0-9_]+_[0-9]{2}_[0-9a-f]{16}\.xdb0$` — no separators, no `:`. This regex is the delete-authority for the uninstaller and the GC.
- `[bundles]` exactly 4 fields; `[deltas]` exactly **8**. `base-size` is new and mandatory: eligibility must be self-contained, because when a player skips versions the base bundle is frequently absent from the *installed* manifest and there is nothing to read its size from. (Correctness journey N→N+2.)
- **`[deltas]` is cumulative.** Every historical edge published for a bundle that still exists stays in the manifest (~150 B/edge; 20 bundles × 20 releases ≈ 60 KB). The resolver does a shortest-path walk from whatever base the player actually has. Publishing cost is unchanged: one delta per changed bundle per release. (Ops-S3.)
- Duplicate bundle names rejected; duplicate `(base-sha256, target-sha256)` pairs rejected.
- `content-id` = SHA-256 over `<name>\n<sha256>\n` per bundle, sorted by name, UTF-8.
- **`repo=` from a network-fetched manifest is informational only.** The allowed asset host is pinned in code; a mirror change is a shipped-code change. (Ops-S10.)

### A2. `content-state.txt` — advisory cache

```
schema=dead-air-refined.content-state/1
content-id=<64 hex>
<sha256>\t<size>\t<mtime-decimal-FILETIME>\t<bundle-name>
```
Trusted only on an exact (name, size, mtime) match whose recorded hash is what the manifest wants — the `LoadVfsIndexCache` model (`LocatorAPI.cpp:89-101`). **Never invalidated on a `content-id` change** — versions share bundles, and invalidating would rehash 4 GB on every update. Deleting it costs a rescan, never correctness.

### A3. `content-incomplete.txt` — the latch (constraints #10, G-critical)

```
schema=dead-air-refined.content-incomplete/1
version=<version>
reason=install-commit|update-commit|repair-commit|verify-failed|manifest-missing|manifest-invalid
time=<decimal FILETIME>
```
Written **before the first mutation** of `database\` and before any install-time bookkeeping that can leave a half-state. Deleted **only** after a full resolver pass reports zero jobs and zero obsolete. `ContentPin`, `ContentService`, the play gate and the repair dialog all key off it. A mid-commit crash or a failed `ssPostInstall` therefore cannot present as healthy.

### A4. `.darpatch` v1

```
u8[8] "DARPATCH" | u32 version=1
u8[32] baseSha256 | u64 baseSize
u8[32] targetSha256 | u64 targetSize
u64 opCount
opCount × { u8 kind; COPY: u64 baseOffset,u64 length | INSERT: u64 length,u8[length] }
```
Structural validation before one output byte: every COPY satisfies `baseOffset + length <= baseSize`; Σ op lengths == `targetSize`; `opCount` and each INSERT length bounded. Builder: content-defined chunking (gear rolling hash, min 8 KiB / avg 32 KiB / max 128 KiB), index base chunks by SHA-256, emit COPY on match, merge adjacent COPYs. Delta asset name: `content_<group>_<shard2>_<baseHash16>_to_<targetHash16>.darpatch`. Skip when `delta > 0.35 × target` (not 0.75 — a 225 MB delta against a 300 MB target buys nothing and adds a failure path).

### A5. `groups.ltx` — one copy, in the engine repo only

`packaging/dead-air-x64/content/groups.ltx` is the single source of truth. The assets repo records it *inside* `index/content-<tag>.txt` as audit, never as a second editable copy. (Correctness-S21.)

```ini
[groups]
textures_ui
textures_world
meshes
sounds
anims
misc

[textures_world]
root            = textures
target_shard_mb = 250

; Append-only. A line is NEVER edited or removed - editing one renames a bundle
; and forces every installed player to re-download it.
[assign]
textures_world/act   = 00
textures_world/wpn   = 01
...
```

**Sharding is an explicit append-only directory→shard table, not `hash % shard_count`.** Reasons, both fatal to the hash scheme:
- `shard_count` is *guaranteed* to be bumped: Batch 8 moves 19,204,670 B of content, which is `shard_count=1` per group at a 250 MB target; growing to 5 GB needs ~12-20 shards. Under `% n`, that bump is a full re-download of every group for every player. (Ops-S4.)
- Real edits are thematic. A coherent 20 MB change spread uniformly over 12 hash shards republishes ~3 GB. (Ops-S5.)

Assignment key is the first path component under `root`, so `.dds`/`.thm`/`.seq` companions always co-locate (a free 2× win over hashing full paths). New directories are appended by the builder to the currently smallest shard and **written back into `groups.ltx`** so the change is reviewable and committed. If one assigned directory alone exceeds 1.5 × target, the builder subdivides it by second component and records those lines the same way — documented escape hatch, never automatic re-balancing.

---

## B. Bundles

STORE-packed `.xdb0`, same tool as the compat archive:
```
converter.exe -pack -xdb -xdb_ud <generated-userdata.ltx> -out <bundle.xdb0> <stage-root>
```
Generated header: `auto_load = true` (read at `LocatorAPI.cpp:713-725`), `entry_point = $fs_root$\gamedata\` (must begin with `$` — `R_ASSERT2` at `:552`), `level_ver = <content digest, full 64 hex>`. The compat archive's hand-bumped `level_ver = 1.4.0` stays hand-bumped; only bundles derive it. (G13, OPEN-10.)

**Two hashes, both needed, different jobs:**
- **content digest** — computed over members *before* packing (`epoch=1\n`, `header=<sha256 of userdata.ltx>\n`, then `<lowercase-vpath>\n<sha256>\n` sorted). This is the **build-cache key only**: it lets the builder decide not to pack. Never appears in a filename.
- **file hash** — SHA-256 of the finished `.xdb0`. Appears in the **filename** (16 hex) and in the manifest (64 hex).

Naming: `xtra_dead_air_x64_content_<group>_<shard2>_<fileHash16>.xdb0`, all lowercase (`Recurse` sorts raw `findData.name` with `strcmp` *before* `ProcessOne` lowercases, `LocatorAPI.cpp:953-963`, `:789`).

**Mount position, verified against the real install** (`xtra.xdb0`, `xtra_da_inventory_sort.xdb0`, `xtra_dar2.xdb0/1`, `xtra_dead_air_x64.xdb0`): at index 17, `.`(0x2E) < `_`(0x5F), so content sorts immediately after the compat archive and last among today's `xtra_*`. Last `Register()` wins unconditionally (`:346-356`). Today's effective priority is preserved exactly; loose `gamedata\`, JSGME and XMS still override content because `$game_data$` is listed after `$arch_dir$`.

**Wording correction to carry into the docs:** content does *not* sort after every conceivable third-party archive — a hypothetical `xtra_zzz.xdb0` still beats it. That is the desired outcome (a mod overriding content is a mod working), so fix the sentence, not the design.

Flat in `database\`. `database\content\` is rejected: `$arch_dir_patches$` is `recurse=false` in the shipped `fsgame.ltx`, so it would not even mount; and depending on a flag in a user-editable file is wrong regardless. Overlay deltas are rejected: an archive can only add or replace a vpath, there is no tombstone, so content deletion across versions would be impossible.

---

## C. The three delta gates (unchanged in substance, fixed in failure handling)

- **Gate 1 — eligibility (resolver).** Base file exists, `file_size == base-size` **from the delta record** (not from the installed `[bundles]`), SHA-256 == `base-sha256`, taken from the state cache only on an exact (name,size,mtime) match. Ineligible → full bundle, silently, no marker.
- **Gate 2 — re-hash the base immediately before applying.** Time passed since Gate 1.
- **Gate 3 — authoritative.** Output is written to `content-cache\<target-sha256>.part`, hashed as written, and must equal both the header's `targetSha256` and the manifest's `[bundles]` hash. Only then renamed to `content-cache\<target-sha256>`.

**Blacklisting is integrity-only.** `rejected-deltas.txt` receives an entry **only** when a hash verdict fails with every I/O call having succeeded. `ERROR_DISK_FULL`, read/write errors and cancellations are retryable and are never recorded. (Correctness-S8 — otherwise a transient disk-full permanently upgrades a 20 MB delta into a 400 MB download.) The file is per-delta, and is cleared on a successful full commit of a new content-id.

For a chained multi-hop apply, free each intermediate immediately after the next hop verifies; peak cache use is 2 × the largest target on the path.

---

## D. Resolver

```
Resolve(manifest, gameDir) -> Plan
  if manifest absent or invalid:                       // constraints #3
      latch("manifest-missing"|"manifest-invalid"); return RecoveryPlan
  CacheGC(cacheDir, manifest)                          // ops-S7, before anything else
  for b in manifest.bundles:
      p = database/b.name
      if !regular_file(p):                  missing += b; continue
      if size(p) != b.size:                  corrupt += b; continue
      if state vouches (size,mtime,sha):     satisfied += b; continue
      if sha256_file(p) == b.sha256:         satisfied += b; state.record(b)
      else:                                  corrupt += b
  obsolete = files in database\ matching the strict pattern that manifest does not name
  for b in missing + corrupt:
      if cache holds a verified <b.sha256>:  jobs += CommitJob(b)      // G14: cache before network
      else if bestDeltaPath(b) exists:       jobs += DeltaJob(path, b)
      else:                                  jobs += FullJob(b)
  if jobs empty and obsolete empty:  clear latch; return   // constraint 6: not one socket opened
```

`CacheGC`: delete every cache entry that is not a target hash of the current plan and not a hash in any manifest in play; delete `*.part` older than 14 days; enforce an LRU size cap; log reclaimed bytes.

**Free space** (correctness-S7, ops-S8):
```
required = Σ target size of every job
         + Σ delta asset size of every delta job
         + max(target size over jobs)          // deltas apply one at a time
         + 512 MiB
```
checked with `GetDiskFreeSpaceExW` against the volume holding `{app}`. **Behaviour at runtime is specified, not left to the error handler**: insufficient space keeps the installation in the incomplete state, keeps the repair dialog up showing required vs free, and keeps play blocked. Repair never resolves into a playable state without the content.

---

## E. Acquisition

### E1. Downloader (`content_download.cpp`)

Modelled on `download_update` (`UpdateService.cpp:1077-1163`) with these differences:
- **Content-addressed part file** `content-cache\<expected-sha256>.part` — a stale part for a different target is impossible by construction.
- **Resume.** Truncate the last 1 MiB before re-hashing the prefix (NTFS can leave a zero-filled tail after power loss and the existing `FILE_ATTRIBUTE_TEMPORARY` open does not flush). Send `Range: bytes=<n>-`. `206` → validate `Content-Range` **start offset and total length** against expectations, then append. `200` → server ignored the range: truncate, reset hash, restart. Anything else → job error, keep the part.
- **Complete-transfer hash mismatch → delete the part, restart once from 0; a second mismatch is a hard job error.** (Correctness-S9 — otherwise every launch resumes onto a proven-bad prefix forever.)
- **Retry with backoff**: 5 attempts per job, 1/2/4/8/16 s, resuming each time, before anything surfaces to the UI. **Idle timeout**: no bytes for 120 s fails the attempt.
- **Concurrency**: 3 simultaneous asset downloads (OPEN-3). A single stream from `objects.githubusercontent.com` plateaus at 20-40 Mbps regardless of the player's line; this is the largest single lever on install time.
- **Re-acquisition cap**: 3 attempts per asset per session, then stop with a real error naming expected vs actual hash. No unbounded loop over 300 MB. (Ops-S1.)
- 1 MiB buffers (the updater's 64 KiB is fine at 20 MB, wasteful at 4 GB).
- URL: `https://github.com/<pinned-repo>/releases/download/<release-tag>/<asset>`. No `api.github.com` call at all — sidesteps `MaximumApiResponseBytes = 4 MiB` (`UpdateService.cpp:48`), the 60 req/h budget, and the 1000-asset enumeration.
- QA override `DAR_QA_CONTENT_BASE`, accepted only under `-qa_update` and only when it starts with `http://127.0.0.1:` or `http://localhost:` — same shape as `qa_api_url` (`:530-543`). It redirects the source; it never skips the fetch, and hashes still gate the commit. **No companion "skip content" flag is ever added.**

### E2. Commit (`content_commit.cpp`) — two phases, never one

**Phase 1 (transactional, add-only).** `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` each verified cache file into `database\`, retrying 5× with 200 ms backoff (Defender holds a freshly written 300 MB file open). Then rewrite `content-state.txt`. Because bundle names are content-addressed, every commit is **create-only** — it can never target a name the running engine has mapped, and `archive::open` (`LocatorAPI.cpp:634`) holds `FILE_SHARE_READ|FILE_SHARE_WRITE` with no `FILE_SHARE_DELETE` plus a live mapping, so overwriting a mounted bundle is impossible anyway.

**Phase 2 (deferred, idempotent).** Obsolete bundles are **moved to `content-cache\<their-sha256>`** (a same-volume rename, free), never deleted, and only on a launch where the on-disk manifest and bundle set verify consistent. This makes a downgrade-then-upgrade a zero-byte operation and means a failed commit can never destroy bundles that `create_backup` (`DeadAirUpdater.cpp:596-612`) does not snapshot and `restore_backup` cannot restore. (Correctness-S5, S10, S11.)

**Volume check at resolve time**: if `.dead-air-x64` and `database` are on different volumes (junction), `MoveFileExW` without `MOVEFILE_COPY_ALLOWED` fails outright — detect it during `Resolve` and fail loudly with a clear message, not at commit under the updater's 5-minute wall.

### E3. Install-time (Inno)

- Fetch runs **immediately after `ValidateSelectedDirectory` and before any backup, before `RemoveObsoleteManagedFiles`, before `install-mode.txt`/`managed-files.txt`/`port-version.txt`**. A failed fetch today aborts *after* files from the previous install were already deleted and after `port-version.txt` claims the new version. (Correctness-S3.)
- **A custom wizard page before `wpReady`**, not `CreateOutputProgressPage`: it needs a real **Cancel**, and a 4 GB fetch is 18-55 minutes. Cancel sets a flag file the fetcher polls; the cache is kept (it is the resume state) and a `content-cache\README.txt` names it so an abandoned install does not orphan 4 GB in a directory the user will never find.
- `DeadAirContent.exe` is a `dontcopy` copy of the fetcher (same source as `DeadAirUpdater.exe`, `--content-*` modes) extracted to `{tmp}` — the installed updater does not exist yet at this point.
- **Termination is detectable**: the fetcher holds a named mutex while alive and writes `content-cache\content-fetch-result.txt` (exit code + message) as its last act. The poll loop fails on "mutex gone and no result file" and on "no progress-file mtime change for N seconds". (Correctness-S13.)
- **No `/CONTENT=` switch, ever.** `#ifndef MaintenanceOnly` is already structural — the maintenance exe is a separate compile of the same `.iss` and physically cannot contain the block. The updater's `/CONTENT=no` is dropped too. Positive assertion instead: a failed `ExtractTemporaryFile('content-manifest.txt')` aborts the install. (Constraints #1.)
- `[Setup]` gains `ExtraDiskSpaceRequired` sized for the content set, and `NextButtonClick` at `wpSelectDir` does a `GetDiskFreeSpaceEx` check. (Ops-S8, G20.)
- `[Setup]` gains `AppMutex` / `CheckForMutexes` naming the engine, so install and uninstall refuse while the game runs. (G17.)
- Commit in `ssPostInstall`; a failure there is a failed install — the latch was written before the first mutation so the next launch cannot mistake it for healthy.

### E4. Update-time — content commits **before** the update is armed

Content is never put in the update ZIP: 4 GB blows `MaximumExpandedBytes = 1 GiB` (`DeadAirUpdater.cpp:43`) and `MaximumFiles = 1024` (`:42`), and stage-2's hard 5-minute `wait_for_process` (`:191`, `:814`) would return 20 with the cache uncleaned and the game never relaunched.

Sequence:
1. `UpdateService` finds an update as today.
2. On accept, fetch `Dead-Air-Refined-<V>-content-manifest.txt` (a few KB) from the main repo release. **Verify it against its entry in the target's `update/2` manifest before parsing a single bundle line.** Any failure (missing asset, null digest, no network) is silent: skip, let the post-restart repair path handle it. Never block the runtime update on it. (Correctness-S2, ops-S10.)
3. `ContentService::Resolve` against the installed `database\`. Usually nothing to do.
4. Jobs run as one aggregate progress with the runtime archive; deltas apply in the game process on the worker thread, into the cache.
5. **The update is not armed until every content job is verified into the cache** (constraints #5). Then `ContentService` runs **Phase-1 commit in the game process** — create-only, safe against the running engine — and only then does `RestartAndApply` launch the updater.
6. The updater's `--content-commit` remains, reading **only** `{app}\.dead-air-x64\content-manifest.txt` after `synchronize_payload`. It is a fallback and an installer path, never a correctness dependency — which is what makes the first content release work on a pre-content installed updater that silently ignores unknown arguments. (Correctness-S1, G7.)
7. New exit codes: **26** content commit failed (backup restored, like 18), **27** content cache unusable.

---

## F. Startup integrity

### F1. Mount gate — `src/xrCore/Content/ContentPin.{h,cpp}`

```cpp
namespace ContentPin
{
    void Load(pcstr fsRoot);
    bool IsContentBundleName(pcstr fileName);
    bool ShouldMount(pcstr fileName, size_t size);
    void RecordSkipped(pcstr fileName, pcstr reason);
    const xr_vector<shared_str>& Skipped();
    bool ManifestLoaded();
}
```

`Load` at the top of `CLocatorAPI::_initialize` (`:1112`). `ProcessArchive` consults it before opening — and **its signature changes to `ProcessArchive(pcstr path, size_t size)`**, with `entry.size` threaded from `ProcessOne` (`:815`); `_finddata_t::size` is 32-bit, fine at 400 MB, asserted in the builder. (Correctness-S17.)

**Soft-fail, not assert, for bundle-named archives.** `LoadArchive` does `R_ASSERT3(GetArchiveChunkSignature(...))` at `:588`, `R_ASSERT(hdr)` at `:601`, and parses `archive_file_header` with no bounds validation; `archive::open` asserts on handle, size and mapping. A single flipped bit inside a 400 MB bundle is the realistic failure and it is size-preserving, so the name+size gate does not catch it. For `IsContentBundleName` archives only: signature/header failure → `A.close()`, `RecordSkipped(name, "invalid index")`, return. This converts the one unrecoverable content failure into the repair path. (Correctness-S6.)

Two distinct log lines, per the plan's own uncertainty 8: `unrecognised bundle-shaped archive` (name matches the pattern, manifest never heard of it) vs `stale bundle` (manifest declares this group/shard at a different hash).

**Manifest absent is the incomplete state, not a neutral one.** A pre-content install has no bundle-named files at all, so the "compatibility" justification is hollow — the branch only ever fires on a broken or tampered install. `ManifestLoaded() == false` writes the latch and routes to the repair dialog in recovery mode, which re-fetches the manifest keyed off `port-version.txt`. A compile-time constant set by the build script says "this build has content" so the branch cannot be argued away later. (Constraints #3.)

**One parser, one source file.** `src/xrContentSync/content_manifest.cpp` is compiled into `xrContentSync`, into `xrCore` (for `ContentPin`), and into the updater. Not two readers held together by a fixture. (Correctness-S15.)

**`xrCore.def`** gains exactly three appended entries with the next free ordinals (currently ~1514+), never renumbering: `ContentPin::ManifestLoaded`, `ContentPin::Skipped`, `ContentPin::StatePath`. Everything else stays internal to xrCore; the hashing helper is not exported because xrGame links `xrContentSync`, which has its own. (G3.)

### F2. Verification and repair

- `ContentService::StartVerify()` alongside `UpdateService::StartCheck()` at `MainMenu.cpp:194`, on a worker thread. Cold cache = one pass over 3-5 GB (~3 s NVMe, ~8 s SATA SSD, ~34 s on a 7200 rpm HDD — log the duration); steady state = a stat per bundle.
- **`ModOptOut` gates neither content verification nor content repair.** It covers the update check and the bug report only, as `ModOptOut.h` itself says. Stated explicitly because the natural implementation reuses `UpdateService`'s HTTP layer, whose `StartCheck` returns early at `:1262`; a modded install is the *most* likely one to be missing content, and a silent no-op there would be a per-installation opt-out granted by a mod. QA case required. (Constraints #7.)
- `CUIContentWnd` (`src/xrGame/ui/UIContentWnd.{h,cpp}`), layout `gamedata/configs/ui/ui_content.xml`, **shipped in `xtra_dead_air_x64.xdb0`, never in a bundle** — same reason `ui_update.xml` lives there.
- `CMainMenu::CheckContentDialog()` at the top of the `OnFrame` precedence chain (`:602-608`), **above** `CheckCrashReportDialog()`, built on the one-shot latch of `:776-789` with the lazy-construct fallback of `EnsureBugReportDialog` (`:755-768`).
- Two actions: **Repair** and **Exit**. No "play anyway".
- **Repair always ends in a mandatory relaunch** (reuse `write_restart_command` / `launch_command`), because a repaired missing bundle is not picked up until FS re-init, and `unload_archive` is already broken (it erases one `m_files` entry and `break`s, `:734`). (Correctness-S19, OPEN-8.)
- Persistent banner reusing `DrawModOptOutNotice`'s vehicle (`:856-890`).
- `CHECK_OR_EXIT` is **not** used — content has a repair path, so the player must reach the UI.

### F3. The play gate — at the level, not the button

`CLevel::net_Start` (`xrGame/Level_start.cpp:26`) refuses while the latch is set, printing to console. That is the single chokepoint for `CCC_Start` (`xrEngine/xr_ioc_cmd.cpp:247`), `-start`, new game, load, and level transition — and it is the only placement that survives `execUserScript()` running `user.ltx` as console commands (`x_ray.cpp:300`, called at `:389`), which is exactly what the project's own smoke recipe does. Menu buttons merely reflect the state. **This is a crash guard, not a product decision**: with content unmounted a level load is a missing-asset crash, so the "menu fully usable" alternative is removed, not flagged. (Constraints #4, correctness-S20, G15.)

### F4. Console commands (G12)

`dar_content_verify` (force a full re-hash and report), `dar_content_repair` (open the repair dialog), `dar_content_state` (dump the current plan to the log). Read-only or repair-only; nothing that can skip content.

### F5. Diagnostics

`capture_content_snapshot` (`CrashReport.cpp:1683-1698`) gains `content-id`, the latch state, and the skipped-bundle list — every string through the existing `Sanitizer`. `docs/dead-air/DIAGNOSTIC_REPORTS.md` updated; the privacy scan QA case re-run. (G10.)

---

## G. Uninstaller

### G1. Delete (per-version rollback) — `DeadAir-x64.iss`

Lines: 120; 121-122; 134-137, 139; 148-158; 192-195; 454-469; 524-622; 708-747; 804-827; 829-920; 922-925; 927-941; 943-953; 955-986; 988-1021 (`InitializeSetup` → `Result := True`); 1035-1040; 1042-1049; 1176-1202; 1225, 1228, 1230-1233; 1275-1290. Plus the "delete saved versions" sub-feature that dies with `backups\`: 132, 138, 140, 197-203, 1169-1174, 1229, 1258, 1261, 1292-1294, 1344-1347.

Rollback removal is **not optional and not deferrable past the first content release**: `PrepareUpgradeBackup` snapshots `ManagedFiles`, bundles are deliberately not in it, so a version rollback would pair an old runtime with new content.

### G2. Delete the backup checkbox

`BackupPage` (129, 297-305) and `BackupParameterEnabled` (174-180). With per-version snapshots gone its only meaning would be "back up the original x86 game", and the updater passes `/BACKUP=no` on every update — so an installation never backed up would never get its x86 backup. Replace with an unconditional call guarded by the existing idempotence at `:485-489`:
```pascal
if CurrentVersionName(WizardDirValue) = 'original-x86' then
  Result := PrepareOriginalX86Backup(WizardDirValue);
```
`/BACKUP` stays accepted-and-ignored so an older installed updater keeps working.

### G3. Keep

`LoadRuntimeFiles` (328-331), `AppendString` (333-340), `StringArrayContains` (342-355), `AppendUniqueString` (357-361), `LoadManagedFilesFromControl` (363-376), `BuildManagedFiles` (378-394), `RemoveObsoleteManagedFiles` (401-432), `CurrentVersionName` (434-452), `PrepareOriginalX86Backup` (471-522), `OriginalX86BackupAvailable` (749-754), `RestoreOriginalX86Runtime` (756-802), `RemoveX64Runtime` (1023-1033), `RemoveControlMetadata` (697-706), `GetBinaryType` (160-161), `Build-MaintenanceInstaller` (build script 169-188).

`create_backup`/`restore_backup` (`DeadAirUpdater.cpp:562-680`) stay — they are the transactional rollback and the only source for patch-omitted files after the Inno pass. **Add the deletion on success**: after `DeleteFileW(patch-rejected.txt)` at `:937`, `std::filesystem::remove_all(*backup, error)`. Nothing consumes the snapshot afterwards and nothing prunes it, which is why `backups\` grows without bound.

### G4. Add — content removal, and it must fail loudly

```pascal
function RemoveContentBundles(GameDirectory: String; var Remaining: String): Boolean;
```
- Refuse if `database\` is a reparse point (a junctioned/shared `database\` would lose both installs' content to the wildcard sweep).
- Delete each `[bundles]` entry from `content-manifest.txt`; then sweep `database\` for `xtra_dead_air_x64_content_*.xdb0` and delete every match (catches a failed mid-update state).
- `DelTree(...\.dead-air-x64\content-cache, True, True, True)`.
- Each survivor gets `MoveFileEx(path, '', MOVEFILE_DELAY_UNTIL_REBOOT)` where privileges allow, is named in `Remaining`, and makes the function return `False`.

`CurUninstallStepChanged` (1319-1348) rewritten so content goes **first** — the `exit` at `:1335` on a failed x86 restore currently short-circuits everything after it:
```pascal
GameDirectory := ExpandConstant('{app}');
ContentRemoved := RemoveContentBundles(GameDirectory, Remaining);

if RestoreOriginalX86OnUninstall then
begin
  if not RestoreOriginalX86Runtime(GameDirectory) then
  begin
    MsgBox('Не удалось восстановить исходную версию игры. ...', mbError, MB_OK);
    exit;
  end;
  DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backup-x86', True, True, True);
end
else
  RemoveX64Runtime(GameDirectory);

if not ContentRemoved then
  MsgBox('Часть файлов контента удалить не удалось...' + Remaining, mbError, MB_OK);

RemoveControlMetadata(GameDirectory);
DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backups', True, True, True);
```
`RemoveControlMetadata` runs **only after** content is accounted for — deleting `content-manifest.txt` while bundles remain destroys the only record of what should have been removed. (Correctness-S12, constraints #8.)

`RemoveControlMetadata` gains `content-manifest.txt`, `content-state.txt`, `content-incomplete.txt`, `rejected-deltas.txt`.

`[UninstallDelete]` gains, before the `dirifempty` lines:
```
Type: filesandordirs; Name: "{app}\.dead-air-x64\content-cache"
Type: files; Name: "{app}\database\xtra_dead_air_x64_content_*.xdb0"
Type: files; Name: "{app}\appdata\vfs-index-*.cache"
```
The last line closes G8's leak on uninstall; the commit path additionally deletes the `vfs-index-<path_crc32>.cache` of every demoted bundle.

### G5. Dialog

`ShowUninstallActionDialog` (1079-1238) keeps the branded form, loses the choice. `RemovePatchRadio` (1158-1167) collapses into descriptive text; the caption logic at 1162-1165 survives as a label. `CreateCustomForm(500, 377, ...)` at 1096 shrinks; `MaintenanceFooterHeight = 74` stays valid. The two `MsgBox` confirmations at 1304-1316 already say the right thing and stay.

---

## H. Assets repo — `MMadmer/Dead-Air-Refined_Assets`

In git (kilobytes, forever): `README.md`, `CONTENT_RULES.md`, `index/content-<tag>.txt` (verbatim published manifest, with the `groups.ltx` used, inlined as audit), `index/source-<tag>.txt` (`<sha256>\t<size>\t<source-relative-path>` per authored file), **`index/bundle-hashes.txt`** (append-only `<bundle-name>\t<sha256>` ledger; the builder hard-fails on any contradiction), `.gitattributes` (`* -text`, explicitly no LFS), `.gitignore`. **No LFS** — LFS bandwidth is metered, release-asset bandwidth is not (2 GiB/asset, 1000 assets/release, no total or bandwidth limit).

Releases: tag `content-<MAJOR.MINOR.PATCH>` — the game version that first published these assets. **One tag per content set, never reused, never deleted, and no asset in a published tag is ever replaced.** `gh release upload --clobber` is forbidden; the publisher refuses to run if an asset name already exists with different bytes. (G18, ops-S1.)

Publishing — `tools/package/publish_dead_air_x64_content.ps1`:
1. Create the tag's release if absent.
2. Upload one asset per `gh` invocation inside a retry loop; skipping already-present assets **is** the resume mechanism (`gh release upload` has no resume of its own).
3. Verify without re-downloading via `gh api repos/MMadmer/Dead-Air-Refined_Assets/releases/tags/<tag> --jq '.assets[] | [.name,.digest] | @tsv'` — `gh release view --json assets` does **not** expose `digest`. Compare `sha256:<hex>` against the manifest.
4. Write `index/*`, commit, push.
5. Refuses without explicit `-Confirm` (`PROJECT_RULES.md` §11).

---

## I. Build tooling

- **`tools/package/dead_air_x64_content_bundles.ps1`** (new, core). Params `-SourceRoot -GroupsFile -BundleCache -PreviousManifest -OutputManifest -ConverterPath -WorkRoot -PortVersion -ReleaseTag -AllowRepack`. **There is no `-SkipContent`.** Steps: hash the source in parallel; assign to group + shard via `[assign]`, appending new directories and writing `groups.ltx` back; compute the content digest per bundle; **reuse ladder** — (a) cache hit on content digest whose file hash matches `-PreviousManifest` → reuse verbatim; (b) not cached but in `-PreviousManifest` → `gh release download`, verify, reuse; (c) otherwise pack. `-AllowRepack` required to force (c) over (a)/(b) and it warns. Round-trip verify **replaced with a single parallel .NET hasher** (`ForEach-Object -Parallel` over `SHA256.HashData`) — the compat script's per-file `Get-FileHash` loop was written for 170 files and would take 10-25 minutes at 20-40k. Add a **headless mount pass** over each freshly packed bundle (a malformed generated `userdata.ltx` is a startup `R_ASSERT2` at `LocatorAPI.cpp:552` that no per-file round-trip catches). Assert no member ≥ 4 GiB (`file::size_real` is `u32`, `LocatorAPI.h:91`). **Disjointness assertion**: no vpath in two bundles, nor in a bundle and `xtra_dead_air_x64.xdb0`.
- **`tools/package/dead_air_x64_content_delta.ps1`** + **`tools/package/src/dar_delta.cpp` → `build\installer\DarDelta.exe`**, compiled by the existing `Build-NativeHelpers` pattern (`build_dead_air_x64_installer.ps1:139-167`).
- **`tools/package/dead_air_x64_compatibility_archive.ps1`** — extract `New-XdbArchive` (`-SourceRoot -OutputPath -UserDataPath -Overrides -TextFixups -ConverterPath -WorkRoot`); `New-DeadAirCompatibilityArchive` becomes a thin wrapper preserving today's hardcoded values exactly (name `:21`, overrides `:25-29`, cp1251 fixups `:58-79` — these are compat-specific and must never run on a texture bundle). Byte-identical output proven by SHA-256 before and after.
- **`tools/package/build_dead_air_x64_installer.ps1`** — new params `-ContentSourceRoot -ContentBundleCache -PreviousContentManifest -ContentReleaseTag`. New ordered step between `Build-CompatibilityArchive` (`:407`) and `Build-MaintenanceInstaller` (`:408`). A missing or unverified `content-manifest.txt` is a **hard throw**, alongside the existing runtime-files consistency check (`:393-403`), extended to assert every `[bundles]` entry exists in the cache and hashes correctly, and that the manifest's `version=` equals `-PortVersion`. `New-UpdateArchive` copies the manifest into the payload near `:215`. `SHA256SUMS.txt` covers the manifest; bundles are covered by the manifest itself and by the assets-repo digest verification — state this, since `SHA256SUMS.txt` structurally cannot cover them (G23).
- **`tools/package/build_dead_air_x64_package.ps1`** and the two `Install-/Uninstall-DeadAir-x64.ps1` scripts: **retired** (OPEN-1). They produce an installation with no `port-version.txt`, no `managed-files.txt` and no content pin — one the content system cannot see and where constraint 1 cannot be honoured. The Inno installer gains a one-line check: an existing `.dead-air-x64\install-state.json` is reported and the user is directed to reinstall via Setup.

---

## J. QA

- **`tools/qa/Start-ContentAssetMock.ps1`** — loopback static server on the `releases/download/<tag>/<name>` shape, with `Range` support, a `-NoRange` mode, a `-Throttle` mode, and a `-DropAfter <bytes>` mode.
- **`tools/qa/Test-ContentFlow.ps1`** — synthetic end-to-end in the shape of `Test-UpdatePatchFlow.ps1`, driving the real binaries. Proves: nothing downloaded when everything matches (zero sockets); only the changed bundle downloaded; delta output byte-identical to the published bundle; **delta refused when the base is absent or altered, full bundle taken instead**; a corrupted delta rejected at Gate 3 without looping; interrupted download resumes at the right offset; `-NoRange` restarts cleanly; a hash-mismatched complete transfer restarts once then hard-fails; obsolete bundles demoted to the cache, not deleted; cache GC reclaims; disk-full during apply does **not** blacklist the delta; **opt out via `dead_air_disable_auto_update`, delete a bundle, confirm repair still runs**; the maintenance exe never fetches; uninstall leaves only stock archives and no `appdata\vfs-index-*.cache`.
- **`tools/qa/Test-ContentScale.ps1`** (new, G5) — generates N GB of incompressible synthetic bundles and runs one full cycle: cold hash duration, FS metadata delta, `_initialize` delta, commit duration, peak disk. Run once per major shard-table change, not per release. This is what stands in for an end-to-end 5 GB test; say so in `TEST_MATRIX.md`.
- **Real-GitHub probe** (G5): a `content-0.0.0-probe` tag in the assets repo with one ~50 MB bundle, used to verify `Range` on the 302 target, the `digest` field, and resume behaviour. Run before Batch 3 lands and again before any release that changes the downloader.
- **`tools/qa/Test-UpdatePatchFlow.ps1:222`** — keep the `\.dead-air-x64\backups\` filter, add `content-cache` and `content-state.txt`; add a content-bearing variant so the tree-compare knows about bundles.

---

## K. Documentation and rules

| Doc | Change |
|---|---|
| **New** `docs/dead-air/CONTENT_BUNDLES.md` | Normative spec: `content/1`, `content-state/1`, `content-incomplete/1`, `.darpatch` v1, bundle naming and the two hashes, `groups.ltx` and the append-only shard table, the resolver, the three delta gates, two-phase commit, repair, mount order **with the corrected wording** (content sorts after the compat archive and after today's `xtra_*`, not after every conceivable name), the tag/asset immutability rule (referencing §11, not restating it), and the XMS note that verification looks at bundle *files*, never resolved vpaths. |
| `docs/dead-air/AUTO_UPDATE.md` | Release contract gains `Dead-Air-Refined-<V>-content-manifest.txt`. New section on update-time sequencing and why content is not in the ZIP. `update/1` and `/2` explicitly untouched. Fix `:111-119` — the `refined-version` snapshot is no longer a rollback target and is now deleted on success. Add the manual-install note: `Setup_Manual.zip` carries no content; it arrives on first launch via repair, with the size. |
| `PROJECT_RULES.md` | §4 line 125 **must change** — proposed: *"Assets are published as versioned, hashed bundles pinned per game version, obtained by the installer or by the game's repair path, and committed atomically: an installation either has the complete set for its version or it is reported as incomplete and play is refused until it is repaired."* §11: fix the stale two-asset list (reality: Setup / Setup_Manual / Update_Patch / Update alias) and add the content manifest; add the assets repo to the explicit-addressing rule; add **"a published content release tag is never deleted and no asset in it is ever replaced"** — this is the single highest-consequence process rule the system introduces and it belongs here, not in a `docs/` file. |
| `docs/dead-air/MODDING.md` | New content section: the archive prefix, its exact load position, the guarantee that loose `gamedata\`, JSGME and XMS still override content, and that startup verification never looks at resolved paths. |
| `docs/dead-air/TEST_MATRIX.md` | The new cases from §J. |
| `docs/dead-air/DEPENDENCIES.md` | `xrContentSync` (WinHTTP, BCrypt), `DarDelta.exe`, `converter.exe`, `gh` CLI. |
| `docs/dead-air/DIAGNOSTIC_REPORTS.md` | The content-id / latch / skipped-bundle fields and their sanitisation. |
| `packaging/dead-air-x64/README_RU.md` | Install now downloads content: disk space and network required, with figures. Uninstall removes content. **Version rollback is gone.** Original-game restore stays. The manual ZIP path fetches content on first launch. |
| Assets repo | `README.md`, `CONTENT_RULES.md`. |

**Release runbook** (G4), written into `AUTO_UPDATE.md`, in this exact order:
1. Clean build; `ProductVersion.h` bumped.
2. `dead_air_x64_content_bundles.ps1` → new manifest + build report (bundles new/reused, bytes to upload). Review the `groups.ltx` diff — any new `[assign]` line is a permanent commitment.
3. `dead_air_x64_content_delta.ps1` against the previous manifest.
4. `build_dead_air_x64_installer.ps1` (compat archive, installer, Setup_Manual, patch, SHA256SUMS).
5. QA: `Test-ContentFlow.ps1`, `Test-UpdatePatchFlow.ps1`, the real-GitHub probe if the downloader changed.
6. **Ask for permission. Publish the content release first** (`publish_dead_air_x64_content.ps1 -Confirm`), verify every asset digest via `gh api`.
7. **Then** publish the game release and verify per §11, including `releases?per_page=30`.
8. A content release published without a following game release is harmless: an orphan tag that is never deleted.

**Backup obligations** (G24), stated for the owner: the bundle cache must be retained indefinitely (losing it while a release is unreachable forces a repack that may not reproduce the published bytes), and the authoring tree at `D:\Games\Dead Air\ContentSource\gamedata\` has no version-controlled backup — `index/source-<tag>.txt` proves what was published but cannot reconstruct it.

---

## L. Batches

Each ends in a state verifiable without the next existing. **Batches 0-7 are dark: no content is removed from the compat archive and no release is cut from them.** (Constraints #6.)

### Batch 0 — hazard removal
- `ProcessArchive` (`LocatorAPI.cpp:701-703`): normalize before the dedupe compare (lexically-normal + lowercase). `$arch_dir$` → `database\` and four aliases → `database\.\` are distinct strings today, so every archive is opened, mapped and indexed **twice** (`36 archives` for 18 files).
- `matches_payload` (`DeadAirUpdater.cpp:488-493`): lowercase the manifest hash like `hash_matches:284`.
- `append_unique` (`:587-594`) and `apply_payload`'s GC (`:694-701`): compare through `lower_key`/`generic_wstring()` instead of `_wcsicmp` on the native string — `managed-files.txt` uses backslashes, the manifest forward slashes.
- Delete the updater's backup snapshot on success (after `:937`).
- Promote `sha256_file`/`sha256_handle` from `CrashReport.cpp:33-34` into `src/xrCore/Crypto/xr_sha256.{h,cpp}` (internal to xrCore, no `.def` entry).

**Verify:** log reports 18 archives, not 36; FS file count unchanged; **dump `vpath → owning archive` over `m_files` before and after and diff** — archive count and file count would not catch a changed winner, and "last `Register` wins" means the current winner is decided by the second pass (correctness-S18); `Test-UpdatePatchFlow.ps1` green; game boots and loads a save.

### Batch 1 — tooling, against a scratch source tree
`New-XdbArchive` refactor (byte-identical, proven by hash). `groups.ltx`. `content_manifest.cpp` (the one parser). `dead_air_x64_content_bundles.ps1` with the reuse ladder, disjointness, headless mount, parallel verify.
**Verify:** bundles build from a scratch tree and pass verification; the manifest parses in all three consumers; re-running with no source change repacks nothing; adding one file in one directory changes exactly one bundle; the compat archive's hash is unchanged by the refactor.

### Batch 2 — mount gate, verify-only, latch
`ContentPin` + `.def` entries; `ProcessArchive(path, size)`; soft-fail on a bad index for bundle-named archives; the two log lines. `ContentService` verify-only (no network), `content-state.txt`, `content-incomplete.txt`. Banner + `CheckContentDialog` with **Exit only**. Play gate at `CLevel::net_Start`. Console commands. `capture_content_snapshot` extension.
**Verify:** correct set → silent boot; second launch uses the state cache (no hashing in the log); delete a bundle → latch + banner + dialog + play refused via console *and* via `user.ltx` `server(...)`; truncate a bundle → not mounted, no assert; flip one byte without changing size → not mounted, no assert, reported as `invalid index`; rename to a stale name → skipped and reported; remove `content-manifest.txt` → recovery mode, not silence.

### Batch 3 — `xrContentSync`, download, install-time
Real-GitHub `Range` probe first. The static library (`content_manifest`, `content_hash`, `content_resolver`, `content_download`, `content_commit`), linked into both `xrGame` and `DeadAirUpdater.exe`. Updater `--content-plan/--content-fetch/--content-commit`, exit codes 26/27. Inno: custom wizard page with Cancel before `wpReady`, moved ahead of all destructive bookkeeping, mutex + result file, `ExtraDiskSpaceRequired`, `AppMutex`. `Start-ContentAssetMock.ps1`.
**Verify:** clean install pulls the full set; kill mid-download → resumes at the right offset; corrupt a cached part → restarts once, then hard-fails; `-NoRange` → clean restart; killing the fetcher → the wizard fails instead of spinning; Cancel keeps the cache and leaves the README; free-space precheck fires with the correct figure; the maintenance exe never fetches.

### Batch 4 — in-game repair, update-time, cache-before-network
`ContentService` download path with retry/backoff/concurrency; aggregate progress; `UIContentWnd` Repair ending in a mandatory relaunch. `UpdateService` fetches and verifies the target manifest against the `update/2` entry; content commits in the game process before arming; `RestartAndApply` passes `--content-commit` as a fallback only.
**Verify:** `Test-ContentFlow.ps1` end-to-end; the owner's scenario — patch the runtime only, launch → repair dialog and a correct full fetch, never a degraded game; quit instead of restarting → next launch commits from the cache without downloading a byte; a *pre-content* updater binary in place → the update still completes correctly.

### Batch 5 — deltas
`.darpatch`, `DarDelta.exe`, `content_delta.cpp`, cumulative edges + shortest-path resolution, `rejected-deltas.txt` with integrity-only blacklisting.
**Verify:** delta output byte-identical to the published bundle; wrong/absent base refused before any write and the full bundle taken; a corrupted delta rejected at Gate 3 without looping; a simulated disk-full during apply does **not** blacklist; a two-hop chain (N→N+2) produces the correct bundle and frees intermediates; measured ratios recorded.

### Batch 6 — uninstaller rework
§G in full: rollback deletions, backup checkbox removal, `RemoveContentBundles` with reparse guard and honoured return value, reordered `CurUninstallStepChanged`, metadata, `[UninstallDelete]`, dialog reshape, running-game refusal.
**Verify:** uninstall → `database\` holds only stock archives, no `content-cache`, no `backups`, no `vfs-index-*.cache`; with an x86 backup → original game restored and content gone; a *forced* restore failure no longer orphans bundles and the manifest survives to describe them; a locked bundle produces a message and a reboot-delete, not a silent success; Add/Remove Programs entry gone.

### Batch 7 — publishing and documentation
`publish_dead_air_x64_content.ps1` with digest verification and the append-only ledger; assets repo scaffold; all of §K including the `PROJECT_RULES.md` §4 and §11 edits (owner-approved wording); retire `build_dead_air_x64_package.ps1` and the two PS1 scripts.
**Verify:** a dry run enumerates exactly the new assets and refuses a name collision with different bytes; digest verification passes; docs match shipped behaviour.

### Batch 8 — cutover and first content release
Move the 40 binary files (19,204,670 B — 94.4 % of the compat tree: `textures/` 29 files, `meshes/` 4, `sounds/` 5, `anims/` 2) out of `packaging/dead-air-x64/compatibility/gamedata/` into the content source tree. The 129 code/config files stay — scripts, shaders and configs are version-coupled to the engine. Pin the shard table for the projected 5 GB, not for today's 19 MB. Run `Test-ContentScale.ps1`. Then the runbook.
**Verify:** compat archive drops 20.3 MB → ~1.15 MB; a 1.4.x installation updated to the content release ends complete with no degraded window; a fresh Setup install is complete; a `Setup_Manual.zip` extraction is incomplete-and-repairs on first launch; the full `TEST_MATRIX.md` content block passes.

---

## M. OPEN questions, with defaults

| # | Question | Recommended default |
|---|---|---|
| OPEN-1 | Retire `build_dead_air_x64_package.ps1` + `Install-/Uninstall-DeadAir-x64.ps1`? | **Retire.** They produce an installation the content system cannot see and where constraint 1 cannot hold. |
| OPEN-2 | Initial shard table sizing | Pin at the **projected 5 GB**, ~16-24 shards for the big groups (144 sequential fetches at 6×24 costs ~90 s of 302 overhead alone), never at today's 19 MB. |
| OPEN-3 | Concurrent downloads | **3.** |
| OPEN-4 | Cache size cap | `max(2 × current content set, 6 GiB)`, LRU. |
| OPEN-5 | Keep the legacy `-Update.zip` alias in the first content release? | **Keep** — dropping it is a separate decision. |
| OPEN-6 | `PROJECT_RULES.md` §4 line 125 wording | Owner's text; §K carries a proposal. Do not ship the design while the governing rule contradicts it. |
| OPEN-7 | Delta skip threshold | **0.35**. |
| OPEN-8 | May repair commit without a relaunch? | **No.** Repair always ends in a mandatory relaunch. |
| OPEN-9 | Authoring-tree backup | Separate private repo or an ordinary backup — outside this system's scope, but it is the one unrecoverable single point of failure. |
| OPEN-10 | Compat archive `level_ver` | Stays hand-bumped; only bundles derive it. |
| OPEN-11 | Does the first content release get a `MAJOR` bump given the install-size change? | **No** — it is a normal minor. But the release text and `README_RU.md` must say this update is larger and why. |