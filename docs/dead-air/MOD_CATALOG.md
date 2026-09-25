# The mod catalog: the in-game Mod Browser, its trust and its sandbox

This document is the contract between Dead Air: Refined, XFined Editor, the catalog
repository and every module listed in it. It builds on [MOD_UPDATES.md](MOD_UPDATES.md):
a listed module is an ordinary XMS module with ordinary GitHub releases; the catalog adds
who may publish it, what the player is shown before installing it, and what its scripts
may do once installed.

## 1. What the catalog guarantees

A player who installs a module through the Mod Browser gets these guarantees, in this
order of importance:

1. **Its scripts cannot harm the computer.** While any module from the catalog is enabled
   the game runs every script in *protected mode* (section 9): no native code, no
   programs, no network, no file outside the game's own data, no file the game or Windows
   would later load as code.
2. **It contains no program.** A catalog module holds game data only - no executable,
   library, OS script, archive or precompiled code (section 8). The game refuses to
   install or update one that does, whatever signed it.
3. **It is what its author released.** Every release is signed by the author's key
   (section 4); the catalog names that key for the module (section 5). A stolen GitHub
   account cannot publish an update the game accepts, and nobody on the way - GitHub, a
   mirror, a proxy - can change a byte.
4. **Only a module that passes the checks is listed.** The catalog's own workflows list
   what meets section 7 and nothing else (10.2). The index is signed with the catalog key,
   which the catalog's publishing workflow uses after the checks; the owner of the catalog
   keeps the key file as well (4.1). Write access to the catalog repository - its owner's
   GitHub account - can still change anything: the catalog is exactly as safe as that
   account.
5. **A bad module is switched off.** A version VirusTotal blocks, and a new release its
   author did not scan, is withdrawn automatically (5.2): the catalog reads the report of a
   new release within six hours and has every listed package analysed again every
   `vt_rescan_days` (section 11). A person with write access to the catalog may also revoke
   a module, a version or an author key by hand. The game disables what is withdrawn at the
   next start and says why.
6. **VirusTotal.** Every listed release was scanned by its author and has a public
   VirusTotal report the player can open; with the player's own free VirusTotal key the
   game checks the live report before it installs (section 11).

What protected mode does not cover: the game's own data. A module may still do inside the
game what modules have always done - change the rules, the world, the saves it writes. That
is what the players' reviews (section 12) and revocation by hand (5.3) are for.

Nothing in this document runs on a server of ours. The catalog is a handful of static
files in a public git repository, kept up to date by the repository's own workflows on
GitHub Actions and served by free static hosts; the trust is in the signatures, so any
copy on any host is as good as the original (section 13).

## 2. Parties

| Party | Holds | Trusted for |
|---|---|---|
| Catalog workflows | the catalog key and a VirusTotal key, as repository secrets | listing what passes section 7; withdrawing what VirusTotal blocks or does not know |
| Maintainer (optional) | the catalog repository, as its owner, and the catalog key file | revocations by hand; moderation of reviews |
| Author | an author key, made and kept by XFined Editor, and their own VirusTotal key | the content of their releases; scanning every release on VirusTotal before it goes out |
| Mirrors, hosts, proxies | nothing secret | nothing - everything they serve is verified |
| Player | optionally their own VirusTotal key | - |

## 3. Keys and signatures

- Algorithm: ECDSA over P-256 with SHA-256 (FIPS 186-4). The game and the editor use
  Windows CNG (`bcrypt.dll`, `ncrypt.dll`); the catalog tools use CNG on Windows and a
  plain big-integer verifier elsewhere. No third-party cryptography is shipped.
- A public key is written `p256:<X||Y>`: the 64 bytes of the uncompressed point without
  the `04` prefix, base64url without padding.
- A key id is the first 16 hex digits (lower case) of the SHA-256 of those 64 bytes.
- A signature is written `<key id>:<r||s>`: the two 32-byte halves, base64url without
  padding.

### 3.1 Signed text files

A signed file is an ini text whose **last** section is

```ini
[signature]
key = p256:...
sig = 0123456789abcdef:...
```

