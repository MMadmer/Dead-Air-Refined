# Dead Air: Refined save compatibility

## Compatibility contract

Dead Air: Refined keeps the original Dead Air 0.98b save formats intact. New
state is stored only in one optional companion container:

| File | Contract |
| --- | --- |
| `.scop` | Original ALife payload. Its serialized fields and ordering remain unchanged. |
| `.scoc` | Existing optional Lua/custom payload. Its format remains controlled by the original Lua API. |
| `.scov` | Refined's only persistent extension container for all current and future native mechanics. |
| `.scop.txn` | Temporary durable transaction marker. It contains no gameplay state and is removed after commit or recovery. |

An original 0.98b save without `.scov` loads with empty Refined extension
state. Original 0.98b ignores `.scov`, so a Refined save remains loadable there.
Loading and saving that `.scop`/`.scoc` pair in the original game does not
invalidate either original file; Refined treats a now-mismatched sidecar as
stale instead of changing the original payloads.

## Frozen container format

All scalar values are explicitly encoded in little-endian order. Container
version 3 is the permanent directory format. Future project versions must not
change the container header version, header layout, entry size, flag meanings,
alignment, or checksum rules. Extensibility is provided exclusively by chunks.

The first 48 bytes remain compatible with the earlier Refined v1 and v2
containers:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `u32` | magic `SOV1` |
| 4 | `u32` | container version |
| 8 | `u64` | transaction save ID |
| 16 | `u32` | uncompressed `.scop` source size |
| 20 | `u32` | compressed `.scop` payload size |
| 24 | `u64` | complete `.scop` file size |
| 32 | `u32` | complete `.scop` CRC-32 |
| 36 | `u64` | complete `.scoc` file size, or zero |
| 44 | `u32` | complete `.scoc` CRC-32, or zero |

Version 3 extends the header to 88 bytes:

| Offset | Type | Field |
| ---: | --- | --- |
| 48 | `u32` | header size, currently 88 |
| 52 | `u32` | bit 0: `.scoc` is present; all other bits are reserved |
| 56 | `u32` | chunk count |
| 60 | `u32` | directory entry size, permanently 32 |
| 64 | `u64` | directory offset, currently 88 |
| 72 | `u64` | complete container size |
| 80 | `u32` | directory CRC-32 |
| 84 | `u32` | header CRC-32 with this field cleared |

Each 32-byte directory entry contains:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `u32` | globally unique chunk type |
| 4 | `u16` | chunk schema version |
| 6 | `u16` | chunk flags |
| 8 | `u64` | payload offset |
| 16 | `u64` | payload size |
| 24 | `u32` | payload CRC-32 |
| 28 | `u32` | reserved, must be zero |

Payloads are aligned to eight bytes. The hard limits are 64 MiB per container,
16 MiB per chunk, 1,024 chunks, and 65,535 object records in each current
gameplay chunk. Directory entries must have unique types and non-overlapping
ranges.

Version 1 is accepted as the original 48-byte header-only container. Version 2
is accepted and migrated as the earlier fixed helmet-filter payload. Both are
written back as version 3 on the next successful Refined save.

## Chunk evolution rules

Chunk IDs are registered centrally in `save_extension_chunk_ids.h` and must
never be reused. A new mechanic gets a new chunk in `.scov`, never another save
file. A changed record contract gets a higher chunk version. It must not mutate
an older version in place.

An older Refined build:

- applies only the chunk versions and flags it understands;
- keeps unknown chunk types byte-for-byte;
- keeps known chunks with a newer version or nonzero flags byte-for-byte;
- never replaces a loaded chunk with a lower version;
- rejects duplicate mutations for the same chunk type before changing the
  background snapshot.

The container header flags and reserved fields therefore stay frozen. Future
metadata belongs in a new chunk, including metadata that affects several other
chunks.

## Registered gameplay chunks

Every record stream begins with a little-endian `u32` count. Object-bound
records use both the ALife object ID and a section-name CRC-32 so that an ID
reused for another section cannot receive stale state. Permanent ALife
unregistration clears every object-bound staged and runtime record before the
ID can be reused; ordinary online/offline transitions keep the record alive.

