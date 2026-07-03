#!/usr/bin/env python3
import argparse
import csv
import re
from collections import Counter, defaultdict, deque
from pathlib import Path


FIELD_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^=]*?)(?= [A-Za-z][A-Za-z0-9_]*=|$)")


def parse_fields(message):
    return {key: value.strip() for key, value in FIELD_RE.findall(message or "")}


def as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def first_value(fields, *keys, default=""):
    for key in keys:
        value = fields.get(key)
        if value not in (None, ""):
            return value
    return default


def read_rows(path):
    rows = []
    if not path.exists():
        return rows

    useful_events = {
        "VisibleBotActivityTrace",
        "BotHealthLow",
        "VisibleBotLowApm",
        "VisibleBotStuckState",
        "DeadCorpseMetricStall",
        "DeadCorpseProgressStall",
        "DeadCorpseRepathNudge",
        "DeadRecoveryRouteDecision",
    }

    with path.open(errors="replace", newline="") as handle:
        for row in csv.reader(handle):
            if len(row) < 9 or row[2] not in useful_events:
                continue
            fields = parse_fields(row[8])
            level = first_value(fields, "level", "lvl", default=row[5])
            action = first_value(fields, "action", default=row[7])
            rows.append(
                {
                    "time": row[0],
                    "bot": row[1],
                    "event": row[2],
                    "position": row[3],
                    "map": row[4],
                    "level": level,
                    "klass": row[6],
                    "action": action,
                    "fields": fields,
                    "message": row[8],
                }
            )
    return rows


def summarize(rows):
    latest = {}
    samples = defaultdict(list)
    low_events = Counter()
    event_counts = Counter(row["event"] for row in rows)
    recent = defaultdict(lambda: deque(maxlen=5))

    for row in rows:
        bot = row["bot"]
        latest[bot] = row
        recent[bot].append(row)
        fields = row["fields"]
        if "healthScore" in fields:
            samples[bot].append(
                {
                    "score": as_float(fields.get("healthScore"), 100.0),
                    "apm": as_float(fields.get("apm")),
                    "decision_tpm": as_float(fields.get("decisionTpm")),
                    "since_progress": as_float(fields.get("sinceProgressSec")),
                    "state": fields.get("visibleState", ""),
                    "issue": fields.get("healthIssue", ""),
                    "event": row["event"],
                }
            )
        if row["event"] != "VisibleBotActivityTrace":
            low_events[bot] += 1

    bot_scores = []
    for bot, bot_samples in samples.items():
        if not bot_samples:
            continue
        last = latest[bot]
        recent_scores = bot_samples[-5:]
        avg_score = sum(sample["score"] for sample in recent_scores) / len(recent_scores)
        avg_apm = sum(sample["apm"] for sample in recent_scores) / len(recent_scores)
        max_no_progress = max(sample["since_progress"] for sample in recent_scores)
        bot_scores.append(
            {
                "bot": bot,
                "avg_score": avg_score,
                "avg_apm": avg_apm,
                "max_no_progress": max_no_progress,
                "low_events": low_events[bot],
                "latest": last,
                "latest_fields": last["fields"],
                "recent": list(recent[bot]),
            }
        )

    bot_scores.sort(key=lambda item: (item["avg_score"], -item["max_no_progress"], -item["low_events"], item["bot"]))
    return event_counts, bot_scores


def print_summary(event_counts, bot_scores, limit):
    print("event_counts")
    for event, count in event_counts.most_common():
        print(f"  {event}: {count}")

    print()
    print(f"worst_bots limit={limit}")
    for item in bot_scores[:limit]:
        latest = item["latest"]
        fields = item["latest_fields"]
        print(
            f"{item['bot']}: score={item['avg_score']:.1f} apm={item['avg_apm']:.1f} "
            f"noProgress={item['max_no_progress']:.0f}s lowEvents={item['low_events']} "
            f"event={latest['event']} state={fields.get('visibleState', 'n/a')} "
            f"issue={fields.get('healthIssue', 'n/a')} action={latest['action']} "
            f"pos={latest['position']} lvl={latest['level']} class={latest['klass']}"
        )


def main():
    parser = argparse.ArgumentParser(description="Rank playerbots by activity health score and recent stuck signals.")
    parser.add_argument("--events", default="logs/bot_events.csv", help="Path to bot_events.csv")
    parser.add_argument("--limit", type=int, default=25, help="Number of worst bots to print")
    args = parser.parse_args()

    rows = read_rows(Path(args.events))
    event_counts, bot_scores = summarize(rows)
    print_summary(event_counts, bot_scores, args.limit)


if __name__ == "__main__":
    main()
