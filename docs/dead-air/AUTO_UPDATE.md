# Dead Air: Refined automatic updates

## Release contract

Each published version uses one GitHub Release whose tag is a strict Semantic
Version in `MAJOR.MINOR.PATCH` form. The release contains these assets:

- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Setup.exe` for the recommended guided
  installation;
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Update.zip` — the complete runtime
  payload, for manual extraction and as the update every installation can
  always take. It deliberately carries no content bundles. The name is fixed
  for the whole 1.x line: it is the one asset every client since 1.0 looks
  for, and a release without it is invisible to every installation in the
  field (a rename to `Setup_Manual.zip` was tried and reverted for exactly that
  reason);
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-Update_Patch.zip` — optional. Carries only
  the files that differ from the previous release, so an up-to-date player
  downloads a fraction of the full archive. Only a client that knows the patch
  (1.4.0 and later) selects it; older clients take the full archive, and the
  updater that applies a patch is the one already installed, so the first
  release whose patch is ever used is the one after 1.4.0;
- `Dead-Air-Refined-MAJOR.MINOR.PATCH-content-manifest.txt` — the content
  manifest of that version: the same file the payload installs as
  `.dead-air-x64\content-manifest.txt`. It is published on its own so a player
  or a tool can read which content bundles a version pins without installing
  it. The game downloads it for one purpose only: once the update archive is in
  hand, `prefetch_content` (UpdateService.cpp) reads the target version's
  manifest (GitHub digest checked, content-id recomputed), plans what the
  installed content lacks - deltas included - and stages it into
  `content-cache\` before the update is armed, so the restart is one rename and
  not another download. Nothing is installed from it: the commit after the
  restart works from the manifest the payload put under `.dead-air-x64\`, and
  no manifest path is ever accepted from outside the installation.

The game reads the public release list for
`MMadmer/Dead-Air-Refined`. Drafts, prereleases, malformed tags and releases
without a usable full archive are ignored. No separate release is required
for the installer: GitHub Releases support multiple assets under one tag.

A game release is publishable only once the content release its manifest names
is already published. That is a publishing obligation, not something the client
can check: the client trusts the installed manifest and, if the assets are not
there yet, simply fails to fetch them. See the release runbook below.

Every archive must have a non-zero size and a GitHub-provided SHA-256 digest.
The full archive contains the same installed runtime payload as the setup
program, including the updater, maintenance program and uninstaller launcher.
Neither of them contains content bundles.

The release description follows the bilingual template in `PROJECT_RULES.md`.
The English `## Changes` list must be inside `## EN`, and the Russian
`## Изменения` list must be inside `## RU`. Only bullet items from those exact
sections are shown by the client; installation instructions and the other
language are never included in the update dialog.

Each language block may also carry an optional `## Theme` / `## Тема` section
holding one short line — the headline of the release. The dialogs print it in
bold above the change list; a release without the section simply has no such
line. Only the first non-empty line is read, a bullet list there is ignored,
and anything longer than 256 bytes is dropped. The section is a 1.4.0 feature
and is off limits while 1.3.x clients are still updating: their parser treats
any `## ` heading between the language heading and `## Changes` as the end of
the block and shows no changes at all. The 1.4.0 release body had its theme
sections removed by hand for exactly that reason. If a theme is ever published
in the 1.x line, it must follow the change list, which both parsers accept.

References:

