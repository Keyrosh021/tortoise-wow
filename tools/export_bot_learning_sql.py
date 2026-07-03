#!/usr/bin/env python3

import argparse
import csv
import re
from datetime import datetime
from pathlib import Path


BUCKET_RE = re.compile(r"^(-?\d+),(-?\d+)$")
OBJECTIVE_RE = re.compile(r"^(?P<name>.*?)(?:#(?P<entry>-?\d+))?(?: quest=(?P<quest>\d+))?$")


def sql_string(value):
    return "'" + (value or "").replace("\\", "\\\\").replace("'", "''") + "'"


def sql_datetime(value):
    return sql_string(value.strftime("%Y-%m-%d %H:%M:%S"))


def parse_bucket(value):
    match = BUCKET_RE.match(value or "")
    if not match:
        return None
    return int(match.group(1)), int(match.group(2))


def parse_int(value, default=-1):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def parse_objective(value):
    match = OBJECTIVE_RE.match(value or "")
    if not match:
        return value or "", 0, 0
    name = match.group("name") or ""
    entry = int(match.group("entry") or 0)
    quest = int(match.group("quest") or 0)
    return name.strip(), abs(entry), quest


def confidence_for(count):
    if count >= 100:
        return 0.95
    if count >= 50:
        return 0.85
    if count >= 20:
        return 0.70
    return 0.45


def penalty_for(row):
    count = int(row["count"])
    kind = row["type"]
    if kind == "destination_blackspot":
        return min(8.0, 1.5 + count / 100.0)
    if kind == "path_blackspot":
        return min(6.0, 1.0 + count / 125.0)
    if kind == "vertical_los_trap":
        return min(5.0, 1.0 + count / 150.0)
    if kind == "loot_rights_failure":
        return min(4.0, 0.75 + count / 100.0)
    if kind == "event_blackspot":
        return min(10.0, 3.0 + count / 40.0)
    if kind == "rpg_distraction":
        return min(30.0, 8.0 + count / 8.0)
    return 1.0


def export_sql(candidates_path, output_path, run_id):
    now = datetime.utcnow()
    statements = [
        "-- Generated from PlayerBot learned candidates.",
        "-- Review before applying to a live world database.",
        "",
    ]

    with candidates_path.open(newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            count = int(row["count"])
            kind = row["type"]
            reason = row["reason"]
            penalty = penalty_for(row)
            confidence = confidence_for(count)

            if kind in ("path_blackspot", "destination_blackspot", "event_blackspot"):
                bucket = parse_bucket(row["key"])
                if not bucket:
                    continue
                bucket_x, bucket_y = bucket
                map_id = parse_int(row.get("map_id"), -1)
                statements.append(
                    "INSERT INTO `ai_playerbot_learned_blackspot` "
                    "(`source_run_id`, `blackspot_type`, `map_id`, `bucket_x`, `bucket_y`, `radius`, "
                    "`reason`, `hit_count`, `penalty`, `confidence`, `first_seen_at`, `last_seen_at`, `enabled`) "
                    "VALUES "
                    f"({run_id}, {sql_string(kind)}, {map_id}, {bucket_x}, {bucket_y}, 25, "
                    f"{sql_string(reason)}, {count}, {penalty:.2f}, {confidence:.2f}, "
                    f"{sql_datetime(now)}, {sql_datetime(now)}, 1) "
                    "ON DUPLICATE KEY UPDATE "
                    "`source_run_id` = VALUES(`source_run_id`), "
                    "`radius` = GREATEST(`radius`, VALUES(`radius`)), "
                    "`hit_count` = `hit_count` + VALUES(`hit_count`), "
                    "`penalty` = GREATEST(`penalty`, VALUES(`penalty`)), "
                    "`confidence` = GREATEST(`confidence`, VALUES(`confidence`)), "
                    "`last_seen_at` = VALUES(`last_seen_at`), "
                    "`enabled` = 1;"
                )
            elif kind in ("vertical_los_trap", "loot_rights_failure", "rpg_distraction"):
                key = row["key"]
                if kind in ("loot_rights_failure", "rpg_distraction") and ":" in key:
                    reason, key = key.split(":", 1)
                name, entry, quest = parse_objective(key)
                statements.append(
                    "INSERT INTO `ai_playerbot_objective_stats` "
                    "(`source_run_id`, `objective_type`, `quest_id`, `entry`, `name`, `map_id`, `bucket_x`, `bucket_y`, "
                    "`attempt_count`, `success_count`, `failure_count`, `avg_seconds`, `pain_score`, `learned_penalty`, "
                    "`last_failure_reason`, `first_seen_at`, `last_seen_at`) "
                    "VALUES "
                    f"({run_id}, {sql_string(kind)}, {quest}, {entry}, {sql_string(name[:128])}, -1, 0, 0, "
                    f"{count}, 0, {count}, 0, {count * penalty:.2f}, {penalty:.2f}, "
                    f"{sql_string(reason[:128])}, {sql_datetime(now)}, {sql_datetime(now)});"
                )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(statements) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Export PlayerBot learned candidate CSV rows as reviewable SQL.")
    parser.add_argument("--candidates", default="reports/bot_learning/latest/learned_candidates.csv", type=Path)
    parser.add_argument("--out", default="reports/bot_learning/latest/learned_candidates.sql", type=Path)
    parser.add_argument("--run-id", default=0, type=int)
    args = parser.parse_args()

    export_sql(args.candidates, args.out, args.run_id)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
