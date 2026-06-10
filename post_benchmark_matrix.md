# Post-Benchmark Recovery Matrix

This is the strict status matrix for work described in:

- [history_copilot.md:39724](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39724)
- [post_benchmark_full_ledger.md](/home/zuppier/tw/server_dev/tortoise-wow-pr79-replay/post_benchmark_full_ledger.md)

Status meanings:

- `Recovered in replay`: clearly present as replay-tree changes
- `Present / likely base`: present in code/config, but not clearly a replay-only delta
- `Partial`: some of the work is present, but not the full feature/fix set
- `Missing from replay`: not found in the replay tree in its intended form
- `Experimental / backed out`: base-server experiment that was later considered undesirable or reverted

## A. Benchmark / Scale / Runtime Stability

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Benchmark harness creation and hardening | `Recovered in replay` | `tools/bench_randombots.sh` exists and includes config targeting, cleanup, detach, staging logic | `tools/bench_randombots.sh` |
| Login-loop diagnostics | `Recovered in replay` | replay tree has modified `PlayerbotMgr.cpp` / `RandomPlayerbotMgr.cpp`; this was part of the benchmark recovery work | `PlayerbotMgr.cpp`, `RandomPlayerbotMgr.cpp` |
| Pending-login accounting fix | `Recovered in replay` | replay tree has modified `RandomPlayerbotMgr.cpp`; this was one of the core ceiling fixes | `RandomPlayerbotMgr.cpp` |
| Off-world cached bot pointers not treated as online | `Recovered in replay` | replay tree has modified `RandomPlayerbotMgr.cpp`; this was part of the online-count correction cluster | `RandomPlayerbotMgr.cpp` |
| Successful login clears `login` event immediately | `Recovered in replay` | replay tree has modified `RandomPlayerbotMgr.cpp`; this was part of the stale login marker cleanup | `RandomPlayerbotMgr.cpp` |
| Failed world-entry cleanup via `OnPlayerLoginError(...)` | `Recovered in replay` | `RandomPlayerbotFactory.cpp` calls `sRandomPlayerbotMgr.OnPlayerLoginError(guidlo)` and manager implements it | `RandomPlayerbotFactory.cpp`, `RandomPlayerbotMgr.cpp/.h`, `PlayerbotMgr.cpp` |
| `playerBots` map concurrency fix | `Recovered in replay` | replay tree has modified `PlayerbotMgr.cpp/.h`; this was one of the benchmark crash fixes | `PlayerbotMgr.cpp/.h` |
| Async bot-login callback handoff to world thread | `Recovered in replay` | replay tree has modified `PlayerbotMgr.cpp/.h`; this was one of the major replay restorations | `PlayerbotMgr.cpp/.h` |
| First-world-tick bot creation instead of startup-time creation | `Recovered in replay` | marker present: `Playerbots first world tick invoking RandomPlayerbotFactory::CreateRandomBots()` | `HostHooks.cpp`, `World.h`, `RandomPlayerbotFactory.cpp` |
| Script-id guard for bad creature AI IDs | `Recovered in replay` | replay tree has modified `ScriptMgr.cpp`; this was part of the high-count startup stabilization | `ScriptMgr.cpp` |
| Teleport ACK counter capture / reuse | `Recovered in replay` | `pendingTeleportAckCounter` exists in `PlayerbotAI.h/.cpp`; replay tree also modified `PlayerbotMgr.cpp` | `PlayerbotAI.cpp/.h`, `PlayerbotMgr.cpp` |
| Shared calculated-value locking | `Recovered in replay` | replay tree has modified `Value.h` | `Value.h` |
| `TriggerNode` first-relevance crash fix | `Recovered in replay` | replay tree has modified `Trigger.cpp/.h` and `ChooseRpgTargetAction.cpp`; `getFirstRelevance()` present | `Trigger.cpp/.h`, `ChooseRpgTargetAction.cpp` |
| Engine/trigger stability hardening | `Recovered in replay` | replay tree has modified `Engine.cpp/.h` | `Engine.cpp/.h` |
| Warden soft-fail instead of aborting server | `Recovered in replay` | replay tree has modified Warden files and `libanticheat.cpp` | `WardenModuleMgr.cpp/.hpp`, `libanticheat.cpp` |
| Auction house mirror crash fix | `Recovered in replay` | `GetAuctionsMap()` present and replay tree has modified AH files | `AuctionHouseMgr.cpp/.h` |
| MySQL header / PCH compatibility fix | `Recovered in replay` | replay tree has modified shared DB headers and `ThreadPool.cpp` | `DatabaseMysql.h`, `QueryResultMysql.h`, `ThreadPool.cpp` |
| PlayerBots CMake / install generation fixes | `Recovered in replay` | replay tree has modified top-level and PlayerBots CMake files | `CMakeLists.txt`, `src/modules/PlayerBots/CMakeLists.txt` |

