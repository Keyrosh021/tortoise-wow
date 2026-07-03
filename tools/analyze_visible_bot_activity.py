#!/usr/bin/env python3
import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


FIELD_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^=]*?)(?= [A-Za-z][A-Za-z0-9_]*=|$)")


def parse_fields(message):
    return {key: value.strip() for key, value in FIELD_RE.findall(message or "")}


def as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def read_events(path):
    if not path.exists():
        return []

    rows = []
    with path.open(errors="replace", newline="") as handle:
        for row in csv.reader(handle):
            if len(row) < 9:
                continue

            event = row[2]
            if event not in ("VisibleBotActivityTrace", "VisibleBotLowApm", "VisibleBotStuckState"):
                continue

            fields = parse_fields(row[8])
            rows.append(
                {
                    "time": row[0],
                    "bot": row[1],
                    "event": event,
                    "position": row[3],
                    "map": row[4],
                    "level": row[5],
                    "class": row[6],
                    "action_name": row[7],
                    "fields": fields,
                    "message": row[8],
                }
            )

    return rows


def summarize(rows):
    event_counts = Counter(row["event"] for row in rows)
    state_action_counts = Counter()
    bot_counts = Counter()
    state_examples = {}
    bot_latest = {}
    apm_by_state = defaultdict(list)
    decision_by_state = defaultdict(list)

    for row in rows:
        fields = row["fields"]
        state = fields.get("visibleState", "unknown")
        action = fields.get("action", row["action_name"] or "none")
        key = (state, action)

        if row["event"] in ("VisibleBotLowApm", "VisibleBotStuckState"):
            state_action_counts[key] += 1
            state_examples.setdefault(key, row)
            bot_counts[row["bot"]] += 1
            bot_latest[row["bot"]] = row

        apm_by_state[state].append(as_float(fields.get("apm")))
        decision_by_state[state].append(as_float(fields.get("decisionTpm")))

    state_metrics = {}
    states = set(apm_by_state) | set(decision_by_state)
    for state in sorted(states):
        apms = apm_by_state[state]
        decisions = decision_by_state[state]
        state_metrics[state] = {
            "samples": len(apms),
            "avg_apm": round(sum(apms) / len(apms), 2) if apms else 0,
            "avg_decision_tpm": round(sum(decisions) / len(decisions), 2) if decisions else 0,
        }

    return {
        "event_counts": event_counts,
        "state_action_counts": state_action_counts,
        "state_examples": state_examples,
        "bot_counts": bot_counts,
        "bot_latest": bot_latest,
        "state_metrics": state_metrics,
    }


def write_outputs(summary, outdir):
    outdir.mkdir(parents=True, exist_ok=True)

    json_out = {
        "event_counts": dict(summary["event_counts"]),
        "state_metrics": summary["state_metrics"],
        "top_state_actions": [
            {
                "visible_state": state,
                "action": action,
                "count": count,
                "example_bot": summary["state_examples"][(state, action)]["bot"],
                "example_position": summary["state_examples"][(state, action)]["position"],
                "example_message": summary["state_examples"][(state, action)]["message"],
            }
            for (state, action), count in summary["state_action_counts"].most_common(50)
        ],
        "top_bots": [
            {
                "bot": bot,
                "count": count,
                "latest_event": summary["bot_latest"][bot]["event"],
                "latest_position": summary["bot_latest"][bot]["position"],
                "latest_message": summary["bot_latest"][bot]["message"],
            }
            for bot, count in summary["bot_counts"].most_common(50)
        ],
    }

    (outdir / "visible_activity_summary.json").write_text(json.dumps(json_out, indent=2), encoding="utf-8")

    with (outdir / "visible_stuck_states.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["count", "visible_state", "action", "example_bot", "example_position", "example_message"])
        for (state, action), count in summary["state_action_counts"].most_common():
            example = summary["state_examples"][(state, action)]
            writer.writerow([count, state, action, example["bot"], example["position"], example["message"]])

    lines = [
        "# Visible Bot Activity",
        "",
        "## Event Counts",
        "",
    ]
    for event, count in summary["event_counts"].most_common():
        lines.append(f"- {event}: {count}")

    lines.extend(["", "## State Metrics", ""])
    for state, metrics in sorted(summary["state_metrics"].items(), key=lambda item: item[1]["samples"], reverse=True):
        lines.append(
            f"- {state}: samples={metrics['samples']} avg_apm={metrics['avg_apm']} avg_decision_tpm={metrics['avg_decision_tpm']}"
        )

    lines.extend(["", "## Top Visible Stuck States", ""])
    for (state, action), count in summary["state_action_counts"].most_common(25):
        example = summary["state_examples"][(state, action)]
        lines.append(f"- {count}x {state} / {action}: {example['bot']} {example['position']}")

    lines.extend(["", "## Top Bots", ""])
    for bot, count in summary["bot_counts"].most_common(25):
        latest = summary["bot_latest"][bot]
        lines.append(f"- {bot}: {count} latest={latest['event']} {latest['position']}")

    (outdir / "visible_activity_summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Summarize foreground visible playerbot activity and stuck labels.")
    parser.add_argument("--events", default="logs/bot_events.csv", help="Path to bot_events.csv")
    parser.add_argument("--outdir", default="reports/bot_visible_activity/latest", help="Output directory")
    args = parser.parse_args()

    rows = read_events(Path(args.events))
    summary = summarize(rows)
    write_outputs(summary, Path(args.outdir))

    print(f"rows={len(rows)}")
    print("event_counts", dict(summary["event_counts"]))
    print(f"wrote {args.outdir}")


if __name__ == "__main__":
    main()
