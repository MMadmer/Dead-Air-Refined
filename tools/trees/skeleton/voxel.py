# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Fallback skeleton for wood the ring/level-set decomposition cannot read (welded or self-intersecting pieces,
sculpted blobs): voxelise the piece (1-4 cm, at most ~4M cells), close it into a solid proxy (surface lattice,
1-cell closing, exterior flood fill), thin it by geodesic level sets from the root (hop distance through the solid,
slabs of about one local radius, connected pieces of each slab = skeleton nodes), and hand the node centres back
as stations: the station stage cuts the stock mesh there and snaps each ring onto the stock section's centroid."""
import collections

import numpy as np

import crown as CR

OFF6 = np.array([(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)], np.int64)


class Solid:
    def __init__(self, lo, h, dims):
        self.lo = lo
        self.h = h
        self.dims = dims


OFF26 = np.array([(a, b, c) for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1) if (a, b, c) != (0, 0, 0)],
                 np.int64)


def frontier_bfs(free, seeds_flat, octagonal=False):
    """Hop distance through the cells where `free` is True from the seed cells (flat indices); -1 where
    unreached. 6-connected, or alternating 6/26-connected steps (octagonal metric, closer to euclidean fronts).
    Work is proportional to the cells reached, not to grid x diameter."""
    shape = free.shape
    flat = free.ravel()
    strides = np.array([shape[1] * shape[2], shape[2], 1], np.int64)
    dist = np.full(flat.size, -1, np.int32)
    fr = np.unique(np.asarray(seeds_flat, np.int64))
    fr = fr[flat[fr]]
    dist[fr] = 0
    d = 0
    while len(fr):
        coords = np.stack(np.unravel_index(fr, shape), 1)
        offs = OFF26 if (octagonal and d % 2 == 1) else OFF6
        cand = []
        for off in offs:
            c = coords + off
            ok = np.all((c >= 0) & (c < shape), axis=1)
            cand.append(fr[ok] + int(off @ strides))
        nb = np.unique(np.concatenate(cand))
        nb = nb[flat[nb] & (dist[nb] < 0)]
        d += 1
        dist[nb] = d
        fr = nb
    return dist.reshape(shape)


def border_flat(shape):
    m = np.zeros(shape, bool)
    m[0], m[-1] = True, True
    m[:, 0], m[:, -1] = True, True
    m[:, :, 0], m[:, :, -1] = True, True
    return np.flatnonzero(m.ravel())


def voxelise(V, F, max_cells=4_000_000):
    lo = V[F.ravel()].min(0)
    hi = V[F.ravel()].max(0)
    ext = hi - lo
    h = float(np.clip((max(ext.prod(), 1e-9) / max_cells) ** (1 / 3), 0.01, 0.04))
    h = max(h, float(ext.max()) / 400.0)
    lo = lo - 3 * h
    dims = np.ceil((hi - lo) / h).astype(np.int64) + 4
    sv = Solid(lo, h, dims)
    pts = CR.lattice_points(V, F, h / 2.5)
    idx = np.floor((pts - lo) / h).astype(np.int64)
    idx = np.clip(idx, 0, dims - 1)
    surf = np.zeros(tuple(dims), bool)
    surf[idx[:, 0], idx[:, 1], idx[:, 2]] = True
    shell = CR.dilate(surf, 1)
    ext_ = frontier_bfs(~shell, border_flat(shell.shape)) >= 0
    inside = ~ext_
    solid = CR.erode(inside, 1) | surf
    return sv, solid


def bfs_solid(solid, seeds, octagonal=True):
    """Hop distance through the solid from the seed cells (k, 3); -1 where unreached."""
    seeds = np.asarray(seeds, np.int64).reshape(-1, 3)
    flat = np.ravel_multi_index(seeds.T, solid.shape)
    return frontier_bfs(solid, flat, octagonal)


def label_slab(cells, dims):
    """Connected pieces (6-connected) of a set of cells (k, 3): label per cell."""
    key = (cells[:, 0] * dims[1] + cells[:, 1]) * dims[2] + cells[:, 2]
    o = np.argsort(key)
    ks = key[o]
    par = np.arange(len(cells))

    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]
            a = par[a]
        return a
    for off in OFF6[::2]:
        nb = cells + off
        nk = (nb[:, 0] * dims[1] + nb[:, 1]) * dims[2] + nb[:, 2]
        pos = np.searchsorted(ks, nk)
        pos = np.minimum(pos, len(ks) - 1)
        hit = ks[pos] == nk
        for a, b in zip(np.flatnonzero(hit), o[pos[hit]]):
            ra, rb = find(a), find(b)
            if ra != rb:
                par[max(ra, rb)] = min(ra, rb)
    return np.array([find(a) for a in range(len(cells))])


