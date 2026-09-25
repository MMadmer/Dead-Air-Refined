"""The mod catalog's own tool (docs/dead-air/MOD_CATALOG.md): what the catalog repository runs in
its workflows - by itself, with nobody there - and what a maintainer may still run by hand.

    catalog.py judge [--issue <number>] [--outputs <file>] [--summary <file>]
                                             section 7 on every open submission issue: accepted,
                                             refused (and closed) or waiting; the verdict is a
                                             comment (10.2)
    catalog.py merge "<number>:<sha256> ..." commits what judge accepted - the judged bytes and
                                             nothing else - closes those issues, starts publish.yml
    catalog.py cards [--issues <file>]       public/cards.txt and public/thumbs/ from the live releases
    catalog.py vt                            public/vt.txt (VT_API_KEY); unknown packages are uploaded
    catalog.py holds                         holds.ltx: the versions VirusTotal blocks
    catalog.py reviews [--csv <url|file>]    public/ratings.txt and public/reviews/ from the review inbox
    catalog.py status [--issues <file>]      the one issue that says what fails
    catalog.py publish --key <file>          public/index.ltx with the next serial, signed

    catalog.py init <folder>                 a new catalog repository: files, workflows, this tool
    catalog.py keygen <file> [--password]    a key file (4.1): the catalog's, or an author's
    catalog.py revoke <id|key:<id>> <version|*> <reason>
    catalog.py review <mods/id.ltx>          every check again, on a maintainer's machine
    catalog.py selftest

Run it from the root of the catalog repository (or pass --repo). The GitHub commands use GH_TOKEN
(or GITHUB_TOKEN) and GITHUB_REPOSITORY, as a workflow has them. Network access goes to GitHub,
the VirusTotal API, the modules' websites and the review inbox only. For tests XMS_CATALOG_GITHUB,
XMS_CATALOG_API and XMS_CATALOG_VT point those somewhere else, XMS_CATALOG_NOW fixes the clock and
XMS_CATALOG_VT_PAUSE / XMS_CATALOG_VT_POLL shorten VirusTotal's pacing.
"""
import argparse
import base64
import contextlib
import csv
import datetime
import getpass
import hashlib
import io
import json
import os
import re
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xms_catalog as xc  # noqa: E402

GITHUB = os.environ.get("XMS_CATALOG_GITHUB", "https://github.com").rstrip("/")
API = os.environ.get("XMS_CATALOG_API", "https://api.github.com").rstrip("/")
VT_API = os.environ.get("XMS_CATALOG_VT", "https://www.virustotal.com/api/v3/").rstrip("/") + "/"
USER_AGENT = "DeadAirRefined-ModCatalog/1"
DESCRIPTOR_LIMIT = 256 * 1024
INDEX_LIMIT = 16 * 1024 * 1024
THUMBNAIL_LIMIT = 256 * 1024
CARDS_LIMIT = 16 * 1024 * 1024
SUBMISSION_LIMIT = 4096
# a release this big or smaller is downloaded whole once; a larger one is read file by file
PACKAGE_FETCH_LIMIT = 2 * 1024 * 1024 * 1024
# VirusTotal's free API: 4 requests a minute, 500 a day; files up to 32 MiB go straight in, up to
# 650 MiB through an upload address
VT_PAUSE = float(os.environ.get("XMS_CATALOG_VT_PAUSE", "16"))
VT_POLL = float(os.environ.get("XMS_CATALOG_VT_POLL", "60"))
VT_POLL_MINUTES = 10
VT_DIRECT_LIMIT = 32 * 1024 * 1024
VT_UPLOAD_LIMIT = 650 * 1024 * 1024
VT_UPLOADS_PER_RUN = 5
VT_REUPLOAD_DAYS = 7

JUDGED_PER_RUN = int(os.environ.get("XMS_CATALOG_JUDGED_PER_RUN", "30"))

BOT = "github-actions[bot]"
MEMBERS = ("OWNER", "MEMBER", "COLLABORATOR")
SUBMISSION_MARKER = "<!-- xms-catalog submission -->"
VERDICT_MARKER = "<!-- xms-catalog verdict="
STATE_MARKER = "<!-- xms-catalog-state "
STATUS_TITLE = "Listed modules failing the checks"
SUBMISSION_BLOCK = re.compile(r"^```ini[ \t]*\n(.*?)^```[ \t]*$", re.S | re.M)
HOLDS_HEADER = ("; Versions withdrawn automatically because VirusTotal blocks them (MOD_CATALOG 11).\n"
                "; publish.yml rewrites this file; catalog.py publish signs it into public/index.ltx\n"
                "; together with revoked.ltx.\n"
                "; <module id> = <version>, <package sha256>+..., <reason>\n"
                "[holds]\n")

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
# a module id; a release asset name cannot start with a dot, so neither can an id the catalog lists
ID_RE = re.compile(r"^[a-z0-9_-][a-z0-9_.-]{0,63}$")
REPO_RE = re.compile(r"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$")
LOGIN_RE = re.compile(r"^[A-Za-z0-9-]{1,39}(\[bot\])?$")
VERSION_TEXT = re.compile(r"^(\*|[0-9][0-9A-Za-z.+-]{0,31})$")
WEBSITE_RE = [
    re.compile(r"^https://(www\.)?ap-pro\.ru/stuff/[a-z0-9_-]+/[a-z0-9_-]+-r\d+/?([?#].*)?$", re.I),
    re.compile(r"^https://(www\.)?ap-pro\.ru/forums/topic/\d+-[a-z0-9_-]+/?([?#].*)?$", re.I),
    re.compile(r"^https://(www\.)?moddb\.com/(mods|addons)/[a-z0-9_-]+(/.*)?$", re.I),
    re.compile(r"^https://(www\.)?moddb\.com/games/[a-z0-9_-]+/addons/[a-z0-9_-]+(/.*)?$", re.I)]


class Finding:
    def __init__(self, level, text):
        self.level = level  # "block", "wait", "review", "note"
        self.text = text


# ---- small helpers ---------------------------------------------------------------------------------------

def now():
    fixed = os.environ.get("XMS_CATALOG_NOW")
    moment = parse_time(fixed) if fixed else datetime.datetime.now(datetime.timezone.utc)
    return moment.replace(microsecond=0)


def parse_time(text):
    moment = datetime.datetime.fromisoformat(text.strip().replace("Z", "+00:00"))
    return moment if moment.tzinfo else moment.replace(tzinfo=datetime.timezone.utc)


def stamp(moment=None):
    return (moment or now()).strftime("%Y-%m-%dT%H:%M:%SZ")


def today():
    return now().date().isoformat()


def days_since(text):
    """Whole days from a date or a moment to now; a very large number for none."""
    try:
        return (now() - parse_time(text if "T" in text else text + "T00:00:00Z")).days
    except (TypeError, ValueError):
        return 1 << 30


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def read_lines(path):
    return read_text(path).splitlines() if os.path.isfile(path) else []


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def write_text(path, text):
    write_bytes(path, text.encode("utf-8"))


def append_text(path, text):
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text)


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


def clean(text, limit=400):
    """Text that came from a submission, a release or a service, made safe for a comment: no markup,
    no mentions, one line."""
    text = re.sub(r"[\x00-\x1f\x7f<]", " ", str(text)).replace("@", "(at)").strip()
    return text if len(text) <= limit else text[:limit - 3] + "..."


def http_get(url, limit, headers=None, byte_range=None):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for name, value in (headers or {}).items():
        # a key goes to the host it is meant for, never on to wherever that host redirects
        request.add_unredirected_header(name, value)
    if byte_range:
        request.add_header("Range", "bytes=%d-%d" % byte_range)
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError("%s is larger than %d bytes" % (url, limit))
        return response.status, data


