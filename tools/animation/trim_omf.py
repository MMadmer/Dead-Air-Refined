#!/usr/bin/env python3
"""Trim an X-Ray OMF to a name-filtered motion subset.

Format (from src/xrCore/Animation/SkeletonMotions.cpp of this repo):
  top-level chunks: [u32 id][u32 size][bytes]
  OGF_S_MOTIONS (14): nested chunks: 0 -> u32 count; i+1 -> stringZ name + track data
  OGF_S_SMPARAMS (15): u16 vers; u16 part_count; partitions {stringZ, u16 n, {stringZ, u32}xN};
    u16 mot_count; defs: stringZ name, u32 flags, u16 bone_or_part, u16 motion(u16 index into
    MOTIONS data), f32 speed/power/accrue/falloff, [vers>=4: u32 mark_cnt, marks:
    {line-string(\r\n), u32 icnt, (f32,f32)xicnt}]
The engine VERIFYs def order == data order, so both lists are rebuilt in def order.
"""
import struct
import sys

SRC, DST, PREFIX = sys.argv[1], sys.argv[2], sys.argv[3].lower()

data = open(SRC, "rb").read()


def read_chunks(buf):
    out = []
    off = 0
    while off + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, off)
        off += 8
        out.append((cid, buf[off:off + size]))
        off += size
    assert off == len(buf), f"trailing bytes at {off}/{len(buf)}"
    return out


def cstr(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("cp1251"), end + 1


top = read_chunks(data)
chunks = dict(top)
assert 14 in chunks and 15 in chunks, f"chunk ids: {[c for c, _ in top]}"

# ---- SMPARAMS ------------------------------------------------------------------------
sp = chunks[15]
off = 0
vers, part_count = struct.unpack_from("<HH", sp, off)
off += 4
print(f"smparams vers={vers} partitions={part_count}")
part_start = off
for _ in range(part_count):
    _, off = cstr(sp, off)
    (bn,) = struct.unpack_from("<H", sp, off)
    off += 2
    for _ in range(bn):
        _, off = cstr(sp, off)
        off += 4
partitions_blob = sp[part_start:off]
(mot_count,) = struct.unpack_from("<H", sp, off)
off += 2
print(f"defs={mot_count}")

defs = []  # (name, blob_after_name, motion_index)
for _ in range(mot_count):
    name, off = cstr(sp, off)
    start = off
    (flags,) = struct.unpack_from("<I", sp, off)
    off += 4
    bone_or_part, motion = struct.unpack_from("<HH", sp, off)
    off += 4
    off += 16  # speed/power/accrue/falloff
    if vers >= 4:
        (mcnt,) = struct.unpack_from("<I", sp, off)
        off += 4
        for _ in range(mcnt):
            # line-string: terminated by \r\n (IReader::r_string)
            eol = sp.index(b"\n", off)
            off = eol + 1
            (icnt,) = struct.unpack_from("<I", sp, off)
            off += 4 + icnt * 8
    defs.append((name, sp[start:off], motion))
assert off == len(sp), f"smparams trailing {len(sp)-off}"

# ---- MOTIONS -------------------------------------------------------------------------
mo = read_chunks(chunks[14])
mo_map = dict(mo)
(mo_count,) = struct.unpack_from("<I", mo_map[0], 0)
assert mo_count == mot_count, (mo_count, mot_count)

keep = [(i, d) for i, (name, _, _) in enumerate(defs) if name.lower().startswith(PREFIX)
        for d in [defs[i]]]
print(f"keeping {len(keep)} of {len(defs)} motions")
for i, (name, _, _) in keep:
    pass

# Rebuild both lists in kept-def order. The engine maps name -> def index and reads data
# chunk (def_index + 1); the def's own `motion` field is NOT used for that lookup (real
# files carry unrelated values there), so it is left untouched.
new_defs = bytearray()
new_motions = [(0, struct.pack("<I", len(keep)))]
for new_idx, (old_i, (name, blob, old_motion)) in enumerate(keep):
    new_defs += name.encode("cp1251") + b"\x00" + blob
    new_motions.append((new_idx + 1, mo_map[old_i + 1]))

sp_out = struct.pack("<HH", vers, part_count) + partitions_blob + \
    struct.pack("<H", len(keep)) + bytes(new_defs)

mo_out = bytearray()
for cid, blob in new_motions:
    mo_out += struct.pack("<II", cid, len(blob)) + blob

out = bytearray()
for cid, blob in top:
    payload = bytes(mo_out) if cid == 14 else (sp_out if cid == 15 else blob)
    out += struct.pack("<II", cid, len(payload)) + payload

open(DST, "wb").write(out)
print(f"wrote {DST}: {len(out)} bytes (was {len(data)})")

# ---- verification pass ---------------------------------------------------------------
v = open(DST, "rb").read()
vt = dict(read_chunks(v))
vm = dict(read_chunks(vt[14]))
(vc,) = struct.unpack_from("<I", vm[0], 0)
assert vc == len(keep)
# Data-chunk "names" are binary garbage from the authoring SDK (the engine reads them only
# in a Debug sanity check); verify chunk identity by byte-equality with the source instead.
for new_idx, (old_i, _d) in enumerate(keep):
    assert vm[new_idx + 1] == mo_map[old_i + 1], f"data mismatch at {new_idx}"
names = [keep[i][1][0] for i in range(vc)]
# defs order check
vs = vt[15]
o = 4
for _ in range(part_count):
    _, o = cstr(vs, o)
    (bn,) = struct.unpack_from("<H", vs, o)
    o += 2
    for _ in range(bn):
        _, o = cstr(vs, o)
        o += 4
(vdc,) = struct.unpack_from("<H", vs, o)
o += 2
assert vdc == vc
for i in range(vdc):
    n, o = cstr(vs, o)
    (fl,) = struct.unpack_from("<I", vs, o)
    o += 4
    bp, mt = struct.unpack_from("<HH", vs, o)
    o += 4 + 16
    if vers >= 4:
        (mc,) = struct.unpack_from("<I", vs, o)
        o += 4
        for _ in range(mc):
            eol = vs.index(b"\n", o)
            o = eol + 1
            (ic,) = struct.unpack_from("<I", vs, o)
            o += 4 + ic * 8
    assert n.lower() == names[i].lower(), (n, names[i])
    assert n.lower().startswith(PREFIX), n
print("verify OK:", ", ".join(names[:6]), "...")
