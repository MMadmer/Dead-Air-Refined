#!/usr/bin/env python3
"""Cuts the visuals inside a box out of a compiled level of the game into a level piece of a test module.

The piece (docs/dead-air/XMS_ARCHITECTURE.md 8.3) gets the visuals hanging directly on a sector root whose box lies
inside the cut box, with everything below them, their vertex and index ranges packed into buffers of its own, the
shader table entries they name and the lightmap pages those entries name. The module's overlay_visuals.ltx hides the
originals with the same box, so the level with the module draws what the level without it draws, the cut part
coming from the piece.

    python tools/qa/pieces/cut_piece.py --level l01_escape --box -220,-10,-140,-170,40,-90 --module <folder>
    python tools/qa/pieces/cut_piece.py --level l01_escape --near -200,0,-120 --radius 80      # what is there

The level is read from the game's archives (or its loose gamedata): standard library only.
"""
import argparse
import os
import struct
import sys

BS = chr(92)
MT_NORMAL, MT_HIERRARHY, MT_PROGRESSIVE, MT_LOD, MT_TREE_ST, MT_TREE_PM = 0, 1, 2, 6, 7, 11
STATIC_TYPES = {MT_NORMAL, MT_HIERRARHY, MT_PROGRESSIVE, MT_LOD, MT_TREE_ST, MT_TREE_PM}
OGF_HEADER, OGF_CHILDREN_L, OGF_SWICONTAINER, OGF_GCONTAINER, OGF_FASTPATH = 1, 10, 20, 21, 22
FSL_HEADER, FSL_SHADERS, FSL_VISUALS, FSL_SECTORS, FSL_VB, FSL_IB, FSL_SWIS = 1, 2, 3, 8, 9, 10, 11
FSP_ROOT = 2
PIECE_ROOTS = 32
DECL_SIZES = {0: 4, 1: 8, 2: 12, 3: 16, 4: 4, 5: 4, 6: 4, 7: 8, 8: 4, 9: 4, 10: 8, 11: 4, 12: 8, 13: 4, 14: 4,
              15: 4, 16: 8}


def die(message):
    print(f"cut_piece: {message}", file=sys.stderr)
    sys.exit(2)


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