- [GitHub REST API for releases](https://docs.github.com/en/rest/releases/releases)
- [GitHub REST API for release assets](https://docs.github.com/en/rest/releases/assets)
- [About GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)

## Client behavior

The check starts once per game process after the main menu has initialized. An
empty release list, a network error or the absence of a valid newer release does
not open a dialog and does not block the menu. Updates inside the installed
major line are always checked — those are the ones a player is expected to take,
so there is no switch that turns them off.

The ordinary offer is confined to that major line: a build on 1.9.0 is offered
1.9.1 and 1.12.0, never 2.x. Crossing a major is not an update — saves and mods
do not carry over — so a release from a higher major line is announced by its own
dialog, which links to the release page and asks the player to install it into a
separate folder, leaving the current build in place. That notice is decided
independently of the ordinary offer and both can be pending at once: the notice
comes first, the update dialog behind it, and it reappears on every launch whose
check succeeds.

Only that notice can be switched off, by the checkbox inside it or by
`Уведомлять о крупных версиях` in the Game options — one setting, backed by the
`dar_major_update_notice` console command and persisted in `user.ltx` like any
other. With it off the check still runs and still offers updates within the
installed major line.

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

Two other dialogs outrank the update offer, in this order. The main menu tries
the content repair dialog first, the unhandled-crash report prompt second, and
reaches the update dialog only when neither is showing: an installation that
cannot load a level has nothing useful to say about updates or crash reports,
and a crash that has not been reported yet is worth more than an offer that
will still be there next launch. The update check itself keeps running to
completion in the background; only its dialog waits.

The version comparison uses the product version compiled into the engine. The
updater does not modify or infer that version independently.

## Mod opt-out

Mods can take the installation out of the update flow entirely by declaring themselves in
`[auto_update_opt_out]` (in `configs/dead_air_x64_mod_opt_out.ltx` or in `system.ltx` via an
XMS `.ltxp` patch), or by calling `main_menu.disable_auto_update("<name>")` from Lua. The
check then never starts, the bug report menu entry disappears, and the menu shows
`Автообновление отключено модами:` followed by the declared names. See
`docs/dead-air/MODDING.md` for the full contract.

The opt-out covers the update check and the bug report, and nothing else.
Content verification, the content latch and the repair path are outside it —
`ContentService::StartVerify()` runs beside `UpdateService::StartCheck()`
unconditionally. A modded installation is the one most likely to be missing
content, and a mod must not be able to grant itself a content opt-out.

## Update application

The game copies the external updater into the version-specific cache and exits.
The updater then:

1. waits for the game process to finish;
2. validates the downloaded archive and every entry in `update-manifest.txt`;
3. rejects absolute paths, parent traversal, duplicate paths, more than 1024
   files and more than 1 GiB of expanded size;
4. takes a `refined-version` snapshot of the managed file set;
5. applies the payload with atomic file replacement, then deletes the files in
   the snapshot scope that the incoming manifest does not name;
6. refreshes the installed maintenance program and uninstaller metadata;
7. commits content, when the command line carried `--content-commit`;
8. deletes the snapshot;
9. starts a cleanup instance, removes the downloaded cache and restarts the
   original game command line.

If payload application, the maintenance pass or the consistency check after it
fails, the saved snapshot is restored. On success the snapshot is deleted. It
is the transaction rollback for this one update — not a retained version and
not a rollback target. Nothing read it afterwards and nothing pruned it, so
`backups\` used to grow by the whole managed set on every single update.

Content bundles are never inside that snapshot. `create_backup` strips every
bundle-shaped name out of the managed list before it copies anything, because
the two halves of the snapshot would otherwise work against each other: the
backup cannot restore a bundle, since the update manifest never declares one,
while step 5 deletes everything in scope the incoming manifest does not name.
One bundle name in `managed-files.txt` would therefore be silent, permanent
data loss. The snapshot restores the runtime and never the content, which is
safe because the content commit only ever adds — obsolete bundles are demoted
to the content cache rather than deleted, so a failed update cannot destroy
content the snapshot could not have restored.

The original x86 snapshot remains reserved for complete removal and is not used
as an update rollback target.

## Content and the update

Content is not carried in the update archive, and that is a property of the
applier rather than a preference. It refuses an archive with more than 1024
entries or more than 1 GiB of expanded payload, and its second stage waits at
most five minutes on the process it hands off to. A multi-gigabyte content set
survives none of the three. Bundles therefore travel as release assets of the
assets repository, pinned per game version by
`.dead-air-x64\content-manifest.txt`, and are obtained by the installer or by
the game's repair path.

`.dead-air-x64\content-manifest.txt` is a managed file: two kilobytes, and the
only record of what the installation should contain. Being managed, it must
also be carried in the payload and listed in the update manifest, like every
other managed file — step 5 above deletes managed files the incoming manifest
does not name, and an installation left without a manifest cannot say what it
should contain. It latches in recovery, refuses to start a level, and has to be
reinstalled rather than repaired, because fetching the manifest itself is a
reinstall and not a repair. Content bundles are the exact opposite on every
count: never payload, never managed, never in the snapshot.

Acquisition happens either before an update — the installer's own fetch, or a
repair the player already ran — or after it, through the repair dialog on the
next launch. The update flow itself downloads the runtime archive and nothing
else. What the update does do with content is commit it: `RestartAndApply`
always appends `--content-commit` to the updater's command line. An updater
from before the content system ignores the unknown token, which is what lets an
installation still on an older build apply a content-bearing payload; a newer
one runs the commit at step 7, when the payload is in place and the manifest
under `.dead-air-x64` is already the target version's. A commit failure there
is not an update failure: the runtime is complete and correct, the latch is
still set, and the next launch reports the installation incomplete and repairs
it. Rolling the whole update back would turn a self-healing state into a lost
one.

The updater's three content modes are `--content-plan` (report what would have
to happen and whether there is room for it, writing nothing), `--content-fetch`
(acquire into the cache) and `--content-commit` (move what is verified into
`database\` and clear the latch when nothing is left). Each takes `--game-dir`
and nothing else; a fetch also accepts `--cancel-flag`. None of them accepts a
manifest path — the trust root is the installed manifest at its pinned
location, and a path on a command line would let anything nominate what
"complete" means. They run standalone, without `--wait-pid`, without a dialog
and without the five-minute wait, because a fetch legitimately takes an hour.
Everything they know reaches the caller through the exit code and
`content-cache\content-fetch-result.txt`: 0 done, 26 the work could not be
completed, 27 the installation cannot be worked on at all.

What a hand-extracted `Update.zip` produces therefore depends on what it
is extracted over. The archive carries `.dead-air-x64\content-manifest.txt` and
no bundles, so extracting it over an existing installation replaces the manifest
and leaves the bundles alone: everything the new manifest still pins is already
in `database\`, and only the bundles the release actually changed are missing.
That is the ordinary case, and it is indistinguishable from any other update —
the installation comes up `Incomplete` only if something changed, and the repair
fetches the difference rather than the set. Extracted into a folder Refined was
never installed into, the same archive gives the installation a manifest and not
one byte of the content it names. On first launch the content service parses the
manifest, finds every declared bundle missing, writes
`.dead-air-x64\content-incomplete.txt`, settles in `Incomplete`, refuses to
start a level and opens the repair dialog, which fetches the full set and ends
in a mandatory relaunch. Neither outcome is a failure mode: shipping the
manifest without the bundles keeps the archive inside the applier's limits while
the content set is measured in gigabytes, and it is also what keeps the manual
path repairable at all. An installation that arrived without a manifest would
land in `Recovery` instead, and `Recovery` has no repair — there is nothing to
repair against. `Setup.exe` does the same fetch on its own progress
page, first in `PrepareToInstall` and before anything in the installation is
changed, so a failed fetch aborts the install with the installation untouched.
A silent Setup fetches exactly like an interactive one — there is no page to
skip, so content cannot become optional by accident.

## Archive manifest

A full archive uses schema `dead-air-refined.update/1`:

```text
schema=dead-air-refined.update/1
version=MAJOR.MINOR.PATCH
<sha256><TAB><size><TAB><relative-path>
```

The manifest must list every regular payload file exactly once, and the payload
must include `.dead-air-x64/content-manifest.txt`, so that an update installs
the content manifest of the version it delivers rather than deleting the one
already there. The ZIP may contain only those files, directory entries and the
manifest itself — never a content bundle. Every
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

Both schemas are frozen. The updater that applies an archive is the one already
installed, so an older updater must keep working on the full archive it
downloads; only patches use `/2`, and only a client new enough to choose a
patch will ever hand one to an updater new enough to read it. The content
system changes neither of them: content has its own manifest under its own
schema, and its entire footprint in the update format is one more payload file
and one more command-line flag.

## Build and QA tooling

`tools/package/build_dead_air_x64_installer.ps1` builds every asset a release
carries. `-PortVersion` must equal the version compiled into
`src/xrCore/ProductVersion.h` or the build refuses to start. `-ContentManifest`
points at the manifest produced for this release; it is mandatory and has no
default at all, and a manifest that is missing or declares no bundles is a hard
failure rather than a Setup that installs no content. The declared bundle sizes
plus a tenth become the installer's disk-space requirement, so the wizard cannot
ask for 200 MB and then die an hour into a multi-gigabyte fetch. Pass
`-PreviousFullArchive <path to the previous Update.zip>` to cut the patch, and
`-PatchOnly` to re-cut a patch against a different base without rebuilding the
release.

`tools/package/dead_air_x64_content_bundles.ps1` builds the bundles and the
content manifest. `-SourceRoot`, `-BundleCache`, `-OutputManifest`,
`-PortVersion` and `-ReleaseTag` are mandatory; `-GroupsFile` defaults to
`packaging/dead-air-x64/content/groups.ltx`, and `-DisjointFrom` takes the
compatibility archive so the build fails if any virtual path exists in both.
`-AllowRepack` forces a repack over a cache hit and is diagnostic only. There
is no switch that skips content. `-BundleCache` is what makes an unchanged
bundle free across releases — it is not build output and must never be deleted.

The same script also cuts the deltas, and there is no separate step for them.
Given `-PreviousManifest <the previous release's content-manifest.txt>` it
diffs every bundle whose bytes changed against the previous revision in the
cache, carries forward every edge that manifest already listed, and writes the
`[deltas]` section itself. It shells out to `DarDelta.exe`, which it expects at
`bin\x64\Release\DarDelta.exe` and which `-DeltaTool` points elsewhere for any
other configuration. `DarDelta` is a CMake target at `src/utils/DarDelta`, so
the ordinary engine build (`tools\build\build_x64.ps1`) produces it under
`bin\x64\<configuration>\`; the installer build's native-helpers step has
nothing to do with it — that step compiles the uninstaller launcher, the updater
and the content fetcher, and nothing else.

`tools/package/publish_dead_air_x64_content.ps1` uploads what that manifest
names. `-Manifest` and `-BundleCache` are mandatory; `-AssetsRepo` defaults to
`MMadmer/Dead-Air-Refined_Assets`, `-AssetsClone` to
`D:\Games\Dead-Air-Refined_Assets`, and `-DryRun` reports what would be uploaded
and touches nothing. Every check it makes exists because a published content
asset can never be taken back: it recomputes the content-id from the bundle rows
and refuses a manifest whose id does not match, hashes every named asset in the
cache against the manifest before it opens a connection, and refuses outright to
replace a name already published at a different size. It writes the release
ledger into `index\` in the assets clone but does not commit it, and it never
touches the game release.

QA scripts, all under `tools/qa`:

- `Test-UpdatePatchFlow.ps1` exercises the applier end to end on a synthetic
  installation;
- `Start-UpdateApiMock.ps1` serves a fake release list on loopback for the
  in-game flow (`-qa_update` plus `DAR_QA_UPDATE_API`);
- `Start-ContentAssetMock.ps1` serves bundle downloads on loopback in the
  `releases/download/<tag>/<asset>` shape, with `-NoRange`, `-DropAfter` and
  `-Throttle` to produce the failures a healthy connection never reaches;
- `Test-ContentFlow.ps1` drives the shipped updater's content modes against
  that mock on a scratch installation: a clean fetch and commit, a complete
  installation that plans no work, a repeatedly dropped transfer that still
  completes, proof that it resumed rather than restarted (the mock saw `Range`
  requests from a non-zero offset), a clean restart when the server answers a
  ranged request with `200`, and a commit that refuses a tampered cache file;
- `Test-ContentGate.ps1` boots the real engine on the QA rig once per case and
  checks that the mount gate, the verifier, the latch and the play gate agree:
  a healthy install, a second launch served from the state cache, a missing
  bundle, a truncated one, a corrupt index at the correct size, a single
  flipped data byte, stale and unrecognised leftovers that must not block play,
  a missing manifest that must land in recovery rather than silence, and a
  leftover latch on an intact install that has to clear itself;
- `Run-ContentProbe.ps1` is the single-boot probe those cases are built on. It
  appends console commands to `user.ltx`, which is also the path the play gate
  has to survive.

## Release runbook

The order below is the release procedure, and steps 5 and 6 are ordered on
purpose.

1. Clean build, with `src/xrCore/ProductVersion.h` bumped. The installer build
   refuses to run when `-PortVersion` disagrees with it. The same build produces
   `bin\x64\Release\DarDelta.exe`, which the next step needs.
2. `dead_air_x64_content_bundles.ps1` — new bundles, the deltas and the new
   `content-manifest.txt`. Review the `groups.ltx` diff before anything else:
   the builder appends an `[assign]` line for every new content directory, and
   such a line is a permanent commitment. Editing one later moves files between
   bundles, changes both bundles' bytes, changes both filenames, and makes
   every installed player re-download both. Pass `-PreviousManifest` pointing at
   the previous release's `content-manifest.txt` and the script cuts the deltas
   and writes the `[deltas]` rows itself: one edge per bundle whose bytes
   changed, skipped whenever the delta is not below 0.35 of its target, because
   below that the saving does not pay for the base re-hash, the apply pass and
   the extra way to fail — the resolver in the engine weighs a delta by the same
   ratio. Delta rows are cumulative: every edge ever published for a bundle that
   still exists stays listed, so an installation that skipped releases is walked
   from whatever revision it actually holds instead of being handed the full
   bundle. Omit `-PreviousManifest` and the release simply ships no delta edges,
   which is correct for the first content release and means a whole-bundle
   re-download for every installed player on any release after it. A delta also
   needs the previous bundle in `-BundleCache` to diff against; when it is not
   there the script says so and publishes the whole bundle instead.
3. `build_dead_air_x64_installer.ps1`, with `-ContentManifest` from step 2 and
   `-PreviousFullArchive` pointing at the previous `Update.zip`. This
   produces Setup, `Update.zip`, the patch and `SHA256SUMS.txt`.
   `SHA256SUMS.txt` covers the distribution files only —
   bundles are covered by the per-bundle SHA-256 in the content manifest and by
   the hash check step 5 runs before it uploads anything, and structurally
   cannot be in it.
4. QA: `Test-ContentFlow.ps1`, `Test-ContentGate.ps1`,
   `Test-UpdatePatchFlow.ps1`, and a probe against the real host if the
   downloader changed.
5. Ask for permission, then publish the **content** release first, with
   `publish_dead_air_x64_content.ps1 -Manifest <the manifest from step 2>
   -BundleCache <the cache step 2 packed into>`. It creates the
   `content-MAJOR.MINOR.PATCH` tag in the assets repository when it is missing
   and uploads one asset per bundle and per delta, so there is nothing to do by
   hand. Before it opens a connection it recomputes the content-id from the
   bundle rows and refuses a manifest whose id does not match — a manifest every
   client would reject as tampered is better caught here than after its assets
   exist — and it hashes every named asset in the cache against the manifest.
   An asset already published under the same name and size is skipped rather
   than re-uploaded; one published at a different size stops the run, because a
   published asset is never clobbered and never replaced (`PROJECT_RULES.md`
   §11), and the name carries the hash of the bytes, so the two disagreeing
   means something upstream produced a different file under a name that
   promised otherwise. Republish under a new tag instead. `-DryRun` prints the
   upload list and changes nothing, which is worth running first. What the
   script does not do is commit: it writes the ledger into `index\` in the
   assets clone and leaves the commit to you. To see what GitHub itself holds,
   `gh api repos/<assets-repo>/releases/tags/<tag> --jq '.assets[] |
   [.name,.digest] | @tsv'`, because `gh release view --json assets` does not
   expose `digest`.
6. Ask for permission, then publish the game release and verify it per
   `PROJECT_RULES.md` §11, including
   `repos/MMadmer/Dead-Air-Refined/releases?per_page=30` — that page, not the
   "Latest" badge, is what the client actually reads.

The manifest inside a game release names, for every bundle, the content release
tag that holds it. Publish the game release first and every installation made
or updated in the window between the two resolves its bundles to a tag that
does not exist yet — and the window is real, not theoretical: it spans the
uploads and a permission prompt. Those installations do not degrade into a
playable state with missing assets. They latch, refuse to start a level, and
offer a repair that cannot succeed until the content release they were pointed
at exists. The correct order costs nothing extra: a content release published
without a game release behind it is an orphan tag that no manifest names —
harmless, never deleted, and either picked up by the next release or left
alone.

Publication and verification rules are defined in
[`PROJECT_RULES.md`](../../PROJECT_RULES.md).
