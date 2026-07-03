#!/usr/bin/env python3
"""
bot_scorecard.py — Honest activity scorecard for the playerbot fleet.

Reads logs/bot_events.csv and produces numbers that separate REAL play from
FAKE work, so AFK behavior cannot disguise itself as progress. Designed to be
run after each server session and diffed against the previous run.

Core idea: a "task" only counts as real if it produced concrete state change
(position moved toward a goal, XP/level gained, quest progressed, loot taken,
a spell cast, a combat swing). Everything else is churn — and churn is the
thing we are trying to kill.

Usage:
  python3 tools/bot_scorecard.py [path/to/bot_events.csv]
  python3 tools/bot_scorecard.py --save logs/scorecards/run.json
  python3 tools/bot_scorecard.py --compare logs/scorecards/prev.json
"""
import sys, csv, json, argparse, os
from collections import defaultdict, Counter

# Events that represent the bot actually playing the game.
PRODUCTIVE = {
    "BotStatsSnapshot",            # neutral sampler (parsed separately)
}
# Events that exist only because a bot was stuck / being rescued / re-planning.
# High counts here = the fleet is thrashing instead of playing.
CHURN_PREFIXES = (
    "HardIdle", "IdleNoConcreteWork", "IdleRescue", "StaleTarget",
    "AssignedTravelNoMove", "ForcedTravelMove", "ExpiredTravelStale",
    "TravelStaleAttackers", "TravelCombatInterrupt", "TravelCooldown",
    "CombatStateNoWorkExit", "NoWorkGrind", "AttackForceChase",
    "ConcreteTargetWork", "TargetFiller", "RespawnAccel", "RepathNudge",
    "SpellCastDeferred", "QuestRescue", "QuestInactiveFallback",
    "DecisionLoop", "VisibleBotStuck", "VisibleBotLowApm",
    # failed/spun work — must count as churn so a fix that merely SPAMS a
    # rejected action cannot masquerade as a churn reduction:
    "ReviveFromCorpseStillDead", "DeadCorpseReclaimFailed",
    "DeadCorpseReclaimRetrySuppressed", "DeadCorpseProgressStall",
    "DeadCorpseRepathNudge", "AttackCurrentTargetRetry",
)

def is_churn(name):
    return any(name.startswith(p) for p in CHURN_PREFIXES)

def parse_kv(s):
    d = {}
    for tok in s.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d

