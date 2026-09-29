"""Check configs/dead_air_x64_ai.ltx, the data of the human NPC AI (docs/dead-air/ai/PLAN.md 2.3-2.4).

The engine never crashes on bad AI data: a key it cannot parse keeps the value of the next section of the chain and
logs one line, a missing section falls back to the default profile. This catches those mistakes before a run:

- the file parses the way CInifile does; no section is defined twice, no key twice within a section, every
  "[a]:b" parent is defined before its child;
- every "; [ai:<lane>] begin <name>" block is closed by its own end marker, blocks do not nest, and every section
  sits inside one block;
- [ai_profile_default] holds every profile key of 2.3.3; the profiles of 2.4 exist (the ten communities, the
  default bands and the character sections), and every [ai_profile_by_character] target exists;
- every key of an ai_profile_* section is a profile key and its value parses as that key's type (numbers, ranges,
  four band values, enums, roles, grenade_issue);
- the rank bands, weapon classes, class rules and armour tables are well formed.

Sections other lanes add for their own use are accepted as they are. Exit code 0 when clean, 1 otherwise.

    python tools/qa/ai/Check-AiLtx.py [path/to/dead_air_x64_ai.ltx]
"""

import re
import sys
from pathlib import Path

DEFAULT_PATH = Path(__file__).resolve().parents[3] / \
    "packaging/dead-air-x64/compatibility/gamedata/configs/dead_air_x64_ai.ltx"

# Profile keys of 2.3.3 by type (plus aggr_winning and winning_ratio of 2.4).
FLOAT = {"react_idle_ms", "settle_s", "detect_rate", "hear_skill", "hear_gain", "aim_head", "lead_error",
         "light_discipline", "radio_leader_bonus", "aggr", "winning_ratio", "cover", "flank", "supp",
         "supp_window_s", "gren", "courage", "break_cas", "break_ratio", "leader_dead", "surrender", "recover_s",
         "stamina", "search_delay_s", "search_s", "return_fire_m"}
OPTIONAL_FLOAT = {"peek", "aggr_winning"}  # empty = follows cover / aggr
RANGE = {"react_ms", "hq_delay_s", "decide_ms", "understand_ms", "giveup_stagger_s"}
BAND4 = {"radio_chance"}
BYTE = {"hq_relay", "gestures"}
BOOL = {"pda", "callouts", "bounding", "fire_on_order", "flank_fire", "fanatic", "quiet_search"}
ENUMS = {
    "voice": {"auto", "loud", "hushed", "none"},
    "rout_style": {"orderly", "scatter", "relocate", "never"},
    "search_style": {"pairs", "single", "straight", "none"},
    "investigate": {"pair", "single", "none", "advance"},
}
ROLES = {"opportunist", "suppressor", "flanker", "grenadier", "rearguard"}
SPECIAL = {"roles", "grenade_issue"}
PROFILE_KEYS = FLOAT | OPTIONAL_FLOAT | RANGE | BAND4 | BYTE | BOOL | set(ENUMS) | SPECIAL

COMMUNITIES = ["army", "dolg", "killer", "csky", "freedom", "stalker", "bandit", "monolith", "ecolog", "zombied"]
REQUIRED_PROFILES = ["ai_profile_default"] + [f"ai_profile_default_r{b}" for b in range(1, 5)] + \
    [f"ai_profile_{c}" for c in COMMUNITIES] + \
    ["ai_profile_ecolog_guard", "ai_profile_army_sniper", "ai_profile_monolith_sniper", "ai_profile_monolith_rg6"]

WEAPON_CLASSES = ["pistol", "smg", "shotgun", "rifle_old", "rifle", "mg", "dmr", "launcher"]
ARMOUR_CLASSES = ["none", "light", "heavy", "exo"]
RULE_LISTS = {"pistol_slots", "none_ammo", "launcher_ammo", "shotgun_ammo", "smg_ammo"}
RULE_NUMBERS = {"mg_mag", "dmr_zoom", "dmr_max_mag", "scoped_range_mult", "slug_engage_max"}
BAND_NAMES = ["novice", "trainee", "experienced", "professional", "veteran", "expert", "master", "legend"]

MARKER = re.compile(r"^;\s*\[ai:([a-z]+)\]\s+(begin|end)\s+(\S+)\s*$")
SECTION = re.compile(r"^\[([^\]]+)\]\s*(?::\s*(.*))?$")