def release_url(repo, asset):
    return "%s/%s/releases/latest/download/%s" % (GITHUB, repo, urllib.parse.quote(asset))


# ---- GitHub's API -------------------------------------------------------------------------------------------

class ApiError(Exception):
    def __init__(self, status, message):
        super().__init__("GitHub answered HTTP %d: %s" % (status, message))
        self.status = status


class Github:
    """GitHub's REST API with the workflow's token. Requests never leave API, the token never
    follows a redirect."""

    def __init__(self, repo, token):
        if not REPO_RE.match(repo or ""):
            sys.exit("the catalog repository is not known: set GITHUB_REPOSITORY or pass --catalog")
        self.repo, self.token = repo, token

    @staticmethod
    def from_env(repo=None):
        return Github(repo or os.environ.get("GITHUB_REPOSITORY", ""),
                      os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or "")

    def call(self, method, path, body=None, params=None):
        url = path if path.startswith(API + "/") else API + "/" + path.lstrip("/")
        if params:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(params)
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(url, data=data, method=method, headers={
            "User-Agent": USER_AGENT, "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"})
        if data is not None:
            request.add_header("Content-Type", "application/json")
        if self.token:
            request.add_unredirected_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                raw = response.read(64 * 1024 * 1024)
                return (json.loads(raw.decode("utf-8")) if raw.strip() else None), response.headers
        except urllib.error.HTTPError as error:
            try:
                message = json.loads(error.read(64 * 1024).decode("utf-8")).get("message", "")
            except (ValueError, AttributeError, OSError):
                message = ""
            raise ApiError(error.code, message or str(error.reason)) from None

    def get(self, path, params=None):
        return self.call("GET", path, params=params)[0]

    def get_all(self, path, params=None):
        items, url, query = [], path, dict(params or {}, per_page=100)
        while url and len(items) < 10000:
            data, headers = self.call("GET", url, params=query)
            items += data or []
            found = re.search(r'<([^>]+)>;\s*rel="next"', headers.get("Link") or "")
            url, query = (found.group(1) if found and found.group(1).startswith(API + "/") else None), None
        return items

    def branch(self):
        return (self.get("repos/" + self.repo) or {}).get("default_branch") or "main"

    def dispatch(self, workflow, branch):
        self.call("POST", "repos/%s/actions/workflows/%s/dispatches" % (self.repo, urllib.parse.quote(workflow)),
                  {"ref": branch})


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

    @staticmethod
    def load(path):
        return Submission.parse(path, read_text(path))

    @staticmethod
    def parse(path, text):
        return Submission(path, dict(parse_ini(text).get("mod", [])))

    @property
    def owner(self):
        return self.github.split("/")[0]

    def problems(self):
        out = []
        expected = os.path.splitext(os.path.basename(self.path))[0]
        if not ID_RE.match(self.id):
            out.append("id is not a module id (a-z 0-9 _ . -, not starting with a dot)")
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


def listed_dates(repo):
    """Each module's listing date as the signed index has it."""
    listed = repo.index()
    dates = {}
    for key, value in listed[0].get("mods", []) if listed else []:
        fields = [f.strip() for f in value.split(",")]
        if len(fields) >= 4:
            dates[key] = fields[3]
    return dates


def bound_keys(repo, module_id):
    """The author keys a module id is bound to already: its file and the signed index."""
    keys = []
    path = repo.path("mods", module_id + ".ltx")
    if os.path.isfile(path):
        keys.append(Submission.load(path).key)
    listed = repo.index()
    for key, value in listed[0].get("mods", []) if listed else []:
        fields = [f.strip() for f in value.split(",")]
        if key == module_id and len(fields) >= 2:
            keys.append(fields[1])
    return keys


def listing(repo):
    """What the catalog lists: mods/ of this checkout - only files that passed the checks get there -
    with each module's listing date from the signed index, today for a module it does not hold yet."""
    dates = listed_dates(repo)
    return {module_id: {"github": sub.github, "key": sub.key, "min": sub.version, "date": dates.get(module_id, today())}
            for module_id, sub in repo.submissions().items() if not sub.problems()}


def read_holds(path):
    """holds.ltx: {(module id, version): (package hashes, reason)}."""
    holds = {}
    if os.path.isfile(path):
        for module_id, value in parse_ini(read_text(path)).get("holds", []):
            parts = [p.strip() for p in value.split(",", 2)]
            if len(parts) == 3:
                holds[(module_id, parts[0])] = (parts[1], parts[2])
    return holds


def withdrawals(repo):
    """Every (subject, version, reason) the catalog withdraws: by hand (revoked.ltx) and by VirusTotal
    (holds.ltx). A line that is not one is left out rather than signed."""
    rows = repo.revoked() + [(m, v, r) for (m, v), (_, r) in sorted(read_holds(repo.path("holds.ltx")).items())]
    return [(s, v, re.sub(r"[\x00-\x1f;]", " ", r)[:200].strip()) for s, v, r in rows
            if (ID_RE.match(s) or re.fullmatch(r"key:[0-9a-f]{16}", s)) and VERSION_TEXT.match(v)]


def withdrawn(rows, module_id, key_text, version):
    try:
        key_id = xc.key_id(xc.parse_key(key_text))
    except ValueError:
        key_id = ""
    return any(s in (module_id, "key:" + key_id) and v in ("*", version) for s, v, _ in rows)


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
        self.local = {}

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
        if self.files:
            return self.files
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

    def package_file(self, n, folder):
        """Package n (from 1) as a file in folder: fetched once, checked against the descriptor."""
        if n in self.local:
            return self.local[n]
        name, size, sha = self.packages[n - 1]
        path = os.path.join(folder, "package-%d" % n)
        request = urllib.request.Request(release_url(self.repo, name), headers={"User-Agent": USER_AGENT})
        digest, total = hashlib.sha256(), 0
        with urllib.request.urlopen(request, timeout=300) as response, open(path, "wb") as out:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                total += len(chunk)
                if total > size:
                    raise ValueError("%s is larger than the descriptor says" % name)
                digest.update(chunk)
                out.write(chunk)
        if total != size or digest.hexdigest() != sha:
            raise ValueError("%s does not match the descriptor" % name)
        self.local[n] = path
        return path

    def read_file(self, entry, limit=None):
        """The bytes of one file, fetched as the game fetches them: a byte range of its package."""
        if limit is not None and entry["size"] > limit:
            raise ValueError("%s is larger than %d bytes" % (entry["path"], limit))
        if not 1 <= entry["package"] <= len(self.packages) or entry["offset"] < 0 or entry["packed"] < 0:
            raise ValueError("%s points outside the packages" % entry["path"])
        local = self.local.get(entry["package"])
        if local:
            with open(local, "rb") as f:
                f.seek(entry["offset"])
                raw = f.read(entry["packed"])
        else:
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
    for entry in release.fetch_index():
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


class VtUnavailable(Exception):
    """VirusTotal gave no answer about a file: the network, the service or the key."""


class VtQuota(VtUnavailable):
    """The key's free quota is used up (4 lookups a minute, 500 a day): no point asking again today."""


class VtKeyRefused(VtUnavailable):
    """The key itself was refused: no point asking again with it."""


_vt_last = [0.0]


def vt_pace():
    """The free API takes 4 requests a minute: every request waits for its turn."""
    wait = _vt_last[0] + VT_PAUSE - time.monotonic()
    if wait > 0:
        time.sleep(wait)
    _vt_last[0] = time.monotonic()


