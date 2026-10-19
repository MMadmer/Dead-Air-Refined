"""Batch: upscaled bump pairs for every Two-K bump that was sharper than Dead Air's, with detail from our new diffuse."""
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import bump
from common import TREE16, WORK, install, is_shared, load_rgba, orig_path


def job(j):
    rel = j['rel']
    base = rel[:-len('_bump.dds')]
    relx = base + '_bump#.dds'
    n, gloss, height = bump.decode(orig_path(rel), orig_path(relx))
    # Detail comes from the diffuse the player will see: ours when we made one, else Dead Air's
    diff_rel = base + '.dds'
    ours = os.path.join(TREE16, diff_rel)
    if os.path.isfile(ours) and not is_shared(diff_rel):
        diffuse = load_rgba(ours)[..., :3]
    elif orig_path(diff_rel):
        diffuse = load_rgba(orig_path(diff_rel))[..., :3]
    else:
        diffuse = None
    n, gloss, height = bump.upscale_pair(n, gloss, height, j['w'], j['h'], diffuse)
    out = os.path.join(WORK, 'bumps')
    os.makedirs(out, exist_ok=True)
    key = base[len('textures/'):].replace('/', '__')
    ob, ox = os.path.join(out, key + '_b.dds'), os.path.join(out, key + '_x.dds')
    bump.encode(n, gloss, height, ob, ox)
    install(ob, rel)
    install(ox, relx)
    return rel


def main():
    plan = json.load(open(os.path.join(WORK, sys.argv[1])))
    done_path = os.path.join(WORK, 'bumps_done.txt')
    done = set(open(done_path).read().split()) if os.path.exists(done_path) else set()
    jobs = [j for j in plan['jobs'] if j['kind'] == 'bump' and j['rel'] not in done]
    print(len(jobs), 'bump jobs', flush=True)
    with ProcessPoolExecutor(max_workers=10) as ex, open(done_path, 'a') as log:
        for k, (j, f) in enumerate(zip(jobs, [ex.submit(job, j) for j in jobs])):
            try:
                log.write(f.result() + '\n')
                log.flush()
            except Exception as e:
                print('ERROR', j['rel'], repr(e), flush=True)
            if (k + 1) % 50 == 0:
                print(f'{k + 1}/{len(jobs)}', flush=True)
    print('all done', flush=True)


if __name__ == '__main__':
    main()
