#!/usr/bin/env python3
"""Audit Lua script calls against the engine's exported bindings.

Finds calls whose native binding is absent and calls that resolve to a known
native stub before a third-party mod silently loses behavior at runtime.

The binding inventory comes from a `lua_help.script`-style dump produced by the
engine's own bindings exporter (the same input `lua_help_inventory.py` parses),
so the audited names are the actual Lua-visible names, never guessed C++ ones.

Only conservatively recognizable call forms are audited to keep false positives
near zero:

* `db.actor:method(...)` — checked against the `game_object` class (inherited
  methods resolved through base classes);
* `namespace.func(...)` where `namespace` is a name the engine really exports —
  checked against that namespace's function list. Unknown receivers are Lua
  modules and are ignored as dynamic.

Comments and string literals are stripped before scanning. Dynamic constructs
are never reported. Exit code is non-zero only for findings that are neither
allowlisted nor present in the supplied baseline.

Usage:
  python lua_call_audit.py --bindings lua_help.script --scripts <dir> [...]
  python lua_call_audit.py --selftest
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lua_help_inventory import parse_help  # noqa: E402

ACTOR_CALL_RE = re.compile(r"\bdb\.actor\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(")
NAMESPACE_CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_.:])([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\("
)
ACTOR_CLASS = "game_object"


def strip_lua_noise(source: str) -> str:
    """Blank out comments and string literals, preserving line numbers."""
    out: list[str] = []
    i, n = 0, len(source)
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if ch == "-" and nxt == "-":
            block = re.match(r"--\[(=*)\[", source[i:])
            if block:
                closer = "]" + block.group(1) + "]"
                end = source.find(closer, i)
                end = n if end < 0 else end + len(closer)
            else:
                end = source.find("\n", i)
                end = n if end < 0 else end
            out.append(re.sub(r"[^\n]", " ", source[i:end]))
            i = end
            continue
        if ch in "'\"":
            j = i + 1
            while j < n and source[j] != ch:
                j += 2 if source[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(ch + " " * (j - i - 2 & 0x7FFFFFFF) + (ch if j - i > 1 else ""))
            i = j
            continue
        if ch == "[" and re.match(r"\[(=*)\[", source[i:]):
            block = re.match(r"\[(=*)\[", source[i:])
            closer = "]" + block.group(1) + "]"
            end = source.find(closer, i)
            end = n if end < 0 else end + len(closer)
            out.append(re.sub(r"[^\n]", " ", source[i:end]))
            i = end
            continue
        out.append(ch)
        i += 1
    return "".join(out)


class Bindings:
    def __init__(self, inventory: dict):
        self.classes = inventory.get("classes", {})
        self.namespaces = inventory.get("namespaces", {})

    def class_methods(self, name: str) -> set[str]:
        methods: set[str] = set()
        pending, seen = [name], set()
        while pending:
            cls = pending.pop()
            if cls in seen:
                continue
            seen.add(cls)
            data = self.classes.get(cls)
            if not data:
                continue
            methods.update(data.get("functions", ()))
            methods.update(data.get("properties", ()))
            pending.extend(data.get("bases", ()))
        return methods

    def namespace_functions(self, name: str) -> set[str] | None:
        if name in self.namespaces:
            return set(self.namespaces[name])
        return None


def signature_name(signature: str) -> str:
    """`give_money(number)` -> `give_money`; bare names pass through unchanged."""
    head, separator, _ = signature.partition("(")
    words = head.strip().split()
    if not separator or not words:
        return signature.strip()
    return words[-1]


def load_inventory(path: Path) -> Bindings:
    if path.suffix == ".json":
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    else:
        data = parse_help(path)
    # parse_help keeps full signatures; the audit compares bare Lua-visible names
    classes = {
        name: {
            "bases": sorted(base.strip() for base in info.get("bases", ())),
            "functions": sorted({signature_name(s) for s in info.get("functions", ())}),
            "properties": sorted({signature_name(s) for s in info.get("properties", ())}),
        }
        for name, info in data.get("classes", {}).items()
    }
    namespaces = {
        name: sorted({signature_name(s) for s in funcs})
        for name, funcs in data.get("namespaces", {}).items()
    }
    return Bindings({"classes": classes, "namespaces": namespaces})


def audit_source(name: str, source: str, bindings: Bindings, stubs: set[str]) -> list[dict]:
    findings: list[dict] = []
    actor_methods = bindings.class_methods(ACTOR_CLASS)
    clean = strip_lua_noise(source)
    for line_no, line in enumerate(clean.splitlines(), start=1):
        for match in ACTOR_CALL_RE.finditer(line):
            method = match.group(1)
            qualified = f"{ACTOR_CLASS}:{method}"
            if qualified in stubs or method in stubs:
                findings.append({"script": name, "line": line_no, "kind": "stub", "name": qualified})
            elif method not in actor_methods:
                findings.append({"script": name, "line": line_no, "kind": "missing", "name": qualified})
        for match in NAMESPACE_CALL_RE.finditer(line):
            namespace, func = match.group(1), match.group(2)
            if namespace == "db":
                continue
            exported = bindings.namespace_functions(namespace)
            if exported is None:
                continue  # a Lua module, not an engine namespace: dynamic, not audited
            qualified = f"{namespace}.{func}"
            if qualified in stubs:
                findings.append({"script": name, "line": line_no, "kind": "stub", "name": qualified})
            elif func not in exported:
                findings.append({"script": name, "line": line_no, "kind": "missing", "name": qualified})
    return findings


def finding_key(finding: dict) -> tuple:
    return (finding["script"], finding["kind"], finding["name"])


def run_audit(args: argparse.Namespace) -> int:
    bindings = load_inventory(Path(args.bindings))

    stubs: set[str] = set()
    if args.stubs:
        stubs = {
            line.strip()
            for line in Path(args.stubs).read_text(encoding="utf-8-sig").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }

    allowlist: dict[str, str] = {}
    if args.allowlist:
        for entry in json.loads(Path(args.allowlist).read_text(encoding="utf-8-sig")):
            allowlist[entry["name"]] = entry.get("reason", "")

    baseline_keys: set[tuple] = set()
    if args.baseline and Path(args.baseline).is_file():
        previous = json.loads(Path(args.baseline).read_text(encoding="utf-8-sig"))
        baseline_keys = {finding_key(f) for f in previous.get("findings", [])}

    findings: list[dict] = []
    scanned = 0
    for root in args.scripts:
        root_path = Path(root)
        for script in sorted(root_path.rglob("*.script")) + sorted(root_path.rglob("*.lua")):
            scanned += 1
            rel = script.relative_to(root_path).as_posix()
            source = script.read_text(encoding="utf-8-sig", errors="replace")
            findings.extend(audit_source(rel, source, bindings, stubs))

    findings.sort(key=lambda f: (f["script"], f["line"], f["name"]))
    new_findings = []
    for finding in findings:
        if finding["name"] in allowlist:
            finding["kind"] = "allowlisted"
            finding["reason"] = allowlist[finding["name"]]
        elif finding_key(finding) not in baseline_keys:
            new_findings.append(finding)

    report = {
        "scripts_scanned": scanned,
        "counts": {
            kind: sum(1 for f in findings if f["kind"] == kind)
            for kind in ("missing", "stub", "allowlisted")
        },
        "findings": findings,
    }
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    for finding in findings:
        reason = f" ({finding['reason']})" if finding.get("reason") else ""
        print(f"{finding['kind']:>11}: {finding['script']}:{finding['line']} {finding['name']}{reason}")
    print(
        f"scanned {scanned} scripts: {report['counts']['missing']} missing, "
        f"{report['counts']['stub']} stub, {report['counts']['allowlisted']} allowlisted, "
        f"{len(new_findings)} new vs baseline"
    )
    return 1 if new_findings else 0


FIXTURE_HELP = """\
C++ class game_object {
    function give_money(number);
    function character_community();
};
C++ class CScriptActor : game_object {
    function actor_special();
};
List of the namespaces exported to LUA
namespace level {
    function name();
    function present();
};
"""

FIXTURE_SCRIPT = """\
local money = db.actor:give_money(100)      -- bound: direct method
local com = db.actor:character_community()  -- bound
db.actor:perk_unlock(1)                     -- missing: no such binding
-- db.actor:commented_out(1)
local text = "db.actor:in_string(1)"
level.name()                                -- bound namespace function
level.jump_to_level("zaton")                -- missing namespace function
mymodule.helper(1)                          -- unknown receiver: ignored
local t = {}
t.field(1)                                  -- unknown receiver: ignored
db.actor:stubbed_call()                     -- stub via marker list
"""


def selftest() -> int:
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "help.script").write_text(FIXTURE_HELP, encoding="utf-8")
        bindings = load_inventory(tmp_path / "help.script")
        findings = audit_source(
            "fixture.script", FIXTURE_SCRIPT, bindings, stubs={"game_object:stubbed_call"}
        )

    got = {(f["kind"], f["name"], f["line"]) for f in findings}
    expected = {
        ("missing", "game_object:perk_unlock", 3),
        ("missing", "level.jump_to_level", 7),
        ("stub", "game_object:stubbed_call", 11),
    }
    if got != expected:
        print("selftest FAILED")
        print("  expected:", sorted(expected))
        print("  got:     ", sorted(got))
        return 1
    print("selftest OK: fixtures classified correctly")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bindings", help="lua_help.script dump or inventory JSON")
    parser.add_argument("--scripts", action="append", default=[],
                        help="script corpus root (repeatable, e.g. an extracted addon tree)")
    parser.add_argument("--stubs", help="optional text file of Lua-visible names known to be native stubs")
    parser.add_argument("--allowlist", help="JSON list of {name, reason} reviewed exceptions")
    parser.add_argument("--baseline", help="previous JSON report; only new findings affect the exit code")
    parser.add_argument("--json", help="write machine-readable report to this path")
    parser.add_argument("--selftest", action="store_true", help="run built-in parser fixtures and exit")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.bindings or not args.scripts:
        parser.error("--bindings and at least one --scripts are required (or use --selftest)")
    return run_audit(args)


if __name__ == "__main__":
    raise SystemExit(main())
