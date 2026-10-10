# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Stage 1: which skeleton every upgradable kind uses.

Kinds: the inventory's TREE families (alive, dead, broken) and the BUSH_KEPT_STOCK families. Skeleton id:
  * TREE with bark: the content key of its flora\\trunk_wave part (16 lower-case hex); with several trunk_wave
    parts (bark zones, cut-end caps) the keys combined: FNV-1a 64 over the sorted part keys as little-endian u64.
    Kinds re-dressed with other textures share the key and the file. When kinds on one trunk mesh carry different
    card layouts (Jaccard of the card triangles < 0.8), the most placed layout keeps the key and every other
    layout gets combine_keys([trunk key, its first kind key]).
  * BUSH_KEPT_STOCK, and any kind without a bark part: 'p' + the family key (procedural, synthetic level-0 axis).
Writes WORK/plan.json and out/skel_index.json (kind -> skeleton for tools/trees/make_trees.py)."""
import collections
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import common as C  # noqa: E402

JACCARD_SAME = 0.8


def card_signature(kind):
    tris = set()
    for p in kind.card_parts():
        q = np.round(p.P * 200).astype(np.int64)
        for t in p.T:
            tris.add(tuple(sorted(tuple(q[i]) for i in t)))
    return tris


def build_plan():
    fams = C.inventory_families()
    targets = [f for f in fams if f['category'] in C.TREE_CLASSES + C.BUSH_CLASSES]
    base = collections.defaultdict(list)
    info = {}
    for f in targets:
        k = C.Kind(f['key'])
        wood = k.wood_parts()
        procedural = f['category'] in C.BUSH_CLASSES or not k.bark_parts()
        if procedural:
            bid = 'p' + f['key']
        else:
            keys = sorted(p.key for p in wood)
            bid = C.fmt_key(keys[0] if len(keys) == 1 else C.combine_keys(keys))
        placements = int(k.side.get('placement_stats', {}).get('placements') or k.side['instances']['count'])
        kept = int(k.side['instances'].get('kept_stock', placements))
        info[f['key']] = {'key': f['key'], 'category': f['category'], 'state': f['state'], 'flags': f['flags'],
                          'species': f['species'], 'conifer': bool(f['conifer']), 'members': f['family_members'],
                          'placements': placements, 'kept': kept, 'procedural': procedural,
                          'wood_parts': [{'key': C.fmt_key(p.key), 'texture': p.texture, 'role': p.role} for p in wood],
                          'wood_tris': int(sum(len(p.T) for p in wood)),
                          'card_tris': int(sum(len(p.T) for p in k.card_parts())),
                          'card_textures': sorted({p.texture for p in k.card_parts()}),
                          'sig': card_signature(k)}
        base[bid].append(f['key'])
    skels = {}
    for bid, keys in base.items():
        keys = sorted(keys, key=lambda k: -info[k]['kept'] * 10000 - info[k]['placements'])
        clusters = []
        for k in keys:
            for cl in clusters:
                a, b = info[cl[0]]['sig'], info[k]['sig']
                j = len(a & b) / max(len(a | b), 1)
                if j >= JACCARD_SAME:
                    cl.append(k)
                    break
            else:
                clusters.append([k])
        for n, cl in enumerate(clusters):
            if n == 0 or bid.startswith('p'):
                sid = bid
            else:
                sid = C.fmt_key(C.combine_keys([int(bid, 16), int(cl[0], 16)]))
            kinds = cl
            skels[sid] = {'skel': sid, 'trunk_id': bid, 'kinds': kinds, 'rep': kinds[0],
                          'members': sorted({m for k in kinds for m in info[k]['members']}),
                          'procedural': info[kinds[0]]['procedural'],
                          'categories': sorted({info[k]['category'] for k in kinds}),
                          'states': sorted({info[k]['state'] for k in kinds}),
                          'flags': sorted({fl for k in kinds for fl in info[k]['flags']}),
                          'species': sorted({s for k in kinds for s in info[k]['species']}),
                          'conifer': all(info[k]['conifer'] for k in kinds),
                          'placements': sum(info[k]['placements'] for k in kinds),
                          'kept': sum(info[k]['kept'] for k in kinds),
                          'wood_parts': info[kinds[0]]['wood_parts'],
                          'wood_tris': info[kinds[0]]['wood_tris'], 'card_tris': info[kinds[0]]['card_tris'],
                          'card_textures': sorted({t for k in kinds for t in info[k]['card_textures']}),
                          'layout_variant': n}
    return skels, info


def main():
    skels, info = build_plan()
    C.atomic_json(os.path.join(C.WORK, 'plan.json'), {'skeletons': skels})
    index = {'kinds': {}, 'members': {}}
    for sid, s in skels.items():
        for k in s['kinds']:
            index['kinds'][k] = sid
            for m in info[k]['members']:
                index['members'][m] = sid
    C.atomic_json(os.path.join(C.OUT, 'skel_index.json'), index)
    n_proc = sum(1 for s in skels.values() if s['procedural'])
    print(f'{len(info)} kinds -> {len(skels)} skeletons ({n_proc} procedural)')
    return skels


if __name__ == '__main__':
    main()
