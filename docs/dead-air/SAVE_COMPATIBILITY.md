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
| `SEA1` | 1 | One finite `f32` environment season value; no record count. |
| `0x584DFF01` | 1 | NQ quest-graph runtime state: opaque marshal blob owned by `xms_nq` (`xms.save_data("xms.nq", ...)`); no record count. Core pseudo-namespace `0xFF01` on the XMS per-module data channel `0x584D0000\|ns`; the range `0xFF00`–`0xFFFF` is reserved for engine-owned blobs and is never allocated to a module. |

The XMS module set manifest (`XMS1`) and the per-module data (`0x584D0000|ns`)
and spawn-ledger (`0x584C0000|ns`) chunk families are described in
`XMS_ARCHITECTURE.md`; a core blob such as `0x584DFF01` follows the module blob
rules (opaque payload, version 1, erased when the owner stores an empty blob).

The AFO1 values are weight, health, radiation, satiety, power, bleeding,
additional carry weight, and the nine original hit-immunity classes. WEX1
stores the post-0.98b chamber-cycle and detached-magazine state bits. Its reader
also accepts fire-mode and addon-mount bits duplicated by earlier Refined WEX1
writers, then migrates them back to their original 0.98b field without erasing
an authoritative legacy value. The weapon field written to `.scop` always keeps
all original 0.98b bits and excludes only the two sidecar-owned bits.

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
