"""The action lists loose .pe files carry in [actions], summed the way the game's load summary sums them.

    pe_actions.py sum <file.pe | @list> ...    -> "<count> <bytes> <crc>": the lists of the files that carry one,
                                                  the CRC-32 of each list XORed together (lower-case hex, 8 digits);
                                                  @list names a file of paths, one a line
    pe_actions.py names <particles.xr>         -> every effect name of the archive, one a line

Run with python -I; it reads only the files it is given.
"""
import struct
import sys
import zlib


def actions_of(path):
    sect, vals = None, {}
    for raw in open(path, "rb").read().decode("cp1251").splitlines():
        line = raw.split(";", 1)[0].strip()
        if line.startswith("["):
            sect = line[1:line.index("]")].strip().lower()
        elif sect == "actions" and "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
    if sect is None and not vals:
        return None
    if "size" not in vals:
        return None
    size = int(vals["size"])
    data = "".join(vals.get(f"data_{i:04d}", "") for i in range((size + 1023) // 1024))
    return bytes.fromhex(data)


def chunks(data, pos=0):
    while pos + 8 <= len(data):
        cid, size = struct.unpack_from("<II", data, pos)
        yield cid, data[pos + 8:pos + 8 + size]
        pos += 8 + size


def find(data, cid):
    for c, body in chunks(data):
        if c & 0x7FFFFFFF == cid:
            return body
    return None


def main(argv):
    if len(argv) >= 2 and argv[0] == "sum":
        paths = []
        for a in argv[1:]:
            # @<file>: the paths one a line, for more than a command line holds
            if a.startswith("@"):
                paths += [l.strip() for l in open(a[1:], encoding="utf-8-sig") if l.strip()]
            else:
                paths.append(a)
        count, total, crc = 0, 0, 0
        for path in paths:
            b = actions_of(path)
            if b is None:
                continue
            count += 1
            total += len(b)
            crc ^= zlib.crc32(b) & 0xFFFFFFFF
        print(f"{count} {total} {crc:08x}")
        return 0
    if len(argv) == 2 and argv[0] == "names":
        data = open(argv[1], "rb").read()
        for _, body in chunks(find(data, 3) or b""):
            print(find(body, 2).split(b"\0")[0].decode("cp1251"))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