def vt_error(error):
    if isinstance(error, urllib.error.HTTPError):
        if error.code == 429:
            return VtQuota("the VirusTotal key's quota is used up")
        if error.code in (401, 403):
            return VtKeyRefused("VirusTotal refused the key (HTTP %d)" % error.code)
        return VtUnavailable("VirusTotal answered HTTP %d" % error.code)
    return VtUnavailable("VirusTotal did not answer: %s" % getattr(error, "reason", error))


def vt_lookup(sha, key):
    """The per-engine results of a file VirusTotal knows ({} while it is still analysing it), or None
    when it does not know it. Raises VtQuota when the key's quota is used up and VtUnavailable when
    there was no usable answer."""
    vt_pace()
    try:
        _, data = http_get(VT_API + "files/" + sha, 8 * 1024 * 1024, headers={"x-apikey": key})
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise vt_error(error) from None
    except (urllib.error.URLError, OSError) as error:
        raise vt_error(error) from None
    try:
        return json.loads(data.decode("utf-8"))["data"]["attributes"].get("last_analysis_results") or {}
    except (ValueError, KeyError, TypeError):
        raise VtUnavailable("VirusTotal's answer is not a file report") from None


def vt_upload(path, name, key):
    """Hands a package to VirusTotal; its report follows within minutes."""
    size = os.path.getsize(path)
    url = VT_API + "files"
    try:
        if size > VT_DIRECT_LIMIT:
            vt_pace()
            _, data = http_get(VT_API + "files/upload_url", 64 * 1024, headers={"x-apikey": key})
            url = json.loads(data.decode("utf-8"))["data"]
            # the key goes to VirusTotal's own host and nowhere else
            if urllib.parse.urlsplit(url)[:2] != urllib.parse.urlsplit(VT_API)[:2]:
                raise VtUnavailable("VirusTotal gave an upload address on another host")
        boundary = "xms-" + secrets.token_hex(16)
        head = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="%s"\r\n'
                'Content-Type: application/octet-stream\r\n\r\n' %
                (boundary, re.sub(r"[^A-Za-z0-9._-]", "_", name))).encode("ascii")
        tail = ("\r\n--%s--\r\n" % boundary).encode("ascii")

        def body():
            yield head
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    yield chunk
            yield tail

        request = urllib.request.Request(url, data=body(), method="POST", headers={
            "User-Agent": USER_AGENT, "Content-Type": "multipart/form-data; boundary=" + boundary,
            "Content-Length": str(len(head) + size + len(tail))})
        request.add_unredirected_header("x-apikey", key)
        vt_pace()
        with urllib.request.urlopen(request, timeout=1800) as response:
            response.read(64 * 1024)
    except (urllib.error.URLError, OSError) as error:
        raise vt_error(error) from None
    except (ValueError, KeyError, TypeError):
        raise VtUnavailable("VirusTotal gave no upload address") from None


def vt_counts(policy, tm, ts, om):
    return "block" if tm >= policy["block_trusted"] or om >= policy["block_others"] else \
        "warn" if ts >= policy["warn_trusted"] or om >= policy["warn_others"] else "clean"


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
    return vt_counts(policy, tm, ts, om), len(results), tm, ts, om, flagged


def read_vt(path):
    """public/vt.txt: {sha256: row}."""
    out = {}
    for line in read_lines(path)[1:]:
        parts = line.split(" ")
        if len(parts) == 7:
            try:
                out[parts[0]] = {"line": line, "date": parts[1], "engines": int(parts[2]), "tm": int(parts[3]),
                                 "ts": int(parts[4]), "om": int(parts[5]),
                                 "flagged": [e.replace("_", " ") for e in parts[6].split(",") if e and e != "-"]}
            except ValueError:
                continue
    return out


def vt_package(sha, name, key, policy):
    """(finding, None) when VirusTotal has a report on a package, (None, "unknown") when it does not
    know it, (None, reason) when there is no report yet. VtQuota and VtKeyRefused pass through:
    after them nothing more is asked."""
    try:
        results = vt_lookup(sha, key)
    except (VtQuota, VtKeyRefused):
        raise
    except VtUnavailable as error:
        return None, "VirusTotal did not answer about %s (%s)" % (name, error)
    if results is None:
        return None, "unknown"
    if not results:
        return None, "VirusTotal is analysing %s" % name
    verdict, engines, tm, ts, om, flagged = vt_verdict(results, policy)
    text = "VirusTotal %s for %s: %d engines, %d trusted malicious, %d trusted suspicious, %d other malicious%s" % (
        verdict, name, engines, tm, ts, om, (" (" + ", ".join(flagged) + ")") if flagged else "")
    if verdict == "warn":
        text += "; the game asks the player before it installs"
    return Finding("block" if verdict == "block" else "note", text), None


def vt_checks(release, key, policy, memory=None, folder=None, wait_days=0, poll_minutes=0):
    """VirusTotal's word on every package of a release. The judge (memory given) uploads a package
    VirusTotal does not know and waits for its report - up to poll_minutes within this run, up to
    wait_days in all, then judges without it; review only reports what VirusTotal already has."""
    findings, pending, uploaded = [], {}, False
    try:
        for n, (name, size, sha) in enumerate(release.packages, 1):
            finding, reason = vt_package(sha, name, key, policy)
            if finding:
                findings.append(finding)
                continue
            if reason == "unknown":
                if size > VT_UPLOAD_LIMIT:
                    findings.append(Finding("note", "%s is larger than VirusTotal takes: judged without it" % name))
                    continue
                reason = "VirusTotal is scanning %s" % name
                if memory is None:
                    reason = "VirusTotal does not know %s" % name
                elif days_since(memory.get("vt_uploaded")) >= 1:
                    try:
                        vt_upload(release.package_file(n, folder), name, key)
                        memory["vt_uploaded"] = stamp()
                        uploaded = True
                    except ValueError as error:
                        # the package is not what the signed descriptor says
                        findings.append(Finding("block", str(error)))
                        continue
                    except (VtQuota, VtKeyRefused):
                        raise
                    except (VtUnavailable, urllib.error.URLError, OSError) as error:
                        reason = "%s could not be handed to VirusTotal (%s)" % (name, getattr(error, "reason", error))
            pending[n] = (name, sha, reason)
        # a package handed to VirusTotal is usually analysed within minutes: waiting for it here
        # decides most submissions in the run that uploaded it
        deadline = time.monotonic() + poll_minutes * 60
        while pending and uploaded and time.monotonic() < deadline:
            time.sleep(VT_POLL)
            for n, (name, sha, _) in list(pending.items()):
                finding, _ = vt_package(sha, name, key, policy)
                if finding:
                    findings.append(finding)
                    del pending[n]
    except (VtQuota, VtKeyRefused) as error:
        for n, (name, _, sha) in enumerate(release.packages, 1):
            if n not in pending and not any(name in f.text for f in findings):
                pending[n] = (name, sha, "")
        for n, (name, sha, _) in pending.items():
            pending[n] = (name, sha, "VirusTotal did not answer about %s (%s)" % (name, error))
    if pending:
        late = memory is not None and days_since(memory.setdefault("vt_since", stamp())) >= wait_days
        for name, sha, reason in pending.values():
            if memory is None or late:
                findings.append(Finding("note", reason + (": no report in %d days, judged without it" % wait_days if late else "")))
            else:
                findings.append(Finding("wait", reason))
    return findings


# ---- section 7 ------------------------------------------------------------------------------------------------

