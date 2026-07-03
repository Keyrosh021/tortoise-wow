#!/usr/bin/env python3

import csv
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path


def to_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def read_csv_rows(path):
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        return [{key: to_float(value) for key, value in row.items()} for row in reader]


def split_runs(rows):
    runs = []
    current = []
    previous_uptime = None

    for row in rows:
        uptime = row.get("uptime_sec", math.nan)
        if current and not math.isnan(uptime) and previous_uptime is not None and uptime < previous_uptime:
            runs.append(current)
            current = []
        current.append(row)
        previous_uptime = uptime

    if current:
        runs.append(current)

    return runs


def avg(rows, key):
    values = [row[key] for row in rows if key in row and not math.isnan(row[key])]
    return sum(values) / len(values) if values else math.nan


def corr(rows, left_key, right_key):
    pairs = [
        (row[left_key], row[right_key])
        for row in rows
        if left_key in row and right_key in row and not math.isnan(row[left_key]) and not math.isnan(row[right_key])
    ]
    if len(pairs) < 2:
        return math.nan

    left = [x for x, _ in pairs]
    right = [y for _, y in pairs]
    left_mean = sum(left) / len(left)
    right_mean = sum(right) / len(right)
    numerator = sum((x - left_mean) * (y - right_mean) for x, y in pairs)
    left_den = math.sqrt(sum((x - left_mean) ** 2 for x in left))
    right_den = math.sqrt(sum((y - right_mean) ** 2 for y in right))
    if not left_den or not right_den:
        return math.nan
    return numerator / (left_den * right_den)


def summarize_runs(rows):
    runs = split_runs(rows)
    print(f"runs={len(runs)} rows={len(rows)}")
    for index, run in enumerate(runs, start=1):
        first = run[0]
        last = run[-1]
        print(
            "run",
            index,
            f"uptime={int(first['uptime_sec'])}->{int(last['uptime_sec'])}",
            f"rows={len(run)}",
            f"tick_avg={avg(run, 'tick_ms'):.1f}",
            f"map_avg={avg(run, 'map_manager_ms'):.1f}",
            f"creatures_delta={int(last['total_creatures'] - first['total_creatures'])}",
            f"gameobjects_delta={int(last['total_gameobjects'] - first['total_gameobjects'])}",
            f"corpses_delta={int(last['total_corpses'] - first['total_corpses'])}",
        )


def summarize_maps(rows):
    slowest_counts = Counter(int(row["slowest_map_id"]) for row in rows if not math.isnan(row["slowest_map_id"]))
    busiest_counts = Counter(int(row["busiest_map_id"]) for row in rows if not math.isnan(row["busiest_map_id"]))
    print("slowest_map_id_counts", dict(slowest_counts.most_common()))
    print("busiest_map_id_counts", dict(busiest_counts.most_common()))


def summarize_correlations(rows):
    targets = ["tick_ms", "map_manager_ms"]
    candidates = [
        "uptime_sec",
        "world_update_avg_ms",
        "playerbots_avg_ms",
        "busiest_map_update_ms",
        "busiest_map_players_ms",
        "update_players_ms",
        "player_update_ms",
        "player_update_bot_ms",
        "playerbot_hook_ms",
        "playerbot_ai_ms",
        "busiest_map_obj_updates_ms",
        "busiest_map_players_post_vis_ms",
        "total_players",
        "total_creatures",
        "total_gameobjects",
        "total_corpses",
    ]

    for target in targets:
        print(f"{target}_correlations")
        ranked = sorted(
            ((name, corr(rows, name, target)) for name in candidates if name != target and name in rows[0]),
            key=lambda item: abs(item[1]) if not math.isnan(item[1]) else -1,
            reverse=True,
        )
        for name, value in ranked:
            if not math.isnan(value):
                print(f"  {name}={value:.3f}")


def summarize_spikes(rows, limit):
    ranked = sorted(
        [row for row in rows if not math.isnan(row["tick_ms"])],
        key=lambda row: row["tick_ms"],
        reverse=True,
    )[:limit]
    print(f"top_tick_rows={len(ranked)}")
    for row in ranked:
        print(
            "  uptime={uptime} tick={tick:.1f} map={map_id} slowest_map_ms={slow:.1f} "
            "slow_players={players} slow_obj={obj:.1f} slow_postvis={post:.1f} "
            "busiest_map={busy_map} busiest_players_ms={busy_players:.1f} busiest_obj_ms={busy_obj:.1f} corpses={corpses}".format(
                uptime=int(row["uptime_sec"]),
                tick=row["tick_ms"],
                map_id=int(row["slowest_map_id"]),
                slow=row["slowest_map_ms"],
                players=int(row["slowest_map_players"]),
                obj=row["slowest_map_obj_updates_ms"],
                post=row["slowest_map_players_post_vis_ms"],
                busy_map=int(row["busiest_map_id"]),
                busy_players=row["busiest_map_players_ms"],
                busy_obj=row["busiest_map_obj_updates_ms"],
                corpses=int(row["total_corpses"]),
            )
        )


