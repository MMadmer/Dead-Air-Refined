"""Remove from the 1.6.0 tree every Two-K / AN4 file that gets no replacement, so Dead Air's own file shows.

Only files that are still the main tree's file (same hardlink) are removed: a file already replaced
by the pipeline is never touched here.
"""
import json
import os

from common import BORROWED, TREE16, WORK, is_shared, orig_path, remove16

BARK8 = {'trees_bark', 'trees_bark_mox', 'trees_bark_sosna', 'trees_bark_sux1', 'trees_bark_sux2', 'trees_bark_sux3',
         'trees_bark_topol', 'trees_elkabark'}


def main():
    plan = json.load(open(os.path.join(WORK, 'twok_plan.json')))
    log = []
    # Two-K: everything the plan does not replace
    for rel, why in plan['removes']:
        if is_shared(rel) and remove16(rel):
            log.append((rel, why))
    bump_bases = {j['rel'][:-len('_bump.dds')] for j in plan['jobs'] if j['kind'] == 'bump'}
    for j in plan['jobs']:
        if j['kind'] == 'bump#' and j['rel'][:-len('_bump#.dds')] not in bump_bases and is_shared(j['rel']):
            remove16(j['rel'])
            log.append((j['rel'], 'Two-K error map without a replaced bump: Dead Air pair kept'))
    # AN4: descriptions, and textures it did not make sharper
    trees = os.path.join(BORROWED, 'textures', 'trees')
    for f in sorted(os.listdir(trees)):
        rel = 'textures/trees/' + f
        stem = os.path.splitext(f)[0].replace('_bump#', '').replace('_bump', '')
        if stem in BARK8 or not is_shared(rel):
            continue
        if f.endswith('.thm'):
            remove16(rel)
            log.append((rel, 'AN4 description: Dead Air\'s own applies'))
            continue
        o = orig_path(rel)
        from PIL import Image
        if o and Image.open(os.path.join(BORROWED, rel)).size[0] * Image.open(os.path.join(BORROWED, rel)).size[1] <= \
                Image.open(o).size[0] * Image.open(o).size[1]:
            remove16(rel)
            log.append((rel, 'AN4 not sharper than Dead Air'))
    with open(os.path.join(WORK, 'removed_1.6.0.txt'), 'a', encoding='utf-8') as f:
        for rel, why in log:
            f.write(f'{rel}\t{why}\n')
    print(len(log), 'removed')


if __name__ == '__main__':
    main()
