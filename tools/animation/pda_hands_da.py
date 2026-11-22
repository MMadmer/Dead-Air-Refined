#!/usr/bin/env python3
"""The 3D PDA hand motions on the Dead Air hands rig - the rig every outfit's hands model shares.

The PDA clips were authored for the Gunslinger hands rig: arms ~1.8x larger and longer, bone axes
pointing the other way. The Weapons Evolution addon adapted the show, holster and walk clips to the
DA rig by hand (in a DCC tool); this script carries the rest of the set over the same way, so one
omf plays on any outfit's hands and no hands model is swapped in for the device.

The adaptation, measured on the addon's clips against ours:
 - the whole Gunslinger scene is uniformly SCALED by s (~0.65, fitted here): the device anchor
   (lead_gun) keeps its orientation, every hand and finger position is s times the original
   (within a millimetre, thumbs aside);
 - hands, fingers and the anchor keep the Gunslinger WORLD orientation up to a constant per-bone
   offset C_b (the axis conventions of the two rigs differ);
 - the arms keep the DA bone lengths; the shoulders move and the elbows are solved to reach the
   wrists;
 - the device model is scaled to match (tools/animation/pda_device_scale.py).

Everything here is calibrated on the addon's rest pose (its kpk_idle against our pda_idle) and then
moves by the Gunslinger clip's own DELTAS from rest, so at rest the result IS the addon's pose and
its clips join ours without a pop:
 - lead_gun: world delta, scaled;
 - hands: their pose in the anchor's frame (contact with the device survives any device turn);
 - fingers: local deltas in the parent's frame;
 - arms: shoulder = rest shoulder + scaled delta, clavicle turned by the original's delta, elbow by
   two-bone IK in the arm plane transported from rest, forearm roll shared with the twist bone.
The addon's own show / holster / walk clips are copied verbatim.

The Gunslinger-rig source is pda_hands_animation.omf as content-1.6.0 shipped it (this script's
output replaced it); the addon's omf is meshes\dynamics\devices\dev_bolt of Weapons Evolution 2026.

Usage:
  pda_hands_da.py --gs-omf <pda_hands_animation.omf on the Gunslinger rig>
                  --we-omf <Weapons Evolution dev_hand_bolt_hud_animation.omf>
                  --out <omf>
  pda_hands_da.py --dump-rig <hands.ogf> <rig.json>
Rigs: data/pda_gs_rig.json (the Gunslinger PDA rig, wpn_hand_pda3d.ogf of content-1.6.0),
      data/da_hands_rig.json (DA wpn_hand_no_outfit.ogf; every outfit's hands share it).
"""
import argparse
import json
import math
import os
import struct
import sys
import zlib

import numpy as np

from ogf_bones import parse_bones, quat_to_mat, mat_to_quat

HERE = os.path.dirname(os.path.abspath(__file__))
GS_RIG_JSON = os.path.join(HERE, "data", "pda_gs_rig.json")
DA_RIG_JSON = os.path.join(HERE, "data", "da_hands_rig.json")

KEY_QUANT = 32767.0
FL_T_PRESENT = 1 << 0
FL_R_ABSENT = 1 << 1
FL_T16 = 1 << 2

# The clips the PDA hud section plays (dead_air_x64_pda3d_items.ltx). Ours carry our definitions
# (speed, blend, flags); the motion data of the ones listed in FROM_WE is the addon's.
CLIPS = [
    "pda_draw", "pda_hide", "pda_aim_hide", "pda_idle", "pda_vibros_end",
    "pda_walk", "pda_walk_slow",
    "pda_aim_start", "pda_aim_idle", "pda_aim_end",
    "pda_aim_idle_up", "pda_aim_idle_right_up", "pda_aim_idle_right", "pda_aim_idle_right_down",
    "pda_aim_idle_down", "pda_aim_idle_left_down", "pda_aim_idle_left", "pda_aim_idle_left_up",
    "pda_aim_click", "pda_headflash", "pda_aim_headflash",
]
FROM_WE = {"pda_draw": "kpk_draw", "pda_hide": "kpk_hide", "pda_walk": "kpk_walk", "pda_walk_slow": "kpk_walk"}
REST_GS, REST_WE = "pda_idle", "kpk_idle"

# Share of the wrist roll the forearm bone takes (the rest goes on the twist bone and the wrist),
# and how much of it the twist bone, which sits a hand's width short of the wrist, follows.
FOREARM_ROLL = 0.5
TWIST_ROLL = 0.85
# The arm never straightens fully: the shoulder comes forward instead.
REACH = 0.985


