# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""The census of the stock level trees for the 1.6.0 tree upgrade (docs/dead-air/TREES.md, TREE_DATA.md): writes
tools/trees/data/tree_kinds.json and tools/trees/data/tree_photo_colours.json, which tools/trees/make_trees.py reads.

Inputs (read-only):
  --levels     the compiled levels (level + level.geom), keyed here exactly as the engine keys a placed model
               (da_shrubs.cpp read_part_keys / kind_of): every FLOD of tree parts under a sector root, its parts'
               content keys, the kind key, the tolerant family signature, its placed top and scale
  --inventory  the tree inventory (inventory.json, meshes/<key>/<key>.json): the families of the tree set, their
               category, state, species hints from the card textures, flags and model sizes
  --species    the species research (tree_colours.json): the measured monthly colours, copied compactly
  --shrubs     dead_air_x64_shrubs.ltx: the key parity check (every [shrub_kinds] key and [shrub_families] row must
               come out of the same code)
  --log        a game log: the engine's own shrub census of the levels it loaded, recomputed here

    python tools/trees/census_trees.py [--levels DIR] [--inventory DIR] [--species DIR]

The skeleton id of a kind is the content key of its wood (the flora\\trunk_wave parts: bark and cut ends): one part's
key as it is, several hashed together like a kind key (FNV-1a 64 over the sorted keys as little-endian u64). Kinds
without wood and the oversize bushes grown as young trees get `p<kind key>` (procedural). The other plausible
readings are kept as `skel_candidates`; make_trees.py takes the first one that has a .dtsk file once the skeletons
exist.
"""
import argparse
import collections
import json
import math
import os
import re
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
DATA = os.path.join(HERE, 'data')
SCRATCH = r'C:\Users\admin\AppData\Local\Temp\claude\D--Games-Dead-Air-DeadAir-x64\f9b3463a-7c5c-40f8-a3bd-38013b895726\scratchpad\trees'
DEF_LEVELS = os.environ.get('DA_LEVELS', r'D:\Games\Dead-Air-AssetCompare\work\seasons\da_levels\levels')
DEF_INVENTORY = os.environ.get('DA_TREES_INVENTORY', os.path.join(SCRATCH, 'inventory'))
DEF_SPECIES = os.environ.get('DA_TREES_SPECIES', os.path.join(SCRATCH, 'species'))
SHRUBS_LTX = os.path.join(REPO, 'packaging', 'dead-air-x64', 'compatibility', 'gamedata', 'configs',
                          'dead_air_x64_shrubs.ltx')
DEF_LOG = os.environ.get('DA_GAME_LOG', r'D:\Games\Dead Air\appdata\logs\grizzly_admin.log')
OUT_KINDS = os.path.join(DATA, 'tree_kinds.json')
OUT_COLOURS = os.path.join(DATA, 'tree_photo_colours.json')

# ======================================================================================================
# The engine's keys (da_shrubs.cpp read_part_keys, kind_of)
# ======================================================================================================
FNV_OFF = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK = (1 << 64) - 1


def fnv(data, h=FNV_OFF):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def fnv_rows(rows):
    """FNV-1a 64 of every row of an (n, k) uint8 array."""
    h = np.full(len(rows), FNV_OFF, np.uint64)
    p = np.uint64(FNV_PRIME)
    with np.errstate(over='ignore'):
        for c in range(rows.shape[1]):
            h = (h ^ rows[:, c].astype(np.uint64)) * p
    return h


def shader_bytes(shader):
    # (tolower in the C locale: ASCII letters only, as the engine's fnv_shader)
    return shader.encode('cp1251', 'replace').lower() + b'\0'


def part_key(shader, P, tc, tris, vcount):
    """A part's key: its vertices as millimetre positions and uv wrapped into one tile, its triangles by their sorted
    vertex hashes summed (a triangle with an index past the vertex range is skipped, its count stays), then the lower-
    case shader string, the triangle count and the sum."""
    rec = np.empty(len(P), dtype=[('p', '<i4', 3), ('t', '<i2', 2)])
    rec['p'] = np.floor(P.astype(np.float64) * 1000.0 + 0.5).astype('<i4')
    rec['t'] = np.mod(tc.astype(np.int64), 2048).astype('<i2')
    ok = (tris < vcount).all(axis=1)
    with np.errstate(over='ignore'):
        hv = fnv_rows(np.frombuffer(rec.tobytes(), np.uint8).reshape(len(P), 16))
        t = np.sort(hv[tris[ok]], axis=1).astype('<u8')
        ht = fnv_rows(np.frombuffer(t.tobytes(), np.uint8).reshape(len(t), 24))
        s = int(ht.sum(dtype=np.uint64))
    return fnv(struct.pack('<IQ', len(tris), s), fnv(shader_bytes(shader)))


def kind_key(keys):
    return fnv(b''.join(struct.pack('<Q', k) for k in sorted(keys)))


def signature_hash(parts):
    """The tolerant signature's hash: the parts sorted by shader (case-insensitive, stable), FNV-1a 64 over each
    one's lower-case shader string with its zero and its u32 triangle count."""
    h = FNV_OFF
    for p in sorted(parts, key=lambda q: shader_bytes(q['shader'])):
        h = fnv(shader_bytes(p['shader']) + struct.pack('<I', p['tris']), h)
    return h


