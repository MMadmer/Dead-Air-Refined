"""Put this checkout's build and its gamedata overlay into the live install (D:\\Games\\Dead Air).

Every file goes in by rename from a temp copy next to it, so a hardlink shared with the live game is
replaced, never written through. Content bundles are deployed separately by deploy_dead_air_x64_content.ps1.
"""
import hashlib
import os
import shutil
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DST = r"D:/Games/Dead Air"


def put(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.deploy-part'
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    rel_bin = os.path.join(REPO, 'bin', 'x64', 'Release')
    names = [l.strip() for l in open(os.path.join(REPO, 'packaging', 'dead-air-x64', 'installer', 'runtime-files.txt'), encoding='utf-8')
             if l.strip() and not l.startswith('#')]
    n, missing = 0, []
    for name in names:
        src = os.path.join(rel_bin, name)
        if not os.path.isfile(src):
            missing.append(name)
            continue
        put(src, os.path.join(DST, name))
        n += 1
    # pdbs the live install carries, so a crash log has symbols
    for f in os.listdir(DST):
        if f.endswith('.pdb') and os.path.isfile(os.path.join(rel_bin, f)):
            put(os.path.join(rel_bin, f), os.path.join(DST, f))
    over = os.path.join(REPO, 'packaging', 'dead-air-x64', 'compatibility', 'gamedata')
    m = 0
    deployed = []
    for dp, _, fs in os.walk(over):
        for f in fs:
            src = os.path.join(dp, f)
            rel = os.path.relpath(src, over).replace('\\', '/')
            deployed.append(rel)
            dst = os.path.join(DST, 'gamedata', rel)
            if not os.path.isfile(dst) or sha(src) != sha(dst):
                put(src, dst)
                m += 1
    # Files the overlay put in before and no longer has go with it (the manifest of the previous deploy).
    manifest = os.path.join(DST, 'gamedata', '.overlay-manifest.txt')
    removed = 0
    if os.path.isfile(manifest):
        current = set(deployed)
        for rel in (l.strip() for l in open(manifest, encoding='utf-8')):
            dst = os.path.join(DST, 'gamedata', rel)
            if rel and rel not in current and os.path.isfile(dst):
                os.remove(dst)
                removed += 1
    with open(manifest + '.part', 'w', encoding='utf-8') as out:
        out.write('\n'.join(sorted(deployed)) + '\n')
    os.replace(manifest + '.part', manifest)
    print(f'runtime files {n}, missing {missing}, overlay files updated {m}, removed {removed}')
    # verify
    bad = [nm for nm in names if os.path.isfile(os.path.join(rel_bin, nm)) and sha(os.path.join(rel_bin, nm)) != sha(os.path.join(DST, nm))]
    print('sha mismatches:', bad)


if __name__ == '__main__':
    main()