# ---------------------------------------------------------------------------------------------- the game's archives
def lzhuf_decode(src):
    """The archives' index codec (xrCore LzHuf.cpp, Okumura's LZHUF), decoding only."""
    N, F, THRESHOLD = 4096, 60, 2
    N_CHAR = 256 - THRESHOLD + F
    T = N_CHAR * 2 - 1
    R = T - 1
    MAX_FREQ = 0x4000
    d_code = ([0] * 32 + [1] * 16 + [2] * 16 + [3] * 16 + [4] * 8 + [5] * 8 + [6] * 8 + [7] * 8 + [8] * 8 + [9] * 8 +
              [10] * 8 + [11] * 8 + [i for i in range(12, 24) for _ in range(4)] +
              [i for i in range(24, 48) for _ in range(2)] + list(range(48, 64)))
    d_len = [3] * 32 + [4] * 48 + [5] * 64 + [6] * 48 + [7] * 48 + [8] * 16
    freq = [0] * (T + 1)
    prnt = [0] * (T + N_CHAR)
    son = [0] * T
    for i in range(N_CHAR):
        freq[i] = 1
        son[i] = i + T
        prnt[i + T] = i
    i, j = 0, N_CHAR
    while j <= R:
        freq[j] = freq[i] + freq[i + 1]
        son[j] = i
        prnt[i] = prnt[i + 1] = j
        i += 2
        j += 1
    freq[T] = 0xffff
    prnt[R] = 0

    pos = 4
    state = {'buf': 0, 'len': 0}

    def fill():
        nonlocal pos
        while state['len'] <= 8:
            c = src[pos] if pos < len(src) else 0
            pos += 1
            state['buf'] |= c << (8 - state['len'])
            state['len'] += 8

    def get_bit():
        fill()
        v = state['buf']
        state['buf'] = (state['buf'] << 1) & 0xffff
        state['len'] -= 1
        return (v & 0x8000) >> 15

    def get_byte():
        fill()
        v = state['buf']
        state['buf'] = (state['buf'] << 8) & 0xffff
        state['len'] -= 8
        return (v & 0xff00) >> 8

    def reconst():
        j = 0
        for i in range(T):
            if son[i] >= T:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        i, j = 0, N_CHAR
        while j < T:
            f = freq[j] = freq[i] + freq[i + 1]
            k = j - 1
            while f < freq[k]:
                k -= 1
            k += 1
            freq[k + 1:j + 1] = freq[k:j]
            freq[k] = f
            son[k + 1:j + 1] = son[k:j]
            son[k] = i
            i += 2
            j += 1
        for i in range(T):
            k = son[i]
            if k >= T:
                prnt[k] = i
            else:
                prnt[k] = prnt[k + 1] = i

    def update(c):
        if freq[R] == MAX_FREQ:
            reconst()
        c = prnt[c + T]
        while True:
            freq[c] += 1
            k = freq[c]
            if k > freq[c + 1]:
                l = c + 1
                while k > freq[l + 1]:
                    l += 1
                freq[c] = freq[l]
                freq[l] = k
                i = son[c]
                prnt[i] = l
                if i < T:
                    prnt[i + 1] = l
                j = son[l]
                son[l] = i
                prnt[j] = c
                if j < T:
                    prnt[j + 1] = c
                son[c] = j
                c = l
            c = prnt[c]
            if c == 0:
                break

    size = struct.unpack_from('<I', src, 0)[0]
    out = bytearray()
    text = bytearray(b' ' * (N - F)) + bytearray(F)
    r = N - F
    while len(out) < size:
        c = son[R]
        while c < T:
            c = son[c + get_bit()]
        c -= T
        update(c)
        if c < 256:
            out.append(c)
            text[r] = c
            r = (r + 1) & (N - 1)
        else:
            i = get_byte()
            p = d_code[i] << 6
            n = d_len[i] - 2
            while n:
                i = (i << 1) + get_bit()
                n -= 1
            p |= i & 0x3f
            i = (r - p - 1) & (N - 1)
            for k in range(c - 255 + THRESHOLD):
                ch = text[(i + k) & (N - 1)]
                out.append(ch)
                text[r] = ch
                r = (r + 1) & (N - 1)
    return bytes(out[:size])


def archive_index(path):
    entries = {}
    with open(path, 'rb') as f:
        size = os.path.getsize(path)
        off = 0
        while off + 8 <= size:
            f.seek(off)
            kind, length = struct.unpack('<II', f.read(8))
            if kind & 0x7fffffff == 1:
                data = f.read(length)
                if kind & 0x80000000:
                    data = lzhuf_decode(data)
                p = 0
                while p + 2 <= len(data):
                    entry_size, real, packed, _crc = struct.unpack_from('<HIII', data, p)
                    name = data[p + 14:p + 14 + entry_size - 16].decode('cp1251').lower()
                    ptr = struct.unpack_from('<I', data, p + 14 + entry_size - 16)[0]
                    entries[name] = (ptr, real, packed)
                    p += entry_size + 2
                break
            off += 8 + length
    return entries


class GameFiles:
    """The level's files as the game finds them: loose gamedata first, then the archives."""

    def __init__(self, game, level):
        self.game, self.level = game, level
        self.prefix = f'levels{BS}{level}{BS}'
        self.files = {}
        database = os.path.join(game, 'database')
        for name in sorted(os.listdir(database)):
            archive = os.path.join(database, name)
            if not os.path.isfile(archive):
                continue
            try:
                index = archive_index(archive)
            except (OSError, struct.error, IndexError, UnicodeDecodeError):
                continue
            for entry, where in index.items():
                if entry.startswith(self.prefix):
                    self.files[entry[len(self.prefix):]] = (archive, where)

    def read(self, rel, required=True):
        loose = os.path.join(self.game, 'gamedata', 'levels', self.level, rel)
        if os.path.isfile(loose):
            return open(loose, 'rb').read()
        hit = self.files.get(rel.lower())
        if not hit:
            if required:
                die(f'the game has no levels{BS}{self.level}{BS}{rel}')
            return None
        archive, (ptr, real, packed) = hit
        if real != packed:
            die(f'{rel} is compressed in {archive}; the cutter reads stored entries only')
        with open(archive, 'rb') as f:
            f.seek(ptr)
            return f.read(real)