def release_checks(repo, sub, author=None):
    """Section 7 short of the release's files and VirusTotal: (findings, release). A failure that
    is not the author's - GitHub or a website not answering - is a wait, not a refusal."""
    findings = [Finding("block", "%s: %s" % (os.path.basename(sub.path), p)) for p in sub.problems()]
    if findings:
        return findings, None
    if author and author.lower() != sub.owner.lower():
        findings.append(Finding("block", "the submission comes from %s, and the repository %s belongs to %s: "
                                         "submit it from %s" % (author, sub.github, sub.owner, sub.owner)))
    if any(key != sub.key for key in bound_keys(repo, sub.id)):
        findings.append(Finding("block", "%s is listed with another author key: sign the release with that key, "
                                         "or submit the module under another id" % sub.id))
    try:
        release = Release(sub.github, sub.id).fetch_descriptor()
    except urllib.error.HTTPError as error:
        if error.code in (404, 410):
            findings.append(Finding("block", "the latest release of %s has no %s.update.ltx: publish the module "
                                             "with XFined Editor first" % (sub.github, sub.id)))
        else:
            findings.append(Finding("wait", "GitHub answered HTTP %d for the release of %s" % (error.code, sub.github)))
        return findings, None
    except (urllib.error.URLError, OSError) as error:
        findings.append(Finding("wait", "the release of %s could not be read this time (%s)" %
                                (sub.github, getattr(error, "reason", error))))
        return findings, None
    except ValueError as error:
        findings.append(Finding("block", "the release descriptor of %s cannot be read: %s" % (sub.github, error)))
        return findings, None
    if release.fields.get("id") != sub.id:
        findings.append(Finding("block", "the release descriptor is for %s" % release.fields.get("id")))
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
        findings += website_findings(website)
    return findings, release


def website_findings(url):
    """A module page that is not there refuses a listing; a site that is down or turns robots away
    only gets a note."""
    try:
        http_get(url, 4 * 1024 * 1024)
    except urllib.error.HTTPError as error:
        if error.code in (404, 410):
            return [Finding("block", "website %s does not exist (HTTP %d)" % (url, error.code))]
        if "moddb.com" in url and error.code in (403, 429, 503):
            return [Finding("note", "ModDB turns automated requests away: %s was not checked" % url)]
        return [Finding("note", "website %s answered HTTP %d" % (url, error.code))]
    except (urllib.error.URLError, OSError) as error:
        return [Finding("note", "website %s could not be reached: %s" % (url, getattr(error, "reason", error)))]
    except ValueError:
        pass
    return []


def content_checks(release, folder=None):
    """The content policy and the script report; a release up to PACKAGE_FETCH_LIMIT is downloaded
    whole first, which is one request instead of one per file."""
    try:
        if folder and sum(size for _, size, _ in release.packages) <= PACKAGE_FETCH_LIMIT:
            for n in range(1, len(release.packages) + 1):
                release.package_file(n, folder)
        return content_report(release)
    except urllib.error.HTTPError as error:
        if error.code in (404, 410, 416):
            return [Finding("block", "the release files could not be read (HTTP %d)" % error.code)]
        return [Finding("wait", "GitHub answered HTTP %d for the release files" % error.code)]
    except (urllib.error.URLError, OSError) as error:
        return [Finding("wait", "the release files could not be downloaded this time (%s)" %
                        getattr(error, "reason", error))]
    except (ValueError, zlib.error, KeyError, IndexError) as error:
        return [Finding("block", "the release files could not be read: %s" % error)]


def accept_settings(config):
    section = dict(config.get("accept", []))
    out = {}
    for key, default in (("min_account_days", 14), ("max_mods_per_owner", 20), ("max_new_per_day", 10),
                         ("vt_wait_days", 3), ("dormant_days", 30)):
        try:
            out[key] = max(0, int(section.get(key, default)))
        except ValueError:
            out[key] = default
    return out


def listing_counts(repo):
    """What is listed already: modules per owner, and new module ids of today."""
    subs = repo.submissions()
    dates = listed_dates(repo)
    owners = {}
    for sub in subs.values():
        owners[sub.owner.lower()] = owners.get(sub.owner.lower(), 0) + 1
    return {"owners": owners, "new_today": sum(1 for module_id in subs if dates.get(module_id, today()) == today())}


def verdict_state(findings):
    if any(f.level == "block" for f in findings):
        return "refused"
    if any(f.level == "wait" for f in findings):
        return "waiting"
    return None


class Outcome:
    def __init__(self, state, findings, sub=None, new=False, text=""):
        self.state, self.findings, self.sub, self.new, self.text = state, findings, sub, new, text


def judge_submission(repo, sub, author, created, settings, counts, vt_key, policy, memory, folder):
    """Section 7 on one submission, cheapest first: the release's files are read last, once
    everything else holds and VirusTotal has spoken. Returns (state, findings, new)."""
    findings, release = release_checks(repo, sub, author)
    new = not bound_keys(repo, sub.id)
    if new and counts["owners"].get(sub.owner.lower(), 0) >= settings["max_mods_per_owner"]:
        findings.append(Finding("block", "%s lists %d modules already, the most one account may list" %
                                (sub.owner, counts["owners"][sub.owner.lower()])))
    # a young account is refused, not kept waiting: a submission that waits is judged again every
    # half hour, and a pile of them from throwaway accounts would eat the workflow's API budget
    age = (now() - created).days
    if age < settings["min_account_days"]:
        ready = (created + datetime.timedelta(days=settings["min_account_days"])).date().isoformat()
        findings.append(Finding("block", "the GitHub account %s is %d days old, and the catalog lists modules of accounts "
                                         "at least %d days old: submit again from %s" %
                                (author, age, settings["min_account_days"], ready)))
    state = verdict_state(findings)
    if state:
        return state, findings, new
    if new and counts["new_today"] >= settings["max_new_per_day"]:
        findings.append(Finding("wait", "%d new modules were listed today, the most in one day: this one follows "
                                        "tomorrow" % counts["new_today"]))
    state = verdict_state(findings)
    if state:
        return state, findings, new
    if vt_key:
        findings += vt_checks(release, vt_key, policy, memory, folder, settings["vt_wait_days"], VT_POLL_MINUTES)
    else:
        findings.append(Finding("note", "VirusTotal is not set up for this catalog: judged without it"))
    state = verdict_state(findings)
    if state:
        return state, findings, new
    findings += content_checks(release, folder)
    return verdict_state(findings) or "accepted", findings, new


def submission_text(body):
    """The mods/<id>.ltx text a submission issue carries in its first ```ini block; None for an
    issue that is not a submission, "" for one without the block."""
    body = (body or "").replace("\r\n", "\n")
    if not body.lstrip().startswith(SUBMISSION_MARKER):
        return None
    found = SUBMISSION_BLOCK.search(body)
    return found.group(1) if found else ""


def submission_of(text):
    """A submission issue's text as the file it would be."""
    module_id = dict(parse_ini(text).get("mod", [])).get("id", "")
    return Submission.parse("mods/%s.ltx" % (module_id if ID_RE.match(module_id) else "invalid"), text)


def judge_issue(api, repo, issue, settings, counts, vt_key, policy, memory, folder):
    text = submission_text(issue.get("body"))
    if text is None:
        return None
    if not text:
        return Outcome("refused", [Finding("block", "the issue holds no ```ini block with the submission: submit "
                                                    "from XFined Editor (Mod > Submit to Mod Browser)")])
    if len(text.encode("utf-8")) > SUBMISSION_LIMIT:
        return Outcome("refused", [Finding("block", "the submission is larger than %d bytes" % SUBMISSION_LIMIT)])
    sub = submission_of(text)
    login = (issue.get("user") or {}).get("login", "")
    user = api.get("users/" + urllib.parse.quote(login))
    state, findings, new = judge_submission(repo, sub, login, parse_time(user["created_at"]), settings, counts, vt_key,
                                            policy, memory, folder)
    return Outcome(state, findings, sub, new, text)