# ---- file formats ------------------------------------------------------------------------------

def read_chunks(buf):
    out = []
    off = 0
    while off + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, off)
        off += 8
        out.append((cid, buf[off:off + size]))
        off += size
    return out


def cstr(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("cp1251"), end + 1


def load_rig(path):
    if path.lower().endswith(".json"):
        bones = json.load(open(path, encoding="utf-8"))
        for b in bones:
            b["bind_R"] = np.array(b["R"], dtype=np.float64).reshape(3, 3)
            b["bind_T"] = np.array(b["T"], dtype=np.float64)
        idx = {b["name"]: i for i, b in enumerate(bones)}
        for b in bones:
            b["parent_idx"] = idx.get(b["parent"], -1)
        return bones
    return parse_bones(path)


def dump_rig(ogf, out):
    bones = parse_bones(ogf)
    data = [{"name": b["name"], "parent": b["parent"], "R": b["bind_R"].ravel().round(9).tolist(),
             "T": b["bind_T"].round(9).tolist()} for b in bones]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    json.dump(data, open(out, "w", encoding="utf-8"), indent=1)
    print(f"wrote {out}: {len(data)} bones")


def read_omf(path):
    """-> (vers, [(part name, [(bone, idx)])], [def dict], {data index: blob})"""
    top = dict(read_chunks(open(path, "rb").read()))
    sp = top[15]
    off = 0
    vers, part_count = struct.unpack_from("<HH", sp, off)
    off += 4
    parts = []
    for _ in range(part_count):
        pname, off = cstr(sp, off)
        (bn,) = struct.unpack_from("<H", sp, off)
        off += 2
        bones = []
        for _ in range(bn):
            b, off = cstr(sp, off)
            (bid,) = struct.unpack_from("<I", sp, off)
            off += 4
            bones.append((b.lower(), bid))
        parts.append((pname, bones))
    (mot_count,) = struct.unpack_from("<H", sp, off)
    off += 2
    defs = []
    for _ in range(mot_count):
        name, off = cstr(sp, off)
        start = off
        off += 4 + 4 + 16
        if vers >= 4:
            (mcnt,) = struct.unpack_from("<I", sp, off)
            off += 4
            for _ in range(mcnt):
                off = sp.index(b"\n", off) + 1
                (icnt,) = struct.unpack_from("<I", sp, off)
                off += 4 + icnt * 8
        defs.append({"name": name, "blob": sp[start:off]})
    mo = dict(read_chunks(top[14]))
    return vers, parts, defs, mo


def decode_motion(blob, idx2name):
    """-> (frames, {bone: (Q[f,4], T[f,3])}) in data order."""
    o = blob.index(b"\x00") + 1
    (ln,) = struct.unpack_from("<I", blob, o)
    o += 4
    out = {}
    for i in range(len(idx2name)):
        flags = blob[o]
        o += 1
        if flags & FL_R_ABSENT:
            q = np.frombuffer(blob, "<i2", 4, o).astype(np.float64) / KEY_QUANT
            Q = np.repeat(q[None, :], ln, 0)
            o += 8
        else:
            o += 4
            Q = np.frombuffer(blob, "<i2", 4 * ln, o).astype(np.float64).reshape(ln, 4) / KEY_QUANT
            o += 8 * ln
        if flags & FL_T_PRESENT:
            o += 4
            if flags & FL_T16:
                k = np.frombuffer(blob, "<i2", 3 * ln, o).astype(np.float64).reshape(ln, 3)
                o += 6 * ln
            else:
                k = np.frombuffer(blob, "<i1", 3 * ln, o).astype(np.float64).reshape(ln, 3)
                o += 3 * ln
            size = np.frombuffer(blob, "<f4", 3, o).astype(np.float64)
            o += 12
            init = np.frombuffer(blob, "<f4", 3, o).astype(np.float64)
            o += 12
            T = k * size + init
        else:
            init = np.frombuffer(blob, "<f4", 3, o).astype(np.float64)
            o += 12
            T = np.repeat(init[None, :], ln, 0)
        # The 16-bit keys are a hair off unit length; the math below wants true rotations.
        out[idx2name[i]] = (Q / np.linalg.norm(Q, axis=1, keepdims=True), T)
    assert o == len(blob), f"motion blob: {len(blob) - o} trailing bytes"
    return ln, out


def encode_motion(name, ln, tracks):
    """tracks: data-order list of (Q[f,4], T[f,3])."""
    out = bytearray(name.encode("cp1251") + b"\x00")
    out += struct.pack("<I", ln)
    for Q, T in tracks:
        qk = np.clip(np.rint(Q * KEY_QUANT), -32767, 32767).astype("<i2")
        r_const = bool((qk == qk[0]).all())
        t_const = bool(np.abs(T - T[0]).max() < 1e-6)
        flags = (FL_R_ABSENT if r_const else 0) | (0 if t_const else FL_T_PRESENT | FL_T16)
        out += struct.pack("<B", flags)
        if r_const:
            out += qk[0].tobytes()
        else:
            rb = qk.tobytes()
            out += struct.pack("<I", zlib.crc32(rb) & 0xFFFFFFFF) + rb
        if t_const:
            out += T[0].astype("<f4").tobytes()
        else:
            mn, mx = T.min(0), T.max(0)
            init = (mx + mn) / 2
            size = np.maximum((mx - mn) / 2 / 32767.0, 1e-9)
            k = np.clip(np.rint((T - init) / size), -32767, 32767).astype("<i2")
            tb = k.tobytes()
            out += struct.pack("<I", zlib.crc32(tb) & 0xFFFFFFFF) + tb
            out += size.astype("<f4").tobytes() + init.astype("<f4").tobytes()
    return bytes(out)


def write_omf(path, vers, bone_names, defs, blobs):
    sp = bytearray(struct.pack("<HH", vers, 1))
    sp += b"default\x00" + struct.pack("<H", len(bone_names))
    for i, n in enumerate(bone_names):
        sp += n.encode("cp1251") + b"\x00" + struct.pack("<I", i)
    sp += struct.pack("<H", len(defs))
    for d in defs:
        sp += d["name"].encode("cp1251") + b"\x00" + d["blob"]
    mo = bytearray()
    for cid, blob in [(0, struct.pack("<I", len(blobs)))] + [(i + 1, b) for i, b in enumerate(blobs)]:
        mo += struct.pack("<II", cid, len(blob)) + blob
    out = struct.pack("<II", 14, len(mo)) + bytes(mo) + struct.pack("<II", 15, len(sp)) + bytes(sp)
    open(path, "wb").write(out)
    return len(out)


# ---- math (row vectors, as the engine: world = local @ parent) ---------------------------------

def rot_axis(axis, ang):
    """Row-vector rotation by ang about unit axis: v' = v @ R."""
    x, y, z = axis
    c, s = math.cos(ang), math.sin(ang)
    C = 1 - c
    return np.array([[c + x * x * C, x * y * C + z * s, x * z * C - y * s],
                     [y * x * C - z * s, c + y * y * C, y * z * C + x * s],
                     [z * x * C + y * s, z * y * C - x * s, c + z * z * C]])


def twist(Rl, axis):
    """Signed roll of the local rotation Rl about the unit axis, radians."""
    t = np.cross(axis, (0.0, 0.0, 1.0))
    if np.linalg.norm(t) < 1e-3:
        t = np.cross(axis, (0.0, 1.0, 0.0))
    t /= np.linalg.norm(t)
    r = t @ Rl
    r -= (r @ axis) * axis
    r /= np.linalg.norm(r)
    return math.atan2(np.dot(np.cross(t, r), axis), np.dot(t, r))


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def orthonormal(R):
    u, _, vt = np.linalg.svd(R)
    m = u @ vt
    if np.linalg.det(m) < 0:
        u[:, -1] *= -1
        m = u @ vt
    return m


def frame(a, p):
    """Rows: a, p made orthogonal to a, their cross."""
    a = a / np.linalg.norm(a)
    p = p - (p @ a) * a
    p /= np.linalg.norm(p)
    return np.stack([a, p, np.cross(a, p)])


def fk(rig, mot, f):
    """-> {bone: (R world 3x3, P world)}"""
    W = {}
    for b in rig:
        Q, T = mot[b["name"]]
        R = quat_to_mat(Q[f])
        if b["parent_idx"] < 0:
            W[b["name"]] = (R, T[f].copy())
        else:
            Rp, Pp = W[rig[b["parent_idx"]]["name"]]
            W[b["name"]] = (R @ Rp, T[f] @ Rp + Pp)
    return W


# ---- the retarget ------------------------------------------------------------------------------

class Retarget:
    def __init__(self, gs_rig, da_rig, gs_rest, we_rest):
        self.gs_rig, self.da_rig = gs_rig, da_rig
        self.da = {b["name"]: b for b in da_rig}
        self.parent = {b["name"]: b["parent"] for b in da_rig}
        names = [b["name"] for b in da_rig]
        self.arm = {n for n in names if any(k in n for k in ("clavicle", "upperarm", "forearm"))}
        self.hands = {"l_hand", "r_hand"}
        self.group = [n for n in names if n not in self.arm and n != "bip01"]
        G, W = gs_rest, we_rest
        self.G0, self.W0 = G, W

        # Uniform scale from the rest positions of the anchor, the wrists and the three outer
        # fingers (the addon nudged the thumbs and the index fingers by hand; they would only bias
        # the fit).
        fit = ["lead_gun", "l_hand", "r_hand"] + \
            [f"bip01_{side}_finger{k}{j}" for side in "lr" for k in "234" for j in ("", "1", "2")]
        a = np.concatenate([G[n][1] for n in fit])
        b = np.concatenate([W[n][1] for n in fit])
        self.s = float(a @ b / (a @ a))

        # World-orientation offsets, exact at rest.
        self.C = {n: orthonormal(W[n][0] @ G[n][0].T) for n in self.group}

        self.lead = "lead_gun"
        # Rest of each hand in the anchor's frame, on both rigs.
        self.hand_rest = {}
        for h in self.hands:
            self.hand_rest[h] = ((G[h][1] - G[self.lead][1]) @ G[self.lead][0].T,
                                 (W[h][1] - W[self.lead][1]) @ W[self.lead][0].T)
        # Rest local translations of the fingers, both rigs.
        self.local_rest = {}
        for n in self.group:
            if n == self.lead or n in self.hands:
                continue
            p = self.parent[n]
            self.local_rest[n] = ((G[n][1] - G[p][1]) @ G[p][0].T, (W[n][1] - W[p][1]) @ W[p][0].T)

        # Arms at rest.
        self.arms = {}
        for side in "lr":
            cl, ua, fa, tw, hd = (f"{side}_clavicle", f"{side}_upperarm", f"{side}_forearm",
                                  f"{side}_forearm_twist", f"{side}_hand")
            L1 = float(np.linalg.norm(self.da[fa]["bind_T"]))
            L2 = float(np.linalg.norm(self.da[hd]["bind_T"]))
            sh_g, el_g, wr_g = G[ua][1], G[fa][1], G[hd][1]
            sh_w, el_w, wr_w = W[ua][1], W[fa][1], W[hd][1]
            F_g = frame(wr_g - sh_g, el_g - sh_g)
            F_w = frame(wr_w - sh_w, el_w - sh_w)
            n_w = F_w[2]
            u = (el_w - sh_w) / np.linalg.norm(el_w - sh_w)
            v = (wr_w - el_w) / np.linalg.norm(wr_w - el_w)
            U = np.stack([u, np.cross(n_w, u), n_w])
            V = np.stack([v, np.cross(n_w, v), n_w])
            axis_l = self.da[hd]["bind_T"] / np.linalg.norm(self.da[hd]["bind_T"])
            self.arms[side] = {
                "names": (cl, ua, fa, tw, hd), "L1": L1, "L2": L2,
                "sh_g": sh_g, "sh_w": sh_w, "F_g": F_g, "F_w": F_w,
                "Rcl_g": G[cl][0], "Rcl_w": W[cl][0],
                "K_ua": W[ua][0] @ U.T, "K_fa": W[fa][0] @ V.T,
                "axis": axis_l,
                "hand_twist_rest": twist(W[hd][0] @ W[fa][0].T, axis_l),
                "tw_local_rest": W[tw][0] @ W[fa][0].T,
                "T_ua": self.da[ua]["bind_T"], "T_tw": self.da[tw]["bind_T"],
            }

    def frame_pose(self, Gf):
        """Gunslinger world pose of one frame -> DA world pose {bone: (R, P)}."""
        s, G0, W0, C = self.s, self.G0, self.W0, self.C
        out = {"bip01": (np.eye(3), np.zeros(3))}
        lead = self.lead
        R_lead = C[lead] @ Gf[lead][0]
        P_lead = W0[lead][1] + s * (Gf[lead][1] - G0[lead][1])
        out[lead] = (R_lead, P_lead)
        for h in self.hands:
            g_rest, w_rest = self.hand_rest[h]
            g_now = (Gf[h][1] - Gf[lead][1]) @ Gf[lead][0].T
            d = w_rest + s * (g_now - g_rest) @ C[lead].T
            out[h] = (C[h] @ Gf[h][0], d @ R_lead + P_lead)

        for side, A in self.arms.items():
            cl, ua, fa, tw, hd = A["names"]
            R_cl = A["Rcl_w"] @ (A["Rcl_g"].T @ Gf[cl][0])
            sh = A["sh_w"] + s * (Gf[ua][1] - A["sh_g"])
            wr = out[hd][1]
            L1, L2 = A["L1"], A["L2"]
            d = np.linalg.norm(wr - sh)
            if d > REACH * (L1 + L2):
                sh = sh + (wr - sh) / d * (d - REACH * (L1 + L2))
                d = REACH * (L1 + L2)
            d = max(d, abs(L1 - L2) + 1e-3)
            a = (wr - sh) / np.linalg.norm(wr - sh)
            # The arm plane: the rest plane carried by the original arm's own turn since rest.
            F_now = frame(Gf[hd][1] - Gf[ua][1], Gf[fa][1] - Gf[ua][1])
            p = (A["F_w"] @ (A["F_g"].T @ F_now))[1]
            p = p - (p @ a) * a
            p /= np.linalg.norm(p)
            x = (d * d + L1 * L1 - L2 * L2) / (2 * d)
            y = math.sqrt(max(L1 * L1 - x * x, 0.0))
            el = sh + a * x + p * y
            n = np.cross(a, p)
            u = (el - sh) / np.linalg.norm(el - sh)
            v = (wr - el) / np.linalg.norm(wr - el)
            R_ua = orthonormal(A["K_ua"] @ np.stack([u, np.cross(n, u), n]))
            R_fa = orthonormal(A["K_fa"] @ np.stack([v, np.cross(n, v), n]))
            # Wrist roll since rest, shared between the forearm bone and the twist bone.
            ax = A["axis"]
            delta = wrap(twist(out[hd][0] @ R_fa.T, ax) - A["hand_twist_rest"])
            roll = rot_axis(ax, -FOREARM_ROLL * delta)
            R_try = roll @ R_fa
            if abs(wrap(twist(out[hd][0] @ R_try.T, ax) - A["hand_twist_rest"])) > abs(delta):
                roll = rot_axis(ax, FOREARM_ROLL * delta)
                R_try = roll @ R_fa
            R_fa = R_try
            rest_dev = wrap(twist(out[hd][0] @ R_fa.T, ax) - A["hand_twist_rest"])
            # The twist bone turns, relative to the forearm, the way the wrist went.
            tw_l = A["tw_local_rest"]
            phi = (TWIST_ROLL - FOREARM_ROLL) / max(1.0 - FOREARM_ROLL, 1e-6) * rest_dev
            tw0 = twist(tw_l, ax)
            R_tw_l = min((tw_l @ rot_axis(ax, phi), tw_l @ rot_axis(ax, -phi)),
                         key=lambda c: abs(wrap(twist(c, ax) - tw0 - phi)))
            out[cl] = (R_cl, sh - A["T_ua"] @ R_cl)
            out[ua] = (R_ua, sh)
            out[fa] = (R_fa, el)
            out[tw] = (R_tw_l @ R_fa, A["T_tw"] @ R_fa + el)

        # Fingers: local deltas from rest, in the parent's frame, then forward to world.
        for b in self.da_rig:
            n = b["name"]
            if n in out:
                continue
            p = self.parent[n]
            g_rest, w_rest = self.local_rest[n]
            Gp = Gf[p]
            g_now = (Gf[n][1] - Gp[1]) @ Gp[0].T
            T = w_rest + s * (g_now - g_rest) @ C[p].T
            Rp, Pp = out[p]
            out[n] = (C[n] @ Gf[n][0], T @ Rp + Pp)
        return out

    def locals(self, pose):
        """World pose -> data-order-free {bone: (q, T local)}."""
        res = {}
        for b in self.da_rig:
            n = b["name"]
            R, P = pose[n]
            if b["parent_idx"] < 0:
                res[n] = (mat_to_quat(orthonormal(R)), P)
            else:
                Rp, Pp = pose[b["parent"]]
                res[n] = (mat_to_quat(orthonormal(R @ Rp.T)), (P - Pp) @ Rp.T)
        return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gs-omf")
    ap.add_argument("--we-omf")
    ap.add_argument("--out")
    ap.add_argument("--gs-rig", default=GS_RIG_JSON)
    ap.add_argument("--da-rig", default=DA_RIG_JSON)
    ap.add_argument("--dump-rig", nargs=2, metavar=("OGF", "JSON"))
    args = ap.parse_args()
    if args.dump_rig:
        dump_rig(*args.dump_rig)
        return
    if not (args.gs_omf and args.we_omf and args.out):
        ap.error("--gs-omf, --we-omf and --out are required")

    gs_rig, da_rig = load_rig(args.gs_rig), load_rig(args.da_rig)
    da_names = [b["name"] for b in da_rig]
    assert sorted(da_names) == sorted(b["name"] for b in gs_rig), "the rigs name different bones"

    g_vers, g_parts, g_defs, g_mo = read_omf(args.gs_omf)
    w_vers, w_parts, w_defs, w_mo = read_omf(args.we_omf)
    g_idx = {i: b for _, bl in g_parts for b, i in bl}
    w_idx = {i: b for _, bl in w_parts for b, i in bl}
    assert [w_idx[i] for i in range(len(w_idx))] == da_names, "the addon omf is not in DA bone order"
    g_by = {d["name"].lower(): (i, d) for i, d in enumerate(g_defs)}
    w_by = {d["name"].lower(): i for i, d in enumerate(w_defs)}

    def gs_clip(name):
        i, _ = g_by[name]
        return decode_motion(g_mo[i + 1], g_idx)

    def we_clip(name):
        return decode_motion(w_mo[w_by[name] + 1], w_idx)

    _, g_rest = gs_clip(REST_GS)
    _, w_rest = we_clip(REST_WE)
    rt = Retarget(gs_rig, da_rig, fk(gs_rig, g_rest, 0), fk(da_rig, w_rest, 0))
    print(f"scale {rt.s:.4f}")

    # Self-check 1: the rest frame reproduces the addon's rest pose.
    pose = rt.frame_pose(fk(gs_rig, g_rest, 0))
    W0 = rt.W0
    dp = max(np.linalg.norm(pose[n][1] - W0[n][1]) for n in da_names)
    dr = max(np.degrees(np.arccos(np.clip((np.trace(pose[n][0] @ W0[n][0].T) - 1) / 2, -1, 1))) for n in da_names)
    print(f"rest vs addon rest: {dp * 1000:.2f} mm, {dr:.2f} deg")
    assert dp < 2e-3 and dr < 1.0, "the calibration does not reproduce the addon's rest pose"

    # Self-check 2: our retarget of the addon's walk source against the addon's walk (hands only).
    ln_g, g_walk = gs_clip("pda_walk")
    ln_w, w_walk = we_clip("kpk_walk")
    worst = 0.0
    for f in range(min(ln_g, ln_w)):
        a = rt.frame_pose(fk(gs_rig, g_walk, f))
        b = fk(da_rig, w_walk, f)
        worst = max(worst, max(np.linalg.norm(a[n][1] - b[n][1]) for n in rt.group))
    print(f"walk, hands and fingers vs the addon: {worst * 1000:.2f} mm")

    defs, blobs = [], []
    for name in CLIPS:
        _, d = g_by[name]
        defs.append({"name": name, "blob": d["blob"]})
        if name in FROM_WE:
            blobs.append(w_mo[w_by[FROM_WE[name]] + 1])
            print(f"  {name:24s} <- addon {FROM_WE[name]}")
            continue
        ln, mot = gs_clip(name)
        frames = [rt.locals(rt.frame_pose(fk(gs_rig, mot, f))) for f in range(ln)]
        tracks = []
        for n in da_names:
            Q = np.stack([fr[n][0] for fr in frames])
            T = np.stack([fr[n][1] for fr in frames])
            for f in range(1, ln):
                if Q[f] @ Q[f - 1] < 0:
                    Q[f] = -Q[f]
            tracks.append((Q, T))
        blobs.append(encode_motion(name, ln, tracks))
        stretch = max(abs(np.linalg.norm(fr["l_hand"][1]) - rt.arms["l"]["L2"]) for fr in frames)
        print(f"  {name:24s} {ln:4d} frames, wrist length drift {stretch * 1000:.2f} mm")

    size = write_omf(args.out, g_vers, da_names, defs, blobs)
    print(f"wrote {args.out}: {len(defs)} motions, {size} bytes")

    # Self-check 3: our own output reads back on the DA rig.
    o_vers, o_parts, o_defs, o_mo = read_omf(args.out)
    o_idx = {i: b for _, bl in o_parts for b, i in bl}
    for i, d in enumerate(o_defs):
        decode_motion(o_mo[i + 1], o_idx)
    print("read back OK:", ", ".join(d["name"] for d in o_defs[:5]), "...")


if __name__ == "__main__":
    sys.exit(main())