The signed message is every byte of the file before the `[signature]` header line - up to
and including the line break that ends the line before it. The header is exactly
`[signature]` at the start of a line and occurs once; after it come the `key` and `sig`
lines and blank lines only. A file with anything else after the header, or with no header,
is unsigned. `key` names the signer for tools and readers; the game never takes a key from
the file it verifies - it knows the key it expects (the maintainer's, built in; an
author's, from the catalog) and compares ids.

## 4. The author's release

A catalog module publishes releases exactly as MOD_UPDATES.md section 2 says. Two optional
descriptor sections are added there (2.1), both written by XFined Editor's Package Release:

```ini
[card]
name        = "Overnight Delivery"
name@eng    = "Overnight Delivery"
author      = "Madmer"
description = "Night shifts of a courier in the Zone.\nA new game mode."
website     = https://ap-pro.ru/stuff/zov_pripjati/overnight-delivery-r600/
mode        = new darf-overnight-delivery
mode_title  = "Overnight Delivery"
tags        = story, quests
thumbnail   = thumbnail.dds, 43832, 5e8f...c1
manifest    = mod.ltx, 812, 9a1b...77

[signature]
key = p256:...
sig = 0123456789abcdef:...
```

- `[card]` is what the Mod Browser shows: the module's own manifest with every string
  table id resolved (the browser shows modules that are not installed, whose string tables
  it does not have). Text follows MOD_UPDATES 1.1; `<key>@eng` is the English text, used
  when the game runs in English, the plain key otherwise.
- `mode` is `plain` (the ordinary game), `new <mode id>` (a mode the module brings) or
  `mode <mode id>` (a mode another module or the game has); `mode_title` is its title.
- `tags`: see 6.2.
- `thumbnail` and `manifest`: a file of the release - path as in the index, size, SHA-256.
  `manifest` is the release's `mod.ltx`; the game checks at install that the index lists
  the same file, so the card and the installed manifest are one text.
- `[signature]` signs the whole descriptor, the index hash and the package hashes with it:
  one signature covers every byte of the release.

### 4.1 Key files

A key - the author's and the catalog's alike - is a file the owner keeps, never a
secret tied to one computer. Losing a laptop loses nothing as long as the file is kept
somewhere else: a USB stick, a cloud folder, a password manager.

```text
xms-key 1
public  = p256:...
private = <base64url of the 32-byte scalar d>
```

or, protected by a password:

```text
xms-key 1
public  = p256:...
kdf     = pbkdf2-sha256, 600000, <base64url salt>
cipher  = hmac-sha256-ctr, <base64url nonce>
private = <base64url of the encrypted scalar>
mac     = <base64url of the tag>
```

PBKDF2-HMAC-SHA256 turns the password and the salt into 64 bytes: the first 32 key the
stream (block `i` is `HMAC-SHA256(key, nonce || i as 4 big-endian bytes)`, XORed over the
scalar), the last 32 key the tag (`HMAC-SHA256(key, nonce || encrypted scalar)`, checked
before anything is decrypted). Both Windows CNG and a language's standard library have
every piece of it.

XFined Editor creates the author's key file on first use - `Documents\XFined Editor\Keys\
author.xkey` unless the author picks another place, never inside a project - and says to
keep a copy. It remembers where the file is and asks for its password once per session.
Another computer imports the same file.

The catalog's rules never replace a lost author key. A submission that brings another key
for a listed module is refused automatically: nobody but the holder of the old key can
vouch for a new one, and the catalog has no person to ask. The module keeps its listing and
installed copies keep working, but no update is accepted; the author lists the module again
under a new module id. A maintainer, if there is one, can still bind a new key by hand with
a commit (5.3).

## 5. The catalog

### 5.1 Files

The catalog is five kinds of files under one base URL:

| File | Signed by | Content |
|---|---|---|
| `index.ltx` | the catalog key (3.1) | the listed modules, their keys, the revocations and withdrawals, the mirrors, the VirusTotal policy |
| `cards.txt` | nobody - every card is signed by its author | the latest release descriptor of every listed module |
| `thumbs/<sha256>.dds` | nobody - named by a signed card | the thumbnails those cards name |
| `vt.txt` | nobody - informational | VirusTotal results per package |
| `ratings.txt`, `reviews/<id>.txt` | nobody - every review is signed by its reviewer | ratings and reviews (section 12) |

### 5.2 `index.ltx`

UTF-8, at most 1 MiB:

```ini
[catalog]
format  = 1
serial  = 17
issued  = 2026-09-25
mirrors = https://mmadmer.github.io/darf-mods/, https://cdn.jsdelivr.net/gh/MMadmer/darf-mods@main/public/

[virustotal]
trusted       = Microsoft, Kaspersky, ESET-NOD32, BitDefender, ...
block_trusted = 1
warn_trusted  = 1
block_others  = 5
warn_others   = 2

[mods]
darf-overnight-delivery = MMadmer/DARF-Overnight-Delivery, p256:..., 1.0.0, 2026-09-25

[revoked]
evil-mod                  = *, Malware in scripts\x.script
some-mod                  = 1.2.0, Breaks saves - update to 1.2.1
bad-mod                   = 1.0.3, VirusTotal: Kaspersky, ESET-NOD32
key:0123456789abcdef      = *, Author key reported stolen

[signature]
key = p256:...
sig = ...
```

- `format`: 1. A client that finds another number reports that the game needs an update
  and shows nothing.
- `serial`: a whole number that grows with every publication. A client never accepts a
  serial below the highest it has accepted (rollback protection).
- `mirrors`: base URLs (`https://`, ending in `/`) the client also tries, after the ones
  built into the game. This is how the catalog moves host without a game release.
- `[virustotal]`: section 11.
- `[mods]`: `<module id> = <owner/repo>, <author key>, <lowest listed version>, <date>`.
  A release below the lowest listed version is not offered. `<date>` is the day the module
  was first listed: publishing takes it from the previous index, so an updated listing
  keeps its date.
- `[revoked]`: `<module id> = <version or *>, <reason>` or `key:<key id> = *, <reason>`.
  Publishing signs two files of the catalog repository into it together, and the game
  makes no difference between them: `revoked.ltx`, the revocations made by hand (5.3), and
  `holds.ltx`, the automatic withdrawals. The publishing workflow (10.2) rewrites
  `holds.ltx`, section `[holds]`, one line a version: `<module id> = <version>, <package
  sha256>+..., <reason>`; the index gets `<module id> = <version>, <reason>`. A version is
  held when VirusTotal's verdict on a package of it is `block` (reason `VirusTotal:
  <engines>`), and a module's current release is held while VirusTotal does not know a
  package of it - its author released it without scanning it (reason `Not scanned by
  VirusTotal yet`). A hold on a current release is lifted when every package of it has a
  report that is not `block` (the author's scan, or a false positive fixed); a held version
  that is no longer the latest stays held. The game disables a revoked module at the start
  in memory only, so a lifted hold enables it again.

The game carries the public catalog key(s) in code (`kMaintainerKeys` in
`src/xrCore/XMS/xms_catalog.cpp`); changing them is a game release. Several keys may be
built in; any one of them may sign.

### 5.3 Changes by the rules and by hand

The rules make these changes by themselves, with no person involved (10.2):

- listing a module that meets section 7;
- updating a listing with the same key: its repository, its lowest listed version, its
  website;
- withdrawing a version VirusTotal blocks or a new release VirusTotal does not know, and
  restoring the current release when a report of its packages clears it (5.2).

These need a person with write access to the catalog repository. The catalog works
without one, and then they do not happen:

- binding another key to a listed module (4.1);
- revoking and un-revoking by hand (`revoked.ltx`);
- adding a mirror;
- changing the VirusTotal policy or the limits of section 7 (`catalog.ltx`);
- moderating reviews (12.4).

### 5.4 `cards.txt`

```text
xms-cards 1
card darf-overnight-delivery 1234
<1234 bytes: the release descriptor, verbatim>
card another-module 987
<987 bytes>
```

At most 16 MiB. A card is used only when its descriptor parses (MOD_UPDATES 2.1), its `id`
is the card's, it carries a valid signature by the module's key from the index, its
version is at least the lowest listed one and neither the module, the version nor the key
is revoked. A card that fails is skipped. The file is rebuilt by the publishing workflow's
`refresh` job, which holds no key and cannot write to the repository (10.2): whoever builds
it can only leave a card out or serve an older one, and an installation always fetches the
live descriptor from the module's own repository anyway.

While the live release of a listed module fails the catalog's checks, the module keeps the
last card that passed. After `dormant_days` (section 7) of failing it loses its card: it
stays listed - its id stays bound to its key - and has a card again with the first release
that passes.

### 5.5 Thumbnails

`thumbs/<sha256>.dds`: the bytes a card names, fetched only for a verified card and
accepted only when size and SHA-256 match and the file is a thumbnail (6.1).

### 5.6 `vt.txt`

```text
xms-vt 1
<package sha256> <YYYY-MM-DD> <engines> <trusted malicious> <trusted suspicious> <other malicious> <engine,engine,...>
```

One line per package of every card, as VirusTotal's last analysis of it saw it, with the
day of that analysis; the last field names the engines that flagged it (`-` for none).
Unsigned: the Mod Browser shows it and warns by it, the live report is one click away
(section 11).

## 6. Manifest additions

MOD_UPDATES.md section 1 gains two optional keys.

### 6.1 `[module] thumbnail`

A path inside the module to a DDS picture: 256 x 256, DXT1 or DXT5, one mip level or the
full chain, at most 180 KiB, of exactly the size those dimensions give. Anything else is
not shown. The Mods window shows it next to an installed module, the Mod Browser next to a
listed one. XFined Editor makes it (`thumbnail.dds` at the module root) from any picture
the author picks.

### 6.2 `[module] tags`

A comma list from this vocabulary; other words are ignored:
`story`, `quests`, `gameplay`, `weapons`, `equipment`, `npcs`, `mutants`, `locations`,
`graphics`, `audio`, `interface`, `fixes`.

## 7. Listing requirements

A module is listed when all of these hold. XFined Editor checks them before it submits,
and the catalog's `accept` workflow judges the submission by them (10.2):

1. `[update] github` names a public repository whose Latest release carries the module's
   descriptor (MOD_UPDATES section 2).
2. The release carries `[card]` and a valid `[signature]`; the key is the one submitted.
3. The release version is at least `1.0.0` (MOD_UPDATES 1.2 ordering).
4. `website` is a page of the module on AP-PRO or ModDB, not the site or an index of it
   (MOD_UPDATES 1.3 plus): AP-PRO `/stuff/<section>/<slug>-r<number>/` or
   `/forums/topic/<number>-<slug>/`; ModDB `/mods/<slug>`, `/addons/<slug>` or
   `/games/<game>/addons/<slug>`, optionally followed by more path. The page answers
   HTTP 200. ModDB turns automated requests away: its refusal is a note in the verdict and
   does not stop the listing.
5. The release passes the content policy (section 8) and the script report has no
   blocking finding (10.3). Its other findings are notes in the verdict and stop no
   listing: protected mode stops every one of them.
6. The submission is opened by the owner of the module's repository, whose GitHub
   account is at least `min_account_days` old. A younger one is refused, with the day it
   may submit again: a submission that waited would be judged every half hour, and a pile
   of them from throwaway accounts would use up the workflow's API budget.
7. The module id is not listed yet, or listed with the same key. A listing with the same
   key may change its repository (the key proves it is the same author), its lowest
   version and its website. Another key is refused (4.1).
8. One owner lists at most `max_mods_per_owner` modules.
9. At most `max_new_per_day` new module ids are listed a day; the rest wait.
10. VirusTotal: the author scanned every package with their own VirusTotal key (XFined
    Editor does it on Submit), so VirusTotal has a report of it, and no package's verdict
    (section 11) is `block`. A `warn` is listed, and the game asks the player before it
    installs. The catalog only reads the report, once, and uploads nothing: a package
    VirusTotal does not know is refused, one VirusTotal is still analysing waits. When the
    catalog cannot ask VirusTotal at all, the submission waits `vt_wait_days` and is then
    judged without it.

A submission that cannot be judged yet - where a requirement says it waits, or while
VirusTotal, GitHub or the network does not answer - stays open and is judged again every
half hour and whenever its author submits again (10.2).

The numbers are the `[accept]` section of `catalog.ltx` in the catalog repository. They
are the repository's own and are not copied into the signed index:

```ini
[accept]
min_account_days   = 14
max_mods_per_owner = 20
max_new_per_day    = 10
vt_wait_days       = 3
vt_rescan_days     = 30
dormant_days       = 30
```

`vt_rescan_days` (0 = never) and `dormant_days` are for modules already listed (section 11,
5.4).

## 8. Content policy

Enforced by the game for every catalog module it installs or updates, by the catalog's CI
and by XFined Editor before a submission. The list is code on every side, never data a
module could change.

- Paths: MOD_UPDATES 2.2, and no component starting with a dot.
- A file has no extension or one of: `ltx ltxp xml xmlp script txt md json csv ini cfg seq
  nqasset behasset pdaasset xspawn xcform dds thm ogg ogm ogf omf skl skls anm ppe xr ps vs
  gs hs ds cs h hlsl s spawn graph ai gct cform details env_mod fog_vol game geom geomx hom
  ps_static snd_env snd_static som sectors wallmarks lanims`.
- Whatever its name, a file must not start like a program or a container: a PE image
  (`MZ` with a `PE\0\0` header where `e_lfanew` points), ELF, Mach-O, ZIP, RAR, 7-Zip,
  CAB, GZIP, XZ, OLE (MSI, legacy Office), a Windows shortcut, Java class, WebAssembly,
  Lua or LuaJIT bytecode.

X-Ray archives (`.db`, `.xdb`) are containers the policy cannot see into: not allowed.

## 9. Protected mode

### 9.1 When

At every start, before the first script runs, XMS decides once for the whole process.
Protection is a switch that **stays on once it is on**:

- it turns on by itself at the first start with an enabled catalog module - installed
  through the Mod Browser, or listed in the last verified catalog under its own
  repository, however it got onto the disk;
- the player can turn it on at any time in the Mods window;
- only the player turns it off, in the Mods window, after a warning, and only while no
  catalog module is enabled.

A game that never had a catalog module runs every script exactly as before. The switch
stays on after the last catalog module is removed on purpose: whatever a protected script
left behind in the game's data must not meet unprotected scripts because a module was
uninstalled. The switch is `modules\.catalog\state.ltx`, which no script can write (9.3),
and no console command changes it. `xms_protect` prints the state and why.

### 9.2 The Lua state

Every Lua state the game opens is treated alike: the scripts' and the renderer's (the
shader `.s` files are Lua too).

| Removed | |
|---|---|
| `ffi` | not opened at all |
| `package.loadlib`, the C and all-in-one searchers, `package.cpath` | native modules |
| `os.execute`, `io.popen`, `os.tmpname`, `os.getenv`, `io.tmpfile` | programs, the world outside |
| `string.dump` | bytecode |
| `jit.util` | engine internals |
| `debug.*` except `traceback`, `getinfo`, `sethook`, `gethook` | reaching around the guards |
| `lfs.link`, `lfs.chdir` | links, moving the relative root |
| `CUIMapList:StartDedicatedServer` | a process launch |

| Guarded | |
|---|---|
| `load`, `loadstring`, `loadfile`, `dofile`, `require` | text chunks only - never bytecode |
| `marshal.encode/decode/clone` | no functions: a function travels as bytecode |
| `io.open`, `io.lines`, `io.input`, `io.output`, `os.remove`, `os.rename`, `lfs.*` | the path rules of 9.3 |
| `getFS()`: `w_open`, `file_copy`, `file_rename`, `file_delete`, `dir_delete`, `append_path` | the path rules of 9.3 |
| `ini_file:save`, `ini_file:save_as` | the path rules of 9.3 |
| `get_console()`: `execute`, `execute_script`, `execute_deferred` | the console rules below |

A refused call logs `! [protect] <what> refused: <why>` with the script and line, and
fails the way the library fails (`nil, message` for io and os, a Lua error elsewhere).

#### The console

The console checks every command by **who gave it**, not by the binding it came through:
a command runs later than it was asked for (`execute_deferred`), from a key bound to it
(`bind_console`), line by line from a file (`cfg_load`, `execute_script`), or from the
options menu. The player's own commands are the ones typed into the console, the lines of
`user.ltx` (no script may write it, 9.3) and the command line; everything else is checked,
including a script's command inside one the player typed (`run_string`). A nested command
keeps the origin of the one that ran it.

- `xms_install`, `xms_remove`, `xms_remove_cancel`, `xms_enable`, `xms_disable`,
  `xms_update` and `xms_catalog` are the player's alone: which modules the game has is not
  a script's choice, and a review is posted by the player, never in their name.
  `xms_catalog after <command>` runs the command later as whoever asked.
- A command is one line. A value with a line break would become a line of its own in
  `user.ltx`, which runs as the player's at the next start.
- `cfg_save`, `save`, `save_silent`, `load`, `demo_record` and `demo_play` take a name,
  never a path (`\`, `/`, `:` or nothing but dots).
- `cfg_load` takes a name, or a path the read rule of 9.3 allows (never a network path:
  reading one hands the player's Windows credentials to its server): the stock scripts load
  files of commands from the game's configs by their path. Each line it runs is checked in
  turn. `xms_mods <command>` (the Mods window's buttons, 14) is the player's too.
- `start` connects to `client(localhost)` only: a single-player game connects to itself.

### 9.3 Paths

Every path is made absolute, then resolved through links to the real file; UNC and device
paths (`\\...`), alternate data streams (`name:stream`) and reserved device names are
refused outright.

- **Read**: inside the game folder or the player's data folder (`$app_data_root$`).
- **Write** (create, change, rename, delete, make or remove a folder): inside those two
  folders, except `modules\`, `mods\`, `gamedata\scripts\`, `fsgame.ltx`, `user.ltx` and
  any `desktop.ini` (Explorer reads it when the folder is opened, network icon paths
  included), and only files with no extension or one of
  `txt log ltx xml json csv ini cfg dat sav scop scoc scov tmp bak md`.

## 10. Submission and acceptance

### 10.1 The author, in XFined Editor

**Mod > Submit to Mod Browser...** (MCP `mod_catalog`) checks section 7 on the project and
its latest release, scans the packages on VirusTotal with the author's own key - uploading
one VirusTotal does not know and waiting for its report; without a key, or without a report
of every package, nothing is sent (section 11) - and opens an issue in the catalog
repository from the author's GitHub account - no
fork, no branch, no pull request; the catalog's owner submits the same way. An issue and
not a pull request: GitHub holds pull-request runs for an approval (a first-time
contributor's fork, a run it flags) and turns `pull_request_target` off by default in
public repositories, and the catalog has nobody to approve anything. An issue starts the
catalog's workflow with none of that.

The issue's form is a contract between the editor and the catalog. Its title is
`Submit <module id>`; its body starts with the line `<!-- xms-catalog submission -->`, then
one line of text, then a fenced `ini` block holding exactly the text of `mods/<id>.ltx`,
then the editor's report. The `ini` block:

```ini
[mod]
id      = darf-overnight-delivery
github  = MMadmer/DARF-Overnight-Delivery
key     = p256:...
version = 1.0.0
website = https://ap-pro.ru/stuff/zov_pripjati/overnight-delivery-r600/
```

`version` is the lowest listed version. The file carries no date: the index keeps the day
of the first listing (5.2).

One module and one account have at most one open submission: submitting again edits that
issue, which makes the catalog judge it again; with none open, a new one is opened. The
window follows the issue and shows the catalog's verdict (10.2).

### 10.2 The catalog repository

`mods/<id>.ltx` (the listings, one per module), `catalog.ltx` (mirrors, VirusTotal policy,
review inbox, `[accept]`), `revoked.ltx` (revocations by hand), `holds.ltx` (automatic
withdrawals), `moderation.ltx` (12.4), `public/` (what mirrors serve: `index.ltx`,
`cards.txt`, `vt.txt`, `thumbs/`, `ratings.txt`, `reviews/`), `tools/catalog.py`,
`.github/workflows/`:

- `accept.yml` - when an issue is opened, edited or reopened, every half hour, and by
  hand. Job `judge` (reads the repository, may comment on and close issues, holds the
  VirusTotal key) reads the submission through the GitHub API and applies section 7. Of
  VirusTotal it asks one thing a package: the report the author's scan left. The verdict
  is one of three:
  - **accepted** - job `merge`, the only one of `accept` with write access to the
    repository, reads the issue again, requires its `ini` block to be byte-identical to
    the one judged (SHA-256), commits `mods/<id>.ltx` with exactly those bytes, authored
    by the submitter's GitHub noreply address, together with the VirusTotal reports the
    judge read (into `public/vt.txt`, so nothing asks about those packages again), closes
    the issue as completed and starts `publish.yml`;
  - **refused** - the issue is closed with the reasons; the author fixes them and submits
    again;
  - **waiting** - the issue stays open and is judged again every half hour and whenever
    its author submits again: VirusTotal still analysing the package the author uploaded,
    VirusTotal, GitHub or the network not answering, the day's quota of new listings used
    up.

  A run judges at most 30 submissions, the oldest first; the rest wait for the next run,
  so a flood of issues costs time and never the workflow's API budget. An issue without
  the submission marker is left alone. A pull request by anyone but the
  repository's owner, members or collaborators is closed with a pointer to **Mod > Submit
  to Mod Browser...**; theirs are left alone, so maintenance by hand stays possible.
- `publish.yml` - the whole pipeline: every six hours, when `accept` starts it, by hand,
  and on a person's push to `mods/`, `catalog.ltx` or `revoked.ltx`. Three jobs, each on
  a machine of its own:
  - `refresh` (holds the VirusTotal key, reads the repository, may edit the status issue)
    does everything that reads what strangers publish: the cards (5.4), VirusTotal's
    reports (section 11 - read, never uploaded), `holds.ltx` (5.2), the reviews
    (12.3), and one status issue, "Listed modules failing the checks", created, kept up to
    date and closed when nothing fails. It works from `mods/` of the default branch - the
    files that passed the checks - not from the signed index, so a new module has its card
    in the same run that signs it in. Its files go on to `sign` as an artifact.
  - `sign` (holds the catalog key as the secret `CATALOG_KEY`, may write to the
    repository) takes those files, signs `public/index.ltx` from `mods/` of its own
    checkout, `revoked.ltx` and `holds.ltx`, and commits.
  - `deploy` publishes `public/` to GitHub Pages and refreshes the jsDelivr mirror.

  The catalog key never shares a machine with anything that parses a stranger's file,
  and the job that parses them cannot write to the repository: at worst it could withhold
  or add a withdrawal, never list a module.
- `pages.yml` - publishes `public/` to GitHub Pages after a person's own push to
  `public/`.

Only a person with write access changes a workflow: `merge` commits nothing but one
`mods/<id>.ltx`, and nothing a submitter sends is ever run - only read.

**The verdict** is a comment on the submission's issue, and XFined Editor shows it (10.1):
its form is a contract between the two as well. The body starts with the line
`<!-- xms-catalog verdict=accepted|waiting|refused -->` (one of the three), the next line
says the verdict in plain text, then come a blank line and the report in Markdown:

```text
<!-- xms-catalog verdict=waiting -->
Waiting: VirusTotal is still analysing mymod-1.0.0.zip. The catalog looks again every half hour.

<the report, in Markdown>
```

Only comments by `github-actions[bot]` that start with that line count; the newest of them
is the verdict. An unchanged verdict edits the bot's last comment and a changed one is a
new comment, so the author is notified once per change, not every half hour.

### 10.3 The script report

The catalog reads every `.script` of the release with a Lua tokenizer. **Blocking**:
`ffi`, `os.execute`, `io.popen`, `package.loadlib`, `string.dump`, bytecode,
`StartDedicatedServer`, `lfs.link`. **Reported, not blocking** (notes in the verdict,
10.2): `load`/`loadstring`/`require` of a computed string, `_G[...]`, `setfenv`,
`debug.*`, `getFS()` writes, `ini_file:save_as`, `get_console():execute`, long encoded
blobs, `string.char` chains. Protected mode stops every one of them anyway; the report
stops a module that could only work without it.

### 10.4 A person, optionally

Nothing requires a person: the workflows of 10.2 list, withdraw and publish by themselves.
The local tools stay for whoever maintains the catalog by hand:

- `catalog.py review` repeats every check and the VirusTotal verdict for a submission on
  their own machine;
- `catalog.py revoke` records a revocation in `revoked.ltx`;
- `catalog.py publish --key <file>` builds `public/index.ltx` with the next serial, signs
  it with the catalog key file (4.1) and refreshes the cards, as `publish.yml` does.

A push that changes `mods/`, `catalog.ltx` or `revoked.ltx` starts `publish.yml`, so a
change made by hand is signed and published like any other; a `public/` published by hand
goes out through `pages.yml`. The key file stays the master copy: a repository secret
cannot be read back.

## 11. VirusTotal

VirusTotal's free public API needs a personal key, allows 4 requests a minute and 500 a
day, and is for non-commercial use. Files uploaded to it are shared with the security
industry. It is one layer, not the protection: antivirus engines know Windows malware,
which section 8 already keeps out, and have no idea what an X-Ray script does, which
section 9 contains.

**Verdict.** From a report's per-engine results, with the index's `[virustotal]` policy:

- `block` - at least `block_trusted` engines of the `trusted` list call it malicious, or at
  least `block_others` other engines do;
- `warn` - at least `warn_trusted` trusted engines call it suspicious, or at least
  `warn_others` other engines call it malicious;
- `clean` - otherwise. One or two little-known engines flagging a file is noise, not a
  verdict.

**Who pays for what.** Scanning is the author's: whoever wants a module in the browser
scans every release of it with their own free VirusTotal key, which a solo author's quota
easily covers. The catalog's key does the pipeline's own checks only, and as few as it can:
it never uploads a file and never waits for an analysis, and it does not repeat the
author's scan - it reads the report that scan left, once.

**Where it runs.**

- The author, in XFined Editor, with their own key, and required: Submit to Mod Browser
  scans the packages before the submission is sent (10.1), and Publish Release scans every
  release of a listed module before anything goes out on GitHub - uploading a package
  VirusTotal does not know and waiting for its report. A package VirusTotal blocks is not
  sent at all; without a key nothing is sent. A submission leaves only once VirusTotal has
  a report of every package: VirusTotal answers "not found" for a file until its first
  analysis ends, and the catalog refuses that as a package nobody scanned. A release
  published while VirusTotal still analyses it is held back by the catalog until the
  report is out (5.2).
- The catalog, with its own key, a repository secret, one request at a time every 16
  seconds and at most 60 a run:
  - `accept` reads the report of each package of a submission once: `block` refuses it,
    `warn` is listed, a package VirusTotal does not know is refused (its author scans it
    and submits again), one still being analysed waits (section 7). `merge` keeps those
    reports in `vt.txt`, so nothing asks about them again.
  - `publish` reads the report of a package it has none of - a new release of a listed
    module, which its author scanned when they published it - once. It reads again only
    what needs it: a report that is not `clean` (once a day, until it is), a package
    VirusTotal did not know (once a day, withdrawn until its author scans it, 5.2), and a
    package the catalog asked VirusTotal to analyse again. That is the pipeline's own
    safety net: engines learn, a report does not renew itself, so every `vt_rescan_days`
    (30) each listed package is analysed again with that day's engines - one request - and
    read once more; a version the fresh analysis blocks is withdrawn (5.2). With 100
    listed modules that is about seven requests a day.
  - When the quota is used up, the key is refused or VirusTotal does not answer, the
    packages keep their last results and nothing else of the run is held up: the cards and
    the reviews still go out, and a submission waits (section 7).
- The player, in the Mod Browser: the verdict from `vt.txt` next to every module, and the
  report one click away (`https://www.virustotal.com/gui/file/<sha256>`). With their own
  key (Mods window, stored in the Windows Credential Manager, used for nothing else) the
  game asks VirusTotal for the live report before it installs. `block` refuses the
  installation; `warn` asks, with Cancel as the default; a package VirusTotal does not know
  installs on the other guarantees.

## 12. Ratings and reviews

A listed module has a rating and reviews, shown the same way for a module in the browser
and an installed one. Writing one needs no account anywhere: only the game.

### 12.1 The reviewer

- **Device identity**: the game creates one P-256 key per Windows user
  (CNG persisted key `DeadAirRefined Review Identity`) and signs every review with it. A
  second review from the same identity replaces the first: that is how a player edits.
- **Identity cost**: a new identity is accepted only with a proof of work over its public
  key - `SHA-256("xms-identity|" + key + "|" + nonce)` starting with `identity_bits` zero
  bits (the index says how many; about a minute of one core, done once, in the background).
- **Hardware**: `SHA-256("xms-hw|" + catalog salt + SMBIOS system UUID + Windows
  MachineGuid)`, sent with every review. Only equality matters; neither value can be
  recovered from it. One review per hardware per module counts - a reinstalled Windows or a
  second account on the same computer is the same reviewer.
- **Review cost**: every review carries a proof of work over its own text
  (`review_bits` zero bits, a few seconds - computed while the player writes).

None of this makes forgery impossible: a determined spammer can fake a hardware hash and
pay the proofs of work. It makes spam expensive and repeat voting from one computer
impossible for everyone else; what gets through anyway, the maintainer, if there is one,
removes (12.4).

### 12.2 The review

Written for an installed module only: 1-5 stars, picked by clicking, and a text of 30 to
2000 characters (after trimming) - both required. An optional name of up to 32 characters,
otherwise "Stalker #<first 6 of the key id>".

```text
xms-review 1
mod      = darf-overnight-delivery
version  = 1.0.0
stars    = 4
date     = 2026-09-25T12:34:56Z
name     = Strelok
text     = <base64url of the UTF-8 text>
hardware = <64 hex>
identity = p256:...
identity_pow = <nonce>
pow      = <nonce>
[signature]
key = p256:...
sig = ...
```

`pow` proves `SHA-256("xms-review|" + <every byte before the pow line> + nonce)` starts with
`review_bits` zero bits. The signature (3.1) is by `identity`.

### 12.3 Where reviews go

The game posts the review, as one text field, to the **review inbox** the index names - by
default a Google Form (anonymous, free, no server of ours); its answers land in a Google
Sheet published as CSV. `publish.yml` reads that CSV every six hours, keeps what passes
12.1-12.2, and writes `public/ratings.txt` and `public/reviews/<id>.txt`. The inbox is
configuration in the signed index, so it can move to another form service without a game
release:

```ini
[reviews]
inbox_url     = https://docs.google.com/forms/d/e/<form id>/formResponse
inbox_field   = entry.123456789
salt          = <random text>
identity_bits = 24
review_bits   = 20
```

Without `inbox_url` reviews are shown but cannot be written.

The review form says that reviews do not appear at once, and a posted review is
acknowledged with "it will appear after a while, usually within a few hours". It does not
say why: a reviewer who expects a delay writes for a reader, not for a counter.

### 12.4 What counts

The latest review of every identity, one per hardware, for a listed module, dated no
earlier than its listing; valid signature, proofs, stars and text; not by an identity or a
hardware on the catalog's `moderation.ltx` (the maintainer's list of removed reviews,
identities and hardware hashes, applied by `publish.yml`).