# ---------------------------------------------------------------------------------------------- chunks
def chunks(buf, off=0, end=None):
    end = len(buf) if end is None else end
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, off)
        out.append((cid & 0x7FFFFFFF, off + 8, size))
        off += 8 + size
    return out


def chunk_map(buf, off=0, end=None):
    return {cid: (o, s) for cid, o, s in chunks(buf, off, end)}


def write_chunk(out, cid, data):
    out += struct.pack('<II', cid, len(data))
    out += data


def decl_length(data, off):
    n = 0
    while data[off + n * 8] != 0xFF:
        n += 1
        if n > 64:
            die('a vertex declaration without its end')
    return n + 1


def decl_stride(data, off, elements):
    stride = 0
    for e in range(elements - 1):
        stream, offset, kind = struct.unpack_from('<HHB', data, off + e * 8)
        if stream == 0:
            stride = max(stride, offset + DECL_SIZES[kind])
    return stride


def read_buffers(geom):
    top = chunk_map(geom)
    vbs, ibs, swis = [], [], []
    o, s = top[FSL_VB]
    count = struct.unpack_from('<I', geom, o)[0]
    p = o + 4
    for _ in range(count):
        n = decl_length(geom, p)
        decl = geom[p:p + n * 8]
        stride = decl_stride(geom, p, n)
        p += n * 8
        vcount = struct.unpack_from('<I', geom, p)[0]
        p += 4
        vbs.append({'decl': decl, 'stride': stride, 'count': vcount, 'data': geom[p:p + vcount * stride]})
        p += vcount * stride
    o, s = top[FSL_IB]
    count = struct.unpack_from('<I', geom, o)[0]
    p = o + 4
    for _ in range(count):
        icount = struct.unpack_from('<I', geom, p)[0]
        p += 4
        ibs.append({'count': icount, 'data': geom[p:p + icount * 2]})
        p += icount * 2
    if FSL_SWIS in top:
        o, s = top[FSL_SWIS]
        count = struct.unpack_from('<I', geom, o)[0]
        p = o + 4
        for _ in range(count):
            reserved = geom[p:p + 16]
            n = struct.unpack_from('<I', geom, p + 16)[0]
            swis.append(reserved + geom[p + 16:p + 20 + n * 8])
            p += 20 + n * 8
    return vbs, ibs, swis


class Packer:
    """New buffers holding only the ranges the piece's visuals use, one new buffer per source buffer."""

    def __init__(self, vbs, ibs):
        self.src_vb, self.src_ib = vbs, ibs
        self.vb_map, self.ib_map = {}, {}  # source id -> new id
        self.vb_out, self.ib_out = [], []
        self.ranges = {}

    def place(self, vb, vbase, vcount, ib, ibase, icount):
        key = (vb, vbase, vcount, ib, ibase, icount)
        if key in self.ranges:
            return self.ranges[key]
        if vb not in self.vb_map:
            self.vb_map[vb] = len(self.vb_out)
            self.vb_out.append({'decl': self.src_vb[vb]['decl'], 'stride': self.src_vb[vb]['stride'],
                                'data': bytearray(), 'count': 0})
        if ib not in self.ib_map:
            self.ib_map[ib] = len(self.ib_out)
            self.ib_out.append({'data': bytearray(), 'count': 0})
        nv, ni = self.vb_out[self.vb_map[vb]], self.ib_out[self.ib_map[ib]]
        stride = nv['stride']
        indices = self.src_ib[ib]['data'][ibase * 2:(ibase + icount) * 2]
        if icount and max(struct.unpack(f'<{icount}H', indices)) >= vcount:
            die(f'an index range reaches past its vertex range (vb {vb}, ib {ib})')
        new_vbase, new_ibase = nv['count'], ni['count']
        nv['data'] += self.src_vb[vb]['data'][vbase * stride:(vbase + vcount) * stride]
        nv['count'] += vcount
        ni['data'] += indices
        ni['count'] += icount
        self.ranges[key] = (self.vb_map[vb], new_vbase, vcount, self.ib_map[ib], new_ibase, icount)
        return self.ranges[key]

    def write(self, swis=None):
        out = bytearray()
        vb = bytearray(struct.pack('<I', len(self.vb_out)))
        for b in self.vb_out:
            vb += b['decl'] + struct.pack('<I', b['count']) + b['data']
        ib = bytearray(struct.pack('<I', len(self.ib_out)))
        for b in self.ib_out:
            ib += struct.pack('<I', b['count']) + b['data']
        write_chunk(out, FSL_VB, vb)
        write_chunk(out, FSL_IB, ib)
        if swis is not None:
            sw = bytearray(struct.pack('<I', len(swis)))
            for item in swis:
                sw += item
            write_chunk(out, FSL_SWIS, sw)
        return bytes(out)


