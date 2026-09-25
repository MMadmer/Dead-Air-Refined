"""The catalog judging and listing by itself (MOD_CATALOG 10.2), against a mock of GitHub's API,
of release downloads and of VirusTotal, with a real git remote: accepted, refused and waiting
submissions, the merge committing the judged bytes only, a forged verdict, the listing rules,
withdrawals from a VirusTotal block, the status issue and a dormant module.

    python test_accept.py
"""
import argparse
import contextlib
import hashlib
import http.server
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import catalog  # noqa: E402
import xms_catalog as xc  # noqa: E402

REPO = "darf/catalog"
BOT_TOKEN = "bot-token"
VT_KEY = "vt-key"
CLEAN = {"Kaspersky": {"category": "undetected"}, "Microsoft": {"category": "undetected"},
         "NoName": {"category": "undetected"}}
WARN = {"Kaspersky": {"category": "undetected"}, "NoName": {"category": "malicious"}, "Other": {"category": "malicious"}}
BLOCK = {"Kaspersky": {"category": "malicious"}, "Microsoft": {"category": "undetected"}}
WEBSITE = "https://ap-pro.ru/stuff/mods/test-r1/"


class Mock:
    def __init__(self):
        self.issues, self.comments, self.users, self.assets = {}, {}, {}, {}
        self.vt, self.vt_uploads, self.dispatches = {}, [], []
        self.next_id = 5000
        self.base = ""

    def issue(self, user, body, title="Submit", pull=False, association="NONE"):
        number = len(self.issues) + 1
        self.issues[number] = {"number": number, "title": title, "body": body, "state": "open", "state_reason": None,
                               "user": {"login": user, "id": self.users.get(user, {}).get("id", 99)},
                               "author_association": association}
        if pull:
            self.issues[number]["pull_request"] = {"url": "x"}
        return number

    def comment(self, number, user, body):
        self.next_id += 1
        entry = {"id": self.next_id, "user": {"login": user}, "body": body, "edits": 0}
        self.comments.setdefault(number, []).append(entry)
        return entry

    def bot_comments(self, number):
        return [c for c in self.comments.get(number, []) if c["user"]["login"] == catalog.BOT]


