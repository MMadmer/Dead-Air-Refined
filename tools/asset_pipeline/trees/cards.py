"""Own tree-card textures for Dead Air: Refined 1.6.0.

Only the mesh's contract is taken from Dead Air's card: where the branch enters the card (the base),
the rough envelope it fills, how much of the card is opaque, and the colour statistics of wood and
foliage. The branches are grown here (space colonisation), the leaves and needles are CC0 scans.
"""
import math

import cv2
import numpy as np

LIGHT = np.array([-0.55, -0.83])  # light from the upper left, image coordinates


# ---------------------------------------------------------------- layout of Dead Air's card

def classes(rgba, prefer_green=False):
    """Wood / foliage split of a card's opaque pixels by colour. Returns (foliage mask, stats per class)."""
    a = rgba[..., 3] > 127
    lab = cv2.cvtColor(rgba[..., :3], cv2.COLOR_RGB2LAB).astype(np.float32)
    px = lab[a]
    if len(px) < 50:
        return np.zeros_like(a), {}
    feat = np.stack([px[:, 1] - 128, px[:, 2] - 128], 1).astype(np.float32)
    _, lbl, cent = cv2.kmeans(feat, 2, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.2), 4, cv2.KMEANS_PP_CENTERS)
    lbl = lbl.ravel()
    score = np.hypot(cent[:, 0], cent[:, 1]) + 0.6 * np.maximum(0, -cent[:, 0])
    if prefer_green:
        # Conifers: needles are the greener cluster even when brown twigs are more saturated
        score = -cent[:, 0] - 0.3 * cent[:, 1]
    fol = int(np.argmax(score))
    sep = abs(score[0] - score[1])
    fmask = np.zeros_like(a)
    stats = {}
    if sep < 5.0 or min((lbl == 0).mean(), (lbl == 1).mean()) < 0.04:
        stats['wood'] = (px.mean(0), px.std(0))
        stats['foliage_frac'] = 0.0
        return fmask, stats
    fsel = lbl == fol
    idx = np.flatnonzero(a.ravel())
    fmask.ravel()[idx[fsel]] = True
    stats['foliage'] = (px[fsel].mean(0), px[fsel].std(0))
    stats['wood'] = (px[~fsel].mean(0), px[~fsel].std(0))
    stats['foliage_frac'] = float(fsel.mean())
    return fmask, stats


def analyse(rgba):
    """Base point, initial direction, density field and trunk radius of a card (normalised to max side = 1)."""
    a = (rgba[..., 3] > 127)
    H, W = a.shape
    S = float(max(H, W))
    dist = cv2.distanceTransform(a.astype(np.uint8), cv2.DIST_L2, 5)
    band = max(2, int(0.025 * S))
    border = np.zeros_like(a)
    border[:band] = border[-band:] = True
    border[:, :band] = border[:, -band:] = True
    cand = dist * border
    y, x = np.unravel_index(int(np.argmax(cand)), cand.shape)
    trunk_r = float(dist[y, x]) / S
    # Snap to the edge the branch enters through
    dists = {'top': y, 'bottom': H - 1 - y, 'left': x, 'right': W - 1 - x}
    edge = min(dists, key=dists.get)
    if edge == 'top':
        y = 0
    elif edge == 'bottom':
        y = H - 1
    elif edge == 'left':
        x = 0
    else:
        x = W - 1
    ys, xs = np.nonzero(a)
    cen = np.array([xs.mean(), ys.mean()])
    base = np.array([x, y], np.float32)
    d = cen - base
    d /= max(np.linalg.norm(d), 1e-6)
    density = cv2.GaussianBlur(a.astype(np.float32), (0, 0), 0.02 * S)
    return {'base': base / S, 'dir': d, 'density': density, 'S': S, 'W': W / S, 'H': H / S,
            'trunk_r': max(trunk_r, 0.004), 'coverage': float(a.mean()), 'edge': edge}


# ---------------------------------------------------------------- growth

