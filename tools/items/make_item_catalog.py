"""Generates the item catalog, gamedata configs\\item_catalog.ltx (the game's overlay).

The catalog says, for every config key the engine and the game's scripts read of an item's
section, its HUD section, its upgrade sections and the sections those name: its type, the item
families it belongs to, whether the game stops without it, the code's default, and - where a
person wrote one - a title, a group, a unit and a sentence of help. The game itself reads none
of it; it is for tools and for people.

Inputs, all in this folder:
    item_keys.tsv            every key the code reads (extracted from the engine and the scripts)
    item_classes.tsv         the item classes (`class =` values) and their family
    item_catalog_notes.ltx   what no extraction knows: families, groups, titles, help, units,
                             which keys are common, and corrections of the extraction

    python tools\\items\\make_item_catalog.py              write the catalog
    python tools\\items\\make_item_catalog.py --check      check the inputs and that the catalog
                                                         on disk is what they make
    python tools\\items\\make_item_catalog.py --check --game <configs folder>
                                                         also: every `like` section exists there

Regenerating reproduces the file byte for byte: the output depends on the inputs alone (sorted,
LF line ends, ASCII). Do not edit the catalog by hand; edit the notes and run this again.
"""
import argparse
import fnmatch
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(REPO, "packaging", "dead-air-x64", "compatibility", "gamedata", "configs", "item_catalog.ltx")

TYPES = {"bool", "int", "float", "string", "vec2", "vec3", "vec4", "color", "list", "section", "visual", "texture",
         "sound", "particles", "motion", "string_id", "functor", "token"}
UNITS = {"RU", "kg", "m", "m/s", "s", "ms", "deg", "rpm", "%", "x"}
KEY_FIELDS = ("families", "type", "group", "title", "unit", "required", "default", "values", "common", "presence",
              "section", "pattern", "help")
NOTE_FIELDS = {"families", "type", "group", "title", "unit", "required", "default", "values", "common", "help"}


class Problem(Exception):
    pass


# ---------------------------------------------------------------------------- readers
def read_tsv(path):
    rows, header = [], None
    with open(path, encoding="ascii") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cells = line.split("\t")
            if header is None:
                header = cells
                continue
            if len(cells) != len(header):
                raise Problem("%s:%d: %d cells, the header has %d" % (os.path.basename(path), n, len(cells), len(header)))
            rows.append(dict(zip(header, cells)))
    return rows


def read_ltx(path):
    """The notes: ordered sections of ordered key = value lines; `;` starts a comment."""
    sections, order, cur = {}, [], None
    with open(path, encoding="ascii") as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            if ";" in line:
                raise Problem("notes:%d: a `;` inside a line - the game's reader would cut the value there" % n)
            m = re.match(r"^\[([^\]]+)\]$", line)
            if m:
                cur = m.group(1).strip()
                if cur in sections:
                    raise Problem("notes:%d: section [%s] twice" % (n, cur))
                sections[cur] = {}
                order.append(cur)
                continue
            if cur is None:
                raise Problem("notes:%d: a line before any section" % n)
            if "=" not in line:
                raise Problem("notes:%d: no `=` in [%s]: %s" % (n, cur, line))
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip()
            if k in sections[cur]:
                raise Problem("notes:%d: [%s] %s twice" % (n, cur, k))
            sections[cur][k] = v
    return sections, order


def csv(v):
    return [x.strip() for x in v.split(",") if x.strip()]


# ---------------------------------------------------------------------------- the model
def entry_name(key):
    """The name a key goes by in the catalog: a series like use<N>_functor becomes use_n_functor."""
    name = re.sub(r"<([^>]*)>", lambda m: "_" + re.sub(r"[^a-z0-9]+", "_", m.group(1).lower()) + "_", key)
    return re.sub(r"_+", "_", name).strip("_")


def title_of(name):
    words = [w for w in name.split("_") if w]
    return " ".join(w if w.isupper() or not w.isalpha() else w.capitalize() for w in words)