MOCK = Mock()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def send(self, code, body=None, headers=None):
        data = b"" if body is None else body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        self.send_response(code)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length") or 0))

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PATCH(self):
        self.route("PATCH")

    def route(self, method):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        if url.path.startswith("/api/"):
            return self.api(method, url.path[4:], query)
        if url.path.startswith("/web/"):
            return self.web(url.path[4:])
        if url.path.startswith("/vt/"):
            return self.virustotal(method, url.path[3:])
        self.send(404)

    def page(self, items, query, path):
        # two to a page: the client has to follow the Link header
        number = int(query.get("page", 1))
        headers = {}
        if number * 2 < len(items):
            headers["Link"] = '<%s/api%s?%s>; rel="next"' % (MOCK.base, path, urllib.parse.urlencode(dict(query, page=number + 1)))
        self.send(200, items[(number - 1) * 2:number * 2], headers)

    def api(self, method, path, query):
        user = catalog.BOT if self.headers.get("Authorization") == "Bearer " + BOT_TOKEN else None
        if method != "GET" and not user:
            return self.send(401, {"message": "Requires authentication"})
        base = "/repos/" + REPO
        found = re.fullmatch(r"/users/([^/]+)", path)
        if found:
            entry = MOCK.users.get(urllib.parse.unquote(found.group(1)))
            return self.send(200, entry) if entry else self.send(404, {"message": "Not Found"})
        if path == base:
            return self.send(200, {"full_name": REPO, "default_branch": "main"})
        if path == base + "/issues":
            if method == "POST":
                data = json.loads(self.body())
                number = MOCK.issue(user, data["body"], data["title"])
                return self.send(201, MOCK.issues[number])
            items = [i for _, i in sorted(MOCK.issues.items()) if query.get("state", "open") in ("all", i["state"])]
            if "creator" in query:
                items = [i for i in items if i["user"]["login"] == query["creator"]]
            return self.page(items, query, path)
        found = re.fullmatch(re.escape(base) + r"/(issues|pulls)/(\d+)", path)
        if found:
            issue = MOCK.issues.get(int(found.group(2)))
            if not issue:
                return self.send(404, {"message": "Not Found"})
            if method == "PATCH":
                issue.update({k: v for k, v in json.loads(self.body()).items() if k in ("state", "state_reason", "body")})
            return self.send(200, issue)
        found = re.fullmatch(re.escape(base) + r"/issues/(\d+)/comments", path)
        if found:
            number = int(found.group(1))
            if method == "POST":
                return self.send(201, MOCK.comment(number, user, json.loads(self.body())["body"]))
            return self.page(MOCK.comments.get(number, []), query, path)
        found = re.fullmatch(re.escape(base) + r"/issues/comments/(\d+)", path)
        if found and method == "PATCH":
            for comments in MOCK.comments.values():
                for entry in comments:
                    if entry["id"] == int(found.group(1)):
                        entry["body"] = json.loads(self.body())["body"]
                        entry["edits"] += 1
                        return self.send(200, entry)
        found = re.fullmatch(re.escape(base) + r"/actions/workflows/([^/]+)/dispatches", path)
        if found and method == "POST":
            MOCK.dispatches.append((found.group(1), json.loads(self.body())["ref"]))
            return self.send(204)
        self.send(404, {"message": "Not Found"})

    def web(self, path):
        found = re.fullmatch(r"/([^/]+/[^/]+)/releases/latest/download/(.+)", path)
        data = MOCK.assets.get((found.group(1), urllib.parse.unquote(found.group(2)))) if found else None
        if data is None:
            return self.send(404)
        wanted = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range") or "")
        if wanted:
            first, last = int(wanted.group(1)), int(wanted.group(2))
            return self.send(206, data[first:last + 1])
        self.send(200, data)

    def virustotal(self, method, path):
        if self.headers.get("x-apikey") != VT_KEY:
            return self.send(401, {"error": {"code": "WrongCredentialsError"}})
        found = re.fullmatch(r"/api/v3/files/([0-9a-f]{64})", path)
        if found and method == "GET":
            answers = MOCK.vt.get(found.group(1))
            if answers is None:
                return self.send(404, {"error": {"code": "NotFoundError"}})
            results = answers.pop(0) if len(answers) > 1 else answers[0]
            return self.send(200, {"data": {"attributes": {"last_analysis_results": results}}})
        if path == "/api/v3/files/upload_url" and method == "GET":
            return self.send(200, {"data": MOCK.base + "/vt/upload/one"})
        if method == "POST" and path in ("/api/v3/files", "/upload/one"):
            boundary = re.search(r"boundary=(\S+)", self.headers["Content-Type"]).group(1).encode()
            part = self.body().split(b"--" + boundary)[1]
            sha = hashlib.sha256(part.split(b"\r\n\r\n", 1)[1][:-2]).hexdigest()
            MOCK.vt_uploads.append(sha)
            # analysed after one look: the judge has to wait for it within its run
            MOCK.vt.setdefault(sha, [{}, CLEAN])
            return self.send(200, {"data": {"type": "analysis", "id": "x"}})
        self.send(404)


# ---- fixtures ------------------------------------------------------------------------------------------------

def sha(data):
    return hashlib.sha256(data).hexdigest()


def deflate(data):
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    return packer.compress(data) + packer.flush()