# ---- verdicts --------------------------------------------------------------------------------------------------

def verdict_body(outcome, memory):
    """The verdict comment (10.2): the marker line, one plain sentence, the report."""
    def first_of(level):
        return clean(next((f.text for f in outcome.findings if f.level == level), ""), 300)

    if outcome.state == "accepted":
        summary = "Accepted: the module is listed and appears in the Mod Browser within minutes." if outcome.new else \
            "Accepted: the listing is updated."
    elif outcome.state == "refused":
        summary = "Refused: %s." % first_of("block")
    else:
        summary = "Waiting: %s. The catalog looks again every half hour." % first_of("wait")
    lines = [VERDICT_MARKER + outcome.state + " -->", summary, ""]
    for level, title in (("block", "Blocking"), ("wait", "Waiting"), ("review", "Reported, not blocking"),
                         ("note", "Notes")):
        chosen = [f for f in outcome.findings if f.level == level]
        if chosen:
            lines += ["**%s**" % title] + ["- " + clean(f.text) for f in chosen[:40]]
            if len(chosen) > 40:
                lines.append("- and %d more" % (len(chosen) - 40))
            lines.append("")
    if outcome.state == "refused":
        lines += ["Fix what blocks and submit again from XFined Editor (Mod > Submit to Mod Browser).", ""]
    lines.append("<sub>Judged on %s by the catalog's own checks (MOD_CATALOG.md, section 7); nobody reviews by "
                 "hand.</sub>" % stamp().replace("T", " ").replace("Z", " UTC"))
    if memory:
        lines.append(STATE_MARKER + " ".join("%s=%s" % pair for pair in sorted(memory.items())) + " -->")
    return "\n".join(lines) + "\n"


def previous_verdict(api, number):
    """The newest verdict comment of the catalog's own bot on an issue: (comment, state, memory).
    Anyone can post a comment that looks like one; only the bot's count."""
    latest = None
    for comment in api.get_all("repos/%s/issues/%d/comments" % (api.repo, number)):
        if (comment.get("user") or {}).get("login") == BOT and (comment.get("body") or "").lstrip().startswith(VERDICT_MARKER):
            latest = comment
    if not latest:
        return None, None, {}
    body = latest["body"].lstrip()
    state = body[len(VERDICT_MARKER):].split("-->", 1)[0].strip()
    memory = {}
    found = re.search(re.escape(STATE_MARKER) + r"([^<>\n]*?) -->", body)
    for pair in found.group(1).split() if found else []:
        key, _, value = pair.partition("=")
        if key in ("vt_since", "vt_uploaded") and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", value) or \
                key == "text" and re.fullmatch(r"[0-9a-f]{64}", value):
            memory[key] = value
    return latest, state, memory


def say(api, number, previous, previous_state, state, body):
    """A changed verdict is a new comment - its author hears of it - and an unchanged one edits the last."""
    if previous and previous_state == state:
        if (previous.get("body") or "") != body:
            api.call("PATCH", "repos/%s/issues/comments/%d" % (api.repo, previous["id"]), {"body": body})
    else:
        api.call("POST", "repos/%s/issues/%d/comments" % (api.repo, number), {"body": body})


def close_issue(api, number, reason):
    api.call("PATCH", "repos/%s/issues/%d" % (api.repo, number), {"state": "closed", "state_reason": reason})


def cmd_judge(args):
    repo = Repo(args.repo)
    api = Github.from_env(args.catalog)
    config = repo.config()
    settings, policy = accept_settings(config), vt_policy(config)
    vt_key = os.environ.get("VT_API_KEY")
    counts = listing_counts(repo)
    if args.issue:
        items = [api.get("repos/%s/issues/%d" % (api.repo, args.issue))]
    else:
        items = api.get_all("repos/%s/issues" % api.repo, {"state": "open", "sort": "created", "direction": "asc"})
    merges, lines, judged = [], [], 0
    for item in items:
        number = item["number"]
        pull = "pull_request" in item
        text = None if pull else submission_text(item.get("body"))
        # submissions are issues; any other issue, and maintenance by the repository's own people, is theirs
        if item.get("state") != "open" or (pull and item.get("author_association") in MEMBERS) or (not pull and text is None):
            continue
        # the oldest first, and a bounded number a run: a flood of issues costs time, never the API budget
        if judged >= JUDGED_PER_RUN:
            lines.append("#%d: left for the next run" % number)
            continue
        judged += 1
        try:
            if pull:
                previous, previous_state, _ = previous_verdict(api, number)
                outcome = Outcome("refused", [Finding("block", "submissions are issues, not pull requests: submit "
                                                               "from XFined Editor (Mod > Submit to Mod Browser)")])
                say(api, number, previous, previous_state, "refused", verdict_body(outcome, {}))
                api.call("PATCH", "repos/%s/pulls/%d" % (api.repo, number), {"state": "closed"})
                lines.append("#%d: a pull request - closed" % number)
                continue
            previous, previous_state, memory = previous_verdict(api, number)
            digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
            # a changed submission starts its waits over
            if memory.get("text") != digest:
                memory = {}
            with tempfile.TemporaryDirectory() as folder:
                outcome = judge_issue(api, repo, item, settings, counts, vt_key, policy, memory, folder)
            memory["text"] = digest
            body = verdict_body(outcome, memory if outcome.state == "waiting" else {})
            say(api, number, previous, previous_state, outcome.state, body)
            if outcome.state == "refused":
                close_issue(api, number, "not_planned")
            elif outcome.state == "accepted":
                merges.append("%d:%s" % (number, digest))
                if outcome.new:
                    counts["new_today"] += 1
                    owner = outcome.sub.owner.lower()
                    counts["owners"][owner] = counts["owners"].get(owner, 0) + 1
            lines.append("#%d: %s" % (number, body.splitlines()[1]))
        except (ApiError, urllib.error.URLError, OSError, KeyError, TypeError, ValueError) as error:
            lines.append("#%d: not judged this time (%s)" % (number, error))
    print("\n".join(lines) or "no open submissions")
    append_text(args.outputs, "merge=%s\n" % " ".join(merges))
    append_text(args.summary, "## Submissions\n\n" + ("\n".join("- " + clean(line) for line in lines) or "none open") + "\n")
    return 0


def git(repo, *args):
    # the catalog's bot commits, and replays its commits when a rebase needs to
    env = dict(os.environ, GIT_COMMITTER_NAME="catalog-bot", GIT_COMMITTER_EMAIL="catalog-bot@users.noreply.github.com")
    return subprocess.run(["git", "-C", repo.root] + list(args), check=True, capture_output=True, text=True, env=env).stdout


def push(repo, branch):
    """Pushes this checkout's commits, rebased onto whatever reached the branch meanwhile."""
    for attempt in range(3):
        try:
            git(repo, "pull", "--rebase", "origin", branch)
            git(repo, "push", "origin", "HEAD:" + branch)
            return
        except subprocess.CalledProcessError:
            if attempt == 2:
                raise
            time.sleep(5)