def family_row(key, parts):
    """A [shrub_families] row: the kind key, the box of all parts in mm (each part's box rounded to the millimetre,
    as the shrubs' census wrote them) and their area (each part's to 1e-4 m2, summed)."""
    bmin = [min(int(round(p['bmin'][i] * 1000.0)) for p in parts) for i in range(3)]
    bmax = [max(int(round(p['bmax'][i] * 1000.0)) for p in parts) for i in range(3)]
    area = sum(round(p['area'], 4) for p in parts)
    return '%016x:%d:%d:%d:%d:%d:%d:%.4f' % (key, *bmin, *bmax, area)


# ======================================================================================================
# The compiled level (level, level.geom)
# ======================================================================================================
MT_HIERRARHY, MT_LOD, MT_TREE_ST, MT_TREE_PM = 1, 6, 7, 11
OGF_HEADER, OGF_CHILDREN_L, OGF_TREEDEF2, OGF_SWICONTAINER, OGF_GCONTAINER = 1, 10, 12, 20, 21
DECL_SIZE = {0: 4, 1: 8, 2: 12, 3: 16, 4: 4, 5: 4, 6: 4, 7: 8, 8: 4, 9: 4, 10: 8, 11: 4, 12: 8, 13: 4, 14: 4, 15: 4,
             16: 8}
POSITION, TEXCOORD, FLOAT3, SHORT2, SHORT4 = 0, 5, 2, 6, 7


def chunks(buf, off, end):
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, off)
        out.append((cid & 0x7FFFFFFF, off + 8, size))
        off += 8 + size
    return out


