#!/usr/bin/env python3

import argparse
import csv
import json
import math
import re
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path


POINT_RE = re.compile(r"POINT\((-?\d+(?:\.\d+)?) (-?\d+(?:\.\d+)?)\)")
DETAIL_KEY_RE = re.compile(r"(?:^| )([A-Za-z_][A-Za-z0-9_]*)=")


@dataclass
class BotStats:
    first_ts: datetime | None = None
    last_ts: datetime | None = None
    first_level: int | None = None
    last_level: int | None = None
    events: Counter = field(default_factory=Counter)
    items_looted: Counter = field(default_factory=Counter)
    quest_items: Counter = field(default_factory=Counter)
    travel_targets: Counter = field(default_factory=Counter)
    movement_rejects: Counter = field(default_factory=Counter)
    combat_deferred: Counter = field(default_factory=Counter)
    loot_rejects: Counter = field(default_factory=Counter)
    rpg_suppressed: Counter = field(default_factory=Counter)
    positions_by_event: dict = field(default_factory=lambda: defaultdict(list))
    last_action: str | None = None
    same_action_streak_max: int = 0
    stat_samples: int = 0
    moving_samples: int = 0
    dead_samples: int = 0
    ghost_samples: int = 0
    low_apm_samples: int = 0
    max_since_progress_sec: int = 0
    first_xp: int | None = None
    last_xp: int | None = None
    first_money: int | None = None
    last_money: int | None = None
    last_quests: int | None = None
    last_quest_complete: int | None = None
    last_quest_reward_ready: int | None = None


def parse_timestamp(value):
    value = value.replace("Z", "+00:00")
    if re.search(r"[+-]\d{2}$", value):
        value += ":00"
    return datetime.fromisoformat(value)


def parse_point(value):
    match = POINT_RE.match(value or "")
    if not match:
        return None
    return float(match.group(1)), float(match.group(2))


def parse_detail(value):
    result = {}
    value = value or ""
    matches = list(DETAIL_KEY_RE.finditer(value))
    for index, match in enumerate(matches):
        key = match.group(1)
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(value)
        result[key] = value[start:end].strip().strip('"')
    return result


def detail_value(kv, key, default=""):
    value = kv.get(key)
    if value is None or value == "":
        return default
    return value


def parse_int(value, default=-1):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def point_from_kv(kv, prefix="end"):
    try:
        return float(kv[f"{prefix}X"]), float(kv[f"{prefix}Y"])
    except (KeyError, TypeError, ValueError):
        return None


def end_position_key(kv):
    map_id = detail_value(kv, "endMap", "?")
    x = detail_value(kv, "endX", "?")
    y = detail_value(kv, "endY", "?")
    z = detail_value(kv, "endZ", "?")
    return f"{map_id}:{x}:{y}:{z}"


def target_key(kv, arg):
    target = detail_value(kv, "target", arg or "unknown")
    entry = detail_value(kv, "entry", "")
    quest_id = detail_value(kv, "questId", "")
    if entry:
        target = f"{target}#{entry}"
    if quest_id:
        target = f"{target} quest={quest_id}"
    return target


def read_bot_events(path):
    rows = []
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.reader(handle)
        for line_no, row in enumerate(reader, start=1):
            if len(row) < 9:
                continue
            try:
                ts = parse_timestamp(row[0])
                level = int(float(row[6]))
            except (ValueError, TypeError):
                continue
            rows.append(
                {
                    "line": line_no,
                    "ts": ts,
                    "bot": row[1],
                    "event": row[2],
                    "point": parse_point(row[3]),
                    "race": row[4],
                    "class": row[5],
                    "level": level,
                    "arg": row[7],
                    "detail": row[8],
                    "kv": parse_detail(row[8]),
                }
            )
    return rows