def cmd_merge(args):
    """Commits what judge accepted: the file is the judged bytes exactly, re-read and re-checked
    against their hash here, so a submission edited after the verdict waits for its next verdict."""
    repo = Repo(args.repo)
    api = Github.from_env(args.catalog)
    branch = api.branch()
    accepted = []
    for pair in args.pairs.split():
        found = re.fullmatch(r"(\d{1,9}):([0-9a-f]{64})", pair)
        if not found:
            print("ignored: %s" % clean(pair, 80))
            continue
        number, digest = int(found.group(1)), found.group(2)
        issue = api.get("repos/%s/issues/%d" % (api.repo, number))
        text = submission_text(issue.get("body"))
        if issue.get("state") != "open" or "pull_request" in issue or not text or \
                hashlib.sha256(text.encode("utf-8")).hexdigest() != digest:
            print("#%d changed since it was judged: the next run judges it again" % number)
            continue
        sub = submission_of(text)
        user = issue.get("user") or {}
        login = user.get("login", "")
        # what a writer can check without trusting the judge: a well-formed file from its repository's
        # owner, for an id that is free or bound to this very key
        if sub.problems() or not LOGIN_RE.match(login) or login.lower() != sub.owner.lower() or \
                any(key != sub.key for key in bound_keys(repo, sub.id)):
            print("#%d does not hold up: the next run judges it again" % number)
            continue
        path = repo.path("mods", sub.id + ".ltx")
        old = open(path, "rb").read() if os.path.isfile(path) else None
        if old != text.encode("utf-8"):
            write_bytes(path, text.encode("utf-8"))
            git(repo, "add", "--", "mods/%s.ltx" % sub.id)
            git(repo, "commit", "-q", "--author=%s <%d+%s@users.noreply.github.com>" % (login, int(user.get("id", 0)), login),
                "-m", "%s %s (#%d)" % ("Update" if old is not None else "List", sub.id, number),
                "-m", "Accepted by the catalog's own checks (MOD_CATALOG.md 10.2).")
        accepted.append((number, sub.id))
    if not accepted:
        return 0
    push(repo, branch)
    for number, _ in accepted:
        close_issue(api, number, "completed")
    api.dispatch("publish.yml", branch)
    print("listed %s; publish.yml started" % ", ".join(module_id for _, module_id in accepted))
    return 0


# ---- the pipeline: cards, VirusTotal, holds, reviews, the status issue ------------------------------------

def read_live(path):
    """state/live.txt: the live release of every listed module whose descriptor its key signs,
    withdrawn or not: {id: {version, github, packages}}."""
    out = {}
    for line in read_lines(path):
        parts = line.split(" ")
        if len(parts) < 3:
            continue
        packages = []
        for item in parts[3:]:
            name, _, rest = item.partition(":")
            size, _, sha = rest.partition(":")
            if size.isdigit() and re.fullmatch(r"[0-9a-f]{64}", sha):
                packages.append((urllib.parse.unquote(name), int(size), sha))
        out[parts[0]] = {"version": urllib.parse.unquote(parts[1]), "github": parts[2], "packages": packages}
    return out


def read_pairs(path):
    out = {}
    for line in read_lines(path):
        key, _, value = line.strip().partition(" ")
        if key:
            out[key] = value.strip()
    return out


def write_pairs(path, pairs):
    write_text(path, "".join("%s %s\n" % pair for pair in sorted(pairs.items())))


def cmd_cards(args):
    repo = Repo(args.repo)
    settings = accept_settings(repo.config())
    mods, rows = listing(repo), withdrawals(repo)
    previous = read_cards(repo.path("public", "cards.txt"))
    failing = read_pairs(repo.path("state", "failing.txt"))
    out, thumbs, issues, live, still = [b"xms-cards 1\n"], set(), [], [], {}
    for module_id, listed in sorted(mods.items()):
        try:
            release = Release(listed["github"], module_id).fetch_descriptor()
            release.verify_key(listed["key"])
            if release.fields.get("id") != module_id:
                raise ValueError("the descriptor names %s" % release.fields.get("id"))
            live.append(" ".join([module_id, urllib.parse.quote(release.version, safe=""), listed["github"]] +
                                 ["%s:%d:%s" % (urllib.parse.quote(n, safe=""), s, h) for n, s, h in release.packages]))
            if version_tuple(release.version) < version_tuple(listed["min"]):
                raise ValueError("release %s is below the listed %s" % (release.version, listed["min"]))
            if withdrawn(rows, module_id, listed["key"], release.version):
                raise ValueError("release %s is withdrawn" % release.version)
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
        except (urllib.error.URLError, OSError, ValueError, zlib.error) as error:
            since = failing.get(module_id, today())
            still[module_id] = since
            issues.append("%s: %s (since %s)" % (module_id, getattr(error, "reason", error), since))
            text = previous.get(module_id)
            # a module whose release stays broken leaves the browser; its id stays bound to its key
            if text is None or days_since(since) >= settings["dormant_days"]:
                continue
        out.append(b"card %s %d\n" % (module_id.encode(), len(text)) + text)
    data = b"".join(out)
    if len(data) > CARDS_LIMIT:
        sys.exit("cards.txt would exceed 16 MiB")
    write_bytes(repo.path("public", "cards.txt"), data)
    write_text(repo.path("state", "live.txt"), "".join(line + "\n" for line in live))
    write_pairs(repo.path("state", "failing.txt"), still)
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
    # every package of a served card and of a live release, withdrawn ones included: a hold is
    # lifted by a later report of the same package
    mods = listing(repo)
    packages = {}
    for module_id, entry in read_live(repo.path("state", "live.txt")).items():
        for name, size, sha in entry["packages"]:
            packages.setdefault(sha, (module_id, entry["github"], name, size))
    for module_id, text in read_cards(repo.path("public", "cards.txt")).items():
        try:
            body, _, _, _ = xc.split_signed(text)
        except ValueError:
            continue
        for name, value in parse_ini(body.decode("utf-8")).get("packages", []):
            size, _, sha = value.partition(",")
            if size.strip().isdigit():
                packages.setdefault(sha.strip().lower(), (module_id, (mods.get(module_id) or {}).get("github"), name,
                                                          int(size)))
    old = {sha: row["line"] for sha, row in read_vt(repo.path("public", "vt.txt")).items()}
    uploads = read_pairs(repo.path("state", "vt-uploads.txt"))
    # A VirusTotal that cannot answer - the quota used up, the key refused, the service down -
    # costs a stale vt.txt and never the run: the cards and the reviews still go out, and the
    # game treats a package without a result as one VirusTotal does not know (MOD_CATALOG 11).
    lines, stopped, uploaded = ["xms-vt 1"], "", 0
    with tempfile.TemporaryDirectory() as folder:
        for sha, (module_id, github, name, size) in sorted(packages.items(), key=lambda item: (item[1][0], item[1][2])):
            # a result younger than a day is kept: the free API allows 500 lookups a day
            if (sha in old and old[sha].split(" ")[1] == today()) or stopped:
                if sha in old:
                    lines.append(old[sha])
                continue
            try:
                results = vt_lookup(sha, key)
            except VtUnavailable as error:
                print("%s %s: %s - the last result stays" % (module_id, name, error))
                if sha in old:
                    lines.append(old[sha])
                if isinstance(error, (VtQuota, VtKeyRefused)):
                    stopped = str(error)
                continue
            if not results:
                if sha in old:
                    lines.append(old[sha])
                # a package nobody handed to VirusTotal is handed to it by the catalog: an update
                # an author released gets a report like the release they submitted
                if results is None and github and size <= VT_UPLOAD_LIMIT and uploaded < VT_UPLOADS_PER_RUN and \
                        days_since(uploads.get(sha)) >= VT_REUPLOAD_DAYS:
                    try:
                        release = Release(github, module_id)
                        release.packages = [(name, size, sha)]
                        vt_upload(release.package_file(1, folder), name, key)
                        uploads[sha] = today()
                        uploaded += 1
                        print("%s %s: handed to VirusTotal" % (module_id, name))
                    except VtUnavailable as error:
                        print("%s %s: not handed to VirusTotal (%s)" % (module_id, name, error))
                        if isinstance(error, (VtQuota, VtKeyRefused)):
                            stopped = str(error)
                    except (urllib.error.URLError, OSError, ValueError) as error:
                        print("%s %s: not handed to VirusTotal (%s)" % (module_id, name, getattr(error, "reason", error)))
                continue
            verdict, engines, tm, ts, om, flagged = vt_verdict(results, policy)
            lines.append("%s %s %d %d %d %d %s" % (sha, today(), engines, tm, ts, om,
                                                   ",".join(e.replace(" ", "_") for e in flagged) or "-"))
            print("%s %s: %s" % (module_id, name, verdict))
    write_text(repo.path("public", "vt.txt"), "\n".join(lines) + "\n")
    write_pairs(repo.path("state", "vt-uploads.txt"), {sha: date for sha, date in uploads.items() if sha in packages})
    if stopped:
        print("VirusTotal stopped for this run (%s): the other packages keep their last results" % stopped)
    return 0