| ID | Version | Payload |
| --- | ---: | --- |
| `ADR1` | 1 | Actor adrenaline: `u16 objectId`, `u16 reserved`, `u32 sectionCRC`, `f32 remainingSeconds`. |
| `HFT1` | 1 | Helmet filter: `u16 objectId`, `u16 elapsed`, `u32 sectionCRC`. |
| `AFO1` | 1 | Artefact overrides: `u16 objectId`, `u16 reserved`, `u32 sectionCRC`, `u32 changedMask`, then 16 `f32` values. |
| `WEX1` | 1 | Weapon extensions: `u16 objectId`, `u16 reserved`, `u32 sectionCRC`, `u32 extendedConditionMask`. |
| `WFL1` | 1 | Weapon fault accumulators: `u16 objectId`, `u16 reserved`, `u32 sectionCRC`, `u16 fouling`, `u16 stress`, `u16 wear`, `u16 foulingStage`. |
| `SEA1` | 1 | One finite `f32` environment season value; no record count. |
| `0x584DFF01` | 1 | NQ quest-graph runtime state: opaque marshal blob owned by `xms_nq` (`xms.save_data("xms.nq", ...)`); no record count. Core pseudo-namespace `0xFF01` on the XMS per-module data channel `0x584D0000\|ns`; the range `0xFF00`–`0xFFFF` is reserved for engine-owned blobs and is never allocated to a module. |
| `0x584DFF02` | 1 | The world a new game was composed with (`XmsWorld`, MODDING.md "World rules"): `u8 version`, the rule sets of the modules, the run-time overrides, the area, the locks, the Player Start. Absent when no module declared a world. |
| `0x584DFF03` | 1 | Run-time vars of a module PDA layout (`XmsPda`, MODDING.md "PDA layouts"): `u8 version`, `u32 count`, `count` pairs of zero-terminated name and value (cp1251). Absent when no script set a var. |
| `AIK1` | 1 | NPC beliefs (`NPC_AI.md` "Knowledge"), 32-byte records: `u16 npcId`, `u16 targetId`, `u32 npcSectionCRC`, `f32 x, y, z`, `f32 err`, `u32 ageMs`, `u8 source`, `u8 confidence` (0..255), `u16 flags` (bit 0 enemy, bit 1 own sighting). Absent when no online stalker holds a belief. |
| `AIT1` | 1 | NPC radio traits (`NPC_AI.md` "Communication"): `u16 npcId`, `u32 npcSectionCRC`, `u8 flags` (bit 0 rolled, bit 1 carries a radio), sorted by id. |
| `XMK1` | 1 | The absent modules this save keeps content of (below, "What a module brings"): `u32 count`, then per module `stringZ id`, `stringZ version`, `u16 ns`, `u32 spawnBase`, `u32 spawnSize`. Absent when no module is kept. |
| `0x584F0000\|ns` | 1 | A module's ALife objects: `u32 count`, `stringZ module id`, `stringZ module version`, then `count` records `u16 objectId`, `u16 parentId` (`0xFFFF` none), `u8 flags` (bit 0: owned by declaration), `stringZ section`, `u64 classId` (the section's `class` the record was written by), `stringZ parentSection` (empty without a parent), `u32 size`, then `size` bytes: `u16 spawnSize`, the spawn packet, `u16 updateSize`, the update packet - exactly what the object registry writes into `.scop` for an object. Parents come before their children. Absent when the module has no object. |
| `0x58450000\|ns` | 1 | What the game's own objects carry of a module's data: `u32 count`, `stringZ module id`, `stringZ module version`, then records `u16 objectId`, `u32 sectionCRC`, `u8 kind` and the kind's fields: 1 loaded ammo type and 2 grenade type (`u8 value`, `u8 written`), 3 a shotgun's cartridges (`u32 writtenCRC`, `u8 n`, `n` × (`u8 place`, `u8 value`)), 4 an installed upgrade (`u32 writtenCRC`, `u8 place`, `stringZ upgrade`), 5 the actor's model (`stringZ worn`, `stringZ written`), 6 a quest task of the module in the actor's task registry (`u16 registryKey`, `u16 place`, `u32 size`, then the task as the registry writes it: id and `CGameTask`). `written` is the value `.scop` was given, `writtenCRC` the CRC-32 of the list as `.scop` holds it - the game's own entries, the module entries left out or set to the first type. A kind this build does not know ends the reading of the chunk. Absent when nothing names the module's data. |
| `0x584DFF04` | 1 | The radios' state (`xms_radio.script`, `xms.save_data("xms.radio", ...)`): an opaque blob on the per-module data channel under the core pseudo-namespace `0xFF04`, kept like the quest runtime's. |

