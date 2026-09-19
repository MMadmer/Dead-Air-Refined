# XMS module metadata, the Mods menu and module updates

This document is the contract between Dead Air: Refined, XFined Editor and every
published XMS module. Sections marked **frozen** describe what an installed
module or a published release relies on; they change only by addition.

## 1. Manifest metadata

`mod.ltx` gains optional keys. An engine that does not know them ignores them.

```ini
[module]
id          = madmer.agroprom_story
name        = Agroprom Story
version     = 1.2.0
author      = Madmer
description = "A quest line on Agroprom.\nRequires a new game."
website     = https://ap-pro.ru/forums/topic/1234-agroprom-story/

[update]
github      = Madmer/agroprom-story
```

| Key | Meaning |
|---|---|
| `[module] author` | Display text. |
| `[module] description` | Display text, may span lines through `\n`. |
| `[module] website` | The module's page. AP-PRO or ModDB only, see 1.3. |
| `[update] github` | `owner/repo` of the GitHub repository whose releases update the module. |

### 1.1 Text (**frozen**)

- `mod.ltx` is UTF-8 without BOM. A BOM is skipped; a file that is not valid
  UTF-8 is read as Windows-1251.
- `name`, `author` and `description` may be enclosed in double quotes. Inside
  quotes `\n` is a line break, `\"` a quote and `\\` a backslash, and `;` does
  not start a comment. Every other value is taken as written.
- `name`, `author` and `description` may be a string table id. The Mods menu
  resolves an id through the game's string table - the module ships the text in
  its own `gamedata\configs\text\<language>\*.xml` - and shows anything that is
  not an id as written. A disabled module is not mounted, so its ids stay
  unresolved until it is enabled.

### 1.2 Version (**frozen**)

One to four dot-separated decimal numbers: `1`, `1.2`, `1.2.0`, `1.2.0.4`.
Missing components count as zero, so `1.2` equals `1.2.0`. Anything after the
numeric part is ignored for ordering. A version that does not start with a
number is older than every valid one. A release version is at most 32
characters of `A-Z a-z 0-9 . _ + -`; XFined Editor writes numbers and dots only.

### 1.3 Website (**frozen**)

The game opens a website only when all of these hold; otherwise the key is
ignored and the button stays disabled:

- the scheme is `https://`, in lower case;
- the host - everything up to the first `/` - is exactly `ap-pro.ru`,
  `www.ap-pro.ru`, `moddb.com` or `www.moddb.com` in any case, with no user info
  and no port;
- the rest consists of `A-Z a-z 0-9 - . _ ~ / ? # & = % +` only;
- the whole URL is at most 512 bytes.

XFined Editor refuses to save any other value. The allow-list is code, not
configuration: extending it is a game release.

### 1.4 Update source (**frozen**)

`owner/repo`: the owner is 1-39 characters of `A-Z a-z 0-9 -`, the repository is
1-100 characters of `A-Z a-z 0-9 - . _` and is neither `.` nor `..`. One key
names one provider; another provider would be another key.

## 2. Release contract (**frozen**)

A module release is an ordinary GitHub Release. The game looks only at the
release GitHub marks **Latest** - drafts and pre-releases are invisible - and
reads three kinds of assets from it:

| Asset | Name | Read |
|---|---|---|
| Release descriptor | `<module id>.update.ltx` | on every check |
| File index | named by the descriptor | when an update is on offer |
| Package, one or more | named by the descriptor | in byte ranges, while updating |

Every asset is fetched from

```text
https://github.com/<owner>/<repo>/releases/latest/download/<asset name>
```

The GitHub REST API is not used: it allows an anonymous address 60 requests an
hour, which a handful of installed modules would exhaust between them. A
repository may serve several modules as long as every release carries the
assets of each.

A release is always complete and self-contained: it carries every file of the
module and refers to no other release. What makes an update small is the client,
not the publisher. The index says which bytes of which package hold each file;
the client compares it with the files it already has and asks for the rest with
HTTP `Range` requests. The distance between the installed version and the
released one does not matter - one version behind or twenty, the download is
what actually differs, and nobody has to publish a patch for it.

