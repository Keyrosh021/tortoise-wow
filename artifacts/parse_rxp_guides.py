#!/usr/bin/env python3
"""
parse_rxp_guides.py -- Parse RestedXP (RXPGuides) leveling guide files into
tw_world.ai_playerbot_leveling_step for playerbot quest routing.

Scope: vanilla 1.12 (Turtle WoW). Parses the Classic-*.lua guide files plus
Era.lua (the classic-era 21-60 continuation guides that the Classic-* #next
chains point into). Every guide must carry the '#classic' header tag; guides
gated to tbc/wotlk/sod/som are dropped. Evaluation context:
  game = CLASSIC/ERA, season = 0 (no SoD), xprate = 1.0, softcore (not
  hardcore, not SSF), gender ignored, numeric level gates treated as met.

Output rows: guide_name, faction(A/H/both), race_mask, class_mask,
min_level, max_level, step_no, seq_no, action(accept|turnin|complete|goto|
grind), quest_id, objective_idx, zone_name, x_pct, y_pct, detail(mob name),
next_guide.  Masks: 0 = all (relative to row faction).

Usage:
  python3 parse_rxp_guides.py [--guides-dir DIR] [--load] [--summary]
    --load     create/replace table and load rows via mysql CLI
    --summary  print coverage summary (implied by --load)
Without --load it does a dry-run parse + summary.
"""

import argparse
import itertools
import os
import re
import subprocess
import sys
from collections import OrderedDict, defaultdict

GUIDES_DIR = "/home/zuppier/tw/server_dev/bot_guides/RXPGuides/Guides"
MYSQL = ["mysql", "-h127.0.0.1", "-uroot", "-pmangos", "tw_world"]

# ---------------------------------------------------------------- constants

RACES = {  # vanilla race masks (ChrRaces)
    "Human": 1, "Orc": 2, "Dwarf": 4, "NightElf": 8,
    "Undead": 16, "Scourge": 16, "Tauren": 32, "Gnome": 64, "Troll": 128,
}
RACE_FACTION = {
    "Human": "A", "Dwarf": "A", "NightElf": "A", "Gnome": "A",
    "Orc": "H", "Undead": "H", "Tauren": "H", "Troll": "H",
}
CLASSES = {  # vanilla class masks (ChrClasses)
    "WARRIOR": 1, "PALADIN": 2, "HUNTER": 4, "ROGUE": 8, "PRIEST": 16,
    "SHAMAN": 64, "MAGE": 128, "WARLOCK": 256, "DRUID": 1024,
}
# valid vanilla 1.12 race/class combinations
VALID_COMBOS = frozenset(
    (r, c) for r, cs in {
        "Human":    ("WARRIOR", "PALADIN", "ROGUE", "PRIEST", "MAGE", "WARLOCK"),
        "Dwarf":    ("WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST"),
        "NightElf": ("WARRIOR", "HUNTER", "ROGUE", "PRIEST", "DRUID"),
        "Gnome":    ("WARRIOR", "ROGUE", "MAGE", "WARLOCK"),
        "Orc":      ("WARRIOR", "HUNTER", "ROGUE", "SHAMAN", "WARLOCK"),
        "Undead":   ("WARRIOR", "ROGUE", "PRIEST", "MAGE", "WARLOCK"),
        "Tauren":   ("WARRIOR", "HUNTER", "SHAMAN", "DRUID"),
        "Troll":    ("WARRIOR", "HUNTER", "ROGUE", "PRIEST", "SHAMAN", "MAGE"),
    }.items() for c in cs
)
ALL_RACES = frozenset(RACES) - {"Scourge"}
ALL_CLASSES = frozenset(CLASSES)

# version/mode tokens: True = applies on vanilla Turtle, False = does not
MODE_TOKENS = {
    "CLASSIC": True, "ERA": True, "VANILLA": True,
    "SOM": False, "SOD": False, "SEASONAL": False,
    "TBC": False, "WOTLK": False, "CATA": False, "MOP": False,
    "MISTS": False, "RETAIL": False, "DF": False, "TWW": False,
    "HARDCORE": False, "HC": False, "SOFTCORE": True, "SSF": False,
    "SKIP": False,
    "MALE": True, "FEMALE": True,   # gender irrelevant for routing
    "AH": True,
}

