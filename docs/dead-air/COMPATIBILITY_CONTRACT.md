# Dead Air x64 compatibility contract

This document defines the compatibility boundary for the Dead Air x64 runtime.
Any intentional break must be documented, justified, and covered by a migration
path before a public build is produced.

## Supported addon model

The compatibility target covers the standard X-Ray addon surface:

- Lua source scripts;
- LTX, XML, localization, shader, sound, texture, mesh, animation, spawn, and
  level resources;
- packed `.db*` and `.xdb*` databases;
- loose `gamedata` overrides;
- JSGME-style packages that copy files into the game root;
- existing Dead Air and Dead Air Revolution II save files, subject to runtime
  validation.

An addon must keep using the same relative paths it uses on the x86 game. The
x64 package must not require addon authors to add a special manifest or place
their files in a new directory.

## Filesystem and load order

The shipped `fsgame.ltx` remains authoritative. The x64 engine must preserve:

1. the game root as `$fs_root$`;
2. databases under `database\`;
3. loose overrides under `gamedata\`;
4. the current archive ordering rules;
5. loose-file precedence over packed resources;
6. `appdata\logs`, `appdata\savedgames`, and the existing user configuration
   locations.

The x64 runtime must boot against an unmodified Dead Air installation before
any engine-specific resource override is considered.

## Lua compatibility

Lua addon compatibility is source-level compatibility with the Dead Air Lua
API, including the engine exports listed by the x86 runtime in
`lua_help.script`.

The port must:

- preserve existing global functions, classes, methods, properties, constants,
  callbacks, and argument conventions;
- retain Dead Air-specific exports that are absent from upstream OpenXRay;
- accept the existing `.script` source files and their current encodings;
- preserve the current script loading and override order;
- detect API differences automatically during development.

Precompiled x86 LuaJIT bytecode is outside the transparent compatibility
guarantee. Normal Dead Air addons ship Lua source, which will be compiled by the
x64 runtime.

## Native binary boundary

A 32-bit DLL, ASI plugin, proxy DLL, injector, or binary engine patch cannot be
loaded into a 64-bit process. This is an operating-system ABI restriction, not
an engine policy.

Such addons require an x64 rebuild or a dedicated compatibility implementation.
They must never be silently reported as compatible. Script/config/resource
addons remain the primary transparent compatibility target.

## Saves and network data

The port must preserve the serialized field order, sizes, identifiers, packet
layout, and version handling used by Dead Air wherever they affect:

- `.scop` and `.scoc` saves;
- game graph and ALife state;
- object packets;
- multiplayer or GameSpy-era packet structures still exercised by the game.

Save compatibility is a release gate and is not considered proven until the
x64 runtime loads, advances, saves, and reloads representative x86 saves.

## User-facing compatibility gates

A release candidate is acceptable only after it passes all of the following:

- boots from the existing game root with the existing `fsgame.ltx`;
- mounts the existing Dead Air databases;
- applies loose `gamedata` overrides in the same order;
- reaches the main menu using the existing configs and scripts;
- starts a new game;
- loads the latest x86 save and creates a reloadable x64 save;
- completes level transitions;
- loads representative script, UI, localization, texture, shader, sound, and
  gameplay addons without repackaging;
- retains JSGME install and uninstall behavior;
- survives an extended gameplay soak without x86 address-space exhaustion.

## Packaging rule

Development and testing use a separate runtime directory. Public packages may
replace engine binaries, but must not overwrite or delete a player's databases,
`gamedata`, saves, addon-manager state, or unrelated configuration files.
