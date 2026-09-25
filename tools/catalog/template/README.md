# Dead Air: Refined Mod Browser catalog

This repository is the catalog the game's Mod Browser reads (Mods window > Mod Browser). It
holds no mods: every module stays in its author's own GitHub repository and releases there.
The catalog lists which modules the browser offers, under which author key, and publishes
what the browser shows about them. The rules are the game repository's
`docs/dead-air/MOD_CATALOG.md`.

It runs by itself. Submissions are judged and listed by its workflows, updates come from the
authors' own releases, VirusTotal results and reviews are refreshed every six hours, and a
version VirusTotal blocks - or an update its author did not scan - is withdrawn
automatically. Nobody has to be here.

Scanning is the authors' job, with their own free VirusTotal keys: XFined Editor scans a
module when it is submitted and every release of a listed module before it goes out. The
catalog's own key (`VT_API_KEY`) never uploads anything; it reads the report an author's scan
left, once, and has every listed package analysed again every 30 days.

## What is here

| Path | What it is |
|---|---|
| `mods/<id>.ltx` | one file per listed module, committed by the catalog when its submission passed; a withdrawn module keeps its file, marked `withdrawn` |
| `catalog.ltx` | mirrors, the VirusTotal policy, the review inbox, the listing rules (`[accept]`) |
| `revoked.ltx` | modules, versions and author keys withdrawn by hand |
| `holds.ltx` | versions withdrawn automatically: VirusTotal blocks them, or does not know them |
| `moderation.ltx` | reviews, reviewer identities and hardware hashes that are not counted |
| `state/` | what the workflows remember between runs |
| `public/` | what the game downloads: `index.ltx` (signed), `cards.txt`, `thumbs/`, `vt.txt`, `ratings.txt`, `reviews/` |
| `tools/catalog.py` | the checks, the verdicts, publishing, VirusTotal, reviews |

The game trusts `public/index.ltx` only when it carries the catalog key's signature, and every
card in `cards.txt` only when it carries its author's. Whoever serves these files can hide
something, never change it.

## Listing a module

Authors do not edit this repository: XFined Editor's **Mod > Submit to Mod Browser** checks the
module and opens an issue here. The catalog answers in that issue within minutes: accepted
(the module is listed), refused (with the reasons; fix them and submit again) or waiting
(VirusTotal still analysing the package, the day's quota of new modules used up). The
requirements:

1. The module's `[update] github` repository is public and its latest release was made by
   XFined Editor's Package Release (it carries a signed `[card]`).
2. The release version is 1.0.0 or higher.
3. The website is the module's own page on AP-PRO or ModDB.
4. The release holds no programs, libraries, archives or compiled scripts, and its scripts
   pass the script report.
5. The author scanned the package with their own VirusTotal key, and VirusTotal does not
   block it.
6. The submission comes from the owner of the module's repository, whose GitHub account is at
   least 14 days old; the module id is new, or listed with the same author key.

## Withdrawing a module

XFined Editor takes a module out of the browser too: **Mod > Submit to Mod Browser** opens a
withdrawal issue here, and the catalog accepts it from the account that owns the module's
repository and from nobody else. The module leaves the browser within minutes and installed copies
keep working. Its file stays, marked `withdrawn`, so the module id stays bound to its author key:
submitting the module again with that key lists it again, and no other key can take the id.

## Workflows

- `accept.yml` - every submission and withdrawal issue, and every half hour: `judge` gives the
  verdict, `merge` commits an accepted module's file (or marks it withdrawn) and starts
  `publish.yml`.
- `publish.yml` - every six hours, after a listing, by hand: `refresh` (cards, VirusTotal,
  withdrawals, reviews, one status issue), `sign` (signs `public/index.ltx` with the secret
  `CATALOG_KEY`), `deploy` (GitHub Pages).
- `pages.yml` - after a push to `public/` by hand.

Repository secrets: `CATALOG_KEY` (the catalog key file's content; `CATALOG_KEY_PASSWORD` for a
protected one) and `VT_API_KEY` (a free VirusTotal key).

## By hand, optionally

Whoever has write access can still step in; nothing waits for it.

```text
python tools/catalog.py keygen catalog.xkey --password     a catalog key file; keep it safe
python tools/catalog.py review mods/<id>.ltx                every check again, on your machine
python tools/catalog.py revoke <id> <version|*> "<reason>"  then push: publish.yml signs it in
python tools/catalog.py publish --key catalog.xkey          signs public/index.ltx locally
```
