"""Summarise the stand runs of the human NPC AI into the verdict table of docs/dead-air/ai/PLAN.md 7.5.

Reads the log of every run given (a run folder of tools/qa/Run-WaterQa.ps1 with its log.txt, or a log file): the
engine's counter lines "[ai] t=<s> dt=<s> frames=<n> key=value ..." and "[ai] c=<community> key=value ..." (printed
every ai_log_period and reset each time, so counts are summed over the periods) and the probes' lines
"[qa-ai] t=<s> <event> key=value ..." of tools/qa/water/ai_*.lua. A run is recognised by its probe's start line:

    ai_fair      R1   hive mind, voice / radio / HQ reach, hearing, night
    ai_factions  R2   three faction fights, the performance pass
    ai_actor     R3   the actor as the target, casualties and leader death
    ai_save a/b  R4a / R4b   wall, suppression, search; beliefs and radios across a save
    ai_newgame   R6   a new game: the start level is reached and every online stalker resolves a profile
    anything else     the checks every run gets (P1, P5, P9, P11)

Each criterion gets PASS, FAIL, VOID (the probe reported a broken layout), NOT MEASURED (the run holds no data for
it: never "passed") or INFO (a measured value the plan tunes by data). Numbers the plan takes from the AI data
(react_ms, hq_delay_s, weapon bands, rank bands) are read from configs/dead_air_x64_ai.ltx. Exit code 1 when a
criterion fails, else 0.

    python tools/qa/ai/Parse-AiLog.py <run folder or log> [...] [--ltx path] [--out verdict.md]
"""

import argparse
import copy
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_LTX = Path(__file__).resolve().parents[3] / \
    "packaging/dead-air-x64/compatibility/gamedata/configs/dead_air_x64_ai.ltx"

# Counter kinds of src/xrGame/da_ai.h (DA_AI_STATS): how the per-period values add up.
KINDS = {
    "npcs": "gauge", "squads": "gauge", "profile_default": "count", "rays": "rate", "rays_deferred": "rate",
    "p_total_us": "rate", "p_max_us": "max",
    "s_ident_ms": "avg", "s_heard_err": "avg", "c_lat_voice_ms": "avg", "c_lat_radio_ms": "avg",
    "c_lat_min_ms": "min", "c_rep_err": "avg", "t_react_ms": "avg", "t_react_due_ms": "avg", "t_react_base_ms": "avg",
    "t_open_m": "sum", "r_giveup_spread_ms": "avg",
}
for _lane_timer in ("p_know_us", "p_sense_us", "p_comms_us", "p_tactics_us", "p_morale_us", "p_search_us",
                    "p_know_cand_us"):
    KINDS[_lane_timer] = "rate"
# The worst of each part of one NPC update (da_ai.cpp): what p_max_us is made of.
MAX_PARTS = ("p_max_pre_us", "p_max_know_us", "p_max_comms_us", "p_max_morale_us", "p_max_tactics_us",
             "p_max_search_us", "p_max_squad_us")
# Inside those parts: the comms part's sending and taking in (its sound starts, spot descriptions, one delivery), the
# tactics part's flank route search, the squad step of each lane.
MAX_SUBPARTS = ("p_max_comms_tx_us", "p_max_comms_rx_us", "p_max_flank_us", "p_max_comms_snd_us", "p_max_comms_desc_us",
                "p_max_comms_msg_us", "p_max_sq_comms_us", "p_max_sq_tactics_us", "p_max_sq_morale_us",
                "p_max_sq_search_us")
for _part in MAX_PARTS + MAX_SUBPARTS:
    KINDS[_part] = "max"

# 6.3: average think time per frame, ms.
LANE_BUDGET_MS = {"p_know_us": 0.15, "p_sense_us": 0.15, "p_comms_us": 0.1, "p_tactics_us": 0.1,
                  "p_morale_us": 0.1, "p_search_us": 0.1}
TOTAL_BUDGET_MS, SINGLE_NPC_BUDGET_US = 0.5, 400
# R2: a layout that broke after this many seconds of the probe clock is judged on the data before the break.
R2_MIN_JUDGED_S = 45
# B4: a side's contact share needs this many samples (0.25 s apart), else its fight share stands.
CONTACT_MIN = 20

# 7.5 B1: identification in danger state by band, s (+-40 %); calm state x2.5.
IDENT_BANDS = {1: (1.0, 1.8), 2: (0.7, 1.2), 3: (0.45, 0.8), 4: (0.3, 0.6)}
IDENT_TOLERANCE, CALM_FACTOR = 0.4, 2.5
LATENCY_FLOOR_MS = 250
WALL_STALE_MS = 750          # P2: one worst-case schedule period + margin
AWAY_PLACED_M = 5            # B7: T farther than this from the away spot a second after the move never got there
VOICE_REACH_M = 60           # B8, also the earshot of a death cry in P7
PDA_MIN_S = 8                # P3: decide + typing + delay
RADIO_WINDOW_S = (1.6, 5.5)  # P4: sum of the profile minima .. maxima + two schedule periods
HQ_DELAY_DEFAULT_S = 30

QA = re.compile(r"\[qa-ai\] t=(-?[\d.]+) (\S+)(.*)$")
AI = re.compile(r"\[ai\] (?:(t=[\d.]+ dt=[\d.]+ frames=\d+)|c=(\S+))(.*)$")
KV = re.compile(r"([A-Za-z_][\w]*)=(\S+)")
# The engine's log file writes at most 1023 characters of a line (IWriter::VPrintf) and then no line break: a longer
# counter line loses its tail and the next line follows on the same line (r2l: "p_tactics_us=2903.9[ai] c=stalker ...",
# the community's values then overwrote the period's global ones). Lines are split again where a counter or script
# line starts.
LINE_START = re.compile(r"(?=\[ai\] (?:t=|c=))|(?=\[LUA\])")
ERRORS = re.compile(r"FATAL|Assertion|access violation|stack trace|LUA error|SCRIPT RUNTIME ERROR|SCRIPT ERROR"
                    r"|^! \[ai\]|! \[dead_air_x64_ai\]", re.IGNORECASE)