## B. Config / Population / Logging

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Replay config drift fixes between `etc` and `bin` | `Recovered in replay` | both copies exist and were repeatedly aligned during replay work | `etc/aiplayerbot.conf`, `bin/aiplayerbot.conf`, `bin/mangosd.conf` |
| Active bot population tuning to 1000 / 2000 | `Recovered in replay` | both `etc` and `bin` configs are set to 2000 right now | `etc/aiplayerbot.conf`, `bin/aiplayerbot.conf` |
| Login ramp tuning | `Recovered in replay` | `RandomBotsMaxLoginsPerInterval = 5` in live replay configs | `etc/aiplayerbot.conf`, `bin/aiplayerbot.conf` |
| `bot_events.csv` enabled | `Recovered in replay` | `AiPlayerbot.AllowedLogFiles = bot_events.csv` in both replay configs | `etc/aiplayerbot.conf`, `bin/aiplayerbot.conf` |
| `RandomBotLoginWithPlayer` gating experiment | `Recovered in replay` | replay config now has it disabled again (`0`), but this toggle was part of the later runtime investigation | `etc/aiplayerbot.conf`, `bin/aiplayerbot.conf` |
| Action-speed / activity tuning | `Partial` | configs include benchmark/login tuning, but the full later “snappier actions” pass from the other tree is not clearly replayed here | `etc/aiplayerbot.conf`, `bin/aiplayerbot.conf`, `PlayerbotAIConfig.cpp` |

## C. Movement / Humanization / Bot Feel

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Human-like movement imperfection system | `Missing from replay` | no `HumanLike*` config/settings found in replay tree; `MovementActions.cpp` is not part of replay diff | `MovementActions.h/.cpp`, `MoveToRpgTargetAction.cpp`, config files |
| Steering noise / destination jitter | `Missing from replay` | no replay diff in movement action files; no config hooks found | `MovementActions.cpp` |
| AFK-like pauses during travel | `Missing from replay` | no `PauseChance`/`PauseMinMs`/`PauseMaxMs` style settings found in replay configs | `MovementActions.cpp`, config files |
| Goofy jumps / imperfect turns / non-perfect spin feel | `Missing from replay` | replay tree still has upstream jump strategies/config docs, but not the later custom humanization pass | `MovementActions.cpp`, `MoveToRpgTargetAction.cpp`, config files |
| Group spacing / anti-stacking feel | `Missing from replay` | no replay diff in movement spacing files; the later movement pass does not appear restored | `MovementActions.cpp`, `MoveToRpgTargetAction.cpp` |
| Movement-speed tracing for zombie walk | `Missing from replay` | no `MovementSpeedTrace` / `LogMovementSpeed` markers found in replay tree | `PlayerbotMgr.cpp/.h`, config files |
| Slow zombie-walk fix | `Missing from replay` | no replay diff in `MovementActions.cpp` or `ReviveFromCorpseAction.cpp`; the later walk-mode patch set is not restored here | `MovementActions.cpp`, `ReviveFromCorpseAction.cpp`, `MoveToRpgTargetAction.cpp` |
| Corpse-run walk-mode forcing / revive travel fixes | `Missing from replay` | no replay diff for `ReviveFromCorpseAction.cpp`; likely not yet replayed | `ReviveFromCorpseAction.cpp`, `ReachTargetActions.h` |
| Ranged bots circling / refusing to commit | `Missing from replay` | no replay diff in `MovementActions.cpp`; the LOS/hold-position patch does not appear present | `MovementActions.cpp` |

