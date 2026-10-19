"""Re-render the tree impostors of a level's LOD atlas (level_lods) from the cards of a season look.

A LOD visual lists its tree's meshes (children) and eight view quads with their atlas rectangles. Each view is
rendered orthographically along its quad's normal: alpha-tested triangles at 4x4 supersampling, mips that keep
the alpha-tested coverage (as texconv -keepcoverage does for the game's own files), then box-filtered down to the
atlas cell. Only cells of vegetation are rendered; cars, ruins and rocks keep Dead Air's pictures.
"""
import hashlib
import os

import cv2
import numpy as np
from PIL import Image

import xr_level as X

SS = 4  # supersampling per axis


def coverage_mips(rgba, ref=0.5, max_kept=512):
    """Mip chain whose alpha-tested coverage matches the top level (premultiplied colour averaging).
    Returns (first kept level, [uint8 RGBA mips]): an impostor cell never samples finer than max_kept texels,
    so the top levels are dropped once their successors exist."""
    img = rgba.astype(np.float32) / 255.0
    cov0 = float((img[..., 3] > ref).mean())
    kept, first, level = [], None, 0
    while True:
        if max(img.shape[:2]) <= max_kept:
            if first is None:
                first = level
            kept.append(np.clip(img * 255 + 0.5, 0, 255).astype(np.uint8))
        if min(img.shape[:2]) <= 1:
            break
        h, w = img.shape[0] // 2 * 2, img.shape[1] // 2 * 2
        t = img[:h, :w]
        a = t[..., 3:4]
        a2 = a.reshape(h // 2, 2, w // 2, 2, 1).mean((1, 3))
        c2 = (t[..., :3] * a).reshape(h // 2, 2, w // 2, 2, 3).mean((1, 3)) / np.maximum(a2, 1e-4)
        # Colour of fully transparent texels: plain average, so bilinear edges do not darken
        plain = t[..., :3].reshape(h // 2, 2, w // 2, 2, 3).mean((1, 3))
        c2 = np.where(a2 > 1e-4, c2, plain)
        lo, hi = 0.0, 8.0
        for _ in range(18):
            mid = 0.5 * (lo + hi)
            if float((np.clip(a2 * mid, 0, 1) > ref).mean()) < cov0:
                lo = mid
            else:
                hi = mid
        img = np.concatenate([c2, np.clip(a2 * hi, 0, 1)], -1)
        level += 1
    return first, kept


class TexCache:
    def __init__(self, resolve):
        self.resolve, self.cache = resolve, {}

    def get(self, name):
        if name not in self.cache:
            p = self.resolve(name)
            if p is None:
                self.cache[name] = None
            else:
                im = np.asarray(Image.open(p).convert('RGBA'))
                self.cache[name] = coverage_mips(im)
        return self.cache[name]


def sample(mip, u, v):
    """Bilinear, wrapping; uint8 mips are converted only at the fetched texels."""
    h, w = mip.shape[:2]
    k = 1.0 / 255.0 if mip.dtype == np.uint8 else 1.0
    x = u * w - 0.5
    y = v * h - 0.5
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0)[:, None]
    fy = (y - y0)[:, None]
    x0 %= w
    y0 %= h
    x1 = (x0 + 1) % w
    y1 = (y0 + 1) % h
    f = lambda yy, xx: mip[yy, xx].astype(np.float32) * k
    return (f(y0, x0) * (1 - fx) * (1 - fy) + f(y0, x1) * fx * (1 - fy) +
            f(y1, x0) * (1 - fx) * fy + f(y1, x1) * fx * fy)


def facet_frame(quad):
    P = np.array([q[0] for q in quad], np.float64)
    T = np.array([q[1] for q in quad], np.float64)

    def mkn(a, b, c):
        n = np.cross(b - a, c - a)
        return n / max(np.linalg.norm(n), 1e-12)
    N = (mkn(P[0], P[1], P[2]) + mkn(P[1], P[2], P[3]) + mkn(P[2], P[3], P[0]) + mkn(P[3], P[0], P[1])) / 4
    N = -N / max(np.linalg.norm(N), 1e-12)
    return P, T, N


def fragments(meshes, quad, cell_w, cell_h):
    """Rasterise a view once: per mesh, the covered supersampled pixels with depth, texture coords and texel density.
    Shading (texture lookup, alpha test, depth resolve) is per look and reuses these."""
    P4, T4, N = facet_frame(quad)
    e1, e2 = P4[1] - P4[0], P4[3] - P4[0]
    G = np.array([[e1 @ e1, e1 @ e2], [e1 @ e2, e2 @ e2]])
    Gi = np.linalg.inv(G)
    W, H = cell_w * SS, cell_h * SS
    out = []
    for P, uv, tris, tex in meshes:
        if len(tris) == 0:
            continue
        d = P - P4[0]
        st = (d @ np.c_[e1, e2]) @ Gi.T
        sx, sy, sz = st[:, 0] * W, st[:, 1] * H, d @ N
        tx, ty, tz = sx[tris], sy[tris], sz[tris]
        tu, tv = uv[tris, 0], uv[tris, 1]
        area = (tx[:, 1] - tx[:, 0]) * (ty[:, 2] - ty[:, 0]) - (tx[:, 2] - tx[:, 0]) * (ty[:, 1] - ty[:, 0])
        x0 = np.clip(np.floor(tx.min(1)), 0, W).astype(np.int64)
        x1 = np.clip(np.ceil(tx.max(1)), 0, W).astype(np.int64)
        y0 = np.clip(np.floor(ty.min(1)), 0, H).astype(np.int64)
        y1 = np.clip(np.ceil(ty.max(1)), 0, H).astype(np.int64)
        ok = (np.abs(area) > 1e-9) & (x1 > x0) & (y1 > y0)
        if not ok.any():
            continue
        k = np.flatnonzero(ok)
        tx, ty, tz, tu, tv, area = tx[k], ty[k], tz[k], tu[k], tv[k], area[k]
        x0, x1, y0, y1 = x0[k], x1[k], y0[k], y1[k]
        uvarea = np.abs((tu[:, 1] - tu[:, 0]) * (tv[:, 2] - tv[:, 0]) - (tu[:, 2] - tu[:, 0]) * (tv[:, 1] - tv[:, 0]))
        dens = np.maximum(uvarea, 1e-12) / np.abs(area)
        w = x1 - x0
        n = w * (y1 - y0)
        parts = []
        start = 0
        while start < len(k):
            cs = np.cumsum(n[start:])
            stop = start + max(1, int(np.searchsorted(cs, 3_000_000)))
            sel = np.arange(start, min(stop, len(k)))
            start = stop
            nn = n[sel]
            ti = np.repeat(sel, nn)
            loc = np.arange(nn.sum()) - np.repeat(np.cumsum(nn) - nn, nn)
            px = x0[ti] + loc % w[ti]
            py = y0[ti] + loc // w[ti]
            cx, cy = px + 0.5, py + 0.5
            ax, ay = tx[ti], ty[ti]
            ar = area[ti]
            l1 = ((ax[:, 2] - ax[:, 0]) * (cy - ay[:, 0]) - (ay[:, 2] - ay[:, 0]) * (cx - ax[:, 0])) / -ar
            l2 = ((ax[:, 0] - ax[:, 1]) * (cy - ay[:, 1]) - (ay[:, 0] - ay[:, 1]) * (cx - ax[:, 1])) / -ar
            l0 = 1 - l1 - l2
            inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
            if not inside.any():
                continue
            ti, px, py, l0, l1, l2 = ti[inside], px[inside], py[inside], l0[inside], l1[inside], l2[inside]
            z = tz[ti, 0] * l0 + tz[ti, 1] * l1 + tz[ti, 2] * l2
            u = tu[ti, 0] * l0 + tu[ti, 1] * l1 + tu[ti, 2] * l2
            v = tv[ti, 0] * l0 + tv[ti, 1] * l1 + tv[ti, 2] * l2
            parts.append((py * W + px, z.astype(np.float32), u.astype(np.float32), v.astype(np.float32), dens[ti].astype(np.float32)))
        if parts:
            out.append((tex,) + tuple(np.concatenate([p[i] for p in parts]) for i in range(5)))
    return out, W, H


def shade(frags, W, H, texs):
    """Texture lookup, alpha test at 0.5 and nearest-surface resolve. Returns supersampled RGB and mask."""
    zbuf = np.full(W * H, np.inf, np.float32)
    cbuf = np.zeros((W * H, 3), np.float32)
    for tex, pix, z, u, v, dens in frags:
        got = texs.get(tex)
        if got is None:
            continue
        first, mips = got
        # Texel density in the top level's texels, then shifted to the first level kept
        th, tw = mips[0].shape[0] << first, mips[0].shape[1] << first
        lv = np.clip(np.round(0.5 * np.log2(dens * tw * th)) - first, 0, len(mips) - 1).astype(np.int64)
        col = np.empty((len(pix), 4), np.float32)
        for L in np.unique(lv):
            m = lv == L
            col[m] = sample(mips[L], u[m].astype(np.float64), v[m].astype(np.float64))
        keep = col[:, 3] > 0.5
        if not keep.any():
            continue
        p, zz, c = pix[keep], z[keep], col[keep, :3]
        o = np.lexsort((zz, p))
        p, zz, c = p[o], zz[o], c[o]
        first = np.r_[True, p[1:] != p[:-1]]
        p, zz, c = p[first], zz[first], c[first]
        closer = zz < zbuf[p]
        zbuf[p[closer]] = zz[closer]
        cbuf[p[closer]] = c[closer]
    return cbuf.reshape(H, W, 3), np.isfinite(zbuf).reshape(H, W)


def render_view(meshes, quad, cell_w, cell_h, texs):
    frags, W, H = fragments(meshes, quad, cell_w, cell_h)
    return shade(frags, W, H, texs)


def downsample(rgb, mask, cell_w, cell_h):
    m = mask.astype(np.float32)
    cov = m.reshape(cell_h, SS, cell_w, SS).mean((1, 3))
    col = (rgb * m[..., None]).reshape(cell_h, SS, cell_w, SS, 3).sum((1, 3)) / np.maximum(m.reshape(cell_h, SS, cell_w, SS).sum((1, 3)), 1e-6)[..., None]
    return col, cov


def leaf_meshes(L, v, out=None):
    out = [] if out is None else out
    for c in v.get('children', []):
        cv = L.visuals[c]
        if 'children' in cv and 'gc' not in cv:
            leaf_meshes(L, cv, out)
        elif 'gc' in cv:
            tex = L.texture_of(cv)[1]
            if cv['type'] in (X.MT_TREE_ST, X.MT_TREE_PM):
                P, uv, tris = L.tree_mesh(cv)
            else:
                P, uv, tris = L.normal_mesh(cv)
            out.append((P, uv, tris, tex))
    return out


def lod_objects(L, atlas_w, atlas_h):
    """Unique LOD objects: one instance per atlas rectangle set."""
    seen = {}
    for v in L.visuals:
        if v['type'] != X.MT_LOD:
            continue
        key = tuple(round(q[1][0], 4) for f in v['facets'] for q in f) + tuple(round(q[1][1], 4) for f in v['facets'] for q in f)
        if key not in seen:
            seen[key] = v
    objs = []
    for v in seen.values():
        cells = []
        for f in v['facets']:
            us = [q[1][0] for q in f]
            vs = [q[1][1] for q in f]
            x0, x1 = int(np.floor(min(us) * atlas_w + 0.01)), int(np.ceil(max(us) * atlas_w - 0.01))
            y0, y1 = int(np.floor(min(vs) * atlas_h + 0.01)), int(np.ceil(max(vs) * atlas_h - 0.01))
            cells.append((x0, y0, x1 - x0, y1 - y0))
        objs.append((v, cells))
    return objs


def object_key(meshes):
    h = hashlib.sha1()
    for P, uv, tris, tex in sorted(meshes, key=lambda m: (m[3], len(m[2]))):
        h.update(tex.encode())
        c = P.mean(0)
        h.update(np.round(P - c, 2).astype(np.float32).tobytes())
        h.update(np.round(uv, 4).astype(np.float32).tobytes())
    return h.hexdigest()