# classic uiMapID -> zone name (used by numeric .goto lines)
UIMAP = {
    1411: "Durotar", 1412: "Mulgore", 1413: "The Barrens", 1414: "Kalimdor",
    1415: "Eastern Kingdoms", 1416: "Alterac Mountains",
    1417: "Arathi Highlands", 1418: "Badlands", 1419: "Blasted Lands",
    1420: "Tirisfal Glades", 1421: "Silverpine Forest",
    1422: "Western Plaguelands", 1423: "Eastern Plaguelands",
    1424: "Hillsbrad Foothills", 1425: "The Hinterlands", 1426: "Dun Morogh",
    1427: "Searing Gorge", 1428: "Burning Steppes", 1429: "Elwynn Forest",
    1430: "Deadwind Pass", 1431: "Duskwood", 1432: "Loch Modan",
    1433: "Redridge Mountains", 1434: "Stranglethorn Vale",
    1435: "Swamp of Sorrows", 1436: "Westfall", 1437: "Wetlands",
    1438: "Teldrassil", 1439: "Darkshore", 1440: "Ashenvale",
    1441: "Thousand Needles", 1442: "Stonetalon Mountains", 1443: "Desolace",
    1444: "Feralas", 1445: "Dustwallow Marsh", 1446: "Tanaris",
    1447: "Azshara", 1448: "Felwood", 1449: "Un'Goro Crater",
    1450: "Moonglade", 1451: "Silithus", 1452: "Winterspring",
    1453: "Stormwind City", 1454: "Orgrimmar", 1455: "Ironforge",
    1456: "Thunder Bluff", 1457: "Darnassus", 1458: "Undercity",
}
ZONE_ALIASES = {"StormwindClassic": "Stormwind City"}

XPRATE = 1.0

STATS = defaultdict(int)


# ------------------------------------------------------------ gate handling

def gate_combos(expr):
    """Evaluate a '<<' gate expression against every valid vanilla
    (race, class) combo.  Returns the frozenset of combos it applies to.
    Semantics (from RXPGuides GuideLoader.applies): '/' separates OR
    clauses; whitespace-separated tokens inside a clause are ANDed;
    '!' negates a token."""
    expr = expr.split("--")[0].strip()
    if not expr:
        return VALID_COMBOS
    result = set()
    clauses = [c for c in expr.split("/") if c.strip()]
    for race, cls in VALID_COMBOS:
        ok_any = False
        for clause in clauses:
            ok = True
            for tok in re.findall(r"!?[A-Za-z0-9']+", clause):
                neg = tok.startswith("!")
                if neg:
                    tok = tok[1:]
                up = tok.upper()
                if tok in ("Alliance", "Horde"):
                    val = RACE_FACTION[race] == ("A" if tok == "Alliance" else "H")
                elif tok in RACES:
                    val = (race == tok) or (tok == "Scourge" and race == "Undead") \
                        or (tok == "Undead" and race == "Undead")
                elif up in CLASSES:
                    val = cls == up
                elif re.fullmatch(r"\d+", tok):
                    val = True          # level gate: treat as satisfied
                elif up in MODE_TOKENS:
                    val = MODE_TOKENS[up]
                else:
                    val = False         # unknown token -> does not apply
                if val == neg:
                    ok = False
                    break
            if ok:
                ok_any = True
                break
        if ok_any:
            result.add((race, cls))
    return frozenset(result)


def combos_to_masks(combos):
    """(faction, race_mask, class_mask) for a combo set; mask 0 == all
    relative to the resulting faction."""
    races = {r for r, _ in combos}
    classes = {c for _, c in combos}
    factions = {RACE_FACTION[r] for r in races}
    faction = "both" if len(factions) != 1 else factions.pop()
    if faction == "A":
        frange = {r for r in ALL_RACES if RACE_FACTION[r] == "A"}
    elif faction == "H":
        frange = {r for r in ALL_RACES if RACE_FACTION[r] == "H"}
    else:
        frange = ALL_RACES
    race_mask = 0 if races >= frange else sum(RACES[r] for r in races)
    fclasses = {c for r, c in VALID_COMBOS if r in frange}
    class_mask = 0 if classes >= fclasses else sum(CLASSES[c] for c in classes)
    return faction, race_mask, class_mask


def xprate_ok(expr):
    """Evaluate an #xprate expression ('<1.99', '>1.49', '1.49-1.99') at 1x."""
    expr = expr.split("--")[0].strip()
    m = re.match(r"([<>]=?)\s*([\d.]+)", expr)
    if m:
        op, val = m.group(1), float(m.group(2))
        return {"<": XPRATE < val, "<=": XPRATE <= val,
                ">": XPRATE > val, ">=": XPRATE >= val}[op]
    m = re.match(r"([\d.]+)\s*-\s*([\d.]+)", expr)
    if m:
        return float(m.group(1)) <= XPRATE <= float(m.group(2))
    return True


