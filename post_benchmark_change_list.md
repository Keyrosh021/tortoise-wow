# Post-Benchmark Change List

This file tracks only the changes made after the benchmark phase began in [history_copilot.md](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39724).

It excludes the earlier PR79 import/build/startup work.

## Benchmark / Harness

- `tools/bench_randombots.sh`
  - target installed `etc/aiplayerbot.conf`, not the wrong config copy
  - better detached `mangosd` launch
  - better cleanup for detached `./mangosd -c mangosd.conf`
  - staged count runs and summary output
  - extra diagnostics for stalls/crashes

## Scheduler / Login-Ramp Fixes

- `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
  - do not treat off-world cached bot pointers as already online
  - track pending `login` markers separately from actual online bots
  - stop counting pending logins as filled slots in the online budget
  - clear successful random-bot `login` event immediately on attach
  - candidate / loop fixes around stalled startup selection
  - level 40+ teleport filter to avoid trivial low-level zones

- `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - extra login diagnostics
  - world-thread safety around bot login processing
  - failed world-entry cleanup no longer leaves poisoned `login` state
  - later changed failed login cleanup to route through `sRandomPlayerbotMgr.OnPlayerLoginError(...)`

- `src/modules/PlayerBots/playerbot/PlayerbotMgr.h`
  - holder state additions for the login/scheduler fixes

- `etc/aiplayerbot.conf`
  - staged benchmark target changes: `200`, `300`, `500`, `1000`, `2000`
  - tuning around login ramp / intervals during testing

## Concurrency / Crash Fixes

- `src/modules/PlayerBots/playerbot/PlayerbotMgr.h`
- `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - mutex/serialization for the `playerBots` map
  - fix `OnBotLogin()` / map rebalance crash around ~106 online bots
  - lock broad reads / iterations / erases, not just one insert site

- `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
  - fix stale marker plateau around ~116
  - fix repeated failed callback poison around bot `4699`

## Failed Bot Login / Enter-World Cleanup

- `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - failed callback no longer only logs and returns
  - it now clears scheduler state properly
  - later routed through `OnPlayerLoginError(...)` for full cleanup

- `src/game/Handlers/CharacterHandler.cpp`
  - supporting fixes around the failed-login / world-entry path while tracing bad bots like `4699`

## Replay-Era Stability Fixes After Benchmark Success

These were later history-backed fixes after the benchmark targets were basically recovered:

- `src/modules/PlayerBots/playerbot/HostHooks.cpp`
- `src/game/World.h`
  - move random bot creation to first normal world tick

- `src/game/ScriptMgr.cpp`
  - guard bad/out-of-range script IDs during bot creation/startup

- `src/modules/PlayerBots/playerbot/PlayerbotAI.cpp`
- `src/modules/PlayerBots/playerbot/PlayerbotAI.h`
- `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - teleport ACK counter capture/reuse instead of hardcoded `0`

- `src/modules/PlayerBots/playerbot/strategy/Value.h`
  - lock calculated value access

- `src/modules/PlayerBots/playerbot/strategy/Trigger.h`
- `src/modules/PlayerBots/playerbot/strategy/Trigger.cpp`
- `src/modules/PlayerBots/playerbot/strategy/actions/ChooseRpgTargetAction.cpp`
  - cache first relevance / null-safe trigger access to stop `TriggerNode` crashes

- `etc/aiplayerbot.conf`
- `bin/aiplayerbot.conf`
  - replay-only config drift fixes, including `RandomBotLoginWithPlayer` toggles during login debugging

## What Actually Moved The Ceiling

The biggest post-benchmark wins were:

1. `bench_randombots.sh` detached launch / cleanup fixes
2. `RandomPlayerbotMgr.cpp` pending-login accounting fixes
3. `PlayerbotMgr.cpp/.h` `playerBots` map locking
4. successful-login `login` flag clearing
5. failed-login cleanup routed through `OnPlayerLoginError(...)`

Those are the changes that moved the server from:
- fake `0` / `44` harness results
- crash at ~`106`
- plateau around ~`116`

to:
- stable `200`
- then `300`
- and later replay work toward `1000` / `2000`