def colonise(lay, rng, n_points=2600, R=0.11, kill=0.018, step=0.0075, tropism=(0.0, 0.0), inertia=0.6,
             jitter=0.25, max_iter=900, trunk_len=0.0):
    """Space colonisation inside the density field. Returns node positions (normalised) and parents."""
    dens = lay['density']
    Hd, Wd = dens.shape
    p = dens.ravel() / dens.sum()
    pick = rng.choice(dens.size, n_points, p=p)
    pts = np.stack([(pick % Wd + rng.random(n_points)) / lay['S'], (pick // Wd + rng.random(n_points)) / lay['S']], 1)
    alive = np.ones(n_points, bool)
    pos = [lay['base'].astype(np.float64)]
    par = [-1]
    dirs = [lay['dir'].astype(np.float64)]
    trop = np.array(tropism, np.float64)
    # Trunk: straight-ish run until the crown is in reach
    while True:
        d2 = ((pts[alive] - pos[-1]) ** 2).sum(1)
        travelled = np.linalg.norm(pos[-1] - pos[0])
        if (d2.min() if len(d2) else 0) < R * R and travelled >= trunk_len:
            break
        nd = dirs[-1] * 0.85 + lay['dir'] * 0.15 + trop * 0.1 + rng.normal(0, 0.03, 2)
        nd /= np.linalg.norm(nd)
        pos.append(pos[-1] + nd * step)
        par.append(len(pos) - 2)
        dirs.append(nd)
        if len(pos) > 400:
            break
    P = np.array(pos)
    D = np.array(dirs)
    parent = list(par)
    children = np.zeros(len(P), int)
    # Nearest node per attraction point, updated incrementally against new nodes only
    best_d = np.full(n_points, np.inf)
    best_i = np.zeros(n_points, int)

    def update(new_idx):
        q = pts
        for s in range(0, len(new_idx), 256):
            nn = new_idx[s:s + 256]
            d2 = ((q[:, None, :] - P[None, nn, :]) ** 2).sum(2)
            j = d2.argmin(1)
            dj = d2[np.arange(len(q)), j]
            better = dj < best_d
            best_d[better] = dj[better]
            best_i[better] = np.asarray(nn)[j[better]]

    update(np.arange(len(P)))
    for it in range(max_iter):
        alive &= best_d >= kill * kill
        infl = alive & (best_d < R * R)
        if not infl.any():
            break
        vec = pts[infl] - P[best_i[infl]]
        vec /= np.maximum(np.linalg.norm(vec, axis=1, keepdims=True), 1e-9)
        acc = np.zeros_like(P)
        np.add.at(acc, best_i[infl], vec)
        grow = np.flatnonzero(np.abs(acc).sum(1) > 0)
        grow = grow[children[grow] < 4]
        if len(grow) == 0:
            break
        nd = acc[grow]
        nd /= np.maximum(np.linalg.norm(nd, axis=1, keepdims=True), 1e-9)
        nd = nd * (1 - inertia * 0.5) + D[grow] * inertia * 0.5 + trop + rng.normal(0, jitter * 0.3, nd.shape)
        nd /= np.maximum(np.linalg.norm(nd, axis=1, keepdims=True), 1e-9)
        newp = P[grow] + nd * step
        first_new = len(P)
        P = np.vstack([P, newp])
        D = np.vstack([D, nd])
        parent += list(grow)
        children[grow] += 1
        children = np.concatenate([children, np.zeros(len(grow), int)])
        update(np.arange(first_new, len(P)))
    return P, np.array(parent), D


def _rot(d, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return np.array([d[0] * c - d[1] * s, d[0] * s + d[1] * c])


def grow_tree(lay, rng, cfg):
    """Recursive branching: a leader, tiers of laterals, sub-laterals; every branch stops at the card's envelope.

    cfg (per depth lists): length, spacing, angle (deg range), per_node, trop (vector added per step), noise.
    """
    E = lay['density']
    S = lay['S']
    thr = cfg.get('env', 0.06)
    H, W = E.shape

    def inside(p):
        x, y = int(p[0] * S), int(p[1] * S)
        return 0 <= x < W and 0 <= y < H and E[y, x] > thr

    step = cfg.get('step', 0.005)
    lengths = cfg['length']
    spacing = cfg['spacing']
    angle = cfg['angle']
    per_node = cfg['per_node']
    trop = [np.array(t, np.float64) for t in cfg['trop']]
    noise = cfg['noise']
    max_depth = len(lengths) - 1
    clear = cfg.get('clear', 0.0)
    taper = cfg.get('taper', 0.55)
    P, parent, D = [lay['base'].astype(np.float64)], [-1], [lay['dir'].astype(np.float64)]
    shoots = cfg.get('shoots', 0)
    if shoots:
        spread = cfg.get('shoot_spread', 14)
        stack = [(0, P[0].copy(), _rot(D[0], a + rng.normal(0, 2)), lengths[0] * rng.uniform(0.75, 1.0), 0)
                 for a in np.linspace(-spread, spread, shoots)]
    else:
        stack = [(0, P[0].copy(), D[0].copy(), lengths[0], 0)]
    while stack:
        pidx, pos, d, L, dep = stack.pop()
        n = max(1, int(L / step))
        cur, travelled = pidx, 0.0
        if dep >= max_depth:
            next_spawn = float('inf')
        else:
            next_spawn = clear if dep == 0 else spacing[dep] * rng.uniform(0.2, 0.8)
        side = 1 if rng.random() < 0.5 else -1
        outside_run = 0
        for k in range(n):
            d = d + trop[dep] + rng.normal(0, noise[dep], 2)
            d /= np.linalg.norm(d)
            newp = pos + d * step
            if not inside(newp):
                # The leader may cross a thin gap in the envelope; laterals stop at the edge
                outside_run += 1
                if dep > 0 or outside_run > 14:
                    break
            else:
                outside_run = 0
            P.append(newp)
            parent.append(cur)
            D.append(d.copy())
            cur, pos = len(P) - 1, newp
            travelled += step
            if dep < max_depth and travelled >= next_spawn:
                frac = travelled / L
                for _ in range(per_node[dep]):
                    cd = _rot(d, side * rng.uniform(*angle[dep]))
                    side = -side
                    clen = lengths[dep + 1] * rng.uniform(0.7, 1.15) * (1.0 - taper * frac)
                    if clen > step * 2:
                        stack.append((cur, pos.copy(), cd, clen, dep + 1))
                next_spawn = travelled + spacing[dep] * rng.uniform(0.7, 1.3)
    return np.array(P), np.array(parent), np.array(D)


def coverage_curve(P, parent, r, plan, pool, W, H, scale, batch=25):
    """Coverage after the wood and after each batch of foliage, in plan order (alpha only, one pass)."""
    m = np.zeros((H, W), np.uint8)
    for i in range(1, len(P)):
        a, b = P[parent[i]] * scale, P[i] * scale
        rr = max(0.5, r[i] * scale)
        cv2.line(m, tuple(np.round(a * 16).astype(int)), tuple(np.round(b * 16).astype(int)), 1, max(1, int(round(rr * 2))), cv2.LINE_8, shift=4)
    covs = [float(m.mean())]
    cache = {}
    for k, (i, ang, size, tint, back) in enumerate(plan):
        sp = pool[k % len(pool)] if pool else None
        if sp is None:
            break
        key = id(sp)
        if key not in cache:
            cache[key] = (sp.rgba[..., 3] > 0.5).astype(np.uint8)
        src = cache[key]
        hpx = size * scale
        kk = hpx / src.shape[0]
        if kk <= 0:
            continue
        d = int(math.ceil(math.hypot(src.shape[0] * kk, src.shape[1] * kk))) * 2 + 4
        M = cv2.getRotationMatrix2D((src.shape[1] / 2, src.shape[0]), -ang, kk)
        M[0, 2] += d / 2 - src.shape[1] / 2
        M[1, 2] += d / 2 - src.shape[0]
        patch = cv2.warpAffine(src, M, (d, d), flags=cv2.INTER_NEAREST)
        x0, y0 = int(round(P[i][0] * scale - d / 2)), int(round(P[i][1] * scale - d / 2))
        xa, ya, xb, yb = max(0, x0), max(0, y0), min(W, x0 + d), min(H, y0 + d)
        if xa < xb and ya < yb:
            np.maximum(m[ya:yb, xa:xb], patch[ya - y0:yb - y0, xa - x0:xb - x0], out=m[ya:yb, xa:xb])
        if (k + 1) % batch == 0:
            covs.append(float(m.mean()))
    covs.append(float(m.mean()))
    return np.array(covs)


def add_twigs(P, parent, D, rng, n_per_tip=(1, 3), length=(0.008, 0.03), step=0.004, spread=55, lateral=0.0):
    """Short fine twigs off the tips (and, with lateral > 0, off thin branches): detail the colonisation does not reach."""
    nchild = np.bincount(parent[parent >= 0], minlength=len(P))
    tips = list(np.flatnonzero(nchild == 0))
    if lateral > 0:
        tips += [i for i in range(1, len(P)) if nchild[i] == 1 and rng.random() < lateral]
    P, D, parent = list(P), list(D), list(parent)
    for t in tips:
        for _ in range(rng.integers(n_per_tip[0], n_per_tip[1] + 1)):
            ang = math.radians(rng.uniform(-spread, spread))
            d = np.array(D[t])
            d = np.array([d[0] * math.cos(ang) - d[1] * math.sin(ang), d[0] * math.sin(ang) + d[1] * math.cos(ang)])
            L = rng.uniform(*length)
            cur = t
            n = max(1, int(L / step))
            for _ in range(n):
                d = d + rng.normal(0, 0.12, 2)
                d /= np.linalg.norm(d)
                P.append(P[cur] + d * step)
                D.append(d)
                parent.append(cur)
                cur = len(P) - 1
    return np.array(P), np.array(parent), np.array(D)


def smooth(P, parent, it=2):
    P = P.copy()
    kids = [[] for _ in range(len(P))]
    for i, p in enumerate(parent):
        if p >= 0:
            kids[p].append(i)
    for _ in range(it):
        Q = P.copy()
        for i in range(1, len(P)):
            p = parent[i]
            if p >= 0 and len(kids[i]) == 1:
                Q[i] = 0.5 * P[i] + 0.25 * P[p] + 0.25 * P[kids[i][0]]
        P = Q
    return P


def radii(parent, r_tip, r_base, expo=2.4):
    """Pipe model: a node's cross-section carries all tips above it."""
    n = len(parent)
    tips = np.zeros(n)
    nchild = np.bincount(parent[parent >= 0], minlength=n)
    tips[nchild == 0] = 1
    for i in range(n - 1, 0, -1):
        tips[parent[i]] += tips[i]
    r = r_tip * tips ** (1.0 / expo)
    if r[0] > 0:
        r *= min(1.0, r_base / r[0]) if r[0] > r_base else 1.0
    return np.maximum(r, r_tip)


# ---------------------------------------------------------------- rendering

class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.rgb = np.zeros((h, w, 3), np.float32)
        self.a = np.zeros((h, w), np.float32)
        self.fol = np.zeros((h, w), np.float32)  # foliage share of each pixel's colour

    def over(self, x0, y0, rgb, a, fol):
        """Composite a patch (straight alpha) at x0, y0."""
        h, w = a.shape
        xa, ya, xb, yb = max(0, x0), max(0, y0), min(self.w, x0 + w), min(self.h, y0 + h)
        if xa >= xb or ya >= yb:
            return
        sa = a[ya - y0:yb - y0, xa - x0:xb - x0, None]
        sr = rgb[ya - y0:yb - y0, xa - x0:xb - x0]
        fol = fol[ya - y0:yb - y0, xa - x0:xb - x0]
        dr, da, df = self.rgb[ya:yb, xa:xb], self.a[ya:yb, xa:xb, None], self.fol[ya:yb, xa:xb, None]
        out_a = sa + da * (1 - sa)
        safe = np.maximum(out_a, 1e-6)
        self.rgb[ya:yb, xa:xb] = (sr * sa + dr * da * (1 - sa)) / safe
        self.fol[ya:yb, xa:xb] = ((fol * sa + df * da * (1 - sa)) / safe)[..., 0]
        self.a[ya:yb, xa:xb] = out_a[..., 0]


def draw_wood(cv, P, parent, r, scale, bark_tex, base_col, rng, white_from=None, white_col=None):
    """Branches as tapered quads with cylinder shading and bark grain."""
    H, W = cv.h, cv.w
    m_full = np.zeros((H, W), np.uint8)
    m_mid = np.zeros((H, W), np.uint8)
    m_hi = np.zeros((H, W), np.uint8)
    m_lo = np.zeros((H, W), np.uint8)
    m_white = np.zeros((H, W), np.uint8)
    for i in range(1, len(P)):
        p = parent[i]
        a, b = P[p] * scale, P[i] * scale
        d = b - a
        L = np.hypot(*d)
        if L < 1e-6:
            continue
        n = np.array([-d[1], d[0]]) / L
        if n @ LIGHT < 0:
            n = -n
        ra, rb = r[p] * scale, r[i] * scale
        layers = [(m_full, 1.0, 0.0), (m_mid, 0.72, 0.0), (m_hi, 0.38, 0.32), (m_lo, 0.4, -0.45)]
        if white_from is not None and r[i] >= white_from:
            layers.append((m_white, 1.0, 0.0))
        for mask, k, off in layers:
            pa, pb = a + n * ra * off, b + n * rb * off
            poly = np.array([pa + n * ra * k, pb + n * rb * k, pb - n * rb * k, pa - n * ra * k])
            cv2.fillPoly(mask, [np.round(poly * 16).astype(np.int32)], 255, cv2.LINE_AA, shift=4)
            cv2.circle(mask, tuple(np.round(pb * 16).astype(int)), int(max(1, rb * k * 16)), 255, -1, cv2.LINE_AA, shift=4)
    f = m_full.astype(np.float32) / 255
    shade = 0.62 + 0.22 * (m_mid / 255.0) + 0.24 * (m_hi / 255.0) - 0.16 * (m_lo / 255.0)
    tex = bark_tex[np.arange(H)[:, None] % bark_tex.shape[0], np.arange(W)[None, :] % bark_tex.shape[1]]
    rgb = np.clip(base_col[None, None, :] * (shade * tex)[..., None], 0, 1)
    if white_from is not None:
        # Birch: white bark with dark horizontal lenticels on the trunk and thick limbs
        rng2 = np.random.default_rng(7)
        marks = np.ones((H, W), np.float32)
        for _ in range(int(H * W / 900)):
            x, y = int(rng2.integers(0, W)), int(rng2.integers(0, H))
            cv2.ellipse(marks, (x, y), (int(rng2.integers(2, 9)), 1), 0, 0, 360, float(rng2.uniform(0.15, 0.5)), -1)
        marks = cv2.GaussianBlur(marks, (0, 0), 0.7)
        wrgb = np.clip(white_col[None, None, :] * (shade * marks)[..., None], 0, 1)
        wm = (m_white.astype(np.float32) / 255)[..., None]
        rgb = rgb * (1 - wm) + wrgb * wm
    ys, xs = np.nonzero(f > 0)
    if len(ys) == 0:
        return
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    cv.over(x0, y0, rgb[y0:y1, x0:x1], f[y0:y1, x0:x1], np.zeros((y1 - y0, x1 - x0, 1), np.float32))


def place(cv, sprite, x, y, angle_deg, height_px, tint=1.0, sat=1.0, fol=1.0):
    """Sprite with its attachment (bottom centre) at x, y, pointing along angle (0 = up).

    Warped into the tight box of the rotated sprite: a square around the anchor costs four times the pixels.
    """
    s = sprite.rgba
    sh, sw = s.shape[:2]
    k = height_px / sh
    if k <= 0:
        return
    M = cv2.getRotationMatrix2D((sw / 2, sh), -angle_deg, k)
    corners = np.array([[0, 0, 1], [sw, 0, 1], [0, sh, 1], [sw, sh, 1]], np.float64) @ M.T
    rel = corners - np.array([sw / 2, sh])
    x0r, y0r = np.floor(rel.min(0)) - 1
    x1r, y1r = np.ceil(rel.max(0)) + 1
    W, H = int(x1r - x0r), int(y1r - y0r)
    if W <= 0 or H <= 0:
        return
    M[0, 2] += -(sw / 2) - x0r
    M[1, 2] += -sh - y0r
    patch = cv2.warpAffine(s, M, (W, H), flags=cv2.INTER_AREA if k < 1 else cv2.INTER_CUBIC, borderValue=0)
    a = np.clip(patch[..., 3], 0, 1)
    if a.max() < 0.05:
        return
    rgb = patch[..., :3]
    if sat != 1.0:
        g = rgb.mean(2, keepdims=True)
        rgb = g + (rgb - g) * sat
    rgb = np.clip(rgb * tint, 0, 1)
    cv.over(int(round(x + x0r)), int(round(y + y0r)), rgb, a, np.full((H, W, 1), fol, np.float32))


# ---------------------------------------------------------------- colour

def transfer(rgb_u8, alpha, fol, stats, keep_wood=False):
    """Per-class Lab mean/spread transfer to Dead Air's card."""
    if keep_wood:
        stats = {k: v for k, v in stats.items() if k != 'wood'}
        if 'foliage' not in stats:
            return rgb_u8
        lab = cv2.cvtColor(rgb_u8, cv2.COLOR_RGB2LAB).astype(np.float32)
        sel = (alpha > 0.5) & (fol > 0.5)
        if sel.sum() < 30:
            return rgb_u8
        tm, ts = stats['foliage']
        sm, ss = lab[sel].mean(0), lab[sel].std(0)
        t = (lab - sm) * (ts / np.maximum(ss, 1e-3)) + tm
        out = lab * (1 - fol[..., None]) + t * fol[..., None]
        return cv2.cvtColor(np.clip(out + 0.5, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB)
    lab = cv2.cvtColor(rgb_u8, cv2.COLOR_RGB2LAB).astype(np.float32)
    m = alpha > 0.5
    out = lab.copy()
    wood_w = 1.0 - fol
    res = np.zeros_like(lab)
    wsum = np.zeros(lab.shape[:2], np.float32)
    for name, wmap in (('wood', wood_w), ('foliage', fol)):
        if name not in stats:
            continue
        tgt_m, tgt_s = stats[name]
        sel = m & (wmap > 0.5)
        if sel.sum() < 30:
            continue
        src_m, src_s = lab[sel].mean(0), lab[sel].std(0)
        t = (lab - src_m) * (tgt_s / np.maximum(src_s, 1e-3)) + tgt_m
        res += t * wmap[..., None]
        wsum += wmap
    if wsum.max() == 0:
        return rgb_u8
    # Pixels whose class has no target (e.g. leaves on a bare variant) fall back to the other class
    other = 'foliage' if 'foliage' in stats else 'wood'
    tgt_m, tgt_s = stats[other]
    sel = m
    src_m, src_s = lab[sel].mean(0), lab[sel].std(0)
    fallback = (lab - src_m) * (tgt_s / np.maximum(src_s, 1e-3)) + tgt_m
    out = np.where(wsum[..., None] > 1e-3, res / np.maximum(wsum, 1e-6)[..., None], fallback)
    rem = np.clip(1 - wsum, 0, 1)[..., None]
    out = out * (1 - rem) + fallback * rem
    return cv2.cvtColor(np.clip(out + 0.5, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB)


def occlude(cv, strength=0.52, sigma_rel=0.016):
    """Crown interiors darker than their rims, like light falling into foliage."""
    S = max(cv.w, cv.h)
    dens = cv2.GaussianBlur(cv.a, (0, 0), sigma_rel * S)
    shade = 1.0 - strength * np.clip((dens - 0.15) / 0.6, 0, 1)
    cv.rgb *= shade[..., None]


def finalize(cv, out_w, out_h):
    """Premultiplied area downsample to the output size."""
    pm = cv.rgb * cv.a[..., None]
    pm = cv2.resize(pm, (out_w, out_h), interpolation=cv2.INTER_AREA)
    a = cv2.resize(cv.a, (out_w, out_h), interpolation=cv2.INTER_AREA)
    fol = cv2.resize(cv.fol * cv.a, (out_w, out_h), interpolation=cv2.INTER_AREA) / np.maximum(a, 1e-6)
    rgb = pm / np.maximum(a, 1e-6)[..., None]
    return np.clip(rgb, 0, 1), np.clip(a, 0, 1), np.clip(fol, 0, 1)


# ---------------------------------------------------------------- seasonal dressing (applied to the final RGBA)

def snow(rgb, a, amount=1.0, depth_rel=0.012, frost=0.18, seed=0, col=(232, 236, 243)):
    """Snow on every upward-facing edge (nothing opaque just above it), plus a light frost over the rest."""
    H, W = a.shape
    A = (a.astype(np.float32) / 255.0)
    d = max(2, int(depth_rel * max(H, W)))
    above = np.zeros_like(A)
    for k in range(1, d + 1):
        above[k:] = np.maximum(above[k:], A[:-k] * (1.0 - (k - 1) / d))
    expo = np.clip(A * (1.0 - above), 0, 1)
    # Inside dense crowns: the top of every clump, where density falls off upwards
    Ab = cv2.GaussianBlur(A, (0, 0), max(1.0, d * 0.5))
    up = np.zeros_like(Ab)
    up[d:] = Ab[:-d]
    expo = np.maximum(expo, np.clip((Ab - up) * 3.0, 0, 1) * A)
    rng = np.random.default_rng(seed)
    nz = cv2.GaussianBlur(rng.random((H, W)).astype(np.float32), (0, 0), max(1.0, d * 0.35))
    nz = (nz - nz.min()) / max(float(np.ptp(nz)), 1e-6)
    sn = cv2.GaussianBlur(expo, (0, 0), max(0.8, d * 0.18)) * (0.65 + 0.7 * nz) * amount
    sn = np.clip(sn * 1.6, 0, 1) * A
    out = rgb.astype(np.float32)
    snowc = np.array(col, np.float32)
    lum = out.mean(2, keepdims=True)
    out = out * (1 - frost) + (lum * 0.35 + snowc * 0.65) * frost * A[..., None]
    out = out * (1 - sn[..., None]) + snowc * sn[..., None]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def dangles(rgb, a, pts, scale, length_rel, width_rel, col, rng, hang=True, count=(2, 4), jitter=25):
    """Catkins and similar: short soft capsules drawn at the given (normalised) points."""
    H, W = a.shape
    S = max(H, W)
    ss = 4
    layer = np.zeros((H * ss, W * ss), np.uint8)
    for p in pts:
        for _ in range(int(rng.integers(count[0], count[1] + 1))):
            L = length_rel * S * rng.uniform(0.7, 1.25) * ss
            w = max(1.0, width_rel * S * rng.uniform(0.8, 1.2) * ss)
            ang = (90 if hang else -90) + rng.normal(0, jitter)
            cx = p[0] * scale * ss + rng.normal(0, 0.004 * S * ss)
            cy = p[1] * scale * ss + rng.normal(0, 0.004 * S * ss)
            ex, ey = cx + math.cos(math.radians(ang)) * L * 0.5, cy + math.sin(math.radians(ang)) * L * 0.5
            cv2.ellipse(layer, (int(ex), int(ey)), (int(L * 0.5), int(w * 0.5)), ang, 0, 360, 255, -1, cv2.LINE_AA)
    m = cv2.resize(layer.astype(np.float32) / 255.0, (W, H), interpolation=cv2.INTER_AREA)
    tex = 0.8 + 0.4 * cv2.GaussianBlur(rng.random((H, W)).astype(np.float32), (0, 0), 0.8)
    c = np.array(col, np.float32)[None, None, :] * tex[..., None]
    out = rgb.astype(np.float32) * (1 - m[..., None]) + c * m[..., None]
    na = np.maximum(a.astype(np.float32), m * 255.0)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8), np.clip(na + 0.5, 0, 255).astype(np.uint8)


def tint_near(rgb, a, pts, scale, radius_rel, col, strength, fol=None):
    """Recolour foliage around points (new conifer shoots, blossom on sprigs)."""
    H, W = a.shape
    S = max(H, W)
    m = np.zeros((H, W), np.float32)
    r = max(1, int(radius_rel * S))
    for p in pts:
        cv2.circle(m, (int(p[0] * scale), int(p[1] * scale)), r, 1.0, -1, cv2.LINE_AA)
    m = cv2.GaussianBlur(m, (0, 0), r * 0.4) * (a.astype(np.float32) / 255.0)
    if fol is not None:
        m *= fol
    m = np.clip(m * strength, 0, 1)
    out = rgb.astype(np.float32) * (1 - m[..., None]) + np.array(col, np.float32) * (0.75 + 0.25 * rgb.astype(np.float32) / 255.0) * m[..., None]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)