def number(value, default=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


# ------------------------------------------------------------------------------------------------------------------
# AI data
# ------------------------------------------------------------------------------------------------------------------
class Ltx:
    def __init__(self, path):
        self.sections = {}
        if not path or not Path(path).is_file():
            return
        current = None
        for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.split(";", 1)[0].strip()
            head = re.match(r"^\[([^\]]+)\]", line)
            if head:
                current = self.sections.setdefault(head.group(1).strip(), {})
            elif current is not None and "=" in line:
                key, value = line.split("=", 1)
                current.setdefault(key.strip(), value.strip())

    def get(self, section, key):
        return self.sections.get(section, {}).get(key)

    def numbers(self, section, key):
        return [n for n in (number(v.strip()) for v in (self.get(section, key) or "").split(",")) if n is not None]

    def profile_range(self, community, key):
        """Every number of the key over the community's profile sections (base and bands)."""
        values = []
        for section in [f"ai_profile_{community}"] + [f"ai_profile_{community}_r{b}" for b in range(1, 5)]:
            values += self.numbers(section, key)
        return (min(values), max(values)) if values else None

    def engage_max(self, weapon_class, ammo=None):
        band = self.numbers("ai_weapon_classes", weapon_class)
        limit = band[1] if len(band) >= 2 else None
        # A shotgun loaded with slugs reaches slug_engage_max (keys of [ai_slug_ammo] are prefixes).
        slug = self.numbers("ai_weapon_class_rules", "slug_engage_max")
        if limit is not None and weapon_class == "shotgun" and ammo and slug and any(
                ammo.startswith(key) for key in self.sections.get("ai_slug_ammo", {})):
            limit = max(limit, slug[0])
        return limit

    def band_of_rank(self, rank):
        rating = [v.strip() for v in (self.get("game_relations", "rating") or "").split(",") if v.strip()]
        name = None
        if rating:
            name = rating[-1]
            for i in range(0, len(rating) - 1, 2):
                if rank < number(rating[i + 1], float("inf")):
                    name = rating[i]
                    break
        band = number(self.get("ai_rank_bands", name)) if name else None
        return int(band) if band and 1 <= band <= 4 else None


# DA's rating thresholds (configs/creatures/game_relations.ltx), used when the ltx given does not carry them.
DA_RATING = "novice, 499, trainee, 1499, experienced, 2999, professional, 4999, veteran, 7499, expert, 10499, " \
            "master, 13999, legend"


# ------------------------------------------------------------------------------------------------------------------
# One run
# ------------------------------------------------------------------------------------------------------------------
class Run:
    def __init__(self, path):
        self.path = path
        self.events = []        # (t, event, fields, raw)
        self.periods = []       # {"dt", "frames", "global": {}, "comm": {c: {}}}
        self.errors = []
        self.warnings = defaultdict(int)
        self.scenario, self.phase = "other", ""
        self.label_note = ""
        self._read()

    @staticmethod
    def _lines(text):
        """The log's lines, those the log writer ran together split again; a cut line loses its last, maybe partial,
        value."""
        for raw in text.splitlines():
            pieces = [piece for piece in LINE_START.split(raw) if piece]
            for index, piece in enumerate(pieces):
                if index + 1 < len(pieces):
                    piece = piece.rsplit(" ", 1)[0]
                yield piece

    def _read(self):
        period, qa_t = None, 0.0
        for raw in self._lines(self.path.read_text(encoding="utf-8", errors="replace")):
            qa = QA.search(raw)
            if qa:
                t, event, rest = float(qa.group(1)), qa.group(2), qa.group(3)
                qa_t = t
                fields = dict(KV.findall(rest))
                self.events.append((t, event, fields, rest.strip()))
                if event == "start":
                    self.scenario = fields.get("scenario", "other")
                    self.phase = fields.get("phase", "")
                if event == "error":
                    self.errors.append(raw.strip())
                continue
            ai = AI.search(raw)
            if ai:
                header, community, rest = ai.group(1), ai.group(2), ai.group(3)
                values = {k: number(v, 0.0) for k, v in KV.findall(rest)}
                if header:
                    h = dict(KV.findall(header))
                    # a long table continues on further lines with the same header
                    if not (period and period["t"] == number(h["t"]) and not period["comm"]):
                        # qa_t: the probe's clock when the engine printed the period (the engine keeps its own)
                        period = {"t": number(h["t"]), "dt": number(h["dt"], 0.0), "frames": int(h["frames"]),
                                  "qa_t": qa_t, "global": {}, "comm": {}}
                        self.periods.append(period)
                    period["global"].update(values)
                elif period is not None:
                    period["comm"].setdefault(community, {}).update(values)
                continue
            if ERRORS.search(raw):
                self.errors.append(raw.strip())
            elif raw.startswith("!"):
                self.warnings[raw.strip()[:160]] += 1

    @property
    def label(self):
        names = {"ai_fair": "R1", "ai_factions": "R2", "ai_actor": "R3", "ai_newgame": "R6"}
        if self.scenario == "ai_save":
            return "R4b" if self.phase == "b" else "R4a"
        base = names.get(self.scenario, self.path.parent.name if self.path.name == "log.txt" else self.path.stem)
        return base + self.label_note

    def of(self, event):
        return [(t, f) for t, e, f, _ in self.events if e == event]

    def void(self):
        for t, e, f, rest in self.events:
            if e == "done" and rest.startswith("void"):
                return rest
            if e == "void":
                return rest
        return None

    def done(self):
        return next((rest for t, e, f, rest in self.events if e == "done"), None)

    def before(self, t_cut):
        """The run up to a probe time: its probe lines before it and the counter periods printed by then."""
        view = copy.copy(self)
        view.events = [e for e in self.events if e[0] < t_cut and e[1] != "done"]
        view.periods = [p for p in self.periods if p["qa_t"] <= t_cut]
        return view

    def total(self, key, community=None):
        """A counter over all periods, combined by its kind; None when no period printed it."""
        kind = KINDS.get(key, "count")
        values, weights = [], []
        for p in self.periods:
            table = p["global"] if community is None else p["comm"].get(community, {})
            if key in table:
                values.append(table[key])
                weights.append(p["dt"])
        if not values:
            return None
        if kind in ("count", "sum"):
            return sum(values)
        if kind == "avg":
            return statistics.mean(values)
        if kind == "min":
            return min(values)
        if kind in ("max", "gauge"):
            return max(values)
        span = sum(weights)
        return sum(v * w for v, w in zip(values, weights)) / span if span else statistics.mean(values)

    def per_frame_ms(self, key):
        """A per-second timer (us/s) as the average ms per frame."""
        us, frames = 0.0, 0
        for p in self.periods:
            if key in p["global"]:
                us += p["global"][key] * p["dt"]
                frames += p["frames"]
        return us / frames / 1000.0 if frames else None

    def communities(self):
        return sorted({c for p in self.periods for c in p["comm"]})


# ------------------------------------------------------------------------------------------------------------------
# Probe data helpers
# ------------------------------------------------------------------------------------------------------------------
def beliefs(run, target):
    """(t, fields) of every belief change on the target, in order."""
    return [(t, f) for t, f in run.of("belief") if f.get("target") == target]


def role_group(role, prefix):
    return role.startswith(prefix) and role[len(prefix):].isdigit()


def first(items, predicate=lambda t, f: True):
    return next(((t, f) for t, f in items if predicate(t, f)), (None, None))


def event_time(run, event, **match):
    t, _ = first(run.of(event), lambda t, f: all(f.get(k) == v for k, v in match.items()))
    return t


def fmt(value, digits=1):
    return "-" if value is None else (f"{value:.{digits}f}" if isinstance(value, float) else str(value))


class Table:
    def __init__(self):
        self.rows = []

    def add(self, cid, run, verdict, measured):
        self.rows.append((cid, run, verdict, measured))

    def failed(self):
        return any(v == "FAIL" for _, _, v, _ in self.rows)

    def markdown(self):
        lines = ["| Id | Run | Verdict | Measured |", "|---|---|---|---|"]
        lines += [f"| {c} | {r} | {v} | {m.replace('|', '/')} |" for c, r, v, m in self.rows]
        return "\n".join(lines)


def verdict(ok):
    return "PASS" if ok else "FAIL"


# ------------------------------------------------------------------------------------------------------------------
# Checks every run gets
# ------------------------------------------------------------------------------------------------------------------
def check_common(run, table):
    name = run.label
    if not run.periods:
        table.add("P1", name, "NOT MEASURED", "no [ai] counter lines (ai_log_period off?)")
    else:
        blind = run.total("k_blind_shot") or 0
        table.add("P1", name, verdict(blind == 0), f"k_blind_shot={fmt(blind, 0)}")

    lat_min = run.total("c_lat_min_ms")
    early = []
    seen_src = {}
    for t, f in run.of("belief"):
        key = (f.get("npc"), f.get("target"))
        src = f.get("src", "")
        if src.startswith("report_") and seen_src.get(key) != src and number(f.get("age"), 1e9) < LATENCY_FLOOR_MS:
            early.append(f"{f.get('role')}:{src}:{f.get('age')}ms")
        seen_src[key] = src
    if lat_min is None and not run.of("belief"):
        table.add("P5", name, "NOT MEASURED", "no delivery latency and no report beliefs")
    else:
        ok = (lat_min is None or lat_min >= LATENCY_FLOOR_MS) and not early
        table.add("P5", name, verdict(ok), f"c_lat_min_ms={fmt(lat_min, 0)}; report beliefs younger than "
                  f"{LATENCY_FLOOR_MS} ms at first sight: {', '.join(early) or 'none'}")

    finished = run.done()
    # "error no_<spot>" is the probe failing to place a scenario part (layout), not an error of the game.
    placement = [e for e in run.errors if re.search(r"\] t=[\d.]+ error no_", e)]
    problems = [e for e in run.errors if e not in placement]
    if finished is None:
        problems.append("probe never printed done")
    elif finished.startswith("error"):
        problems.append(f"probe: {finished}")
    frequent = sorted(run.warnings.items(), key=lambda kv: -kv[1])[:3]
    detail = "; ".join(problems[:5]) or "no errors"
    if placement:
        detail += " | probe could not place: " + "; ".join(e.split(" error ", 1)[-1] for e in placement)
    if frequent:
        detail += " | most frequent warnings: " + "; ".join(f"{n}x {w}" for w, n in frequent)
    table.add("P9", name, verdict(not problems), detail)

    violations = run.total("c_radio_violation")
    if violations is None:
        table.add("P11", name, "NOT MEASURED" if not run.periods else "PASS", "c_radio_violation never counted")
    else:
        table.add("P11", name, verdict(violations == 0), f"c_radio_violation={fmt(violations, 0)}")


def radio_flags(run):
    return {f["id"]: (f.get("comm"), f.get("radio") == "1", f.get("role"), number(f.get("rank"), 0))
            for t, f in run.of("npc") if "id" in f}


def reports(run):
    return run.of("report")


def check_reach(run, table, want_army_far=True, want_duty=False):
    """B8: voice reaches <= 60 m; army radio reaches farther; Duty radios work in R2; bandits have none."""
    rows = reports(run)
    if not rows:
        table.add("B8", run.label, "NOT MEASURED", "no report lines")
        return
    voice_far = [f for t, f in rows if f.get("channel") == "voice" and number(f.get("d"), -1) > VOICE_REACH_M]
    army_far = [f for t, f in rows if f.get("channel") == "radio" and f.get("comm") == "army"
                and number(f.get("d"), -1) > VOICE_REACH_M]
    duty_radio = [f for t, f in rows if f.get("channel") == "radio" and f.get("comm") == "dolg"]
    bandit_radio = [f for t, f in rows if f.get("channel") in ("radio", "hq") and f.get("comm") == "bandit"]
    ok = not voice_far and not bandit_radio and (army_far or not want_army_far) and (duty_radio or not want_duty)
    table.add("B8", run.label, verdict(ok),
              f"voice receivers beyond {VOICE_REACH_M} m: {len(voice_far)}; army radio receivers beyond "
              f"{VOICE_REACH_M} m: {len(army_far)}; Duty radio receivers: {len(duty_radio)}; bandit radio "
              f"receivers: {len(bandit_radio)}")


def check_reaction(run, table, ltx, communities):
    """B2: t_react_ms (identification -> first command to fire at him over an unbroken sight, fire-on-order holds and
    out-of-band targets left out). T2 adds distance, peripheral, fear and fatigue terms to the rolled react_ms, so the
    bare react_ms +-30 % of the plan's B2 applies to the rolled base (t_react_base_ms), and the measured reaction is
    judged against the one the gate required (t_react_due_ms): p50 t_react_ms <= 1.3 x p50 t_react_due_ms. The engine
    prints one average per period; their median stands for the p50 (one slow exchange does not move it). A log
    without t_react_due_ms (an older build) is judged by the old envelope: t_react_ms within react_ms +-30 %."""
    def median_of(community, key):
        values = [p["comm"][community][key] for p in run.periods if key in p["comm"].get(community, {})]
        return (statistics.median(values), len(values)) if values else (None, 0)

    parts, ok_all, measured = [], True, False
    for community in communities:
        value, count = median_of(community, "t_react_ms")
        due, _ = median_of(community, "t_react_due_ms")
        base, _ = median_of(community, "t_react_base_ms")
        span = ltx.profile_range(community, "react_ms")
        if value is None or not span:
            parts.append(f"{community}: {fmt(value, 0)} (profile {span})")
            continue
        measured = True
        if due is None or base is None:
            ok = span[0] * 0.7 <= value <= span[1] * 1.3
            parts.append(f"{community}: {value:.0f} ms (n={count}) vs {span[0]:.0f}-{span[1]:.0f} ms "
                         f"{'ok' if ok else 'OFF'} (no t_react_due_ms: old envelope)")
        else:
            base_ok = span[0] * 0.7 <= base <= span[1] * 1.3
            due_ok = value <= due * 1.3
            ok = base_ok and due_ok
            parts.append(f"{community}: {value:.0f} ms (n={count}) vs due {due:.0f} ms {'ok' if due_ok else 'OFF'}, "
                         f"base {base:.0f} vs {span[0]:.0f}-{span[1]:.0f} ms {'ok' if base_ok else 'OFF'}")
        ok_all &= ok
    table.add("B2", run.label, verdict(ok_all) if measured else "NOT MEASURED",
              "t_react_ms (median of the period averages): " + "; ".join(parts))


# ------------------------------------------------------------------------------------------------------------------
# The level's own creatures (R1, R4a): the probes' sweep
# ------------------------------------------------------------------------------------------------------------------
SWEEP_SETTLE_S = 3.0  # the switch manager has taken the flagged creatures offline by then


def check_area(run, table):
    """Q (information, no verdict): the level's own creatures the probe kept offline (its "sweep" lines: "online" is the
    number of foreign creatures still online within the sweep radius, printed on every change) and the stretches after
    SWEEP_SETTLE_S in which any was online: the trials measured the probe's own NPCs only outside them."""
    sweeps = run.of("sweep")
    if not sweeps:
        table.add("Q", run.label, "INFO", "no sweep lines (a probe before the sweep): the level's own NPCs took part")
        return
    last = sweeps[-1][1]
    t_end = run.events[-1][0] if run.events else sweeps[-1][0]
    busy, peak, nearest = [], 0, None
    for index, (t, f) in enumerate(sweeps):
        count = int(number(f.get("online"), 0))
        until = sweeps[index + 1][0] if index + 1 < len(sweeps) else t_end
        if count <= 0 or until < SWEEP_SETTLE_S:
            continue
        start = max(t, SWEEP_SETTLE_S)
        if busy and busy[-1][1] >= start:
            busy[-1] = (busy[-1][0], until)
        else:
            busy.append((start, until))
        peak = max(peak, count)
        d = number(f.get("nearest"), -1)
        if d >= 0:
            nearest = d if nearest is None else min(nearest, d)
    stretches = ", ".join(f"{a:.1f}-{b:.1f}" for a, b in busy[:6]) + (" ..." if len(busy) > 6 else "")
    table.add("Q", run.label, "INFO",
              f"level creatures kept offline: {fmt(number(last.get('squads'), 0), 0)} squads, "
              f"{fmt(number(last.get('singles'), 0), 0)} single (left online as traders or companions: "
              f"{fmt(number(last.get('kept'), 0), 0)}); foreign creatures online within the sweep radius after "
              f"t={SWEEP_SETTLE_S:.0f} s: " + (f"up to {peak} at t={stretches} (nearest {fmt(nearest, 0)} m)" if busy
                                               else "none"))


def gunner_note(run, role):
    """The scripted shooter's rounds (its "shot" lines) and why it missed its burst, if it did."""
    rounds = sum(int(number(f.get("rounds"), 0)) for t, f in run.of("shot") if f.get("role") == role)
    missed = first(run.of("fire_missed"), lambda t, f: f.get("role") == role)[1]
    return f"{role} {rounds} rounds" + (f" (missed: {missed.get('why')})" if missed else "")


# The muzzle flash's counters (da_ai_light.cpp): processed, queued, and why a flash was not processed for an observer
# (the shooter's flash period, the range at this darkness, the view cone, the shooter already in sight, the line,
# dropped while waiting for rays).
FLASH_KEYS = (("s_flash", "processed"), ("s_flash_q", "queued"), ("s_flash_x_period", "period"),
              ("s_flash_x_range", "range"), ("s_flash_x_view", "view"), ("s_flash_x_seen", "seen"),
              ("s_flash_x_los", "line"), ("s_flash_x_wait", "wait"))


def flash_counts(periods, community=None):
    """The flash counters summed over the periods, globally or for one community; "none" when all are zero."""
    parts = []
    for key, label in FLASH_KEYS:
        total = sum((p["global"] if community is None else p["comm"].get(community, {})).get(key, 0.0)
                    for p in periods)
        if total:
            parts.append(f"{label} {total:.0f}")
    return ", ".join(parts) or "none"


def flash_window(run):
    """The counter periods of F's burst: printed after the probe's "flash_window open" line up to its "close" line
    (the probe prints and resets the counters at both), else the periods of the 6 s after the open line."""
    t_open = next((t for t, e, f, rest in run.events if e == "flash_window" and rest.startswith("open")), None)
    if t_open is None:
        return None, None
    t_close = next((t for t, e, f, rest in run.events if e == "flash_window" and rest.startswith("close")), None)
    end = t_close if t_close is not None else t_open + 6
    note = first(run.of("flash_window"), lambda t, f: True)[1] or {}
    return [p for p in run.periods if t_open < p["qa_t"] <= end], note


def flash_view_note(run, t_from):
    """O's view of F's muzzle at the first logged round (else at the burst's order) as the probe judged it by the
    engine's rules, and the rules that fail."""
    views = [(t, f) for t, f in run.of("flash_view") if t >= t_from]
    view = first(views, lambda t, f: f.get("at") == "shot")[1] or first(views)[1]
    if not view:
        return ""
    fails = []
    if view.get("in_view") != "1":
        fails.append("out of the view cone")
    vis, d, reach = number(view.get("vis")), number(view.get("d")), number(view.get("range"))
    if vis is not None and vis <= 0.05:
        fails.append("line blocked")
    if d is not None and reach is not None and d > reach:
        fails.append("beyond the range")
    if view.get("see") == "1":
        fails.append("F already in sight")
    return (f"O at the {view.get('at')}: {view.get('angle')} of {view.get('half_fov')} deg off its gaze, line vis "
            f"{view.get('vis')}, {view.get('d')} of {view.get('range')} m (dark {view.get('dark')}), see "
            f"{view.get('see')} -> " + (", ".join(fails) if fails else "every rule passes"))


def flash_by_sight(run, f_shot):
    """O's sight of F that the flash itself produced: the update that processes a flash raises O's meter by the flash's
    cue, and when that identifies F, sight replaces the flash's estimate in the same update, before any probe poll
    (r1l: heard_shot at the round, seen 0.10 s later, never a muzzle_flash belief in between). Counted when O did not
    see F before the round, O's view at the round passes every rule of the flash, and the burst's counters processed a
    flash in O's community. The moment of the sight, else None."""
    view = first([(t, f) for t, f in run.of("flash_view") if t >= f_shot - 0.2], lambda t, f: f.get("at") == "shot")[1]
    if not view or view.get("in_view") != "1" or view.get("see") == "1" or number(view.get("vis"), 0) <= 0.05 \
            or number(view.get("d"), 1e9) > number(view.get("range"), 0):
        return None
    periods, _ = flash_window(run)
    community = first(run.of("npc"), lambda t, f: f.get("role") == "O")[1] or {}
    if not periods or not sum(p["comm"].get(community.get("comm", "stalker"), {}).get("s_flash", 0) for p in periods):
        return None
    o_on_f = [(t, f) for t, f in run.of("belief") if f.get("role") == "O" and f.get("target") == "F"]
    before = [f for t, f in o_on_f if t < f_shot]
    if before and before[-1].get("see") == "1":
        return None
    return first(o_on_f, lambda t, f: t >= f_shot and f.get("see") == "1")[0]


def flash_result(run, f_shot):
    """B1's flash: (seconds from F's first round to O's muzzle_flash estimate or to the sight of F the flash produced,
    text)."""
    flash = first([(t, f) for t, f in run.of("belief") if f.get("role") == "O" and f.get("target") == "F"],
                  lambda t, f: t >= f_shot and f.get("src") == "muzzle_flash")[0]
    if flash is not None:
        return flash - f_shot, f"muzzle flash estimate after {flash - f_shot:.2f} s"
    sight = flash_by_sight(run, f_shot)
    if sight is not None:
        return sight - f_shot, (f"muzzle flash processed and F identified by it {sight - f_shot:.2f} s after the round "
                                f"(sight replaced the flash's estimate in the same update)")
    return None, "muzzle flash estimate after - s"


def flash_diagnosis(run, t_from):
    """B1's flash detail: O's view, then the burst's counters (all observers and O's community, loners)."""
    parts = []
    view = flash_view_note(run, t_from)
    if view:
        parts.append(view)
    periods, note = flash_window(run)
    if periods is None:
        periods = [p for p in run.periods if p["qa_t"] >= t_from]
        scope = "counters after the night"
    else:
        scope = f"counters of the burst (other probe stalkers in range: {note.get('near', '-')})"
    parts.append(f"{scope}: all {flash_counts(periods)}; loners {flash_counts(periods, 'stalker')}")
    return "; ".join(parts)


def think_time(run):
    """P10's figures (6.3): think time per frame in all and by lane, the worst single NPC update and the worst of each
    of its parts. (within the budget, text)"""
    total = run.per_frame_ms("p_total_us")
    worst = run.total("p_max_us")
    lanes = {k: run.per_frame_ms(k) for k in LANE_BUDGET_MS}
    over = [f"{k}={v:.3f}" for k, v in lanes.items() if v is not None and v > LANE_BUDGET_MS[k]]
    ok = total is not None and total <= TOTAL_BUDGET_MS and (worst or 0) <= SINGLE_NPC_BUDGET_US and not over
    periods = sorted(p["global"]["p_max_us"] for p in run.periods if "p_max_us" in p["global"])
    typical = statistics.median(periods) if periods else None
    parts = {k[6:-3]: run.total(k) for k in MAX_PARTS if run.total(k) is not None}
    subparts = {k[6:-3]: run.total(k) for k in MAX_SUBPARTS if run.total(k) is not None}
    over_periods = sum(1 for v in periods if v > SINGLE_NPC_BUDGET_US)
    return ok, (f"p_total {fmt(total, 3)} ms/frame, p_max_us {fmt(worst, 0)} (median of the periods {fmt(typical, 0)}, "
                f"{over_periods} of {len(periods)} periods over {SINGLE_NPC_BUDGET_US}), lanes "
                + ", ".join(f"{k[2:-3]} {fmt(v, 3)}" for k, v in lanes.items()) + (f"; over: {over}" if over else "")
                + (("; worst part " + ", ".join(f"{k} {fmt(v, 0)}" for k, v in parts.items())) if parts else "")
                + ((" (" + ", ".join(f"{k} {fmt(v, 0)}" for k, v in subparts.items()) + ")") if subparts else ""))


def pre_step_note(run, ltx):
    """The senses' pre step (p_max_pre_us) against [ai_noise] pre_budget_us, which bounds it after its first
    stimulus: the worst, the median of the periods' worst, the periods over the budget and over 400 us."""
    pre = sorted(p["global"]["p_max_pre_us"] for p in run.periods if "p_max_pre_us" in p["global"])
    if not pre:
        return "no pre step counted"
    budget = (ltx.numbers("ai_noise", "pre_budget_us") or [150])[0]
    return (f"pre step worst {pre[-1]:.0f} us, median of the periods {statistics.median(pre):.0f}, "
            f"{sum(1 for v in pre if v > budget)} of {len(pre)} periods over pre_budget_us {budget:.0f}, "
            f"{sum(1 for v in pre if v > SINGLE_NPC_BUDGET_US)} over {SINGLE_NPC_BUDGET_US}")


# ------------------------------------------------------------------------------------------------------------------
# R1 ai_fair
# ------------------------------------------------------------------------------------------------------------------
def check_r1(run, table, ltx):
    name = run.label
    check_area(run, table)
    void = run.void()
    actor = beliefs(run, "actor")
    t_hit_a = event_time(run, "event", role="A1")
    t_hit_c = event_time(run, "event", role="C1")

    # P3: the hive mind. A layout that broke late is judged on what happened before it broke: the separations held
    # until then, and after it the squads may hear each other.
    t_void = first(run.of("layout"), lambda t, f: f.get("ok") == "0")[0] if void else None
    late = f" (layout broke at t={t_void:.1f}: {void}; judged before it)" if t_void is not None else ""
    if void and t_void is None:
        table.add("P3", name, "VOID", void)
        table.add("P4", name, "VOID", void)
    else:
        if t_void is not None:
            actor = [(t, f) for t, f in actor if t < t_void]
        problems = []
        seen_npcs = set()
        for t, f in actor:
            role, src = f.get("role", "-"), f.get("src", "")
            if f.get("see") == "1":
                seen_npcs.add(f.get("npc"))
            if role_group(role, "B"):
                if src.startswith("report_") or src in ("witness", "script") or src.startswith("heard_"):
                    problems.append(f"{role} {src} t={t:.1f}")
                elif src == "seen" and f.get("npc") not in seen_npcs:
                    problems.append(f"{role} seen without sight t={t:.1f}")
            if role == "A4" and src not in ("none", "report_pda") and not (src == "seen" and f.get("see") == "1"):
                problems.append(f"A4 {src} t={t:.1f}")
            if role == "A4" and src == "report_pda" and t_hit_a is not None and t - t_hit_a < PDA_MIN_S:
                problems.append(f"A4 report_pda after {t - t_hit_a:.1f} s")
            if role in ("A2", "A3") and src.startswith("report_") and t_hit_a is not None \
                    and (t - t_hit_a) * 1000 < LATENCY_FLOOR_MS:
                problems.append(f"{role} {src} after {(t - t_hit_a) * 1000:.0f} ms")
            if role in ("A2", "A3") and src in ("report_radio", "report_hq"):
                problems.append(f"{role} {src} (bandits carry no radio)")
        mates = {f.get("role"): (round(t - t_hit_a, 2) if t_hit_a is not None else None, f.get("src"))
                 for t, f in actor if f.get("role") in ("A2", "A3") and f.get("src", "").startswith("report_")}
        table.add("P3", name, verdict(not problems) if t_hit_a is not None else "NOT MEASURED",
                  f"violations: {', '.join(problems) or 'none'}; A2/A3 first reports (s after the hit, source): "
                  f"{mates or 'none'}{late}")

        # P4: radio and HQ
        problems, facts = [], []
        c4 = first([(t, f) for t, f in actor if f.get("role") == "C4"],
                   lambda t, f: f.get("src", "").startswith("report_"))
        if t_hit_c is None:
            table.add("P4", name, "NOT MEASURED", "no hit on C1")
        else:
            if c4[0] is None or c4[1].get("src") != "report_radio":
                problems.append(f"C4 first report {c4[1].get('src') if c4[1] else 'none'}")
            else:
                delay = c4[0] - t_hit_c
                facts.append(f"C4 report_radio after {delay:.2f} s")
                if not RADIO_WINDOW_S[0] <= delay <= RADIO_WINDOW_S[1] + 0.1:
                    problems.append(f"C4 radio after {delay:.2f} s outside {RADIO_WINDOW_S}")
            for role in ("C2", "C3"):
                got = first([(t, f) for t, f in actor if f.get("role") == role],
                            lambda t, f: f.get("src") in ("report_voice", "report_hushed", "report_radio", "seen"))
                if got[0] is None:
                    problems.append(f"{role} got no report")
                else:
                    facts.append(f"{role} {got[1].get('src')} after {got[0] - t_hit_c:.2f} s")
            c_first = first([(t, f) for t, f in actor if role_group(f.get("role", ""), "C") or f.get("role") == "C4"],
                            lambda t, f: t >= t_hit_c and f.get("src", "").startswith("report_"))[0]
            hq_min = (ltx.numbers("ai_profile_army", "hq_delay_s") or [HQ_DELAY_DEFAULT_S])[0]
            for t, f in actor:
                if not f.get("role", "").startswith("Cp"):
                    continue
                src = f.get("src", "")
                if src.startswith("report_") and src != "report_hq":
                    problems.append(f"{f.get('role')} {src} t={t:.1f}")
                if src == "report_hq" and c_first is not None and t - c_first < hq_min:
                    problems.append(f"{f.get('role')} report_hq {t - c_first:.1f} s after C's first report")
            hq = first([(t, f) for t, f in actor if f.get("role", "").startswith("Cp")],
                       lambda t, f: f.get("src") == "report_hq")
            if hq[0] is None and t_void is not None and not problems:
                table.add("P4", name, "VOID", "; ".join(facts) + f"; no report_hq before the layout broke{late}")
                hq = None
            elif hq[0] is None:
                problems.append("squad C' got no report_hq")
            else:
                facts.append(f"C' report_hq at t={hq[0]:.1f}" + (f" ({hq[0] - c_first:.1f} s after C's first report)"
                                                                  if c_first is not None else ""))
            if hq is not None:
                table.add("P4", name, verdict(not problems), "; ".join(problems + facts) + late)

    # P6: heard-shot estimates, report errors
    e_on = {s: first([(t, f) for t, f in run.of("belief") if f.get("role") == "E" and f.get("target") == s],
                     lambda t, f: f.get("src") == "heard_shot")[1] for s in ("S1", "S2")}
    err1 = number(e_on["S1"].get("err")) if e_on["S1"] else None
    err2 = number(e_on["S2"].get("err")) if e_on["S2"] else None
    report_problems = report_error_problems(run, ltx)
    s2_fired = bool(first(run.of("shot"), lambda t, f: f.get("role") == "S2")[0] is not None)
    bursts = f"; bursts: {gunner_note(run, 'S1')}, {gunner_note(run, 'S2')}"
    if err1 is not None and err2 is None and s2_fired:
        # The suppressed burst gave E no estimate at all: less than the unsuppressed one, as the criterion wants.
        ok = 3 <= err1 <= 25 and not report_problems
        table.add("P6", name, verdict(ok), f"unsuppressed {err1:.1f} m, suppressed burst not heard at all; report "
                  f"errors below the sender's: {', '.join(report_problems) or 'none'}{bursts}")
    elif err1 is None or err2 is None:
        table.add("P6", name, "NOT MEASURED", f"E heard_shot error S1={fmt(err1)} S2={fmt(err2)} m; report errors "
                  f"below the sender's: {len(report_problems)}{bursts}")
    else:
        ok = 3 <= err1 <= 25 and err2 >= 1.5 * err1 and not report_problems
        table.add("P6", name, verdict(ok), f"unsuppressed {err1:.1f} m, suppressed {err2:.1f} m; report errors below "
                  f"the sender's: {', '.join(report_problems) or 'none'}{bursts}")

    # B1 (R1 part): steps, night, muzzle flash
    parts, ok_all, measured = [], True, False
    hearing = {(f.get("gait"), f.get("d")): f for t, f in run.of("hearing")}
    run15, crouch15 = hearing.get(("run", "15")), hearing.get(("crouch", "15"))
    if run15 and crouch15 and run15.get("calm") == "1" and crouch15.get("calm") == "1":
        measured = True
        ok = run15.get("suspicious") == "1" and crouch15.get("suspicious") == "0"
        ok_all &= ok
        parts.append(f"run 15 m suspicious={run15.get('suspicious')} (calm={run15.get('calm')}), crouch 15 m "
                     f"suspicious={crouch15.get('suspicious')}")
    elif run15 or crouch15:
        # What E fought instead (probes since the sweep log it on the hearing line).
        why = [f"{g} 15 m calm={h.get('calm')}" + (f" enemy={h.get('enemy')} at {h.get('enemy_d')} m"
                                                   if h.get("enemy") else "")
               for g, h in (("run", run15), ("crouch", crouch15)) if h]
        parts.append("steps not judged, E not calm: " + ", ".join(why))
    t_night = event_time(run, "night")
    t_torch = event_time(run, "torch_on")
    o_on_t = [(t, f) for t, f in run.of("awareness") if f.get("role") == "O" and f.get("target") == "T"]
    # A torch T still carried at night (logs before the probe stripped it on every update) voids the dark trial.
    lamp_t = first([(t, f) for t, f in run.of("belief") if f.get("role") == "O" and f.get("target") == "T"],
                   lambda t, f: f.get("src") == "flashlight")[0]
    if t_night is not None and t_torch is not None and lamp_t is not None and lamp_t < t_torch:
        parts.append(f"night trial void: T's own torch gave it away at t={lamp_t:.1f}, before torch_on at "
                     f"t={t_torch:.1f}")
    elif t_night is not None and t_torch is not None:
        measured = True
        dark = [t for t, f in o_on_t if t_night <= t < min(t_torch, t_night + 10) and number(f.get("value"), 0) >= 1]
        lit = first(o_on_t, lambda t, f: t >= t_torch and number(f.get("value"), 0) >= 1)[0]
        ok = not dark and lit is not None and lit - t_torch <= 3
        ok_all &= ok
        # How squarely T's heading pointed at O when its torch came on (1 = straight at O; older probes: not logged).
        facing = number(first(run.of("torch_on"), lambda t, f: True)[1].get("facing"))
        low = first(run.of("relaxed"), lambda t, f: f.get("spot") == "T" and f.get("low_line") == "0")[0]
        parts.append(f"unlit identified: {'yes' if dark else 'no'}; lit identified after "
                     f"{fmt(lit - t_torch if lit is not None else None, 2)} s"
                     + (f" (T facing O {facing:.2f})" if facing is not None and facing > -1.5 else "")
                     + (" (no clear line to T's head)" if low is not None else ""))
    f_shot = first(run.of("shot"), lambda t, f: f.get("role") == "F")[0]
    # Where F stood (the plan's 150 m from O; older probes took the best-hidden spot at 130-170 m).
    f_spot = first(run.of("spot"), lambda t, f: f.get("name") == "F")[1]
    f_where = f" (F {f_spot.get('from_O')} m from O)" if f_spot else ""
    if f_shot is not None:
        measured = True
        delay, text = flash_result(run, f_shot)
        ok = delay is not None and delay <= 1.0
        ok_all &= ok
        parts.append(f"{text}{f_where} ({flash_diagnosis(run, t_night if t_night is not None else f_shot)})")
    elif t_night is not None:
        parts.append("muzzle flash not measured: " + (f"F never fired{f_where}: {gunner_note(run, 'F')}" if f_spot
                                                      else "F not placed"))
    table.add("B1", name, verdict(ok_all) if measured else "NOT MEASURED", "; ".join(parts) or "no data")
    check_reach(run, table, want_army_far=True)
    if run.periods:
        ok, text = think_time(run)
        table.add("P10", name, "INFO", f"judged in R2 (here {'within' if ok else 'over'} the budget): {text}; "
                  f"{pre_step_note(run, ltx)}")


REPORT_CALLBACK_DEFAULT_MS = 2000
# The probe prints a belief when its source changes (a 100 ms poll) and a report at its delivery (dispatched at most a
# frame later): the report that made a belief lies this close to it, and a sender's belief is printed this late.
REPORT_MATCH_S = 0.6
POLL_LAG_S = 0.2
# Kinds whose delivery places the target (da_ai_comms.h enemy_kind): only these make a report belief.
LOCATING_KINDS = {"contact", "update", "lost", "taking_fire", "reloading", "flanking", "cover_me", "fall_back",
                  "attack", "search_assign"}


def report_error_problems(run, ltx):
    """P6: a receiver's report belief never carries a smaller error than the sender's own belief had.

    The report behind a report_<channel> belief is the delivery to that receiver of that target on that channel next to
    the belief line. The engine calls the probe's report callback at most once per receiver and target per [ai_comms]
    report_callback_ms, so that delivery may be missing (r1i: A1's report_pda came 1.9 s after A3's logged
    search_assign, and the parser compared it with A3's error): then the senders of the same channel's reports of that
    target to anybody within the throttle stand for it, the best of them counted. The sender's error is its logged
    belief when the estimate was observed (t - age, plus the poll's lag), the error that went into the report."""
    throttle = (ltx.numbers("ai_comms", "report_callback_ms") or [REPORT_CALLBACK_DEFAULT_MS])[0] / 1000.0
    problems = []
    history = defaultdict(list)   # (npc id, target) -> [(t, err)] of its logged beliefs
    roles = {}                    # role -> npc ids
    for t, e, f, _ in run.events:
        if e == "belief":
            roles.setdefault(f.get("role"), set()).add(f.get("npc"))
    reports = [(t, f) for t, e, f, _ in run.events if e == "report"]

    def error_at(role, target, t):
        errs = [err for sid in roles.get(role, ()) for bt, err in history[(sid, target)] if bt <= t]
        return errs[-1] if errs else None

    for t, e, f, _ in run.events:
        if e != "belief":
            continue
        key = (f.get("npc"), f.get("target"))
        err = number(f.get("err"))
        src = f.get("src", "")
        if src.startswith("report_") and err is not None and err >= 0:
            channel = src[len("report_"):]
            target = f.get("target")
            same = [(rt, r) for rt, r in reports if r.get("target") == target and r.get("channel") == channel
                    and r.get("kind") in LOCATING_KINDS]
            own = [(rt, r) for rt, r in same if r.get("npc") == f.get("npc") and abs(rt - t) <= REPORT_MATCH_S]
            if own:
                senders = {min(own, key=lambda x: abs(x[0] - t))[1].get("sender")}
            else:
                senders = {r.get("sender") for rt, r in same if abs(rt - t) <= throttle + REPORT_MATCH_S}
            age = number(f.get("age"))
            observed = t - age / 1000.0 if age is not None and age >= 0 else t
            errs = [x for x in (error_at(role, target, observed + POLL_LAG_S) for role in senders)
                    if x is not None and x >= 0]
            if errs and err + 0.05 < min(errs):
                who = "/".join(sorted(s for s in senders if s))
                problems.append(f"{f.get('role')} {err:.1f} < {who} {min(errs):.1f}")
        history[key].append((t, err))
    return problems


# ------------------------------------------------------------------------------------------------------------------
# R2 ai_factions
# ------------------------------------------------------------------------------------------------------------------
def before_night(run, t_night):
    """The run up to the night trial's "night" line: the counter periods printed by then and the probe lines before
    it, the done line (and its void) kept. The periods after it hold the flash trial, not the fights."""
    view = copy.copy(run)
    view.events = [e for e in run.events if e[0] < t_night or e[1] == "done"]
    view.periods = [p for p in run.periods if p["qa_t"] <= t_night]
    return view


def check_r2(run, table, ltx):
    # The night muzzle-flash trial after the fights (probes since r2m): B1's flash from the lines after its "night"
    # line, the fights' criteria from what came before it. A log without the trial reads as before.
    t_night = event_time(run, "night")
    if t_night is not None:
        check_flash(run, table)
        run = before_night(run, t_night)
    name = run.label
    void = run.void()

    # P10 performance
    npcs = run.total("npcs")
    if npcs is None or npcs < 16:
        table.add("P10", name, "NOT MEASURED", f"online stalkers (max) {fmt(npcs, 0)} < 16")
    else:
        ok, text = think_time(run)
        table.add("P10", name, verdict(ok), f"{text}; {pre_step_note(run, ltx)}")

    # P11 (R2 part): radios by profile and no radio reports to NPCs without one
    flags = radio_flags(run)
    problems = []
    for npc_id, (comm, radio, role, rank) in flags.items():
        if comm == "army" and not radio:
            problems.append(f"army {role} without radio")
        if comm == "bandit" and radio:
            problems.append(f"bandit {role} with radio")
    for t, f in run.of("belief"):
        entry = flags.get(f.get("npc"))
        if f.get("src") == "report_radio" and entry and entry[0] in ("killer", "dolg") and not entry[1]:
            problems.append(f"{entry[2]} report_radio without a radio")
    table.add("P11", name + " radios", verdict(not problems) if flags else "NOT MEASURED",
              "; ".join(problems) or f"{sum(1 for v in flags.values() if v[1])} of {len(flags)} NPCs carry a radio")

    # A layout that broke late (a beaten side fleeing towards another fight) is judged on the fights before it broke,
    # as R1's is; one that broke early tests nothing.
    if void:
        t_void = first(run.of("layout"), lambda t, f: f.get("ok") == "0")[0]
        if t_void is None or t_void < R2_MIN_JUDGED_S:
            for cid in ("B3", "B4", "B5", "B6"):
                table.add(cid, name, "VOID", void)
            check_reaction(run, table, ltx, ["army", "bandit", "monolith", "stalker", "killer", "dolg"])
            check_reach(run, table, want_army_far=False, want_duty=True)
            return
        run = run.before(t_void)
        run.label_note = f" before t={t_void:.0f} ({void})"
        name = run.label

    check_reaction(run, table, ltx, ["army", "bandit", "monolith", "stalker", "killer", "dolg"])

    # B3 doctrine: judged at the round by the engine (t_shots_oob: a round at a living target known beyond the
    # weapon's engage_max). The probe polls every 250 ms and measures to the enemy selected at the poll, so its lines
    # are a cross-check; a line whose best enemy changed since the last poll (switch=1) is not even that.
    cross, switched = [], 0
    close_holders = set()
    for t, f in run.of("shot"):
        cls = f.get("class")
        if cls in ("pistol", "shotgun", "smg"):
            close_holders.add(f.get("npc"))
        limit = ltx.engage_max(cls, f.get("ammo")) if cls in ("pistol", "shotgun") else None
        if limit is None or number(f.get("dist"), -1) <= limit:
            continue
        if f.get("switch") == "1":
            switched += 1
        else:
            cross.append(f"{f.get('role')} {cls} {f.get('ammo', '?')} {f.get('dist')} m see={f.get('see', '?')} "
                         f"target={f.get('target', '?')}")
    engine_oob = run.total("t_shots_oob") or 0.0
    open_m = run.total("t_open_m") or 0.0
    open_limit = 15 * max(1, len(close_holders))
    table.add("B3", name, verdict(engine_oob == 0 and open_m <= open_limit),
              f"t_shots_oob={engine_oob:.0f}; t_open_m={open_m:.0f} (limit 15 x {len(close_holders)} close-range "
              f"holders); probe cross-check, pistol/shotgun rounds beyond engage_max at the poll: {len(cross)} "
              f"{cross[:5]}, {switched} more after a target switch")

    # B4 cover share: the fight's samples in contact (a living member of the paired side as the enemy, the member
    # neither broken nor surrendered, and it saw that enemy itself within the last 3 s: the firefight, not a search or a
    # withdrawal out of sight) where the probe logs them and a side has CONTACT_MIN of them, else the fight's samples;
    # logs before either count every sample with a best enemy of any kind.
    # Probes before the quadrant fix read a quarter of the directions with an extrapolated cover value (see
    # ai_factions.lua cover_in_direction): their shares are recorded, not comparable.
    share, share_all, share_fight, basis = {}, {}, {}, {}
    fight_logged = False
    for t, f in run.of("side_state"):
        side = f.get("side")
        combat = number(f.get("combat"), 0)
        share_all[side] = number(f.get("covered"), 0) / combat if combat else None
        if "fight" in f:
            fight_logged = True
            fight = number(f.get("fight"), 0)
            share_fight[side] = number(f.get("fight_covered"), 0) / fight if fight else None
            contact = number(f.get("contact"), 0)
            if contact >= CONTACT_MIN:
                share[side], basis[side] = number(f.get("contact_covered"), 0) / contact, f"contact {contact:.0f}"
            else:
                share[side], basis[side] = share_fight[side], f"fight {fight:.0f}"
        else:
            share[side] = share_all[side]
    army, bandit, killer, duty = (share.get(s) for s in ("army", "bandit", "killer", "dolg"))
    measure = ("contact share (" + ", ".join(f"{s} {basis[s]}" for s in ("army", "bandit", "killer", "dolg")
                                            if s in basis) + " samples)") if fight_logged else         "all-enemy share (old probe)"
    others = ", ".join(f"{side} {value:.2f}" for side, value in sorted(share_all.items()) if value is not None)
    # The army's and the mercenaries' uncovered fight samples by combat action (the probe's last side_open lines).
    action_names = {f.get("id"): f.get("name", "?").replace("action_", "") for t, f in run.of("action_id")}
    open_actions = {}
    for t, f in run.of("side_open"):
        counts = [item.split(":") for item in f.get("actions", "-").split(",") if ":" in item]
        open_actions[f.get("side")] = sorted(((int(n), action_names.get(a, a)) for a, n in counts), reverse=True)
    open_text = "; ".join(f"{side} open in " + ", ".join(f"{a} {n}" for n, a in open_actions[side][:4])
                          for side in ("army", "killer") if open_actions.get(side))
    if None in (army, bandit, killer, duty):
        table.add("B4", name, "NOT MEASURED", f"{measure} {share}; all-enemy share {others}")
    else:
        ok = army >= 0.6 and army >= bandit + 0.15 and killer >= duty
        fights = ", ".join(f"{side} {value:.2f}" for side, value in sorted(share_fight.items()) if value is not None)
        table.add("B4", name, verdict(ok), f"{measure}: army {army:.2f}, bandit {bandit:.2f}, mercs {killer:.2f}, "
                  f"Duty {duty:.2f}; fight share {fights or '-'}; all-enemy share {others}"
                  + (f"; {open_text}" if open_text else ""))

    # B5 coordination (army and mercs judged; Duty and Monolith, trained by lore too, reported). A side that never
    # took on its paired side (the probe's "engaged" line) cannot show coordination against it, nor can one that the
    # level's own NPCs took over: half of it or more killed by a killer outside the probe's squads (r2k, k9p, r2e,
    # p10-profile: level loners killed three or four mercenaries), reported as not measured when it fails.
    engaged = {f.get("side") for t, f in run.of("engaged")}
    kicked = [f.get("fight") for t, f in run.of("kickoff")]
    probe_ids = {f.get("id") for t, f in run.of("npc")}
    third_kills, side_sizes = defaultdict(int), defaultdict(int)
    for t, f in run.of("npc"):
        side_sizes[f.get("comm")] += 1
    for t, f in run.of("death"):
        killer = f.get("killer")
        if probe_ids and killer and killer != "-" and killer not in probe_ids:
            third_kills[f.get("side")] += 1
    parts, ok_all, unmeasured = [], True, []
    for community in ("army", "killer", "dolg", "monolith"):
        supp, flank = run.total("t_suppress", community) or 0, run.total("t_flank", community) or 0
        ret = run.total("t_return_fire", community) or 0
        judged = community in ("army", "killer")
        taken_over = third_kills[community] >= max(2, (side_sizes[community] + 1) // 2)
        if judged and community not in engaged and run.of("engaged"):
            unmeasured.append(community)
        elif judged and taken_over and not (supp >= 1 and flank >= 1):
            unmeasured.append(f"{community} (third party killed {third_kills[community]})")
        elif judged:
            ok_all &= supp >= 1 and flank >= 1
        # Flank orders taken and dropped for want of a covered route (logs before K8 have neither); the leader's
        # other choices when he found no covered route (logs before the closing round have none).
        roles, noroute = run.total("t_flank_role", community), run.total("t_flank_noroute", community)
        alts = [(kind, run.total(f"t_alt_{kind}", community)) for kind in ("advance", "grenade", "hold")]
        parts.append(f"{community} t_suppress={supp:.0f} t_flank={flank:.0f} t_return_fire={ret:.0f}"
                     + (f" (flank roles {fmt(roles, 0)}, no route {fmt(noroute, 0)})"
                        if roles is not None or noroute is not None else "")
                     + (" (no route, instead: " + ", ".join(f"{kind} {fmt(value, 0)}" for kind, value in alts) + ")"
                        if any(value is not None for kind, value in alts) else "")
                     + ("" if community in engaged or not run.of("engaged") else " (never engaged)")
                     + (f" (killed by third parties {third_kills[community]})" if third_kills[community] else ""))
    parts.append(f"bandit t_flank={fmt(run.total('t_flank', 'bandit'), 0)}")
    if kicked:
        parts.append(f"kick-off in {', '.join(kicked)}")
    if unmeasured:
        parts.append(f"not measured: {', '.join(unmeasured)}")
    b5 = "NOT MEASURED" if unmeasured and ok_all else verdict(ok_all)
    table.add("B5", name, b5, "; ".join(parts))

    # B6 (R2 part)
    deaths = defaultdict(int)
    for t, f in run.of("death"):
        deaths[f.get("side")] += 1
    sizes = defaultdict(int)
    for npc_id, (comm, radio, role, rank) in flags.items():
        sizes[comm] += 1
    mono = (run.total("m_retreat_orderly", "monolith") or 0) + (run.total("m_retreat_scatter", "monolith") or 0)
    army_scatter = run.total("m_retreat_scatter", "army") or 0
    army_losses = deaths["army"] / sizes["army"] if sizes["army"] else 0
    merc_scatter = run.total("m_retreat_scatter", "killer") or 0
    ok = mono == 0 and (army_scatter == 0 or army_losses >= 0.55) and merc_scatter == 0
    table.add("B6", name, verdict(ok), f"Monolith retreats {mono:.0f}; army scatter {army_scatter:.0f} at "
              f"{army_losses:.0%} losses; mercs scatter {merc_scatter:.0f}, orderly "
              f"{fmt(run.total('m_retreat_orderly', 'killer'), 0)}")
    check_reach(run, table, want_army_far=False, want_duty=True)


# ------------------------------------------------------------------------------------------------------------------
# R3 ai_actor
# ------------------------------------------------------------------------------------------------------------------
def check_r3(run, table, ltx):
    name = run.label
    void = run.void()
    flags = radio_flags(run)
    if "rating" not in ltx.sections.get("game_relations", {}):
        ltx.sections.setdefault("game_relations", {})["rating"] = DA_RATING

    # B1: identification of the actor per NPC
    onset, identified, calm = {}, {}, {}
    for t, f in run.of("awareness"):
        if f.get("target") != "actor":
            continue
        npc, value = f.get("npc"), number(f.get("value"), 0)
        if value > 0 and npc not in onset:
            onset[npc] = t
            calm[npc] = f.get("danger") == "0"
        if value >= 1 and npc not in identified:
            identified[npc] = t
    parts, ok_all = [], True
    for npc, t in sorted(identified.items(), key=lambda kv: kv[1]):
        entry = flags.get(npc)
        band = ltx.band_of_rank(entry[3]) if entry else None
        took = t - onset.get(npc, t)
        if band is None:
            parts.append(f"{entry[2] if entry else npc} {took:.2f} s (band ?)")
            continue
        lo, hi = IDENT_BANDS[band]
        factor = CALM_FACTOR if calm.get(npc) else 1.0
        ok = lo * factor * (1 - IDENT_TOLERANCE) <= took <= hi * factor * (1 + IDENT_TOLERANCE)
        ok_all &= ok
        parts.append(f"{entry[2]} R{band}{' calm' if calm.get(npc) else ''} {took:.2f} s {'ok' if ok else 'OFF'}")
    table.add("B1", name, verdict(ok_all) if identified else "NOT MEASURED", "; ".join(parts) or "nobody identified")
    check_reaction(run, table, ltx, ["army", "bandit", "monolith"])

    # settle: hits on the actor per second after each shooter identified him
    hits = defaultdict(list)
    for t, f in run.of("actor_hit"):
        hits[f.get("npc")].append(t)
    settle = []
    for npc, times in hits.items():
        start = identified.get(npc)
        if start is not None and times:
            span = max(1.0, times[-1] - start)
            settle.append(f"{flags.get(npc, (None, None, npc))[2]} {len(times)} hits / {span:.0f} s")
    table.add("settle", name, "INFO", "; ".join(settle) or "no hits on the actor")

    if void:
        table.add("P7", name, "VOID", void)
        table.add("B6", name, "VOID", void)
        return

    # P7: leader death known only by perception or message
    t_kill, leader_role = None, None
    for t, f in run.of("event"):
        if f.get("squad") == "army" and f.get("leader") == "1":
            t_kill, leader_role = t, f.get("role")
    if t_kill is None:
        table.add("P7", name, "NOT MEASURED", "no army leader killed")
    else:
        saw = {f.get("npc") for t, f in run.of("witness") if f.get("victim") == leader_role and f.get("see") == "1"}
        near = {f.get("npc") for t, f in run.of("witness")
                if f.get("victim") == leader_role and number(f.get("d"), 1e9) <= VOICE_REACH_M}
        told = {f.get("npc") for t, f in reports(run)
                if f.get("target") == leader_role and f.get("kind") in ("man_down", "leader_down")}
        known = run.total("m_leader_known", "army") or 0
        bound = len(saw | near | told)
        handover = first(run.of("leader"),
                         lambda t, f: t > t_kill and f.get("comm") == "army" and f.get("value") == "1")
        table.add("P7", name, verdict(known <= bound),
                  f"m_leader_known={known:.0f} <= saw {len(saw)} + within {VOICE_REACH_M} m {len(near - saw)} + told "
                  f"{len(told - saw - near)} = {bound}; new leader: "
                  f"{handover[1].get('role') + f' after {handover[0] - t_kill:.1f} s' if handover[1] else 'none'}")

    # B6 (R3 part): bandits scatter, Monolith holds
    parts, ok_all = [], True
    for squad, expect_break in (("bandit", True), ("monolith", False)):
        t_k = next((t for t, f in run.of("event") if f.get("squad") == squad), None)
        if t_k is None:
            parts.append(f"{squad}: no kills")
            ok_all = False
            continue
        broke = [f.get("role") for t, f in run.of("morale")
                 if f.get("comm") == squad and t_k <= t <= t_k + 10 and f.get("level") in ("0", "5")]
        retreats = sum(run.total(k, squad) or 0 for k in ("m_retreat_orderly", "m_retreat_scatter"))
        scatter = run.total("m_retreat_scatter", squad) or 0
        ok = (bool(broke) or scatter >= 1) if expect_break else (not broke and retreats == 0)
        ok_all &= ok
        parts.append(f"{squad}: broke within 10 s {broke or 'none'}, scatter {scatter:.0f}, retreats {retreats:.0f}")
    table.add("B6", name, verdict(ok_all), "; ".join(parts))


# ------------------------------------------------------------------------------------------------------------------
# R4 ai_save
# ------------------------------------------------------------------------------------------------------------------
def search_ends(run, t_from):
    """Per searcher the time its search ended for good after t_from (its last "searching value=0" with no later 1):
    the give-ups the probe saw, beside the engine's r_giveup_spread_ms."""
    ends = {}
    for t, f in run.of("searching"):
        if t < t_from:
            continue
        role = f.get("role", "-")
        ends[role] = t if f.get("value") == "0" else None
    return {role: t for role, t in ends.items() if t is not None}


def check_flash(run, table):
    """B1's muzzle flash from a run other than R1 (R4a holds the trial after its save): the criterion of check_r1, a
    muzzle_flash estimate of F by O within 1 s of F's first round, the round fired at night."""
    t_night = event_time(run, "night")
    f_spot = first(run.of("spot"), lambda t, f: f.get("name") == "F")[1]
    placed = next((t for t, e, f, rest in run.events if e == "error" and rest.startswith(("no_flash_pair",
                                                                                           "no_flash_spot"))), None)
    if t_night is None:
        table.add("B1", run.label, "NOT MEASURED", "no night muzzle-flash trial in this log")
        return
    f_where = f"F {f_spot.get('from_O')} m from O" if f_spot else ""
    f_shot = first(run.of("shot"), lambda t, f: f.get("role") == "F" and t >= t_night)[0]
    if f_shot is None:
        table.add("B1", run.label, "NOT MEASURED", "muzzle flash not measured: " + (
            f"F never fired ({f_where}): {gunner_note(run, 'F')}" if f_spot else
            "F not placed" + (" (no pair of spots)" if placed is not None else "")))
        return
    delay, text = flash_result(run, f_shot)
    ok = delay is not None and delay <= 1.0
    table.add("B1", run.label, verdict(ok), f"{text} ({f_where}, night from t={t_night:.1f}; "
              f"{flash_diagnosis(run, t_night)})")


def check_r4a(run, table):
    name = run.label
    check_area(run, table)
    check_flash(run, table)
    void = run.void()
    if void:
        table.add("P2", name, "VOID", void)
        table.add("B7", name, "VOID", void)
        return
    t_wall = next((t for t, f in run.of("event") if "from_T" in f), None)
    # A dead T is a corpse: the wall is judged before the death and the search not at all. Probes before K10 did not
    # log the death; a T that never reached the away spot (a corpse does not move) gives it away (r4a5, r4a6).
    t_dead = next((t for t, f in run.of("target_dead")), None)
    away = next((f for t, f in run.of("placed") if f.get("name") == "away"), None)
    stuck = away is not None and number(away.get("off"), 0) > AWAY_PLACED_M
    # A run that ended early (a crash: no done line) is judged only on the phases it finished.
    t_end = run.events[-1][0] if run.events else 0.0
    ended = None if run.done() is not None else f"the run ended at t={t_end:.1f} without the probe's done line"
    # A shooter that never had T in sight tests neither the wall nor the search: such a run is no evidence.
    saw = sorted({f.get("role") for t, f in run.of("see") if f.get("value") == "1"})
    if ended and (t_wall is None or t_end < t_wall + 10):
        table.add("P2", name, "NOT MEASURED", ended)
    elif t_wall is None:
        table.add("P2", name, "NOT MEASURED", "T never moved behind the wall")
    elif not saw:
        table.add("P2", name, "NOT MEASURED", "no shooter ever saw T")
    else:
        window = (t_wall, t_wall + 10 if t_dead is None else min(t_wall + 10, t_dead))
        stale = []
        merc_rounds = 0
        for t, f in run.of("shot"):
            if not window[0] <= t <= window[1] or f.get("target_see") != "0":
                continue
            gone = number(f.get("unseen_ms"), -1)
            if gone >= 0 and gone < WALL_STALE_MS:
                continue
            if f.get("comm") == "bandit":
                stale.append(f"{f.get('role')} {f.get('rounds')} rounds {gone:.0f} ms")
            else:
                merc_rounds += int(number(f.get("rounds"), 0))
        bandit_supp = run.total("t_shots_supp", "bandit") or 0
        merc_supp = run.total("t_shots_supp", "killer") or 0
        blind_hits = [f"{f.get('role')} {f.get('unseen_ms')} ms" for t, f in run.of("target_hit")
                      if window[0] <= t <= window[1] and f.get("see") == "0"
                      and not 0 <= number(f.get("unseen_ms"), -1) < WALL_STALE_MS]
        blind = run.total("k_blind_shot") or 0
        ok = not stale and bandit_supp == 0 and merc_rounds <= merc_supp and not blind_hits and blind == 0
        table.add("P2", name, verdict(ok),
                  f"bandit shots {WALL_STALE_MS} ms after losing T: {stale or 'none'}; bandit t_shots_supp="
                  f"{bandit_supp:.0f}; merc unseen rounds {merc_rounds} vs merc t_shots_supp={merc_supp:.0f}; hits on "
                  f"T unseen: {blind_hits or 'none'}; k_blind_shot={blind:.0f}; saw T: {', '.join(saw)}")

    spots = run.total("r_spots") or 0
    spread = run.total("r_giveup_spread_ms")
    pair = [number(f.get("d"), 0) for t, f in run.of("pair") if f.get("searching") == "1,1"]
    quiet = run.total("r_quiet", "killer") or 0
    barks = run.total("c_sent_voice", "bandit") or 0
    # Diagnosis (after K9): handovers to the search, and fights the planner left with the enemy still believed (the
    # "[ai] trace drop" lines of ai_trace say why).
    handovers = run.total("r_handover", "killer") or 0
    drops = run.total("r_drop", "killer") or 0
    pair_ok = bool(pair) and statistics.median(pair) <= 15
    ok = spots >= 2 and (spread is None or spread >= 3000) and pair_ok and quiet > 0 and barks > 0
    if ended and not stuck and t_dead is None:
        table.add("B7", name, "NOT MEASURED", ended)
        return
    if t_dead is not None or stuck:
        why = (f"T died at t={t_dead:.1f}" if t_dead is not None else
               f"T never reached the away spot (off={away.get('off')} m: dead, a corpse does not move)")
        table.add("B7", name, "VOID", f"{why}; r_handover={handovers:.0f}, r_drop={drops:.0f}")
        return
    # Information: the bandits' searches that ended after T went away (the give-ups the spread is made of).
    t_away = next((t for t, f in run.of("event") if "from_wall" in f), None)
    ended = search_ends(run, t_away) if t_away is not None else {}
    bandit_ends = sorted((t, role) for role, t in ended.items() if role.startswith("D"))
    ends_text = ", ".join(f"{role} {t:.1f}" for t, role in bandit_ends) or "none"
    if len(bandit_ends) >= 2:
        ends_text += f" (spread {bandit_ends[-1][0] - bandit_ends[0][0]:.1f} s)"
    table.add("B7", name, verdict(ok) if run.periods and saw else "NOT MEASURED",
              f"r_spots={spots:.0f}, r_giveup_spread_ms={fmt(spread, 0)}, merc pair median distance while both "
              f"search {fmt(statistics.median(pair) if pair else None)} m, mercs r_quiet={quiet:.0f}, "
              f"r_handover={handovers:.0f}, r_drop={drops:.0f}, bandit voice messages {barks:.0f}; bandit searches "
              f"ended after T went away: {ends_text}")


def finals(run):
    return {f.get("npc"): f for t, f in run.of("final") if f.get("target") == "T"}


def check_r4b(before, after, table):
    name = "R4"
    if after is None:
        table.add("P8", name, "NOT MEASURED", "no R4b run")
        return
    if before is None:
        table.add("P8", name, "NOT MEASURED", "no R4a run")
        return
    problems = []
    loaded = after.done() == "complete"
    if not loaded:
        problems.append(f"R4b ended: {after.done()}")
    a, b = finals(before), finals(after)
    with_belief = [npc for npc, f in a.items() if f.get("src") != "none"]
    for npc, f in a.items():
        g = b.get(npc)
        if g is None:
            problems.append(f"{f.get('role')} missing after the load")
            continue
        if f.get("radio") != g.get("radio"):
            problems.append(f"{f.get('role')} radio {f.get('radio')} -> {g.get('radio')}")
        if f.get("src") == "none" and g.get("src") != "none":
            problems.append(f"{f.get('role')} gained {g.get('src')}")
        if f.get("src") != "none" and g.get("src") == "none":
            problems.append(f"{f.get('role')} lost its belief")
    restored = after.total("k_restored") or 0
    if restored < len(with_belief):
        problems.append(f"k_restored {restored:.0f} < {len(with_belief)} NPCs with a belief on T")
    table.add("P8", name, verdict(not problems),
              "; ".join(problems) or f"{len(a)} NPCs: beliefs and radios equal after the load, k_restored="
              f"{restored:.0f}")


# ------------------------------------------------------------------------------------------------------------------
# R6 ai_newgame
# ------------------------------------------------------------------------------------------------------------------
def check_r6(run, table, ltx):
    """R6: the new game leaves fake_start; profile_default only for communities without a profile section."""
    name = run.label
    finished = run.done() or ""
    level = next((f.get("level") for t, f in run.of("newgame")), None)
    if not level or level == "fake_start":
        verdict_text = "FAIL" if finished.startswith("fail") else "NOT MEASURED"
        table.add("R6", name, verdict_text, f"no start level reached ({finished or 'probe never printed done'})")
        return
    profiles = run.of("profile")
    wrong, defaulted = [], []
    for community in run.communities():
        count = run.total("profile_default", community) or 0
        if not count:
            continue
        target = ltx.get("ai_community_alias", community) or community
        (wrong if f"ai_profile_{target}" in ltx.sections else defaulted).append(f"{community}:{count:.0f}")
    ok = bool(profiles) and not wrong
    table.add("R6", name, verdict(ok),
              f"start level {level}; {len(profiles)} online stalkers with a profile; profile_default for "
              f"communities with a section: {', '.join(wrong) or 'none'}; "
              f"without one: {', '.join(defaulted) or 'none'}")


# ------------------------------------------------------------------------------------------------------------------
def log_of(path):
    path = Path(path)
    return path / "log.txt" if path.is_dir() else path


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runs", nargs="+", help="run folders (with log.txt) or log files")
    parser.add_argument("--ltx", default=str(DEFAULT_LTX), help="configs/dead_air_x64_ai.ltx")
    parser.add_argument("--out", help="also write the table (markdown) here")
    args = parser.parse_args()

    ltx = Ltx(args.ltx)
    table = Table()
    runs = []
    for item in args.runs:
        path = log_of(item)
        if not path.is_file():
            table.add("-", str(item), "NOT MEASURED", "no log")
            continue
        runs.append(Run(path))

    r4a = r4b = None
    for run in runs:
        check_common(run, table)
        if run.scenario == "ai_fair":
            check_r1(run, table, ltx)
        elif run.scenario == "ai_factions":
            check_r2(run, table, ltx)
        elif run.scenario == "ai_actor":
            check_r3(run, table, ltx)
        elif run.scenario == "ai_newgame":
            check_r6(run, table, ltx)
        elif run.scenario == "ai_save":
            if run.phase == "b":
                r4b = run
            else:
                r4a = run
                check_r4a(run, table)
    if r4a or r4b:
        check_r4b(r4a, r4b, table)

    text = table.markdown()
    print(text)
    if args.out:
        Path(args.out).write_text(text + "\n", encoding="utf-8")
    return 1 if table.failed() else 0


if __name__ == "__main__":
    sys.exit(main())