def season_ok(expr):
    """#season list -- true iff season 0 (plain vanilla/era) is included."""
    nums = re.findall(r"\d+", expr.split("--")[0])
    return "0" in nums


# step-level '#tag' conditions: return True/False/None(ignore)
def step_tag_applies(tag, arg):
    if tag == "xprate":
        return xprate_ok(arg)
    if tag == "season":
        return season_ok(arg)
    if tag in ("era",):          # era-only content: Turtle ~ era
        return True
    if tag in ("som", "phase", "hardcore", "hardcoreserver", "ssf"):
        return False
    if tag in ("softcore", "sofcore", "softcoreserver"):
        return True
    return None                  # completewith/label/loop/requires/... ignore


# --------------------------------------------------------------- extraction

GOTO_RE = re.compile(r"\.goto\s+([^,]+?)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)")
ACCEPT_RE = re.compile(r"\.accept\s+(\d+)")
ACCEPTM_RE = re.compile(r"\.acceptmultiple\s+([\d,\s]+)")
TURNIN_RE = re.compile(r"\.turnin\s+(\d+)")
COMPLETE_RE = re.compile(r"\.complete\s+(\d+)\s*(?:,\s*(\d+))?")
MOB_RE = re.compile(r"\.(?:mob|unitscan)\s+\+?(.+?)\s*$")


def norm_zone(z):
    z = z.strip()
    m = re.fullmatch(r"(\d+)(?:/\d+)?", z)
    if m:
        return UIMAP.get(int(m.group(1)))
    z = ZONE_ALIASES.get(z, z)
    # tolerate '#era/som' style suffixes never seen on zones; keep as-is
    return z


class Guide:
    def __init__(self):
        self.name = None
        self.group = ""
        self.next = []            # list of next-guide names (gate-filtered)
        self.combos = VALID_COMBOS
        self.min_level = 0
        self.max_level = 0
        self.classic = False
        self.skip_reason = None
        self.rows = []            # dict rows


def parse_header_line(line, guide):
    line = line.strip()
    # separate trailing gate on tag lines:  '#tag value << gate'
    gate = None
    m = re.match(r"(.*?)\s*<<\s*(.+)$", line)
    if m and line.startswith("#"):
        line, gate = m.group(1).strip(), m.group(2)
    if line.startswith("<<"):
        guide.combos = guide.combos & gate_combos(line[2:])
        return
    m = re.match(r"#(\w+)\s*(.*)$", line)
    if not m:
        return
    tag, arg = m.group(1), m.group(2).strip()
    if gate is not None and not gate_combos(gate):
        return  # tag line gated away for everyone
    if tag == "name":
        guide.name = arg.split("--")[0].strip()
        lm = re.match(r"(\d{1,2})-(\d{1,2})\b", guide.name)
        if not lm:  # tolerate typo'd names like '41-400_41-41 Swamp of Sorrows'
            for cand in re.finditer(r"(\d{1,2})-(\d{1,2})\b", guide.name):
                if 1 <= int(cand.group(1)) <= int(cand.group(2)) <= 60:
                    lm = cand
                    break
        if lm:
            guide.min_level = int(lm.group(1))
            guide.max_level = int(lm.group(2))
    elif tag == "group":
        guide.group = arg.split("--")[0].strip()
    elif tag == "next":
        if gate is None or gate_combos(gate):
            for cand in arg.split(";"):
                cand = cand.split("--")[0].strip()
                if "\\" in cand:                 # 'Group\GuideName'
                    cand = cand.split("\\")[-1].strip()
                if cand and cand not in guide.next:
                    guide.next.append(cand)
    elif tag == "classic":
        guide.classic = True
    elif tag == "xprate":
        if not xprate_ok(arg):
            guide.skip_reason = "guide #xprate excludes 1x"
    elif tag == "season":
        if not season_ok(arg):
            guide.skip_reason = "guide #season excludes vanilla"


