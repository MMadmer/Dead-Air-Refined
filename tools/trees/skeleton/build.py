# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Skeleton files of the 1.6.0 tree upgrade (tools/trees/FORMAT.md, CONTRACT section 4).

    python -I tools/trees/skeleton/build.py              all stages (plan, collision sample, skeletons, summary)
    python -I tools/trees/skeleton/build.py <skel> ...   only these skeletons (and the summary)
    options: --jobs N (worker processes; default DA_THREADS or cores - 2), --no-report,
             --force (ignore the cache), --stage plan|collide|skeletons|summary

Stages: plan.py (kinds -> skeleton ids), collide.py (level.cform of sampled placements), job.py per skeleton
(axes, refit, patches, gate, cards, crown field, .dtsk, report sheet), summary.py (fit.csv, README.md).
A skeleton is rebuilt only when its cache key changed: the hash of its inputs (plan entry, the stock mesh files of
its kinds, its collision sample, its card textures) and of the tool sources that shape the output."""
import argparse
import hashlib
import json
import multiprocessing as mp
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import common as C  # noqa: E402

DEFAULT_JOBS = max(1, (os.cpu_count() or 4) - 2)
# Sources that do not shape a skeleton's file or sheet (orchestration, summaries, checks, previews)
NOT_HASHED = {'build.py', 'summary.py', 'check.py', 'preview.py', 'plan.py', 'collide.py'}


def tool_hash():
    h = hashlib.sha1()
    for f in sorted(os.listdir(HERE)):
        if f.endswith('.py') and f not in NOT_HASHED:
            h.update(f.encode())
            h.update(open(os.path.join(HERE, f), 'rb').read().replace(b'\r\n', b'\n'))
    return h.hexdigest()


def input_hash(sid, entry):
    h = hashlib.sha1(json.dumps(entry, sort_keys=True).encode())
    for k in entry['kinds']:
        d = os.path.join(C.INVENTORY, 'meshes', k)
        for f in (f'{k}.npz', f'{k}.json', f'{k}_instances.csv'):
            p = os.path.join(d, f)
            if os.path.exists(p):
                h.update(open(p, 'rb').read())
    p = os.path.join(C.WORK, 'cform', f'{sid}.npz')
    if os.path.exists(p):
        h.update(open(p, 'rb').read())
    for t in entry.get('card_textures', []):
        p = os.path.join(C.INVENTORY, 'textures', t + '.png')
        if os.path.exists(p):
            h.update(open(p, 'rb').read())
    return h.hexdigest()


def cached(sid, key, report):
    p = os.path.join(C.WORK, 'result', f'{sid}.json')
    if not os.path.exists(p) or not os.path.exists(os.path.join(C.OUT_DTSK, f'{sid}.dtsk')):
        return False
    if report and not os.path.exists(os.path.join(C.REPORT_PNG, f'{sid}.png')):
        return False
    try:
        r = json.load(open(p, encoding='utf-8'))
    except (OSError, ValueError):
        return False
    return r.get('cache_key') == key and not r.get('error')


def _one(args):
    sid, entry, report, key = args
    import job
    try:
        res = job.run(sid, entry, write=True, report=report)
    except Exception:
        res = {'skel': sid, 'error': traceback.format_exc(), 'kinds': entry['kinds']}
    res['cache_key'] = key
    C.atomic_json(os.path.join(C.WORK, 'result', f'{sid}.json'), res)
    return sid, res.get('error'), res.get('time')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('skels', nargs='*')
    ap.add_argument('--jobs', type=int, default=int(os.environ.get('DA_THREADS', DEFAULT_JOBS)))
    ap.add_argument('--no-report', action='store_true')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--stage', default=None)
    a = ap.parse_args()
    jobs = max(1, a.jobs)
    t0 = time.time()
    if a.stage in (None, 'plan') and not a.skels:
        import plan
        plan.main()
    if a.stage in (None, 'collide') and not a.skels:
        import collide
        collide.main()
    if a.stage in (None, 'skeletons'):
        p = json.load(open(os.path.join(C.WORK, 'plan.json'), encoding='utf-8'))['skeletons']
        todo = [s for s in p if not a.skels or s in a.skels]
        th = tool_hash()
        keys = {s: hashlib.sha1((th + input_hash(s, p[s])).encode()).hexdigest() for s in todo}
        skipped = [s for s in todo if not a.force and cached(s, keys[s], not a.no_report)]
        todo = [s for s in todo if s not in set(skipped)]
        print(f'{len(todo)} skeletons to build, {len(skipped)} up to date', flush=True)
        # big ones first so the pool drains evenly
        todo.sort(key=lambda s: -(p[s].get('wood_tris', 0) * 4 + p[s].get('card_tris', 0)))
        args = [(s, p[s], not a.no_report, keys[s]) for s in todo]
        done = 0
        if jobs > 1 and len(args) > 1:
            with mp.Pool(jobs, maxtasksperchild=8) as pool:
                for sid, err, dt in pool.imap_unordered(_one, args):
                    done += 1
                    print(f'[{done}/{len(args)}] {sid} {"ERROR" if err else "ok"} {dt or 0:.1f}s', flush=True)
                    if err:
                        print(err, flush=True)
        else:
            for arg in args:
                sid, err, dt = _one(arg)
                done += 1
                print(f'[{done}/{len(args)}] {sid} {"ERROR" if err else "ok"} {dt or 0:.1f}s', flush=True)
                if err:
                    print(err, flush=True)
    if a.stage in (None, 'summary', 'skeletons'):
        import summary
        summary.main()
    print(f'done in {time.time() - t0:.0f}s')


if __name__ == '__main__':
    main()
