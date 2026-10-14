"""The documents the game takes from the repository at each start (src/xrGame/ui/RemoteConfig.cpp).

They are plain files kept out of git - by default the folder `remote` of the main checkout:
  menu.html     the community panel of the main menu and the pause menu; pictures it names by a relative
                path (icons/telegram.png) are put into the page as data: URIs
  services.txt  "key = value" lines: the addresses of the mod's services (report_host, report_path)
They are published encrypted (AES-256-GCM) to the branch `remote` of origin as menu.dar and services.dar,
and the game keeps a copy of them in appdata\\remote. The key is src/xrGame/ui/RemoteConfigSecrets.local.h,
out of git like BugReportSecrets.local.h; a build without it reads no documents.

usage:
  remote_config.py keygen [--force]               a new key, into this checkout and the main one
  remote_config.py build --out DIR [--src DIR]     the encrypted documents into DIR
  remote_config.py show FILE.dar                   a published document as plain text
  remote_config.py publish [--src DIR] [--dry-run] build and push to origin's branch `remote`
A document whose text did not change keeps its published file, so a publish without changes pushes nothing.
"""
import argparse
import base64
import mimetypes
import os
import re
import secrets
import subprocess
import sys

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
SECRETS_REL = os.path.join('src', 'xrGame', 'ui', 'RemoteConfigSecrets.local.h')
BRANCH = 'remote'
FORMAT = 1
# the document ids of RemoteConfig::Document
DOCUMENTS = (('menu', 'menu.html'), ('services', 'services.txt'))
README = b"""# remote

Documents the game takes from here at each start (src/xrGame/ui/RemoteConfig.cpp in `main`): the community
panel of the menus and the addresses of the mod's services. They are encrypted; only the game and
tools/remote/remote_config.py read them. Nothing here is part of a release.
"""


def git(*args, cwd=REPO, data=None):
    out = subprocess.run(['git', *args], cwd=cwd, input=data, capture_output=True, check=True)
    return out.stdout


def main_checkout():
    common = git('rev-parse', '--path-format=absolute', '--git-common-dir').decode().strip()
    return os.path.dirname(common)


def secrets_paths():
    paths = [os.path.join(REPO, SECRETS_REL)]
    main = os.path.join(main_checkout(), SECRETS_REL)
    if os.path.normcase(main) != os.path.normcase(paths[0]):
        paths.append(main)
    return paths


