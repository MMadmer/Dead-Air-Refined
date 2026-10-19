"""Audit of the 1.6.0 tree: every planned replacement present and no longer the main tree's file."""
import collections
import json
import os

from common import TREE16, WORK, dds_head, is_shared

plan = json.load(open(os.path.join(WORK, 'twok_plan.json')))
c = collections.Counter()
bad = []
for j in plan['jobs']:
    if j['kind'] in ('diffuse', 'normal'):
        rels = [j['rel']]
    elif j['kind'] == 'bump':
        rels = [j['rel'], j['rel'].replace('_bump.dds', '_bump#.dds')]
    else:
        rels = []
    for rel in rels:
        p = os.path.join(TREE16, rel)
        if not os.path.isfile(p):
            bad.append(('missing', rel))
            continue
        if is_shared(rel):
            bad.append(('still borrowed', rel))
            continue
        h = dds_head(p)
        if j['kind'] in ('diffuse', 'normal') and (h['w'], h['h']) != (j['w'], j['h']):
            bad.append(('size', rel, h['w'], h['h'], j['w'], j['h']))
        c[j['kind']] += 1
left = []
for sub in 'briks crete detail door floor glas glass grnd mtl prop roof ston tile veh wall water wind wood trees'.split():
    for dp, _, fs in os.walk(os.path.join(TREE16, 'textures', sub)):
        for f in fs:
            rel = os.path.relpath(os.path.join(dp, f), TREE16).replace(os.sep, '/')
            if is_shared(rel):
                left.append(rel)
print(dict(c), 'problems:', len(bad), bad[:8])
print('files still shared with main:', len(left))
print(left[:60])