### 2.1 Descriptor

UTF-8 ini, at most 64 KiB.

```ini
[release]
schema        = 1
id            = madmer.agroprom_story
version       = 1.2.0
requires_game = 1.4.2
files         = 412
unpacked      = 181403648
index         = madmer.agroprom_story-1.2.0.files, 61538, 2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae

[packages]
madmer.agroprom_story-1.2.0.zip = 48211044, 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

| Key | Required | Meaning |
|---|---|---|
| `schema` | yes | Descriptor schema, currently `1`. |
| `id` | yes | Must equal the installed module's id. |
| `version` | yes | Version of the release, grammar of 1.2. |
| `requires_game` | no | Lowest Dead Air: Refined version (`MAJOR.MINOR.PATCH`) the release runs on. |
| `files` | no | Number of files in the release. When present it equals the index. |
| `unpacked` | no | Total size of those files in bytes. When present it equals the index. |
| `index` | yes | The file index: `<asset name>, <size in bytes>, <SHA-256 hex>`. |
| `[packages]` | yes | One line per package: `<asset name> = <size in bytes>, <SHA-256 hex>`. |

An asset name is 1-128 characters of `A-Z a-z 0-9 - . _` and does not start with
a dot; a package ends with `.zip`, the index with `.files`. At most 64 packages.
XFined Editor names them `<id>-<version>.zip` - `<id>-<version>.partN.zip` when it
splits a module to stay under GitHub's 2 GiB asset limit - and
`<id>-<version>.files`.

### 2.2 File index

UTF-8 text, at most 64 MiB, one record per line, fields separated by single
spaces:

```text
xms-files 1
pack madmer.agroprom_story-1.2.0.zip
file 9f86d0...0a08 1284 1 4096 517 8 spawn/l01_escape.xspawn
file 2c26b4...e7ae 394 1 66 214 8 mod.ltx
```

- The first line names the format and its version.
- `pack <asset name>` - a package of `[packages]`. Packages are numbered from 1 in
  the order of these lines.
- `file <sha256> <size> <pack> <offset> <packed> <method> <path>` - one file of
  the module: the SHA-256 of its content in lower-case hex, its size in bytes,
  the number of the package that holds it, the offset of the first byte of its
  stored data inside that package, the length of that data, how it is stored
  (`0` as is, `8` raw deflate - the ZIP method numbers), and its path: the rest of
  the line, relative to the module folder, with forward slashes.
- A line that starts with any other word is ignored.
- A path has no empty, `.` or `..` component, no `< > : " | ? * \`, no control
  character, no component ending in a dot or a space, and no reserved device
  name (`con`, `nul`, `com1`, ...). No path occurs twice.
- The index lists `mod.ltx`, whose `[module] id` and `version` equal the
  descriptor's.

### 2.3 Package

- A ZIP archive: stored or deflated entries, ZIP64 where needed, no encryption,
  no spanning. Every entry is `modules/<module id>/` followed by its index path,
  and a name outside ASCII carries the ZIP UTF-8 flag. The same archive is
  therefore the manual installation: extracted into the game folder it lands
  where the game reads modules from.
- The game never reads ZIP structures. It addresses a package through the index
  alone, so an archive must not be rewritten once its index exists - repacking
  moves the data and the offsets with it. Whatever the bytes at an offset turn
  out to be, a file is accepted only when it matches its size and SHA-256.

### 2.4 Evolution

- A client ignores keys, sections and index records it does not know. New
  capabilities arrive as new optional ones under `schema = 1` and `xms-files 1`.
- `schema` and the index version rise only for a release an older client must
  not install. Such a client reports that the game has to be updated first and
  installs nothing.
- The asset names, the download URL and every rule of 2.1-2.3 stay valid for as
  long as the game supports XMS modules. A release always carries its complete
  packages and index.