def num(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default

def analyze(path):
    event_counts = Counter()
    snaps = 0
    # honest activity buckets, counted over BotStatsSnapshot rows
    truly_idle = 0          # moving=0 combat=0 casting=0  -> visibly AFK
    fake_busy = 0           # moving/combat/casting=1 BUT no progress > 30s -> looks busy, isn't
    real_active = 0         # active AND made progress recently
    dead = 0
    ghost = 0
    has_target_idle = 0     # target set but moving=combat=casting=0 (the "stares at boar")
    since_progress = []
    apm = []
    levels = {}             # bot -> (first_level, last_level, first_xp_total, last_xp_total)
    money = {}              # bot -> (first, last)
    quest_complete_seen = defaultdict(int)

    with open(path, newline="", errors="replace") as f:
        r = csv.reader(f)
        for row in r:
            if len(row) < 9:
                continue
            name, event = row[1], row[2]
            event_counts[event] += 1
            if event != "BotStatsSnapshot":
                continue
            snaps += 1
            kv = parse_kv(row[8])
            mv = kv.get("moving") == "1"
            cb = kv.get("combat") == "1"
            ca = kv.get("casting") == "1"
            sp = num(kv.get("sinceProgressSec"))
            since_progress.append(sp)
            apm.append(num(kv.get("apm")))
            if kv.get("dead") == "1": dead += 1
            if kv.get("ghost") == "1": ghost += 1
            active = mv or cb or ca
            if not active:
                truly_idle += 1
                if kv.get("target", "none") != "none":
                    has_target_idle += 1
            elif sp > 30:
                fake_busy += 1
            else:
                real_active += 1
            # progress tracking per bot
            lvl = int(num(kv.get("level")))
            xp = int(num(kv.get("xp")))
            nextxp = int(num(kv.get("nextXp")))
            xp_total = lvl * 1_000_000 + xp  # monotonic-ish proxy for ranking progress
            if name not in levels:
                levels[name] = [lvl, lvl, xp_total, xp_total]
            else:
                levels[name][1] = lvl
                levels[name][3] = xp_total
            m = int(num(kv.get("money")))
            if name not in money:
                money[name] = [m, m]
            else:
                money[name][1] = m
            qc = int(num(kv.get("questRewardReady")))

    def pct(n): return round(100.0 * n / snaps, 1) if snaps else 0.0
    sp_sorted = sorted(since_progress)
    def p(q):
        if not sp_sorted: return 0
        return round(sp_sorted[min(len(sp_sorted)-1, int(q*len(sp_sorted)))], 1)

    leveled_up = sum(1 for v in levels.values() if v[1] > v[0])
    xp_gained = sum(1 for v in levels.values() if v[3] > v[2])
    total_churn = sum(c for e, c in event_counts.items() if is_churn(e))
    total_events = sum(event_counts.values())

    return {
        "file": path,
        "snapshots": snaps,
        "distinct_bots": len(levels),
        "activity_pct": {
            "real_active": pct(real_active),
            "fake_busy_no_progress": pct(fake_busy),
            "truly_idle": pct(truly_idle),
            "idle_with_target_set": pct(has_target_idle),
            "dead": pct(dead),
            "ghost": pct(ghost),
        },
        "since_progress_sec": {"p50": p(0.5), "p90": p(0.9), "p99": p(0.99)},
        "apm_avg": round(sum(apm)/len(apm), 1) if apm else 0,
        "progress": {
            "bots_leveled_up": leveled_up,
            "bots_gained_xp": xp_gained,
            "bots_seen": len(levels),
        },
        "churn": {
            "churn_events": total_churn,
            "total_events": total_events,
            "churn_pct_of_all_events": round(100.0*total_churn/total_events, 1) if total_events else 0,
        },
        "top_events": event_counts.most_common(20),
    }

def fmt(s):
    a = s["activity_pct"]
    out = []
    out.append(f"=== BOT SCORECARD: {s['file']} ===")
    out.append(f"snapshots={s['snapshots']}  distinct_bots={s['distinct_bots']}  apm_avg={s['apm_avg']}")
    out.append("")
    out.append("ACTIVITY (% of snapshots):")
    out.append(f"  real_active (acting + progressing)   : {a['real_active']}%   <- want this HIGH")
    out.append(f"  fake_busy (acting, NO progress >30s) : {a['fake_busy_no_progress']}%   <- want this LOW (AFK in disguise)")
    out.append(f"  truly_idle (not moving/combat/cast)  : {a['truly_idle']}%   <- want this LOW")
    out.append(f"    ...of which had a target selected  : {a['idle_with_target_set']}%   <- the 'stares at boar'")
    out.append(f"  dead={a['dead']}%  ghost={a['ghost']}%")
    out.append("")
    sp = s["since_progress_sec"]
    out.append(f"TIME SINCE REAL PROGRESS (sec):  p50={sp['p50']}  p90={sp['p90']}  p99={sp['p99']}")
    pr = s["progress"]
    out.append(f"PROGRESS:  leveled_up={pr['bots_leveled_up']}  gained_xp={pr['bots_gained_xp']}  of {pr['bots_seen']} bots")
    c = s["churn"]
    out.append(f"CHURN:  {c['churn_events']} rescue/replan events = {c['churn_pct_of_all_events']}% of ALL bot events  <- want this LOW")
    out.append("")
    out.append("TOP EVENTS:")
    for e, n in s["top_events"]:
        tag = " [churn]" if is_churn(e) else ""
        out.append(f"  {n:>8}  {e}{tag}")
    return "\n".join(out)

def diff(cur, prev):
    out = ["", "=== DELTA vs previous run ==="]
    def d(label, a, b, good_up=True):
        delta = round(a - b, 1)
        arrow = "↑" if delta > 0 else ("↓" if delta < 0 else "=")
        verdict = ""
        if delta != 0:
            improved = (delta > 0) == good_up
            verdict = "  BETTER" if improved else "  WORSE"
        out.append(f"  {label}: {b} -> {a}  ({arrow}{abs(delta)}){verdict}")
    d("real_active%", cur["activity_pct"]["real_active"], prev["activity_pct"]["real_active"], True)
    d("fake_busy%", cur["activity_pct"]["fake_busy_no_progress"], prev["activity_pct"]["fake_busy_no_progress"], False)
    d("truly_idle%", cur["activity_pct"]["truly_idle"], prev["activity_pct"]["truly_idle"], False)
    d("idle_with_target%", cur["activity_pct"]["idle_with_target_set"], prev["activity_pct"]["idle_with_target_set"], False)
    d("churn% of events", cur["churn"]["churn_pct_of_all_events"], prev["churn"]["churn_pct_of_all_events"], False)
    d("p90 since_progress", cur["since_progress_sec"]["p90"], prev["since_progress_sec"]["p90"], False)
    d("apm_avg", cur["apm_avg"], prev["apm_avg"], True)
    return "\n".join(out)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="?", default="logs/bot_events.csv")
    ap.add_argument("--save", help="write scorecard JSON to this path")
    ap.add_argument("--compare", help="diff against a previously saved scorecard JSON")
    args = ap.parse_args()

    s = analyze(args.csv)
    print(fmt(s))
    if args.compare and os.path.exists(args.compare):
        with open(args.compare) as f:
            print(diff(s, json.load(f)))
    if args.save:
        os.makedirs(os.path.dirname(args.save) or ".", exist_ok=True)
        with open(args.save, "w") as f:
            json.dump(s, f, indent=2)
        print(f"\nsaved -> {args.save}")