def read_key():
    for path in secrets_paths():
        if not os.path.exists(path):
            continue
        text = open(path, encoding='utf-8').read()
        mask = int(re.search(r'mask = (0x[0-9a-fA-F]+)', text).group(1), 16)
        body = re.search(r'scattered\[32\] = \{([^}]*)\}', text).group(1)
        scattered = [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', body)]
        return bytes(b ^ mask ^ ((i * 0x3B) & 0xFF) for i, b in enumerate(scattered))
    sys.exit('no key: run "remote_config.py keygen" first')


def write_key(key, force):
    mask = secrets.randbelow(255) + 1
    scattered = [b ^ mask ^ ((i * 0x3B) & 0xFF) for i, b in enumerate(key)]
    rows = ',\n        '.join(', '.join('0x%02X' % v for v in scattered[r:r + 8]) for r in range(0, 32, 8))
    text = ('// The key of the documents the game takes from the repository (RemoteConfig.cpp). Written by\n'
            '// tools/remote/remote_config.py keygen; kept out of git, like BugReportSecrets.local.h.\n'
            '#pragma once\n\n'
            'namespace RemoteConfigSecrets\n{\n'
            'inline void Key(u8 (&key)[32])\n{\n'
            '    constexpr u8 mask = 0x%02X;\n'
            '    constexpr u8 scattered[32] = {\n        %s};\n'
            '    for (size_t i = 0; i < 32; ++i)\n'
            '        key[i] = u8(scattered[i] ^ mask ^ u8(i * 0x3B));\n'
            '}\n}\n') % (mask, rows)
    for path in secrets_paths():
        if os.path.exists(path) and not force:
            sys.exit('%s exists; --force replaces the key, and every copy published with the old one stops opening' % path)
    for path in secrets_paths():
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print('key ->', path)


def header(doc_id):
    return b'DARC' + bytes([FORMAT, doc_id, 0, 0])


def encrypt(key, doc_id, plain):
    nonce = secrets.token_bytes(12)
    sealed = AESGCM(key).encrypt(nonce, plain, header(doc_id))
    return header(doc_id) + nonce + sealed[-16:] + sealed[:-16]


def decrypt(key, data):
    if len(data) < 36 or data[:4] != b'DARC' or data[4] != FORMAT:
        raise ValueError('not a document of this format')
    nonce, tag, body = data[8:20], data[20:36], data[36:]
    return data[5], AESGCM(key).decrypt(nonce, body + tag, data[:8])


def inline_pictures(html, src):
    """src="icons/x.png" and url(icons/x.png) become data: URIs; addresses with a scheme stay."""
    def data_uri(rel):
        path = os.path.join(src, rel.replace('/', os.sep))
        mime = mimetypes.guess_type(path)[0] or 'application/octet-stream'
        with open(path, 'rb') as f:
            return 'data:%s;base64,%s' % (mime, base64.b64encode(f.read()).decode())

    def local(rel):
        return not re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', rel) and not rel.startswith('//')

    html = re.sub(r'(\bsrc\s*=\s*")([^"]+)(")',
                  lambda m: m.group(1) + (data_uri(m.group(2)) if local(m.group(2)) else m.group(2)) + m.group(3), html)
    html = re.sub(r'(url\(\s*["\']?)([^"\')]+)(["\']?\s*\))',
                  lambda m: m.group(1) + (data_uri(m.group(2)) if local(m.group(2)) else m.group(2)) + m.group(3), html)
    return html


def plain_documents(src):
    docs = {}
    for doc_id, (name, source) in enumerate(DOCUMENTS):
        path = os.path.join(src, source)
        if not os.path.exists(path):
            print('(%s: no %s, not published)' % (name, path))
            continue
        text = open(path, encoding='utf-8').read()
        if name == 'menu':
            text = inline_pictures(text, src)
        docs[name] = (doc_id, text.encode('utf-8'))
    return docs


def sealed_documents(key, src, previous):
    """name.dar -> bytes; a document whose text is the published one keeps the published file."""
    out = {}
    for name, (doc_id, plain) in plain_documents(src).items():
        old = previous.get(name + '.dar')
        if old:
            try:
                if decrypt(key, old) == (doc_id, plain):
                    out[name + '.dar'] = old
                    print('%s: unchanged' % name)
                    continue
            except Exception:
                pass
        out[name + '.dar'] = encrypt(key, doc_id, plain)
        print('%s: %d byte(s) of text' % (name, len(plain)))
    return out


def published():
    """The tip of origin's branch and its files, or (None, {})."""
    if not git('ls-remote', '--heads', 'origin', BRANCH).strip():
        return None, {}
    git('fetch', '--quiet', 'origin', 'refs/heads/%s' % BRANCH)
    tip = git('rev-parse', 'FETCH_HEAD').decode().strip()
    files = {}
    for line in git('ls-tree', tip).decode().splitlines():
        meta, name = line.split('\t', 1)
        files[name] = git('cat-file', 'blob', meta.split()[2])
    return tip, files


def cmd_build(args):
    key = read_key()
    os.makedirs(args.out, exist_ok=True)
    previous = {}
    for name in os.listdir(args.out):
        if name.endswith('.dar'):
            previous[name] = open(os.path.join(args.out, name), 'rb').read()
    for name, data in sealed_documents(key, args.src, previous).items():
        with open(os.path.join(args.out, name), 'wb') as f:
            f.write(data)


def cmd_show(args):
    doc_id, plain = decrypt(read_key(), open(args.file, 'rb').read())
    print('document %d (%s)' % (doc_id, DOCUMENTS[doc_id][0]))
    sys.stdout.buffer.write(plain + b'\n')


def cmd_publish(args):
    key = read_key()
    tip, previous = published()
    files = sealed_documents(key, args.src, previous)
    files['README.md'] = README
    entries = []
    for name in sorted(files):
        blob = git('hash-object', '-w', '--stdin', data=files[name]).decode().strip()
        entries.append('100644 blob %s\t%s' % (blob, name))
    tree = git('mktree', data=('\n'.join(entries) + '\n').encode()).decode().strip()
    if tip and git('rev-parse', tip + '^{tree}').decode().strip() == tree:
        print('nothing changed, nothing pushed')
        return
    parents = ['-p', tip] if tip else []
    commit = git('commit-tree', tree, *parents, '-m', 'The documents the game takes at each start').decode().strip()
    if args.dry_run:
        print('would push', commit, 'to origin/%s' % BRANCH)
        return
    git('push', '--quiet', 'origin', '%s:refs/heads/%s' % (commit, BRANCH))
    print('pushed', commit[:9], 'to origin/%s' % BRANCH)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('keygen')
    p.add_argument('--force', action='store_true')
    p.set_defaults(fn=lambda a: write_key(secrets.token_bytes(32), a.force))
    default_src = os.path.join(main_checkout(), 'remote')
    p = sub.add_parser('build')
    p.add_argument('--src', default=default_src)
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_build)
    p = sub.add_parser('show')
    p.add_argument('file')
    p.set_defaults(fn=cmd_show)
    p = sub.add_parser('publish')
    p.add_argument('--src', default=default_src)
    p.add_argument('--dry-run', action='store_true')
    p.set_defaults(fn=cmd_publish)
    args = parser.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
