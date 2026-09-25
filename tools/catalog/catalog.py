"""The mod catalog's own tool (docs/dead-air/MOD_CATALOG.md): what the catalog repository runs in
its workflows and what the maintainer runs at home.

    catalog.py init <folder>                 a new catalog repository: files, workflows, this tool
    catalog.py keygen <file> [--password]    a key file (4.1): the maintainer's, or an author's
    catalog.py check [--changed <list>] [--author <login>] [--summary <file>] [<mods/id.ltx>...]
                                             section 7 on submissions, the content policy (8) and
                                             the script report (10.3); non-zero on a blocking finding
    catalog.py only-submissions <list>       fails when a pull request touches anything but mods/<id>.ltx
    catalog.py cards [--issues <file>]       public/cards.txt and public/thumbs/ from the live releases
    catalog.py vt                            public/vt.txt (VT_API_KEY in the environment)
    catalog.py reviews [--csv <url|file>]    public/ratings.txt and public/reviews/ from the review inbox
    catalog.py publish --key <file>          public/index.ltx with the next serial, signed; then cards
    catalog.py revoke <id|key:<id>> <version|*> <reason>
    catalog.py review <mods/id.ltx | pull request number>
                                             every check again, on the maintainer's machine
    catalog.py selftest

Run it from the root of the catalog repository (or pass --repo). Network access goes to
github.com, the VirusTotal API and the review inbox only; XMS_CATALOG_GITHUB points the release
downloads somewhere else for tests.
"""
import argparse
import csv
import datetime
import getpass
import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xms_catalog as xc  # noqa: E402

GITHUB = os.environ.get("XMS_CATALOG_GITHUB", "https://github.com").rstrip("/")
VT_API = os.environ.get("XMS_CATALOG_VT", "https://www.virustotal.com/api/v3/").rstrip("/") + "/"
USER_AGENT = "DeadAirRefined-ModCatalog/1"
DESCRIPTOR_LIMIT = 256 * 1024
INDEX_LIMIT = 16 * 1024 * 1024
THUMBNAIL_LIMIT = 256 * 1024
CARDS_LIMIT = 16 * 1024 * 1024

ALLOWED_EXTENSIONS = {
    "ltx", "ltxp", "xml", "xmlp", "script", "txt", "md", "json", "csv", "ini", "cfg", "seq", "nqasset", "behasset",
    "pdaasset", "xspawn", "xcform", "dds", "thm", "ogg", "ogm", "ogf", "omf", "skl", "skls", "anm", "ppe", "xr", "ps",
    "vs", "gs", "hs", "ds", "cs", "h", "hlsl", "s", "spawn", "graph", "ai", "gct", "cform", "details", "env_mod",
    "fog_vol", "game", "geom", "geomx", "hom", "ps_static", "snd_env", "snd_static", "som", "sectors", "wallmarks",
    "lanims"}
MAGICS = [
    (b"\x7fELF", "a Linux program"), (b"\xfe\xed\xfa\xce", "a macOS program"), (b"\xfe\xed\xfa\xcf", "a macOS program"),
    (b"\xce\xfa\xed\xfe", "a macOS program"), (b"\xcf\xfa\xed\xfe", "a macOS program"),
    (b"\xca\xfe\xba\xbe", "a Java class or a universal binary"), (b"PK\x03\x04", "a ZIP archive"),
    (b"PK\x05\x06", "a ZIP archive"), (b"Rar!\x1a\x07", "a RAR archive"), (b"7z\xbc\xaf\x27\x1c", "a 7-Zip archive"),
    (b"MSCF", "a CAB archive"), (b"\x1f\x8b", "a GZIP archive"), (b"\xfd7zXZ", "an XZ archive"),
    (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "an OLE container"), (b"L\x00\x00\x00\x01\x14\x02\x00", "a Windows shortcut"),
    (b"\x00asm", "WebAssembly"), (b"\x1bLua", "Lua bytecode"), (b"\x1bLJ", "LuaJIT bytecode")]
TAGS = ["story", "quests", "gameplay", "weapons", "equipment", "npcs", "mutants", "locations", "graphics", "audio",
        "interface", "fixes"]
ID_RE = re.compile(r"^[a-z0-9_.-]{1,64}$")
REPO_RE = re.compile(r"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$")
WEBSITE_RE = [
    re.compile(r"^https://(www\.)?ap-pro\.ru/stuff/[a-z0-9_-]+/[a-z0-9_-]+-r\d+/?([?#].*)?$", re.I),
    re.compile(r"^https://(www\.)?ap-pro\.ru/forums/topic/\d+-[a-z0-9_-]+/?([?#].*)?$", re.I),
    re.compile(r"^https://(www\.)?moddb\.com/(mods|addons)/[a-z0-9_-]+(/.*)?$", re.I),
    re.compile(r"^https://(www\.)?moddb\.com/games/[a-z0-9_-]+/addons/[a-z0-9_-]+(/.*)?$", re.I)]


class Finding:
    def __init__(self, level, text):
        self.level = level  # "block", "review", "note"
        self.text = text


# ---- small helpers ---------------------------------------------------------------------------------------

def today():
    return datetime.date.today().isoformat()


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def write_text(path, text):
    write_bytes(path, text.encode("utf-8"))


def parse_ini(text):
    """[section] key = value, with ; comments; values keep what follows the first '='."""
    sections, section = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            sections.setdefault(section, [])
            continue
        if section is None or "=" not in line:
            continue
        key, _, value = line.partition("=")
        sections[section].append((key.strip(), strip_comment(value.strip())))
    return sections


def strip_comment(value):
    quoted = False
    for at, ch in enumerate(value):
        if ch == '"':
            quoted = not quoted
        elif ch == ";" and not quoted:
            return value[:at].strip()
    return value


