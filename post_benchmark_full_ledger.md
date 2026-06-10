# Post-Benchmark Full Ledger

This is the fuller ledger for everything in `history_copilot.md` after the benchmark section starts at:

- [history_copilot.md:39724](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39724)

Goal of this file:
- include the benchmark/stability fixes
- include user-requested movement / behavior features
- include respawn / corpse / neutral-mob / bot-population tuning work
- note when something was experimental or later backed out

This is intentionally broader than the short stability checklist.

## A. Benchmark Harness And Population Scaling

### A1. Benchmark harness creation and hardening
- Added and iterated `tools/bench_randombots.sh`
- Key fixes:
  - target the installed `etc/aiplayerbot.conf` instead of the wrong copy
  - avoid stacking multiple `mangosd` processes
  - detach `mangosd` correctly from the shell
  - kill detached `./mangosd -c mangosd.conf` processes during cleanup
  - stage counts and collect summaries/samples
- Files:
  - `tools/bench_randombots.sh`
- Anchors:
  - [39724](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39724)
  - [41282](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41282)
  - [41825](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41825)
  - [44823](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:44823)

### A2. Login-loop visibility / diagnostics
- Added rate-limited diagnostics to tell whether startup was:
  - paused by DB delay
  - blocked by pending logins
  - silently filtered by existing bot pointers
  - crashing after the first few `AddPlayerBot()` calls
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- Anchors:
  - [41330](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41330)
  - [41512](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41512)
  - [42595](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:42595)

### A3. Pending-login accounting fix
- Core scheduler bug:
  - pending `login` markers were being counted as if bots were already online
  - that created fake ceilings like `44` and later `116`
- Fixes:
  - separate actual online bots from scheduled/pending logins
  - do not let pending markers consume the target budget
- Files:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- Anchors:
  - [43142](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:43142)
  - [43216](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:43216)

### A4. Off-world cached bot pointers not treated as online
- Another scheduler bug:
  - cached/off-world bot pointers were causing silent `continue`s in the login loop
- Fix:
  - off-world cached pointers are no longer treated as already-online bots
- Files:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- Anchors:
  - [3493](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3493)
  - [5014](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:5014)

### A5. Successful login clears `login` event immediately
- Prevent stale `login` rows for already-online bots
- Files:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- Anchors:
  - [6555](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6555)
  - [6688](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6688)

### A6. Failed world-entry cleanup and bad-bot retry poisoning
- Failed callbacks used to just log and return
- That left scheduler state poisoned by bad bots like `4699`
- Fixes evolved through:
  - clear `login` on failure
  - then route failure through `sRandomPlayerbotMgr.OnPlayerLoginError(...)`
  - clear `add`, `login`, and remove the guid from current active bot tracking
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - `src/game/Handlers/CharacterHandler.cpp`
- Anchors:
  - [10394](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:10394)
  - [11946](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:11946)
  - [15428](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:15428)

### A7. `playerBots` map concurrency crash
- Crash around ~`106` online bots
- Stack pointed into `PlayerbotHolder::OnBotLogin()` / `std::map` rebalance
- Fix:
  - add dedicated mutex
  - lock insert / erase / read / broad iteration paths, not just one crash line
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.h`
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
- Anchors:
  - [4496](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:4496)
  - [4522](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:4522)
  - [4530](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:4530)

### A8. Config target tuning during benchmark
- Changed active target counts during scaling tests
- `etc/aiplayerbot.conf` was repeatedly tuned through:
  - 200
  - 300
  - 500
  - 1000
  - 2000
- Files:
  - `etc/aiplayerbot.conf`
- Anchors:
  - [3588](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3588)
  - [7646](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:7646)
  - [17912](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:17912)
  - [18212](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:18212)

## B. Movement / Humanization / Behavior Features

These are the user-requested bot “feel” features you explicitly asked for later.

### B1. Human-like movement imperfection system
- Added first-pass humanization layer for random/free bots
- Behavior goals from history:
  - not walk perfect straight lines
  - some steering noise / destination jitter
  - group spacing instead of pin-point stacking
  - goofy jumps / turns / imperfect facing
  - pauses / AFK-like standing
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/actions/MovementActions.h`
  - `src/modules/PlayerBots/playerbot/strategy/actions/MovementActions.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/MoveToRpgTargetAction.cpp`