def build(notes, order, keys, classes):
    problems = []
    cat = notes.get("catalog", {})
    scopes = csv(notes.get("scopes", {}).get("order", ""))
    families = csv(notes.get("families", {}).get("order", ""))
    groups = csv(notes.get("groups", {}).get("order", ""))
    if not scopes or not families or not groups:
        raise Problem("notes: [scopes], [families] and [groups] each need an order")

    # families and their classes
    fam_classes = {f: [] for f in families}
    for c in classes:
        if c["family"] not in fam_classes:
            problems.append("class %s: family %s is not in [families] order" % (c["class"], c["family"]))
            continue
        fam_classes[c["family"]].append(c["class"])
    seen_class = {}
    for f, cl in fam_classes.items():
        for c in cl:
            if c in seen_class:
                problems.append("class %s is in two families: %s and %s" % (c, seen_class[c], f))
            seen_class[c] = f

    # group globs, in [groups] order
    group_match = []
    for g in groups:
        sec = notes.get("group." + g)
        if sec is None:
            problems.append("group %s has no [group.%s]" % (g, g))
            continue
        group_match.append((g, csv(sec.get("match", ""))))

    entries = {}
    for r in keys:
        scope, key = r["scope"], r["key"]
        if scope not in scopes:
            problems.append("key %s: scope %s is not in [scopes] order" % (key, scope))
            continue
        name = scope + "." + entry_name(key)
        if name in entries:
            problems.append("two keys make [%s]" % name)
            continue
        e = {"scope": scope, "key": key, "name": entry_name(key), "families": r["families"], "type": r["type"],
             "required": {"1": True, "0": False}.get(r["required"], r["required"]), "default": r["default"], "presence": r["presence"] == "1",
             "section": r["section"], "pattern": key if r["pattern"] == "1" else "", "noted": False}
        entries[name] = e

    # the notes over the extraction
    special = {"catalog", "scopes", "families", "groups"}
    for sec in order:
        if sec in special or sec.startswith(("scope.", "family.", "group.")):
            continue
        e = entries.get(sec)
        if e is None:
            problems.append("notes: [%s] is no key of the extraction" % sec)
            continue
        e["noted"] = True
        for k, v in notes[sec].items():
            if k not in NOTE_FIELDS:
                problems.append("notes: [%s] %s is no field of a key" % (sec, k))
            elif k == "required":
                e["required"] = True if v == "true" else (False if v == "false" else v)
            elif k == "common":
                e["common"] = v == "true"
            else:
                e[k] = v
                if k == "title":
                    e["titled"] = True

    # what the notes leave out
    for name, e in entries.items():
        if not e.get("group"):
            e["group"] = "other"
            for g, globs in group_match:
                if any(fnmatch.fnmatchcase(e["key"], p) for p in globs):
                    e["group"] = g
                    break
        if not e.get("title"):
            e["title"] = title_of(e["name"])

    # checks of every record
    for name, e in entries.items():
        where = "[%s]" % name
        if e["type"] not in TYPES:
            problems.append("%s: type %s is not one of the catalog's" % (where, e["type"]))
        if e["type"] == "token" and not e.get("values"):
            problems.append("%s: a token without values" % where)
        if not e["families"]:
            problems.append("%s: no families" % where)
        elif e["families"] != "*":
            for f in csv(e["families"]):
                if f not in fam_classes:
                    problems.append("%s: family %s is not in [families] order" % (where, f))
        if isinstance(e["required"], str):
            for c in csv(e["required"]):
                if c not in seen_class:
                    problems.append("%s: required names %s, which is no item class" % (where, c))
        if e["group"] not in groups:
            problems.append("%s: group %s is not in [groups] order" % (where, e["group"]))
        if e.get("unit") and e["unit"] not in UNITS:
            problems.append("%s: unit %s is not one of the catalog's" % (where, e["unit"]))
        if e["scope"] == "global" and not e["section"]:
            problems.append("%s: a global key without its section" % where)
    for sec in [s for s in order if s.startswith(("scope.", "family.", "group."))]:
        kind, _, ident = sec.partition(".")
        known = {"scope": scopes, "family": families, "group": groups}[kind]
        if ident not in known:
            problems.append("notes: [%s] is not in [%ss] order" % (sec, kind if kind != "family" else "familie"))
    return {"catalog": cat, "scopes": scopes, "families": families, "groups": groups, "fam_classes": fam_classes,
            "entries": entries}, problems


