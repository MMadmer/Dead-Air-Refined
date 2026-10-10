# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Validation of the written skeletons: every upgradable kind (and every exact member key) has its file, every
file parses, has the size FORMAT.md gives and passes dtsk.validate; no stray temp files.

    python tools/trees/skeleton/check.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import common as C  # noqa: E402
import dtsk as DT  # noqa: E402


def main():
    idx = json.load(open(os.path.join(C.OUT, 'skel_index.json'), encoding='utf-8'))
    files = {f[:-5] for f in os.listdir(C.OUT_DTSK) if f.endswith('.dtsk')}
    stray = [f for f in os.listdir(C.OUT_DTSK) if not f.endswith('.dtsk')]
    missing = sorted({s for s in list(idx['kinds'].values()) + list(idx['members'].values()) if s not in files})
    bad = {}
    total = 0
    for sid in sorted(files):
        data = open(os.path.join(C.OUT_DTSK, sid + '.dtsk'), 'rb').read()
        total += len(data)
        try:
            sk = DT.unpack(data)
        except ValueError as e:
            bad[sid] = [str(e)]
            continue
        nx, ny, nz = sk.crown['n']
        exp = DT.expected_size(len(sk.axes), len(sk.rings), len(sk.pverts), len(sk.ptris), len(sk.cards), nx, ny, nz)
        probs = DT.validate(sk)
        if exp != len(data):
            probs.append(f'size {len(data)} != {exp}')
        want = int(sid[1:] if sid.startswith('p') else sid, 16)
        if sk.trunk_key != want:
            probs.append('trunk_key does not match the file name')
        if probs:
            bad[sid] = probs
    unused = sorted(files - set(idx['kinds'].values()))
    print(f'{len(files)} files, {total / 1048576:.1f} MB; kinds {len(idx["kinds"])}, member keys {len(idx["members"])}')
    print(f'missing files: {missing or "none"}; files no kind uses: {unused or "none"}; stray files: {stray or "none"}')
    print(f'invalid: {len(bad)}')
    for sid, probs in bad.items():
        print(f'  {sid}: ' + '; '.join(probs[:4]))
    return not (missing or bad or stray)


if __name__ == '__main__':
    sys.exit(0 if main() else 1)