def sum_col(rows, key):
    return sum(row[key] for row in rows if key in row and not math.isnan(row[key]))


def max_col(rows, key):
    values = [row[key] for row in rows if key in row and not math.isnan(row[key])]
    return max(values) if values else math.nan


def summarize_player_updates(rows):
    if not rows or "update_players_calls" not in rows[0]:
        return

    update_calls = sum_col(rows, "update_players_calls")
    update_ms = sum_col(rows, "update_players_ms")
    processed = sum_col(rows, "update_players_processed")
    skipped = sum_col(rows, "update_players_skipped")
    bot_processed = sum_col(rows, "update_players_bot_processed")
    bot_skipped = sum_col(rows, "update_players_bot_skipped")
    wait_bot_only_skipped = sum_col(rows, "update_players_wait_bot_only_skipped")
    player_calls = sum_col(rows, "player_update_calls")
    player_ms = sum_col(rows, "player_update_ms")
    bot_calls = sum_col(rows, "player_update_bot_calls")
    bot_ms = sum_col(rows, "player_update_bot_ms")
    hook_calls = sum_col(rows, "playerbot_hook_calls")
    hook_ms = sum_col(rows, "playerbot_hook_ms")
    suppressed_calls = sum_col(rows, "playerbot_hook_suppressed_calls")
    suppressed_elapsed_ms = sum_col(rows, "playerbot_hook_suppressed_elapsed_ms")
    ai_calls = sum_col(rows, "playerbot_ai_calls")
    ai_ms = sum_col(rows, "playerbot_ai_ms")
    mgr_calls = sum_col(rows, "playerbot_mgr_calls")
    mgr_ms = sum_col(rows, "playerbot_mgr_ms")

    print("player_updates")
    print(
        "  update_players_calls={calls:.0f} update_players_ms={ms:.0f} avg_ms={avg_ms:.2f} "
        "max_ms={max_ms:.0f} processed={processed:.0f} skipped={skipped:.0f} "
        "bot_processed={bot_processed:.0f} bot_skipped={bot_skipped:.0f} "
        "wait_bot_only_skipped={wait_bot_only_skipped:.0f}".format(
            calls=update_calls,
            ms=update_ms,
            avg_ms=(update_ms / update_calls) if update_calls else math.nan,
            max_ms=max_col(rows, "update_players_max_ms"),
            processed=processed,
            skipped=skipped,
            bot_processed=bot_processed,
            bot_skipped=bot_skipped,
            wait_bot_only_skipped=wait_bot_only_skipped,
        )
    )
    print(
        "  player_update_calls={calls:.0f} player_update_ms={ms:.0f} avg_ms={avg_ms:.2f} "
        "max_ms={max_ms:.0f} bot_calls={bot_calls:.0f} bot_ms={bot_ms:.0f} "
        "bot_avg_ms={bot_avg:.2f} bot_max_ms={bot_max:.0f}".format(
            calls=player_calls,
            ms=player_ms,
            avg_ms=(player_ms / player_calls) if player_calls else math.nan,
            max_ms=max_col(rows, "player_update_max_ms"),
            bot_calls=bot_calls,
            bot_ms=bot_ms,
            bot_avg=(bot_ms / bot_calls) if bot_calls else math.nan,
            bot_max=max_col(rows, "player_update_bot_max_ms"),
        )
    )
    print(
        "  playerbot_hook_calls={hook_calls:.0f} hook_ms={hook_ms:.0f} hook_avg_ms={hook_avg:.2f} "
        "hook_max_ms={hook_max:.0f} suppressed_calls={suppressed_calls:.0f} "
        "suppressed_elapsed_ms={suppressed_elapsed_ms:.0f} ai_calls={ai_calls:.0f} ai_ms={ai_ms:.0f} "
        "ai_avg_ms={ai_avg:.2f} ai_max_ms={ai_max:.0f} mgr_calls={mgr_calls:.0f} "
        "mgr_ms={mgr_ms:.0f} mgr_avg_ms={mgr_avg:.2f} mgr_max_ms={mgr_max:.0f}".format(
            hook_calls=hook_calls,
            hook_ms=hook_ms,
            hook_avg=(hook_ms / hook_calls) if hook_calls else math.nan,
            hook_max=max_col(rows, "playerbot_hook_max_ms"),
            suppressed_calls=suppressed_calls,
            suppressed_elapsed_ms=suppressed_elapsed_ms,
            ai_calls=ai_calls,
            ai_ms=ai_ms,
            ai_avg=(ai_ms / ai_calls) if ai_calls else math.nan,
            ai_max=max_col(rows, "playerbot_ai_max_ms"),
            mgr_calls=mgr_calls,
            mgr_ms=mgr_ms,
            mgr_avg=(mgr_ms / mgr_calls) if mgr_calls else math.nan,
            mgr_max=max_col(rows, "playerbot_mgr_max_ms"),
        )
    )