- Config:
  - `etc/aiplayerbot.conf`
  - `bin/aiplayerbot.conf`
- Anchors:
  - [58162](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58162)
  - [58182](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58182)
  - [39626](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39626)

### B2. Walk-mode / slow-zombie-walk fixes
- User-reported issue:
  - bots sometimes slow-walked like NPCs
  - especially corpse runs / ambient travel
- Fixes:
  - force ghost corpse-runs to clear walk mode
  - force generic dispatched movement to refuse walk mode while ghosted
  - additional RPG/movement walk-mode tuning
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/actions/MovementActions.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/ReviveFromCorpseAction.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/MoveToRpgTargetAction.cpp`
- Anchors:
  - [58654](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58654)
  - [41790](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41790)

### B3. Corpse-run / revive behavior
- Fixes around corpse-run target reach and revive flow
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/actions/ReviveFromCorpseAction.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/ReachTargetActions.h`
  - `src/game/Movement/TargetedMovementGenerator.cpp`
- Anchors:
  - [58654](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58654)
  - [58688](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58688)

### B4. Movement speed tracing
- Added movement-speed logging to track the source of zombie-walk behavior
- Logged walk/run state and movement source in bot events
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.h`
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
- Related config:
  - `etc/aiplayerbot.conf`
  - `bin/aiplayerbot.conf`
- Anchors:
  - [42918](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:42918)
  - [42928](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:42928)

### B5. Ranged-bot orbit / “running circles around target” fix
- User-reported issue:
  - ranged bots stare at a target, keep repositioning, never cast/commit
- Fix:
  - if ranged bot already has valid ranged position and LOS, stop moving and keep target
  - do not re-enter chase/orbit logic unnecessarily
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/actions/MovementActions.cpp`
- Anchors:
  - [58982](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58982)

### B6. Group spacing / anti-stacking feel
- User asked for mobs/bots in a group to not walk on the exact same point
- Movement humanization and spacing logic were intended to address that in the bot movement layer
- Main touched files:
  - `MovementActions.cpp`
  - `MoveToRpgTargetAction.cpp`
- Note:
  - the transcript is clearer about bot spacing than about creature/mob native formation changes

## C. Quest / Travel / RPG Behavior Fixes

### C1. Quest travel target selection fix
- Strong post-benchmark work around starter-zone stalling and `TravelTargetSelectFailed`
- Core issue:
  - quest IDs were being misused in travel partition/entry logic
  - starter bots got stuck with `NoTarget`
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/generic/TravelStrategy.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/ChooseTravelTargetAction.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/MoveToTravelTargetAction.cpp`
  - `src/modules/PlayerBots/playerbot/TravelMgr.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/values/TravelValues.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/TalkToQuestGiverAction.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/QuestAction.cpp`
- Anchors:
  - [58915](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58915)
  - [58927](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:58927)
  - [59857](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:59857)
  - [60284](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:60284)
  - [63248](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:63248)

### C2. Quest reward / completion behavior
- Added reward/turn-in reset and travel-expiry handling around finished quests
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/actions/QuestAction.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/TalkToQuestGiverAction.cpp`
- Anchors:
  - [59430](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:59430)
  - [59446](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:59446)

### C3. Travel manager / population / node fixes
- Additional tuning of named travel destinations, partitions, and selection logic
- Files:
  - `src/modules/PlayerBots/playerbot/TravelMgr.cpp`
  - `src/modules/PlayerBots/playerbot/AiFactory.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/values/TravelValues.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/ChooseTravelTargetAction.cpp`
- Anchors:
  - [59573](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:59573)
  - [61594](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:61594)
  - [62915](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:62915)

## D. Loot / Gathering / Shared Value Fixes

