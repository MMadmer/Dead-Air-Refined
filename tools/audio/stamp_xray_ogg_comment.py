"""Writes an X-Ray v3 sound block as user_comments[0] of an .ogg.

X-Ray does not read a KEY=value pair there. CSoundRender_Source::LoadWave takes
user_comments[0] as raw bytes and reads a u32 version followed by the fields
(src/xrSound/SoundRender_Source.cpp). Any file whose first comment is an encoder tag - which
is what ffmpeg, oggenc and every other normal encoder writes - is read as an unrecognised
version and logs "Invalid ogg-comment version" on every load.

That warning is not cosmetic in intent, only in effect: with no readable block the engine
falls back to SoundSourceInfo's defaults (minDist 1, maxDist 300, maxAIDist 300, gameType 0),
which is why such a file still plays. It is the log noise, and a file that cannot state its own
distances, that this fixes. Run it on any .ogg authored outside the SDK before it ships.

    python tools/audio/stamp_xray_ogg_comment.py <file.ogg> <minDist> <maxDist> <volume> <gameType> <maxAIDist>

Re-stamping is idempotent: an existing X-Ray block is replaced, and the encoder's own tags are
kept after it. Only the page carrying the comment header is rebuilt; every other byte of the
file, audio included, is left exactly as it was.
"""
import struct, sys, zlib

def crc32_ogg(data):
    # Ogg uses CRC-32 with polynomial 0x04c11db7, init 0, no reflection, no final xor.
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04c11db7) & 0xffffffff if crc & 0x80000000 else (crc << 1) & 0xffffffff
    return crc

def read_pages(data):
    pages, off = [], 0
    while off < len(data):
        assert data[off:off+4] == b'OggS', 'not an ogg page at %d' % off
        nseg = data[off+26]
        segs = list(data[off+27:off+27+nseg])
        body_at = off + 27 + nseg
        blen = sum(segs)
        pages.append({
            'off': off, 'header': bytearray(data[off:body_at]), 'segs': segs,
            'body': data[body_at:body_at+blen], 'end': body_at+blen,
            'flags': data[off+5], 'granule': data[off+6:off+14],
            'serial': data[off+14:off+18], 'seq': struct.unpack_from('<I', data, off+18)[0],
        })
        off = body_at + blen
    return pages

def split_packets(page):
    packets, cur = [], b''
    pos = 0
    for s in page['segs']:
        cur += page['body'][pos:pos+s]; pos += s
        if s < 255:
            packets.append(cur); cur = b''
    return packets, cur  # cur non-empty => packet continues on the next page

def build_page(flags, granule, serial, seq, packets):
    body, segs = b'', []
    for p in packets:
        n = len(p)
        while True:
            lace = min(255, n)
            segs.append(lace)
            n -= lace
            if lace < 255:
                break
        body += p
    assert len(segs) <= 255, 'packets do not fit one page'
    head = bytearray(b'OggS' + bytes([0]) + bytes([flags]) + granule + serial +
                     struct.pack('<I', seq) + b'\0\0\0\0' + bytes([len(segs)]) + bytes(segs))
    crc = crc32_ogg(bytes(head) + body)
    head[22:26] = struct.pack('<I', crc)
    return bytes(head) + body

def stamp(path, minDist, maxDist, volume, gameType, maxAIDist):
    data = open(path, 'rb').read()
    pages = read_pages(data)

    # Page 0 is the identification header. Page 1 normally carries the comment header and the
    # setup header together; rebuild exactly that page and leave everything else byte-identical.
    packets, tail = split_packets(pages[1])
    assert not tail, 'the comment page continues onto another page - unhandled'
    assert packets and packets[0][:1] == b'\x03' and packets[0][1:7] == b'vorbis', 'no comment header on page 1'

    p, i = packets[0], 7
    vlen = struct.unpack_from('<I', p, i)[0]; i += 4
    vendor = p[i:i+vlen]; i += vlen
    n = struct.unpack_from('<I', p, i)[0]; i += 4
    existing = []
    for _ in range(n):
        l = struct.unpack_from('<I', p, i)[0]; i += 4
        existing.append(p[i:i+l]); i += l

    block = struct.pack('<IfffIf', 3, minDist, maxDist, volume, gameType, maxAIDist)
    # Drop any previous X-Ray block so re-stamping stays idempotent.
    existing = [c for c in existing if not (len(c) == 24 and struct.unpack_from('<I', c, 0)[0] in (1, 2, 3))]
    comments = [block] + existing

    out = b'\x03vorbis' + struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(comments))
    for c in comments:
        out += struct.pack('<I', len(c)) + c
    out += b'\x01'  # framing bit

    rebuilt = build_page(pages[1]['flags'], pages[1]['granule'], pages[1]['serial'],
                         pages[1]['seq'], [out] + packets[1:])
    open(path, 'wb').write(data[:pages[1]['off']] + rebuilt + data[pages[1]['end']:])
    print('stamped %s  minDist=%.1f maxDist=%.1f vol=%.1f gameType=%d maxAIDist=%.1f' %
          (path.rsplit('/', 1)[-1], minDist, maxDist, volume, gameType, maxAIDist))

if __name__ == '__main__':
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    try:
        path = sys.argv[1]
        mn, mx, vol = (float(v) for v in sys.argv[2:5])
        gt = int(sys.argv[5])
        ai = float(sys.argv[6])
    except ValueError as bad:
        sys.exit('bad argument: %s\n%s' % (bad, __doc__))
    stamp(path, mn, mx, vol, gt, ai)