def cmd_holds(args):
    """holds.ltx from VirusTotal's latest word: a live release it blocks is withdrawn and lifted again
    when every package of it has a later report that does not block; an older release that was
    withdrawn stays withdrawn."""
    repo = Repo(args.repo)
    policy = vt_policy(repo.config())
    vt = read_vt(repo.path("public", "vt.txt"))
    live = read_live(repo.path("state", "live.txt"))
    path = repo.path("holds.ltx")
    old = read_holds(path)
    holds = {key: row for key, row in old.items() if key[0] not in live or live[key[0]]["version"] != key[1]}
    for module_id, entry in live.items():
        key = (module_id, entry["version"])
        shas = [sha for _, _, sha in entry["packages"]]
        verdicts = [vt_counts(policy, vt[sha]["tm"], vt[sha]["ts"], vt[sha]["om"]) if sha in vt else None for sha in shas]
        if "block" in verdicts:
            flagged = sorted({engine for sha in shas if sha in vt for engine in vt[sha]["flagged"]})
            holds[key] = ("+".join(shas), "VirusTotal: " + (", ".join(flagged) or "blocked"))
        elif key in old and (None in verdicts or not shas):
            holds[key] = old[key]
    if holds != old or not os.path.isfile(path):
        write_text(path, HOLDS_HEADER + "".join("%s = %s, %s, %s\n" % (m, v, shas, reason)
                                                for (m, v), (shas, reason) in sorted(holds.items())))
    print("holds: %d version(s) withdrawn%s" % (len(holds), ", changed" if holds != old else ""))
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
    mods = listing(repo)
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
        except (ValueError, KeyError):
            rejected += 1
            continue
        listed = mods.get(review["mod"])
        if not listed or review["date"] < listed["date"]:
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


def cmd_status(args):
    """One issue says what fails: opened when something does, kept up to date, closed when nothing
    does - never an issue per run in a repository nobody may be reading."""
    api = Github.from_env(args.catalog)
    found = [line.strip() for line in read_lines(args.issues) if line.strip()] if args.issues else []
    issues = api.get_all("repos/%s/issues" % api.repo, {"state": "open", "creator": BOT})
    current = next((i for i in issues if i.get("title") == STATUS_TITLE and "pull_request" not in i), None)
    if found:
        body = ("What the catalog's last check found (%s). publish.yml keeps this issue up to date and closes it "
                "when nothing fails.\n\n%s\n" % (stamp().replace("T", " ").replace("Z", " UTC"),
                                                 "\n".join("- " + clean(line.lstrip("- ")) for line in found)))
        if current:
            api.call("PATCH", "repos/%s/issues/%d" % (api.repo, current["number"]), {"body": body})
        else:
            api.call("POST", "repos/%s/issues" % api.repo, {"title": STATUS_TITLE, "body": body})
        print("status issue: %d finding(s)" % len(found))
    elif current:
        api.call("PATCH", "repos/%s/issues/%d" % (api.repo, current["number"]),
                 {"state": "closed", "state_reason": "completed",
                  "body": "Nothing fails since %s." % stamp().replace("T", " ").replace("Z", " UTC")})
        print("status issue: closed")
    return 0


# ---- signing and the maintainer's own tools -------------------------------------------------------------

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
    dates = listed_dates(repo)
    lines = ["[catalog]", "format  = 1", "serial  = %d" % serial, "issued  = %s" % today()]
    mirrors = first(config, "catalog", "mirrors")
    if mirrors:
        lines.append("mirrors = " + mirrors)
    lines += ["", "[virustotal]"] + ["%s = %s" % (k, v) for k, v in config.get("virustotal", [])]
    reviews = dict(config.get("reviews", []))
    if reviews.get("inbox_url") and reviews.get("inbox_field"):
        lines += ["", "[reviews]"] + ["%s = %s" % (k, reviews[k]) for k in
                                      ("inbox_url", "inbox_field", "salt", "identity_bits", "review_bits") if reviews.get(k)]
    # a module keeps the date it was first listed on, whatever its listing changes later
    lines += ["", "[mods]"] + ["%s = %s, %s, %s, %s" % (module_id, sub.github, sub.key, sub.version,
                                                        dates.get(module_id) or today())
                               for module_id, sub in sorted(subs.items())]
    lines += ["", "[revoked]"] + ["%s = %s, %s" % row for row in withdrawals(repo)]
    body = "\n".join(lines) + "\n"
    # an index that would say the same again is not signed again: no new serial, no commit
    if previous and not getattr(args, "force", False):
        old_body, _, old_signer, _ = xc.split_signed(previous[1])
        if old_signer == xc.key_id(xc.public_of(d)) and unstamped(old_body.decode("utf-8")) == unstamped(body):
            print("index.ltx: nothing changed, serial %d stays" % (serial - 1))
            if not args.no_cards:
                cmd_cards(argparse.Namespace(repo=args.repo, issues=None))
            return 0
    text = xc.sign_text(body, d)
    write_text(repo.path("public", "index.ltx"), text)
    print("index.ltx: serial %d, %d module(s), signed by %s" % (serial, len(subs), xc.key_id(xc.public_of(d))))
    if not args.no_cards:
        cmd_cards(argparse.Namespace(repo=args.repo, issues=None))
    return 0


def unstamped(body):
    return [line for line in body.splitlines() if not re.match(r"^(serial|issued)\s*=", line)]


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
    print("revoked %s (%s): push it, and publish.yml signs it in" % (target, args.version))
    return 0


def cmd_review(args):
    """Every check again on a maintainer's machine; nothing is uploaded and nothing waited for."""
    repo = Repo(args.repo)
    sub = Submission.load(args.target)
    findings, release = release_checks(repo, sub)
    if release and not verdict_state(findings):
        key = os.environ.get("VT_API_KEY")
        if key:
            findings += vt_checks(release, key, vt_policy(repo.config()))
        with tempfile.TemporaryDirectory() as folder:
            findings += content_checks(release, folder)
    print(report(findings, "%s (%s)" % (sub.id, sub.github)))
    if release:
        print("release %s, packages: %s" % (release.version, ", ".join("%s (%s)" % (n, s[:12]) for n, _, s in release.packages)))
        for _, _, sha in release.packages:
            print("VirusTotal: https://www.virustotal.com/gui/file/%s" % sha)
    return 1 if any(f.level == "block" for f in findings) else 0


def report(findings, title):
    lines = ["## " + title, ""]
    if not findings:
        lines.append("Nothing to report.")
    for level, name in (("block", "Blocking"), ("wait", "Not checked this time"), ("review", "Reported, not blocking"),
                        ("note", "Notes")):
        chosen = [f for f in findings if f.level == level]
        if chosen:
            lines += ["**%s**" % name] + ["- " + f.text for f in chosen] + [""]
    return "\n".join(lines) + "\n"


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