def parse_guide(block, file_faction):
    """Parse one RegisterGuide([[...]]) body -> Guide."""
    lines = block.splitlines()
    guide = Guide()
    # ---- split header / steps
    step_starts = [i for i, l in enumerate(lines)
                   if re.match(r"^step\b", l.strip()) or l.strip() == "step"]
    header_end = step_starts[0] if step_starts else len(lines)
    for l in lines[:header_end]:
        if l.strip():
            parse_header_line(l, guide)

    if not guide.classic:
        guide.skip_reason = guide.skip_reason or "no #classic tag"
    if file_faction and guide.combos is VALID_COMBOS:
        guide.combos = gate_combos(file_faction)  # faction from file wrapper
    elif file_faction:
        guide.combos = guide.combos & gate_combos(file_faction)
    if not guide.combos:
        guide.skip_reason = guide.skip_reason or "guide gate never applies"
    if guide.skip_reason:
        return guide

    # ---- steps
    step_no = 0
    seq_no = 0
    for si, start in enumerate(step_starts):
        end = step_starts[si + 1] if si + 1 < len(step_starts) else len(lines)
        step_no += 1
        head = lines[start].strip()
        combos = guide.combos
        m = re.match(r"^step\s*<<\s*(.+)$", head)
        if m:
            combos = combos & gate_combos(m.group(1))
        if not combos:
            STATS["steps_gated_out"] += 1
            continue
        body = lines[start + 1:end]

        # first pass: step tags may kill the step or shrink its combos
        skip_step = False
        for raw in body:
            s = raw.strip()
            tm = re.match(r"#(\w+)\s*(.*)$", s)
            if not tm:
                continue
            tag, arg = tm.group(1), tm.group(2)
            gate = None
            gm = re.match(r"(.*?)\s*<<\s*(.+)$", arg)
            if gm:
                arg, gate = gm.group(1), gm.group(2)
            applies = step_tag_applies(tag, arg)
            if applies is False:
                if gate:  # only the gated audience loses the step
                    combos = combos - gate_combos(gate)
                else:
                    skip_step = True
                    break
        if skip_step or not combos:
            STATS["steps_gated_out"] += 1
            continue

        # second pass: extract actions in order
        last_goto = None            # (zone, x, y)
        pending = []                # rows awaiting coords (action before .goto)
        for raw in body:
            s = raw.strip()
            if not s or s.startswith("#") or s.startswith(">>") \
               or s.startswith("+") or s.startswith("--"):
                continue
            line_combos = combos
            gm = re.match(r"(.*?)\s*<<\s*(.+)$", s)
            if gm:
                s = gm.group(1)
                line_combos = combos & gate_combos(gm.group(2))
                if not line_combos:
                    STATS["lines_gated_out"] += 1
                    continue

            def emit(action, quest_id=None, obj=None, zone=None,
                     x=None, y=None, detail=None, defer_coords=False):
                nonlocal seq_no
                seq_no += 1
                faction, rmask, cmask = combos_to_masks(line_combos)
                row = dict(step_no=step_no, seq_no=seq_no, action=action,
                           quest_id=quest_id, objective_idx=obj,
                           zone_name=zone, x=x, y=y, detail=detail,
                           faction=faction, race_mask=rmask, class_mask=cmask)
                guide.rows.append(row)
                if defer_coords:
                    pending.append(row)

            gt = GOTO_RE.search(s)
            if gt:
                zone = norm_zone(gt.group(1))
                x, y = float(gt.group(2)), float(gt.group(3))
                if zone is None or not (0 <= x <= 100 and 0 <= y <= 100):
                    STATS["gotos_skipped_coords"] += 1
                    continue
                last_goto = (zone, x, y)
                for row in pending:      # backfill actions listed before goto
                    if row["zone_name"] is None:
                        row["zone_name"], row["x"], row["y"] = zone, x, y
                pending = []
                emit("goto", zone=zone, x=x, y=y)
                continue

            am = ACCEPTM_RE.search(s) if ".acceptmultiple" in s else None
            if am:
                for qid in re.findall(r"\d+", am.group(1)):
                    z, x, y = last_goto or (None, None, None)
                    emit("accept", quest_id=int(qid), zone=z, x=x, y=y,
                         defer_coords=last_goto is None)
                continue
            m2 = ACCEPT_RE.search(s)
            if m2 and ".acceptmultiple" not in s:
                z, x, y = last_goto or (None, None, None)
                emit("accept", quest_id=int(m2.group(1)), zone=z, x=x, y=y,
                     defer_coords=last_goto is None)
                continue
            m2 = TURNIN_RE.search(s)
            if m2:
                z, x, y = last_goto or (None, None, None)
                emit("turnin", quest_id=int(m2.group(1)), zone=z, x=x, y=y,
                     defer_coords=last_goto is None)
                continue
            m2 = COMPLETE_RE.search(s)
            if m2:
                z, x, y = last_goto or (None, None, None)
                emit("complete", quest_id=int(m2.group(1)),
                     obj=int(m2.group(2)) if m2.group(2) else None,
                     zone=z, x=x, y=y, defer_coords=last_goto is None)
                continue
            m2 = MOB_RE.search(s)
            if m2 and re.match(r"\.(mob|unitscan)\b", s):
                z, x, y = last_goto or (None, None, None)
                name = m2.group(1).split("--")[0].strip()
                emit("grind", zone=z, x=x, y=y, detail=name[:120],
                     defer_coords=last_goto is None)
                continue
    return guide


