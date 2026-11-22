#!/usr/bin/env python3
"""Uniformly scale an X-Ray skeletal hud model (OGF) and its own motions (OMF) about the origin.

The 3D PDA device is held by hands that tools/animation/pda_hands_da.py fitted to the Dead Air rig
at ~0.65 of the Gunslinger scale; the device follows at 0.686 - the Weapons Evolution addon's
figure, which leaves the DA fingers resting on the case instead of sinking into it.

Everything that is a length is scaled, nothing else: vertex positions (normals and tangents keep
their directions), every bounding box and sphere, the bones' OBBs and collision shapes, their bind
translations and centres of mass; in the OMF the translation tracks (the quantized keys stay, only
their float range and origin scale, so the result is exact). Chunk ids, flags and every other byte
are kept as they are.

Usage: pda_device_scale.py --scale 0.686 --ogf <in> <out> [--omf <in> <out>]
"""
import argparse
import struct

# OGF chunk ids (engine: Layers/xrRender/FBasicVisual.h, SkeletonCustom.cpp)
OGF_HEADER, OGF_VERTICES, OGF_CHILDREN = 1, 3, 9
OGF_S_BONE_NAMES, OGF_S_MOTIONS, OGF_S_SMPARAMS, OGF_S_IKDATA = 13, 14, 15, 16
ID_MASK = 0x7FFFFFFF

# vertex type -> (stride, offset of the position)
VERTEX = {1: (60, 0), 2: (64, 4), 3: (70, 6), 4: (76, 8),
          0x12071980: (60, 0), 0x240e3300: (64, 4), 0x4a1c6600: (70, 6), 0x947c3301: (76, 8)}

FL_T_PRESENT = 1 << 0
FL_R_ABSENT = 1 << 1
FL_T16 = 1 << 2


def chunks(buf):
    out = []
    off = 0
    while off + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, off)
        out.append((cid, bytearray(buf[off + 8:off + 8 + size])))
        off += 8 + size
    assert off == len(buf), "trailing bytes"
    return out


def join(items):
    out = bytearray()
    for cid, blob in items:
        out += struct.pack("<II", cid, len(blob)) + blob
    return bytes(out)


def scale_floats(buf, off, count, k):
    vals = struct.unpack_from(f"<{count}f", buf, off)
    struct.pack_into(f"<{count}f", buf, off, *(v * k for v in vals))


def cstr_end(buf, off):
    return buf.index(b"\x00", off) + 1


def scale_header(blob, k):
    # u8 version, u8 type, u16 shader; bbox min/max; bsphere centre + radius
    scale_floats(blob, 4, 10, k)


def scale_obb(buf, off, k):
    # Fobb: rotation 3x3, translation, half size
    scale_floats(buf, off + 36, 6, k)


def scale_ogf(data, k):
    top = chunks(data)
    for cid, blob in top:
        c = cid & ID_MASK
        if c == OGF_HEADER:
            scale_header(blob, k)
        elif c == OGF_CHILDREN:
            kids = chunks(bytes(blob))
            for _, kid in kids:
                parts = chunks(bytes(kid))
                for pid, part in parts:
                    p = pid & ID_MASK
                    if p == OGF_HEADER:
                        scale_header(part, k)
                    elif p == OGF_VERTICES:
                        vt, n = struct.unpack_from("<II", part, 0)
                        stride, pos = VERTEX[vt]
                        assert 8 + n * stride == len(part), "unknown vertex layout"
                        for i in range(n):
                            scale_floats(part, 8 + i * stride + pos, 3, k)
                kid[:] = join(parts)
            blob[:] = join(kids)
        elif c == OGF_S_BONE_NAMES:
            (count,) = struct.unpack_from("<I", blob, 0)
            off = 4
            for _ in range(count):
                off = cstr_end(blob, off)
                off = cstr_end(blob, off)
                scale_obb(blob, off, k)
                off += 60
            assert off == len(blob)
        elif c == OGF_S_IKDATA:
            off = 0
            while off < len(blob):
                (vers,) = struct.unpack_from("<I", blob, off)
                off += 4
                off = cstr_end(blob, off)
                # SBoneShape: u16 type, u16 flags, Fobb, Fsphere, Fcylinder
                scale_obb(blob, off + 4, k)
                scale_floats(blob, off + 64, 4, k)                # sphere centre, radius
                scale_floats(blob, off + 80, 3, k)                # cylinder centre
                scale_floats(blob, off + 104, 2, k)               # cylinder height, radius
                off += 112
                off += 4 + 3 * 16 + 5 * 4 + (4 if vers > 0 else 0)  # joint data
                off += 12                                         # bind rotation
                scale_floats(blob, off, 3, k)                     # bind translation
                off += 12 + 4                                     # mass
                scale_floats(blob, off, 3, k)                     # centre of mass
                off += 12
            assert off == len(blob)
        elif c == OGF_S_MOTIONS:
            blob[:] = scale_motions(bytes(blob), k, bones_in(top))
    return join(top)


def bones_in(top):
    for cid, blob in top:
        if cid & ID_MASK == OGF_S_BONE_NAMES:
            return struct.unpack_from("<I", blob, 0)[0]
    return None


def scale_motions(mo, k, bone_count=None):
    items = chunks(mo)
    if bone_count is None:
        raise ValueError("bone count unknown")
    for cid, blob in items:
        if cid == 0:
            continue
        o = cstr_end(blob, 0)
        (ln,) = struct.unpack_from("<I", blob, o)
        o += 4
        for _ in range(bone_count):
            flags = blob[o]
            o += 1
            o += 8 if flags & FL_R_ABSENT else 4 + 8 * ln
            if flags & FL_T_PRESENT:
                o += 4 + (6 if flags & FL_T16 else 3) * ln
                scale_floats(blob, o, 6, k)                       # size, origin
                o += 24
            else:
                scale_floats(blob, o, 3, k)
                o += 12
        assert o == len(blob), "motion layout mismatch"
    return join(items)


def omf_bone_count(top):
    for cid, blob in top:
        if cid & ID_MASK == OGF_S_SMPARAMS:
            off = 2
            (parts,) = struct.unpack_from("<H", blob, off)
            off += 2
            total = 0
            for _ in range(parts):
                off = cstr_end(blob, off)
                (n,) = struct.unpack_from("<H", blob, off)
                off += 2
                for _ in range(n):
                    off = cstr_end(blob, off) + 4
                total += n
            return total
    raise ValueError("no SMPARAMS")


def scale_omf(data, k):
    top = chunks(data)
    n = omf_bone_count(top)
    for i, (cid, blob) in enumerate(top):
        if cid & ID_MASK == OGF_S_MOTIONS:
            blob[:] = scale_motions(bytes(blob), k, n)
    return join(top)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=float, required=True)
    ap.add_argument("--ogf", nargs=2, metavar=("IN", "OUT"))
    ap.add_argument("--omf", nargs=2, metavar=("IN", "OUT"))
    a = ap.parse_args()
    if a.ogf:
        src = open(a.ogf[0], "rb").read()
        out = scale_ogf(src, a.scale)
        assert len(out) == len(src)
        open(a.ogf[1], "wb").write(out)
        print(f"wrote {a.ogf[1]} ({len(out)} bytes) x{a.scale}")
    if a.omf:
        src = open(a.omf[0], "rb").read()
        out = scale_omf(src, a.scale)
        assert len(out) == len(src)
        open(a.omf[1], "wb").write(out)
        print(f"wrote {a.omf[1]} ({len(out)} bytes) x{a.scale}")


if __name__ == "__main__":
    main()
