# Dead Air: Refined content bundles

How game content gets from an authoring tree to a running installation: the bundle format,
the manifest that pins a version to an exact set of bundles, the gate that decides what may
be mounted, and the resolve/fetch/commit pipeline that makes an installation match its
manifest.

Publication rules — which release is cut first, what is never deleted — are in
[`PROJECT_RULES.md`](../../PROJECT_RULES.md) §4 and §11 and are not restated here.

## Why content lives outside the engine repository

The engine repository holds sources. Textures, meshes, sounds and animations are published
separately, as versioned hashed bundles, and a game version pins the exact set it needs. Two
constraints force that shape:

- Content is never optional (`PROJECT_RULES.md` §4). An installation either has the complete
  set for its version, or it is reported as incomplete and play is refused until it is
  repaired. There is no opt-in download and no reduced edition, so nothing in this system has
  a switch that skips content.
- Content cannot travel inside the update archive. The applier is bounded at 1 GiB expanded
  and 1024 files (`DeadAirUpdater.cpp`, `MaximumExpandedBytes` / `MaximumFiles`), and its
  second stage has a five-minute wall — none of which survive a multi-gigabyte payload. So
  content is fetched on its own, verified on its own, and an update is armed only after every
  bundle it needs is verified on disk.

The consequence to keep in mind while reading the rest: the download host is a replaceable
detail. Integrity comes from hashes the installation already has, never from trusting where
the bytes came from.

## The pieces