def cmd_init(args):
    target = os.path.abspath(args.folder)
    if os.path.exists(target) and os.listdir(target):
        sys.exit("%s is not empty" % target)
    templates = os.path.join(HERE, "template")
    shutil.copytree(templates, target, dirs_exist_ok=True)
    os.makedirs(os.path.join(target, "tools"), exist_ok=True)
    for name in ("catalog.py", "xms_catalog.py"):
        shutil.copy2(os.path.join(HERE, name), os.path.join(target, "tools", name))
    for name in ("mods", "state", os.path.join("public", "thumbs"), os.path.join("public", "reviews")):
        os.makedirs(os.path.join(target, name), exist_ok=True)
        keep = os.path.join(target, name, ".gitkeep")
        if not os.path.exists(keep):
            open(keep, "w").close()
    print("catalog repository: %s" % target)
    print("next: catalog.py keygen <key file> --password, then catalog.py publish --key <key file> in it")
    return 0


# ---- selftest --------------------------------------------------------------------------------------------------

def selftest_vt_quota():
    """VirusTotal out of quota: vt.txt keeps every result it had and the run still succeeds."""
    with tempfile.TemporaryDirectory() as folder:
        d = xc.new_private()
        cards, old_lines = b"xms-cards 1\n", ["xms-vt 1"]
        for n in range(2):
            sha = hashlib.sha256(b"package %d" % n).hexdigest()
            body = "[release]\nid = m%d\nversion = 1.0.0\n\n[packages]\nm%d-1.0.0.zip = 10, %s\n" % (n, n, sha)
            text = xc.sign_text(body, d).encode("utf-8")
            cards += b"card m%d %d\n" % (n, len(text)) + text
            old_lines.append("%s 2000-01-01 70 0 0 0 -" % sha)
        write_bytes(os.path.join(folder, "public", "cards.txt"), cards)
        write_text(os.path.join(folder, "public", "vt.txt"), "\n".join(old_lines) + "\n")

        def quota(*_args, **_kwargs):
            raise urllib.error.HTTPError(VT_API, 429, "Quota exceeded", None, None)

        saved_get, saved_key = globals()["http_get"], os.environ.get("VT_API_KEY")
        globals()["http_get"] = quota
        os.environ["VT_API_KEY"] = "selftest"
        try:
            # what the run says about the quota is the expected outcome here, not news
            with contextlib.redirect_stdout(io.StringIO()):
                assert cmd_vt(argparse.Namespace(repo=folder)) == 0
        finally:
            globals()["http_get"] = saved_get
            if saved_key is None:
                os.environ.pop("VT_API_KEY", None)
            else:
                os.environ["VT_API_KEY"] = saved_key
        assert read_text(os.path.join(folder, "public", "vt.txt")).splitlines() == old_lines


def selftest_verdicts():
    """The verdict comment reads back as it was written, a forged one does not count, and a
    submission is found in an issue whatever its line ends."""
    class Api:
        repo = "o/r"

        def __init__(self, comments):
            self.comments = comments

        def get_all(self, _path, _params=None):
            return self.comments

    memory = {"vt_since": "2026-09-25T10:00:00Z", "text": "ab" * 32}
    body = verdict_body(Outcome("waiting", [Finding("wait", "VirusTotal is scanning <x>.zip @someone")]), memory)
    assert body.startswith(VERDICT_MARKER + "waiting -->\nWaiting: VirusTotal is scanning  x>.zip (at)someone."), body
    ours = {"id": 1, "user": {"login": BOT}, "body": body}
    forged = {"id": 2, "user": {"login": "someone"}, "body": VERDICT_MARKER + "accepted -->\nAccepted: sure.\n"}
    comment, state, read = previous_verdict(Api([ours, forged]), 1)
    assert comment is ours and state == "waiting" and read == memory, (state, read)
    text = "[mod]\nid = a\n"
    issue = SUBMISSION_MARKER + "\r\nFrom the editor.\r\n\r\n```ini\r\n" + text.replace("\n", "\r\n") + "```\r\nrest\r\n"
    assert submission_text(issue) == text and submission_text("hello") is None
    assert submission_text(SUBMISSION_MARKER + "\nno block\n") == ""


def selftest_holds():
    """A live release VirusTotal blocks is withdrawn, lifted by a clean report, and an older
    release stays withdrawn."""
    with tempfile.TemporaryDirectory() as folder:
        blocked, clean_sha, old = (hashlib.sha256(s).hexdigest() for s in (b"blocked", b"clean", b"old"))
        write_text(os.path.join(folder, "catalog.ltx"), "[virustotal]\ntrusted = Kaspersky\nblock_trusted = 1\n")
        write_text(os.path.join(folder, "state", "live.txt"), "a 1.2.0 o/a a-1.2.0.zip:10:%s\nb 2.0.0 o/b b-2.0.0.zip:10:%s\n" %
                   (blocked, clean_sha))
        write_text(os.path.join(folder, "public", "vt.txt"), "xms-vt 1\n%s 2026-09-25 70 1 0 0 Kaspersky\n%s 2026-09-25 70 0 0 0 -\n" %
                   (blocked, clean_sha))
        write_text(os.path.join(folder, "holds.ltx"), HOLDS_HEADER + "b = 2.0.0, %s, VirusTotal: Kaspersky\n"
                                                                     "c = 1.0.0, %s, VirusTotal: Kaspersky\n" % (clean_sha, old))
        with contextlib.redirect_stdout(io.StringIO()):
            cmd_holds(argparse.Namespace(repo=folder))
        holds = read_holds(os.path.join(folder, "holds.ltx"))
        assert set(holds) == {("a", "1.2.0"), ("c", "1.0.0")}, holds
        assert holds[("a", "1.2.0")][1] == "VirusTotal: Kaspersky"
        rows = withdrawals(Repo(folder))
        assert ("a", "1.2.0", "VirusTotal: Kaspersky") in rows and not withdrawn(rows, "b", "", "2.0.0")


def cmd_selftest(_args):
    globals()["VT_PAUSE"] = 0
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
    assert ID_RE.match("darf-overnight-delivery") and not ID_RE.match(".hidden") and not ID_RE.match("A")
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
    assert clean("a <b> @c\nd") == "a  b> (at)c d"
    selftest_vt_quota()
    selftest_verdicts()
    selftest_holds()
    print("catalog selftest: ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--repo", default=".", help="the catalog repository (default: here)")
    parser.add_argument("--catalog", help="owner/repo on GitHub (default: GITHUB_REPOSITORY)")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("judge")
    p.add_argument("--issue", type=int, help="judge this issue only")
    p.add_argument("--outputs", help="where the merge list goes (GITHUB_OUTPUT)")
    p.add_argument("--summary", help="where the run's summary goes (GITHUB_STEP_SUMMARY)")
    p = sub.add_parser("merge")
    p.add_argument("pairs")
    p = sub.add_parser("cards")
    p.add_argument("--issues")
    sub.add_parser("vt")
    sub.add_parser("holds")
    p = sub.add_parser("reviews")
    p.add_argument("--csv")
    p = sub.add_parser("status")
    p.add_argument("--issues")
    p = sub.add_parser("publish")
    p.add_argument("--key", required=True)
    p.add_argument("--password-env", help="the environment variable holding the key file's password")
    p.add_argument("--no-cards", action="store_true")
    p.add_argument("--force", action="store_true", help="sign a new serial even when nothing changed")
    p = sub.add_parser("init")
    p.add_argument("folder")
    p = sub.add_parser("keygen")
    p.add_argument("file")
    p.add_argument("--password", action="store_true", help="protect the key file with a password")
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