def release(owner_repo, module_id, version, key, files, website=WEBSITE, vt=None):
    """A module release as Package Release makes it: signed descriptor, file index, one package."""
    blob, lines = b"", ["xms-files 1"]
    # every release its own package: VirusTotal knows packages by their hash
    for path, data in files + [("gamedata/configs/release.ltx", ("[%s]\nversion = %s\n" % (module_id, version)).encode())]:
        packed = deflate(data)
        lines.append("file %s %d 1 %d %d 8 %s" % (sha(data), len(data), len(blob), len(packed), path))
        blob += packed
    index = ("\n".join(lines) + "\n").encode("utf-8")
    index_name, package = "%s-%s.files" % (module_id, version), "%s-%s.zip" % (module_id, version)
    body = ("[release]\nid = %s\nversion = %s\nindex = %s, %d, %s\n\n[packages]\n%s = %d, %s\n\n"
            "[card]\nname = \"%s\"\nwebsite = %s\n" % (module_id, version, index_name, len(index), sha(index), package,
                                                     len(blob), sha(blob), module_id, website))
    MOCK.assets[(owner_repo, module_id + ".update.ltx")] = xc.sign_text(body, key).encode("utf-8")
    MOCK.assets[(owner_repo, index_name)] = index
    MOCK.assets[(owner_repo, package)] = blob
    if vt is not None:
        MOCK.vt[sha(blob)] = [vt]
    return sha(blob)


def submission(module_id, github, key, version="1.0.0", website=WEBSITE):
    return "[mod]\nid      = %s\ngithub  = %s\nkey     = %s\nversion = %s\nwebsite = %s\n" % (
        module_id, github, xc.key_text(xc.public_of(key)), version, website)


def issue_body(text, extra="The editor's report."):
    return catalog.SUBMISSION_MARKER + "\nSubmitted from XFined Editor.\n\n```ini\n" + text + "```\n\n" + extra + "\n"


def git(folder, *args):
    return subprocess.run(["git", "-C", folder] + list(args), check=True, capture_output=True, text=True).stdout


GOOD_FILES = [("gamedata/configs/mod.ltx", b"[module]\nid = x\n"), ("gamedata/scripts/fine.script", b"function a() end\n")]


class Run:
    def __init__(self):
        self.passed, self.failed = 0, []

    def check(self, condition, label):
        if condition:
            self.passed += 1
        else:
            self.failed.append(label)
            print("FAIL: " + label)


def quiet(function, *args):
    with contextlib.redirect_stdout(io.StringIO()) as out:
        result = function(*args)
    return result, out.getvalue()


def judge(work, outputs):
    open(outputs, "w").close()
    quiet(catalog.cmd_judge, argparse.Namespace(repo=work, catalog=REPO, issue=None, outputs=outputs, summary=None))
    return re.search(r"^merge=(.*)$", open(outputs).read(), re.M).group(1).split()


def merge(work, pairs):
    quiet(catalog.cmd_merge, argparse.Namespace(repo=work, catalog=REPO, pairs=" ".join(pairs)))


def publish(work, key_file):
    """What publish.yml does after a listing: cards, the signature, the commit."""
    quiet(catalog.cmd_cards, argparse.Namespace(repo=work, issues=None))
    quiet(catalog.cmd_publish, argparse.Namespace(repo=work, key=key_file, password_env=None, no_cards=True))
    git(work, "add", "-A", "--", "public", "state", "holds.ltx")
    git(work, "-c", "user.name=catalog-bot", "-c", "user.email=b@b", "commit", "-q", "-m", "Publish the catalog")
    git(work, "push", "-q", "origin", "HEAD:main")
    body, _, _, _ = xc.split_signed(open(os.path.join(work, "public", "index.ltx"), "rb").read())
    return catalog.parse_ini(body.decode("utf-8"))


def verdict(number):
    comments = MOCK.bot_comments(number)
    body = comments[-1]["body"] if comments else ""
    return body[len(catalog.VERDICT_MARKER):].split(" -->")[0] if body else "", body


def pair_of(pairs, number):
    return next((p for p in pairs if p.startswith("%d:" % number)), None)