### 2.5 Publishing (informative)

Nothing here binds the game; it says what a publisher has to get right, and what
XFined Editor's **Publish Release** does about it.

- Only the descriptor's name is fixed. The tag, the title and the notes of the
  GitHub release are the author's: the game reads none of them. Which release is
  newer is decided by the descriptor's `version` against the installed
  `mod.ltx`, never by a tag or a date. The editor's **Releases** window edits
  the title and the notes of a release that is out and leaves the rest alone -
  including the Latest mark, which GitHub's default for an edit would move.
- The game sees a release the moment GitHub marks it Latest, so a release must
  be complete by then. The editor uploads the assets into a *draft*, compares
  what GitHub stored with what it packaged (name, size, SHA-256 digest), and only
  then publishes the draft as the latest release. At the end it fetches the
  descriptor from the URL of section 2 and reports whether it is served.
- A published release is never overwritten: the editor refuses to publish a tag
  that already exists. Other files under the same version go out the long way
  round - delete the release, publish the version again - and the game notices
  (3.1): to a client that has version `X`, a release `X` with other content is an
  update like any other, told to the player as a fix. A higher version remains
  the plain way to ship a change. One thing GitHub does on its own: a release
  deleted and published again under the SAME tag keeps its download address, and
  that address goes on answering with the deleted files for several minutes -
  the game sees the old descriptor until then. Right after a deletion the editor
  therefore publishes the version under a fresh tag (`v1.0.0-r2`), whose address
  is new.
- Deleting a release takes its git tag with it in the editor, release first: a
  published release whose tag goes first falls back to a draft. Once the latest
  release is gone GitHub marks the newest remaining one Latest, and that is what
  the game is served from then on; a client that already has a higher version
  than the one served stays where it is.
- The repository has to be public: the game downloads anonymously.
- One repository per module is the simple arrangement. The editor publishes one
  module per release, so it refuses when the current latest release carries the
  descriptor of another module - its own release would hide that module from
  its players.

## 3. Client behaviour

### 3.1 Check

Runs once per process when the main menu opens with no level loaded, and again
for every module whose check failed when the Mods window opens. Modules without
`[update] github` are skipped.
Per module the client fetches the descriptor and compares:

| Result | State |
|---|---|
| HTTP 404 | no release |
| descriptor invalid, id mismatch | check failed |
| `schema` above 1, or `requires_game` above the running game | blocked |
| `version` above the installed one | available |
| `version` equal to the installed one, content differs | available, as a fix |
| otherwise | current |

For an available update the client also fetches the index and tells the player
how much there is to download: the packed size of every file the installed
module does not hold under the same path and size. It is an estimate - the
update itself decides by content.

**The same version, published again.** An author may delete a release and
publish its version a second time with other files. Numbers cannot tell the two
apart, so for a release of the installed version the client compares content:
it fetches the index and checks that the module holds every listed file under
its path with the listed size and SHA-256. Files the module holds beyond the
index do not count. When everything matches the module is current; when
anything differs the release is offered like any update - same staging, same
delta - and the Mods menu words it as a fix by the author rather than as
"version X is available" over an installed X.

The comparison reads the whole module, so its verdict is kept in
`modules\.update_state`, one line per module: the SHA-256 of the index it was
made against and the size and time of the installed `mod.ltx`. While both still
hold the verdict is reused and neither the index nor the module is read; a
staged update drops it, and the first check after the swap makes a fresh one.
Deleting the file costs one comparison.

### 3.2 Update

