"""Batch: AI-upscaled, palette-locked replacements for the diffuse jobs of a plan, installed into the 1.6.0 tree.

Resumable: a job with a done marker is skipped. GPU (Real-ESRGAN) works on chunk N+1 while the CPU
finishes chunk N.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

import upscale
from common import REALESRGAN, WORK, install, load_rgba, orig_path, save_png, texconv

CHUNK = 40


def key_of(rel):
    return rel[len('textures/'):-4].replace('/', '__').replace('#', '_hash')


def prep(job, in_dir):
    src = load_rgba(job['src'])
    if (src.shape[1], src.shape[0]) != (job['src_w'], job['src_h']):
        raise RuntimeError(f"{job['rel']}: source size changed")
    if job['w'] > job['src_w']:
        upscale.prepare(src, os.path.join(in_dir, key_of(job['rel']) + '.png'))
    return job['rel']


def fin(job, ai_dir, out_root, done_dir):
    rel, k = job['rel'], key_of(job['rel'])
    src = load_rgba(job['src'])
    ai = os.path.join(ai_dir, k + '.png') if job['w'] > job['src_w'] else None
    seed = int(hashlib.md5(rel.encode()).hexdigest()[:8], 16)
    da_rgba = load_rgba(orig_path(rel))
    base = da_rgba if job['src'] != orig_path(rel) else None
    out, kind = upscale.finish(src, ai, job['w'], job['h'], normal_like=job['kind'] == 'normal', seed=seed, base_rgba=base)
    enc = os.path.join(out_root, k)
    os.makedirs(enc, exist_ok=True)
    stem = os.path.splitext(os.path.basename(rel))[0]
    png = os.path.join(enc, stem + '.png')
    save_png(png, out)
    dds = texconv(png, enc, job['fmt'], job['mips'], alpha_cutout=kind == 'cutout')
    # QA numbers against Dead Air's own file at its size
    da = da_rgba.astype(np.float32)
    small = np.asarray(Image.fromarray(out).resize((da.shape[1], da.shape[0]), Image.LANCZOS), np.float32)
    stats = {'rel': rel, 'w': job['w'], 'h': job['h'], 'fmt': job['fmt'], 'alpha': kind, 'src': job['src'],
             'd_mean': [round(float(x), 2) for x in (small[..., :3].reshape(-1, 3).mean(0) - da[..., :3].reshape(-1, 3).mean(0))],
             'mad': round(float(np.abs(small[..., :3] - da[..., :3]).mean()), 2)}
    install(dds, rel)
    shutil.rmtree(enc, ignore_errors=True)
    json.dump(stats, open(os.path.join(done_dir, k + '.json'), 'w'))
    return rel


def main():
    plan = json.load(open(os.path.join(WORK, sys.argv[1])))
    tag = sys.argv[2]
    base = os.path.join(WORK, tag)
    done_dir = os.path.join(base, 'done')
    os.makedirs(done_dir, exist_ok=True)
    jobs = [j for j in plan['jobs'] if j['kind'] in ('diffuse', 'normal') and not os.path.exists(os.path.join(done_dir, key_of(j['rel']) + '.json'))]
    print(f'{len(jobs)} jobs left', flush=True)
    chunks = [jobs[i:i + CHUNK] for i in range(0, len(jobs), CHUNK)]
    t0 = time.time()
    with ProcessPoolExecutor(max_workers=8) as pool:
        def start_ai(i):
            ind, outd = os.path.join(base, f'in{i}'), os.path.join(base, f'ai{i}')
            shutil.rmtree(ind, ignore_errors=True)
            shutil.rmtree(outd, ignore_errors=True)
            os.makedirs(ind)
            os.makedirs(outd)
            list(pool.map(prep, chunks[i], [ind] * len(chunks[i])))
            if not os.listdir(ind):
                return None
            return subprocess.Popen([REALESRGAN, '-i', ind, '-o', outd, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png', '-g', '1', '-j', '1:2:2'],
                                    stdout=subprocess.DEVNULL, stderr=open(os.path.join(base, f'ai{i}.log'), 'w'))
        proc = start_ai(0) if chunks else None
        for i, chunk in enumerate(chunks):
            if proc and proc.wait() != 0:
                raise RuntimeError('realesrgan failed, see ai*.log in ' + base)
            nxt = start_ai(i + 1) if i + 1 < len(chunks) else None
            futs = [pool.submit(fin, j, os.path.join(base, f'ai{i}'), os.path.join(base, 'enc'), done_dir) for j in chunk]
            errs = []
            for f, j in zip(futs, chunk):
                try:
                    f.result()
                except Exception as e:
                    errs.append(f"{j['rel']}: {e!r}")
            for e in errs:
                print('ERROR', e, flush=True)
            shutil.rmtree(os.path.join(base, f'in{i}'), ignore_errors=True)
            shutil.rmtree(os.path.join(base, f'ai{i}'), ignore_errors=True)
            print(f'chunk {i + 1}/{len(chunks)} done, {time.time() - t0:.0f}s', flush=True)
            proc = nxt
    print('all done', flush=True)


if __name__ == '__main__':
    main()
