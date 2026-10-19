"""Decide, for every Stalker Two-K file in the 1.6.0 tree, whether it goes away or gets our own replacement.

Removal means the game falls back to Dead Air's own file. A replacement is made only where Two-K raised
the resolution, so the player keeps the sharpness without Two-K's pixels.
"""
import json
import os
import sys

from common import BORROWED, GSC_RAW, WORK, orig_path
import upscale

FOLDERS = 'briks crete detail door floor glas glass grnd mtl prop roof ston tile veh wall water wind wood'.split()
DDS_FMT = {'DXT1': 'BC1_UNORM', 'DXT3': 'BC2_UNORM', 'DXT5': 'BC3_UNORM', 'RGB32A': 'B8G8R8A8_UNORM', 'RGB32': 'B8G8R8X8_UNORM', 'RGB24': 'B8G8R8X8_UNORM'}


def main():
    survey = {r['p']: r for r in json.load(open(os.path.join(WORK, 'twok_survey.json')))}
    gsc = {r['p']: r for r in json.load(open(os.path.join(WORK, 'twok_gsc.json')))}
    jobs, removes = [], []
    for sub in FOLDERS:
        for dp, _, fs in os.walk(os.path.join(BORROWED, 'textures', sub)):
            for f in fs:
                rel = os.path.relpath(os.path.join(dp, f), BORROWED).replace('\\', '/')
                if not f.lower().endswith('.dds'):
                    removes.append((rel, 'thm: Dead Air\'s own description applies'))
                    continue
                r = survey[rel]
                o, b = r.get('o'), r['b']
                if not r.get('orig'):
                    removes.append((rel, 'no Dead Air original'))
                    continue
                if r.get('same_bytes'):
                    removes.append((rel, 'identical to Dead Air'))
                    continue
                if r.get('err') or not o or o['cube'] or o['vol'] or b['cube'] or b['vol']:
                    removes.append((rel, 'special format: Dead Air original kept'))
                    continue
                if b['w'] * b['h'] <= o['w'] * o['h']:
                    removes.append((rel, 'Two-K not sharper than Dead Air'))
                    continue
                kind = 'bump#' if rel.endswith('_bump#.dds') else 'bump' if rel.endswith('_bump.dds') else \
                    'normal' if r.get('normal_like') else 'diffuse'
                src, src_w, src_h = orig_path(rel), o['w'], o['h']
                g = gsc.get(rel, {})
                # The GSC source is the same picture without DXT damage; use it when it is at least as big
                if kind in ('diffuse', 'normal') and g.get('corr') is not None and g['corr'] > 0.9 and 'tga' in g:
                    tw, th = g['tga']
                    if tw >= src_w and th >= src_h and abs(tw / th - src_w / src_h) < 1e-3:
                        src, src_w, src_h = os.path.join(GSC_RAW, rel[len('textures/'):-4] + '.tga'), tw, th
                w, h = upscale.plan_size(src_w, src_h, b['w'], b['h'])
                fmt = DDS_FMT.get(o['fmt'])
                if not fmt:
                    removes.append((rel, f'unknown format {o["fmt"]}: Dead Air original kept'))
                    continue
                jobs.append({'rel': rel, 'kind': kind, 'src': src, 'src_w': src_w, 'src_h': src_h, 'w': w, 'h': h,
                             'fmt': fmt, 'mips': 0 if o['mips'] > 1 else 1, 'twok': [b['w'], b['h']], 'da': [o['w'], o['h']]})
    plan = {'jobs': jobs, 'removes': removes}
    json.dump(plan, open(os.path.join(WORK, 'twok_plan.json'), 'w'), indent=0)
    from collections import Counter
    print('jobs', Counter(j['kind'] for j in jobs), 'gsc sources', sum('rawdata' in j['src'] for j in jobs))
    print('removes', Counter(r[1].split(':')[0] for r in removes))
    print('factor', Counter(round(j['w'] / j['src_w'], 2) for j in jobs))


if __name__ == '__main__':
    main()
