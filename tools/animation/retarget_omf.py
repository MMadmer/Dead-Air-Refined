#!/usr/bin/env python3
"""Retarget an X-Ray hands OMF from one rig to another with identical bone topology.

Per frame, per bone (top-down):
  - target local rotation = source WORLD rotation expressed in the target parent's world
    frame (world orientations are carried over verbatim, so poses match);
  - target local translation = target bind translation (native bone lengths), except the
    ROOT (keeps the source root track - that is the raise/lower motion itself) and the
    PINNED bones (hands): their local T is solved so the WORLD position matches the source
    - palms stay exactly on the device the animation was authored against.

Track encoding mirrors the engine reader (SkeletonMotions.cpp / AnimationKeyCalculate.h):
  QR: s16 quat / 32767; T16: t = k * _sizeT + _initT. SMPARAMS is byte-identical (same
  names, same partition, same defs) - only OGF_S_MOTIONS data is rebuilt.
"""
import struct
import sys
import zlib

import numpy as np

from ogf_bones import parse_bones, quat_to_mat, mat_to_quat, read_chunks, cstr

SRC_OMF, SRC_RIG, DST_RIG, OUT_OMF = sys.argv[1:5]

KEY_QUANT = 32767.0
FL_R_ABSENT = 1 << 1  # flRKeyAbsent
FL_T_PRESENT = 1 << 0  # flTKeyPresent
FL_T16 = 1 << 2  # flTKey16IsBit

# engine flag values: verify against Motion.hpp if anything looks off
# (flRKeyAbsent = 0x02, flTKeyPresent = 0x04, flTKey16IsBit = 0x08 in this repo)

src_rig = parse_bones(SRC_RIG)
dst_rig = parse_bones(DST_RIG)
names = [b["name"] for b in src_rig]
assert names == [b["name"] for b in dst_rig], "rig topologies differ"
NB = len(names)
parent = [b["parent_idx"] for b in src_rig]
dst_bind_R = [b["bind_R"] for b in dst_rig]
dst_bind_T = [b["bind_T"] for b in dst_rig]

# pinned: world position carried over (palms on the device); root keeps its own track
PIN = {i for i, n in enumerate(names) if n.endswith("_hand") and "finger" not in n}
ROOT = next(i for i, p in enumerate(parent) if p < 0)
print("root:", names[ROOT], " pinned:", [names[i] for i in sorted(PIN)])

data = open(SRC_OMF, "rb").read()
top = []
off = 0
while off + 8 <= len(data):
    cid, size = struct.unpack_from("<II", data, off)
    off += 8
    top.append((cid, data[off:off + size]))
    off += size
chunks = dict(top)
mo = read_chunks(chunks[14])
(count,) = struct.unpack_from("<I", mo[0], 0)
print(f"{count} motions, {NB} bones")


def decode_motion(blob):
    """-> (name, length, per-bone dict(Q[frames,4], T[frames,3]))"""
    # the "name" here is authoring-SDK binary garbage (see the trim tool) - skip to NUL
    o = blob.index(b"\x00") + 1
    (ln,) = struct.unpack_from("<I", blob, o)
    o += 4
    bones = []
    for _ in range(NB):
        flags = blob[o]
        o += 1
        if flags & FL_R_ABSENT:
            q = np.frombuffer(blob, "<i2", 4, o).astype(np.float64) / KEY_QUANT
            Q = np.repeat(q[None, :], ln, 0)
            o += 8
        else:
            o += 4  # crc
            q = np.frombuffer(blob, "<i2", 4 * ln, o).astype(np.float64).reshape(ln, 4) / KEY_QUANT
            Q = q
            o += 8 * ln
        if flags & FL_T_PRESENT:
            o += 4  # crc
            if flags & FL_T16:
                k = np.frombuffer(blob, "<i2", 3 * ln, o).astype(np.float64).reshape(ln, 3)
                o += 6 * ln
            else:
                k = np.frombuffer(blob, "<i1", 3 * ln, o).astype(np.float64).reshape(ln, 3)
                o += 3 * ln
            sizeT = np.frombuffer(blob, "<f4", 3, o).astype(np.float64)
            o += 12
            initT = np.frombuffer(blob, "<f4", 3, o).astype(np.float64)
            o += 12
            T = k * sizeT + initT
        else:
            initT = np.frombuffer(blob, "<f4", 3, o).astype(np.float64)
            o += 12
            T = np.repeat(initT[None, :], ln, 0)
        bones.append((Q, T))
    return None, ln, bones