# ---------------------------------------------------------------------------------------------- the level
class Level:
    def __init__(self, files):
        self.files = files
        self.level = files.read('level')
        top = chunk_map(self.level)
        self.header = self.level[top[FSL_HEADER][0]:top[FSL_HEADER][0] + top[FSL_HEADER][1]]
        o, s = top[FSL_SHADERS]
        n = struct.unpack_from('<I', self.level, o)[0]
        p = o + 4
        self.shaders = []
        for _ in range(n):
            q = self.level.index(b'\0', p)
            self.shaders.append(self.level[p:q])
            p = q + 1
        o, s = top[FSL_VISUALS]
        self.visuals = []
        for _cid, vo, vs in chunks(self.level, o, o + s):
            self.visuals.append(self.visual(vo, vs))
        o, s = top[FSL_SECTORS]
        self.sectors = []
        for _cid, so, ss in chunks(self.level, o, o + s):
            sm = chunk_map(self.level, so, so + ss)
            ro, _ = sm[FSP_ROOT]
            self.sectors.append(struct.unpack_from('<I', self.level, ro)[0])

    def visual(self, o, s):
        cm = chunk_map(self.level, o, o + s)
        ho, _ = cm[OGF_HEADER]
        version, kind, shader = struct.unpack_from('<BBH', self.level, ho)
        box = struct.unpack_from('<6f', self.level, ho + 4)
        v = {'off': o, 'size': s, 'type': kind, 'shader': shader, 'box': box, 'chunks': cm, 'children': []}
        if OGF_CHILDREN_L in cm:
            co, _ = cm[OGF_CHILDREN_L]
            n = struct.unpack_from('<I', self.level, co)[0]
            v['children'] = list(struct.unpack_from(f'<{n}I', self.level, co + 4))
        return v


def contained(cut, box):
    return all(cut[i] <= box[i] and box[i + 3] <= cut[i + 3] for i in range(3))


def survey(level, near, radius):
    for s, root in enumerate(level.sectors):
        for c in level.visuals[root]['children']:
            v = level.visuals[c]
            b = v['box']
            centre = [(b[i] + b[i + 3]) / 2 for i in range(3)]
            d = sum((centre[i] - near[i]) ** 2 for i in range(3)) ** 0.5
            if d <= radius:
                shader = level.shaders[v['shader']].decode('cp1251', 'replace')
                print(f'sector {s:3} visual {c:5} type {v["type"]:2} box ' +
                      ','.join(f'{x:.1f}' for x in b) + f'  {shader}')