```text
xms-ratings 1
<module id> <count> <average x 100> <1-star> <2-star> <3-star> <4-star> <5-star>

xms-reviews 1
review <key id> <stars> <YYYY-MM-DD> <version> <name length> <text length>
<name bytes><text bytes>
```

Both files are unsigned, like `cards.txt`: whoever serves them can hide reviews, and the
raw inbox is public for anyone to recount. The game shows review text as plain text:
control characters and the UI's colour codes are removed.

## 13. Reliability

- Free static hosting only: GitHub Pages, jsDelivr and raw.githubusercontent.com serve the
  same `public/` folder. The game tries the built-in mirrors, then the signed `mirrors`.
  Moving the catalog is pushing the same files elsewhere and adding the URL.
- The game keeps the last accepted catalog, cards and thumbnails in `modules\.catalog\`
  and shows them when every mirror is down. Installing needs only the module's own GitHub
  release, as updating always did.
- If the maintainer stops, nothing stops: submissions are judged and listed; updates come
  from the authors' releases; VirusTotal results, the withdrawals they cause and the
  reviews keep being refreshed (10.2).
- GitHub switches off a scheduled workflow of a public repository after 60 days without
  activity and treats working around that as abuse; the catalog does not try. Submissions
  do not depend on the schedule: an issue starts its own judging, and a waiting submission
  is judged again when its author submits again. A catalog that lists modules commits
  genuine changes at least every `vt_rescan_days` (its packages analysed again), which is
  activity. If a
  schedule is switched off all the same, anyone with write access turns it back on in the
  repository's Actions tab; until then new modules are still listed and installed modules
  still update.
- What the catalog still depends on:
  - GitHub: the repository and its Actions, free for public repositories;
  - the catalog's VirusTotal key, for the pipeline's own few requests (section 11) - the
    scanning is done by the authors' own keys; without the catalog's key (none, or
    refused) submissions are judged on the other checks;
  - the review inbox, a Google Form of the maintainer's Google account: if that account is
    deleted (Google deletes accounts unused for two years), new reviews stop and the ones
    shown stay. The inbox moves with a signed index change (12.3).

## 14. The game's Mod Browser

The Mods window gains a **Browser** switch. In browser mode: a search field that filters
as it is typed - every word must match the start of a word of the name, author, tags or
description, a word of five letters or more tolerates one typo, case and `ё`/`е` do not
matter, the name weighs most - tag filters, sorting by relevance, rating, date or name, and per
module its thumbnail, name, author, version, target mode, rating, badges (installed,
update, VirusTotal verdict). The details show the full card, the rating and the reviews,
**Write a review**, the website (AP-PRO/ModDB), the VirusTotal report and **Install**. An
installed module that the catalog lists shows the same rating and reviews. The installed list gains **Enable/Disable** and
**Remove** (with a confirmation naming the module; applied at the next start like an
update) and shows thumbnails too.

An installation is an update from nothing: the live descriptor from the module's
repository is checked against the catalog (key, version floor, revocations), then the
content policy runs on the index, then MOD_UPDATES 3.2 fetches and verifies every file into
`modules\.staged\<id>\payload\`, and the next start moves it to `modules\<id>` and marks the
module as installed through the browser.

**Disclaimer.** Every installation and every update started from the Mods window - Install,
Update, Update all - first asks for confirmation with the disclaimer of the author of Dead
Air: Refined: modules are made by third parties, the author of the game is not responsible
for them, the safety measures described here are taken but give no absolute guarantee.
Cancel is the default. The browser view carries the same sentence at its foot. The console
(`xms_update`) asks nothing and writes the disclaimer to the log.

## 15. QA

- Game: `tools\qa\mods\Run-ModsQa.ps1 -Scenario Catalog` (fixture and mock:
  `tools\qa\mods\catalog_qa.py`) - a catalog signed with a fresh QA key the engine trusts
  only with `-qa_catalog`, cards signed by the author, by nobody and by a stranger, a
  thumbnail, VirusTotal answers (a lone no-name detection, a trusted engine's), installs
  refused for a DLL, a program under a config's name, a missing or foreign signature, a
  revocation and a version floor, one install taken; protected mode against a probe that
  first proves every escape of 9.2 and 9.3 real in an ordinary game, then meets a wall at
  each, the console routes of 9.2 included; an unsigned and a signed update; a rolled-back
  and a forged index; a review verified the way the catalog's workflow verifies it;
  removal, with protection staying on.
- Editor: `tools\qa\test_mod_catalog.py` - the author key, the signed descriptor, the
  thumbnail, the submission issue and the catalog's verdict against the GitHub mock,
  VirusTotal against its mock.
- Catalog: `python tools/catalog.py selftest`.
- Catalog workflows: `python tools/catalog/test_accept.py` against a mock of the GitHub
  API, the releases and VirusTotal - accepted, refused and waiting submissions, the merge
  committing the judged bytes only, a forged verdict comment, the catalog's key reading a
  report once and never uploading, holds from a VirusTotal `block` and from an update its
  author did not scan, the fresh analysis every `vt_rescan_days`, the status issue.