def encode_motion(name_bytes, ln, bones_out):
    """bones_out: per bone (Q[ln,4] float, T[ln,3] float, want_T: bool)"""
    out = bytearray()
    out += name_bytes + b"\x00"
    out += struct.pack("<I", ln)
    for Q, T, want_T in bones_out:
        qk = np.clip(np.rint(Q * KEY_QUANT), -32767, 32767).astype("<i2")
        r_const = bool((qk == qk[0]).all())
        t_const = bool(np.abs(T - T[0]).max() < 1e-6)
        flags = 0
        if r_const:
            flags |= FL_R_ABSENT
        use_T = want_T and not t_const
        if use_T:
            flags |= FL_T_PRESENT | FL_T16
        out += struct.pack("<B", flags)
        if r_const:
            out += qk[0].tobytes()
        else:
            rb = qk.tobytes()
            out += struct.pack("<I", zlib.crc32(rb) & 0xFFFFFFFF) + rb
        if use_T:
            mn = T.min(0)
            mx = T.max(0)
            init = (mx + mn) / 2
            size = np.maximum((mx - mn) / 2 / 32767.0, 1e-9)
            k = np.clip(np.rint((T - init) / size), -32767, 32767).astype("<i2")
            tb = k.tobytes()
            out += struct.pack("<I", zlib.crc32(tb) & 0xFFFFFFFF) + tb
            out += size.astype("<f4").tobytes() + init.astype("<f4").tobytes()
        else:
            out += T[0].astype("<f4").tobytes()
    return bytes(out)


def world_pos(R_parent_world, T_parent_world, T_local):
    # row-vector: pos = T_local @ R_parent(3x3 rows) + parent pos
    return T_local @ R_parent_world + T_parent_world


new_motions = [(0, struct.pack("<I", count))]
for mi in range(count):
    blob = mo[mi + 1]
    nm_end = blob.index(b"\x00")
    name_bytes = blob[:nm_end]
    _, ln, bones = decode_motion(blob)

    # source FK: world rotations and positions per frame
    src_Rw = [None] * NB
    src_Tw = [None] * NB
    for bi in range(NB):
        Q, T = bones[bi]
        Rl = np.stack([quat_to_mat(Q[f]) for f in range(ln)])
        p = parent[bi]
        if p < 0:
            src_Rw[bi] = Rl
            src_Tw[bi] = T.copy()
        else:
            src_Rw[bi] = np.einsum("fij,fjk->fik", Rl, src_Rw[p])
            src_Tw[bi] = np.einsum("fj,fjk->fk", T, src_Rw[p]) + src_Tw[p]

    # target locals, top-down
    dst_Rw = [None] * NB
    dst_Tw = [None] * NB
    out_bones = []
    for bi in range(NB):
        p = parent[bi]
        if p < 0:
            Rl = src_Rw[bi]
            Tl = bones[bi][1]
            dst_Rw[bi] = Rl
            dst_Tw[bi] = Tl.copy()
            want_T = True
        else:
            inv_parent = np.transpose(dst_Rw[p], (0, 2, 1))  # rotations: inverse = transpose
            Rl = np.einsum("fij,fjk->fik", src_Rw[bi], inv_parent)
            dst_Rw[bi] = np.einsum("fij,fjk->fik", Rl, dst_Rw[p])
            if bi in PIN:
                delta = src_Tw[bi] - dst_Tw[p]
                Tl = np.einsum("fj,fkj->fk", delta, dst_Rw[p])  # (world-parentpos) @ inv(R)
                want_T = True
            else:
                Tl = np.repeat(dst_bind_T[bi][None, :], ln, 0)
                want_T = False
            dst_Tw[bi] = np.einsum("fj,fjk->fk", Tl, dst_Rw[p]) + dst_Tw[p]
        q = np.stack([mat_to_quat(Rl[f]) for f in range(ln)])
        # quaternion continuity: slerp between adjacent keys must take the short arc
        for f in range(1, ln):
            if np.dot(q[f], q[f - 1]) < 0:
                q[f] = -q[f]
        out_bones.append((q, Tl, want_T))

    new_motions.append((mi + 1, encode_motion(name_bytes, ln, out_bones)))
    sys.stdout.write(".")
sys.stdout.flush()
print()

mo_out = bytearray()
for cid, blob in new_motions:
    mo_out += struct.pack("<II", cid, len(blob)) + blob

out = bytearray()
for cid, blob in top:
    payload = bytes(mo_out) if cid == 14 else blob
    out += struct.pack("<II", cid, len(payload)) + payload
open(OUT_OMF, "wb").write(out)
print(f"wrote {OUT_OMF}: {len(out)} bytes (src {len(data)})")

# quick self-check: decode our own output, verify world poses match the source (rotations)
mo2 = read_chunks(dict(read_chunks(bytes(out)))[14])
_, ln2, bones2 = decode_motion(mo2[1])
err = 0.0
Rw2 = [None] * NB
for bi in range(NB):
    Q, T = bones2[bi]
    Rl = np.stack([quat_to_mat(Q[f]) for f in range(ln2)])
    p = parent[bi]
    Rw2[bi] = Rl if p < 0 else np.einsum("fij,fjk->fik", Rl, Rw2[p])
# recompute source world for motion 0
_, ln1, bones1 = decode_motion(mo[1])
Rw1 = [None] * NB
for bi in range(NB):
    Q, T = bones1[bi]
    Rl = np.stack([quat_to_mat(Q[f]) for f in range(ln1)])
    p = parent[bi]
    Rw1[bi] = Rl if p < 0 else np.einsum("fij,fjk->fik", Rl, Rw1[p])
for bi in range(NB):
    err = max(err, float(np.abs(Rw1[bi] - Rw2[bi]).max()))
print(f"self-check: max world-rotation deviation motion0 = {err:.5f} (quantization noise expected < 0.01)")