def subtree(level, index, out):
    if index in out:
        return
    for child in level.visuals[index]['children']:
        subtree(level, child, out)
    out.add(index)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=r'D:\Games\Dead Air')
    ap.add_argument('--level', required=True)
    ap.add_argument('--box', help='minx,miny,minz,maxx,maxy,maxz')
    ap.add_argument('--module', help='the test module folder to write')
    ap.add_argument('--id', default='pieces_qa')
    ap.add_argument('--piece', default='cut')
    ap.add_argument('--stream', default='true', choices=['true', 'false'])
    ap.add_argument('--allow-trees', action='store_true')
    ap.add_argument('--near')
    ap.add_argument('--radius', type=float, default=60)
    args = ap.parse_args()

    files = GameFiles(args.game, args.level)
    if 'level' not in files.files and not os.path.isfile(os.path.join(args.game, 'gamedata', 'levels', args.level,
                                                                      'level')):
        die(f'the game has no level {args.level}')
    level = Level(files)
    if args.near:
        survey(level, [float(x) for x in args.near.split(',')], args.radius)
        return 0
    if not args.box or not args.module:
        die('--box and --module are needed to cut')

    # The box as the engine reads it back from overlay_visuals.ltx: centre and half extents, in floats.
    lo_hi = [float(x) for x in args.box.split(',')]
    centre = [f32((lo_hi[i] + lo_hi[i + 3]) / 2) for i in range(3)]
    half = [f32((lo_hi[i + 3] - lo_hi[i]) / 2) for i in range(3)]
    hide = ', '.join(f'{x:.3f}' for x in centre + half)
    parsed = [f32(float(x)) for x in hide.split(',')]
    cut = [f32(parsed[i] - parsed[i + 3]) for i in range(3)] + [f32(parsed[i] + parsed[i + 3]) for i in range(3)]

    roots = []  # (visual, sector)
    for s, root in enumerate(level.sectors):
        for c in level.visuals[root]['children']:
            if contained(cut, level.visuals[c]['box']):
                roots.append((c, s))
    if not roots:
        die('no visual of a sector root lies inside the box')
    if len({c for c, _ in roots}) != len(roots):
        die('a visual inside the box hangs on two sectors')
    selected = set()
    for c, _ in roots:
        subtree(level, c, selected)
    order = sorted(selected)
    remap = {old: new for new, old in enumerate(order)}
    kinds = {level.visuals[i]['type'] for i in order}
    if not kinds <= STATIC_TYPES:
        die(f'the box holds visuals of types {sorted(kinds - STATIC_TYPES)}')
    if kinds & {MT_TREE_ST, MT_TREE_PM, MT_LOD} and not args.allow_trees:
        die('the box holds vegetation (the procedural vegetation replaces it in the level, not in a piece); '
            'pick another box or pass --allow-trees')

    geom = files.read('level.geom')
    vbs, ibs, swis = read_buffers(geom)
    geomx = files.read('level.geomx', required=False) or files.read('level.geomX', required=False)
    fast_vbs, fast_ibs = (read_buffers(geomx)[:2] if geomx else ([], []))
    packer, fast = Packer(vbs, ibs), Packer(fast_vbs, fast_ibs)
    swi_map, swi_out = {}, []

    shader_map = {0: 0}
    shader_out = [b'']
    blobs = []
    for old in order:
        v = level.visuals[old]
        blob = bytearray(level.level[v['off']:v['off'] + v['size']])
        cm = chunk_map(blob)
        ho, _ = cm[OGF_HEADER]
        sid = struct.unpack_from('<H', blob, ho + 2)[0]
        if sid not in shader_map:
            shader_map[sid] = len(shader_out)
            shader_out.append(level.shaders[sid])
        struct.pack_into('<H', blob, ho + 2, shader_map[sid])
        if OGF_GCONTAINER in cm:
            go, _ = cm[OGF_GCONTAINER]
            struct.pack_into('<6I', blob, go, *packer.place(*struct.unpack_from('<6I', blob, go)))
        if OGF_FASTPATH in cm:
            fo, fs = cm[OGF_FASTPATH]
            inner = chunk_map(blob, fo, fo + fs)
            if not geomx or OGF_GCONTAINER not in inner:
                die(f'visual {old} has fast geometry the level does not')
            go, _ = inner[OGF_GCONTAINER]
            struct.pack_into('<6I', blob, go, *fast.place(*struct.unpack_from('<6I', blob, go)))
        if OGF_SWICONTAINER in cm:
            so, _ = cm[OGF_SWICONTAINER]
            sw = struct.unpack_from('<I', blob, so)[0]
            if sw not in swi_map:
                swi_map[sw] = len(swi_out)
                swi_out.append(swis[sw])
            struct.pack_into('<I', blob, so, swi_map[sw])
        if OGF_CHILDREN_L in cm:
            co, _ = cm[OGF_CHILDREN_L]
            n = struct.unpack_from('<I', blob, co)[0]
            for k in range(n):
                child = struct.unpack_from('<I', blob, co + 4 + 4 * k)[0]
                struct.pack_into('<I', blob, co + 4 + 4 * k, remap[child])
        blobs.append(bytes(blob))

    folder = os.path.join(args.module, 'levels', args.level, 'pieces', args.piece)
    os.makedirs(folder, exist_ok=True)
    for name in os.listdir(folder):
        os.remove(os.path.join(folder, name))

    piece = bytearray()
    write_chunk(piece, FSL_HEADER, level.header)
    table = bytearray(struct.pack('<I', len(shader_out)))
    for entry in shader_out:
        table += entry + b'\0'
    write_chunk(piece, FSL_SHADERS, table)
    visuals = bytearray()
    for i, blob in enumerate(blobs):
        write_chunk(visuals, i, blob)
    write_chunk(piece, FSL_VISUALS, visuals)
    root_chunk = bytearray(struct.pack('<I', len(roots)))
    for c, s in roots:
        root_chunk += struct.pack('<IHH', remap[c], s, 0)
    write_chunk(piece, PIECE_ROOTS, root_chunk)
    open(os.path.join(folder, 'piece'), 'wb').write(piece)
    open(os.path.join(folder, 'piece.geom'), 'wb').write(packer.write(swi_out))
    if fast.vb_out:
        open(os.path.join(folder, 'piece.geomx'), 'wb').write(fast.write())

    # The lightmap pages the piece's shaders name, from the level's folder into the piece's.
    pages = set()
    for entry in shader_out[1:]:
        textures = entry.decode('cp1251').split('/', 1)[1].split(',')
        pages.update(t for t in textures if t.lower().startswith('lmap#'))
    for page in sorted(pages):
        open(os.path.join(folder, page + '.dds'), 'wb').write(files.read(page + '.dds'))

    box = [min(level.visuals[c]['box'][i] for c, _ in roots) for i in range(3)] + \
          [max(level.visuals[c]['box'][i + 3] for c, _ in roots) for i in range(3)]
    with open(os.path.join(folder, 'piece.ltx'), 'w', newline='\r\n') as f:
        f.write('[piece]\nversion = 1\n')
        f.write('box     = ' + ', '.join(f'{x:.3f}' for x in box) + '\n')
        f.write(f'sector  = auto\nstream  = {args.stream}\n')
    with open(os.path.join(args.module, 'levels', args.level, 'overlay_visuals.ltx'), 'w', newline='\r\n') as f:
        f.write(f'[cut_{args.piece}]\nhide = {hide}\n')
    with open(os.path.join(args.module, 'mod.ltx'), 'w', newline='\r\n') as f:
        f.write(f'[module]\nid      = {args.id}\nname    = Level pieces test module\nversion = 1.0.0\n'
                'api     = 1\nmode    = *\n')

    vb_bytes = sum(len(b['data']) for b in packer.vb_out + fast.vb_out)
    print(f'piece {args.id}/{args.piece}: {len(order)} visual(s), {len(roots)} root(s), '
          f'{len(shader_out) - 1} shader(s), {len(packer.vb_out) + len(packer.ib_out)} buffer(s) '
          f'+ {len(fast.vb_out) + len(fast.ib_out)} fast, {vb_bytes // 1024} KiB of vertices, '
          f'{len(pages)} lightmap page(s), box {", ".join(f"{x:.1f}" for x in box)}')
    print(f'hide = {hide}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
