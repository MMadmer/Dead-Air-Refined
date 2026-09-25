"""The mod catalog's formats and cryptography (docs/dead-air/MOD_CATALOG.md), standard library only.

Used by the catalog's own tool (catalog.py), its GitHub workflows and the game's QA. The game
verifies with Windows CNG; this module signs and verifies with plain integers, so a signature made
here is checked there - the QA proves the two agree.

    ECDSA P-256 / SHA-256 (FIPS 186-4), deterministic nonces (RFC 6979)
    p256:<base64url X||Y>                      public key
    <16 hex of SHA-256(X||Y)>                  key id
    <key id>:<base64url r||s>                  signature
    ... [signature] key = / sig =               signed text (3.1)
    xms-key 1                                  key file (4.1)
"""
import base64
import hashlib
import hmac
import os
import secrets

# ---- P-256 --------------------------------------------------------------------------------------

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def _add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        slope = (3 * x1 * x1 + A) * pow(2 * y1, P - 2, P) % P
    else:
        slope = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (slope * slope - x1 - x2) % P
    return x3, (slope * (x1 - x3) - y1) % P


def _mul(k, point):
    result = None
    addend = point
    while k:
        if k & 1:
            result = _add(result, addend)
        addend = _add(addend, addend)
        k >>= 1
    return result


def on_curve(point):
    x, y = point
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x + A * x + B)) % P == 0


def _int(data):
    return int.from_bytes(data, "big")


def _bytes(value):
    return value.to_bytes(32, "big")


def public_of(d):
    return _mul(d, G)


def _rfc6979_k(d, digest):
    """RFC 6979 3.2 with HMAC-SHA256 - the nonce is a function of the key and the message."""
    x = _bytes(d)
    h = _bytes(_int(digest) % N)
    v = b"\x01" * 32
    k = b"\x00" * 32
    k = hmac.new(k, v + b"\x00" + x + h, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + x + h, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        candidate = _int(v)
        if 1 <= candidate < N:
            return candidate
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def sign(d, message):
    """r||s, 64 bytes."""
    digest = hashlib.sha256(message).digest()
    z = _int(digest)
    while True:
        k = _rfc6979_k(d, digest)
        r = _mul(k, G)[0] % N
        s = pow(k, N - 2, N) * (z + r * d) % N
        if r and s:
            return _bytes(r) + _bytes(s)


def verify(point, message, signature):
    if len(signature) != 64 or point is None or not on_curve(point):
        return False
    r, s = _int(signature[:32]), _int(signature[32:])
    if not (1 <= r < N and 1 <= s < N):
        return False
    z = _int(hashlib.sha256(message).digest())
    w = pow(s, N - 2, N)
    result = _add(_mul(z * w % N, G), _mul(r * w % N, point))
    return result is not None and result[0] % N == r


def new_private():
    return secrets.randbelow(N - 1) + 1


# ---- text forms -----------------------------------------------------------------------------------

def b64u(data):
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def unb64u(text):
    text = text.strip()
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def key_text(point):
    return "p256:" + b64u(_bytes(point[0]) + _bytes(point[1]))


def parse_key(text):
    text = text.strip()
    if not text.startswith("p256:"):
        raise ValueError("not a p256 key")
    raw = unb64u(text[5:])
    if len(raw) != 64:
        raise ValueError("a p256 key is 64 bytes")
    point = (_int(raw[:32]), _int(raw[32:]))
    if not on_curve(point):
        raise ValueError("the key is not a point of P-256")
    return point


def key_id(point):
    return hashlib.sha256(_bytes(point[0]) + _bytes(point[1])).hexdigest()[:16]


# ---- signed text (MOD_CATALOG 3.1) ---------------------------------------------------------------------

HEADER = "[signature]"


def sign_text(body, d):
    """`body` (str) with its [signature] section; the body must end with a line break."""
    if not body.endswith("\n"):
        body += "\n"
    point = public_of(d)
    signature = sign(d, body.encode("utf-8"))
    return body + "%s\nkey = %s\nsig = %s:%s\n" % (HEADER, key_text(point), key_id(point), b64u(signature))


def split_signed(data):
    """(body bytes, key text, key id, signature bytes) of a signed file, or raise ValueError."""
    at = -1
    start = 0
    while True:
        found = data.find(b"[signature]", start)
        if found < 0:
            break
        start = found + 1
        if found == 0 or data[found - 1:found] != b"\n":
            continue
        rest = data[found + len(HEADER):]
        if rest and rest[:1] not in (b"\n", b"\r"):
            continue
        if at >= 0:
            raise ValueError("more than one [signature] section")
        at = found
    if at < 0:
        raise ValueError("not signed")
    body = data[:at]
    key = sig = None
    for line in data[at + len(HEADER):].decode("utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        name, _, value = line.partition("=")
        name, value = name.strip(), value.strip()
        if name == "key" and key is None:
            key = value
        elif name == "sig" and sig is None:
            sig = value
        else:
            raise ValueError("unexpected content after [signature]")
    if not sig or sig.find(":") != 16:
        raise ValueError("malformed signature")
    signature = unb64u(sig[17:])
    if len(signature) != 64:
        raise ValueError("malformed signature")
    return body, key or "", sig[:16].lower(), signature


def verify_signed(data, point):
    """True when `data` (bytes) carries a valid signature by `point`."""
    body, _, sid, signature = split_signed(data)
    if sid != key_id(point):
        raise ValueError("signed by key %s, expected %s" % (sid, key_id(point)))
    if not verify(point, body, signature):
        raise ValueError("signature does not match")
    return True


# ---- key files (MOD_CATALOG 4.1) -------------------------------------------------------------------------

KDF_ROUNDS = 600000


def _keystream(key, nonce, length):
    out = b""
    counter = 0
    while len(out) < length:
        out += hmac.new(key, nonce + counter.to_bytes(4, "big"), hashlib.sha256).digest()
        counter += 1
    return out[:length]


def write_key_file(path, d, password=None):
    point = public_of(d)
    lines = ["xms-key 1", "public  = " + key_text(point)]
    if password:
        salt = os.urandom(16)
        nonce = os.urandom(16)
        derived = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, KDF_ROUNDS, 64)
        encrypted = bytes(a ^ b for a, b in zip(_bytes(d), _keystream(derived[:32], nonce, 32)))
        tag = hmac.new(derived[32:], nonce + encrypted, hashlib.sha256).digest()
        lines += ["kdf     = pbkdf2-sha256, %d, %s" % (KDF_ROUNDS, b64u(salt)),
                  "cipher  = hmac-sha256-ctr, " + b64u(nonce),
                  "private = " + b64u(encrypted),
                  "mac     = " + b64u(tag)]
    else:
        lines.append("private = " + b64u(_bytes(d)))
    data = "\n".join(lines) + "\n"
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(data)
    return point


def parse_key_file_text(text):
    fields = {}
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    if not lines or lines[0] != "xms-key 1":
        raise ValueError("not an xms key file")
    for line in lines[1:]:
        name, _, value = line.partition("=")
        fields[name.strip()] = value.strip()
    return fields


def key_file_protected(text):
    return "kdf" in parse_key_file_text(text)


def read_key_text(text, password=None):
    """The private scalar from a key file's text; raises ValueError with the reason."""
    fields = parse_key_file_text(text)
    point = parse_key(fields.get("public", ""))
    raw = unb64u(fields.get("private", ""))
    if "kdf" in fields:
        if password is None:
            raise ValueError("the key file is protected by a password")
        kdf = [p.strip() for p in fields["kdf"].split(",")]
        cipher = [p.strip() for p in fields.get("cipher", "").split(",")]
        if len(kdf) != 3 or kdf[0] != "pbkdf2-sha256" or len(cipher) != 2 or cipher[0] != "hmac-sha256-ctr":
            raise ValueError("unknown key file protection")
        derived = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), unb64u(kdf[2]), int(kdf[1]), 64)
        nonce = unb64u(cipher[1])
        tag = hmac.new(derived[32:], nonce + raw, hashlib.sha256).digest()
        if not hmac.compare_digest(tag, unb64u(fields.get("mac", ""))):
            raise ValueError("wrong password")
        raw = bytes(a ^ b for a, b in zip(raw, _keystream(derived[:32], nonce, len(raw))))
    if len(raw) != 32:
        raise ValueError("malformed private key")
    d = _int(raw)
    if not 1 <= d < N or public_of(d) != point:
        raise ValueError("the private key does not match its public key")
    return d


