# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Topology of the stock wood: welded bark mesh -> connected components -> roots and parents -> hop levels from each
root -> level-set nodes (rings, or fragments at junctions) -> axes (level 0 trunks, level 1 modelled limbs).

Stock tubes are n-gon rings: the hop distance from a root ring is the ring index along a clean tube, so a level set
of one component is one ring. Forks split a level set into several components (new axes); welded side branches get
slanted fronts, which the station stage (stations.py) re-slices geometrically."""
import collections

import numpy as np

import geom as G


class Wood:
    """The bark parts of a kind as one welded mesh (caps stay separate: they are kept as patches)."""

    def __init__(self, parts):
        P, N, UV, part, F = [], [], [], [], []
        off = 0
        for p in parts:
            P.append(p.P)
            N.append(p.N)
            UV.append(p.uv)
            part.append(np.full(len(p.P), p.index))
            F.append(p.T + off)
            off += len(p.P)
        self.P = np.concatenate(P) if P else np.zeros((0, 3))
        self.N = np.concatenate(N) if N else np.zeros((0, 3))
        self.UV = np.concatenate(UV) if UV else np.zeros((0, 2))
        self.part = np.concatenate(part) if part else np.zeros(0, np.int64)
        self.Fraw = np.concatenate(F) if F else np.zeros((0, 3), np.int64)
        self.wid, self.rep = G.weld(self.P) if len(self.P) else (np.zeros(0, np.int64), np.zeros(0, np.int64))
        V = self.P[self.rep] if len(self.P) else np.zeros((0, 3))
        Fw = self.wid[self.Fraw] if len(self.Fraw) else np.zeros((0, 3), np.int64)
        keep = (Fw[:, 0] != Fw[:, 1]) & (Fw[:, 1] != Fw[:, 2]) & (Fw[:, 0] != Fw[:, 2]) if len(Fw) else np.zeros(0, bool)
        # Faces with zero area are dropped too (they carry no surface)
        if len(Fw):
            nrm, area = G.tri_normals(V, Fw)
            keep &= area > 1e-10
        self.face_raw = np.flatnonzero(keep)
        self.topo = G.Topo(V, Fw[keep])
        self.V = V


class Node:
    __slots__ = ('id', 'comp', 'level', 'verts', 'centroid', 'clean', 'parent', 'children', 'size', 'axis')

    def __init__(self, nid, comp, level, verts, V):
        self.id = nid
        self.comp = comp
        self.level = level
        self.verts = verts
        self.centroid = V[verts].mean(0)
        self.clean = False
        self.parent = -1
        self.children = []
        self.size = len(verts)
        self.axis = -1


class Axis:
    def __init__(self, aid, comp, parent, level, nodes):
        self.id = aid
        self.comp = comp
        self.parent = parent           # axis id or -1
        self.level = level             # 0 trunk, 1 modelled limb
        self.nodes = nodes             # node ids from the base up
        self.attach_node = -1          # node of the parent axis it leaves from
        self.attach_point = None
        self.merged_from = []          # components appended (continuations)
        self.verts = None
        self.children = []


def component_roots(wood, ground_band):
    """Root vertex set of every component, its parent component and the attachment distance."""
    T = wood.topo
    V = T.V
    used = T.used
    comps = [np.flatnonzero((T.comp == c) & used) for c in range(T.ncomp)]
    loops_of = collections.defaultdict(list)
    for lp in T.loops:
        loops_of[T.comp[lp[0]]].append(lp)
    fc = T.comp[T.F[:, 0]] if len(T.F) else np.zeros(0, np.int64)
    ymin = V[used, 1].min()
    grids = {}

    def grid(c):
        if c not in grids:
            grids[c] = G.TriGrid(V, T.F[fc == c], reach=1.0, cell=0.5)
        return grids[c]

    boxes = {c: (V[vs].min(0), V[vs].max(0)) for c, vs in enumerate(comps) if len(vs)}
    info = {}
    for c, vs in enumerate(comps):
        if len(vs) == 0:
            continue
        loops = loops_of.get(c, [])
        cands = []
        for lp in loops:
            cands.append(('loop', lp, V[lp].mean(0)))
        if not loops:
            # Closed component: its two extremities along the principal axis
            X = V[vs] - V[vs].mean(0)
            w, U = np.linalg.eigh(X.T @ X)
            ax = U[:, -1]
            pr = X @ ax
            k = max(1, len(vs) // 20)
            o = np.argsort(pr)
            for sel in (o[:k], o[-k:]):
                cands.append(('ext', vs[sel], V[vs[sel]].mean(0)))
        low = V[vs, 1].min()
        is_ground = low <= ymin + ground_band
        best = None
        if is_ground:
            # every boundary loop in the ground band (a flared base stands on several root ends), else the lowest
            # vertices
            gl = [cd for cd in cands if cd[0] == 'loop' and cd[2][1] <= ymin + ground_band + 0.05]
            if gl:
                best = (np.unique(np.concatenate([cd[1] for cd in gl])), -1, 0.0)
            else:
                lowv = vs[V[vs, 1] <= low + max(0.03, 0.01 * (V[vs, 1].max() - low))]
                best = (lowv, -1, 0.0)
            info[c] = {'root': best[0], 'parent': -1, 'dist': 0.0, 'ground': True, 'root_loops': len(gl)}
            continue
        # limb: the candidate end nearest to the surface of a component at least as large (limbs of one whorl
        # touch each other inside the trunk; they hang on the trunk, not on each other)
        for kind, verts, cen in cands:
            for o, (bmin, bmax) in boxes.items():
                if o == c or len(comps[o]) < len(vs):
                    continue
                if np.any(cen < bmin - 1.0) or np.any(cen > bmax + 1.0):
                    continue
                d, _, _ = grid(o).query(cen[None])
                if best is None or d[0] < best[2]:
                    best = (verts, o, float(d[0]))
        if best is None or not np.isfinite(best[2]):
            # nothing within reach: the lowest candidate, parent = nearest component by box distance
            cd = min(cands, key=lambda cd: cd[2][1])
            far = min(((np.linalg.norm(np.maximum(0, np.maximum(bmin - cd[2], cd[2] - bmax))), o)
                       for o, (bmin, bmax) in boxes.items() if o != c), default=(np.inf, -1))
            best = (cd[1], far[1], float(far[0]))
        info[c] = {'root': np.asarray(best[0], np.int64), 'parent': best[1], 'dist': best[2], 'ground': False}
    # break parent cycles: walk up from each component; a component not reaching a ground root hangs on the
    # largest member of its cycle, which becomes a root
    for c in list(info):
        seen = []
        x = c
        while x != -1 and x not in seen:
            seen.append(x)
            x = info[x]['parent']
        if x != -1:
            cyc = seen[seen.index(x):]
            big = max(cyc, key=lambda q: len(comps[q]))
            info[big]['parent'] = -1
            info[big]['ground'] = False
    return comps, info


def level_nodes(wood, comp_verts, root, mask):
    """Hop levels from `root` over one component and the level-set nodes (connected pieces of each level)."""
    T = wood.topo
    lev = G.bfs_levels(T, root, mask)
    vs = comp_verts[lev[comp_verts] >= 0]
    # union vertices joined by an edge inside one level
    E = T.E
    same = (lev[E[:, 0]] == lev[E[:, 1]]) & (lev[E[:, 0]] >= 0) & mask[E[:, 0]] & mask[E[:, 1]]
    par = {int(v): int(v) for v in vs}

    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]
            a = par[a]
        return a
    for a, b in E[same]:
        ra, rb = find(int(a)), find(int(b))
        if ra != rb:
            par[max(ra, rb)] = min(ra, rb)
    groups = collections.defaultdict(list)
    for v in vs:
        groups[find(int(v))].append(int(v))
    return lev, groups


def build(wood, ground_band=None):
    """Nodes and axes of the stock wood. Returns (nodes, axes, info)."""
    T = wood.topo
    V = T.V
    if T.nv == 0 or not T.used.any():
        return [], [], {}
    H = V[T.used, 1].max() - V[T.used, 1].min()
    if ground_band is None:
        ground_band = max(0.3, 0.03 * H)
    comps, info = component_roots(wood, ground_band)
    nodes = []
    node_of = np.full(T.nv, -1, np.int64)
    comp_root_node = {}
    for c, vs in enumerate(comps):
        if c not in info or len(vs) < 3:
            continue
        mask = np.zeros(T.nv, bool)
        mask[vs] = True
        lev, groups = level_nodes(wood, vs, info[c]['root'], mask)
        # level 0 is one node (the root set)
        l0 = [g for r, g in groups.items() if lev[r] == 0]
        merged0 = sorted(v for g in l0 for v in g)
        items = [(0, merged0)] + sorted(((int(lev[r]), g) for r, g in groups.items() if lev[r] > 0),
                                        key=lambda t: (t[0], t[1][0]))
        first = len(nodes)
        for level, g in items:
            nd = Node(len(nodes), c, level, np.array(sorted(g), np.int64), V)
            node_of[nd.verts] = nd.id
            nodes.append(nd)
        comp_root_node[c] = first
        # cycle test: every vertex has exactly two neighbours inside its node
        for nd in nodes[first:]:
            if len(nd.verts) < 3:
                continue
            ok = True
            for v in nd.verts:
                nb = T.neighbours(v)
                if np.count_nonzero(node_of[nb] == nd.id) != 2:
                    ok = False
                    break
            nd.clean = ok
    # node DAG: edges between consecutive levels; parent = the lower node sharing most edges
    E = T.E
    na, nb = node_of[E[:, 0]], node_of[E[:, 1]]
    ok = (na >= 0) & (nb >= 0) & (na != nb)
    links = collections.Counter()
    for a, b in zip(na[ok], nb[ok]):
        A, B = nodes[a], nodes[b]
        if A.comp != B.comp:
            continue
        if A.level + 1 == B.level:
            links[(a, b)] += 1
        elif B.level + 1 == A.level:
            links[(b, a)] += 1
    n_parents = collections.Counter(b for (a, b) in links)
    best_parent = {}
    for (a, b), n in links.items():
        if b not in best_parent or n > best_parent[b][1]:
            best_parent[b] = (a, n)
    for b, (a, n) in best_parent.items():
        nodes[b].parent = a
        nodes[a].children.append(b)
    # subtree sizes (levels descend, so a reverse sweep by level works)
    for nd in sorted(nodes, key=lambda n: -n.level):
        if nd.parent >= 0:
            nodes[nd.parent].size += nd.size
    # axes per component, components in parent order
    axes = []
    order = []
    pending = [c for c in comp_root_node if info[c]['parent'] == -1]
    placed = set()
    while pending:
        c = pending.pop(0)
        if c in placed:
            continue
        placed.add(c)
        order.append(c)
        pending += [k for k in comp_root_node if info[k]['parent'] == c and k not in placed]
    order += [c for c in comp_root_node if c not in placed]
    for c in order:
        start = comp_root_node[c]
        if info[c].get('root_loops', 0) > 1:
            # several root ends: the trunk starts where their fronts merge (the lowest node with several parents
            # that carries a good part of the piece); the root fingers below stay stock (patches)
            nverts = len(comps[c])
            cand = [nd for nd in nodes if nd.comp == c and nd.level > 0 and n_parents[nd.id] >= 2
                    and nd.size >= 0.3 * nverts]
            if cand:
                m = min(cand, key=lambda nd: nd.level)
                start = m.id
                nodes[start].parent = -1
        stack = [(start, -1, -1)]
        while stack:
            nid, parent_axis, attach = stack.pop()
            ax = Axis(len(axes), c, parent_axis, 0, [])
            ax.attach_node = attach
            cur = nid
            while True:
                ax.nodes.append(cur)
                nodes[cur].axis = ax.id
                ch = sorted(nodes[cur].children, key=lambda k: -nodes[k].size)
                if not ch:
                    break
                for k in ch[1:]:
                    stack.append((k, ax.id, cur))
                cur = ch[0]
            axes.append(ax)
    for ax in axes:
        ax.verts = np.concatenate([nodes[n].verts for n in ax.nodes])
    return nodes, axes, {'comps': comps, 'info': info, 'node_of': node_of, 'ground_band': ground_band}
