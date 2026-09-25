# Dead Air: Refined Mod Browser catalog

This repository is the catalog the game's Mod Browser reads (Mods window > Mod Browser). It
holds no mods: every module stays in its author's own GitHub repository and releases there.
The catalog lists which modules the browser offers, under which author key, and publishes
what the browser shows about them. The rules are the game repository's
`docs/dead-air/MOD_CATALOG.md`.

It runs by itself. Submissions are judged and listed by its workflows, updates come from the
authors' own releases, VirusTotal results and reviews are refreshed every six hours, and a
version VirusTotal blocks is withdrawn automatically. Nobody has to be here.

## What is here

| Path | What it is |
|---|---|
| `mods/<id>.ltx` | one file per listed module, committed by the catalog when its submission passed |
| `catalog.ltx` | mirrors, the VirusTotal policy, the review inbox, the listing rules (`[accept]`) |
| `revoked.ltx` | modules, versions and author keys withdrawn by hand |
| `holds.ltx` | versions withdrawn automatically because VirusTotal blocks them |
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
(VirusTotal still scanning, the day's quota of new modules used up). The requirements:

1. The module's `[update] github` repository is public and its latest release was made by
   XFined Editor's Package Release (it carries a signed `[card]`).
2. The release version is 1.0.0 or higher.
3. The website is the module's own page on AP-PRO or ModDB.
4. The release holds no programs, libraries, archives or compiled scripts, and its scripts
   pass the script report.
5. VirusTotal does not block the package.
6. The submission comes from the owner of the module's repository, whose GitHub account is at
   least 14 days old; the module id is new, or listed with the same author key.

## Workflows

- `accept.yml` - every submission issue, and every half hour: `judge` gives the verdict,
  `merge` commits an accepted module's file and starts `publish.yml`.
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