def main():
    run = Run()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    MOCK.base = "http://127.0.0.1:%d" % server.server_address[1]
    catalog.API, catalog.GITHUB, catalog.VT_API = MOCK.base + "/api", MOCK.base + "/web", MOCK.base + "/vt/api/v3/"
    catalog.VT_PAUSE, catalog.VT_POLL = 0, 0.01
    catalog.website_findings = lambda url: []
    os.environ.update({"GH_TOKEN": BOT_TOKEN, "GITHUB_REPOSITORY": REPO, "VT_API_KEY": VT_KEY,
                       "XMS_CATALOG_NOW": "2026-09-25T12:00:00Z"})
    MOCK.users.update({
        "alice": {"login": "alice", "id": 1, "created_at": "2019-04-01T00:00:00Z"},
        "bob": {"login": "bob", "id": 2, "created_at": "2026-09-20T08:00:00Z"},
        "mallory": {"login": "mallory", "id": 3, "created_at": "2018-01-01T00:00:00Z"},
        "carol": {"login": "carol", "id": 4, "created_at": "2018-01-01T00:00:00Z"}})
    alice, bob, carol, stranger, catalog_key = (xc.new_private() for _ in range(5))

    with tempfile.TemporaryDirectory() as root:
        # the catalog: one module listed since January, signed by the catalog key
        origin, work, outputs = os.path.join(root, "origin.git"), os.path.join(root, "work"), os.path.join(root, "out.txt")
        key_file, issues_md = os.path.join(root, "catalog.xkey"), os.path.join(root, "issues.md")
        xc.write_key_file(key_file, catalog_key)
        git(root, "init", "-q", "--bare", "-b", "main", origin)
        git(root, "init", "-q", "-b", "main", work)
        catalog.write_text(os.path.join(work, "catalog.ltx"),
                           "[virustotal]\ntrusted = Kaspersky, Microsoft\nblock_trusted = 1\nwarn_trusted = 1\n"
                           "block_others = 5\nwarn_others = 2\n\n[accept]\nmin_account_days = 14\n"
                           "max_mods_per_owner = 4\nmax_new_per_day = 2\nvt_wait_days = 3\ndormant_days = 30\n")
        catalog.write_text(os.path.join(work, "revoked.ltx"), "[revoked]\n")
        catalog.write_text(os.path.join(work, "holds.ltx"), catalog.HOLDS_HEADER)
        catalog.write_text(os.path.join(work, "mods", "old-mod.ltx"), submission("old-mod", "alice/old-mod", alice))
        release("alice/old-mod", "old-mod", "1.0.0", alice, GOOD_FILES, vt=CLEAN)
        index = ("[catalog]\nformat  = 1\nserial  = 7\nissued  = 2026-01-01\n\n[mods]\nold-mod = alice/old-mod, %s, "
                 "1.0.0, 2026-01-01\n\n[revoked]\n" % xc.key_text(xc.public_of(alice)))
        catalog.write_text(os.path.join(work, "public", "index.ltx"), xc.sign_text(index, catalog_key))
        git(work, "add", "-A")
        git(work, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "start")
        git(work, "remote", "add", "origin", origin)
        git(work, "push", "-q", "origin", "main")

        # ---- day one: every kind of submission at once ----
        release("alice/new-mod", "new-mod", "1.0.0", alice, GOOD_FILES, vt=CLEAN)
        release("bob/bob-mod", "bob-mod", "1.0.0", bob, GOOD_FILES)                      # VirusTotal never saw it
        release("alice/evil-mod", "evil-mod", "1.0.0", alice, GOOD_FILES + [("bin/evil.dll", b"MZ...")], vt=CLEAN)
        release("alice/script-mod", "script-mod", "1.0.0", alice,
                [("gamedata/scripts/x.script", b'os.execute("calc")\n')], vt=CLEAN)
        release("alice/vt-mod", "vt-mod", "1.0.0", alice, GOOD_FILES, vt=BLOCK)
        release("alice/warn-mod", "warn-mod", "1.0.0", alice,
                [("gamedata/scripts/w.script", b"local f = loadstring(code)\n")], vt=WARN)
        release("alice/third-mod", "third-mod", "1.0.0", alice, GOOD_FILES, vt=CLEAN)
        release("alice/other-mod", "other-mod", "1.0.0", alice, GOOD_FILES, vt=CLEAN)
        release("alice/unsigned-mod", "unsigned-mod", "1.0.0", stranger, GOOD_FILES, vt=CLEAN)
        release("carol/edit-mod", "edit-mod", "1.0.0", carol, GOOD_FILES, vt=CLEAN)

        n_new = MOCK.issue("alice", issue_body(submission("new-mod", "alice/new-mod", alice)))
        n_bob = MOCK.issue("bob", issue_body(submission("bob-mod", "bob/bob-mod", bob)))
        MOCK.comment(n_bob, "mallory", catalog.VERDICT_MARKER + "accepted -->\nAccepted: trust me.\n")
        n_theft = MOCK.issue("mallory", issue_body(submission("other-mod", "alice/other-mod", alice)))
        n_rekey = MOCK.issue("alice", issue_body(submission("old-mod", "alice/old-mod", stranger)))
        n_evil = MOCK.issue("alice", issue_body(submission("evil-mod", "alice/evil-mod", alice)))
        n_script = MOCK.issue("alice", issue_body(submission("script-mod", "alice/script-mod", alice)))
        n_vt = MOCK.issue("alice", issue_body(submission("vt-mod", "alice/vt-mod", alice)))
        n_unsigned = MOCK.issue("alice", issue_body(submission("unsigned-mod", "alice/unsigned-mod", alice)))
        n_warn = MOCK.issue("alice", issue_body(submission("warn-mod", "alice/warn-mod", alice)))
        n_third = MOCK.issue("alice", issue_body(submission("third-mod", "alice/third-mod", alice)))
        n_edit = MOCK.issue("carol", issue_body(submission("edit-mod", "carol/edit-mod", carol)))
        n_noblock = MOCK.issue("alice", catalog.SUBMISSION_MARKER + "\nno block here\n")
        n_bug = MOCK.issue("carol", "The game crashes when I open the browser.", "A bug")
        n_pr = MOCK.issue("carol", "Please merge", "A pull request", pull=True)
        n_own_pr = MOCK.issue("alice", "Maintenance", "Owner's pull request", pull=True, association="OWNER")

        merges = judge(work, outputs)
        state = {n: verdict(n)[0] for n in MOCK.issues}
        run.check(state[n_new] == "accepted", "a clean module from its owner is accepted")
        run.check(state[n_warn] == "accepted", "VirusTotal warn and a reported script do not stop a listing")
        warn_body = verdict(n_warn)[1]
        run.check("the game asks the player" in warn_body and "loadstring of a computed string" in warn_body,
                  "the warn verdict and the reported script are in the verdict")
        run.check(state[n_third] == "waiting" and "2 new modules were listed today" in verdict(n_third)[1] and
                  state[n_edit] == "waiting", "the day's quota of new modules makes the next ones wait")
        run.check(state[n_bob] == "refused" and "is 5 days old" in verdict(n_bob)[1] and "submit again from 2026-10-04" in
                  verdict(n_bob)[1], "a young account is refused and told from when to submit again")
        run.check(state[n_theft] == "refused" and "belongs to alice" in verdict(n_theft)[1],
                  "a submission for someone else's repository is refused")
        run.check(state[n_rekey] == "refused" and "listed with another author key" in verdict(n_rekey)[1],
                  "a key change of a listed module is refused")
        run.check(state[n_evil] == "refused" and "bin/evil.dll" in verdict(n_evil)[1], "a program in the release is refused")
        run.check(state[n_script] == "refused" and "os.execute" in verdict(n_script)[1], "a blocking script is refused")
        run.check(state[n_vt] == "refused" and "VirusTotal block" in verdict(n_vt)[1], "a VirusTotal block is refused")
        run.check(state[n_unsigned] == "refused" and "not signed by the submitted key" in verdict(n_unsigned)[1],
                  "a release another key signed is refused")
        run.check(state[n_noblock] == "refused", "a submission without its ini block is refused")
        run.check(state[n_bug] == "" and MOCK.issues[n_bug]["state"] == "open", "an issue that is no submission is left alone")
        run.check(state[n_pr] == "refused" and MOCK.issues[n_pr]["state"] == "closed", "a stranger's pull request is closed")
        run.check(state[n_own_pr] == "" and MOCK.issues[n_own_pr]["state"] == "open", "the owner's pull request is left alone")
        run.check(all(MOCK.issues[n]["state"] == "closed" and MOCK.issues[n]["state_reason"] == "not_planned"
                      for n in (n_bob, n_theft, n_rekey, n_evil, n_script, n_vt, n_unsigned, n_noblock)),
                  "refused submissions are closed")
        run.check(MOCK.issues[n_new]["state"] == "open", "an accepted submission stays open until it is committed")
        run.check(sorted(int(p.split(":")[0]) for p in merges) == [n_new, n_warn], "judge hands on exactly the accepted ones")
        lines = verdict(n_new)[1].splitlines()
        run.check(lines[0] == catalog.VERDICT_MARKER + "accepted -->" and lines[1].startswith("Accepted: the module is listed"),
                  "the verdict comment keeps its contract")
        run.check(len(MOCK.bot_comments(n_bob)) == 1, "the forged verdict does not count as the bot's")

        # half an hour later the same verdict edits the comment instead of adding one
        os.environ["XMS_CATALOG_NOW"] = "2026-09-25T12:30:00Z"
        merges = judge(work, outputs)
        third_comments = MOCK.bot_comments(n_third)
        run.check(len(third_comments) == 1 and third_comments[0]["edits"] >= 1, "an unchanged verdict edits the last comment")
        run.check(len(MOCK.bot_comments(n_bob)) == 1, "a closed submission is not judged again")
        run.check(not MOCK.vt_uploads, "nothing is uploaded for a refused submission")

        merge(work, merges)
        pushed = git(origin, "log", "--format=%an <%ae>|%s", "main")
        run.check("alice <1+alice@users.noreply.github.com>|List new-mod (#%d)" % n_new in pushed,
                  "the file is committed as the submitter's, with the issue number")
        run.check(git(origin, "show", "main:mods/new-mod.ltx") == submission("new-mod", "alice/new-mod", alice),
                  "the committed file is the judged text, byte for byte")
        run.check(MOCK.issues[n_new]["state"] == "closed" and MOCK.issues[n_new]["state_reason"] == "completed",
                  "an accepted submission is closed once it is committed")
        run.check(MOCK.dispatches == [("publish.yml", "main")], "merge starts publish.yml on the default branch")
        listed = dict(publish(work, key_file)["mods"])
        run.check(listed["new-mod"].endswith("2026-09-25") and listed["old-mod"].endswith("2026-01-01"),
                  "publish lists the committed modules and keeps each one's first date")

        # ---- day two: the quota is free again; edit-mod is accepted, then edited before its merge ----
        os.environ["XMS_CATALOG_NOW"] = "2026-09-26T12:00:00Z"
        merges = judge(work, outputs)
        edit_pair = pair_of(merges, n_edit)
        run.check(pair_of(merges, n_third) and edit_pair, "waiting submissions are accepted once the quota allows")
        run.check(len(MOCK.bot_comments(n_third)) == 2, "a changed verdict is a new comment")
        MOCK.issues[n_edit]["body"] = issue_body(submission("edit-mod", "carol/edit-mod", carol,
                                                            website="https://ap-pro.ru/stuff/mods/other-r2/"))
        merge(work, [edit_pair])
        run.check(MOCK.issues[n_edit]["state"] == "open" and len(MOCK.dispatches) == 1 and
                  "mods/edit-mod.ltx" not in git(origin, "ls-tree", "-r", "--name-only", "main"),
                  "a submission edited after its verdict is not committed")
        merge(work, [p for p in merges if p != edit_pair])
        publish(work, key_file)

        # the owner's limit: four modules, and alice has them now
        release("alice/fifth-mod", "fifth-mod", "1.0.0", alice, GOOD_FILES, vt=CLEAN)
        n_fifth = MOCK.issue("alice", issue_body(submission("fifth-mod", "alice/fifth-mod", alice)))
        judge(work, outputs)
        run.check(verdict(n_fifth)[0] == "refused" and "lists 4 modules already" in verdict(n_fifth)[1],
                  "one account lists at most max_mods_per_owner modules")

        # ---- bob's account comes of age and he submits again: VirusTotal gets the package, the run waits ----
        os.environ["XMS_CATALOG_NOW"] = "2026-10-05T12:00:00Z"
        n_bob = MOCK.issue("bob", issue_body(submission("bob-mod", "bob/bob-mod", bob)))
        bob_sha = sha(MOCK.assets[("bob/bob-mod", "bob-mod-1.0.0.zip")])
        merges = judge(work, outputs)
        run.check(bob_sha in MOCK.vt_uploads, "an unknown package is uploaded to VirusTotal")
        run.check(verdict(n_bob)[0] == "accepted", "its report is waited for in the same run")
        run.check(pair_of(merges, n_edit), "the edited submission is judged again, as edited")
        merge(work, merges)
        index = publish(work, key_file)
        listed = dict(index["mods"])
        run.check(listed["bob-mod"].endswith("2026-10-05") and listed["new-mod"].endswith("2026-09-25") and
                  catalog.first(index, "catalog", "serial") == "10", "every publication signs the next serial")
        index_path = os.path.join(work, "public", "index.ltx")
        signed = open(index_path, "rb").read()
        os.environ["XMS_CATALOG_NOW"] = "2026-10-05T18:00:00Z"
        quiet(catalog.cmd_publish, argparse.Namespace(repo=work, key=key_file, password_env=None, no_cards=True))
        run.check(open(index_path, "rb").read() == signed, "an index that would say the same is not signed again")
        os.environ["XMS_CATALOG_NOW"] = "2026-10-05T12:00:00Z"
        run.check(git(origin, "show", "main:mods/edit-mod.ltx") == submission("edit-mod", "carol/edit-mod", carol,
                                                                              website="https://ap-pro.ru/stuff/mods/other-r2/"),
                  "what is committed is what was judged last")

        # ---- VirusTotal withdraws a listed release, and gives it back ----
        new_sha = sha(MOCK.assets[("alice/new-mod", "new-mod-1.0.0.zip")])
        MOCK.vt[new_sha] = [BLOCK]
        quiet(catalog.cmd_vt, argparse.Namespace(repo=work))
        quiet(catalog.cmd_holds, argparse.Namespace(repo=work))
        holds = catalog.read_holds(os.path.join(work, "holds.ltx"))
        run.check(holds.get(("new-mod", "1.0.0"), ("", ""))[1] == "VirusTotal: Kaspersky",
                  "a live release VirusTotal blocks is withdrawn")
        quiet(catalog.cmd_publish, argparse.Namespace(repo=work, key=key_file, password_env=None, no_cards=True))
        body, _, _, _ = xc.split_signed(open(os.path.join(work, "public", "index.ltx"), "rb").read())
        run.check(("new-mod", "1.0.0, VirusTotal: Kaspersky") in catalog.parse_ini(body.decode("utf-8")).get("revoked", []),
                  "the withdrawal is signed into the index")
        quiet(catalog.cmd_cards, argparse.Namespace(repo=work, issues=issues_md))
        run.check("new-mod: release 1.0.0 is withdrawn" in catalog.read_text(issues_md),
                  "the withdrawn release is reported and gets no fresh card")

        # a new release nobody handed to VirusTotal is handed to it by the catalog, once
        release("bob/bob-mod", "bob-mod", "1.1.0", bob, GOOD_FILES + [("gamedata/configs/b.ltx", b"[b]\n")])
        bob_new = sha(MOCK.assets[("bob/bob-mod", "bob-mod-1.1.0.zip")])
        quiet(catalog.cmd_cards, argparse.Namespace(repo=work, issues=None))
        uploads = len(MOCK.vt_uploads)
        quiet(catalog.cmd_vt, argparse.Namespace(repo=work))
        run.check(MOCK.vt_uploads[uploads:] == [bob_new], "the catalog uploads an update VirusTotal does not know")
        MOCK.vt.pop(bob_new)
        quiet(catalog.cmd_vt, argparse.Namespace(repo=work))
        run.check(MOCK.vt_uploads[uploads:] == [bob_new], "and does not upload it again within a week")

        # VirusTotal changes its mind: the next day the hold is lifted
        os.environ["XMS_CATALOG_NOW"] = "2026-10-06T12:00:00Z"
        MOCK.vt[new_sha] = [CLEAN]
        quiet(catalog.cmd_vt, argparse.Namespace(repo=work))
        quiet(catalog.cmd_holds, argparse.Namespace(repo=work))
        run.check(("new-mod", "1.0.0") not in catalog.read_holds(os.path.join(work, "holds.ltx")),
                  "a hold is lifted when a later report no longer blocks")

        # ---- the status issue and a dormant module ----
        del MOCK.assets[("alice/third-mod", "third-mod.update.ltx")]
        quiet(catalog.cmd_cards, argparse.Namespace(repo=work, issues=issues_md))
        quiet(catalog.cmd_status, argparse.Namespace(repo=work, catalog=REPO, issues=issues_md))
        status = [i for i in MOCK.issues.values() if i["title"] == catalog.STATUS_TITLE]
        run.check(len(status) == 1 and status[0]["state"] == "open" and "third-mod" in status[0]["body"],
                  "one status issue names the failing module")
        quiet(catalog.cmd_status, argparse.Namespace(repo=work, catalog=REPO, issues=issues_md))
        run.check(len([i for i in MOCK.issues.values() if i["title"] == catalog.STATUS_TITLE]) == 1,
                  "the next run edits it instead of opening another")
        run.check("third-mod" in catalog.read_cards(os.path.join(work, "public", "cards.txt")),
                  "a failing module keeps its last card for a while")
        os.environ["XMS_CATALOG_NOW"] = "2026-11-06T12:00:00Z"
        quiet(catalog.cmd_cards, argparse.Namespace(repo=work, issues=issues_md))
        run.check("third-mod" not in catalog.read_cards(os.path.join(work, "public", "cards.txt")),
                  "after dormant_days it leaves the browser")
        run.check("third-mod" in catalog.listing(catalog.Repo(work)), "and stays listed, bound to its key")
        release("alice/third-mod", "third-mod", "1.0.1", alice, GOOD_FILES, vt=CLEAN)
        quiet(catalog.cmd_cards, argparse.Namespace(repo=work, issues=issues_md))
        quiet(catalog.cmd_status, argparse.Namespace(repo=work, catalog=REPO, issues=issues_md))
        run.check("third-mod" in catalog.read_cards(os.path.join(work, "public", "cards.txt")),
                  "a working release brings it back")
        run.check(status[0]["state"] == "closed", "the status issue closes when nothing fails")

        # a bounded number of submissions a run: a flood waits for the next runs
        catalog.JUDGED_PER_RUN = 2
        flood = [MOCK.issue("mallory", issue_body(submission("flood-%d" % i, "mallory/flood-%d" % i, stranger)))
                 for i in range(3)]
        judge(work, outputs)
        run.check([verdict(n)[0] for n in flood] == ["refused", "refused", ""], "a run judges a bounded number of submissions")
        judge(work, outputs)
        run.check(verdict(flood[2])[0] == "refused", "the next run takes the rest")
        catalog.JUDGED_PER_RUN = 30

    server.shutdown()
    print("test_accept: %d passed, %d failed" % (run.passed, len(run.failed)))
    return 1 if run.failed else 0


if __name__ == "__main__":
    sys.exit(main())