class Level:
    def __init__(self, folder):
        self.name = os.path.basename(folder.rstrip('/\\'))
        b = open(os.path.join(folder, 'level'), 'rb').read()
        top = {cid: (o, s) for cid, o, s in chunks(b, 0, len(b))}
        o, s = top[2]
        n = struct.unpack_from('<I', b, o)[0]
        p = o + 4
        self.shaders = []
        for _ in range(n):
            q = b.index(b'\0', p)
            self.shaders.append(b[p:q].decode('cp1251', 'replace'))
            p = q + 1
        o, s = top[3]
        self.visuals = [self._visual(b, vo, vs) for _, vo, vs in chunks(b, o, o + s)]
        self.roots = []
        if 8 in top:
            o, s = top[8]
            for _, so, ss in chunks(b, o, o + s):
                for cid, co, _ in chunks(b, so, so + ss):
                    if cid == 2:
                        self.roots.append(struct.unpack_from('<I', b, co)[0])
        self.geom = open(os.path.join(folder, 'level.geom'), 'rb').read()
        g = self.geom
        gt = {cid: (o, s) for cid, o, s in chunks(g, 0, len(g))}
        self.vbs = self._vbs(g, gt[9][0])
        self.ibs = self._ibs(g, gt[10][0])
        self.swis = self._swis(g, gt[11][0]) if 11 in gt else []

    @staticmethod
    def _visual(b, o, s):
        v = {}
        for cid, co, cs in chunks(b, o, o + s):
            if cid == OGF_HEADER:
                _, v['type'], v['shader'] = struct.unpack_from('<BBH', b, co)
                v['box'] = struct.unpack_from('<6f', b, co + 4)
            elif cid == OGF_CHILDREN_L:
                n = struct.unpack_from('<I', b, co)[0]
                v['children'] = struct.unpack_from('<%dI' % n, b, co + 4)
            elif cid == OGF_GCONTAINER:
                v['gc'] = struct.unpack_from('<6I', b, co)
            elif cid == OGF_SWICONTAINER:
                v['swi'] = struct.unpack_from('<I', b, co)[0]
            elif cid == OGF_TREEDEF2:
                v['xform'] = struct.unpack_from('<16f', b, co)
        return v

    @staticmethod
    def _vbs(g, o):
        n = struct.unpack_from('<I', g, o)[0]
        p = o + 4
        out = []
        for _ in range(n):
            pos = tc = -1
            stride = 0
            while True:
                stream, off, typ, _m, usage, uidx = struct.unpack_from('<HHBBBB', g, p)
                p += 8
                if stream == 0xFF:
                    break
                stride = max(stride, off + DECL_SIZE.get(typ, 0))
                if usage == POSITION and typ == FLOAT3:
                    pos = off
                if usage == TEXCOORD and uidx == 0 and typ in (SHORT2, SHORT4):
                    tc = off
            cnt = struct.unpack_from('<I', g, p)[0]
            p += 4
            out.append((p, cnt, stride, pos, tc))
            p += cnt * stride
        return out

    @staticmethod
    def _ibs(g, o):
        n = struct.unpack_from('<I', g, o)[0]
        p = o + 4
        out = []
        for _ in range(n):
            cnt = struct.unpack_from('<I', g, p)[0]
            out.append((p + 4, cnt))
            p += 4 + cnt * 2
        return out

    @staticmethod
    def _swis(g, o):
        n = struct.unpack_from('<I', g, o)[0]
        p = o + 4
        out = []
        for _ in range(n):
            p += 16
            cnt = struct.unpack_from('<I', g, p)[0]
            p += 4
            out.append([struct.unpack_from('<IHH', g, p + 8 * k) for k in range(cnt)])
            p += 8 * cnt
        return out

    def tree_flods(self):
        """The candidates of collect_placements: every MT_LOD child of a hierarchy under a sector root whose own
        children are all tree parts (FLOD index, part indices in their order)."""
        V = self.visuals
        out = []
        stack = [r for r in self.roots if r < len(V) and V[r].get('type') == MT_HIERRARHY]
        while stack:
            h = stack.pop()
            for c in V[h].get('children', ()):
                t = V[c].get('type')
                if t == MT_LOD:
                    ch = V[c].get('children', ())
                    if ch and all(V[x].get('type') in (MT_TREE_ST, MT_TREE_PM) and 'gc' in V[x] for x in ch):
                        out.append((c, list(ch)))
                elif t == MT_HIERRARHY:
                    stack.append(c)
        return out

    def part(self, v):
        """One tree part as read_part_keys reads it: key, shader, triangles of the full-detail window, the box over
        the whole vertex range (the shrubs' census rows), the window's own box and the triangles' area."""
        vb_i, v_base, v_cnt, ib_i, i_base, i_cnt = v['gc']
        if v['type'] == MT_TREE_PM:
            if 'swi' not in v or v['swi'] >= len(self.swis) or not self.swis[v['swi']]:
                return None
            off, ntris, nverts = self.swis[v['swi']][0]
            start, tris, vcount = i_base + off, ntris, nverts
        else:
            start, tris, vcount = i_base, i_cnt // 3, v_cnt
        voff, vn, stride, pos, tc = self.vbs[vb_i]
        ioff, icnt = self.ibs[ib_i]
        shader = self.shaders[v['shader']] if v['shader'] < len(self.shaders) else ''
        if pos < 0 or tc < 0 or v_base + max(vcount, v_cnt) > vn or start + tris * 3 > icnt:
            return {'key': 0, 'shader': shader, 'tris': tris, 'bmin': (0.0,) * 3, 'bmax': (0.0,) * 3, 'area': 0.0,
                    'vcount': vcount, 'vrange': v_cnt, 'wbmin': (0.0,) * 3, 'wbmax': (0.0,) * 3}
        n = max(vcount, v_cnt)
        raw = np.frombuffer(self.geom, np.uint8, n * stride, voff + v_base * stride).reshape(n, stride)
        P = raw[:, pos:pos + 12].copy().view('<f4').reshape(n, 3)
        T = raw[:, tc:tc + 4].copy().view('<i2').reshape(n, 2)
        idx = np.frombuffer(self.geom, '<u2', tris * 3, ioff + start * 2).astype(np.int64).reshape(-1, 3)
        key = part_key(shader, P[:vcount], T[:vcount], idx, vcount)
        ok = (idx < vcount).all(axis=1)
        Pd = P.astype(np.float64)
        Q = Pd[idx[ok]]
        area = float(0.5 * np.linalg.norm(np.cross(Q[:, 1] - Q[:, 0], Q[:, 2] - Q[:, 0]), axis=1).sum())
        A = Pd[:v_cnt]
        W = Pd[:vcount]
        return {'key': key, 'shader': shader, 'tris': tris, 'bmin': tuple(A.min(0)), 'bmax': tuple(A.max(0)),
                'area': area, 'vcount': vcount, 'vrange': v_cnt, 'wbmin': tuple(W.min(0)), 'wbmax': tuple(W.max(0))}