### D1. Loot mirror / loot-template layout mismatch
- Large stability issue in loot value path
- Fix included updating `LootValues.h` to match real `LootTemplate::LootGroup` layout
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/values/LootValues.h`
  - `src/modules/PlayerBots/playerbot/strategy/values/LootValues.cpp`
- Anchors:
  - [30306](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:30306)
  - [29243](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:29243)
  - [31317](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:31317)
  - [36521](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:36521)

### D2. Shared value context / named object context hardening
- Shared-value and context safety fixes to prevent invalid shared lookups
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/values/SharedValueContext.h`
  - `src/modules/PlayerBots/playerbot/strategy/NamedObjectContext.h`
- Anchors:
  - [35407](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35407)
  - [35481](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35481)

### D3. Loot / hostile-observer / stale object fixes
- Fix stale object and null observer paths for loot and gathering scans
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/actions/AddLootAction.cpp`
  - `src/modules/PlayerBots/playerbot/PlayerbotAI.cpp`
  - `src/game/Objects/Object.cpp`
- This cluster started pre-benchmark-adjacent but continued to matter afterward

## E. Respawn / Corpse / Combat Behavior Changes

These are the parts you asked about later around corpses, respawn, passive mobs, etc.

### E1. Respawn / corpse debug and behavior experiments
- Added respawn/corpse tracing
- Touched base-server creature logic around:
  - corpse timer handling
  - respawn timing
  - corpse removal when respawn becomes due
- Files:
  - `src/game/Objects/Creature.h`
  - `src/game/Objects/Creature.cpp`
- Anchors:
  - [63488](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:63488)
  - [68309](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:68309)
  - [68343](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:68343)
  - [68458](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:68458)
  - [68470](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:68470)
- Important note:
  - some of this was later considered undesirable base-server divergence and was backed out

### E2. Neutral / passive target filtering
- PlayerBots-side fix to avoid attacking temp-pacified or invalid targets
- File:
  - `src/modules/PlayerBots/playerbot/strategy/values/PossibleTargetsValue.cpp`
- Anchor:
  - [69940](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:69940)

### E3. Unit / combat-path experiments
- Base-server combat-path changes in `Unit.cpp`
- Anchors show multiple patches:
  - [70088](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:70088)
  - [72629](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:72629)
  - [73622](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:73622)
  - [73640](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:73640)
- Important note:
  - these were part of the neutral/passive/respawn experiments and not all were intended to stay

## F. Config / Activity / Logging Features

### F1. Activity / action-speed tuning
- Multiple config changes to make bots more active/snappier
- Included:
  - lower delays
  - more frequent action loops
  - activity tuning
- Files:
  - `etc/aiplayerbot.conf`
  - `bin/aiplayerbot.conf`

### F2. `bot_events.csv` and bot logging
- Config changes to allow logging to `bot_events.csv`
- Later movement-speed trace and action diagnostics depended on this
- Files:
  - `etc/aiplayerbot.conf`
  - `bin/aiplayerbot.conf`

### F3. Replay config drift fixes
- Repeated fixes to keep `etc` and `bin` copies aligned
- Included toggles for:
  - `RandomBotLoginWithPlayer`
  - bot count targets
  - movement humanization settings
  - log files

## G. What Was User-Requested Feature Work vs Pure Stability Work

### Explicit feature work you asked for
- human-like movement imperfections
- less perfect grouping / spacing
- jumps / turns / AFK-like pauses
- movement-speed tracing to catch zombie-walk behavior
- ranged-bot anti-circle/orbit fix
- corpse-run / slow-walk fixes
- quest-travel / target-selection fixes

### Mostly stability / scale work
- benchmark harness
- pending login accounting
- `playerBots` map locking
- failed world-entry cleanup
- stale login marker cleanup
- shared value locking
- trigger relevance safety
- Warden soft-fail
- auction house mirror crash

### Mixed / experimental base-server work
- respawn/corpse logic in `Creature.cpp`
- neutral/passive combat-path work in `Unit.cpp`
- these need special care because some were later reverted or intentionally avoided
