#!/usr/bin/env python3

import argparse
import json
import re
from pathlib import Path


CLASS_RE = re.compile(r"^C\+\+ class ([^ :{]+)(?:\s*:\s*([^{]+))?\s*\{")
NAMESPACE_RE = re.compile(r"^\s*namespace\s*([^\s{]*)\s*\{")
FUNCTION_RE = re.compile(r"^\s*function\s+(.+);(?:\s*--.*)?$")
PROPERTY_RE = re.compile(r"^\s*property\s+(.+);(?:\s*--.*)?$")
CONSTRUCTOR_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\(.*\));(?:\s*--.*)?$")


def parse_help(path: Path) -> dict:
    classes: dict[str, dict] = {}
    namespaces: dict[str, set[str]] = {}
    current_class: str | None = None
    namespace_stack: list[tuple[int, str]] = []
    in_namespaces = False

    for raw_line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = raw_line.rstrip()

        if line == "List of the namespaces exported to LUA":
            in_namespaces = True
            current_class = None
            continue

        if not in_namespaces:
            match = CLASS_RE.match(line)
            if match:
                current_class = match.group(1)
                bases = [base.strip() for base in (match.group(2) or "").split(",") if base.strip()]
                classes[current_class] = {
                    "bases": bases,
                    "constructors": set(),
                    "functions": set(),
                    "properties": set(),
                }
                continue

            if current_class and line.strip() == "};":
                current_class = None
                continue

            if current_class:
                if match := FUNCTION_RE.match(line):
                    classes[current_class]["functions"].add(match.group(1).strip())
                elif match := PROPERTY_RE.match(line):
                    classes[current_class]["properties"].add(match.group(1).strip())
                elif match := CONSTRUCTOR_RE.match(line):
                    if match.group(1) == current_class:
                        classes[current_class]["constructors"].add(match.group(2).strip())
            continue

        if match := NAMESPACE_RE.match(line):
            indent = len(line) - len(line.lstrip())
            while namespace_stack and namespace_stack[-1][0] >= indent:
                namespace_stack.pop()
            name = match.group(1) or "<global>"
            namespace_stack.append((indent, name))
            key = "::".join(part for _, part in namespace_stack)
            namespaces.setdefault(key, set())
            continue

        if line.strip() == "};" and namespace_stack:
            namespace_stack.pop()
            continue

        if namespace_stack and (match := FUNCTION_RE.match(line)):
            key = "::".join(part for _, part in namespace_stack)
            namespaces.setdefault(key, set()).add(match.group(1).strip())

    return {
        "source": str(path.resolve()),
        "classes": {
            name: {key: sorted(value) for key, value in data.items()}
            for name, data in sorted(classes.items())
        },
        "namespaces": {name: sorted(functions) for name, functions in sorted(namespaces.items())},
    }


def flatten(inventory: dict) -> set[str]:
    entries: set[str] = set()
    for class_name, data in inventory["classes"].items():
        entries.update(f"class:{class_name}:base:{base}" for base in data["bases"])
        entries.update(f"class:{class_name}:constructor:{item}" for item in data["constructors"])
        entries.update(f"class:{class_name}:function:{item}" for item in data["functions"])
        entries.update(f"class:{class_name}:property:{item}" for item in data["properties"])
    for namespace, functions in inventory["namespaces"].items():
        entries.update(f"namespace:{namespace}:function:{item}" for item in functions)
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description="Inventory or compare X-Ray lua_help.script exports.")
    parser.add_argument("input", type=Path, help="Dead Air or OpenXRay lua_help.script")
    parser.add_argument("--compare", type=Path, help="Second lua_help.script to compare against the input")
    parser.add_argument("--output", type=Path, help="Write the inventory or diff as JSON")
    args = parser.parse_args()

    inventory = parse_help(args.input)
    result: dict = inventory

    if args.compare:
        other = parse_help(args.compare)
        left = flatten(inventory)
        right = flatten(other)
        result = {
            "reference": inventory["source"],
            "candidate": other["source"],
            "missing_from_candidate": sorted(left - right),
            "added_by_candidate": sorted(right - left),
        }

    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