FLODS = {}


def survey(levels_dir, only=None):
    """Every tree FLOD of every level: kinds (their parts in the first placement's order) and placements; FLODS gets
    each level's count of tree FLODs (the engine's candidates)."""
    kinds = {}
    names = sorted(d for d in os.listdir(levels_dir) if os.path.isfile(os.path.join(levels_dir, d, 'level'))
                   and os.path.isfile(os.path.join(levels_dir, d, 'level.geom')))
    if only:
        names = [n for n in names if n in only]
    for name in names:
        t0 = time.time()
        L = Level(os.path.join(levels_dir, name))
        memo = {}
        flods = L.tree_flods()
        FLODS[name] = len(flods)
        for _, ch in flods:
            parts = []
            for c in ch:
                v = L.visuals[c]
                sk = (v['gc'], v.get('swi'), v['shader'])
                if sk not in memo:
                    memo[sk] = L.part(v)
                parts.append(memo[sk])
            if any(p is None for p in parts):
                continue
            kk = kind_key([p['key'] for p in parts])
            M = L.visuals[ch[0]].get('xform') or (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
            sy = math.sqrt(M[4] * M[4] + M[5] * M[5] + M[6] * M[6])
            tilt = math.degrees(math.acos(max(-1.0, min(1.0, M[5] / max(sy, 1e-12)))))
            top = max(L.visuals[c]['box'][4] for c in ch) - M[13]
            k = kinds.get(kk)
            if k is None:
                k = kinds[kk] = {'parts': parts, 'levels': collections.Counter(), 'placed': []}
            k['levels'][name] += 1
            k['placed'].append((name, top, sy, tilt))
        print('%-22s %6d visuals, %5d tree FLODs, %4d parts keyed, %.1f s' % (
            name, len(L.visuals), len(flods), len(memo), time.time() - t0), flush=True)
        del L
    return kinds


# ======================================================================================================
# The shipped shrub data (the parity check and the bush classes of the oversize bushes)
# ======================================================================================================
def read_ltx_sections(path):
    secs, cur = {}, None
    with open(path, encoding='cp1251') as f:
        for line in f:
            line = line.split(';', 1)[0].strip()
            if not line:
                continue
            m = re.match(r'^\[([^\]]+)\]', line)
            if m:
                cur = secs.setdefault(m.group(1), [])
                continue
            if cur is not None and '=' in line:
                k, v = line.split('=', 1)
                cur.append((k.strip(), v.strip()))
    return secs


# ======================================================================================================
# The tree set (the inventory) and its census
# ======================================================================================================
# Card and bark texture -> the species hint (the inventory's classify_trees.SPECIES_RULES).
SPECIES_RULES = [('sosnodub', 'pine'), ('atp_birch', 'birch'), ('bereza', 'birch'), ('atp_alder', 'alder'),
                 ('atp_oak', 'oak'), ('trees_dub', 'oak'), ('atp_poplar', 'poplar'), ('topol', 'poplar'),
                 ('atp_willow', 'willow'), ('atp_cherry', 'cherry'), ('atp_pine', 'pine'), ('sosna', 'pine'),
                 ('atp_spruce', 'spruce'), ('elka', 'spruce'), ('orex', 'walnut')]
TREE_SET = ('TREE', 'BUSH_KEPT_STOCK', 'FALLEN', 'LOG', 'STUMP', 'DEBRIS')
# Stay stock (CONTRACT 2): the lying and dead-wood categories, leafy or not. Every standing tree is upgraded, the
# anomaly-sculpted and the charred ones too (the skeletons keep the stock triangles where the rings miss the sculpting).
STOCK_CATEGORIES = ('FALLEN', 'LOG', 'STUMP', 'DEBRIS')
# A standing anomaly's or burnt tree's crown is alive when its main cards carry foliage (green, dried or the Red
# Forest's red leaves); bare twigs, dead cards or no cards at all make it a dead tree.
LIVE_CARDS = ('leafy', 'dry', 'red')


def tex_of(shader):
    t = shader.split('/', 1)[1].split(',')[0] if '/' in shader else ''
    return t.split('\\')[-1].lower()


def hint_of(name):
    for k, sp in SPECIES_RULES:
        if k in name:
            return sp
    return None


def skeleton_ids(key, sidecar_parts):
    """The skeleton id of a kind and the other readings of the rule (see the module notes)."""
    wood = [p for p in sidecar_parts if p['blender'].lower() == 'flora\\trunk_wave']
    bark = [p for p in wood if p['role'] != 'cap']
    keys = sorted(int(p['part_key'], 16) for p in wood)
    bkeys = sorted(int(p['part_key'], 16) for p in bark)
    if not keys:
        return 'p%016x' % key, []
    cands = []
    for c in (keys[0] if len(keys) == 1 else kind_key(keys), kind_key(keys),
              bkeys[0] if len(bkeys) == 1 else (kind_key(bkeys) if bkeys else None), kind_key(bkeys) if bkeys else None,
              max(bark or wood, key=lambda p: p['tris'])['part_key']):
        c = c if isinstance(c, str) else ('%016x' % c if c is not None else None)
        if c and c not in cands:
            cands.append(c)
    return cands[0], cands[1:]


def live_log_parity(path, kinds, shrub_kinds, max_top):
    """The engine's own counts in a game log (da_shrubs.cpp collect_placements: '[shrubs] <n> FLODs of tree parts,
    <m> stock bushes of the data (..), <k> kept stock (oversize)', the level named by the caches loaded before it)
    against the same counts from this census."""
    if not path or not os.path.exists(path):
        return 'no game log'
    cls = {int(k, 16): v.split(',')[0].strip() for k, v in shrub_kinds.items()}
    found, level = set(), None
    pat = re.compile(r'\[shrubs\] (\d+) FLODs of tree parts, (\d+) stock bushes of the data \(\d+ placements\), '
                     r'(\d+) kept stock')
    with open(path, encoding='cp1251', errors='replace') as f:
        for line in f:
            m = re.search(r'(?:cdb_cache|levels)[\\/]([a-z0-9_]+)[\\/]', line)
            if m:
                level = m.group(1)
            m = pat.search(line)
            if not m or level not in FLODS:
                continue
            known = oversize = 0
            for kk, k in kinds.items():
                if kk not in cls:
                    continue
                for lv, top, _, _ in k['placed']:
                    if lv == level:
                        known += 1
                        oversize += 1 if top > max_top.get(cls[kk], 8.5) else 0
            mine = (FLODS[level], known - oversize, oversize)
            log = tuple(int(x) for x in m.groups())
            found.add('%s: %s (FLODs, bushes, oversize %d/%d/%d)' % (
                level, 'equal' if mine == log else 'census %d/%d/%d against' % mine, *log))
    return '; '.join(sorted(found)) if found else 'no [shrubs] census line in the game log'


def census(args):
    inv = json.load(open(os.path.join(args.inventory, 'inventory.json'), encoding='utf-8'))
    fams = [f for f in inv['families'] if f['category'] in TREE_SET]
    secs = read_ltx_sections(args.shrubs)
    shrub_kinds = {k: v for k, v in secs.get('shrub_kinds', [])}
    max_top = {'BUSH': 8.5, 'BUSH_GROUP': 8.5, 'CONIFER_BUSH': 4.7, 'THICKET': 10.5}
    min_scale = 0.05
    for k, v in secs.get('shrub_policy', []):
        if k == 'max_top':
            for item in v.split(','):
                c, x = item.strip().split(':')
                max_top[c.strip()] = float(x)
        elif k == 'min_scale':
            min_scale = float(v)

    kinds = survey(args.levels, args.only)

    # ---- parity: the shrubs' keys and family rows out of this code
    parity = {}
    sk = [int(k, 16) for k in shrub_kinds]
    found = [k for k in sk if k in kinds]
    parity['shrub_kinds'] = '%d of %d [shrub_kinds] keys computed' % (len(found), len(sk))
    shipped = set()
    for sig, rows in secs.get('shrub_families', []):
        for r in rows.split(','):
            shipped.add((sig.lower(), r.strip()))
    mine = set()
    for k in found:
        mine.add(('%016x' % signature_hash(kinds[k]['parts']), family_row(k, kinds[k]['parts'])))
    same = len(mine & shipped)
    parity['shrub_families'] = '%d of %d [shrub_families] rows reproduced exactly' % (same, len(shipped))
    parity['shrub_families_differ'] = sorted('%s = %s' % r for r in (mine - shipped))[:20]
    # The census of the inventory's own keys (every family member).
    inv_keys = set()
    for f in fams:
        inv_keys |= {int(m, 16) for m in set(f['family_members']) | {f['key']}}
    parity['tree_keys'] = '%d of %d tree-set keys of the inventory computed' % (len(inv_keys & set(kinds)), len(inv_keys))
    parity['live_log'] = live_log_parity(args.log, kinds, shrub_kinds, max_top)
    # The sidecars' part keys of the exported copy against this code's.
    side_ok = side_n = 0
    # Window vertex count against the vertex range (the engine's box is the window's).
    win_diff = 0

    out = {}
    for f in fams:
        key = int(f['key'], 16)
        side = None
        sp = os.path.join(args.inventory, 'meshes', f['key'], f['key'] + '.json')
        if f.get('exported') and os.path.exists(sp):
            side = json.load(open(sp, encoding='utf-8'))
        members = sorted(set(f['family_members']) | {f['key']})
        if side:
            mk = kinds.get(key)
            if mk:
                side_n += 1
                side_ok += sorted(int(p['part_key'], 16) for p in side['parts']) == sorted(p['key'] for p in mk['parts'])
        # Class: the standing trees, the oversize bushes as young trees, the rest stock.
        flags = list(f.get('flags') or [])
        if f['category'] in STOCK_CATEGORIES:
            cls, why = 'STOCK', f['category'].lower()
        elif f['category'] == 'BUSH_KEPT_STOCK':
            cls, why = 'YOUNG', 'oversize %s' % f.get('bush_class')
        else:
            cls, why = 'TREE', ''
        cards = f.get('cards') or {}
        live = any(cards.get(c) in LIVE_CARDS for c in (f.get('main_cards') or []))
        dead = f['state'] in ('DEAD', 'BROKEN')
        if cls == 'TREE' and ('anomalous' in flags or 'burnt' in flags):
            # (The anomalies and the burnt trees by their crowns: a leafy one lives, resprouting.)
            dead = not live
        red = 'red_forest' in flags
        # Card area per species hint of the main cards (a model dressed in two species' cards takes the larger; the
        # bare twig fronds name no species: an oak frond dresses alder and cherry models too).
        card_area = collections.Counter()
        if side:
            for p in side['parts']:
                if p['role'] == 'leaf':
                    h = hint_of(tex_of(p['shader']))
                    if h:
                        card_area[h] += p['area_m2']
        if side:
            skel, cands = skeleton_ids(key, side['parts'])
        else:
            skel, cands = 'p%016x' % key, []
        if cls == 'YOUNG':
            skel, cands = 'p%016x' % key, []
        wood = []
        if side:
            for p in side['parts']:
                if p['blender'].lower() == 'flora\\trunk_wave':
                    wood.append({'key': p['part_key'], 'role': p['role'], 'texture': tex_of(p['shader']), 'tris': p['tris']})
        # Placements: the census's own (the shrubs' size rule for the oversize bushes).
        levels = collections.Counter()
        tops, tilts, scales = [], [], []
        bush_cls = shrub_kinds.get(f['key'], '').split(',')[0].strip() or f.get('bush_class')
        sig, rows = None, {}
        for m in members:
            k = kinds.get(int(m, 16))
            if not k:
                continue
            for lv, top, sy, tilt in k['placed']:
                if sy < min_scale:
                    continue
                if cls == 'YOUNG' and top <= max_top.get(bush_cls, 8.5):
                    continue
                levels[lv] += 1
                tops.append(top)
                tilts.append(tilt)
                scales.append(sy)
            s = '%016x' % signature_hash(k['parts'])
            sig = sig or s
            if s != sig:
                print('warning: %s member %s has another signature' % (f['key'], m))
            rows[m] = family_row(int(m, 16), k['parts'])
            win_diff += sum(1 for p in k['parts'] if p['vcount'] != p['vrange'])
        q = lambda v, p: round(float(np.percentile(v, p)), 2) if v else None
        model = f.get('model') or {}
        out[f['key']] = {
            'key': f['key'], 'members': members, 'class': cls, 'stock_reason': why,
            'category': f['category'], 'state': f['state'], 'dead': dead, 'red': red, 'conifer': bool(f.get('conifer')),
            'burnt': 'burnt' in flags, 'anomalous': 'anomalous' in flags, 'live_cards': live,
            'species_hint': f.get('species') or [], 'card_area': {h: round(a, 2) for h, a in card_area.most_common()},
            'main_cards': f.get('main_cards') or [], 'cards': f.get('cards') or {}, 'barks': f.get('barks') or [],
            'flags': flags, 'bush_class': bush_cls if f['category'] == 'BUSH_KEPT_STOCK' else None,
            'skel': skel, 'skel_candidates': cands, 'wood_parts': wood,
            'signature': sig, 'family_rows': rows,
            'levels': dict(sorted(levels.items(), key=lambda kv: -kv[1])), 'placements': sum(levels.values()),
            'top': {'p10': q(tops, 10), 'p50': q(tops, 50), 'p90': q(tops, 90), 'max': q(tops, 100)},
            'tilt_p90': q(tilts, 90), 'tilt_max': q(tilts, 100), 'scale_p50': q(scales, 50),
            'model': {k2: model.get(k2) for k2 in ('height', 'span', 'extent_x', 'extent_z', 'crown_base', 'butt_radius')},
            'size_class': f.get('size_class'), 'stems': len(f.get('stems_local_xray') or []),
            'collision': (f.get('collision') or {}).get('collision_kind'),
        }
    parity['sidecar_parts'] = '%d of %d exported copies: the sidecar part keys equal this code\'s' % (side_ok, side_n)
    parity['window_vs_range'] = '%d parts whose full-detail window has fewer vertices than its range' % win_diff
    return out, parity


# ======================================================================================================
# The measured colours, compact (species research, tree_colours.json)
# ======================================================================================================
MONTHS = ['jan', 'feb', 'mar', 'apr', 'may', 'jun', 'jul', 'aug', 'sep', 'oct', 'nov', 'dec']
ROLES = ('leaf', 'young', 'needle', 'flower', 'fruit', 'autumn', 'twig')


def rec(r, label=True):
    if not r:
        return None
    o = {'hex': r['hex'], 'n': r.get('n', 0)}
    if label and r.get('label'):
        o['label'] = r['label']
    if r.get('all') and label:
        o['kinds'] = {k: {'hex': v['hex'], 'n': v.get('n', 0)} for k, v in r['all'].items()}
    return o


def colours(species_dir):
    d = json.load(open(os.path.join(species_dir, 'tree_colours.json'), encoding='utf-8'))
    out = {}
    for sid, s in d.items():
        months = {}
        for m in MONTHS:
            b = s['buckets'].get(m, {})
            e = {r: rec(b.get(r)) for r in ROLES if b.get(r)}
            cm = b.get('crown_main')
            if cm:
                e['crown_main'] = {'hex': cm['hex'], 'n': cm.get('n', 0), 'kind': cm.get('kind')}
            if b.get('foliage_mix'):
                e['foliage_mix'] = b['foliage_mix']
            e['leaf_share'] = b.get('leaf_share')
            e['bare_share'] = b.get('bare_share')
            kinds = b.get('kinds') or {}
            e['kinds'] = {k: {'hex': v['hex'], 'n': v.get('n', 0)} for k, v in kinds.items()
                          if k in ('yellow', 'orange', 'red', 'burgundy', 'brown', 'tan', 'dead', 'white', 'cream')}
            months[m] = e
        ap = s.get('autumn_palette') or {}
        bk = s.get('bark') or {}
        out[sid] = {
            'latin': s.get('latin'), 'group': s.get('group'), 'evergreen': bool(s.get('evergreen')),
            'glaucous_index': s.get('glaucous_index_leafon'), 'months': months,
            'autumn': {'A': ap.get('A'), 'B': ap.get('B'), 'A_hex': (ap.get('A_rec') or {}).get('hex'),
                       'B_hex': (ap.get('B_rec') or {}).get('hex'), 'counts': ap.get('photo_counts'),
                       'mix': ap.get('supplement_mix')} if ap else None,
            'bark': {k: {kk: bk[k].get(kk) for kk in ('hex', 'n', 'ridge_hex', 'fissure_hex', 'contrast', 'dark_frac')}
                     for k in ('all', 'smooth', 'fissured') if bk.get(k)},
            'bark_kinds': {k: {'hex': v['hex'], 'n': v.get('n', 0), 'share': v.get('share_median')}
                           for k, v in (bk.get('kinds') or {}).items()},
            'wood_far': {'hex': s['wood_far']['hex'], 'n': s['wood_far'].get('n', 0)} if s.get('wood_far') else None,
            'trunk_zones_far': {k: {'hex': v['hex'], 'n': v.get('n', 0)} for k, v in (s.get('trunk_zones_far') or {}).items()
                                if isinstance(v, dict)},
            'phenology_inat': {k: {'doy': v.get('doy'), 'n': v.get('n'), 'box': v.get('box')}
                               for k, v in (s.get('phenology_inat') or {}).items() if v.get('n')},
            'flags': s.get('flags') or [],
        }
    return out


def write_json(path, obj):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\r\n') as f:
        json.dump(obj, f, indent=1, sort_keys=False)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--levels', default=DEF_LEVELS)
    ap.add_argument('--inventory', default=DEF_INVENTORY)
    ap.add_argument('--species', default=DEF_SPECIES)
    ap.add_argument('--shrubs', default=SHRUBS_LTX)
    ap.add_argument('--log', default=DEF_LOG, help="a game log with the shrubs' census lines (a parity check)")
    ap.add_argument('--only', nargs='*', help='survey these levels only (a quick check; the census needs all)')
    ap.add_argument('--colours-only', action='store_true')
    args = ap.parse_args()
    os.makedirs(DATA, exist_ok=True)
    col = colours(args.species)
    write_json(OUT_COLOURS, {'meta': {'source': 'species research tree_colours.json (METHOD_TREES.md), 08.10.2026',
                                      'note': 'sRGB hex of linear medians per calendar month and colour role'},
                             'species': col})
    print('colours: %d species -> %s' % (len(col), OUT_COLOURS))
    if args.colours_only:
        return
    kinds, parity = census(args)
    cnt = collections.Counter(k['class'] for k in kinds.values())
    meta = {
        'source': 'levels %s; inventory %s' % (args.levels, args.inventory),
        'key_definition': 'da_shrubs.cpp read_part_keys / kind_of, reproduced by tools/trees/census_trees.py: part key = '
                          'FNV-1a 64 of the lower-case shader string, its zero, the u32 triangle count of the full-detail '
                          'window and the wrapping sum of the triangles\' hashes (vertices as mm positions and uv mod 2048, '
                          'sorted per triangle); kind key = FNV-1a 64 over the sorted part keys (u64 LE)',
        'skeleton_rule': 'the wood parts (flora\\trunk_wave: bark and cut ends): one part\'s key, several hashed like a '
                         'kind key; no wood or a young tree: p<kind key>',
        'signature_rule': '[shrub_families] format: FNV-1a 64 over the parts sorted by shader (lower-case string + zero + '
                          'u32 triangles); rows <member key>:<box mm min xyz>:<box mm max xyz>:<area m2>',
        'classes': dict(cnt), 'parity': parity,
    }
    write_json(OUT_KINDS, {'meta': meta, 'kinds': dict(sorted(kinds.items()))})
    print('kinds: %d families (%s), %d keys -> %s' % (len(kinds), ', '.join('%s %d' % kv for kv in sorted(cnt.items())),
                                                       sum(len(k['members']) for k in kinds.values()), OUT_KINDS))
    for k, v in parity.items():
        print('parity %s: %s' % (k, v))


if __name__ == '__main__':
    main()
