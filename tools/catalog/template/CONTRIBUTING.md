# Submitting a module

Use XFined Editor: **Mod > Submit to Mod Browser**. It checks everything the catalog checks,
scans the package on VirusTotal with your own key, and opens an issue here from your GitHub
account. The scan is required and it is yours: make a free account at virustotal.com and save
its API key in the editor's VirusTotal section. The catalog only reads the report your scan
leaves. The issue carries the one file the catalog will hold for your module,
`mods/<module id>.ltx`:

```ini
[mod]
id      = my-module
github  = my-account/my-module
key     = p256:...
version = 1.0.0
website = https://ap-pro.ru/stuff/<section>/<slug>-r<number>/
```

The catalog judges it by itself, usually within minutes, and says so in the issue:

- **Accepted** - the module is listed; the issue is closed.
- **Refused** - the reasons are listed; the issue is closed. Fix them and submit again. A
  GitHub account younger than 14 days is refused too, with the day it may submit again.
- **Waiting** - for VirusTotal to finish analysing your upload, for a service to answer, or for
  the next day when the day's new modules are listed already. The catalog looks again every
  half hour; submitting again makes it look at once.

Notes:

- `key` is the public half of your author key file. Keep the file itself (and a copy of it)
  safe: every release of the module has to be signed with it. A module is bound to its key for
  good: with the key lost, updates stop, and the module can only be listed again under a new id.
- `version` is the lowest version the browser offers.
- Updates need no submission: publish a new release with XFined Editor (Publish Release scans it
  with your key first) and the browser picks it up within six hours. A release VirusTotal does
  not know - published without the editor, or without the scan - is withdrawn until you scan it.
- Pull requests are not submissions: one is closed with a pointer here.