def summarize_travel_planner(rows):
    if not rows or "travel_quest_requests" not in rows[0]:
        return

    requests = sum_col(rows, "travel_quest_requests")
    request_ms = sum_col(rows, "travel_quest_request_ms")
    fetches = sum_col(rows, "travel_quest_fetches")
    async_jobs = sum_col(rows, "travel_quest_async_jobs")
    async_ms = sum_col(rows, "travel_quest_async_ms")
    async_empty = sum_col(rows, "travel_quest_async_empty_jobs")
    async_points = sum_col(rows, "travel_quest_async_result_points")
    partition_calls = sum_col(rows, "travel_partitions_calls")
    partition_ms = sum_col(rows, "travel_partitions_ms")
    partition_empty = sum_col(rows, "travel_partitions_empty_calls")
    quest_partition_calls = sum_col(rows, "travel_quest_partition_calls")
    quest_partition_ms = sum_col(rows, "travel_quest_partition_ms")
    destinations = sum_col(rows, "travel_partitions_destinations")
    result_points = sum_col(rows, "travel_partitions_result_points")
    strict_calls = sum_col(rows, "travel_quest_strict_partition_calls")
    soft_calls = sum_col(rows, "travel_quest_soft_partition_calls")
    broad_calls = sum_col(rows, "travel_quest_broad_partition_calls")
    planner_checks = sum_col(rows, "playerbot_planner_checks")
    planner_allowed = sum_col(rows, "playerbot_planner_allowed")
    planner_deferred = sum_col(rows, "playerbot_planner_deferred")
    planner_fast_lane = sum_col(rows, "playerbot_planner_fast_lane")
    pressure_level_max = max_col(rows, "bot_pressure_level")
    pressure_score_max = max_col(rows, "bot_pressure_score")
    pressure_updates = sum_col(rows, "bot_pressure_updates")
    pressure_deferred_quest = sum_col(rows, "bot_pressure_deferred_quest")
    pressure_deferred_visible = sum_col(rows, "bot_pressure_deferred_visible_objective")
    pressure_deferred_chase = sum_col(rows, "bot_pressure_deferred_force_chase")
    pressure_deferred_stale = sum_col(rows, "bot_pressure_deferred_stale_recover")
    pressure_deferred_path = sum_col(rows, "bot_pressure_deferred_path")

    print("travel_planner")
    if not math.isnan(pressure_level_max):
        print(
            "  pressure max_level={level:.0f} max_score={score:.0f} updates={updates:.0f} "
            "deferred quest={quest:.0f} visible={visible:.0f} chase={chase:.0f} stale={stale:.0f} path={path:.0f}".format(
                level=pressure_level_max,
                score=pressure_score_max,
                updates=pressure_updates,
                quest=pressure_deferred_quest,
                visible=pressure_deferred_visible,
                chase=pressure_deferred_chase,
                stale=pressure_deferred_stale,
                path=pressure_deferred_path,
            )
        )
    if planner_checks:
        print(
            "  planner_checks={checks:.0f} allowed={allowed:.0f} deferred={deferred:.0f} "
            "deferred_pct={deferred_pct:.1f} fast_lane={fast_lane:.0f}".format(
                checks=planner_checks,
                allowed=planner_allowed,
                deferred=planner_deferred,
                deferred_pct=(planner_deferred * 100.0 / planner_checks) if planner_checks else math.nan,
                fast_lane=planner_fast_lane,
            )
        )
    print(
        "  quest_requests={requests:.0f} request_ms={request_ms:.0f} request_avg_ms={request_avg:.2f} "
        "request_max_ms={request_max:.0f} fetches={fetches:.0f} fetches_per_request={fetch_avg:.2f}".format(
            requests=requests,
            request_ms=request_ms,
            request_avg=(request_ms / requests) if requests else math.nan,
            request_max=max_col(rows, "travel_quest_request_max_ms"),
            fetches=fetches,
            fetch_avg=(fetches / requests) if requests else math.nan,
        )
    )
    print(
        "  async_jobs={jobs:.0f} async_ms={async_ms:.0f} async_avg_ms={async_avg:.2f} async_max_ms={async_max:.0f} "
        "empty_jobs={empty:.0f} empty_pct={empty_pct:.1f} result_points={points:.0f}".format(
            jobs=async_jobs,
            async_ms=async_ms,
            async_avg=(async_ms / async_jobs) if async_jobs else math.nan,
            async_max=max_col(rows, "travel_quest_async_max_ms"),
            empty=async_empty,
            empty_pct=(async_empty * 100.0 / async_jobs) if async_jobs else math.nan,
            points=async_points,
        )
    )
    print(
        "  partition_calls={calls:.0f} partition_ms={ms:.0f} partition_avg_ms={avg_ms:.2f} "
        "partition_max_ms={max_ms:.0f} empty_calls={empty:.0f} empty_pct={empty_pct:.1f}".format(
            calls=partition_calls,
            ms=partition_ms,
            avg_ms=(partition_ms / partition_calls) if partition_calls else math.nan,
            max_ms=max_col(rows, "travel_partitions_max_ms"),
            empty=partition_empty,
            empty_pct=(partition_empty * 100.0 / partition_calls) if partition_calls else math.nan,
        )
    )
    print(
        "  quest_partition_calls={calls:.0f} quest_partition_ms={ms:.0f} strict={strict:.0f} "
        "soft={soft:.0f} broad={broad:.0f} destinations_scanned={dests:.0f} result_points={points:.0f}".format(
            calls=quest_partition_calls,
            ms=quest_partition_ms,
            strict=strict_calls,
            soft=soft_calls,
            broad=broad_calls,
            dests=destinations,
            points=result_points,
        )
    )