def unquote(value):
    if len(value) >= 2 and value[0] == value[-1] == '"':
        out, escape = [], False
        for ch in value[1:-1]:
            if escape:
                out.append({"n": "\n", "t": "\t"}.get(ch, ch))
                escape = False
            elif ch == "\\":
                escape = True
            else:
                out.append(ch)
        return "".join(out)
    return value


def first(sections, section, key, default=""):
    for k, v in sections.get(section, []):
        if k == key:
            return v
    return default


def version_tuple(text):
    numbers = []
    for part in text.strip().split("."):
        digits = re.match(r"^\d+", part)
        if not digits:
            break
        numbers.append(int(digits.group(0)))
        if len(numbers) == 4:
            break
    return tuple(numbers + [0] * (4 - len(numbers)))


def http_get(url, limit, headers=None, byte_range=None):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    if byte_range:
        request.add_header("Range", "bytes=%d-%d" % byte_range)
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError("%s is larger than %d bytes" % (url, limit))
        return response.status, data


def release_url(repo, asset):
    return "%s/%s/releases/latest/download/%s" % (GITHUB, repo, urllib.parse.quote(asset))


# ---- the catalog repository -------------------------------------------------------------------------------

class Repo:
    def __init__(self, root):
        self.root = os.path.abspath(root)

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def config(self):
        path = self.path("catalog.ltx")
        return parse_ini(read_text(path)) if os.path.isfile(path) else {}

    def submissions(self):
        folder = self.path("mods")
        out = {}
        for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            if name.endswith(".ltx"):
                sub = Submission.load(os.path.join(folder, name))
                out[sub.id] = sub
        return out

    def revoked(self):
        path = self.path("revoked.ltx")
        rows = []
        if os.path.isfile(path):
            for key, value in parse_ini(read_text(path)).get("revoked", []):
                version, _, reason = value.partition(",")
                rows.append((key, version.strip(), reason.strip()))
        return rows

    def moderation(self):
        path = self.path("moderation.ltx")
        sections = parse_ini(read_text(path)) if os.path.isfile(path) else {}
        return {name: {k for k, _ in sections.get(name, [])} for name in ("identities", "hardware", "reviews")}

    def index(self):
        path = self.path("public", "index.ltx")
        if not os.path.isfile(path):
            return None
        with open(path, "rb") as f:
            data = f.read()
        body, _, _, _ = xc.split_signed(data)
        return parse_ini(body.decode("utf-8")), data


class Submission:
    """mods/<id>.ltx (10.1)."""

    def __init__(self, path, fields):
        self.path = path
        self.id = fields.get("id", "")
        self.github = fields.get("github", "")
        self.key = fields.get("key", "")
        self.version = fields.get("version", "")
        self.website = fields.get("website", "")
        self.listed = fields.get("listed", "")

    @staticmethod
    def load(path):
        fields = dict(parse_ini(read_text(path)).get("mod", []))
        return Submission(path, fields)

    def problems(self):
        out = []
        expected = os.path.splitext(os.path.basename(self.path))[0]
        if not ID_RE.match(self.id):
            out.append("id is not a module id ([a-z0-9_.-])")
        elif self.id != expected:
            out.append("id %s does not match the file name %s.ltx" % (self.id, expected))
        if not REPO_RE.match(self.github):
            out.append("github is not owner/repo")
        try:
            xc.parse_key(self.key)
        except ValueError:
            out.append("key is not a p256 key")
        if version_tuple(self.version) < (1, 0, 0, 0):
            out.append("version %s is below 1.0.0" % (self.version or "(none)"))
        if not any(pattern.match(self.website) for pattern in WEBSITE_RE):
            out.append("website is not a module page on AP-PRO or ModDB")
        return out


# ---- releases ------------------------------------------------------------------------------------------------

class Release:
    """The live release of a module: descriptor, card, index, and on demand the files."""

    def __init__(self, repo, module_id):
        self.repo = repo
        self.id = module_id
        self.text = b""
        self.fields = {}
        self.card = {}
        self.packages = []
        self.index_name, self.index_size, self.index_sha = "", 0, ""
        self.files = []

    def fetch_descriptor(self):
        _, self.text = http_get(release_url(self.repo, self.id + ".update.ltx"), DESCRIPTOR_LIMIT)
        body = self.text
        try:
            body, _, _, _ = xc.split_signed(self.text)
        except ValueError:
            pass
        sections = parse_ini(body.decode("utf-8"))
        self.fields = dict(sections.get("release", []))
        self.card = {k: unquote(v) for k, v in sections.get("card", [])}
        for name, value in sections.get("packages", []):
            size, _, sha = value.partition(",")
            self.packages.append((name, int(size), sha.strip().lower()))
        index = [p.strip() for p in self.fields.get("index", "").split(",")]
        if len(index) == 3:
            self.index_name, self.index_size, self.index_sha = index[0], int(index[1]), index[2].lower()
        return self

    @property
    def version(self):
        return self.fields.get("version", "")

    def verify_key(self, key_text):
        return xc.verify_signed(self.text, xc.parse_key(key_text))

    def fetch_index(self):
        _, data = http_get(release_url(self.repo, self.index_name), INDEX_LIMIT)
        if len(data) != self.index_size or hashlib.sha256(data).hexdigest() != self.index_sha:
            raise ValueError("the file index does not match the descriptor")
        lines = data.decode("utf-8").splitlines()
        if not lines or lines[0].strip() != "xms-files 1":
            raise ValueError("not a file index")
        for line in lines[1:]:
            parts = line.split(" ", 7)
            if parts[0] == "file" and len(parts) == 8:
                self.files.append({"sha": parts[1].lower(), "size": int(parts[2]), "package": int(parts[3]),
                                   "offset": int(parts[4]), "packed": int(parts[5]), "method": int(parts[6]),
                                   "path": parts[7]})
        return self.files

    def read_file(self, entry, limit=None):
        """The bytes of one file, fetched as the game fetches them: a byte range of its package."""
        if limit is not None and entry["size"] > limit:
            raise ValueError("%s is larger than %d bytes" % (entry["path"], limit))
        package = self.packages[entry["package"] - 1][0]
        first_byte, last_byte = entry["offset"], entry["offset"] + max(entry["packed"], 1) - 1
        status, raw = http_get(release_url(self.repo, package), entry["packed"] + 1, byte_range=(first_byte, last_byte))
        if status == 200:
            raw = raw[first_byte:last_byte + 1]
        raw = raw[:entry["packed"]]
        data = zlib.decompress(raw, -15) if entry["method"] == 8 else raw
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha"]:
            raise ValueError("%s does not match the index" % entry["path"])
        return data


