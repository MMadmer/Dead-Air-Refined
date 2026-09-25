# Dead Air: Refined Mod Browser catalog

This repository is the catalog the game's Mod Browser reads (Mods window > Mod Browser). It
holds no mods: every module stays in its author's own GitHub repository and releases there.
The catalog lists which modules the browser offers, under which author key, and publishes
what the browser shows about them. The rules are the game repository's
`docs/dead-air/MOD_CATALOG.md`.

## What is here

| Path | What it is |
|---|---|
| `mods/<id>.ltx` | one file per listed module, added by the author's pull request |
| `catalog.ltx` | mirrors, the VirusTotal policy, the review inbox |
| `revoked.ltx` | modules, versions and author keys withdrawn from the browser |
| `moderation.ltx` | reviews, reviewer identities and hardware hashes that are not counted |
| `public/` | what the game downloads: `index.ltx` (signed), `cards.txt`, `thumbs/`, `vt.txt`, `ratings.txt`, `reviews/` |
| `tools/catalog.py` | checks, publishing, VirusTotal, reviews |

The game trusts `public/index.ltx` only when it carries the maintainer's signature, and every
card in `cards.txt` only when it carries its author's. Whoever serves these files can hide
something, never change it.

## Listing a module

Authors do not edit this repository by hand: XFined Editor's **Mod > Submit to Mod Browser**
checks the module and opens the pull request. The requirements:

1. The module's `[update] github` repository is public and its latest release was made by
   XFined Editor's Package Release (it carries a signed `[card]`).
2. The release version is 1.0.0 or higher.
3. The website is the module's own page on AP-PRO or ModDB.
4. The release holds no programs, libraries, archives or compiled scripts, and its scripts
   pass the script report.
5. VirusTotal does not flag the package.
6. The pull request is opened by the owner of the module's repository.

## Maintaining the catalog

```text
python tools/catalog.py keygen maintainer.xkey --password   once; keep the file safe
python tools/catalog.py review mods/<id>.ltx                before merging a submission
python tools/catalog.py publish --key maintainer.xkey       after merging: signs public/index.ltx
python tools/catalog.py revoke <id> <version|*> "<reason>"  then publish
```

`publish` signs with the maintainer's key file on this computer. To publish from GitHub
instead, store the key file's content as the repository secret `CATALOG_KEY` (and its
password as `CATALOG_KEY_PASSWORD`): `publish.yml` then signs after every merge. The catalog
is then exactly as safe as this GitHub account.

Workflows: `check.yml` checks every pull request, `watch.yml` refreshes cards, VirusTotal
results (secret `VT_API_KEY`) and reviews every six hours, `pages.yml` publishes `public/` to
GitHub Pages.