def summarize_learning_telemetry(rows):
    if not rows or "learning_flush_calls" not in rows[0]:
        return

    flush_calls = sum_col(rows, "learning_flush_calls")
    flush_ms = sum_col(rows, "learning_flush_ms")
    task_rows = sum_col(rows, "learning_task_rows_flushed")
    combat_rows = sum_col(rows, "learning_combat_rows_flushed")
    task_dropped = sum_col(rows, "learning_task_rows_dropped")
    combat_dropped = sum_col(rows, "learning_combat_rows_dropped")

    print("learning_telemetry")
    print(
        "  flush_calls={calls:.0f} flush_ms={ms:.0f} flush_avg_ms={avg:.2f} flush_max_ms={max_ms:.0f} "
        "task_rows={task_rows:.0f} combat_rows={combat_rows:.0f} task_dropped={task_dropped:.0f} "
        "combat_dropped={combat_dropped:.0f} queue_task_max={queue_task:.0f} queue_combat_max={queue_combat:.0f}".format(
            calls=flush_calls,
            ms=flush_ms,
            avg=(flush_ms / flush_calls) if flush_calls else math.nan,
            max_ms=max_col(rows, "learning_flush_max_ms"),
            task_rows=task_rows,
            combat_rows=combat_rows,
            task_dropped=task_dropped,
            combat_dropped=combat_dropped,
            queue_task=max_col(rows, "learning_task_queue_size"),
            queue_combat=max_col(rows, "learning_combat_queue_size"),
        )
    )


def summarize_bot_events(path):
    if not path.exists():
        print("bot_events.csv missing")
        return

    events = Counter()
    soft_points = []
    broad_points = []

    with path.open() as handle:
        for raw_line in handle:
            parts = raw_line.rstrip("\n").split(",", 8)
            if len(parts) < 8:
                continue

            event_name = parts[2]
            events[event_name] += 1

            if event_name != "QuestFetchPlan":
                continue

            message = parts[-1].replace('"', "")
            values = {}
            for token in message.split():
                if "=" not in token:
                    continue
                key, value = token.split("=", 1)
                try:
                    values[key] = float(value)
                except ValueError:
                    continue

            if "softPoints" in values:
                soft_points.append(values["softPoints"])
            if "broadPoints" in values:
                broad_points.append(values["broadPoints"])

    print("top_bot_events")
    for event_name, count in events.most_common(15):
        print(f"  {event_name}={count}")

    if soft_points:
        soft_points.sort()
        broad_points.sort()
        p95_index = min(len(soft_points) - 1, int(len(soft_points) * 0.95))
        print(
            "quest_fetch_plan",
            f"count={len(soft_points)}",
            f"soft_max={soft_points[-1]:.0f}",
            f"soft_p95={soft_points[p95_index]:.0f}",
            f"broad_max={broad_points[-1]:.0f}",
            f"broad_p95={broad_points[p95_index]:.0f}",
        )


def main():
    if len(sys.argv) > 1:
        perf_path = Path(sys.argv[1])
    else:
        perf_path = Path("logs/server_perf.csv")

    if len(sys.argv) > 2:
        bot_events_path = Path(sys.argv[2])
    else:
        bot_events_path = perf_path.with_name("bot_events.csv")

    rows = read_csv_rows(perf_path)
    summarize_runs(rows)
    summarize_maps(rows)
    summarize_correlations(rows)
    summarize_spikes(rows, limit=10)
    summarize_player_updates(rows)
    summarize_travel_planner(rows)
    summarize_learning_telemetry(rows)
    summarize_bot_events(bot_events_path)


if __name__ == "__main__":
    main()