def parse_file(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    file_faction = None
    m = re.search(r'if\s+faction\s*==\s*"(Alliance|Horde)"\s*then\s*return',
                  text)
    if m:  # file returns early for that faction -> guides are for the other
        file_faction = "Horde" if m.group(1) == "Alliance" else "Alliance"
    guides = []
    for gm in re.finditer(r"RegisterGuide\(\[\[(.*?)\]\]", text, re.S):
        guides.append(parse_guide(gm.group(1), file_faction))
    return guides


# ------------------------------------------------------------------ loading

DDL = """
DROP TABLE IF EXISTS ai_playerbot_leveling_step;
CREATE TABLE ai_playerbot_leveling_step (
  id            INT UNSIGNED      NOT NULL AUTO_INCREMENT,
  guide_id      SMALLINT UNSIGNED NOT NULL,
  guide_name    VARCHAR(80)       NOT NULL,
  guide_group   VARCHAR(80)       NOT NULL DEFAULT '',
  faction       ENUM('A','H','both') NOT NULL DEFAULT 'both',
  race_mask     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  class_mask    SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  min_level     TINYINT UNSIGNED  NOT NULL DEFAULT 0,
  max_level     TINYINT UNSIGNED  NOT NULL DEFAULT 0,
  step_no       SMALLINT UNSIGNED NOT NULL,
  seq_no        SMALLINT UNSIGNED NOT NULL,
  action        ENUM('accept','turnin','complete','goto','grind') NOT NULL,
  quest_id      MEDIUMINT UNSIGNED NULL,
  objective_idx TINYINT UNSIGNED  NULL,
  zone_name     VARCHAR(48)       NULL,
  x_pct         DECIMAL(6,3)      NULL,
  y_pct         DECIMAL(6,3)      NULL,
  detail        VARCHAR(120)      NULL,
  next_guide    VARCHAR(200)      NULL,
  PRIMARY KEY (id),
  KEY idx_guide      (guide_id, seq_no),
  KEY idx_guide_name (guide_name),
  KEY idx_quest      (action, quest_id),
  KEY idx_level      (faction, min_level, max_level)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='RestedXP-derived leveling routes for playerbots (pct map coords)';
"""


def sql_str(v):
    if v is None:
        return "NULL"
    if isinstance(v, str):
        return "'" + v.replace("\\", "\\\\").replace("'", "\\'") + "'"
    return str(v)


def run_mysql(sql):
    p = subprocess.run(MYSQL, input=sql.encode(), capture_output=True)
    if p.returncode != 0:
        sys.exit("mysql error: " + p.stderr.decode()[:2000])
    return p.stdout.decode()


def fetch_quest_ids():
    out = subprocess.run(MYSQL + ["-N", "-e",
                                  "SELECT entry FROM quest_template"],
                         capture_output=True)
    if out.returncode != 0:
        sys.exit("mysql error: " + out.stderr.decode()[:2000])
    return {int(x) for x in out.stdout.split()}


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--guides-dir", default=GUIDES_DIR)
    ap.add_argument("--load", action="store_true")
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.guides_dir)
                   if re.match(r"Classic-.*\.lua$", f))
    files.append("Era.lua")  # classic-era 21-60 guides (#classic-tagged)

    guides = OrderedDict()   # (name, factionkey) -> Guide
    skipped_guides = []
    for f in files:
        for g in parse_file(os.path.join(args.guides_dir, f)):
            if g.skip_reason:
                skipped_guides.append((f, g.name, g.skip_reason))
                continue
            gf, _, _ = combos_to_masks(g.combos)
            key = (g.name, gf)
            if key in guides:
                skipped_guides.append((f, g.name, "duplicate name+faction"))
                continue
            g.file = f
            g.faction = gf
            guides[key] = g

    quest_ids = fetch_quest_ids()

    # quest-id hit rate
    ref_ids = set()
    for g in guides.values():
        for r in g.rows:
            if r["quest_id"] is not None:
                ref_ids.add(r["quest_id"])
    found_ids = ref_ids & quest_ids
    missing_ids = ref_ids - quest_ids

    rows_total = rows_loaded = rows_skipped_missing = 0
    inserts = []
    for gid, g in enumerate(guides.values(), 1):
        nxt = ";".join(g.next)[:200] or None
        for r in g.rows:
            rows_total += 1
            if r["quest_id"] is not None and r["quest_id"] not in quest_ids:
                rows_skipped_missing += 1
                continue
            rows_loaded += 1
            inserts.append("(%s)" % ",".join(sql_str(v) for v in (
                gid, g.name[:80], g.group[:80], r["faction"],
                r["race_mask"], r["class_mask"], g.min_level, g.max_level,
                r["step_no"], r["seq_no"], r["action"], r["quest_id"],
                r["objective_idx"], r["zone_name"] and r["zone_name"][:48],
                r["x"], r["y"], r["detail"], nxt)))

    if args.load:
        sql = [DDL]
        cols = ("guide_id,guide_name,guide_group,faction,race_mask,"
                "class_mask,min_level,max_level,step_no,seq_no,action,"
                "quest_id,objective_idx,zone_name,x_pct,y_pct,detail,"
                "next_guide")
        for i in range(0, len(inserts), 500):
            sql.append("INSERT INTO ai_playerbot_leveling_step (%s) VALUES\n%s;"
                       % (cols, ",\n".join(inserts[i:i + 500])))
        run_mysql("\n".join(sql))
        print("Loaded %d rows into tw_world.ai_playerbot_leveling_step"
              % rows_loaded)

    # ---------------- summary
    print("\n=== RXP guide parse summary (xprate=1.0, vanilla/era) ===")
    print("files parsed: %d   guides kept: %d   guides skipped: %d"
          % (len(files), len(guides), len(skipped_guides)))
    print("rows: %d parsed, %d loaded, %d dropped (quest id missing)"
          % (rows_total, rows_loaded, rows_skipped_missing))
    print("distinct quest ids referenced: %d   found in quest_template: %d "
          "(%.1f%%)   missing: %d"
          % (len(ref_ids), len(found_ids),
             100.0 * len(found_ids) / max(1, len(ref_ids)), len(missing_ids)))
    print("steps gated out: %d   lines gated out: %d   gotos skipped "
          "(bad zone/coords): %d" % (STATS["steps_gated_out"],
                                     STATS["lines_gated_out"],
                                     STATS["gotos_skipped_coords"]))

    # level coverage per faction
    for fac in ("A", "H"):
        lv = set()
        for g in guides.values():
            if g.min_level and g.faction in (fac, "both"):
                lv.update(range(g.min_level, g.max_level + 1))
        gaps = sorted(set(range(1, 61)) - lv)
        print("level coverage %s: %s%s"
              % (fac, compact(sorted(lv)),
                 ("   GAPS: " + compact(gaps)) if gaps else "   (full 1-60)"))

    zones = sorted({r["zone_name"] for g in guides.values() for r in g.rows
                    if r["zone_name"]})
    print("zones referenced (%d): %s" % (len(zones), ", ".join(zones)))
    if missing_ids:
        print("missing quest ids (%d): %s"
              % (len(missing_ids),
                 " ".join(str(i) for i in sorted(missing_ids))))
    if args.summary or not args.load:
        print("\nguides kept:")
        for g in guides.values():
            print("  [%s %2d-%2d] %-45s -> %s"
                  % (g.faction, g.min_level, g.max_level, g.name,
                     ";".join(g.next) or "-"))
        if skipped_guides:
            print("\nguides skipped:")
            for f, n, why in skipped_guides:
                print("  %-40s %-40s %s" % (f, n or "?", why))


def compact(nums):
    """[1,2,3,7,8] -> '1-3,7-8'"""
    if not nums:
        return "-"
    out, s, p = [], nums[0], nums[0]
    for n in nums[1:]:
        if n == p + 1:
            p = n
            continue
        out.append("%d-%d" % (s, p) if s != p else str(s))
        s = p = n
    out.append("%d-%d" % (s, p) if s != p else str(s))
    return ",".join(out)


if __name__ == "__main__":
    main()