The XMS module set manifest (`XMS1`) and the per-module data (`0x584D0000|ns`)
and spawn-ledger (`0x584C0000|ns`) chunk families are described in
`XMS_ARCHITECTURE.md`; a core blob such as `0x584DFF01` follows the module blob
rules (opaque payload, version 1, erased when the owner stores an empty blob).
The four families `0x584D`, `0x584C`, `0x584F` and `0x5845` are ranges by the
high word: no fixed chunk id is registered inside them (`save_extension_chunk_ids.h`
proves it at compile time).

## What a module brings

A module's content never enters the base game's save format. `.scop` always
describes a world the base game can load; everything of a module lives in `.scov`
and is laid over that world when Refined loads the save. A save can therefore go
to the base Dead Air and back, or through a time with the module switched off or
removed, and the module's content is no problem for the game that lacks it and
not lost for the game that has it. Without modules nothing of this exists: a game
with no module enabled, and a save that holds nothing of a module, are written
byte for byte as before - no placeholder, no anchor, no new chunk.

**Which objects are a module's.** An ALife object belongs to a module, in this
order of rules: its section was made by a module (the system config notes the
layer that creates each section; a section of the game a module only changed or
merged into stays the game's) - or the module changed the `class` of a section of
the game, so the object is written by a class only the module explains; a module's
spawn layer added it (its spawn id lies in the module's range); it stands in the
world on a level a module added; a module declared it its own at run time
(`xms.own_object`). Whatever is inside such an object, at any depth, is that
module's too, whatever its own section (a stock medkit in a module character's
pockets), and a squad and its members are one: the squad's own rule, else its
first member's that has one, decides for all of them - the base game's
`register_member` takes any id of its roster for a monster. The actor is never a
module's. A section a module brings by replacing a whole stock config file through
its `gamedata` mirror is read as the game's (the reader sees the stock path); a
module brings sections through `configs\xms\*.ltx`, `.ltxp`, items and characters.

**Where they are written.** Each such object is written into its module's chunk
`0x584F0000|ns`, the record the registry would have written into `.scop`, and the
object scheduler writes in its place, under the same id, a placeholder: a
`space_restrictor` of the base game with no shape, the "none" restrictor kind, no
spawn id, no client data, `flSwitchOnline`, `flVisibleForMap` and `flInteractive`
cleared and `flCanSave` set, standing at the object's game vertex when that is a
vertex of the base graph (else at the actor's), on that vertex's own level vertex,
named `xms_ph_<lineage>_<id>` and with `[xms_placeholder] module = <id>` as its
custom data. The capture keeps its 3 ms budget and its eight-object batches; a
placeholder costs one more packet per module object. A placeholder is no content:
it gives the id a holder, nothing else.

What the base game's reader does with a placeholder was checked in the readers this
tree keeps of it: `CALifeObjectRegistry::get_object` creates it from a section the
game has; `CSE_ALifeSpaceRestrictor::STATE_Read` reads zero shapes; the object is
registered on a vertex of its own graph; `try_switch_online` keeps it offline
because `flSwitchOnline` is clear (a space restrictor never switches offline by
itself, so this flag is what keeps it out of the level); `se_zones.se_restrictor`
(all three script layers the game ships) looks for a story id in its custom data
and for a teleport by its name, and finds neither; no stock script walks space
restrictors by section. The base game keeps it, saves it back and never gives its
id to anything else.

**The lineage.** The placeholders carry the transaction id of the save whose
`.scov` holds the module content (`SOV1` offset 8). When that save holds module
state but no placeholder - a quest blob, a ledger, what the game's objects carry of
a module's data - one more placeholder carries it, the anchor (`anchor = true`, under
an id no record of the save has). The base game carries both forward untouched under
any save name. **The one exception to the stale rule:** when Refined loads a `.scop`
whose own `.scov` is missing, stale, or holds no module chunk at all, and a record of
the `.scop` is a placeholder, it takes the module chunks - `XMS1`, `XMK1` and the
families `0x584D`, `0x584C`, `0x584F`, `0x5845` - from the `.scov` whose transaction id
is that lineage: the stale sidecar of the same name first, then the other `.scov` files
of the save folder, newest first, by their first 16 bytes alone (hundreds of saves cost
hundreds of 16-byte reads, and only on this path). The gameplay chunks bound to the
game's own objects (`WEX1`, `AFO1`, ...) stay under the stale rule. The search says
where it found the content, or, when it finds none, that the module content of the
save could not be found and where it looked; the placeholders are then removed and
the game goes on.

**The load.** After the `.scop` objects are read and before they are registered, every
placeholder of a present module is replaced by the object its record holds; a record
comes back only when its placeholder is in the world (the id is intact by construction),
its section is there with a `class`, that class is the one the record was written by
(never another class's reader), and its owner is there with the section recorded for it;
otherwise it is dropped with one line saying why (`its section is gone from the module`,
`its section's class is not the one it was written for`, `its owner is gone`,
`its owner is no longer what it was`, `its placeholder is not in the save any more`).
A dropped object drops what was inside it. A placeholder of a present module without a
record, and the anchor, are removed. What the game's objects carry of a module's data
comes back where the `.scop` field still holds what Refined wrote there (same id, same
section CRC, the `written` value or CRC unchanged); otherwise the record is dropped with
its line - so a weapon the base game reloaded keeps the ammo it was given there.

**What the game's own objects carry of a module's data** follows `WEX1`: the `.scop`
record keeps what the base game understands, the module's `0x5845|ns` the rest. A loaded
ammo type or a grenade type that indexes past the list the base game gives the weapon's
section (a module made `ammo_class` longer) is written as the first type, and so are such
cartridges in a shotgun's tube; an upgrade whose section a module made is left out of the
item's list. The list lengths before the modules are taken from the system config before
the module stage, and the module that made the list longer is the one whose stage did. The
actor wearing a module's mesh (a module outfit) is written with the model its own section
names. The quest runtime's tasks of a present module (`nq.<module>.<quest>.<task>` in the
actor's task registry, `REGISTRY_CHUNK`) are left out of the registry `.scop` gets and come
back once the registries are read; the active task id stays as it is (the base game finds no
such task and shows none active).

**A module that is absent** - switched off (`xms_disable`, `modules\disabled.ltx`) or
removed from the disk, the same case - is kept. The save loads without an error; its
objects are not in the world (one line per module with the count), every other module's
are; its placeholders stay in the world as the base game's would and hold its ids. Its
chunks - objects, state records, spawn ledger, per-module data - stay in `.scov` byte for
byte through every save made without it, `XMK1` names it, and its placeholders are written
with each new save's lineage. When it is there again each record comes back under the
rules above, and the kept ledger keeps the composer from placing its spawn objects twice.
`xms_forget <module id>` drops what the game in progress keeps of an absent module (its
placeholders are released, its chunks leave the save's state, and `xms_saves.forget(<id>)`
and every `xms.on_forget` handler run for the scripts' part); `xms_list` lists what is kept for which absent module. An older
save that still holds an absent module's spawn-layer objects in `.scop` has them taken
into the module's kept chunk at the load, a placeholder under each id, instead of
releasing them.

**Scripts.** `xms.object_owner(id)` names the module an ALife object belongs to by the
rules above, nil for the game's own - during a save's capture, during a load (a placeholder
answers its module from the moment it is read, an absent module's for as long as it stands;
a module's objects are back in the registry before any object registers, and until the squads
register their members a member's squad is what the squads' rosters say);
`xms.section_owner(section [, file])` names the module that made a section of the system
config or, with `file` (a path under `configs`), of a config read by name through a `#file`
patch - nil for the game's own and for a section a module only changed;
`xms.own_object(id, module)` declares an object a module's, and the declaration is saved
with the object; `xms.module_present(id)` is true while the module is enabled and loaded;
`xms.kept_modules()` lists the absent modules this game keeps content of; `xms.on_forget(fn)`
and `xms.forgotten()` tell what `xms_forget` dropped. What the stock scripts serialize
about a module's objects and the quest runtime's handling of an absent module's quests are
`SAVE_SCRIPTS.md`.

**Limits, said once in the log when hit.** A player who saved standing on a level a module
added has a save that loads only where that module is (`the player stands on a level a
module added`, at each such save). A module that modifies or removes the game's own objects in
place (a spawn layer's `modify` and `remove`, said per module when the spawn layers are composed;
a script changing a stock NPC's profile) changes records of `.scop`: those changes are the
module's state the base game sees. A module's own mode or world rules make a game of their own,
which the base game never starts (said at the game's first save). A squad of the game a module's NPC joined
leaves `.scop` with it, its own members too (a squad and its members are one; the base game
reads a roster it cannot narrow). The engine does not take out of `.scop` a stock record that
names a module's object in the engine's own fields beyond those - a restriction by a module
restrictor, a car of a module the actor sits in - nor the scope index and grenade mode in a
weapon's client data; what the stock scripts write is `SAVE_SCRIPTS.md`'s. One module's
objects in one save fit one chunk: 65,535 records and 16 MiB; over that the save stops and
the previous group stays.

What stands in for the base game in the proof (`tools\qa\saves\Run-ModuleSavesQa.ps1`,
`TEST_MATRIX.md`): the base 0.98b is not on this machine, so its reading of a `.scop` is
`scop_records.py` (the records as the base reader takes them) and Refined with `-no_xms`,
and its saving of a world is that world saved by Refined with `-no_xms` and the `.scop`
copied under another name without its `.scov`.

The AFO1 values are weight, health, radiation, satiety, power, bleeding,
additional carry weight, and the nine original hit-immunity classes. WEX1
stores the post-0.98b chamber-cycle and detached-magazine state bits. Its reader
also accepts fire-mode and addon-mount bits duplicated by earlier Refined WEX1
writers, then migrates them back to their original 0.98b field without erasing
an authoritative legacy value. The weapon field written to `.scop` always keeps
all original 0.98b bits and excludes only the two sidecar-owned bits. That exclusion covers
the update stream as well as the spawn record: `UPDATE_Write` masks the same two bits, and
the client keeps its own copy of them across an update packet, so neither reaches a `.scop`
where the original game would list them as the placeholder faults 26 and 27.

WFL1 stores the fault rework's accumulators: rounds since the last cleaning, shots fired with
a breakable deformation, the progress to the next deformation as a 16-bit fraction, and the
number of dirty faults produced in the current cleaning cycle. A record is written only when
one of them is non-zero. The original 0.98b ignores the chunk and plays on with the fault
mask alone; an older Refined build keeps it byte-for-byte and the counters simply freeze
until the save is next written by a build that knows them; a save without the chunk starts
every counter at zero, and a weapon that already carries dirty faults is treated as one
cleaning cycle in. Records are bound to the ALife id and the section CRC like WEX1, and a
weapon that goes offline keeps its counters in the same id-keyed store the chunk is loaded
into, so an id that is unregistered for good is forgotten with the object.

AIK1 holds what each online human stalker believes about other creatures: at most 16 records
per NPC, only beliefs younger than their forget time, the age measured at capture. On load the
records are staged before the ALife load and applied when the NPC spawns with the same id and
section CRC, as source `restored` (a remembered, reported-only kind that can steer movement and
search but never a shot) with the saved error grown by its age; records nobody claims within
`[ai_errors] restore_grace_s` (10 s) of play are dropped, and a target resolves by id at first
use. A record naming no source or one beyond `restored` (19), an unknown flag bit, a
non-finite or out-of-range position or error, or a duplicated NPC and target pair (every copy)
is ignored, with one log line for the chunk. A save without the chunk loads with empty beliefs: the stock memory
pool of `.scop` gives no stalker a belief.

AIT1 keeps each NPC's radio trait (rolled once per NPC lifetime) stable across loads and offline
periods, so a successor never inherits a dead leader's radio: the table holds every NPC rolled this session or restored, online or offline.
Capture drops a record whose ALife object is gone or whose section CRC changed; staging validates
size, flags and ids and drops every copy of a duplicated id; the table is cleared on a new game and
on load before the chunk is staged. A save without the chunk makes every NPC roll once at its next
online spawn and keep the result. The original 0.98b ignores both chunks, an older Refined build
keeps them byte-for-byte, and `.scop` / `.scoc` are unchanged: the memory managers keep their
layout and simply hold estimates for stalkers.

A bad container header, directory, binding, or payload CRC is never interpreted
speculatively. A bad payload CRC drops only that chunk. Within a structurally
valid current chunk, a semantically invalid or duplicate object record is
ignored without discarding other valid records. Non-finite floating-point
values are never applied. Unknown AFO1 mask bits are ignored by version 1.

## Capture and commit

Extension capture shares the existing 3 ms main-thread save budget. WEX1,
ADR1, HFT1, and AFO1 registry scans and record encoders are resumable and check
the budget every 16 objects or records. The legacy ALife registry container is
also resumed between its independently serialized registries without changing
their original byte order. Its object scheduler keeps issuing batches while
the shared frame budget remains, checks elapsed time after every object, and
limits each batch to eight ALife objects. SEA1 is a single fixed-size value. Compression,
container merge, CRCs, file writes, and commit run on the background writer.
The loaded chunk list is an immutable shared snapshot, so unknown payloads are
copied only on that writer.

A save is committed as one group:

1. Write and flush unique `.scop`, optional `.scoc`, and `.scov` temporary files.
2. Copy and flush every component of the previous complete group to `.bak`.
3. Write and flush `.scop.txn` with the transaction ID, expected companions,
   and previous-component presence bits.
4. Remove the old `.scop` commit marker.
5. Install `.scoc` and `.scov`.
6. Install `.scop` last.
7. Remove the transaction marker.

Failure rolls the complete previous group back with companions first and
`.scop` last. Startup and save-list enumeration wait for the writer, then use a
valid transaction marker to recover an interrupted overwrite or finish a first
save from its already-flushed unique temporary files. A missing main file is
never resurrected from an unmarked stale `.bak`; this preserves deletions made
by original 0.98b. If power is lost while the marker itself is being created,
an incomplete marker is discarded only when the still-present main save passes
the original validation; without a valid main save it remains a hard recovery
failure. The older destructive Lua `before_save` path is guarded by the same
durable marker. Its previous `.scoc` stays visible to the callback, the exact
post-callback result (including an empty file or deletion) is staged in the
unique transaction file, and the previous final is restored until commit.

The incremental and synchronous Lua capture APIs use one result contract:
`nil` means that `.scoc` is intentionally absent, any string including an empty
string is present data, and `false` or any other type aborts the save without
changing the previous group. A capture API selected at save start may not
silently fall back if a later step or encoder disappears.

A physically missing `.scov` means an original-compatible save. A physically
present but unreadable `.scoc`, or an unreadable or structurally invalid
`.scov`, aborts loading before the server broadcasts the load event or unloads
the active ALife state. This prevents a failed quickload from destroying the
current world and prevents damaged or temporarily unavailable extension data
from being silently cleared on the next save. A structurally valid sidecar
whose original-file signatures no longer match is stale rather than damaged;
it is ignored so an original 0.98b load-and-save round trip remains compatible.

## Prepared loading and file identity

The load path opens the selected `.scop` once, records its file identity, and
decompresses an immutable snapshot. Reusing a prepared snapshot requires the
same normalized path, size, storage ID, file ID, creation time, write time, and
change time. Filesystems without a strong file ID additionally require the
complete CRC-32 to match. A legacy `.sav` name is considered only when `.scop`
is physically missing; access, sharing, and identity failures never trigger a
fallback to another file.

On Windows, the final load lease normally prevents replacement of the selected
`.scop` and every existing `.scoc`, `.scoc.bak`, and `.scov` companion until the
native and Lua load callbacks finish. If another process already has a
delete-capable handle that makes the strict lease incompatible, loading retains
verified handle-backed compatibility snapshots instead of treating the valid
save as missing. The decompressed source and mapped companion bytes are shared
directly with the native and Lua readers for that lifetime, so `.scoc`,
`.scoc.bak`, and `.scov` cannot drift away from the verified group. Existing
public vector-based helpers remain available and copy the source before
releasing the same lease.

Engine-managed transaction targets and backups are made writable when an
existing save group carries the Windows read-only attribute. A level transition
whose conventional autosave still cannot be committed retries once with a
unique destination name. If both commits fail, the transition remains latched
instead of submitting the same failing save every frame.

A validated protected `.scop` footer is the authority for the transaction save
ID used by the versioned `DAX64SC2` Lua companion. An unprotected original
`.scop` has no transaction ID, so a valid and signature-matched `.scov` binding
supplies it instead. Without either source, the original zero-ID and raw
`.scoc` behavior is retained.

Deleting a save from the Refined UI removes the complete group, backups,
recovery files, durable marker, unique transaction files, preserved legacy Lua
capture, and thumbnail.