# ---------------------------------------------------------------------------- the writer
def render(model, notes):
    out = []
    w = out.append
    w("; The item catalog: every config key the game reads of an item, its HUD section, its upgrade")
    w("; sections and the sections those name - type, families, whether the game stops without it,")
    w("; the code's default, and for the keys an author works with a title, a group, a unit and help.")
    w("; The game reads none of it. Generated by tools\\items\\make_item_catalog.py: do not edit.")
    w("")
    w("[catalog]")
    w("version = %s" % model["catalog"].get("version", "1"))
    w("")
    w("[scopes]")
    w("order = " + ", ".join(model["scopes"]))
    for s in model["scopes"]:
        sec = notes.get("scope." + s, {})
        w("")
        w("[scope.%s]" % s)
        for k in ("title", "help"):
            if sec.get(k):
                w("%s = %s" % (k, sec[k]))
    w("")
    w("[families]")
    w("order = " + ", ".join(model["families"]))
    for f in model["families"]:
        sec = notes.get("family." + f, {})
        w("")
        w("[family.%s]" % f)
        w("title = %s" % sec.get("title", title_of(f)))
        if model["fam_classes"][f]:
            w("classes = " + ", ".join(sorted(model["fam_classes"][f])))
        if sec.get("like"):
            w("like = " + ", ".join(csv(sec["like"])))
        if sec.get("help"):
            w("help = %s" % sec["help"])
    w("")
    w("[groups]")
    w("order = " + ", ".join(model["groups"]))
    for g in model["groups"]:
        sec = notes.get("group." + g, {})
        w("")
        w("[group.%s]" % g)
        w("title = %s" % sec.get("title", title_of(g)))
    scope_rank = {s: i for i, s in enumerate(model["scopes"])}
    for name in sorted(model["entries"], key=lambda n: (scope_rank[model["entries"][n]["scope"]], model["entries"][n]["name"])):
        e = model["entries"][name]
        w("")
        w("[%s]" % name)
        for k in KEY_FIELDS:
            if k == "families":
                v = "*" if e["families"] == "*" else ", ".join(csv(e["families"]))
            elif k == "required":
                v = ", ".join(csv(e["required"])) if isinstance(e["required"], str) else ("true" if e["required"] else "")
            elif k == "common":
                v = "true" if e.get("common") else ""
            elif k == "presence":
                v = "true" if e["presence"] else ""
            elif k == "values":
                v = ", ".join(csv(e.get("values", "")))
            else:
                v = e.get(k, "")
            if v != "":
                w("%s = %s" % (k, v))
    return ("\n".join(out) + "\n").encode("ascii")


def check_text(data):
    """The game's LTX rules on the result: no `;` inside a value, no `]` in a section name, ASCII."""
    problems = []
    for n, line in enumerate(data.decode("ascii").split("\n"), 1):
        if line.startswith(";") or not line:
            continue
        if line.startswith("["):
            if not re.match(r"^\[[^\]\[]+\]$", line):
                problems.append("catalog:%d: a bad section line: %s" % (n, line))
            continue
        k, sep, v = line.partition(" = ")
        if not sep:
            problems.append("catalog:%d: no ` = `: %s" % (n, line))
        if ";" in v or '"' in v or any(ord(c) < 32 or ord(c) > 126 for c in line):
            problems.append("catalog:%d: a value the game's reader would cut or misread: %s" % (n, line))
    return problems


