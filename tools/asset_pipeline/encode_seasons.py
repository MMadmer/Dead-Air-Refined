"""Encode every season PNG into the 1.6.0 tree.

autumn trees -> textures/trees/<name>.dds (the base set)
other looks  -> textures/seasons/<folder>/<gamedata-relative path>.dds
"""
import json
import os
import struct
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import TREE16, WORK, install, retired, texconv  # noqa: E402

FOLDER = {'winter': 'winter', 'spring_early': 'spring_early', 'spring_mid': 'spring_mid', 'spring_late': 'spring_late', 'summer': 'summer'}
STOCK_TREES = r"D:/Games/Dead Air/_work/texaudit/stock/textures/trees"


def dds_fmt(p):
    h = open(p, 'rb').read(128)
    four = h[84:88].decode('ascii', 'replace').strip('\0')
    return {'DXT1': 'BC1_UNORM', 'DXT3': 'BC2_UNORM', 'DXT5': 'BC3_UNORM'}.get(four, 'B8G8R8A8_UNORM')


def one(job):
    png, rel, fmt, mips, cutout = job
    tmp = tempfile.mkdtemp(dir=os.path.join(WORK, 'enc_tmp'))
    try:
        dds = texconv(png, tmp, fmt, mips, alpha_cutout=cutout)
        install(dds, rel)
    finally:
        for f in os.listdir(tmp):
            os.remove(os.path.join(tmp, f))
        os.rmdir(tmp)
    return rel


def tree_jobs(only=None, looks=None):
    """only: card-name prefixes (families) to encode; looks: subset of looks."""
    out = []
    base = os.path.join(WORK, 'seasons_out')
    for look in looks or (['autumn'] + list(FOLDER)):
        d = os.path.join(base, look)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith('.png'):
                continue
            name = f[:-4]
            if only is not None and not any(name == o or name.startswith(o + '_') for o in only):
                continue
            da = os.path.join(STOCK_TREES, name + '.dds')
            fmt = dds_fmt(da) if os.path.isfile(da) else 'BC1_UNORM'
            rel = f'textures/trees/{name}.dds' if look == 'autumn' else f'textures/seasons/{FOLDER[look]}/trees/{name}.dds'
            if retired(rel):
                continue  # WP7: a look nothing draws any more
            out.append((os.path.join(d, f), rel, fmt, 0, True))
    return out


# The autumn folder held only the LOD atlases (seasons_lods.py), retired by WP7: the engine composes level_lods.
LEVEL_FOLDER = dict(FOLDER)


def plan_jobs(root, plan_name, looks, kinds=None):
    out = []
    p = os.path.join(WORK, root, 'plan.json')
    if not os.path.isfile(p):
        return out
    for j in json.load(open(p)):
        name = j['rel'].rsplit('/', 1)[-1][:-4]
        kind = 'terrain' if '/terrain/' in j['rel'] else 'map' if j['rel'].startswith(('map/', 'ui/')) else name
        if kinds and kind not in kinds:
            continue
        lods = name == 'level_lods'
        if lods:
            continue  # WP7: level_lods has no season look, the engine composes it
        for look in looks:
            if look == 'winter' and kind in ('terrain', 'build_details'):
                continue  # WP7: no [season_folders] band wears a winter terrain or grass atlas
            if look == 'summer' and j.get('skip_summer'):
                continue  # identical to the base set
            if look == 'autumn' and not lods:
                continue  # the base set itself
            png = os.path.join(WORK, root, look, j['rel'][:-4] + '.png')
            if os.path.isfile(png):
                # The LOD atlas is alpha-tested from far away: its mips keep the coverage of the top level
                out.append((png, f"textures/seasons/{LEVEL_FOLDER[look]}/{j['rel']}", j['fmt'], 0 if j['mips'] > 1 else 1, lods))
    return out


def main():
    # 'ground' (winter grnd/detail) is retired by WP7: the ground category never wears a season look
    what = sys.argv[1:] or ['trees', 'levels']
    jobs = []
    if 'trees' in what:
        only = looks = None
        if os.environ.get('ENC_FAMILIES'):
            only = open(os.environ['ENC_FAMILIES']).read().split()
        if os.environ.get('ENC_LOOKS'):
            looks = os.environ['ENC_LOOKS'].split(',')
        jobs += tree_jobs(only, looks)
    if 'levels' in what:
        kinds = os.environ['ENC_LEVEL_KINDS'].split(',') if os.environ.get('ENC_LEVEL_KINDS') else None
        looks = os.environ['ENC_LEVEL_LOOKS'].split(',') if os.environ.get('ENC_LEVEL_LOOKS') else list(LEVEL_FOLDER)
        jobs += plan_jobs('seasons_levels_out', 'plan.json', looks, kinds)
    if 'ground' in what:
        jobs += plan_jobs('seasons_ground_out', 'plan.json', ['winter'])
    os.makedirs(os.path.join(WORK, 'enc_tmp'), exist_ok=True)
    print(len(jobs), 'textures to encode', flush=True)
    done = 0
    with ProcessPoolExecutor(max_workers=8) as ex:
        for f in [ex.submit(one, j) for j in jobs]:
            try:
                f.result()
                done += 1
            except Exception as e:
                print('ERROR', repr(e)[:300], flush=True)
            if done % 100 == 0:
                print(done, flush=True)
    print('encoded', done, flush=True)


if __name__ == '__main__':
    main()