## D. Quest / Travel / RPG Behavior

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Quest travel target selection fix | `Partial` | replay tree has modified `TravelValues.cpp`, but key travel files like `TravelMgr.cpp`, `ChooseTravelTargetAction.cpp`, `MoveToTravelTargetAction.cpp`, `QuestAction.cpp`, `TalkToQuestGiverAction.cpp` are not in replay diff | `TravelValues.cpp`, plus likely-missing travel action files |
| Quest reward / completion reset behavior | `Missing from replay` | no replay diff in `QuestAction.cpp` / `TalkToQuestGiverAction.cpp` | `QuestAction.cpp`, `TalkToQuestGiverAction.cpp` |
| Travel manager / partition / destination tuning | `Partial` | some travel-related work is present indirectly, but most of the wider travel-file patch set is not in replay diff | `TravelValues.cpp`, likely missing `TravelMgr.cpp`, `ChooseTravelTargetAction.cpp` |

## E. Loot / Gathering / Shared Value / Object Safety

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Loot-template layout mismatch fix | `Recovered in replay` | replay tree has modified `LootValues.h/.cpp`; this includes the core loot mirror work | `LootValues.h/.cpp` |
| Shared value context hardening | `Recovered in replay` | replay tree has modified `SharedValueContext.h` | `SharedValueContext.h` |
| Named object context hardening | `Recovered in replay` | replay tree has modified `NamedObjectContext.h` | `NamedObjectContext.h` |
| Stale loot / hostile-observer / null observer fixes | `Partial` | replay tree has modified `AddLootAction.cpp` and `PlayerbotAI.cpp`, but `Object.cpp` is not modified in replay diff | `AddLootAction.cpp`, `PlayerbotAI.cpp`, possibly missing `Object.cpp` side |
| Vendor / item acquisition crash-path fixes | `Recovered in replay` | replay tree has modified `VendorValues.cpp` | `VendorValues.cpp` |

## F. Respawn / Corpse / Neutral Mob / Base-Server Combat Experiments

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Creature respawn/corpse tracing and timer experiments | `Experimental / backed out` | replay tree search shows `Creature.cpp` temp pacify markers exist, but `Creature.cpp` is not a replay diff target for the benchmark restore; this work was later treated as undesirable base-server divergence | `Creature.h/.cpp` |
| Neutral/passive target filtering for temp-pacified mobs | `Present / likely base` | `PossibleTargetsValue.cpp` references temp-pacified creatures, but this file is not modified in replay diff; likely already in recovered/base tree or from earlier recovery | `PossibleTargetsValue.cpp` |
| Base-server `Unit.cpp` combat-path changes | `Experimental / backed out` | not part of replay diff; history notes these were part of the passive/respawn experiment cluster and not all intended to stay | `Unit.cpp` |

## G. Social / Session / Miscellaneous

| Item | Status | Evidence / Notes | Main files |
|---|---|---|---|
| Synthetic-session `JoinChatChannels` guard | `Recovered in replay` | guard is present in `PlayerbotMgr.cpp`; this protects bots with no real socket | `PlayerbotMgr.cpp` |
| Broadcast / channel join companion change | `Present / likely base` | commented/adjusted join behavior exists in `BroadcastHelper.cpp`, but that file is not currently a replay diff target | `BroadcastHelper.cpp` |
| `SayAction` `WorldPacket` move/copy fix | `Recovered in replay` | replay tree has modified `SayAction.cpp` | `SayAction.cpp` |

## H. What This Means For Reapplying Everything

### Already back in replay and ready to preserve
- the benchmark harness
- the 1000/2000 login-scaling and scheduler fixes
- the major crash/stability fixes
- the Warden/AH/MySQL/CMake recovery work
- the shared-value / trigger / teleport / startup sequencing fixes

### Clearly still missing from replay
- the later human-like movement feature pass
- movement-speed tracing
- the slow-walk / corpse-run walk-mode cleanup pass
- the ranged anti-circle/orbit movement fix
- most of the broader quest-travel behavior patch set

### Things we should treat carefully instead of blindly replaying
- `Creature.cpp` respawn/corpse timing experiments
- `Unit.cpp` passive/neutral combat experiments
- any base-server combat-path changes that were later backed out
