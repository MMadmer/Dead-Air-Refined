"""What a save group holds, read the way the base game's reader takes it: the ALife object records of a .scop
(id, parent, section, name, and for a placeholder its custom data, flags and shapes) and the chunk types of a
.scov. Prints JSON. tools/qa/saves/Run-ModuleSavesQa.ps1 judges by it; it needs no game.

    python scop_records.py <save.scop> [<save.scov>]
"""

import json
import struct
import sys

OBJECT_CHUNK_DATA = 0x0002
M_SPAWN = 1
M_SPAWN_VERSION = 1 << 5
PLACEHOLDER_SECTION = "space_restrictor"
PLACEHOLDER_PREFIX = "xms_ph_"
FOOTER_MAGIC = 0x53343644  # the protected .scop footer, saved_game_wrapper.cpp
FOOTER_SIZE = 28


def lzo1x_decompress(src, out_len):
    """LZO1X-1 as lzo1x_d.ch decodes it."""
    out = bytearray()
    ip = 0

    def copy_match(dist_from_end, length):
        start = len(out) - dist_from_end
        if start < 0:
            raise ValueError("lookbehind overrun")
        for k in range(length):
            out.append(out[start + k])

    state = "outer"
    t = 0
    if src[ip] > 17:
        t = src[ip] - 17
        ip += 1
        if t < 4:
            state = "match_next"
        else:
            out += src[ip:ip + t]
            ip += t
            state = "first_literal_run"
    while True:
        if state == "outer":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255
                    ip += 1
                t += 15 + src[ip]
                ip += 1
            out += src[ip:ip + t + 3]
            ip += t + 3
            state = "first_literal_run"
        elif state == "first_literal_run":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            dist = 1 + 0x0800 + (t >> 2) + (src[ip] << 2)
            ip += 1
            copy_match(dist, 3)
            state = "match_done"
        elif state == "match":
            if t >= 64:
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
                ip += 1
                copy_match(dist, (t >> 5) + 1)
            elif t >= 32:
                t &= 31
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 31 + src[ip]
                    ip += 1
                dist = 1 + (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                copy_match(dist, t + 2)
            elif t >= 16:
                dist = (t & 8) << 11
                t &= 7
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 7 + src[ip]
                    ip += 1
                dist += (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                if dist == 0:
                    break
                copy_match(dist + 0x4000, t + 2)
            else:
                dist = 1 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 2)
            state = "match_done"
        elif state == "match_done":
            t = src[ip - 2] & 3
            if t == 0:
                state = "outer"
                continue
            state = "match_next"
        elif state == "match_next":
            out += src[ip:ip + t]
            ip += t
            t = src[ip]
            ip += 1
            state = "match"
    if len(out) != out_len:
        raise ValueError(f"decompressed {len(out)} bytes, the header says {out_len}")
    return bytes(out)


class Reader:
    def __init__(self, data, at=0, end=None):
        self.data = data
        self.at = at
        self.end = len(data) if end is None else end

    def take(self, fmt):
        size = struct.calcsize(fmt)
        if self.at + size > self.end:
            raise ValueError("record overrun")
        value = struct.unpack_from("<" + fmt, self.data, self.at)
        self.at += size
        return value[0] if len(value) == 1 else value

    def string(self):
        zero = self.data.index(0, self.at, self.end)
        text = self.data[self.at:zero].decode("cp1251", errors="replace")
        self.at = zero + 1
        return text


def source_of(scop):
    marker, version, source = struct.unpack_from("<III", scop, 0)
    if marker != 0xFFFFFFFF:
        raise ValueError("no .scop")
    size = source & 0x7FFFFFFF
    end = len(scop)
    if len(scop) >= 12 + FOOTER_SIZE and struct.unpack_from("<I", scop, len(scop) - FOOTER_SIZE)[0] == FOOTER_MAGIC:
        end -= FOOTER_SIZE
    return version, lzo1x_decompress(scop[12:end], size)


def records(source):
    at = 0
    while at + 8 <= len(source):
        chunk, size = struct.unpack_from("<II", source, at)
        at += 8
        if chunk & 0x7FFFFFFF == OBJECT_CHUNK_DATA:
            break
        at += size
    else:
        raise ValueError("no object chunk")
    reader = Reader(source, at, at + size)
    count = reader.take("I")
    out = []
    for _ in range(count):
        spawn_size = reader.take("H")
        spawn = Reader(source, reader.at, reader.at + spawn_size)
        reader.at += spawn_size
        update_size = reader.take("H")
        reader.at += update_size
        if spawn.take("H") != M_SPAWN:
            raise ValueError("a record without M_SPAWN")
        record = {"section": spawn.string(), "name": spawn.string()}
        spawn.take("BB")
        spawn.take("ffffff")
        spawn.take("H")
        record["id"], record["parent"] = spawn.take("HH")
        spawn.take("H")
        flags = spawn.take("H")
        version = spawn.take("H") if flags & M_SPAWN_VERSION else 0
        if version > 120:
            spawn.take("H")
        if version > 69:
            spawn.take("H")
        client = spawn.take("H") if version > 93 else spawn.take("B")
        spawn.at += client
        record["spawn_id"] = spawn.take("H") if version > 79 else 0xFFFF
        spawn.take("H")
        if record["section"] == PLACEHOLDER_SECTION and record["name"].startswith(PLACEHOLDER_PREFIX):
            record["graph"] = spawn.take("H")
            spawn.take("f")
            spawn.take("I")
            spawn.take("I")
            record["flags"] = spawn.take("I")
            record["custom_data"] = spawn.string()
            spawn.take("II")
            shapes = spawn.take("B")
            record["shapes"] = shapes
            for _ in range(shapes):
                spawn.at += 1 + (16 if source[spawn.at] == 0 else 48)
            record["restrictor_type"] = spawn.take("B")
            record["client_data"] = client
        out.append(record)
    if reader.at != reader.end:
        raise ValueError("the object chunk does not end after its records")
    return out


def scov_chunks(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, save_id = struct.unpack_from("<IIQ", data, 0)
    if magic != 0x31564F53 or version != 3:
        raise ValueError("no .scov v3")
    count, entry, directory = struct.unpack_from("<IIQ", data, 56)
    chunks = []
    for i in range(count):
        kind, chunk_version, flags, offset, size = struct.unpack_from("<IHHQQ", data, directory + i * entry)
        chunks.append({"type": f"{kind:08x}", "version": chunk_version, "size": size})
    return {"save_id": f"{save_id:016x}", "chunks": chunks}


def main():
    with open(sys.argv[1], "rb") as f:
        version, source = source_of(f.read())
    result = {"version": version, "records": records(source)}
    if len(sys.argv) > 2:
        result["scov"] = scov_chunks(sys.argv[2])
    json.dump(result, sys.stdout, ensure_ascii=False)


if __name__ == "__main__":
    main()
