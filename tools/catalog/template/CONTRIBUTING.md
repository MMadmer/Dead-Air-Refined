# Submitting a module

Use XFined Editor: **Mod > Submit to Mod Browser**. It checks everything the catalog checks,
can scan the package on VirusTotal with your own key, and opens the pull request from your
GitHub account. A pull request changes exactly one file, `mods/<module id>.ltx`:

```ini
[mod]
id      = my-module
github  = my-account/my-module
key     = p256:...
version = 1.0.0
website = https://ap-pro.ru/stuff/<section>/<slug>-r<number>/
```

- `key` is the public half of your author key file. Keep the file itself (and a copy of it)
  safe: every release of the module has to be signed with it. If you lose it, make a new
  key and submit it from the account that owns the module's repository; the maintainer
  binds the new key.
- `version` is the lowest version the browser offers.
- Updates need no pull request: publish a new release with XFined Editor and the browser
  picks it up within six hours.

Automatic checks run on the pull request; the maintainer reviews and merges. A pull request
that changes anything else is closed.
