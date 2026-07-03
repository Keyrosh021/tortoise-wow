# AI Bot Director Implementation Plan

This note turns the broader AI director plan into a staged implementation path for this MaNGOS/playerbot tree.

## Goal

Support many persistent bot identities without fully simulating every bot all the time. The server remains authoritative for movement legality, combat, quests, loot, inventory, auctions, and persistence. AI and neural components should choose high-level intent from validated candidates.

## Near-Term Priority

Questing is currently the hottest playerbot-side behavior. The first implementation slice is therefore:

1. Reduce expensive quest travel planning.
2. Add a cheap quest-guide intent layer.
3. Log structured quest outcomes and repeated failures.
4. Use the guide layer as the future action space for a director or neural ranker.

This gives immediate CPU relief and builds the exact bridge needed for later Bot LOD and external director work.

## Architecture Direction

The intended flow is:

```text
AI Director / local rules
    -> candidate generator
    -> QuestGuideMgr / route guide / economy profile
    -> small validated candidate list
    -> existing TravelMgr, combat, quest, inventory validation
```

The neural/director layer should rank candidates. It should not directly mutate quest state, inventory, auction tables, movement packets, or combat state.

## Phase 1: Quest Travel Cost Reduction

- Remove or gate diagnostic destination/partition scans that run before the real async travel fetch.
- Reuse one `PlayerTravelInfo` snapshot per quest travel request where practical.
- Keep broad questgiver fallback as a last resort.
- Add lightweight plan-shape logging that does not call `TravelMgr::GetPartitions`.
- Add short-lived per-bot quest plan cache after the first cleanup is stable.

Success metric: fewer `GetPartitions` calls per `RequestQuestTravelTargetAction`, lower bot-side update cost, and no regression in low-level quest progression.

## Phase 2: QuestGuideMgr MVP

Add a guide layer that answers: "where should this bot plausibly quest next?"

Initial data can be hand-authored for starter and 1-20 zones, then expanded from telemetry.

Suggested tables:

```sql
ai_playerbot_quest_hub
id, name, faction, race_mask, class_mask,
min_level, max_level, map_id, area_id,
x, y, z, priority, next_hub_ids
```

```sql
ai_playerbot_quest_hub_quest
hub_id, quest_id, role, priority, required, chain_group
```

Suggested roles: `starter`, `pickup`, `objective`, `turnin`, `breadcrumb`, `exit`.

Quest travel should prefer:

1. Active quest turn-ins.
2. Active quest objectives.
3. Current guide hub quests.
4. Next guide hub quests.
5. Recent turn-in anchor.
6. Broad questgiver fallback.

## Phase 3: Telemetry And Deadlock Reports

Log enough structure to detect loops and train/rank later:

- bot id/name, race, class, level
- model/ruleset/behavior version once available
- current hub id
- quest id and objective id
- selected destination entry and position bucket
- travel time, failure reason, completion/turn-in result
- repeated action hash

Daily reports should rank:

- top failed quests
- top repeated quest objectives
- top bad route buckets
- top CPU-heavy repeated actions
- top null-travel/idling causes

## Phase 4: Soft LOD Before Full LOD

Before full abstract/offline simulation, throttle expensive categories for far or unseen bots:

- quest planning refresh
- loot scanning
- target scanning
- travel replanning
- nonessential chat/social work

Then add true modes:

```cpp
enum class BotSimMode
{
    Full,
    Light,
    Abstract,
    Offline
};
```

## Phase 5: Learned Bot Road Network

Build a learned travel layer that answers: "what human-like corridor should this bot use to get near the target?"

The first version should learn from successful movement traces, repeated route choices, path rejection buckets, corpse recovery routes, and visible-bot smoothness. It should prefer routes that look like normal player travel: roads, valley floors, bridge crossings, cave entrances, and common hub-to-objective paths. It should penalize cliffs, steep ledges, bad vertical line-of-sight routes, and path buckets that repeatedly trigger movement recovery.

Suggested flow:

1. Current position -> nearest learned road anchor.
2. Learned road corridor -> exit anchor near destination.
3. Exit anchor -> normal server-validated destination/path.

Suggested future table:

```sql
ai_playerbot_learned_road
id, map_id, zone_id,
from_bucket_x, from_bucket_y, to_bucket_x, to_bucket_y,
path_ref, width, faction_mask, min_level, max_level,
sample_count, success_count, reject_count,
avg_travel_ms, confidence, last_seen_at, enabled
```

The director or neural ranker may choose among road candidates, but movement legality still belongs to the server path system. Learned roads are advisory: they guide route selection, LOD projection, and human-like travel, but they do not teleport live visible bots or bypass path validation.

## Phase 6: Director Integration

Expose safe validated commands:

- `AssignQuestHub(botId, hubId, duration)`
- `AssignAbstractProfile(botId, profileId, duration)`
- `PromoteBot(botId, reason, maxDuration)`
- `DemoteBot(botId, targetMode)`
- `SetBotPersonality(botId, personalityVector, skillTier)`
- `RequestAuctionPost(botId, itemId, quantity, price, duration)`

The external director or neural model ranks options; the server validates and executes.

## Guardrails

- No direct external DB writes for gameplay state.
- No raw movement-key model for live bots.
- No arbitrary abstract quest completion until guide nodes and validation rules exist.
- Keep rollback/versioning for any learned model.
- Prefer code/data fixes for repeated deadlocks before adding bigger models.
