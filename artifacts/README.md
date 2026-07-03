# Learning Artifacts

Portable, re-appliable artifacts of everything the bot fleet *learns* over time, so a
fresh install / wipe / migration to new hardware can restore accumulated learning instead
of starting from zero.

## What counts as a learning artifact
Anything the bots derive from running that we do NOT want to lose on a DB wipe:

- **Learned blackspots** — stuck/unreachable areas (path failures), spatial-hashed, with
  penalties. DB table: `ai_playerbot_learned_blackspot` (BotLearningMgr).
- **Objective stats** — per-quest/objective success/failure/time/pain scores and learned
  penalties. DB table: `ai_playerbot_objective_stats`.
- **Behavioral samples** — raw progression telemetry used to build synthetic-progression
  rate tables. DB tables: `ai_playerbot_task_sample`, `ai_playerbot_combat_sample`.
- **Synthetic progression rate tables** — derived (level/activity/zone → XP/gold/skill per
  hour) used by the COLD/synthetic tier. Derived artifact, regenerated from samples.
- **Rail / road network** (FUTURE) — the learned travel graph bots build over time and snap
  onto like roads (nodes, edges, exits). This is the highest-value artifact to preserve.

## Layout (convention)
```
artifacts/
  README.md            <- this file
  db/                  <- SQL dumps of the learning tables (mysqldump per table)
  rates/               <- derived synthetic-progression rate tables
  rails/               <- learned rail/road network graph
  export.sh / import.sh  <- (TODO) dump from / load into the live DB
```

## Export / import (TODO — stub for now)
Export: `mysqldump <chars_db> ai_playerbot_learned_blackspot ai_playerbot_objective_stats
ai_playerbot_task_sample ai_playerbot_combat_sample > artifacts/db/learning.sql`
(confirm the actual DB/connection the BotLearningMgr tables live in before wiring this).
Import on fresh install: load the dumps, then regenerate `rates/` and load `rails/`.

Keep artifacts versioned/committed so a reinstall is `import.sh` away from restoring learning.
