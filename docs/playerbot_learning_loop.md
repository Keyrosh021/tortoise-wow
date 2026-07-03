# PlayerBot Learning Loop

This is the practical path from the current hand-tuned bot fixes to bots that improve across runs.

## Goal

The server should watch bot behavior, identify repeated bad loops, and feed safer choices into later runs. Neural nets can help rank choices, but the game server remains authoritative for movement, combat, quests, loot, grouping, inventory, and account state.

The first useful version is not a giant live model. It is a tight loop:

1. Log structured bot decisions and outcomes.
2. Analyze each run offline.
3. Store learned blackspots and objective pain.
4. Penalize bad destinations/objectives in the quest and travel scorer.
5. Train a ranker only after the rule/data loop is producing clean labels.

## What We Can Learn Now

The current `logs/bot_events.csv` already exposes several high-value patterns:

- path blackspots from `MovementPathRejected`
- bad destination buckets from `MovementTravelRetry` and `MovementTravelExpired`
- vertical line-of-sight traps from `QuestObjectiveCombatDeferred reason=vertical_los`
- combat churn from `AttackMeleeSettle`, `StaleTargetRecover`, and `AttackForceChase`
- loot-right failures from `LootRejected`
- RPG/event distractions from `RpgTargetSuppressed` and `RpgTargetLearnedPenaltySkip`
- low progress / high pain bots from per-bot event rates
- successful travel traces that can become learned bot road candidates
- visible-bot movement smoothness, idle time, and APM near real players

Run:

```bash
python3 tools/analyze_bot_runs.py --out-dir reports/bot_learning/latest
```

The script writes:

- `analysis.md` for human review
- `analysis.json` for tooling
- `learned_candidates.csv` for import into learned-memory tables

To generate reviewable SQL from the candidate CSV:

```bash
python3 tools/export_bot_learning_sql.py --run-id 0
```

This writes `reports/bot_learning/latest/learned_candidates.sql`. Keep it review-first until the scorer has enough safeguards and confidence thresholds.

On server startup, the live PlayerBot CSV logs and `server_perf.csv` are archived before they are truncated. Previous runs are kept under `logs/archive/` with timestamped names so the analyzer can compare multiple runs instead of losing evidence at every restart.

Movement path candidates are map-aware. `MovementPathRejected` rows provide the bot's current map and rejected destination map, and the analyzer exports those as `map_id` on learned blackspot candidates. Keep `map_id=-1` only for global/non-positional findings.

## Learned Memory Tables

`sql/database_updates/20260614183000_world_ai_playerbot_learning.sql` adds the first storage layer:

- `ai_playerbot_run_summary`: one row per analyzed run
- `ai_playerbot_learned_blackspot`: path/destination buckets to penalize
- `ai_playerbot_objective_stats`: quest objective pain and success data
- `event_blackspot` and `rpg_distraction` candidates: event areas, NPCs, and objects that repeatedly distract autonomous bots
- `ai_playerbot_behavior_model`: version marker for future rules or neural rankers

These are advisory tables. They should not directly complete quests, move players, grant loot, or mutate gameplay state.

A later travel-memory table should promote high-confidence successful movement into learned bot roads:

```sql
ai_playerbot_learned_road
id, map_id, zone_id,
from_bucket_x, from_bucket_y, to_bucket_x, to_bucket_y,
path_ref, width, faction_mask, min_level, max_level,
sample_count, success_count, reject_count,
avg_travel_ms, confidence, last_seen_at, enabled
```

Road candidates should be built from repeated successful travel, low rejection rates, low stuck time, and visible-bot movement that looks natural. Rejected paths, vertical traps, corpse-run stalls, and cliff loops should lower confidence or blacklist the segment.

## Scoring Direction

For each candidate quest/travel target, the bot should eventually score:

```text
final_score =
    guide_score
  + personality_preference
  + group_objective_bonus
  + novelty/diversity_bonus
  - learned_blackspot_penalty
  - recent_failure_penalty
  - crowding_penalty
  - distance_or_travel_cost
```

This keeps the bots human-like instead of making every bot follow the same perfect route. Some bots can still choose inconvenient-but-plausible paths, but they should avoid proven dead spots and repeated no-progress loops.

## Neural Net Role

The NN should rank validated options, not invent raw actions.

Good action space:

- choose next quest hub
- choose active objective priority
- choose whether to group for a named/rare objective
- choose a nearby pull/loot/travel candidate from server-generated candidates
- choose aggressiveness, patience, and diversity weights

Bad action space:

- raw movement keys
- direct quest completion
- direct inventory mutation
- direct database writes
- bypassing server path/loot/combat validation

## Training Signals

To move beyond heuristics, add telemetry for:

- XP gained per bot and per minute
- money delta per bot and per minute
- quest accepted, objective complete, quest rewarded
- time spent at hub/objective
- time stuck below movement threshold
- deaths and corpse run duration
- group tag ownership and group loot success
- selected behavior model/ruleset version

Useful labels:

- high XP/hour with low death count
- quest completion/hour
- gold/item value/hour
- low repeated-action loops
- low path rejection rate
- low idle time while objective target is alive nearby

## Implementation Order

1. Keep improving telemetry quality.
2. Import high-confidence learned candidates into the learned-memory tables.
3. Teach travel/quest scoring to subtract learned blackspot penalties.
4. Learn successful travel corridors as bot road candidates.
5. Add objective success/failure snapshots for named mobs like Sarkoth.
6. Add a simple contextual bandit/ranker over guide hubs, objectives, and road candidates.
7. Only then add neural models, using the same safe candidate/ranking interface.