def valid_thumbnail(data):
    """The game's own test (ModCatalog valid_thumbnail): 256 x 256 DXT1/DXT5, one level or all nine."""
    if len(data) < 128 or len(data) > THUMBNAIL_LIMIT or not data.startswith(b"DDS "):
        return False
    header, height, width, mips = struct.unpack_from("<I", data, 4)[0], *struct.unpack_from("<II", data, 12), \
        struct.unpack_from("<I", data, 28)[0]
    fmt_size, fmt_flags = struct.unpack_from("<II", data, 76)
    fourcc = data[84:88]
    if header != 124 or width != 256 or height != 256 or fmt_size != 32 or not fmt_flags & 0x4:
        return False
    block = 8 if fourcc == b"DXT1" else 16 if fourcc == b"DXT5" else 0
    levels = mips or 1
    if not block or levels not in (1, 9):
        return False
    expected, side = 128, 256
    for _ in range(levels):
        blocks = max((side + 3) // 4, 1)
        expected += blocks * blocks * block
        side = max(side // 2, 1)
    return len(data) == expected


# ---- the content policy and the script report ------------------------------------------------------------

def path_problem(path):
    parts = path.split("/")
    if not path or path.startswith("/") or "\\" in path or any(p in ("", ".", "..") for p in parts) or ":" in path:
        return "unsafe path"
    if any(p.startswith(".") for p in parts):
        return "a dot-named file or folder"
    name = parts[-1]
    if "." in name:
        extension = name.rsplit(".", 1)[1].lower()
        if extension not in ALLOWED_EXTENSIONS:
            return ".%s files are not allowed in a catalog module" % extension
    return None


def bytes_problem(data):
    if data.startswith(b"MZ") and len(data) >= 0x40:
        offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[offset:offset + 4] == b"PE\x00\x00":
            return "a Windows program or library"
    for magic, what in MAGICS:
        if data.startswith(magic):
            return what
    return None


LUA_TOKEN = re.compile(r"""
    (?P<space>\s+)
  | (?P<long_comment>--\[(?P<eq1>=*)\[.*?\](?P=eq1)\])
  | (?P<comment>--[^\n]*)
  | (?P<long_string>\[(?P<eq2>=*)\[.*?\](?P=eq2)\])
  | (?P<string>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
  | (?P<number>0[xX][0-9a-fA-F.]+(?:[pP][+-]?\d+)?|\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)
  | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>\.\.\.|\.\.|==|~=|<=|>=|::|[-+*/%^\#&~|<>=(){}\[\];:,.])
  | (?P<other>.)
""", re.S | re.X)

BLOCKING = {("ffi",): "ffi", ("os", "execute"): "os.execute", ("io", "popen"): "io.popen",
            ("package", "loadlib"): "package.loadlib", ("string", "dump"): "string.dump",
            ("lfs", "link"): "lfs.link", ("StartDedicatedServer",): "StartDedicatedServer"}
FOR_REVIEW = {("setfenv",): "setfenv", ("getfenv",): "getfenv", ("debug",): "debug.*",
              ("save_as",): "ini_file:save_as", ("w_open",): "getFS():w_open", ("file_delete",): "getFS():file_delete",
              ("file_rename",): "getFS():file_rename", ("file_copy",): "getFS():file_copy", ("dir_delete",): "getFS():dir_delete"}


def lua_tokens(text):
    tokens = []
    for match in LUA_TOKEN.finditer(text):
        kind = match.lastgroup
        if kind in ("space", "comment", "long_comment") or kind in ("eq1", "eq2"):
            continue
        if kind == "long_string":
            kind = "string"
        tokens.append((kind, match.group(0), text.count("\n", 0, match.start()) + 1))
    return tokens


def script_report(path, data):
    """Findings of 10.3 for one script."""
    findings = []
    text = data.decode("cp1251", errors="replace")
    tokens = lua_tokens(text)
    names = [(i, value) for i, (kind, value, _) in enumerate(tokens) if kind == "name"]

    def chain(i):
        # the dotted name starting at token i: a, a.b, a:b
        parts = [tokens[i][1]]
        j = i + 1
        while j + 1 < len(tokens) and tokens[j][1] in (".", ":") and tokens[j + 1][0] == "name":
            parts.append(tokens[j + 1][1])
            j += 2
        return tuple(parts), j

    for i, value in names:
        if i > 0 and tokens[i - 1][1] in (".", ":"):
            continue
        parts, after = chain(i)
        line = tokens[i][2]
        for pattern, what in BLOCKING.items():
            if parts[:len(pattern)] == pattern:
                findings.append(Finding("block", "%s:%d uses %s" % (path, line, what)))
        if parts[0] == "require" and after < len(tokens) and tokens[after][1] == "(" and after + 1 < len(tokens) and \
                tokens[after + 1][0] == "string" and "ffi" in tokens[after + 1][1]:
            findings.append(Finding("block", "%s:%d loads ffi" % (path, line)))
        if parts[0] in ("load", "loadstring", "require", "dofile", "loadfile") and after < len(tokens) and tokens[after][1] == "(":
            argument = tokens[after + 1] if after + 1 < len(tokens) else None
            closing = tokens[after + 2][1] if after + 2 < len(tokens) else ""
            if not argument or argument[0] != "string" or closing not in (")", ","):
                findings.append(Finding("review", "%s:%d %s of a computed string" % (path, line, parts[0])))
        if parts[0] == "_G" and after < len(tokens) and tokens[after][1] == "[":
            findings.append(Finding("review", "%s:%d indexes _G by a computed name" % (path, line)))
        for pattern, what in FOR_REVIEW.items():
            if pattern in [parts[k:k + len(pattern)] for k in range(len(parts))]:
                findings.append(Finding("review", "%s:%d uses %s" % (path, line, what)))
        if parts[-2:] == ("get_console", "execute") or (parts[0] == "get_console" and after + 3 < len(tokens) and
                                                        tokens[after + 2][1] == ":" and tokens[after + 3][1] == "execute"):
            findings.append(Finding("review", "%s:%d runs a console command" % (path, line)))
        if parts == ("string", "char") and after < len(tokens) and tokens[after][1] == "(":
            depth, commas, j = 0, 0, after
            while j < len(tokens):
                if tokens[j][1] == "(":
                    depth += 1
                elif tokens[j][1] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                elif tokens[j][1] == "," and depth == 1:
                    commas += 1
                j += 1
            if commas >= 16:
                findings.append(Finding("review", "%s:%d builds a string of %d characters with string.char" %
                                        (path, line, commas + 1)))
    for kind, value, line in tokens:
        if kind == "string" and len(value) > 2000 and re.fullmatch(r"[\"'\[=]*[A-Za-z0-9+/=_\\\-\s]+[\"'\]=]*", value):
            findings.append(Finding("review", "%s:%d holds a long encoded blob (%d characters)" % (path, line, len(value))))
    return findings


def content_report(release):
    """The content policy (8) and the script report (10.3) over every file of a release."""
    findings = []
    files = release.fetch_index()
    for entry in files:
        problem = path_problem(entry["path"])
        if problem:
            findings.append(Finding("block", "%s: %s" % (entry["path"], problem)))
            continue
        data = release.read_file(entry)
        problem = bytes_problem(data)
        if problem:
            findings.append(Finding("block", "%s: %s" % (entry["path"], problem)))
            continue
        if entry["path"].lower().endswith(".script"):
            findings += script_report(entry["path"], data)
    return findings


# ---- VirusTotal ---------------------------------------------------------------------------------------------

def vt_policy(config):
    section = dict(config.get("virustotal", []))
    return {"trusted": [t.strip().lower() for t in section.get("trusted", "").split(",") if t.strip()],
            "block_trusted": int(section.get("block_trusted", 1)), "warn_trusted": int(section.get("warn_trusted", 1)),
            "block_others": int(section.get("block_others", 5)), "warn_others": int(section.get("warn_others", 2))}


def vt_lookup(sha, key):
    """(engines, trusted malicious, trusted suspicious, other malicious, flagged names) or None when unknown."""
    try:
        _, data = http_get(VT_API + "files/" + sha, 8 * 1024 * 1024, headers={"x-apikey": key})
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise
    results = json.loads(data.decode("utf-8"))["data"]["attributes"].get("last_analysis_results", {})
    return results


def vt_verdict(results, policy):
    tm = ts = om = 0
    flagged = []
    for engine, result in results.items():
        category = result.get("category", "")
        trusted = engine.lower() in policy["trusted"]
        if category == "malicious":
            flagged.append(engine)
            if trusted:
                tm += 1
            else:
                om += 1
        elif category == "suspicious" and trusted:
            ts += 1
            flagged.append(engine)
    verdict = "block" if tm >= policy["block_trusted"] or om >= policy["block_others"] else \
        "warn" if ts >= policy["warn_trusted"] or om >= policy["warn_others"] else "clean"
    return verdict, len(results), tm, ts, om, flagged


# ---- commands -------------------------------------------------------------------------------------------------

def cmd_keygen(args):
    if os.path.exists(args.file):
        sys.exit("%s exists: a key file is never overwritten" % args.file)
    password = None
    if args.password:
        password = getpass.getpass("Password (empty for none): ") or None
        if password and getpass.getpass("Again: ") != password:
            sys.exit("the passwords differ")
    point = xc.write_key_file(args.file, xc.new_private(), password)
    print("key file: %s" % args.file)
    print("public key: %s" % xc.key_text(point))
    print("key id: %s" % xc.key_id(point))
    print("Keep a copy of the file somewhere safe: without it nothing can be signed with this key.")


def check_submission(repo, sub, author=None, vt_key=None, deep=True):
    """Section 7 for one submission: (findings, release or None)."""
    findings = [Finding("block", "%s: %s" % (os.path.basename(sub.path), p)) for p in sub.problems()]
    if findings:
        return findings, None
    owner = sub.github.split("/")[0]
    if author and author.lower() != owner.lower():
        findings.append(Finding("block", "the pull request is opened by %s, the repository belongs to %s" % (author, owner)))
    listed = repo.index()
    if listed:
        for key, value in listed[0].get("mods", []):
            if key == sub.id:
                fields = [f.strip() for f in value.split(",")]
                if len(fields) >= 2 and fields[1] != sub.key:
                    findings.append(Finding("block", "%s is listed with another key: a key change is the maintainer's" % sub.id))
    try:
        release = Release(sub.github, sub.id).fetch_descriptor()
    except (urllib.error.URLError, ValueError) as error:
        findings.append(Finding("block", "%s: no release descriptor in the latest release (%s)" % (sub.github, error)))
        return findings, None
    if release.fields.get("id") != sub.id:
        findings.append(Finding("block", "the descriptor is for %s" % release.fields.get("id")))
    if version_tuple(release.version) < (1, 0, 0, 0):
        findings.append(Finding("block", "release %s is below 1.0.0" % release.version))
    if not release.card:
        findings.append(Finding("block", "the release has no [card] (Package Release in XFined Editor writes it)"))
    try:
        release.verify_key(sub.key)
    except ValueError as error:
        findings.append(Finding("block", "the release is not signed by the submitted key: %s" % error))
    website = release.card.get("website", "") or sub.website
    if not any(pattern.match(website) for pattern in WEBSITE_RE):
        findings.append(Finding("block", "website %s is not a module page on AP-PRO or ModDB" % website))
    else:
        try:
            status, _ = http_get(website, 4 * 1024 * 1024)
            if status != 200:
                findings.append(Finding("block", "website answers HTTP %d" % status))
        except urllib.error.HTTPError as error:
            if "moddb.com" in website and error.code in (403, 429, 503):
                findings.append(Finding("note", "website %s turns automated requests away: open it by hand" % website))
            else:
                findings.append(Finding("block", "website answers HTTP %d" % error.code))
        except urllib.error.URLError as error:
            findings.append(Finding("note", "website %s could not be reached: %s" % (website, error.reason)))
    if deep and not any(f.level == "block" for f in findings):
        try:
            findings += content_report(release)
        except (urllib.error.URLError, ValueError, zlib.error) as error:
            findings.append(Finding("block", "the release files could not be read: %s" % error))
    if vt_key:
        policy = vt_policy(repo.config())
        for name, _, sha in release.packages:
            results = vt_lookup(sha, vt_key)
            if results is None:
                findings.append(Finding("note", "VirusTotal does not know %s yet" % name))
                continue
            verdict, engines, tm, ts, om, flagged = vt_verdict(results, policy)
            level = "block" if verdict == "block" else "review" if verdict == "warn" else "note"
            findings.append(Finding(level, "VirusTotal %s for %s: %d engines, %d trusted malicious, %d trusted suspicious, "
                                           "%d other malicious%s" % (verdict, name, engines, tm, ts, om,
                                                                     (" (" + ", ".join(flagged) + ")") if flagged else "")))
    return findings, release


def report(findings, title):
    lines = ["## " + title, ""]
    if not findings:
        lines.append("Nothing to report.")
    for level in ("block", "review", "note"):
        chosen = [f for f in findings if f.level == level]
        if chosen:
            lines.append({"block": "**Blocking**", "review": "**For review**", "note": "**Notes**"}[level])
            lines += ["- " + f.text for f in chosen]
            lines.append("")
    return "\n".join(lines) + "\n"


def cmd_check(args):
    repo = Repo(args.repo)
    paths = list(args.files)
    if args.changed:
        with open(args.changed, encoding="utf-8") as f:
            paths += [repo.path(line.strip()) for line in f if line.strip().startswith("mods/")]
    if not paths:
        paths = [s.path for s in repo.submissions().values()]
    text, blocking = "", False
    for path in paths:
        if not os.path.isfile(path):
            continue
        sub = Submission.load(path)
        findings, _ = check_submission(repo, sub, args.author, os.environ.get("VT_API_KEY"), deep=not args.shallow)
        blocking |= any(f.level == "block" for f in findings)
        text += report(findings, "%s (%s)" % (sub.id or os.path.basename(path), sub.github))
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as f:
            f.write(text)
    return 1 if blocking else 0


def cmd_only_submissions(args):
    with open(args.changed, encoding="utf-8") as f:
        changed = [line.strip() for line in f if line.strip()]
    wrong = [p for p in changed if not re.fullmatch(r"mods/[a-z0-9_.-]+\.ltx", p)]
    if wrong or len(changed) != 1:
        print("A submission changes one file, mods/<id>.ltx. This pull request changes:\n" + "\n".join(changed))
        return 1
    return 0


def listed_modules(repo):
    listed = repo.index()
    if not listed:
        sys.exit("public/index.ltx is missing: publish first")
    mods = {}
    for key, value in listed[0].get("mods", []):
        fields = [f.strip() for f in value.split(",")]
        if len(fields) >= 4:
            mods[key] = {"github": fields[0], "key": fields[1], "min": fields[2], "date": fields[3]}
    revoked = [(k, v.split(",")[0].strip()) for k, v in listed[0].get("revoked", [])]
    return mods, revoked


def cmd_cards(args):
    repo = Repo(args.repo)
    mods, revoked = listed_modules(repo)
    previous = read_cards(repo.path("public", "cards.txt"))
    out, thumbs, issues = [b"xms-cards 1\n"], set(), []
    for module_id, listing in sorted(mods.items()):
        try:
            release = Release(listing["github"], module_id).fetch_descriptor()
            release.verify_key(listing["key"])
            if release.fields.get("id") != module_id:
                raise ValueError("the descriptor names %s" % release.fields.get("id"))
            if version_tuple(release.version) < version_tuple(listing["min"]):
                raise ValueError("release %s is below the listed %s" % (release.version, listing["min"]))
            if any(k == module_id and v in ("*", release.version) for k, v in revoked):
                raise ValueError("revoked")
            text = release.text
            thumb = [p.strip() for p in release.card.get("thumbnail", "").split(",")]
            if len(thumb) == 3:
                sha = thumb[2].lower()
                target = repo.path("public", "thumbs", sha + ".dds")
                if not os.path.isfile(target):
                    entry = next((f for f in release.fetch_index() if f["path"] == thumb[0]), None)
                    data = release.read_file(entry, THUMBNAIL_LIMIT) if entry else b""
                    if len(data) == int(thumb[1]) and hashlib.sha256(data).hexdigest() == sha and valid_thumbnail(data):
                        write_bytes(target, data)
                if os.path.isfile(target):
                    thumbs.add(sha)
        except (urllib.error.URLError, ValueError, zlib.error) as error:
            issues.append("%s: %s" % (module_id, error))
            text = previous.get(module_id)
            if text is None:
                continue
        out.append(b"card %s %d\n" % (module_id.encode(), len(text)) + text)
    data = b"".join(out)
    if len(data) > CARDS_LIMIT:
        sys.exit("cards.txt would exceed 16 MiB")
    write_bytes(repo.path("public", "cards.txt"), data)
    folder = repo.path("public", "thumbs")
    for name in os.listdir(folder) if os.path.isdir(folder) else []:
        if name.endswith(".dds") and name[:-4] not in thumbs:
            os.remove(os.path.join(folder, name))
    print("cards: %d module(s), %d thumbnail(s)" % (len(out) - 1, len(thumbs)))
    if issues:
        print("\n".join(issues))
        if args.issues:
            write_text(args.issues, "".join("- %s\n" % issue for issue in issues))
    return 0


def read_cards(path):
    cards = {}
    if not os.path.isfile(path):
        return cards
    with open(path, "rb") as f:
        data = f.read()
    at = data.find(b"\n") + 1
    while at < len(data):
        end = data.find(b"\n", at)
        header = data[at:end].split(b" ")
        if len(header) != 3 or header[0] != b"card":
            break
        size = int(header[2])
        cards[header[1].decode()] = data[end + 1:end + 1 + size]
        at = end + 1 + size
    return cards


def cmd_vt(args):
    repo = Repo(args.repo)
    key = os.environ.get("VT_API_KEY")
    if not key:
        print("VT_API_KEY is not set: vt.txt is left as it is")
        return 0
    policy = vt_policy(repo.config())
    cards = read_cards(repo.path("public", "cards.txt"))
    old = {}
    path = repo.path("public", "vt.txt")
    if os.path.isfile(path):
        for line in read_text(path).splitlines()[1:]:
            parts = line.split(" ")
            if len(parts) == 7:
                old[parts[0]] = line
    lines, asked = ["xms-vt 1"], 0
    for module_id, text in sorted(cards.items()):
        body, _, _, _ = xc.split_signed(text)
        for name, value in parse_ini(body.decode("utf-8")).get("packages", []):
            sha = value.split(",")[1].strip().lower()
            # a result younger than a day is kept: the free API allows 500 lookups a day
            if sha in old and old[sha].split(" ")[1] == today():
                lines.append(old[sha])
                continue
            if asked:
                time.sleep(16)
            asked += 1
            results = vt_lookup(sha, key)
            if results is None:
                if sha in old:
                    lines.append(old[sha])
                continue
            verdict, engines, tm, ts, om, flagged = vt_verdict(results, policy)
            lines.append("%s %s %d %d %d %d %s" % (sha, today(), engines, tm, ts, om,
                                                   ",".join(e.replace(" ", "_") for e in flagged) or "-"))
            print("%s %s: %s" % (module_id, name, verdict))
    write_text(path, "\n".join(lines) + "\n")
    return 0


def parse_review(payload, index_reviews):
    """A review from the inbox (12.2), verified; raises ValueError with the reason."""
    data = payload.encode("utf-8")
    body, key_text, sid, _ = xc.split_signed(data)
    lines = body.decode("utf-8").splitlines()
    if not lines or lines[0].strip() != "xms-review 1":
        raise ValueError("not a review")
    fields = {}
    for line in lines[1:]:
        name, _, value = line.partition("=")
        fields[name.strip()] = value.strip()
    identity = xc.parse_key(fields["identity"])
    if key_text != fields["identity"] or sid != xc.key_id(identity):
        raise ValueError("signed by another key than its identity")
    xc.verify_signed(data, identity)
    if not xc.proof_ok(("xms-identity|%s|" % fields["identity"]).encode("ascii"), int(fields["identity_pow"]),
                       index_reviews["identity_bits"]):
        raise ValueError("the identity's proof of work does not hold")
    pow_line = body.rfind(b"\npow = ")
    if pow_line < 0 or not xc.proof_ok(b"xms-review|" + body[:pow_line + 1], int(fields["pow"]), index_reviews["review_bits"]):
        raise ValueError("the review's proof of work does not hold")
    stars = int(fields["stars"])
    text = xc.unb64u(fields["text"]).decode("utf-8")
    trimmed = text.strip()
    name = fields.get("name", "")
    if not 1 <= stars <= 5 or not 30 <= len(trimmed) <= 2000 or len(name) > 32:
        raise ValueError("stars, text or name out of bounds")
    if not re.fullmatch(r"[0-9a-f]{64}", fields.get("hardware", "")):
        raise ValueError("no hardware hash")
    return {"mod": fields["mod"], "version": fields["version"], "stars": stars, "date": fields["date"][:10],
            "stamp": fields["date"], "name": name, "text": trimmed, "hardware": fields["hardware"],
            "identity": xc.key_id(identity)}


def cmd_reviews(args):
    repo = Repo(args.repo)
    config = repo.config()
    source = args.csv or first(config, "reviews", "csv")
    if not source:
        print("no review inbox configured: ratings are left as they are")
        return 0
    mods, _ = listed_modules(repo)
    settings = {"identity_bits": int(first(config, "reviews", "identity_bits", "24")),
                "review_bits": int(first(config, "reviews", "review_bits", "20"))}
    moderation = repo.moderation()
    if re.match(r"^https?://", source):
        _, raw = http_get(source, 64 * 1024 * 1024)
    else:
        with open(source, "rb") as f:
            raw = f.read()
    rows = list(csv.reader(io.StringIO(raw.decode("utf-8-sig"))))
    latest = {}
    rejected = 0
    for row in rows[1:]:
        payload = next((cell for cell in row if cell.startswith("xms-review 1")), None)
        if not payload:
            continue
        try:
            review = parse_review(payload, settings)
        except (ValueError, KeyError) as error:
            rejected += 1
            continue
        listing = mods.get(review["mod"])
        if not listing or review["date"] < listing["date"]:
            continue
        if review["identity"] in moderation["identities"] or review["hardware"] in moderation["hardware"] or \
                "%s %s" % (review["mod"], review["identity"]) in moderation["reviews"]:
            continue
        key = (review["mod"], review["identity"])
        if key not in latest or review["stamp"] > latest[key]["stamp"]:
            latest[key] = review
    # one per hardware: the newest of the identities one computer holds
    by_hardware = {}
    for review in latest.values():
        key = (review["mod"], review["hardware"])
        if key not in by_hardware or review["stamp"] > by_hardware[key]["stamp"]:
            by_hardware[key] = review
    per_mod = {}
    for review in by_hardware.values():
        per_mod.setdefault(review["mod"], []).append(review)
    ratings = ["xms-ratings 1"]
    folder = repo.path("public", "reviews")
    os.makedirs(folder, exist_ok=True)
    for name in os.listdir(folder):
        if name.endswith(".txt") and name[:-4] not in per_mod:
            os.remove(os.path.join(folder, name))
    for module_id, reviews in sorted(per_mod.items()):
        reviews.sort(key=lambda r: r["stamp"], reverse=True)
        counts = [sum(1 for r in reviews if r["stars"] == s) for s in range(1, 6)]
        average = round(100 * sum(r["stars"] for r in reviews) / len(reviews))
        ratings.append("%s %d %d %s" % (module_id, len(reviews), average, " ".join(map(str, counts))))
        out = [b"xms-reviews 1\n"]
        for r in reviews:
            name, text = r["name"].encode("utf-8"), r["text"].encode("utf-8")
            out.append(b"review %s %d %s %s %d %d\n" % (r["identity"].encode(), r["stars"], r["date"].encode(),
                                                      r["version"].encode(), len(name), len(text)) + name + text + b"\n")
        write_bytes(os.path.join(folder, module_id + ".txt"), b"".join(out))
    write_text(repo.path("public", "ratings.txt"), "\n".join(ratings) + "\n")
    print("reviews: %d counted over %d module(s), %d rejected" % (len(by_hardware), len(per_mod), rejected))
    return 0


def cmd_publish(args):
    repo = Repo(args.repo)
    password = os.environ.get(args.password_env) if args.password_env else None
    with open(args.key, encoding="ascii") as f:
        key_text = f.read()
    if xc.key_file_protected(key_text) and password is None:
        password = getpass.getpass("Password of %s: " % args.key)
    d = xc.read_key_text(key_text, password)
    config = repo.config()
    previous = repo.index()
    serial = int(first(previous[0], "catalog", "serial", "0")) + 1 if previous else 1
    subs = repo.submissions()
    problems = ["%s: %s" % (os.path.basename(s.path), p) for s in subs.values() for p in s.problems()]
    if problems:
        sys.exit("fix the submissions first:\n" + "\n".join(problems))
    lines = ["[catalog]", "format  = 1", "serial  = %d" % serial, "issued  = %s" % today()]
    mirrors = first(config, "catalog", "mirrors")
    if mirrors:
        lines.append("mirrors = " + mirrors)
    lines += ["", "[virustotal]"] + ["%s = %s" % (k, v) for k, v in config.get("virustotal", [])]
    reviews = dict(config.get("reviews", []))
    if reviews.get("inbox_url") and reviews.get("inbox_field"):
        lines += ["", "[reviews]"] + ["%s = %s" % (k, reviews[k]) for k in
                                      ("inbox_url", "inbox_field", "salt", "identity_bits", "review_bits") if reviews.get(k)]
    lines += ["", "[mods]"]
    for module_id, sub in sorted(subs.items()):
        if not sub.listed:
            sub.listed = today()
            with open(sub.path, "a", encoding="utf-8", newline="\n") as f:
                f.write("listed  = %s\n" % sub.listed)
        lines.append("%s = %s, %s, %s, %s" % (module_id, sub.github, sub.key, sub.version, sub.listed))
    lines += ["", "[revoked]"] + ["%s = %s, %s" % (k, v, r) for k, v, r in repo.revoked()]
    body = "\n".join(lines) + "\n"
    text = xc.sign_text(body, d)
    write_text(repo.path("public", "index.ltx"), text)
    print("index.ltx: serial %d, %d module(s), signed by %s" % (serial, len(subs), xc.key_id(xc.public_of(d))))
    if not args.no_cards:
        cmd_cards(argparse.Namespace(repo=args.repo, issues=None))
    return 0


def cmd_revoke(args):
    repo = Repo(args.repo)
    target = args.target
    if not (ID_RE.match(target) or re.fullmatch(r"key:[0-9a-f]{16}", target)):
        sys.exit("revoke a module id or key:<key id>")
    path = repo.path("revoked.ltx")
    if not os.path.isfile(path):
        write_text(path, "; Revoked modules, versions and keys (MOD_CATALOG 5.2). catalog.py publish signs them in.\n[revoked]\n")
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        f.write("%s = %s, %s\n" % (target, args.version, args.reason.replace("\n", " ")))
    print("revoked %s (%s): run catalog.py publish" % (target, args.version))
    return 0


def cmd_review(args):
    repo = Repo(args.repo)
    target = args.target
    if target.isdigit():
        sys.exit("fetch the pull request's branch first (gh pr checkout %s), then pass mods/<id>.ltx" % target)
    sub = Submission.load(target)
    findings, release = check_submission(repo, sub, None, os.environ.get("VT_API_KEY"))
    print(report(findings, "%s (%s)" % (sub.id, sub.github)))
    if release:
        print("release %s, packages: %s" % (release.version, ", ".join("%s (%s)" % (n, s[:12]) for n, _, s in release.packages)))
        for _, _, sha in release.packages:
            print("VirusTotal: https://www.virustotal.com/gui/file/%s" % sha)
    return 1 if any(f.level == "block" for f in findings) else 0


def cmd_init(args):
    target = os.path.abspath(args.folder)
    if os.path.exists(target) and os.listdir(target):
        sys.exit("%s is not empty" % target)
    templates = os.path.join(HERE, "template")
    shutil.copytree(templates, target, dirs_exist_ok=True)
    os.makedirs(os.path.join(target, "tools"), exist_ok=True)
    for name in ("catalog.py", "xms_catalog.py"):
        shutil.copy2(os.path.join(HERE, name), os.path.join(target, "tools", name))
    for name in ("mods", os.path.join("public", "thumbs"), os.path.join("public", "reviews")):
        os.makedirs(os.path.join(target, name), exist_ok=True)
        keep = os.path.join(target, name, ".gitkeep")
        if not os.path.exists(keep):
            open(keep, "w").close()
    print("catalog repository: %s" % target)
    print("next: catalog.py keygen <key file> --password, then catalog.py publish --key <key file> in it")
    return 0


def cmd_selftest(_args):
    xc.selftest()
    assert path_problem("gamedata/scripts/a.script") is None
    assert path_problem("bin/evil.dll")
    assert path_problem(".git/config")
    assert path_problem("../escape.ltx")
    assert bytes_problem(b"MZ" + b"\0" * 58 + struct.pack("<I", 64) + b"PE\0\0") == "a Windows program or library"
    assert bytes_problem(b"\x1bLJ\x02") == "LuaJIT bytecode"
    assert bytes_problem(b"[section]\n") is None
    found = script_report("x.script", b'local f = require("ffi")\nos.execute("calc")\nlocal s = loadstring(code)\n'
                                      b'-- os.execute in a comment\nlocal t = "io.popen in a string"\n')
    levels = sorted((f.level, f.text) for f in found)
    assert ("block", "x.script:1 loads ffi") in levels, levels
    assert ("block", "x.script:2 uses os.execute") in levels, levels
    assert ("review", "x.script:3 loadstring of a computed string") in levels, levels
    assert not any("io.popen" in t for _, t in levels) and not any(":4" in t for _, t in levels), levels
    assert version_tuple("1.0") == (1, 0, 0, 0) and version_tuple("0.9.9") < (1, 0, 0, 0)
    for good in ("https://ap-pro.ru/stuff/zov_pripjati/overnight-delivery-r600/", "https://www.moddb.com/mods/dead-air",
                 "https://ap-pro.ru/forums/topic/15259-konkurs-kvestov-2026/"):
        assert any(p.match(good) for p in WEBSITE_RE), good
    for bad in ("https://ap-pro.ru/", "https://www.moddb.com/", "https://ap-pro.ru.evil.example/stuff/a/b-r1/",
                "http://ap-pro.ru/stuff/a/b-r1/"):
        assert not any(p.match(bad) for p in WEBSITE_RE), bad
    policy = {"trusted": ["kaspersky", "microsoft"], "block_trusted": 1, "warn_trusted": 1, "block_others": 5, "warn_others": 2}
    assert vt_verdict({"NoName": {"category": "malicious"}}, policy)[0] == "clean"
    assert vt_verdict({"NoName": {"category": "malicious"}, "Other": {"category": "malicious"}}, policy)[0] == "warn"
    assert vt_verdict({"Kaspersky": {"category": "malicious"}}, policy)[0] == "block"
    print("catalog selftest: ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--repo", default=".", help="the catalog repository (default: here)")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("init")
    p.add_argument("folder")
    p = sub.add_parser("keygen")
    p.add_argument("file")
    p.add_argument("--password", action="store_true", help="protect the key file with a password")
    p = sub.add_parser("check")
    p.add_argument("files", nargs="*")
    p.add_argument("--changed")
    p.add_argument("--author")
    p.add_argument("--summary")
    p.add_argument("--shallow", action="store_true", help="skip the release's files")
    p = sub.add_parser("only-submissions")
    p.add_argument("changed")
    p = sub.add_parser("cards")
    p.add_argument("--issues")
    sub.add_parser("vt")
    p = sub.add_parser("reviews")
    p.add_argument("--csv")
    p = sub.add_parser("publish")
    p.add_argument("--key", required=True)
    p.add_argument("--password-env", help="the environment variable holding the key file's password")
    p.add_argument("--no-cards", action="store_true")
    p = sub.add_parser("revoke")
    p.add_argument("target")
    p.add_argument("version")
    p.add_argument("reason")
    p = sub.add_parser("review")
    p.add_argument("target")
    sub.add_parser("selftest")
    args = parser.parse_args()
    handler = globals()["cmd_" + args.command.replace("-", "_")]
    sys.exit(handler(args) or 0)


if __name__ == "__main__":
    main()
