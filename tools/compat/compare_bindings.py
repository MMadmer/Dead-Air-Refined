#!/usr/bin/env python3

import argparse
import json
import re
from pathlib import Path


CLASS_RE = re.compile(r"^class\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+:\s+(.*))?$")
NAMESPACE_RE = re.compile(r"^namespace\s+([A-Za-z_][A-Za-z0-9_]*)$")


def callable_name(signature: str) -> str | None:
    head, separator, _ = signature.partition("(")
    words = head.strip().split()
    if not separator or not words:
        return None
    return words[-1]


def parse_candidate(path: Path) -> dict:
    classes: dict[str, dict[str, set[str]]] = {}
    namespaces: dict[str, set[str]] = {"<global>": set()}
    scope_kind = "global"
    scope_name = "<global>"
    waiting_for_brace = False

    for raw_line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("[") or line.startswith("const "):
            continue

        if match := CLASS_RE.match(line):
            scope_kind = "class"
            scope_name = match.group(1)
            bases = {
                base.strip().split()[-1]
                for base in (match.group(2) or "").split(",")
                if base.strip()
            }
            classes.setdefault(scope_name, {"bases": bases, "functions": set(), "properties": set()})
            waiting_for_brace = True
            continue

        if match := NAMESPACE_RE.match(line):
            scope_kind = "namespace"
            scope_name = match.group(1)
            namespaces.setdefault(scope_name, set())
            waiting_for_brace = True
            continue

        if waiting_for_brace and line == "{":
            waiting_for_brace = False
            continue

        if line == "}":
            scope_kind = "global"
            scope_name = "<global>"
            continue

        if scope_kind == "class" and "{" in line and "(" not in line:
            property_head = line.partition("{")[0].strip()
            classes[scope_name]["properties"].add(property_head.split()[-1])
            continue

        if scope_kind == "class" and line.startswith("operator string("):
            classes[scope_name]["functions"].add("__tostring")
            continue

        if name := callable_name(line.rstrip(";")):
            if scope_kind == "class":
                classes[scope_name]["functions"].add(name)
            else:
                namespaces.setdefault(scope_name, set()).add(name)

    return {"classes": classes, "namespaces": namespaces}


def parse_reference(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    classes = {}
    for class_name, values in data["classes"].items():
        functions = {
            name
            for signature in values["functions"] + values["constructors"]
            if (name := callable_name(signature))
        }
        classes[class_name] = {
            "bases": set(values["bases"]),
            "functions": functions,
            "properties": set(values["properties"]),
        }

    namespaces = {}
    for namespace, signatures in data["namespaces"].items():
        short_name = namespace.rsplit("::", 1)[-1]
        namespaces[short_name] = {
            name for signature in signatures if (name := callable_name(signature))
        }
    return {"classes": classes, "namespaces": namespaces}


def compare(reference: dict, candidate: dict) -> dict:
    result = {
        "missing_classes": sorted(reference["classes"].keys() - candidate["classes"].keys()),
        "missing_class_functions": {},
        "missing_class_properties": {},
        "missing_namespace_functions": {},
    }

    def inherited_members(class_name: str, key: str, visited: set[str] | None = None) -> set[str]:
        if visited is None:
            visited = set()
        if class_name in visited or class_name not in candidate["classes"]:
            return set()
        visited.add(class_name)
        data = candidate["classes"][class_name]
        members = set(data[key])
        for base in data["bases"]:
            members.update(inherited_members(base, key, visited))
        return members

    for class_name in sorted(reference["classes"].keys() & candidate["classes"].keys()):
        missing_functions = reference["classes"][class_name]["functions"] - inherited_members(class_name, "functions")
        missing_properties = reference["classes"][class_name]["properties"] - inherited_members(class_name, "properties")
        if missing_functions:
            result["missing_class_functions"][class_name] = sorted(missing_functions)
        if missing_properties:
            result["missing_class_properties"][class_name] = sorted(missing_properties)

    for namespace, functions in reference["namespaces"].items():
        missing = functions - candidate["namespaces"].get(namespace, set())
        if missing:
            result["missing_namespace_functions"][namespace] = sorted(missing)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare a Dead Air Lua API inventory with OpenXRay bindings.")
    parser.add_argument("reference", type=Path, help="JSON produced by lua_help_inventory.py")
    parser.add_argument("candidate", type=Path, help="OpenXRay ScriptBindings dump")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    result = compare(parse_reference(args.reference), parse_candidate(args.candidate))
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