| Path | What it is |
| --- | --- |
| `{app}\.dead-air-x64\content-manifest.txt` | The trust root. Ships inside the hash-verified Setup payload; names every bundle this version requires. |
| `{app}\.dead-air-x64\content-state.txt` | Advisory cache of "this file, this size, this mtime, was this hash". Deleting it costs a rescan, never correctness. |
| `{app}\.dead-air-x64\content-incomplete.txt` | The latch. Present means the installation must not be trusted. |
| `{app}\.dead-air-x64\content-cache\` | Downloads land here; bundles enter `database\` only from here, by rename. |
| `{app}\database\xtra_dead_air_x64_content_*.xdb0` | The mounted bundles. |
| `packaging\dead-air-x64\content\groups.ltx` | The append-only directory-to-shard table. Single source of truth, engine repo only. |

Code, one implementation per job:

| Component | Role |
| --- | --- |
| `src/xrContentSync/*` | The shared core: manifest parser, hashing, state files, resolver, downloader, commit, deltas. A static library with no xrCore dependency, compiled into xrCore, into xrGame, and straight into `DeadAirUpdater.exe` by the packaging script. |
| `src/xrCore/Content/ContentPin.{h,cpp}` | The mount gate. Lives in xrCore because it runs inside `CLocatorAPI::_initialize`, before the game DLL exists. |
| `src/xrGame/ui/ContentService.{h,cpp}` | Verification, repair, the play gate's answer, the console commands. |
| `src/xrGame/ui/UIContentWnd.{h,cpp}` | The repair dialog. |
| `packaging/dead-air-x64/installer/DeadAirUpdater.cpp` | `--content-plan`, `--content-fetch`, `--content-commit`. The same binary is shipped as `DeadAirContent.exe` to the installer. |
| `tools/package/dead_air_x64_content_bundles.ps1` | The bundle builder and manifest writer. |

Every path above is derived in exactly one place, `ContentPaths::Layout`
(`src/xrContentSync/ContentPaths.h`), relative to the installation root. Three processes need
these paths and only one of them has an engine FS to ask; deriving them by hand in each is how
the game ends up reading a latch the updater wrote somewhere else. `Database()` is fixed at
`{app}\database` rather than read from `fsgame.ltx`, because the installer and uninstaller
write there by absolute path — a content system that resolved it from data could fetch into a
directory the game never mounts and re-download forever. `ContentService` checks the two agree
and reports a redirected `$arch_dir$` instead of entering that loop.

## The two hashes

Confusing these is the classic way to break the system, so they are named apart everywhere.

**Content digest** — SHA-256 over a bundle's members *before* packing:
`epoch=1\n` followed by `<lowercase vpath>\n<member sha256>\n` for every member sorted by
path (`dead_air_x64_content_bundles.ps1`, the canonical-string block). It is the **build cache
key and nothing else**: it answers "do I already have a packed bundle for exactly these
files", so unchanged content is never repacked. It never appears in a filename. It is written
into the generated archive header as `level_ver`, which makes a bundle self-describing.

**File hash** — SHA-256 of the finished `.xdb0`. It appears in the **filename** (first 16 hex)
and in the **manifest** (all 64). Name and bytes are 1:1 forever.

That 1:1 property is what the whole design rests on. A client can decide it already has a
bundle by looking at a filename; unchanged content keeps its name across releases and is
neither re-uploaded nor re-downloaded; and replacing the bytes behind a published name is
unrecoverable in the field, which is why `PROJECT_RULES.md` §11 forbids it.

## Bundle names

```text
xtra_dead_air_x64_content_<group>_<NN>_<16 hex>.xdb0
```

`group` is `[a-z0-9_]+`, `NN` is exactly two digits, and the hash is exactly 16 lowercase hex
characters. `ContentManifest::SplitBundleName` (`ContentManifest.cpp`) is the one accepting
parser; the uninstaller carries a hand-written twin, `ContentBundleNameValid` in
`DeadAir-x64.iss`, because Pascal Script cannot call it.

**This shape is the delete authority for `database\`.** The uninstaller sweeps by it, and the
resolver lists everything matching it that the manifest does not declare as obsolete, which the
commit then demotes into the cache. Anything the pattern accepts is something those two may
move or remove, which is why it is a strict shape and not a prefix match: a looser pattern
would eat a third-party archive. (The cache collector works in the same directory but keys on
64-hex file names, not on this shape.)

The corollary for modders: the *shape* is reserved, not the prefix. A file merely starting
with `xtra_dead_air_x64_content_` but failing the shape mounts like any other archive and is
never touched — but do not go near the prefix anyway, because the two rules are one typo
apart. See [`MODDING.md`](MODDING.md) for the addon-facing contract.

Group and shard together identify a *slot*; the hash identifies the revision filling it. That
is what lets the gate tell "an older revision of a bundle this version knows" from "a file we
have never heard of" (`Manifest::FindBySlot`), and those two deserve different diagnostics.

## `groups.ltx` — the append-only shard table

`packaging/dead-air-x64/content/groups.ltx` is the single source of truth for how the source
tree is cut into bundles. Shipped shape:

```ini
[groups]
; group name = the gamedata subtree it draws from
textures        = textures
meshes          = meshes
sounds          = sounds
anims           = anims

[shard]
target_mb       = 250
max_mb          = 1536

[assign]
; <root>/<first path component> = <shard index>
textures/act            = 00
textures/ui             = 01
```

`target_mb` is an aim, not a limit: a directory is never split across shards, so one oversized
directory produces one oversized bundle. `max_mb` is a hard ceiling the builder throws on —
GitHub refuses a release asset above 2 GiB, and the builder additionally refuses anything at
or above 4 GiB because the mount gate compares against `_finddata_t::size`, which is 32 bits
wide on Windows and truncates silently.

**The `[assign]` table is append-only.** Editing or removing a line moves files between two
bundles, which changes both bundles' bytes, which changes both filenames, which makes every
installed player re-download both. New directories are appended by the builder into the
smallest existing shard of their group and **written back into `groups.ltx`**, so the change
is reviewed and committed like any other; existing lines are never touched.

**Sharding is an explicit table rather than `hash % shard_count`** for one fatal reason: the
shard count is guaranteed to grow as content grows, and under a modulo scheme that bump
re-shuffles every file at once — a single new bundle would cost a full re-download of
everything. A directory table also keeps thematic edits thematic: a coherent change to one
subtree republishes the bundles that subtree lives in, not a uniform smear across all of them.

The assignment key is the first path component under the group root, so `.dds`/`.thm`
companions always land in the same bundle for free.

## `content-manifest.txt` — schema `dead-air-refined.content/1`

```text
schema=dead-air-refined.content/1
version=1.4.0
content-id=<64 lowercase hex>
repo=MMadmer/Dead-Air-Refined_Assets
[bundles]
<sha256>\t<size>\t<bundle-file-name>\t<release-tag>
[deltas]
<sha256>\t<size>\t<delta-asset>\t<release-tag>\t<base-bundle>\t<base-sha256>\t<base-size>\t<target-sha256>
```

UTF-8 without BOM, `\n`, trailing newline. `ContentManifest::Parse` is deliberately
unforgiving — exact prefixes, no whitespace tolerance, fixed field counts, no partial success.
A manifest is machine-written by the packaging script and consumed by code that deletes files
on its authority, so a typo must fail at build time rather than be interpreted at runtime.

| Rule | Detail |
| --- | --- |
| Header | Lines 1-4 in exactly this order, exact-prefix match. |
| `version=` | Three numeric components, `MAJOR.MINOR.PATCH`. |
| Hashes | Exactly 64 hex, **lowercase enforced on parse**. Consumers compare with `==`, so tolerating uppercase would produce a value that matches nothing and reports itself as a corrupt bundle. |
| `[bundles]` row | Exactly 4 tab-separated fields. Name must satisfy the bundle-name shape; size non-zero; duplicate names rejected. |
| `[deltas]` row | Exactly 8 fields. `base-size` is mandatory and comes from the delta record, not from `[bundles]`: a player who skipped releases often does not have the base in their own manifest, and eligibility has to be decidable anyway. Duplicate `(base-sha256, target-sha256)` pairs rejected. |
| Reachability | Every delta edge must reach a declared bundle, possibly through other edges. Checked by walking backwards from the live bundles until the reachable set stops growing — it is a property of the graph and cannot be decided row by row. |
| Bounds | `MaximumBundles = 4096`, `MaximumManifestBytes = 4 MiB` (`ContentManifest.h`). Beyond these a manifest is corrupt or hostile, not ambitious. |
| `content-id` | SHA-256 over `<name>\n<sha256>\n` per bundle, sorted by name **ordinally**. The builder sorts with `StringComparer::Ordinal` for the same reason: a culture-aware sort disagrees with `std::string` comparison on underscores and digits, and the two only have to differ once for every installed player to be told their manifest was tampered with. |
| `repo=` | **Informational only.** The download host is pinned in shipped code (`AssetsRepository` in both `ContentService.cpp` and `DeadAirUpdater.cpp`); taking it from a file that was downloaded would let whoever wrote that file choose where the next gigabytes come from. Changing the mirror is a code change. |

`Manifest::ContentIdMatches()` recomputes the id from the bundle list and compares it with the
declared one. Both the game (`run_initial_pass`) and the updater (`open_content_session`) run
that check before acting on anything: a manifest whose own id does not match has been edited
by hand, and every claim in it is then worth nothing.

`[deltas]` is **cumulative** — every edge ever published for a bundle that still exists stays
listed, so a player who skipped releases can be walked from whatever revision they actually
hold. Publishing cost does not change: one delta per changed bundle per release.

The manifest lives in three places for one version: inside the Setup payload, installed at
`{app}\.dead-air-x64\content-manifest.txt`, and published on the game release as
`Dead-Air-Refined-<version>-content-manifest.txt` (`PROJECT_RULES.md` §11), so a player or a
tool can see what a version pins without installing it. Only the installed copy is ever
trusted.

## `content-state.txt` — the advisory cache

```text
schema=dead-air-refined.content-state/1
content-id=<64 hex>
<sha256>\t<size>\t<mtime-decimal-FILETIME>\t<bundle-name>
```

An entry is trusted only on an exact match of size **and** mtime whose recorded hash is the
one the manifest wants, and a zero mtime never matches (`ContentResolver::Resolve`). A stale
entry therefore costs a rehash and never a wrong answer.

The `content-id` line is recorded for diagnostics and deliberately **not** used to invalidate
the cache: versions share bundles, so throwing it away on every content-id change would rehash
several gigabytes on every update for no gain. Deleting the file is always safe.

Only passes that actually read bytes may write entries. `ContentCommit::Run` records exactly
the bundles it hashed and merges them over what a previous pass wrote; inventing an entry from
the manifest would be worse than writing nothing, because the resolver would then vouch for a
file nobody has ever read.

## `content-incomplete.txt` — the latch

```text
schema=dead-air-refined.content-incomplete/1
version=<version>
reason=install-commit|update-commit|repair-commit|verify-failed|manifest-missing|manifest-invalid
time=<decimal FILETIME>
```

The latch is not advisory. `ContentService`, the play gate and the repair dialog all key off
it, and it is the single thing that stops a mid-commit crash from presenting as a healthy
installation on the next launch.

Discipline:

- **Written before the first mutation.** The installer writes it in `PrepareToInstall`, at the
  point of no return, before any backup or bookkeeping (`DeadAir-x64.iss`). The in-game repair
  writes it before the first byte moves (`repair_worker`). The startup pass writes it the
  moment it finds a problem.
- **A latch that will not write is itself a problem** and is logged as one: an installation
  that cannot record its own incompleteness will present as healthy after a crash. This is why
  `ContentState::WriteLatch` flushes before deciding its return value.
- **Cleared only on evidence.** `ContentCommit::Run` clears it only after a fresh resolve with
  hashing enabled reports zero outstanding jobs; a stat-only pass structurally cannot see the
  corruption a latch may have been set for, so clearing on that word would be clearing it on
  nothing. The background verifier clears it only when *every* recorded problem is gone, not
  merely the ones hashing can retract.
- **A leftover latch on an otherwise clean install is a question, not a verdict.** The cheap
  startup pass reports `Incomplete` and lets a full hash pass adjudicate; if that comes back
  clean the latch goes and the dialog closes itself.

`install-commit` is written by the installer, `repair-commit` by the in-game repair,
`update-commit` by the update prefetch the moment it decides to put bytes for a not-yet-installed
version into the cache, and `verify-failed` / `manifest-missing` / `manifest-invalid` by the
startup pass.

## The mount gate

`ContentPin::Load(Core.ApplicationPath)` runs at the top of `CLocatorAPI::_initialize`, before
a single archive is opened. `CLocatorAPI::ProcessArchive(path, size)` consults it before
`A.open()`, using the size the directory scan reported rather than the mapped size, so a
truncated file is refused before anything touches it.

Names that do not have the bundle shape always pass: the gate has no opinion about the stock
archives or a third-party mod's. For a bundle-shaped name it mounts only when the installed
manifest declares that exact name at that exact size.

| Skip reason (verbatim) | Meaning | Written by |
| --- | --- | --- |
| `no content manifest` | No readable manifest, so the installation cannot say what it should contain. A pre-content install has no bundle-shaped files at all, so this only fires on a broken or tampered one. | `ContentPin::ShouldMount` |
| `stale bundle` | The manifest declares this group/shard at a different hash — an older revision left behind by a failed commit. | `ContentPin::ShouldMount` |
| `unrecognised bundle-shaped archive` | The manifest knows no such slot: a file from another branch or a hand-renamed archive. | `ContentPin::ShouldMount` |
| `size does not match the manifest` | Correct name, wrong length: truncated or partially written. | `ContentPin::ShouldMount` |
| `invalid index` | The archive's index chunk signature does not read. | `CLocatorAPI::ProcessArchive` |
| `invalid header` | No header chunk, or a header without `auto_load`. Every bundle is written with one, so its absence means the file is not the bundle its name claims to be. | `CLocatorAPI::ProcessArchive` |

The last two exist because name and size cannot catch a flipped bit inside a 400 MB bundle —
that corruption is size-preserving. Every step past the gate answers malformed data with an
assert (`R_ASSERT3` on the chunk signature, `CInifile`'s own on the header), which for content
would turn the one realistic corruption into an unrecoverable boot crash. **For bundle-shaped
archives only**, those two failures close the archive, drop it from `m_archives` and report,
converting a crash into the repair path. Non-bundle archives keep the old assert behaviour.

`ContentPin::RecordSkipped` de-duplicates by file name, because the same archive reaches
`ProcessArchive` under several spellings of its path and a refused file never enters
`m_archives`, where the usual duplicate check lives.

Only four symbols cross the DLL boundary into xrGame — `ManifestLoaded`, `Skipped`,
`SkipReasons` and `MetaDirectory` — and `src/xrCore/xrCore.def` carries them as hand-written
decorated names with fixed ordinals. Adding an export means appending a new ordinal, never
renumbering; the gate itself stays internal to xrCore, and xrGame gets the manifest parser and
the hashing from `xrContentSync` instead.

### Mount order

Archives are mounted in sorted order of their raw file names (`Recurse` sorts with `xr_strcmp`
before `ProcessOne`), and the last `Register` for a vpath wins. `.` (0x2E) sorts before `_`
(0x5F), so `xtra_dead_air_x64.xdb0` comes first and `xtra_dead_air_x64_content_*.xdb0`
immediately after it — content therefore overrides the compatibility archive and sorts last
among today's `xtra_*` archives.

It does **not** sort after every conceivable third-party name: a hypothetical `xtra_zzz.xdb0`
still beats it. That is the desired outcome — a mod overriding content is a mod working. Loose
`gamedata\`, JSGME and XMS also still override content, because `$game_data$` is listed after
`$arch_dir$` in `fsgame.ltx`.

Verification never looks at resolved vpaths. It looks at bundle **files** in `database\`, so
an override cannot make an installation look complete or incomplete.

## Resolve, fetch, commit

### Resolve

`ContentResolver::Resolve(manifest, paths, options, plan, error)` is the one comparison
everybody makes — the installer before it fetches, the game before it repairs, the updater
before it commits — so that they cannot disagree about what "complete" means. Per declared
bundle it produces one of: satisfied, or a `Job` with a `Fault` of `Missing`, `Size` or
`Corrupt`. It also lists `obsolete`: bundle-shaped files in `database\` this manifest does not
declare. Those are listed, never deleted here.

Three options decide what a pass costs:

- `verifyHashes` — false is a stat per bundle (the installer, the commit's own pre-pass);
  true hashes everything `content-state.txt` cannot vouch for.
- `probeCache` — whether to look for an already-downloaded copy in `content-cache\<sha256>`.
  That check is a full hash of a cache file, so the startup pass clears it and accepts the
  pessimistic answer: a job reported as a download that turns out to be a move costs nothing,
  a multi-gigabyte synchronous read before the window exists costs the launch.
- `trustCache` — cleared by a forced verify. The file itself is left alone, so a cancelled
  forced pass does not cost the next launch a full rehash.

**Cache before network.** A verified copy in the cache turns a job into a move (`Job::cached`,
`Plan::bytesToMove`), which is what makes "download, quit without restarting" cost nothing the
second time.

Free space, `ContentResolver::RequiredFreeBytes`:

```text
required = bytes to fetch + the largest single job + 512 MiB
```

The largest-job term is the headroom for an incoming file existing alongside the one it
replaces; the arithmetic saturates rather than wrapping, because a wrapped total would read as
a tiny requirement and wave an impossible install through. `FreeBytes` returning 0 means
"could not determine" and is never treated as "full". When space is genuinely short the
installation **stays incomplete**: the repair reports required versus free, the latch stays,
and play stays blocked. Repair never resolves into a playable state without the content.

`ContentResolver::SameVolume` guards the commit: `MoveFileExW` is atomic only within a volume,
and across volumes it silently degrades into copy-and-delete, doubling the peak disk
requirement the plan was sized against. A `database\` junctioned onto another volume is refused
loudly instead of half-succeeding at five gigabytes.

### Fetch

`ContentDownload::Fetch` downloads every job that is not already cached, verifies it, and
leaves it at `content-cache\<sha256>`. It installs nothing.

- URL: `https://github.com/<pinned repo>/releases/download/<release-tag>/<asset>`. No
  `api.github.com` call at all, which sidesteps the API response cap, the request budget and
  asset enumeration.
- Part files are content-addressed, `content-cache\<expected sha256>.part`, so a resume can
  never continue the wrong revision.
- Resume rewinds the last 1 MiB and re-reads the kept prefix into the running digest before
  sending `Range: bytes=<n>-`. A `206` must answer with a `Content-Range` whose start offset
  and total length both match, or the part is discarded and the bundle restarts from zero. A
  `200` to a ranged request means the server ignored the range: the file is truncated, the
  digest reset, and the transfer starts over rather than appending a second copy of the head.
- 5 attempts per bundle with 1/2/4/8 s backoff, resuming each time; a 120 s **idle** timeout
  (silence, not total time — a large bundle on a slow line legitimately takes an hour); 1 MiB
  buffers; 3 concurrent assets by default.
- A complete transfer whose hash does not match is retried exactly **once** from zero. A
  truncated proxy response is plausible; the same wrong bytes twice is not, and looping on it
  would burn the player's bandwidth forever.
- The finished length on disk is checked against the manifest before the hash verdict is
  believed, because a write that went somewhere the digest did not follow would otherwise be
  vouched for.
- QA override: `DAR_QA_CONTENT_BASE`, accepted only for a loopback `http` host judged on the
  **parsed** host and rejected if it carries userinfo (`http://127.0.0.1:@evil.example/`
  starts with the right characters and points somewhere else). It redirects where bytes come
  from and nothing else; hashes still gate every commit, and there is deliberately no
  companion switch that skips the fetch.

### Commit

`ContentCommit::Run` is two phases and never one.

**Phase 1 — add only.** Each verified cache file is re-hashed (it may have sat there across a
reboot; this is the last point a wrong bundle can be stopped) and then moved into `database\`
with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, retried 5 times with
200 ms backoff because a freshly written 300 MB file is exactly what a real-time scanner still
has open. A job the plan reports as not cached is skipped and logged rather than failing the
commit, so the outcome does not depend on manifest row order.

**The rename is the only way in.** Nothing ever writes into `database\` directly, so a partial
or wrong-hash file cannot acquire a bundle name — not "is checked and rejected", but cannot.
Because names are content-addressed, every commit is create-only and can never target a name
the running engine has mapped.

**Phase 2 — demote.** Only after phase 1, obsolete bundles are **renamed into the cache under
their own hash**, never deleted. **Add before delete, always.** A commit interrupted halfway
then leaves an installation with too many bundles rather than one with too few, and only the
first of those is survivable. Demoting rather than deleting also makes downgrade-then-upgrade a
zero-byte operation.

There is no background tidying pass. `Plan::obsolete` is acted on by phase 2 and by nothing
else, so a leftover bundle waits for the **next** commit — the next update, the next install, or
a repair. Until then the mount gate refuses it (`stale bundle` when the manifest still declares
that slot at another hash, `unrecognised bundle-shaped archive` when it does not), and
`ContentService` files the refusal as a **notice** rather than a problem, so it neither latches
the installation nor blocks play: the manifest does not declare the file, nothing wants it, it
is not mounted, and refusing to start the game over a stray the installer left behind would
punish the player for someone else's mess.

A bundle that cannot be read is left where it is (renaming it to something the collector does
not recognise would strand it), and a demotion that fails is logged, not fatal.

Afterwards the state cache is updated with what this pass actually read, and the latch is
cleared only if a fresh hashing resolve says there is nothing outstanding.

### The cache and its collector

`ContentResolver::CollectCache(paths, manifest, plan, capBytes)` runs after a successful
commit, in both the game and the updater, with a cap of 6 GiB — a full set plus its
predecessor, so a reverted update costs nothing to put back.

- Anything the manifest still declares by hash, or that the current plan is about to use, is
  off limits at any age: deleting it would turn a free move into a fresh download.
- `.part` files older than 14 days are deleted; younger ones are kept (they are resume state)
  but their bytes still count towards the cap.
- Files whose name is not 64 hex are ignored — `README.txt` and the fetch progress/result
  files are not the collector's to reclaim.
- Over the cap, eviction is oldest-write-first. One unreadable entry is skipped rather than
  ending the sweep.

The cache directory carries a `README.txt` in both the installer's and the fetcher's wording,
so an abandoned install does not orphan gigabytes in a directory nobody can identify.

## Deltas

A content release usually changes a handful of files inside one large bundle. Without deltas
every player re-downloads the whole bundle for a small edit, which at gigabytes of content is
the difference between an update people take and one they skip.

The apply path lives in `src/xrContentSync/ContentDelta.{h,cpp}`, the chain selection in
`ContentResolver::plan_deltas`, the chain walk in `ContentDownload::apply_chain`, and the
builder in `DarDelta.exe` (`src/utils/DarDelta/DarDelta.cpp`).

### `.darpatch` v1

```text
u8[8] "DARPATCH" | u32 version = 1
u8[32] baseSha256   | u64 baseSize
u8[32] targetSha256 | u64 targetSize
u64 opCount
opCount × { u8 kind; COPY: u64 baseOffset, u64 length | INSERT: u64 length, u8[length] }
```

`kind` is `0` for COPY and `1` for INSERT. Everything is little-endian, there is no padding, and
there is no compression and no dictionary: bundles are already compressed archives, so the
interesting redundancy is between two revisions of the same file rather than inside one, and a
compressor would buy nothing while adding a way to fail.

Every claim in the header is validated before a single output byte is written. The magic and
`version` must match exactly, `targetSize` must be non-zero, `opCount` must be non-zero, and
both sizes are bounded by `MaximumSize` = 8 GiB, `opCount` by `MaximumOps` = 8 Mi, and each
INSERT by `MaximumInsert` = 256 MiB (`ContentDelta.h`). Those bounds exist because a malformed
header must not be able to make the process allocate its way out of memory before anything has
had the chance to reject it.

Per operation, a COPY is checked **before** the seek — `baseOffset <= baseSize`,
`length <= baseSize - baseOffset` and `length <= targetSize - produced` — because an operation
that reaches past the base is a malformed delta and finding that out by reading garbage would be
too late. An INSERT is bounded the same way against what is left of the target. Finally the op
lengths must sum to exactly `targetSize`.

### How a delta is cut

`DarDelta.exe <base.xdb0> <target.xdb0> <out.darpatch>` maps both files and cuts them with
**content-defined chunking**. Fixed boundaries would work only until something changed length,
after which every later boundary shifts and nothing matches; a rolling hash puts the boundaries
where the *data* says, so an insertion perturbs the chunk it lands in and leaves the rest
recognisable.

The rolling hash is gear-style — `hash = (hash << 1) + gear[byte]` — over a 256-entry table
generated at run time from a fixed linear congruential sequence seeded with
`0x9e3779b97f4a7c15`, rather than shipped as a literal block: the only properties that matter
are that the values are well distributed and identical on every machine, and a seeded generator
says that more clearly than 256 magic numbers would. A boundary is declared when the hash has
the low bits of `BoundaryMask = AverageChunk - 1` clear, which over random data happens on
average once per average chunk, and the search is clamped to a window:

| Parameter | Value |
| --- | --- |
| `MinimumChunk` | 8 KiB |
| `AverageChunk` | 32 KiB |
| `MaximumChunk` | 128 KiB |

The average sets the index's granularity. Smaller chunks match more precisely across an edit but
cost more index entries and more per-chunk overhead in the output; 32 KiB against bundles of
tens to hundreds of megabytes keeps the index in the low hundreds of thousands of entries while
still catching a small edit inside a large archive.

Base chunks are indexed by SHA-256. The target is then chunked the same way: a hit becomes a
COPY, merged with the previous COPY whenever it continues where that one ended, so a whole
unchanged region costs one operation instead of thousands; a miss extends a pending INSERT, so a
run of changed chunks costs one operation instead of one each. The builder checks that its own
operations reconstruct the target before it writes the file — a builder that can emit a delta it
knows is wrong will eventually publish one — and it deliberately does **not** decide whether the
delta is worth publishing. That is a ratio, and a ratio is policy.

### The three gates

1. **Eligibility (`ContentResolver::plan_deltas`).** The base is recognised by **name and size
   only**: either `database\<base-bundle>` is present at the delta record's `base-size`, or
   `content-cache\<base-sha256>` is. Nothing is hashed. That is the point — this pass walks the
   whole cumulative edge list on every resolve, and hashing gigabytes merely to decide which
   edges are worth *considering* would cost far more than deltas save. `base-size` comes from
   the delta record rather than from an installed `[bundles]` row, because a player who skipped
   releases often does not have the base in their own manifest at all. Ineligible means the full
   bundle, silently, with no marker.

   Splitting the cheap gate from the real one is safe because a wrong base cannot produce a
   target that hashes correctly. The worst a name-and-size match can cost is a rebuild that gate
   3 throws away — never a wrong file. (The name already carries the first 16 hex of the base's
   own hash, so the gate is not quite as thin as it looks, but nothing rests on that.)
2. **Re-hash immediately before applying (`ContentDelta::Apply`).** The base is hashed in full
   and must equal the header's `baseSha256`, and its size must equal the header's `baseSize`.
   Time passes between deciding and doing, and a delta is meaningless against the wrong base. A
   mismatch here is **transient**, not an accusation: the file on disk changed, which says
   nothing about the published asset.
3. **Authoritative.** The output is hashed as it is written and must equal both the header's
   `targetSha256` and the manifest's `[bundles]` hash before it is renamed from
   `content-cache\<target-sha256>.rebuilt` into `content-cache\<target-sha256>`. A delta can
   therefore be wrong, truncated or hostile and still only ever cost a wasted download: it can
   never produce a file that gets installed, because the commit only ever renames a
   hash-verified cache file.

### Integrity versus transient

Every failure inside `Apply` is classified, and the classification is what decides whether the
delta is remembered as bad or simply retried later. Getting it wrong is expensive in both
directions: blacklisting a good delta upgrades a 20 MB download into a 400 MB one forever, and
retrying a genuinely corrupt one loops.

**Integrity** means the delta itself is wrong with every I/O call having succeeded: bad magic or
version, a header outside the bounds, a delta that ends early, an operation that copies from
outside the base or inserts more than the target can hold, an opcode this build does not
understand, ops that do not sum to `targetSize`, or output that does not match the delta's
**own** header hash.

**Transient** means something outside the delta went wrong: a file that could not be opened, a
disk that filled up, a read or write that failed, a cancellation, or a base that is not what the
header expects.

The subtle one is the last check of all. Output that matches the header's `targetSha256` but not
the hash the *caller* asked for is classified **transient**, because the delta did exactly what
it said it would and it is the chain that was assembled wrongly. Blaming the asset there would
blacklist a good file and turn a small download into a large one permanently.

**Blacklisting is integrity-only.** `rejected-deltas.txt` receives one line per rejected asset
name, and only on an `Integrity` verdict — otherwise one bad afternoon costs the player a full
bundle on every future release. The resolver consults it while walking the edge graph, so a
rejected edge is skipped rather than re-fetched to reach the same conclusion.

It is cleared by `ContentCommit::Run` when the content-id recorded in `content-state.txt`
differs from the one in the manifest being committed. The read happens **before** the state file
is rewritten, because rewriting it is what erases the answer. On a first install there is no
previous id recorded and nothing is cleared — correctly, since there is no previous release for
a stale verdict to have come from. Otherwise: a delta that was wrong for one release says
nothing about the next, whose content set may not even contain the base it was wrong against.

### Cumulative edges and the chain walk

`[deltas]` is cumulative, so a player who skipped releases can be walked forward from whatever
revision they actually hold. The base of a chain is normally one of the *obsolete* files: an
update renames a bundle whose bytes changed, so the previous revision is still sitting in
`database\` under its own name while the new one is missing. That is what the chain starts from.

`plan_deltas` first collects every base hash the installation can already produce, then runs a
**Dijkstra over the edge graph** for each job and takes the chain whose total **asset bytes** are
smallest. Cheapest rather than shortest: two small hops beat one large one, and bytes are what
the player pays. A chain is taken only when it is decisively cheaper — under
`DeltaSkipRatio = 0.35` of the target bundle's size (`ContentResolver.cpp`), the same ratio the
builder applies when deciding whether to publish an edge at all
(`dead_air_x64_content_bundles.ps1`). Below that the saving does not pay for the base re-hash,
the apply pass, the doubled peak cache use and the extra way to fail.

`ContentDownload::apply_chain` walks the hops in order. Each hop's base is whatever the previous
hop produced, or — for the first — the revision the installation already holds, in `database\`
or in the cache. As soon as a hop's output verifies, its delta asset and the previous
intermediate are both deleted, so peak cache use is two bundles at once rather than the whole
path. Delta assets are named
`content_<group>_<shard2>_<baseHash16>_to_<targetHash16>.darpatch`.

## The game side

`ContentService` (`src/xrGame/ui/ContentService.{h,cpp}`) runs two passes split by cost.

`Initialize()` is synchronous and cheap: parse the manifest, verify its content-id, confirm
`$arch_dir$` really is `{app}\database`, then stat every declared bundle. Missing and
wrong-sized files are the realistic failure — a runtime-only patch, an interrupted install —
and this catches them for a few dozen stats, in time for the play gate. It is called lazily
from `PlayBlocked()` too, because a launch driven by `user.ltx` console commands never
activates the main menu.

`StartVerify()` hashes on a worker thread, at most once per session, started from
`CMainMenu::Activate` next to `UpdateService::StartCheck()` and also from `Initialize()` so a
`-start` launch is not left latched for the whole session. A warm installation costs one stat
per bundle; a cold one costs a full read of the content set, and the duration is logged.

States (`ContentService::State`): `Unknown`, `Complete`, `Verifying`, `Repairing`, `Repaired`,
`Incomplete`, `Recovery`. Two are worth explaining:

- `Verifying` is deliberately permissive — the cheap pass already confirmed every bundle is
  present at the right size and the gate already refused anything it did not recognise, so the
  remaining hash check is not a reason to make the player wait. It is reachable only from a
  picture that is already good; an installation the cheap pass condemned is never promoted
  into it.
- `Recovery` means there is no usable manifest, which is distinct from `Incomplete` because
  there is nothing to repair *against*. Repair refuses and says the installation has to be
  reinstalled, rather than starting a download that cannot know what to download.

`Repaired` still blocks play: the bundles are on disk but the filesystem indexed its archives
at startup without them.

The play gate is `CLevel::net_start1` (`src/xrGame/Level_start.cpp`), not a menu button. It sits
on the loading chain rather than on the buttons because `net_start1` is the first step of the
chain `net_Start` queues, and therefore the single chokepoint every route into a level passes
through: `CCC_Start`, `-start`, new game, load, level transition and demo playback. It is also
the only placement that survives `execUserScript()` running `user.ltx` as console
commands — which is exactly what the project's own smoke recipe does. It is a crash guard, not
a product decision: with content unmounted a level load is a missing-asset crash. It refuses
the way every other start failure does — clearing `net_start_result_total` and letting
`net_start6` tear down — rather than refusing from `net_Start` itself before the chain is
queued, which would leave the process with the menu already off, a level object created and
nothing to destroy it.

The dialog (`CUIContentWnd`, layout `gamedata/configs/ui/ui_content.xml`, shipped inside
`xtra_dead_air_x64.xdb0` and never inside a bundle) offers **Repair** or **Exit**. There is no
"play anyway". `CMainMenu::CheckContentDialog` sits above `CheckCrashReportDialog` in the
`OnFrame` precedence chain, and `DrawContentNotice` keeps a persistent banner
(`ui_mm_content_incomplete`) while play is blocked. Strings live in
`configs/text/rus/dead_air_x64.xml` as `st_content_*`.

**Repair always ends in a mandatory relaunch.** This is not a UX preference: the filesystem
indexes archives once at startup, and `CLocatorAPI::unload_archive` is broken — it erases a
single `m_files` entry and breaks out of the loop — so a bundle that arrives mid-session is
not usable in that session. `ContentService::Relaunch` restarts with the same command line.

`ModOptOut` gates neither verification nor repair. It covers the update check and the bug
report only. A modded installation is the *most* likely one to be missing content, and a
silent no-op there would be a per-installation opt-out granted by a mod.

Console commands: `dar_content_verify` (forced full re-hash, ignoring the state cache),
`dar_content_repair` (fetch and install what is missing), `dar_content_state` (dump the current
picture to the log). Read-only or repair-only; nothing that can skip content.

Diagnostics: `capture_content_snapshot` (`src/xrCore/Debug/CrashReport.cpp`) adds `content_id`,
`content_manifest`, `content_incomplete` and `skipped_bundles` to `report.json`. The id is read
by a line scan rather than the full parser because the process is already dying, and every
bundle name goes through the existing `Sanitizer`. See
[`DIAGNOSTIC_REPORTS.md`](DIAGNOSTIC_REPORTS.md).

## Install, update and uninstall

**Install** (`packaging/dead-air-x64/installer/DeadAir-x64.iss`). `DeadAirContent.exe` — the
updater binary under another name — and `content-manifest.txt` are carried as `dontcopy`
payload. Ordering is the whole point:

1. `NextButtonClick` at `wpSelectDir` checks free space against `ContentBytes` before the
   download. `ExtraDiskSpaceRequired` is a display value only; Inno evaluates it after the
   content page has already spent the bandwidth.
2. `PrepareToInstall` runs `RunContentFetch` **first**, before any backup, before
   `RemoveObsoleteManagedFiles`, before `install-mode.txt` / `managed-files.txt` /
   `port-version.txt`. A failure there aborts the install with the installation untouched.
   A silent Setup fetches exactly like an interactive one — there is no page to skip, so
   content cannot become optional by accident.
3. The manifest is staged to `content-cache\pending-manifest.txt`, a fixed path the fetcher
   knows. It is never named on a command line, so nothing external can nominate what
   "complete" means for this installation.
4. The fetcher holds the named mutex `ContentPaths::FetchMutexName` for its lifetime and
   writes `content-fetch-progress.txt` on every tick (through a temporary, so the installer
   never reads a torn line). The wizard polls the mutex to tell "still working" from "died
   without writing a result", and the progress file's timestamp to tell "slow" from "stalled".
   Cancel is a flag file the fetcher watches; whatever landed stays in the cache as resume
   state.
5. Only then does the latch get written and the payload get installed. `CurStepChanged` at
   `ssPostInstall` runs `--content-commit`. An Inno install cannot be failed from there, so a
   failed commit is not a rolled-back install: the latch stays, the gate refuses what it
   cannot vouch for, and the first launch goes to repair.

Install and uninstall also refuse while the game is running: the engine holds a named mutex for
its whole lifetime (`src/xr_3da/entry_point.cpp`, `ContentPaths::GameMutexName`) and `[Setup]
AppMutex` names the same string, `Local\DeadAirRefined.Game`, which Inno records into the
uninstall data as well, so the guard covers both directions. It is not a single-instance guard —
an existing mutex is fine, the handle just has to outlive the process. `Local\` rather than
`Global\`: a global mutex needs a privilege a standard user does not reliably hold, and setup
stays in the same session even when it elevates.

Updater exit codes: `0` ok, `26` the work could not be completed, `27` the installation cannot
be worked on at all. `--content-plan` reports what would happen and whether there is room,
writing nothing. `--content-fetch` downloads and verifies into the cache. `--content-commit`
installs from the cache and is idempotent — arriving with the work already done is success.
`RestartAndApply` appends `--content-commit` as a belt to the game's braces; an updater built
before the content system silently ignores the unknown argument, which is what makes the first
content release work.

**Update** (`UpdateService.cpp`, `prefetch_content`). Content cannot travel in the update ZIP, so
the incoming version's content is put in place around the update rather than inside it. Once the
payload is downloaded and before the update is offered as ready, the service fetches
`Dead-Air-Refined-<version>-content-manifest.txt` and **verifies it against the `sha256:` digest
the release's own asset listing carries for it** before parsing a line — this file decides what
the next gigabytes are, so an unverified copy of it is worth nothing — then checks its
content-id and resolves it against the installed `database\`. Usually there is nothing to do and the pass
ends there. When there is, the latch is written **before the first byte lands** (reason
`update-commit`), because the cache is about to hold bytes for a version that is not installed
and a crash in between must not read as a healthy installation.

**Into the cache and no further.** Installing here would mean moving files the running game has
mapped and demoting the bundles it is currently reading, and it would survive neither. The
rename happens after this process exits — that is what the appended `--content-commit` is for —
so the player pays one restart rather than a restart and then a download. The whole prefetch is
best-effort: every failure is logged and the update proceeds anyway, because the worst outcome
is the installation the player would have had regardless — incomplete, offering to repair, with
whatever did arrive still in the cache as resume state.

**Bundles are never `ManagedFiles`** (`BuildManagedFiles`, `DeadAir-x64.iss`). Their names
change whenever their bytes do, so a bundle in that list would be deleted from `database\` the
moment a version renamed it — outside the content commit, destroying the very file the commit
wanted to demote — and copied into every backup at gigabytes a time. `content-manifest.txt` is
different and does belong: two kilobytes, and the record of what the installation should
contain.

**Uninstall** (`CurUninstallStepChanged` → `RemoveContentBundles`). Content goes **first**, and
the ordering is load-bearing: `content-manifest.txt` is a managed file, so the runtime removal,
the x86 restore and `RemoveControlMetadata` all delete it, and losing it before the sweep would
destroy the only record of what had to be removed. The function refuses outright if `database\`
is a reparse point (a junctioned `database\` shared between installations would lose both),
deletes every `[bundles]` entry, then sweeps `xtra_dead_air_x64_content_*.xdb0` for anything a
failed mid-update state left behind, then removes `content-cache\`. A file that will not delete
gets `MoveFileEx(..., DELAY_UNTIL_REBOOT)`, and whether *that* succeeded decides the return
value — survivors are named to the user rather than assumed gone.

`[UninstallDelete]` deliberately has **no** `database\xtra_dead_air_x64_content_*.xdb0` entry:
Inno's wildcard deleter has no reparse-point check. `RemoveContentBundles` is the sole delete
authority for that directory. `RemoveControlMetadata` removes all four content files —
`content-manifest.txt`, `content-state.txt`, `content-incomplete.txt` and
`rejected-deltas.txt` — and sweeps `appdata\vfs-index-*.cache` in code rather than only through
`[UninstallDelete]`,
because those entries are baked into `unins000.dat` at install time and never run for an
installation made by an earlier Setup — which is exactly the one carrying the accumulated
index caches.

## Building and publishing

### The assets repository holds no content

`MMadmer/Dead-Air-Refined_Assets` exists to hang releases off, not to store bytes. Its
`.gitignore` excludes the authoring tree, the bundle cache and the generated manifest, so no
content object ever enters git there; the bytes live only as release assets, which are already
content-addressed and immutable without git's help. What is versioned is a README and
`index/content-<version>.txt`, the ledger `publish_dead_air_x64_content.ps1` writes after an
upload — a few kilobytes per release, and the only local record that makes the never-replace
rule auditable without asking GitHub.

One consequence is easy to trip over: a GitHub release hangs off a tag, and a tag needs a
commit. A repository with zero commits cannot take a release at all, so
`gh release create` — and therefore the publisher — fails against a genuinely empty one. The
README commit is what makes the repository publishable.

Neither the authoring tree nor the bundle cache can be regenerated from anything in git, and a
repack is not guaranteed to reproduce published bytes, so both need a backup that is not this
repository.

### The builder

`tools/package/dead_air_x64_content_bundles.ps1` turns an authored gamedata tree into bundles
plus the manifest:

```powershell
dead_air_x64_content_bundles.ps1 -SourceRoot <tree> -BundleCache <dir> `
    -OutputManifest <path> -PortVersion 1.4.0 -ReleaseTag content-1.4.0 `
    [-GroupsFile <path>] [-DisjointFrom <compat archive>] [-AllowRepack] `
    [-PreviousManifest <path>] [-DeltaTool <path>]
```

It hashes the source in parallel, assigns each first-level directory to a group and shard
(appending new ones to `groups.ltx`), computes the content digest per bundle, and reuses the
cached bundle when that digest already has one. Reuse is what keeps an unchanged bundle free
across releases; `-AllowRepack` forces packing and exists for diagnosis only, because a repack
may produce different bytes and force every player to re-download identical content.

**Never delete the bundle cache.** It is what makes an unchanged bundle free, and losing it
while a release is unreachable forces a repack that may not reproduce the published bytes.

Each freshly packed bundle goes through `New-XdbArchive`
(`tools/package/dead_air_x64_archive.ps1`), which packs with `converter.exe -pack -xdb` and
then unpacks and compares every member hash. The generated header sets `auto_load = true`,
`entry_point = $fs_root$\gamedata\` and `level_ver = <content digest>`; the compatibility
archive keeps its hand-maintained `level_ver`, since it is version-coupled to the engine and
bundles are not. The builder also asserts that no vpath appears in two bundles, and (with
`-DisjointFrom`) that none appears in the compatibility archive as well.

`tools/package/build_dead_air_x64_installer.ps1` takes `-ContentManifest`, compiles the
`xrContentSync` sources straight into `DeadAirUpdater.exe`, copies that binary to
`DeadAirContent.exe`, derives `ContentBytes` from the manifest's declared sizes plus 10 %, and
hard-throws when the manifest is missing or declares no bundles. `SHA256SUMS.txt` covers the
installer artefacts; it structurally cannot cover bundles, which are covered by the manifest
itself and by digest verification against the assets release.

### The local development loop

Content changes several times a day while a version is being built, and none of it is published
yet. That leaves the developer's own installation as the one machine that can never satisfy its
own manifest: the game would sit in `Incomplete`, and its repair button would ask GitHub for a
`content-<version>` release that does not exist. Publishing a release per edit is not an answer —
a published tag is permanent.

`tools/package/deploy_dead_air_x64_content.ps1` closes that loop. It does what
`ContentCommit::Run` does, against the bundle cache instead of a download cache: latch, install
everything the manifest names, demote what it no longer names into the content cache, write the
manifest, drop the state cache, re-verify by hash, clear the latch. It is a development tool and
ships to nobody.

```powershell
tools\package\dead_air_x64_content_bundles.ps1 -SourceRoot <tree> -BundleCache <cache> `
    -OutputManifest <manifest> -PortVersion 1.4.1 -ReleaseTag content-1.4.1
tools\package\deploy_dead_air_x64_content.ps1 -Manifest <manifest> -BundleCache <cache> `
    -GameDir "D:\Games\Dead Air"
```

The one rule it does not relax is the one that matters: nothing enters `database\` that has not
matched the manifest's SHA-256 first, and the copy goes through a `.deploy-part` temporary so an
interrupted run cannot leave a short file under a name that promises a hash. A bundle cache that
does not match the manifest stops the run rather than installing something the game will reject.

Two consequences worth stating. A QA rig whose `database` is a junction to the real installation
only needs the real one in `-GameDir`; the junction carries the bundles across. And the game must
be restarted afterwards — archives are mounted once at startup, so a bundle that arrives during a
session is not picked up, which is the same reason an in-game repair ends in a mandatory relaunch.

### Authoring rule for `.ogg`

X-Ray reads its own sound block out of `user_comments[0]` as raw bytes — a u32 version, then
min/max distance, base volume, game type and AI distance
(`CSoundRender_Source::LoadWave`). Every ordinary encoder writes its own tag there instead
(`encoder=Lavc...`), which the engine reads as an unrecognised version and reports as
`! Invalid ogg-comment version` on every load. The file still plays: with nothing readable the
engine keeps `SoundSourceInfo`'s defaults (minDist 1, maxDist 300, maxAIDist 300, gameType 0).
What is lost is the file's ability to state its own distances, and the log fills with a warning
per sound per start.

So every `.ogg` authored outside the SDK is stamped before it is packed:

```powershell
python toolsudio\stamp_xray_ogg_comment.py <file.ogg> <minDist> <maxDist> <volume> <gameType> <maxAIDist>
```

It rebuilds only the page carrying the comment header, leaves the audio bytes untouched, keeps
the encoder's own tags after the block, and is idempotent — re-stamping the same values
reproduces the file byte for byte, which matters because a changed byte is a changed bundle and
a re-download for everyone.

Set `gameType` to 0 for anything ambient. A non-zero game type raises an AI sound event, and
wind rustle that NPCs can hear is a rustle that gets the player shot at.

### What the first content set actually is

The cutover measured, so the numbers in this document are not projections. 1.4.0 moved 40
binary files out of `packaging/dead-air-x64/compatibility/gamedata/` — 19,204,670 B, 94.4 % of
the compatibility tree by size: 29 in `textures/`, 5 in `sounds/`, 4 in `meshes/`, 2 in
`anims/`. The 130 files that stayed are scripts, shaders and configs, which are version-coupled
to the engine and belong in a payload that ships with it.

Two consequences worth keeping in view. The compatibility archive drops from about 20.3 MB to
roughly 1.15 MB, which is what makes an update archive small again. And the shard table is
pinned for the projected 5 GB rather than for today's 19 MB: at a 250 MB target the current set
is one shard per group, and letting it stay that way would mean re-sharding later, which
renames bundles and costs every installed player a full re-download.
`tools/qa/Test-ContentScale.ps1` exists to keep that projection honest — it runs the real
layout policy over a synthetic 5 GB tree.

QA: `tools/qa/Start-ContentAssetMock.ps1` serves the `releases/download/<tag>/<asset>` shape on
loopback with `-NoRange`, `-DropAfter` and `-Throttle` modes; `tools/qa/Test-ContentFlow.ps1`
drives the real updater binary against it (fetch, resume, a 200 answer to a ranged request, a
tampered cache file, commit, latch, and one end-to-end delta);
`tools/qa/Test-ContentGate.ps1` boots the real engine on
the QA rig for the gate cases — missing, truncated, corrupt index, flipped byte, stale and
unrecognised leftovers, no manifest, leftover latch. `docs/dead-air/TEST_MATRIX.md` carries the
release gates.

## Invariants

These are the rules the rest of the system is allowed to assume. Each one is load-bearing for
something specific.

1. **`database\` contains only complete, hash-verified bundles the installed manifest
   declares.** Nothing partial, nothing intermediate, nothing from another version. This is
   what lets the engine mount a bundle without validating it first.
2. **A bundle's filename carries the SHA-256 of the packed file.** Name and bytes are 1:1
   forever, which is what makes an unchanged bundle free and a replaced asset unrecoverable.
3. **Bundles enter `database\` only by renaming a cache file whose hash already matched.**
   Nothing writes into `database\` directly, so a partial file or a delta output cannot
   acquire a bundle name — structurally, not by a check somebody could remove.
4. **Add before delete, always.** Obsolete bundles are demoted to the cache after the
   replacements are in place, never deleted inside a commit. Too many bundles is a tidy-up;
   too few is a broken game.
5. **Bundles are never managed files.** Their whole lifecycle belongs to the content commit
   and the cache collector. Anything else that deletes by list would delete them at the wrong
   moment, and anything that backs up by list would copy gigabytes.
6. **Content has no opt-out, no skip switch and no build flag that omits it.** The maintenance
   installer is a separate compile that physically cannot contain the fetch block; the QA
   override redirects the source and never skips the fetch.
7. **The trust root is the installed `{app}\.dead-air-x64\content-manifest.txt`**, which
   arrived inside a hash-verified payload. No manifest path is ever accepted on a command
   line. The one manifest that has not been installed yet — a fresh install's — is staged at a
   fixed path under the cache and found there, never named by the caller.
