#!/usr/bin/env python3
"""Retarget an X-Ray hands OMF from one rig to another with identical bone topology.

Per frame, per bone: the SKIN DEFORMATION is carried over, not the raw world orientation.
Skinning is v' = v_bind @ inv(BindWorld) @ AnimWorld; equal deformation on both rigs means
  AnimWorld_tgt = BindWorld_tgt @ inv(BindWorld_src) @ AnimWorld_src   (full affine 4x4)
and locals fall out as L = W @ inv(W_parent). This survives rigs whose bone AXES point in
opposite directions (the DA rig runs -X along the arm where the Gunslinger rig runs +X -
carrying raw world rotations there puts the flesh on backwards), and it needs no pinning:
positions ride the same delta. Assumes both rest poses hold the hands in the same stance.

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
ROOT = next(i for i, p in enumerate(parent) if p < 0)


def aff(R, T):
    m = np.eye(4)
    m[:3, :3] = R
    m[3, :3] = T
    return m


def aff_inv(m):
    R = m[:3, :3]
    T = m[3, :3]
    out = np.eye(4)
    out[:3, :3] = R.T
    out[3, :3] = -T @ R.T
    return out


def bind_world(rig):
    W = [None] * NB
    for i, b in enumerate(rig):
        L = aff(b["bind_R"], b["bind_T"])
        p = parent[i]
        W[i] = L if p < 0 else L @ W[p]
    return W


# Per-bone constant delta: BindWorld_tgt @ inv(BindWorld_src)
BW_s = bind_world(src_rig)
BW_t = bind_world(dst_rig)
DELTA = [BW_t[i] @ aff_inv(BW_s[i]) for i in range(NB)]
print("root:", names[ROOT], " (skin-deformation carry-over, no pins)")

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

    # source FK per frame (full affine), then the skin-deformation carry-over:
    #   W_t = DELTA[bone] @ W_s   with DELTA = BindW_tgt @ inv(BindW_src)
    src_W = [None] * NB
    for bi in range(NB):
        Q, T = bones[bi]
        Lf = np.stack([aff(quat_to_mat(Q[f]), T[f]) for f in range(ln)])
        p = parent[bi]
        src_W[bi] = Lf if p < 0 else np.einsum("fij,fjk->fik", Lf, src_W[p])

    # Rotations take the skin delta (flesh faces the right way on the differently-axed
    # target rig); POSITIONS stay the source's world positions verbatim. The device model
    # and its own animation live in source-space coordinates, and the two palms drift by
    # DIFFERENT constant vectors under a full skin carry-over (~0.19 m apart) - no static
    # device offset could ever meet both. Keeping source bone positions keeps the palms
    # and every fingertip exactly where the device expects them; the cost is the target
    # mesh stretching uniformly onto the larger source skeleton (no seams - skinning is
    # continuous), which reads as "big hands", not as breakage.
    dst_W = []
    for bi in range(NB):
        W = src_W[bi].copy()
        W[:, :3, :3] = np.einsum("ij,fjk->fik", DELTA[bi][:3, :3], src_W[bi][:, :3, :3])
        dst_W.append(W)

    out_bones = []
    for bi in range(NB):
        p = parent[bi]
        if p < 0:
            Lt = dst_W[bi]
        else:
            inv_p = np.stack([aff_inv(dst_W[p][f]) for f in range(ln)])
            Lt = np.einsum("fij,fjk->fik", dst_W[bi], inv_p)
        Rl = Lt[:, :3, :3]
        Tl = Lt[:, 3, :3].copy()
        q = np.stack([mat_to_quat(Rl[f]) for f in range(ln)])
        # quaternion continuity: slerp between adjacent keys must take the short arc
        for f in range(1, ln):
            if np.dot(q[f], q[f - 1]) < 0:
                q[f] = -q[f]
        # write a T track wherever the local translation actually moves; constant tracks
        # collapse to _initT in the encoder either way
        want_T = bool(np.abs(Tl - Tl[0]).max() > 1e-5)
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

# self-check: decode our own output and verify the SKIN INVARIANT on motion 0 -
# inv(BindW) @ AnimW must match between source and result (that is what deforms the mesh).
def fk_world(bone_data, ln_):
    W = [None] * NB
    for bi_ in range(NB):
        Q_, T_ = bone_data[bi_]
        Lf_ = np.stack([aff(quat_to_mat(Q_[f]), T_[f]) for f in range(ln_)])
        p_ = parent[bi_]
        W[bi_] = Lf_ if p_ < 0 else np.einsum("fij,fjk->fik", Lf_, W[p_])
    return W


mo2 = read_chunks(dict(read_chunks(bytes(out)))[14])
_, ln2, bones2 = decode_motion(mo2[1])
_, ln1, bones1 = decode_motion(mo[1])
W_out = fk_world(bones2, ln2)
W_src = fk_world(bones1, ln1)
err_r = 0.0
err_p = 0.0
for bi in range(NB):
    skin_src = np.einsum("ij,fjk->fik", aff_inv(BW_s[bi])[:3, :3][None][0], W_src[bi][:, :3, :3])
    skin_out = np.einsum("ij,fjk->fik", aff_inv(BW_t[bi])[:3, :3][None][0], W_out[bi][:, :3, :3])
    err_r = max(err_r, float(np.abs(skin_src - skin_out).max()))
    err_p = max(err_p, float(np.abs(W_out[bi][:, 3, :3] - W_src[bi][:, 3, :3]).max()))
print(f"self-check motion0: rot skin-invariant dev = {err_r:.5f}, world-pos dev vs source = {err_p:.5f}")