def read_perf_rows(path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.DictReader(handle)
        return list(reader)


def to_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def duration_minutes(first, last):
    if not first or not last:
        return 0.0
    return max(0.0, (last - first).total_seconds() / 60.0)


def bucket_point(point, size=25.0):
    if not point:
        return "unknown"
    x, y = point
    return f"{round(x / size) * size:.0f},{round(y / size) * size:.0f}"


def summarize_perf(perf_rows):
    if not perf_rows:
        return {}

    numeric = defaultdict(list)
    for row in perf_rows:
        for key, value in row.items():
            num = to_float(value)
            if not math.isnan(num):
                numeric[key].append(num)

    def avg(key):
        values = numeric.get(key, [])
        return sum(values) / len(values) if values else None

    def maxv(key):
        values = numeric.get(key, [])
        return max(values) if values else None

    return {
        "rows": len(perf_rows),
        "uptime_start": numeric.get("uptime_sec", [None])[0],
        "uptime_end": numeric.get("uptime_sec", [None])[-1] if numeric.get("uptime_sec") else None,
        "tick_avg_ms": avg("tick_ms"),
        "tick_max_ms": maxv("tick_ms"),
        "map_avg_ms": avg("map_manager_ms"),
        "map_max_ms": maxv("map_manager_ms"),
        "playerbots_avg_ms": avg("playerbots_avg_ms"),
        "playerbots_max_ms": maxv("playerbots_max_ms"),
        "corpses_delta": (
            numeric["total_corpses"][-1] - numeric["total_corpses"][0]
            if len(numeric.get("total_corpses", [])) >= 2
            else None
        ),
        "travel_quest_requests": sum(numeric.get("travel_quest_requests", [])),
        "travel_quest_request_ms": sum(numeric.get("travel_quest_request_ms", [])),
        "bot_pressure_deferred_path": sum(numeric.get("bot_pressure_deferred_path", [])),
        "bot_pressure_deferred_quest": sum(numeric.get("bot_pressure_deferred_quest", [])),
    }


def analyze_events(rows):
    bots = defaultdict(BotStats)
    global_events = Counter()
    path_reject_hotspots = Counter()
    path_reject_destinations = Counter()
    movement_retry_targets = Counter()
    vertical_los_targets = Counter()
    loot_reject_targets = Counter()
    rpg_suppressed_targets = Counter()
    rpg_suppressed_hotspots = Counter()
    objective_targets = Counter()
    repeated_action_bots = []

    for row in rows:
        bot = bots[row["bot"]]
        if bot.first_ts is None:
            bot.first_ts = row["ts"]
            bot.first_level = row["level"]
        bot.last_ts = row["ts"]
        bot.last_level = row["level"]
        bot.events[row["event"]] += 1
        global_events[row["event"]] += 1

        if row["event"] == bot.last_action:
            current_streak = getattr(bot, "_current_streak", 0) + 1
        else:
            current_streak = 1
        setattr(bot, "_current_streak", current_streak)
        bot.last_action = row["event"]
        bot.same_action_streak_max = max(bot.same_action_streak_max, current_streak)

        if row["point"]:
            bot.positions_by_event[row["event"]].append(row["point"])

        detail = row["detail"]
        kv = row["kv"]

        if row["event"] == "StoreLootAction":
            bot.items_looted[row["arg"]] += 1
        elif row["event"] == "QuestUpdateAddItemAction":
            bot.quest_items[row["arg"]] += 1
        elif row["event"] == "QuestTravelSelected":
            bot.travel_targets[row["arg"]] += 1
        elif row["event"] == "MovementPathRejected":
            action = detail_value(kv, "action", row["arg"] or "movement")
            reason = detail_value(kv, "reason", "unknown")
            end = end_position_key(kv)
            key = f"{reason} -> {end}"
            bot.movement_rejects[key] += 1
            start_map = parse_int(row["arg"])
            end_map = parse_int(detail_value(kv, "endMap", ""), start_map)
            start_bucket = bucket_point(row["point"])
            end_bucket = bucket_point(point_from_kv(kv))
            path_reject_hotspots[(start_map, start_bucket, reason)] += 1
            path_reject_destinations[(end_map, end_bucket, reason, action)] += 1
        elif row["event"] in ("MovementTravelRetry", "MovementTravelExpired"):
            title = detail_value(kv, "title", row["arg"] or "unknown")
            status = detail_value(kv, "status", "")
            entry = detail_value(kv, "entry", "")
            destination = detail_value(kv, "destination", "")
            key = title
            if entry:
                key = f"{key}#{entry}"
            if destination:
                key = f"{key} destination={destination}"
            if status:
                key = f"{key} status={status}"
            movement_retry_targets[(row["event"], key)] += 1
        elif row["event"] == "QuestObjectiveCombatDeferred" and "reason=vertical_los" in detail:
            target = target_key(kv, row["arg"])
            bot.combat_deferred[target] += 1
            vertical_los_targets[target] += 1
        elif row["event"] == "LootRejected":
            target = target_key(kv, "unknown")
            reason = detail_value(kv, "reason", "unknown")
            key = f"{reason}:{target}"
            bot.loot_rejects[key] += 1
            loot_reject_targets[key] += 1
        elif row["event"] in ("RpgTargetSuppressed", "RpgTargetLearnedPenaltySkip"):
            target = target_key(kv, row["arg"] or "unknown")
            reason = detail_value(kv, "reason", row["event"])
            key = f"{reason}:{target}"
            bot.rpg_suppressed[key] += 1
            rpg_suppressed_targets[key] += 1

            map_id = parse_int(detail_value(kv, "map", ""), -1)
            bucket = bucket_point(row["point"])
            if map_id >= 0 and bucket != "unknown":
                rpg_suppressed_hotspots[(map_id, bucket, reason)] += 1
        elif row["event"] == "QuestObjectiveVisibleAttack":
            target = target_key(kv, row["arg"])
            objective_targets[target] += 1
        elif row["event"] == "BotStatsSnapshot":
            bot.stat_samples += 1
            apm = parse_int(detail_value(kv, "apm", "0"), 0)
            xp = parse_int(detail_value(kv, "xp", ""), -1)
            money = parse_int(detail_value(kv, "money", ""), -1)
            if bot.first_xp is None and xp >= 0:
                bot.first_xp = xp
            if xp >= 0:
                bot.last_xp = xp
            if bot.first_money is None and money >= 0:
                bot.first_money = money
            if money >= 0:
                bot.last_money = money

            bot.moving_samples += 1 if detail_value(kv, "moving", "0") == "1" else 0
            bot.dead_samples += 1 if detail_value(kv, "dead", "0") == "1" else 0
            bot.ghost_samples += 1 if detail_value(kv, "ghost", "0") == "1" else 0
            bot.low_apm_samples += 1 if apm < 20 else 0
            bot.max_since_progress_sec = max(
                bot.max_since_progress_sec,
                parse_int(detail_value(kv, "sinceProgressSec", "0"), 0),
            )
            bot.last_quests = parse_int(detail_value(kv, "quests", ""), -1)
            bot.last_quest_complete = parse_int(detail_value(kv, "questComplete", ""), -1)
            bot.last_quest_reward_ready = parse_int(detail_value(kv, "questRewardReady", ""), -1)

    for name, bot in bots.items():
        if bot.same_action_streak_max >= 40:
            repeated_action_bots.append((name, bot.same_action_streak_max, bot.last_action))

    return {
        "bots": bots,
        "global_events": global_events,
        "path_reject_hotspots": path_reject_hotspots,
        "path_reject_destinations": path_reject_destinations,
        "movement_retry_targets": movement_retry_targets,
        "vertical_los_targets": vertical_los_targets,
        "loot_reject_targets": loot_reject_targets,
        "rpg_suppressed_targets": rpg_suppressed_targets,
        "rpg_suppressed_hotspots": rpg_suppressed_hotspots,
        "objective_targets": objective_targets,
        "repeated_action_bots": repeated_action_bots,
    }


def bot_score(name, bot):
    minutes = duration_minutes(bot.first_ts, bot.last_ts)
    event_rate = bot.events.total() / minutes if minutes else 0.0
    level_gain = (bot.last_level or 0) - (bot.first_level or 0)
    xp_gain = 0
    if bot.first_xp is not None and bot.last_xp is not None:
        xp_gain = max(0, bot.last_xp - bot.first_xp)
    money_gain = 0
    if bot.first_money is not None and bot.last_money is not None:
        money_gain = bot.last_money - bot.first_money
    progress = (
        level_gain * 100
        + xp_gain / 100.0
        + max(0, money_gain) / 1000.0
        + sum(bot.quest_items.values()) * 6
        + sum(bot.items_looted.values()) * 2
        + bot.events["QuestTurnInTravelSelected"] * 3
    )
    pain = (
        bot.events["MovementPathRejected"] * 8
        + bot.events["MovementTravelExpired"] * 20
        + bot.events["QuestObjectiveCombatDeferred"] * 5
        + bot.events["StaleTargetRecover"] * 2
        + bot.events["AttackForceChase"] * 2
        + bot.events["LootRejected"] * 4
        + bot.events["RpgTargetSuppressed"] * 2
        + bot.events["RpgTargetLearnedPenaltySkip"]
        + bot.events["DecisionLoopTrace"] * 3
        + bot.events["AutoReleaseSpiritStall"] * 15
    )
    return {
        "bot": name,
        "minutes": minutes,
        "events": bot.events.total(),
        "event_rate_per_min": event_rate,
        "first_level": bot.first_level,
        "last_level": bot.last_level,
        "level_gain": level_gain,
        "xp_gain": xp_gain,
        "money_gain": money_gain,
        "progress_score": progress,
        "pain_score": pain,
        "net_score": progress - pain,
        "stat_samples": bot.stat_samples,
        "moving_pct": (100.0 * bot.moving_samples / bot.stat_samples) if bot.stat_samples else None,
        "dead_pct": (100.0 * bot.dead_samples / bot.stat_samples) if bot.stat_samples else None,
        "low_apm_pct": (100.0 * bot.low_apm_samples / bot.stat_samples) if bot.stat_samples else None,
        "max_since_progress_sec": bot.max_since_progress_sec,
        "last_quests": bot.last_quests,
        "last_quest_complete": bot.last_quest_complete,
        "last_quest_reward_ready": bot.last_quest_reward_ready,
    }


def make_candidates(analysis):
    candidates = []

    for (map_id, bucket, reason), count in analysis["path_reject_hotspots"].most_common(30):
        severity = "high" if count >= 20 else "medium" if count >= 8 else "low"
        candidates.append(
            {
                "type": "path_blackspot",
                "severity": severity,
                "count": count,
                "map_id": map_id,
                "key": bucket,
                "reason": reason,
                "evidence": "current-position bucket from MovementPathRejected",
                "recommendation": "Apply learned destination penalty or choose alternate point for this objective/area.",
            }
        )

    for (map_id, bucket, reason, action), count in analysis["path_reject_destinations"].most_common(30):
        severity = "high" if count >= 20 else "medium" if count >= 8 else "low"
        candidates.append(
            {
                "type": "destination_blackspot",
                "severity": severity,
                "count": count,
                "map_id": map_id,
                "key": bucket,
                "reason": f"{action}:{reason}",
                "evidence": "destination bucket from rejected movement path",
                "recommendation": "Lower travel score for this destination bucket until a known-good route or alternate landing point exists.",
            }
        )

    for (event, title), count in analysis["movement_retry_targets"].most_common(20):
        severity = "high" if event == "MovementTravelExpired" or count >= 8 else "medium"
        candidates.append(
            {
                "type": "travel_recovery",
                "severity": severity,
                "count": count,
                "map_id": -1,
                "key": title,
                "reason": event,
                "evidence": "travel retry/expiry event for selected target",
                "recommendation": "Lower score for this destination point; add nearby alternate or hub escape route.",
            }
        )

    for target, count in analysis["vertical_los_targets"].most_common(20):
        severity = "high" if count >= 10 else "medium"
        candidates.append(
            {
                "type": "vertical_los_trap",
                "severity": severity,
                "count": count,
                "map_id": -1,
                "key": target,
                "reason": "combat target visible by range scan but unreachable vertically",
                "evidence": "QuestObjectiveCombatDeferred reason=vertical_los",
                "recommendation": "Teach combat scanner to require navigable route or suppress target from this position cluster.",
            }
        )

    for key, count in analysis["loot_reject_targets"].most_common(20):
        severity = "high" if count >= 20 else "medium"
        candidates.append(
            {
                "type": "loot_rights_failure",
                "severity": severity,
                "count": count,
                "map_id": -1,
                "key": key,
                "reason": "bot attempted loot without server loot rights",
                "evidence": "LootRejected",
                "recommendation": "Improve pre-kill grouping/tag ownership or suppress corpse retry for non-looters.",
            }
        )

    for (map_id, bucket, reason), count in analysis["rpg_suppressed_hotspots"].most_common(30):
        severity = "high" if count >= 20 else "medium" if count >= 8 else "low"
        candidates.append(
            {
                "type": "event_blackspot",
                "severity": severity,
                "count": count,
                "map_id": map_id,
                "key": bucket,
                "reason": reason,
                "evidence": "RPG target suppression around low-value event/interactable cluster",
                "recommendation": "Penalize autonomous interaction choices in this event area unless a quest/travel target explicitly requires it.",
            }
        )

    for key, count in analysis["rpg_suppressed_targets"].most_common(30):
        severity = "high" if count >= 20 else "medium" if count >= 8 else "low"
        candidates.append(
            {
                "type": "rpg_distraction",
                "severity": severity,
                "count": count,
                "map_id": -1,
                "key": key,
                "reason": "low-value repeated RPG interaction",
                "evidence": "RpgTargetSuppressed or RpgTargetLearnedPenaltySkip",
                "recommendation": "Lower RPG target score for this NPC/object entry on future runs.",
            }
        )

    return candidates


def write_report(out_dir, events_path, perf_path, rows, perf_summary, analysis, candidates):
    out_dir.mkdir(parents=True, exist_ok=True)
    has_stat_snapshots = analysis["global_events"].get("BotStatsSnapshot", 0) > 0
    bot_scores = sorted(
        [bot_score(name, bot) for name, bot in analysis["bots"].items()],
        key=lambda row: row["pain_score"],
        reverse=True,
    )

    report = []
    report.append("# Bot Learning Run Report")
    report.append("")
    report.append(f"- events: `{events_path}`")
    report.append(f"- perf: `{perf_path}`")
    report.append(f"- bot event rows: {len(rows)}")
    report.append(f"- bots observed: {len(analysis['bots'])}")
    if rows:
        report.append(f"- window: {rows[0]['ts'].isoformat()} to {rows[-1]['ts'].isoformat()}")
    report.append("")

    report.append("## Server Perf")
    if perf_summary:
        for key, value in perf_summary.items():
            if isinstance(value, float):
                report.append(f"- {key}: {value:.2f}")
            else:
                report.append(f"- {key}: {value}")
    else:
        report.append("- no perf rows found")
    report.append("")

    report.append("## Loud Events")
    for event, count in analysis["global_events"].most_common(25):
        report.append(f"- {event}: {count}")
    report.append("")

    report.append("## Worst Bot Pain")
    for row in bot_scores[:25]:
        report.append(
            "- {bot}: pain={pain_score} progress={progress_score:.1f} net={net_score:.1f} "
            "lvl={first_level}->{last_level} xp+={xp_gain} money+={money_gain} "
            "apm~={event_rate_per_min:.1f} events={events} maxNoProgress={max_since_progress_sec}s".format(**row)
        )
    report.append("")

    report.append("## Best Bot Progress")
    for row in sorted(bot_scores, key=lambda r: r["progress_score"], reverse=True)[:20]:
        report.append(
            "- {bot}: progress={progress_score:.1f} pain={pain_score} "
            "lvl={first_level}->{last_level} xp+={xp_gain} money+={money_gain} "
            "apm~={event_rate_per_min:.1f}".format(**row)
        )
    report.append("")

    if has_stat_snapshots:
        report.append("## Bot Stat Snapshots")
        stat_rows = [row for row in bot_scores if row["stat_samples"]]
        for row in sorted(stat_rows, key=lambda r: (r["low_apm_pct"] or 0, r["max_since_progress_sec"]), reverse=True)[:20]:
            report.append(
                "- {bot}: samples={stat_samples} moving={moving_pct:.1f}% dead={dead_pct:.1f}% "
                "low_apm={low_apm_pct:.1f}% maxNoProgress={max_since_progress_sec}s "
                "quests={last_quests} complete={last_quest_complete} rewardReady={last_quest_reward_ready}".format(**row)
            )
        report.append("")

    report.append("## Learned Candidates")
    for candidate in candidates[:60]:
        map_text = f" map={candidate.get('map_id', -1)}" if candidate.get("map_id", -1) >= 0 else ""
        report.append(
            f"- [{candidate['severity']}] {candidate['type']}{map_text} `{candidate['key']}` "
            f"count={candidate['count']} reason={candidate['reason']} -> {candidate['recommendation']}"
        )
    report.append("")

    report.append("## Current Telemetry Gaps")
    if has_stat_snapshots:
        report.append("- XP, money, quest-log state, movement, death, and per-window APM snapshots are now present via BotStatsSnapshot.")
        report.append("- true quest completion/hour still needs quest reward/complete delta events, not only current quest-log state")
        report.append("- gold/hour can now be estimated from BotStatsSnapshot money deltas, but loot-source attribution still needs loot gold events")
    else:
        report.append("- true XP/hour needs an XP gain event or periodic bot stats snapshot")
        report.append("- true gold/hour needs money delta snapshots or loot gold events")
        report.append("- quest completion/hour needs quest reward/complete events, not only item updates")
        report.append("- APM is currently approximated from bot event rows, not all action executions")
    report.append("")

    report.append("## Next Bot-Brain Actions")
    report.append("- import high-confidence path and destination blackspots into learned memory")
    if has_stat_snapshots:
        report.append("- train rankers against BotStatsSnapshot deltas: XP gain, money gain, low-APM time, dead time, and no-progress time")
    else:
        report.append("- add periodic bot stat snapshots for XP, money, quest completion, and stuck-time")
    report.append("- make the quest/travel scorer subtract learned penalties before broad fallback searches")
    report.append("- train the first ranker on guide-hub choices after enough run summaries exist")
    report.append("")

    (out_dir / "analysis.md").write_text("\n".join(report) + "\n", encoding="utf-8")

    payload = {
        "events_path": str(events_path),
        "perf_path": str(perf_path),
        "rows": len(rows),
        "perf": perf_summary,
        "events": dict(analysis["global_events"].most_common()),
        "bot_scores": bot_scores,
        "candidates": candidates,
    }
    (out_dir / "analysis.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")

    with (out_dir / "learned_candidates.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["type", "severity", "count", "map_id", "key", "reason", "evidence", "recommendation"],
        )
        writer.writeheader()
        writer.writerows(candidates)


def main():
    parser = argparse.ArgumentParser(description="Analyze PlayerBot event/perf logs and emit learned-behavior candidates.")
    parser.add_argument("--events", default="logs/bot_events.csv", type=Path)
    parser.add_argument("--perf", default="logs/server_perf.csv", type=Path)
    parser.add_argument("--out-dir", default=None, type=Path)
    args = parser.parse_args()

    rows = read_bot_events(args.events)
    perf_rows = read_perf_rows(args.perf)
    perf_summary = summarize_perf(perf_rows)
    analysis = analyze_events(rows)
    candidates = make_candidates(analysis)

    out_dir = args.out_dir
    if out_dir is None:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        out_dir = Path("reports") / "bot_learning" / stamp

    write_report(out_dir, args.events, args.perf, rows, perf_summary, analysis, candidates)
    print(f"wrote {out_dir / 'analysis.md'}")
    print(f"wrote {out_dir / 'analysis.json'}")
    print(f"wrote {out_dir / 'learned_candidates.csv'}")


if __name__ == "__main__":
    main()
