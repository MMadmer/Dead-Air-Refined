"""For each borrowed texture that is bigger than Dead Air's own: is there a GSC source in the CoP SDK, how big, and is it the same picture as Dead Air's."""
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

RAW = r"D:/Games/X-Ray CoP SDK/editors/rawdata/textures"
SDK_DDS = r"D:/Games/X-Ray CoP SDK/editors/gamedata/textures"
ORIG = [r"D:/Games/Dead Air/_work/texaudit/dar2", r"D:/Games/Dead Air/_work/texaudit/stock"]


def orig_path(rel):
    for r in ORIG:
        p = os.path.join(r, rel)
        if os.path.isfile(p):
            return p
    return None


def check(rec):
    rel = rec['p']
    stem = rel[len('textures/'):-4]
    out = {'p': rel}
    tga = os.path.join(RAW, stem + '.tga')
    sdk = os.path.join(SDK_DDS, stem + '.dds')
    da = orig_path(rel)
    if os.path.isfile(sdk):
        out['sdk_same_as_da'] = open(sdk, 'rb').read() == open(da, 'rb').read()
    if not os.path.isfile(tga):
        return out
    try:
        t = Image.open(tga)
        out['tga'] = list(t.size)
        d = Image.open(da).convert('RGBA')
        tt = np.asarray(t.convert('RGBA').resize(d.size, Image.LANCZOS), np.float32)
        dd = np.asarray(d, np.float32)
        lt, ld = tt[..., :3].mean(-1), dd[..., :3].mean(-1)
        out['corr'] = round(float(np.corrcoef(lt.ravel(), ld.ravel())[0, 1]), 3) if lt.std() > 0 and ld.std() > 0 else None
        out['mad'] = round(float(np.abs(tt[..., :3] - dd[..., :3]).mean()), 2)
    except Exception as e:
        out['err'] = repr(e)
    return out


def main():
    recs = json.load(open(sys.argv[1]))
    todo = [r for r in recs if r.get('orig') and not r.get('same_bytes') and r.get('o') and not r.get('err')
            and not (r['o']['cube'] or r['o']['vol'] or r['b']['cube'] or r['b']['vol'])]
    with ProcessPoolExecutor(max_workers=12) as ex:
        res = list(ex.map(check, todo, chunksize=8))
    json.dump(res, open(sys.argv[2], 'w'))
    print(len(res))


if __name__ == '__main__':
    main()
