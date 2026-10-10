# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Stage 2: the collision of a sample of every skeleton's placements, read once per level.

Per skeleton up to N placements (stock-kept first, round-robin over levels) of all its kinds; per placement the
level.cform triangles of materials\\tree_trunk (solid wood) and materials\\bush / bush_sux (passable crown hull) whose
box meets the placement's model box. Stored in world space with the placement's xform:
WORK/cform/<skel>.npz {xforms (p, 4, 4), levels, tri (t, 3, 3), kind (t: 1 trunk, 2 hull), owner (t: placement)}."""
import collections
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cform as CF  # noqa: E402
import common as C  # noqa: E402

N_PLACEMENTS = 12


def main(only=None):
    plan = json.load(open(os.path.join(C.WORK, 'plan.json'), encoding='utf-8'))['skeletons']
    mtl = CF.read_gamemtl()
    jobs = collections.defaultdict(list)  # level -> [(skel, slot, xform, box)]
    meta = {}
    for sid, s in plan.items():
        if only and sid not in only:
            continue
        inst = []
        box = None
        for k in s['kinds']:
            kind = C.Kind(k)
            mb = kind.side['model_box_xray']
            b = (np.array(mb['min']), np.array(mb['max']))
            box = b if box is None else (np.minimum(box[0], b[0]), np.maximum(box[1], b[1]))
            inst += [dict(i, family=k) for i in kind.instances()]
        picks = CF.pick_placements(inst, N_PLACEMENTS)
        meta[sid] = {'picks': picks, 'box': box}
        for slot, p in enumerate(picks):
            jobs[p['level']].append((sid, slot, p['xform'], CF.world_box(p['xform'], box[0], box[1])))
    out = {sid: {'tri': [], 'kind': [], 'owner': []} for sid in meta}
    for lv in sorted(jobs):
        t0 = time.time()
        L = CF.LevelCform(lv, mtl)
        for sid, slot, xf, (bmin, bmax) in jobs[lv]:
            T, K = L.in_box(bmin, bmax)
            out[sid]['tri'].append(T)
            out[sid]['kind'].append(K)
            out[sid]['owner'].append(np.full(len(K), slot))
        print(f'{lv}: {len(jobs[lv])} placements, {time.time() - t0:.1f}s', flush=True)
        del L
    for sid, m in meta.items():
        o = out[sid]
        tri = np.concatenate(o['tri']) if o['tri'] else np.zeros((0, 3, 3))
        kind = np.concatenate(o['kind']) if o['kind'] else np.zeros(0, np.int64)
        owner = np.concatenate(o['owner']) if o['owner'] else np.zeros(0, np.int64)
        C.atomic_npz(os.path.join(C.WORK, 'cform', f'{sid}.npz'),
                     xforms=np.array([p['xform'] for p in m['picks']]).reshape(-1, 4, 4),
                     levels=np.array([p['level'] for p in m['picks']]),
                     families=np.array([p['family'] for p in m['picks']]),
                     tri=tri.astype(np.float32), kind=kind.astype(np.int8), owner=owner.astype(np.int16))
    print(f'{len(meta)} skeletons written')


if __name__ == '__main__':
    main(set(sys.argv[1:]) or None)
