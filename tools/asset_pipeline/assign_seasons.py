"""Append shard lines for the season sets to the 1.6.0 groups.ltx (append-only table).

Keys go one level below a season (trees, map, ui, grnd, detail) and, for levels, one per level, so no
directory is bigger than a bundle should be. Shards continue after the highest index already in use.
"""
import os
import re
import sys

from common import REPO, TREE16

GROUPS = os.path.join(REPO, 'packaging', 'dead-air-x64', 'content', 'groups.ltx')
TARGET = 400 * 1024 * 1024


def dir_size(p):
    return sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(p) for f in fs)


def main():
    text = open(GROUPS, encoding='utf-8').read()
    used = [int(m) for m in re.findall(r'^\s*textures/[^=]+=\s*(\d+)', text, re.M)]
    have = set(k.strip().lower() for k in re.findall(r'^\s*(textures/[^=]+?)\s*=', text, re.M))
    nxt = max(used) + 1 if used else 0
    root = os.path.join(TREE16, 'textures', 'seasons')
    keys = []
    for season in sorted(os.listdir(root)):
        sd = os.path.join(root, season)
        for sub in sorted(os.listdir(sd)):
            if sub == 'levels':
                for lvl in sorted(os.listdir(os.path.join(sd, sub))):
                    keys.append((f'textures/seasons/{season}/levels/{lvl}', dir_size(os.path.join(sd, sub, lvl))))
            else:
                keys.append((f'textures/seasons/{season}/{sub}', dir_size(os.path.join(sd, sub))))
    lines, acc, shard = [], 0, nxt
    for k, size in keys:
        if k in have:
            continue
        if acc and acc + size > TARGET:
            shard += 1
            acc = 0
        lines.append(f'{k} = {shard:02d}')
        acc += size
    if not lines:
        print('nothing to add')
        return
    with open(GROUPS, 'a', encoding='utf-8') as f:
        if not text.endswith('\n'):
            f.write('\n')
        f.write('\n'.join(lines) + '\n')
    total = sum(s for _, s in keys)
    print(f'{len(lines)} lines, shards {nxt:02d}..{shard:02d}, {total / 2**30:.2f} GB of season content')


if __name__ == '__main__':
    main()
