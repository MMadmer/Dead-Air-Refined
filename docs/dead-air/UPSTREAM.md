# Source lineage and attribution

Dead Air: Refined is an independent derivative project maintained by MMadmer.
It is not presented as an engine implementation created from an empty codebase.

## Engine foundation

The engine foundation originates from the
[OpenXRay `xray-16`](https://github.com/OpenXRay/xray-16) project. Development
of the Dead Air port began from upstream commit:

```text
29030f81b137f6ea5365b3d71f2b588490832f5b
```

The corresponding upstream source and its complete authorship history remain
available at:

```text
https://github.com/OpenXRay/xray-16
```

The local Git remote named `upstream` points to this repository.

## Dead Air source reference

Lanforse provided the surviving unfinished Dead Air 1.0 source tree together
with its matching early CoC x64 engine baseline. Refined used the pair as a
comparative reference to isolate Dead Air-authored changes, with the released
0.98b binary/decompilation retained as the compatibility authority. The CoC
x64 tree was not imported as Refined's engine foundation.

## Refined history

The public Dead Air: Refined history records project-specific release states.
It does not duplicate thousands of upstream commits as if they were changes
made directly to Refined. This keeps the Refined contributor list scoped to
people who contribute to this project while retaining explicit and verifiable
source provenance.

## Attribution

Original copyright notices, dependency licenses, submodule histories, and
third-party acknowledgements remain part of the source distribution. The
project README also identifies OpenXRay as the engine foundation and credits
the Dead Air developers and community, including Lanforse for preserving and
sharing the surviving source reference.
