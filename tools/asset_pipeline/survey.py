"""Survey of a borrowed texture set against Dead Air's own textures: what each file replaces and how."""
import hashlib
import json
import os
import struct
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import BORROWED  # noqa: E402
ORIG = [r"D:/Games/Dead Air/_work/texaudit/dar2", r"D:/Games/Dead Air/_work/texaudit/stock"]
TWOK = 'briks crete detail door floor glas glass grnd mtl prop roof ston tile veh wall water wind wood'.split()


def dds_head(p):
    h = open(p, 'rb').read(148)
    if h[:4] != b'DDS ':
        return None
    height, width = struct.unpack_from('<II', h, 12)
    mips = max(1, struct.unpack_from('<I', h, 28)[0])
    pf_flags = struct.unpack_from('<I', h, 80)[0]
    fourcc = h[84:88]
    caps2 = struct.unpack_from('<I', h, 112)[0]
    if pf_flags & 4:
        fmt = fourcc.decode('ascii', 'replace').strip('\0')
    else:
        bits = struct.unpack_from('<I', h, 88)[0]
        fmt = f'RGB{bits}' + ('A' if pf_flags & 1 else '')
    return {'w': width, 'h': height, 'mips': mips, 'fmt': fmt, 'cube': bool(caps2 & 0x200), 'vol': bool(caps2 & 0x200000)}


def orig_of(rel):
    for r in ORIG:
        p = os.path.join(r, rel)
        if os.path.isfile(p):
            return p, ('dar2' if 'dar2' in r else 'stock')
    return None, None


def analyse(rel):
    b = os.path.join(BORROWED, rel)
    o, src = orig_of(rel)
    rec = {'p': rel, 'b': dds_head(b)}
    if not o:
        rec['orig'] = None
        return rec
    rec['orig'] = src
    rec['o'] = dds_head(o)
    rb, ro = open(b, 'rb').read(), open(o, 'rb').read()
    rec['same_bytes'] = rb == ro
    if rec['same_bytes'] or rec['o'] is None or rec['o']['cube'] or rec['o']['vol'] or rec['b']['cube'] or rec['b']['vol']:
        return rec
    try:
        ib = Image.open(b).convert('RGBA')
        io_ = Image.open(o).convert('RGBA')
    except Exception as e:
        rec['err'] = repr(e)
        return rec
    a = np.asarray(io_, np.float32)
    al = a[..., 3]
    rec['alpha'] = {'min': float(al.min()), 'binary': float(((al < 16) | (al > 239)).mean()), 'cover': float((al > 127).mean())}
    rgb = a[..., :3].reshape(-1, 3).mean(0)
    rec['mean'] = [round(float(x), 1) for x in rgb]
    # Normal-map look: blue dominant, red/green centred near 128
    rec['normal_like'] = bool(rgb[2] > 180 and abs(rgb[0] - 128) < 30 and abs(rgb[1] - 128) < 30)
    small = np.asarray(ib.resize(io_.size, Image.LANCZOS), np.float32)
    la, lb = small[..., :3].mean(-1), a[..., :3].mean(-1)
    rec['corr'] = round(float(np.corrcoef(la.ravel(), lb.ravel())[0, 1]), 3) if la.std() > 0 and lb.std() > 0 else None
    rec['mad'] = round(float(np.abs(small - a).mean()), 2)
    rec['dmean'] = [round(float(x), 1) for x in (small[..., :3].reshape(-1, 3).mean(0) - rgb)]
    return rec


def main():
    out = sys.argv[1]
    rels = []
    for sub in TWOK:
        for dp, _, fs in os.walk(os.path.join(BORROWED, 'textures', sub)):
            for f in fs:
                if f.lower().endswith('.dds'):
                    rels.append(os.path.relpath(os.path.join(dp, f), BORROWED).replace('\\', '/'))
    with ProcessPoolExecutor(max_workers=12) as ex:
        recs = list(ex.map(analyse, rels, chunksize=8))
    json.dump(recs, open(out, 'w'), indent=0)
    print(len(recs), 'records')


if __name__ == '__main__':
    main()
