#!/usr/bin/env python3
"""X-Ray OGF skeleton reader: bone names, parents, bind pose (S_BONE_NAMES + S_IKDATA).

All conventions copied from this repo's engine sources:
 - SkeletonCustom.cpp: chunk layouts;
 - _matrix.h/matrix.cpp: setXYZi(v) == setHPB(-v.y, -v.x, -v.z), row-basis matrices;
 - transform(v) = v.x*i + v.y*j + v.z*k + c  (row-vector), so compose W = L @ P (numpy).
"""
import math
import struct

import numpy as np


def read_chunks(buf):
    out = {}
    off = 0
    while off + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, off)
        off += 8
        out[cid & 0x7FFFFFFF] = buf[off:off + size]
        off += size
    return out


def cstr(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("cp1251"), end + 1


def set_hpb(h, p, b):
    """Row-basis rotation matrix, verbatim from Fmatrix::setHPB."""
    sh, ch = math.sin(h), math.cos(h)
    sp, cp = math.sin(p), math.cos(p)
    sb, cb = math.sin(b), math.cos(b)
    cc, cs, sc, ss = ch * cb, ch * sb, sh * cb, sh * sb
    m = np.zeros((3, 3), dtype=np.float64)
    m[0] = (cc - sp * ss, -cp * sb, sp * cs + sc)   # i
    m[1] = (sp * sc + cs, cp * cb, ss - sp * cc)    # j
    m[2] = (-cp * sh, sp, cp * ch)                  # k
    return m


def quat_to_mat(q):
    """Fmatrix::rotation(Fquaternion), rows i/j/k."""
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    m = np.empty((3, 3), dtype=np.float64)
    m[0] = (1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy))
    m[1] = (2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx))
    m[2] = (2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy))
    return m


def mat_to_quat(m):
    """Inverse of quat_to_mat (same element convention)."""
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        w = 0.25 * s
        x = (m[2, 1] - m[1, 2]) / s
        y = (m[0, 2] - m[2, 0]) / s
        z = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        w = (m[2, 1] - m[1, 2]) / s
        x = 0.25 * s
        y = (m[0, 1] + m[1, 0]) / s
        z = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        w = (m[0, 2] - m[2, 0]) / s
        x = (m[0, 1] + m[1, 0]) / s
        y = 0.25 * s
        z = (m[1, 2] + m[2, 1]) / s
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        w = (m[1, 0] - m[0, 1]) / s
        x = (m[0, 2] + m[2, 0]) / s
        y = (m[1, 2] + m[2, 1]) / s
        z = 0.25 * s
    return np.array((x, y, z, w), dtype=np.float64)


def parse_bones(path):
    data = open(path, "rb").read()
    ch = read_chunks(data)
    assert 13 in ch, f"no S_BONE_NAMES in {path}: {sorted(ch)}"
    bn = ch[13]
    off = 0
    (count,) = struct.unpack_from("<I", bn, off)
    off += 4
    bones = []
    for _ in range(count):
        name, off = cstr(bn, off)
        parent, off = cstr(bn, off)
        off += 60  # Fobb
        bones.append({"name": name.lower(), "parent": parent.lower()})
    assert off <= len(bn)

    assert 16 in ch, f"no S_IKDATA in {path}"
    ik = ch[16]
    off = 0
    for b in bones:
        (vers,) = struct.unpack_from("<I", ik, off)
        off += 4
        _, off = cstr(ik, off)  # game material
        off += 112  # SBoneShape: u16+u16 + Fobb(60) + Fsphere(16) + Fcylinder(32)
        off += 4 + 3 * 16 + 5 * 4  # joint: type + 3 limits + spring/damp/flags/bforce/btorque
        if vers > 0:
            off += 4  # friction
        rx, ry, rz, tx, ty, tz = struct.unpack_from("<6f", ik, off)
        off += 24
        off += 16  # mass + center of mass
        b["bind_R"] = set_hpb(-ry, -rx, -rz)  # setXYZi
        b["bind_T"] = np.array((tx, ty, tz), dtype=np.float64)
    assert off <= len(ik), (off, len(ik))

    idx = {b["name"]: i for i, b in enumerate(bones)}
    for b in bones:
        b["parent_idx"] = idx.get(b["parent"], -1)
    return bones


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        bl = parse_bones(p)
        print(f"{p}: {len(bl)} bones, root={[b['name'] for b in bl if b['parent_idx'] < 0]}")
        for b in bl[:6]:
            print("  ", b["name"], "<-", b["parent"] or "(root)", "T=", np.round(b["bind_T"], 4))