`Update` queues one module, `Update all` every available one; the queue runs one
module at a time and builds the complete new module in
`modules\.staged\<id>\payload\`:

1. The descriptor is fetched again, and the index with it unless the one from the
   check still matches the descriptor's SHA-256.
2. Installed files are matched by content. Every installed file whose size the
   index names is hashed, and a file whose SHA-256 the index names is reused -
   under whatever path the new version wants it, so a renamed or moved file
   costs nothing. Reuse is a hard link, or a copy on a volume without them.
3. Free disk space is checked against what is left to fetch.
4. The rest is downloaded. Per package the needed ranges are sorted by offset,
   ranges less than 256 KiB apart are merged into one request, and every file is
   unpacked as it arrives and must match its size and SHA-256. A file the index
   names more than once is fetched once. A dropped connection resumes at the
   byte it stopped at, in the middle of a file if need be, and gives up after
   five attempts in a row that brought nothing. A server that answers a ranged
   request with `200` still works: the client skips what it did not ask for.
5. The staged `mod.ltx` must carry the descriptor's id and version.
6. `modules\.staged\<id>\ready.ltx` is written last. It names the installed
   module folder the payload replaces.

Files the new version no longer names are simply not carried over. Nothing above
touches the installed module; a failure removes the staging folder and leaves the
module as it was.

### 3.3 Apply

A staged update is applied at the next start, inside `XMS::InitializeAndMount`
and before modules are discovered - the only moment no module file is open:

1. `modules\<folder>` is renamed into the staging folder;
2. `payload` is renamed to `modules\<folder>`;
3. the staging folder is deleted.

If step 2 fails the old folder is renamed back. If step 1 fails - another
process holds a file - the update stays staged and is retried at the next start.
The module is replaced as a whole: files the new version dropped are gone. A
module folder holds no player data, so nothing is migrated. A module installed
in the legacy `mods\` root is updated in place.

When the queue finishes with at least one module staged, the Mods window asks
whether to restart now. `Yes` relaunches the game with its own command line; the
new process waits for the old one to exit (`DAR_RELAUNCH_WAIT_PID`) before it
applies anything. `No` leaves the update for the next start.

### 3.4 Console

| Command | Effect |
|---|---|
| `xms_update <id>` / `xms_update all` | Checks, then stages whatever is available. Needs no finished check to act on. |
| `xms_update_status` | One log line per module: installed version, state, available version, failure reason. |
| `xms_mods` | Opens the Mods window once the main menu has the input. |

### 3.5 QA override

With `-qa_update` on the command line, `DAR_QA_MOD_UPDATE_BASE` replaces
`https://github.com` when it parses as a loopback `http` URL. It redirects where
bytes come from and nothing else; every hash still gates every install.

## 4. Mods menu

The main menu entry `ui_mm_mods` is injected by the engine ahead of the bug
report and quit entries of `menu_main`, `menu_main_last_save` and
`menu_main_logout`, so a mod that replaces `ui_mm_main.xml` or
`ui_main_menu.script` cannot lose it. It is absent while a level is loaded: an
update must not be staged under a running game.

The window lists every discovered XMS module, enabled or not, and selects the
first one with an update until the player picks a row. `Update` is enabled for
an available or failed update, `Update all` while anything is available,
`Website` when the manifest carries a valid one. Keys: the UI move actions walk
the list, `ui_accept` is `Update`, `F5` is `Update all`, `quit` closes the
window; downloads go on without it, and the restart question brings it back.

Layout: `configs\ui\ui_mods.xml`, and `ui_mods_16.xml` for wide screens - the
same layout with every horizontal measure times 0.75, shifted clear of the menu
column. Strings: `configs\text\<language>\dead_air_x64_mods.xml`, Russian and
English.

## 5. QA

`tools\qa\mods\Run-ModsQa.ps1` builds an isolated root under `<game>\_qa\mods`,
generates installed modules and their releases, serves them from
`Start-ContentAssetMock.ps1` - throttled and dropping every connection mid-body -
and drives the engine through the console commands of 3.4 on a hidden desktop.
It covers the check verdicts, the refused website and repository, an index that
names a path outside the module, a package whose bytes do not match the index,
a delta that downloads the changed files only and reuses a renamed one, resume,
staging, the apply and its wait for the relaunching process, a module folder
held open by another process, an interrupted swap and an update for a deleted
module. Every verdict comes from the engine log, the mock's byte count and the
disk; window captures are kept as evidence.
