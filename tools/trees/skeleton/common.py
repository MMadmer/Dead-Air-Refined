# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Paths, crash-safe writes and the stock mesh loader shared by the skeleton stages.

Inputs (read-only): the tree inventory (meshes/<key>/<key>.npz = the stock bytes of every part exactly as level.geom
holds them, <key>.json = the sidecar, <key>_instances.csv = every placement), the compiled levels (level.cform) and
gamemtl.xr. Every path can be overridden through the environment (DA_TREES_INVENTORY, DA_LEVELS, DA_GAMEMTL,
DA_SKEL_REPORT).
"""
import contextlib
import csv
import io
import json
import os
import struct
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TREES = os.path.normpath(os.path.join(HERE, '..'))
REPO = os.path.normpath(os.path.join(TREES, '..', '..'))
SCRATCH = r'C:\Users\admin\AppData\Local\Temp\claude\D--Games-Dead-Air-DeadAir-x64\f9b3463a-7c5c-40f8-a3bd-38013b895726\scratchpad\trees'
INVENTORY = os.environ.get('DA_TREES_INVENTORY', os.path.join(SCRATCH, 'inventory'))
LEVELS = os.environ.get('DA_LEVELS', os.path.join(REPO, '_work', 'asset_pipeline', 'seasons', 'da_levels', 'levels'))
GAMEMTL = os.environ.get('DA_GAMEMTL', r'D:\Games\Dead Air\_work\da_unpack\configs\gamemtl.xr')
# Binary assets live in the content source (Dead-Air-Refined_Assets), never in the engine repository.
ASSETS = os.environ.get('DAR_ASSETS_GAMEDATA', os.path.join(REPO, '..', '..', 'Dead-Air-Refined_Assets', 'source', 'gamedata'))
OUT_DTSK = os.path.join(ASSETS, 'meshes', 'da', 'trees')
OUT = os.path.join(TREES, 'out')
OUT_REPORT = os.path.join(OUT, 'report')
REPORT_PNG = os.environ.get('DA_SKEL_REPORT', os.path.join(SCRATCH, 'skeleton_report'))
WORK = os.environ.get('DA_SKEL_WORK', os.path.join(SCRATCH, 'skeleton_work'))

# Classes the upgrade takes (INVENTORY.md); everything else stays stock
TREE_CLASSES = ('TREE',)
BUSH_CLASSES = ('BUSH_KEPT_STOCK',)


# ------------------------------------------------------------------------------------------------ crash-safe writes
def _replace(tmp, path):
    # A reader (an AV scanner, the preview) can hold the target for a moment on Windows
    for i in range(40):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.05 * (i + 1))
    os.replace(tmp, path)


@contextlib.contextmanager
def atomic_open(path, mode='w', **kw):
    """Writes to `<path>.<pid>.tmp`, fsyncs it and renames it over `path`; a crash leaves the old file or a stray
    .tmp, never a zero-filled target."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = f'{path}.{os.getpid()}.tmp'
    if 'b' not in mode:
        kw.setdefault('encoding', 'utf-8')
    f = open(tmp, mode, **kw)
    try:
        yield f
        f.flush()
        os.fsync(f.fileno())
        f.close()
        _replace(tmp, path)
    except BaseException:
        f.close()
        if os.path.exists(tmp):
            os.remove(tmp)
        raise


def atomic_bytes(path, data):
    with atomic_open(path, 'wb') as f:
        f.write(data)


def atomic_text(path, text, crlf=True):
    """Text with CRLF line ends (docs, CSV) unless crlf=False."""
    if crlf:
        text = text.replace('\r\n', '\n').replace('\n', '\r\n')
    atomic_bytes(path, text.encode('utf-8'))


def atomic_json(path, obj):
    atomic_text(path, json.dumps(obj, indent=1, sort_keys=False) + '\n', crlf=False)


def atomic_png(path, img):
    from PIL import Image
    if isinstance(img, np.ndarray):
        img = Image.fromarray(img)
    buf = io.BytesIO()
    img.save(buf, format='PNG', optimize=False)
    atomic_bytes(path, buf.getvalue())


def atomic_npz(path, **arrays):
    buf = io.BytesIO()
    np.savez_compressed(buf, **arrays)
    atomic_bytes(path, buf.getvalue())


# ------------------------------------------------------------------------------------------------ keys
FNV_OFF = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK = (1 << 64) - 1


def fnv1a64(data, h=FNV_OFF):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def combine_keys(keys):
    """Several part keys as one: FNV-1a 64 over the sorted keys packed as little-endian u64 (the engine's kind key
    rule, applied to the wood parts only)."""
    return fnv1a64(b''.join(struct.pack('<Q', k) for k in sorted(keys)))