class Checker:
    def __init__(self, path):
        self.path = path
        self.errors = []
        self.warnings = []
        self.sections = {}  # name -> {key: (value, line)}
        self.order = []
        self.block_of = {}  # section -> (lane, block)

    def error(self, line, text):
        self.errors.append(f"{self.path.name}:{line}: {text}" if line else f"{self.path.name}: {text}")

    def parse(self):
        current = None
        block = None
        for number, raw in enumerate(self.path.read_text(encoding="utf-8").splitlines(), 1):
            marker = MARKER.match(raw.strip())
            if marker:
                lane, kind, name = marker.groups()
                if kind == "begin":
                    if block:
                        self.error(number, f"block [ai:{lane}] {name} opens inside [ai:{block[0]}] {block[1]}")
                    block = (lane, name, number)
                else:
                    if not block or block[:2] != (lane, name):
                        self.error(number, f"end of [ai:{lane}] {name} without its begin")
                    block = None
                continue
            text = raw.split(";", 1)[0].strip()
            if not text:
                continue
            if len(raw) > 120:
                self.warnings.append(f"{self.path.name}:{number}: longer than 120 columns")
            header = SECTION.match(text)
            if header:
                name = header.group(1).strip().lower()
                if name in self.sections:
                    self.error(number, f"section [{name}] defined twice")
                items = {}
                for parent in (p.strip().lower() for p in (header.group(2) or "").split(",") if p.strip()):
                    if parent not in self.sections:
                        self.error(number, f"[{name}] inherits [{parent}], which is not defined before it")
                    else:
                        items.update(self.sections[parent])
                self.sections[name] = items
                self.order.append(name)
                current = name
                if not block:
                    self.error(number, f"section [{name}] outside any [ai:<lane>] block")
                else:
                    self.block_of[name] = block[:2]
                continue
            if current is None:
                self.error(number, f"line outside any section: {text}")
                continue
            key, _, value = text.partition("=")
            key = key.strip()
            if not key:
                self.error(number, f"line without a key: {text}")
                continue
            own = self.sections[current]
            if key in own and own[key][1] > 0 and own[key][2] == current:
                self.error(number, f"[{current}] {key} set twice (first on line {own[key][1]})")
            own[key] = (value.strip(), number, current)
        if block:
            self.error(block[2], f"block [ai:{block[0]}] {block[1]} is never closed")

    # --- value checks -------------------------------------------------------------------------------------------

    @staticmethod
    def number(text):
        try:
            value = float(text)
        except ValueError:
            return None
        return value if value == value and abs(value) != float("inf") else None

    def items(self, text):
        return [t.strip() for t in text.split(",")] if text.strip() else []

    def check_profile_value(self, section, key, value, line):
        where = f"[{section}] {key} = {value!r}"
        if key in FLOAT:
            if value and self.number(value) is None:
                self.error(line, f"{where}: not a number")
        elif key in OPTIONAL_FLOAT:
            if value and self.number(value) is None:
                self.error(line, f"{where}: not a number (empty follows another key)")
        elif key in RANGE:
            parts = self.items(value)
            numbers = [self.number(p) for p in parts]
            if value and (not 1 <= len(parts) <= 2 or None in numbers):
                self.error(line, f"{where}: expected 'lo, hi'")
            elif len(numbers) == 2 and numbers[1] < numbers[0]:
                self.error(line, f"{where}: hi below lo")
        elif key in BAND4:
            parts = self.items(value)
            numbers = [self.number(p) for p in parts]
            if not 1 <= len(parts) <= 4 or None in numbers:
                self.error(line, f"{where}: expected up to four numbers R1..R4")
            elif any(n < 0 or n > 1 for n in numbers):
                self.error(line, f"{where}: chances are 0..1")
        elif key in BYTE:
            number = self.number(value)
            if value and (number is None or not 0 <= number <= 255):
                self.error(line, f"{where}: expected 0..255")
        elif key in BOOL:
            if value and value not in {"0", "1", "true", "false", "on", "off", "yes", "no"}:
                self.error(line, f"{where}: expected 0 or 1")
        elif key in ENUMS:
            if value and value not in ENUMS[key]:
                self.error(line, f"{where}: expected one of {', '.join(sorted(ENUMS[key]))}")
        elif key == "roles":
            unknown = [r for r in self.items(value) if r not in ROLES]
            if unknown:
                self.error(line, f"{where}: unknown role(s) {', '.join(unknown)}")
        elif key == "grenade_issue":
            parts = self.items(value)
            if parts:
                if len(parts) > 3 or (len(parts) > 1 and self.number(parts[1]) is None) or \
                        (len(parts) > 2 and self.number(parts[2]) is None):
                    self.error(line, f"{where}: expected 'item, count, chance'")
                elif len(parts) > 2 and not 0 <= self.number(parts[2]) <= 1:
                    self.error(line, f"{where}: chance is 0..1")
        else:
            self.error(line, f"{where}: not a profile key (typo?)")

    def check(self):
        self.parse()
        s = self.sections

        default = s.get("ai_profile_default")
        if default is None:
            self.error(0, "no [ai_profile_default]")
        else:
            for key in sorted(PROFILE_KEYS - set(default)):
                self.error(0, f"[ai_profile_default] lacks the profile key {key}")

        for name in REQUIRED_PROFILES:
            if name not in s:
                self.error(0, f"profile [{name}] of plan 2.4 is missing")

        profiles = [n for n in self.order if n.startswith("ai_profile_") and n != "ai_profile_by_character"]
        for name in profiles:
            for key, (value, line, owner) in s[name].items():
                if owner == name:
                    self.check_profile_value(name, key, value, line)

        for key, (value, line, _) in s.get("ai_profile_by_character", {}).items():
            if value not in s:
                self.error(line, f"[ai_profile_by_character] {key} -> [{value}], which does not exist")
        for key, (value, line, _) in s.get("ai_community_alias", {}).items():
            if not value:
                self.error(line, f"[ai_community_alias] {key} has no target community")

        bands = s.get("ai_rank_bands", {})
        for name in BAND_NAMES:
            if name not in bands:
                self.warnings.append(f"[ai_rank_bands] has no {name} (band 2)")
        for key, (value, line, _) in bands.items():
            if value not in {"1", "2", "3", "4"}:
                self.error(line, f"[ai_rank_bands] {key} = {value!r}: bands are 1..4")
        for key, (value, line, _) in s.get("ai_rank_scale", {}).items():
            number = self.number(value)
            if number is None or number <= 0:
                self.error(line, f"[ai_rank_scale] {key} = {value!r}: expected a positive number")

        classes = s.get("ai_weapon_classes")
        if classes is None:
            self.error(0, "no [ai_weapon_classes]")
        else:
            for name in WEAPON_CLASSES:
                if name not in classes:
                    self.error(0, f"[ai_weapon_classes] lacks {name}")
            for key, (value, line, _) in classes.items():
                numbers = [self.number(p) for p in self.items(value)]
                if key not in WEAPON_CLASSES:
                    self.error(line, f"[ai_weapon_classes] {key}: unknown class")
                elif len(numbers) != 3 or None in numbers:
                    self.error(line, f"[ai_weapon_classes] {key}: expected 'effective, engage_max, min'")
                elif not numbers[2] <= numbers[0] <= numbers[1]:
                    self.error(line, f"[ai_weapon_classes] {key}: expected min <= effective <= engage_max")

        rules = s.get("ai_weapon_class_rules")
        if rules is None:
            self.error(0, "no [ai_weapon_class_rules]")
        else:
            for key, (value, line, _) in rules.items():
                if key in RULE_NUMBERS:
                    if self.number(value) is None:
                        self.error(line, f"[ai_weapon_class_rules] {key}: not a number")
                elif key == "pistol_slots":
                    if any(self.number(p) is None for p in self.items(value)):
                        self.error(line, f"[ai_weapon_class_rules] {key}: slots are numbers")
                elif key not in RULE_LISTS:
                    self.error(line, f"[ai_weapon_class_rules] {key}: unknown rule key")
        for key, (value, line, _) in s.get("ai_weapon_class_overrides", {}).items():
            if value not in WEAPON_CLASSES and value != "none":
                self.error(line, f"[ai_weapon_class_overrides] {key} = {value!r}: unknown class")

        thresholds = s.get("ai_armour_classes", {})
        values = [self.number(thresholds.get(n, ("x",))[0]) for n in ARMOUR_CLASSES[1:]]
        if None in values:
            self.error(0, "[ai_armour_classes] needs numeric light, heavy, exo")
        elif not values[0] <= values[1] <= values[2]:
            self.error(0, "[ai_armour_classes] thresholds must rise light <= heavy <= exo")
        for key, (value, line, _) in s.get("ai_armour_overrides", {}).items():
            if value not in ARMOUR_CLASSES:
                self.error(line, f"[ai_armour_overrides] {key} = {value!r}: unknown armour class")

        return profiles


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH
    if not path.is_file():
        print(f"Check-AiLtx: no file {path}")
        return 1
    checker = Checker(path)
    profiles = checker.check()
    for warning in checker.warnings:
        print(f"warning: {warning}")
    for error in checker.errors:
        print(f"error: {error}")
    if checker.errors:
        print(f"Check-AiLtx: {len(checker.errors)} error(s)")
        return 1
    print(f"Check-AiLtx: OK ({len(checker.sections)} sections, {len(profiles)} profiles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
