#!/usr/bin/env python3
"""Damages a level piece on purpose, one way at a time: what the game has to refuse and go on without.

A module is content from anywhere, so a piece of it may be cut short, or say of itself what is not so. Each way
here is one the loader would have followed past the piece's own data before it checked it
(docs/dead-air/XMS_ARCHITECTURE.md 8.3):

    windows    a progressive visual's first sliding window draws far outside the visual's own indices
    swis       the table of sliding windows in piece.geom claims two thousand million items
    children   a hierarchy loses the list of its children
    tree       a tree's definition is cut to half its size

    python tools/qa/pieces/break_piece.py --piece <the piece's folder> --how windows

The file is changed in place: run it on a copy. Standard library only.
"""
import argparse
import os
import struct
import sys

FSL_VISUALS, FSL_SWIS = 3, 11
OGF_HEADER, OGF_SWIDATA, OGF_CHILDREN_L, OGF_TREEDEF2 = 1, 6, 10, 12
MT_HIERRARHY, MT_PROGRESSIVE, MT_LOD, MT_TREE_ST, MT_TREE_PM = 1, 2, 6, 7, 11
# a chunk id no reader of a visual asks for
NO_CHUNK = 0x7F


def die(message):
    print('break_piece: ' + message, file=sys.stderr)
    sys.exit(2)


def chunks(buf, off, end):
    """(id, where its header starts, where its data starts, size) of every chunk in buf[off:end]."""
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, off)
        if cid & 0x80000000:
            die('a compressed chunk: the cutter writes none')
        out.append((cid, off, off + 8, size))
        off += 8 + size
    return out


def visuals(buf):
    """Every visual of the piece: (index, its type, {chunk id: (header, data, size)})."""
    top = {cid: (data, size) for cid, _, data, size in chunks(buf, 0, len(buf))}
    if FSL_VISUALS not in top:
        die('the piece has no visuals')
    data, size = top[FSL_VISUALS]
    out = []
    for index, _, at, length in chunks(buf, data, data + size):
        inner = {cid: (head, d, s) for cid, head, d, s in chunks(buf, at, at + length)}
        if OGF_HEADER not in inner:
            continue
        kind = buf[inner[OGF_HEADER][1] + 1]
        out.append((index, kind, inner))
    return out


def first(found, kinds, chunk, what):
    for index, kind, inner in found:
        if kind in kinds and chunk in inner:
            return index, inner[chunk]
    die('the piece has no ' + what + ': cut a box that holds one')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--piece', required=True, help="the piece's folder (piece, piece.geom)")
    ap.add_argument('--how', required=True, choices=['windows', 'swis', 'children', 'tree'])
    args = ap.parse_args()

    name = 'piece.geom' if args.how == 'swis' else 'piece'
    path = os.path.join(args.piece, name)
    if not os.path.isfile(path):
        die('no ' + path)
    with open(path, 'rb') as f:
        buf = bytearray(f.read())

    if args.how == 'swis':
        top = {cid: (data, size) for cid, _, data, size in chunks(buf, 0, len(buf))}
        if FSL_SWIS not in top or top[FSL_SWIS][1] < 4:
            die('piece.geom has no sliding windows: cut a box with trees (--allow-trees)')
        struct.pack_into('<I', buf, top[FSL_SWIS][0], 0x7FFFFFFF)
        said = 'the table of sliding windows claims 2147483647 items'
    else:
        found = visuals(buf)
        if args.how == 'windows':
            index, (_, data, size) = first(found, (MT_PROGRESSIVE,), OGF_SWIDATA, 'progressive visual')
            if size < 28:
                die('visual %d has no window to damage' % index)
            # sixteen reserved bytes, the count, then the first window: its offset and its triangles
            struct.pack_into('<IH', buf, data + 20, 0x7FFFFF00, 0xFFFF)
            said = 'visual %d: its first sliding window draws outside its indices' % index
        elif args.how == 'children':
            index, (head, _, _) = first(found, (MT_HIERRARHY, MT_LOD), OGF_CHILDREN_L, 'hierarchy')
            struct.pack_into('<I', buf, head, NO_CHUNK)
            said = 'visual %d: a hierarchy without the list of its children' % index
        else:
            index, (head, _, size) = first(found, (MT_TREE_ST, MT_TREE_PM), OGF_TREEDEF2, 'tree')
            # the definition keeps its place and says it is half as long; what was its second half is a chunk
            # of no known kind, so every chunk after it is found as before
            half = size // 2 // 4 * 4
            if size - half < 8:
                die('visual %d: its definition is too short to cut' % index)
            struct.pack_into('<I', buf, head + 4, half)
            struct.pack_into('<II', buf, head + 8 + half, NO_CHUNK, size - half - 8)
            said = 'visual %d: a tree whose definition is cut to %d bytes of %d' % (index, half, size)

    with open(path, 'wb') as f:
        f.write(buf)
    print('break_piece: ' + said)


if __name__ == '__main__':
    main()
