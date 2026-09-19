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
reads two kinds of assets from it:

| Asset | Name |
|---|---|
| Release descriptor | `<module id>.update.ltx` |
| Package, one or more | named by the descriptor |

Every asset is fetched from

```text
https://github.com/<owner>/<repo>/releases/latest/download/<asset name>
```

The GitHub REST API is not used: it allows an anonymous address 60 requests an
hour, which a handful of installed modules would exhaust between them. A
repository may serve several modules as long as every release carries the
descriptor and packages of each.

### 2.1 Descriptor

UTF-8 ini, at most 64 KiB.

```ini
[release]
schema        = 1
id            = madmer.agroprom_story
version       = 1.2.0
requires_game = 1.5.0
files         = 412
unpacked      = 181403648

[packages]
madmer.agroprom_story-1.2.0.zip = 48211044, 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

| Key | Required | Meaning |
|---|---|---|
| `schema` | yes | Descriptor schema, currently `1`. |
| `id` | yes | Must equal the installed module's id. |
| `version` | yes | Version of the release, grammar of 1.2. |
| `requires_game` | no | Lowest Dead Air: Refined version (`MAJOR.MINOR.PATCH`) the release runs on. |
| `files` | no | Number of files in all packages together. |
| `unpacked` | no | Total unpacked size in bytes. |
| `[packages]` | yes | One line per package: `<asset name> = <size in bytes>, <SHA-256 hex>`. |

An asset name is 1-128 characters of `A-Z a-z 0-9 - . _`, does not start with a
dot and ends with `.zip`. At most 64 packages. XFined Editor names a package
`<id>-<version>.zip`, and `<id>-<version>.partN.zip` when it splits one to stay
under GitHub's 2 GiB asset limit.

### 2.2 Package

- A ZIP archive: stored or deflated entries, ZIP64 allowed, no encryption, no
  spanning.
- Every entry lies under `modules/<module id>/`. The same archive is therefore
  the manual installation: extracted into the game folder it lands where the
  game reads modules from.
- Below that prefix a path is relative, has no `.` or `..` component, no
  `< > : " | ? *`, no control character, no component ending in a dot or a
  space, and no reserved device name (`con`, `nul`, `com1`, ...).
- A name outside ASCII requires the ZIP UTF-8 flag.
- No path occurs twice, within one package or across packages.
- The packages together hold `modules/<module id>/mod.ltx`, whose `[module] id`
  and `version` equal the descriptor's.

### 2.3 Evolution

- A client ignores keys and sections it does not know. New capabilities arrive
  as new optional keys or sections under `schema = 1`.
- `schema` rises only for a release an older client must not install. Such a
  client reports that the game has to be updated first and installs nothing.
- The descriptor name, the download URL and every rule of 2.1 and 2.2 stay valid
  for as long as the game supports XMS modules. A release always carries its
  complete packages; anything smaller - a patch, a delta - can only ever be an
  optional addition an older client never notices.

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
| otherwise | current |

### 3.2 Update

`Update` queues one module, `Update all` every available one; the queue runs one
module at a time.

1. Free disk space is checked against the package sizes plus `unpacked`.
2. Each package is downloaded into `modules\.staged\<id>\download\` and must
   match its declared size and SHA-256. A dropped connection resumes with a
   `Range` request, up to five attempts per package; a server that answers a
   ranged request with `200` restarts that package from its first byte.
3. The packages are unpacked into `modules\.staged\<id>\payload\` under the
   rules of 2.2; `files` and `unpacked`, when declared, are upper bounds.
4. The unpacked `mod.ltx` must carry the descriptor's id and version.
5. `modules\.staged\<id>\ready.ltx` is written last. It names the installed
   module folder the payload replaces.

Nothing above touches the installed module. A failure removes the staging
folder and leaves the module as it was.

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
It covers the check verdicts, the refused website and repository, a package
that leaves its folder, a package of another module, resume, staging, the apply
and its wait for the relaunching process, a module folder held open by another
process, an interrupted swap and an update for a deleted module. Every verdict
comes from the engine log and the disk; window captures are kept as evidence.