def skeleton(V, F, root_pts, r_est=None):
    """Skeleton nodes of one wood piece: list of (centre (3,), radius estimate, slab, parent node index)."""
    sv, solid = voxelise(V, F)
    if not solid.any():
        return []
    seeds = np.floor((root_pts - sv.lo) / sv.h).astype(np.int64)
    seeds = np.clip(seeds, 0, np.array(solid.shape) - 1)
    # seeds must lie in the solid: the nearest solid cells to the root points
    cells = np.argwhere(solid)
    if not solid[tuple(seeds.T)].any():
        cen = root_pts.mean(0)
        cc = sv.lo + (cells + 0.5) * sv.h
        k = np.argsort(np.linalg.norm(cc - cen, axis=1))[:max(1, len(root_pts))]
        seeds = cells[k]
    dist = bfs_solid(solid, seeds)
    ok = dist >= 0
    if not ok.any():
        return []
    # slab thickness: about one local radius in cells (solid depth from the surface gives the radius)
    depth = bfs_solid(solid, np.argwhere(solid & ~CR.erode(solid, 1)))
    r_cells = max(3, int(np.percentile(depth[solid], 75))) if r_est is None else max(3, int(r_est / sv.h))
    slab = np.maximum(dist // max(3, r_cells), 0)
    nodes = []
    prev_lab = {}
    lab_grid = np.full(solid.shape, -1, np.int64)
    for s in range(int(slab[ok].max()) + 1):
        cs = np.argwhere(ok & (slab == s))
        if not len(cs):
            continue
        lab = label_slab(cs, solid.shape)
        groups = [(g, lab == g) for g in np.unique(lab)]
        big = max(int(m.sum()) for _, m in groups)
        for g, m in groups:
            pc = cs[m]
            if len(pc) < max(8, 0.04 * big) and s > 0:
                # a crumb of the slab (surface noise): its cells join the touching node of the previous slab
                for off in OFF6:
                    nb = np.clip(pc + off, 0, np.array(solid.shape) - 1)
                    ln = lab_grid[tuple(nb.T)]
                    ln = ln[ln >= 0]
                    if len(ln):
                        lab_grid[tuple(pc.T)] = int(ln[0])
                        break
                continue
            centre = sv.lo + (pc.mean(0) + 0.5) * sv.h
            rad = float(depth[tuple(pc.T)].max()) * sv.h
            nid = len(nodes)
            # parent: the node of the previous slab touching this piece most
            par = -1
            if s > 0:
                votes = {}
                for off in OFF6:
                    nb = np.clip(pc + off, 0, np.array(solid.shape) - 1)
                    ln = lab_grid[tuple(nb.T)]
                    for x in ln[ln >= 0]:
                        if nodes[x][2] == s - 1:
                            votes[int(x)] = votes.get(int(x), 0) + 1
                if votes:
                    par = max(votes, key=votes.get)
            lab_grid[tuple(pc.T)] = nid
            nodes.append((centre, rad, s, par, len(pc)))
    return nodes


def axes_from_nodes(nodes):
    """Chains of skeleton nodes: (list of node indices, parent chain index, attach node) per axis, the
    continuation at a split following the larger subtree."""
    ch = collections.defaultdict(list)
    for i, n in enumerate(nodes):
        if n[3] >= 0:
            ch[n[3]].append(i)
    size = np.array([n[4] for n in nodes], np.float64)
    for i in sorted(range(len(nodes)), key=lambda i: -nodes[i][2]):
        if nodes[i][3] >= 0:
            size[nodes[i][3]] += size[i]
    roots = [i for i, n in enumerate(nodes) if n[3] < 0]
    out = []
    stack = [(r, -1, -1) for r in roots]
    while stack:
        nid, pchain, att = stack.pop()
        chain = []
        cur = nid
        while True:
            chain.append(cur)
            kids = sorted(ch[cur], key=lambda k: -size[k])
            if not kids:
                break
            for k in kids[1:]:
                stack.append((k, len(out), cur))
            cur = kids[0]
        out.append((chain, pchain, att))
    # side chains of a single node are bumps, not branches
    keep = [i for i, (ch_, p, a) in enumerate(out) if p < 0 or len(ch_) >= 2]
    remap = {old: new_ for new_, old in enumerate(keep)}
    return [(out[i][0], remap.get(out[i][1], -1) if out[i][1] >= 0 else -1, out[i][2]) for i in keep]