def game_sections(folder):
    """Every section of the configs under a folder, with its parents and its own `class` line."""
    found = {}
    for dp, _, files in sorted(os.walk(folder)):
        for fn in sorted(files):
            if not fn.lower().endswith(".ltx"):
                continue
            cur = None
            with open(os.path.join(dp, fn), encoding="cp1251", errors="replace") as f:
                for line in f:
                    line = line.split(";", 1)[0].strip()
                    m = re.match(r"^\[([^\]]+)\]\s*(?::\s*(.*))?$", line)
                    if m:
                        cur = found.setdefault(m.group(1).strip().lower(), {"parents": [], "class": None})
                        if m.group(2):
                            cur["parents"] = [p.strip().lower() for p in m.group(2).split(",") if p.strip()]
                    elif cur is not None and re.match(r"^class\s*=", line):
                        cur["class"] = line.split("=", 1)[1].strip()
    return found


def class_of(sections, name, depth=0):
    s = sections.get(name)
    if s is None or depth > 32:
        return None
    if s["class"]:
        return s["class"]
    for p in reversed(s["parents"]):
        c = class_of(sections, p, depth + 1)
        if c:
            return c
    return None


def counts(model):
    entries = model["entries"].values()
    lines = ["%d keys: %d with a written title, %d with help, %d common, %d required" % (
        len(model["entries"]), sum(1 for e in entries if e.get("titled")),
        sum(1 for e in entries if e.get("help")), sum(1 for e in entries if e.get("common")),
        sum(1 for e in entries if e["required"]))]
    for s in model["scopes"]:
        lines.append("  scope %-12s %d" % (s, sum(1 for e in entries if e["scope"] == s)))
    lines.append("required item keys per family (the game stops without them, after inheritance):")
    for f in model["families"]:
        mine = set(model["fam_classes"][f])
        req = [e for e in entries if e["scope"] == "item" and (e["families"] == "*" or f in csv(e["families"]))
               and (e["required"] is True or (isinstance(e["required"], str) and mine & set(csv(e["required"]))))]
        com = [e for e in entries if e.get("common") and (e["families"] == "*" or f in csv(e["families"]))]
        lines.append("  %-17s %3d required, %3d common" % (f, len(req), len(com)))
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(description="Generate the item catalog.")
    ap.add_argument("--check", action="store_true", help="check the inputs and the catalog on disk")
    ap.add_argument("--game", help="the game's configs folder: every family's `like` section must exist there")
    ap.add_argument("--out", default=OUT, help="where the catalog goes")
    a = ap.parse_args(argv)
    try:
        notes, order = read_ltx(os.path.join(HERE, "item_catalog_notes.ltx"))
        keys = read_tsv(os.path.join(HERE, "item_keys.tsv"))
        classes = read_tsv(os.path.join(HERE, "item_classes.tsv"))
        model, problems = build(notes, order, keys, classes)
    except Problem as p:
        print("error: %s" % p)
        return 1
    data = render(model, notes)
    problems += check_text(data)
    if a.game:
        have = game_sections(a.game)
        for f in model["families"]:
            for s in csv(notes.get("family." + f, {}).get("like", "")):
                if s.lower() not in have:
                    problems.append("family %s: like %s is no section of %s" % (f, s, a.game))
                elif class_of(have, s.lower()) not in model["fam_classes"][f]:
                    problems.append("family %s: like %s is of class %s, not of this family" % (f, s, class_of(have, s.lower())))
    if a.check:
        if os.path.exists(a.out):
            with open(a.out, "rb") as f:
                disk = f.read().replace(b"\r\n", b"\n")
            if disk != data:
                problems.append("%s is not what the inputs make: run make_item_catalog.py" % a.out)
        else:
            problems.append("%s does not exist" % a.out)
    for p in problems:
        print("error: %s" % p)
    if problems:
        return 1
    if not a.check:
        with open(a.out, "wb") as f:
            f.write(data)
        print("wrote %s" % a.out)
    print(counts(model))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