# ------------------------------------------------------------------------------------------------ stock meshes
MU_DTYPE = np.dtype([('P', '<f4', 3), ('N', 'u1', 4), ('T', 'u1', 4), ('B', 'u1', 4), ('tc', '<i2', 4)])
TREE_QUANT = 2048.0
CAPS = ('trees_spil', 'trees_atp_pine_cap', 'trees_atp_spruce_cap')
FRONDS = ('trees_atp_b01_frond', 'trees_atp_birch_frond', 'trees_atp_oak_frond3', 'trees_atp_spruce_frond')


def tex_name(shader):
    t = shader.split('/', 1)[1].split(',')[0] if '/' in shader else ''
    return t.split('\\')[-1].lower()


def blender_of(shader):
    return shader.split('/', 1)[0].lower()


def decode(rec):
    """Vertex channels of MU records (mu_model_decl): position, normal (bytes nz, ny, nx, hemi), uv, frac."""
    N = rec['N'].astype(np.float64)
    tc = rec['tc'].astype(np.float64)
    return {'P': rec['P'].astype(np.float64), 'N': N[:, [2, 1, 0]] / 255.0 * 2 - 1, 'hemi': N[:, 3] / 255.0,
            'uv': tc[:, :2] / TREE_QUANT, 'frac': tc[:, 2] / TREE_QUANT, 'tc': rec['tc'][:, :2].astype(np.int64)}


class Part:
    def __init__(self, meta, rec, tris):
        self.meta = meta
        self.index = meta['index']
        self.role = meta['role']
        self.shader = meta['shader']
        self.blender = blender_of(meta['shader'])
        self.texture = tex_name(meta['shader'])
        self.key = int(meta['part_key'], 16)
        d = decode(rec)
        self.P, self.N, self.uv, self.frac, self.hemi = d['P'], d['N'], d['uv'], d['frac'], d['hemi']
        self.T = tris.astype(np.int64)

    @property
    def is_wood(self):
        return 'trunk_wave' in self.blender or self.role in ('trunk', 'cap')

    @property
    def is_card(self):
        return 'leaf_wave' in self.blender or self.role in ('leaf', 'frond')


class Kind:
    """One exported mesh family of the inventory (sidecar + exact stock bytes)."""

    def __init__(self, key):
        self.key = key
        d = os.path.join(INVENTORY, 'meshes', key)
        self.dir = d
        with open(os.path.join(d, key + '.json'), encoding='utf-8') as f:
            self.side = json.load(f)
        z = np.load(os.path.join(d, key + '.npz'))
        self.parts = []
        for p in self.side['parts']:
            i = p['index']
            rec = np.frombuffer(z[f'rec_{i}'].tobytes(), MU_DTYPE)
            self.parts.append(Part(p, rec, z[f'draw_{i}']))
        self.category = self.side['category']
        self.state = self.side['state']
        self.flags = list(self.side.get('flags') or [])
        self.conifer = bool(self.side.get('conifer'))
        self.species = list(self.side.get('species') or [])
        self.members = list(self.side.get('family_members') or [key])
        self.stems = [tuple(s) for s in (self.side.get('stems_local_xray') or [])]

    def wood_parts(self):
        return [p for p in self.parts if p.is_wood]

    def bark_parts(self):
        return [p for p in self.parts if p.is_wood and p.role != 'cap']

    def cap_parts(self):
        return [p for p in self.parts if p.is_wood and p.role == 'cap']

    def card_parts(self):
        return [p for p in self.parts if p.is_card]

    def instances(self):
        """Every placement: level, exact kind key, 4x4 xform (world = [P, 1] x M), part keys, kept/replaced."""
        out = []
        with open(os.path.join(self.dir, self.key + '_instances.csv'), newline='', encoding='utf-8') as f:
            for r in csv.DictReader(f):
                if r.get('collapsed', '0') == '1':
                    continue
                M = np.array([float(x) for x in r['xform_rows'].split()], np.float64).reshape(4, 4)
                out.append({'level': r['level'], 'kind': r['kind_key'], 'xform': M,
                            'replaced': r.get('shrub_replaced', '0') == '1', 'flod': int(r['flod_visual']),
                            'part_keys': r['part_keys'].split()})
        return out


def inventory_families():
    with open(os.path.join(INVENTORY, 'inventory.json'), encoding='utf-8') as f:
        return json.load(f)['families']


def fmt_key(k):
    return f'{k:016x}'