def read_key_file(path, password=None):
    with open(path, encoding="ascii") as f:
        return read_key_text(f.read(), password)


# ---- proofs of work (MOD_CATALOG 12.1) -----------------------------------------------------------------

def leading_zero_bits(digest):
    bits = 0
    for byte in digest:
        if byte == 0:
            bits += 8
            continue
        mask = 0x80
        while mask and not byte & mask:
            bits += 1
            mask >>= 1
        break
    return bits


def proof_ok(prefix, nonce, bits):
    return leading_zero_bits(hashlib.sha256(prefix + str(nonce).encode("ascii")).digest()) >= bits


def solve(prefix, bits):
    nonce = 0
    while not proof_ok(prefix, nonce, bits):
        nonce += 1
    return nonce


# ---- self test ----------------------------------------------------------------------------------------------

def selftest():
    # RFC 6979 A.2.5, P-256 with SHA-256
    d = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
    point = public_of(d)
    assert point == (0x60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6,
                     0x7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299), "public key"
    expected = {
        b"sample": ("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716",
                    "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8"),
        b"test": ("F1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367",
                  "019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083"),
    }
    for message, (r, s) in expected.items():
        signature = sign(d, message)
        assert signature.hex().upper() == r + s, "RFC 6979 signature of %r" % message
        assert verify(point, message, signature)
        assert not verify(point, message + b"!", signature)
    # text, keys, files
    text = sign_text("[catalog]\nformat = 1\n", d)
    assert verify_signed(text.encode("utf-8"), point)
    try:
        verify_signed(text.replace("format = 1", "format = 2").encode("utf-8"), point)
        raise AssertionError("a changed body verified")
    except ValueError:
        pass
    assert parse_key(key_text(point)) == point
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        plain = os.path.join(folder, "plain.xkey")
        locked = os.path.join(folder, "locked.xkey")
        write_key_file(plain, d)
        write_key_file(locked, d, "correct horse")
        assert read_key_file(plain) == d
        assert read_key_file(locked, "correct horse") == d
        try:
            read_key_file(locked, "wrong")
            raise AssertionError("a wrong password opened the key")
        except ValueError:
            pass
    assert proof_ok(b"xms-identity|x|", solve(b"xms-identity|x|", 8), 8)
    return True


if __name__ == "__main__":
    selftest()
    print("xms_catalog selftest: ok")
