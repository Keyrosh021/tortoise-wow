# PR79 Replay Change List

This is the actionable change list mined from [history_copilot.md](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md).

It is split into:
- `PR79 / replay relevant`: changes we should preserve in the replay tree
- `Earlier unrelated work`: changes from other repos that appeared earlier in the same chat history but are not part of the PR79 replay target

This is not a literal dump of every sentence in the transcript. It is the concrete patch checklist.

## PR79 / Replay Relevant

### 1. Interface import from the other tree
- Import Turtle barbershop and Turtle LFG support into PR79.
- Files:
  - `src/game/CMakeLists.txt`
  - `src/game/Handlers/ChatHandler.cpp`
  - `src/game/Objects/GameObject.cpp`
  - `src/game/WorldSession.cpp`
  - `src/scripts/CMakeLists.txt`
  - `src/scripts/ScriptLoader.cpp`
  - `src/game/Barbershop/TurtleBarbershopMgr.h`
  - `src/game/Barbershop/TurtleBarbershopMgr.cpp`
  - `src/game/LFG/TurtleLFGMgr.h`
  - `src/game/LFG/TurtleLFGMgr.cpp`
  - `src/scripts/miscellaneous/barbershop.cpp`
- History anchors:
  - [history_copilot.md:3152](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3152)
  - [history_copilot.md:3178](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3178)

### 2. Build flags and PlayerBots install/config generation
- Reconfigure PR79 with `BUILD_PLAYERBOTS=ON` and `ALLOW_TURTLE_ADDONS=ON`.
- Fix PlayerBots CMake so TurtleWoW generates `aiplayerbot.conf.dist`.
- Files:
  - `CMakeLists.txt`
  - `src/modules/PlayerBots/CMakeLists.txt`
- History anchors:
  - [history_copilot.md:3258](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3258)
  - [history_copilot.md:3310](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3310)
  - [history_copilot.md:4103](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:4103)
  - [history_copilot.md:4163](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:4163)

### 3. Install path / repo-local runtime layout
- Make install land inside the repo-local tree instead of a shared server dir.
- Expected runtime paths:
  - `bin/`
  - `etc/`
  - repo-local configs used consistently
- History anchors:
  - build/install work around [history_copilot.md:3294](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:3294)

### 4. Warden no-module soft-fail
- Prevent startup/login crash when Warden module assets are missing.
- Core idea:
  - expose module availability checks
  - do not build live Warden sessions when no module pack is loaded
  - allow session to continue with null/disabled Warden path
- Files:
  - `src/game/Anticheat/Warden/WardenModuleMgr.hpp`
  - `src/game/Anticheat/Warden/WardenModuleMgr.cpp`
  - `src/game/Anticheat/libanticheat.cpp`
  - later hardening also touched:
    - `src/game/Anticheat/Warden/Warden.cpp`
    - `src/game/Anticheat/Warden/WardenWin.cpp`
    - `src/game/Anticheat/Warden/WardenMac.cpp`
- History anchors:
  - [history_copilot.md:5428](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:5428)
  - [history_copilot.md:6237](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6237)
  - [history_copilot.md:6535](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6535)

### 5. Linux/MySQL header compatibility fix
- Fix MariaDB/MySQL header ordering / async API compile failures.
- Files:
  - `src/shared/Database/QueryResultMysql.h`
  - `src/shared/Database/DatabaseMysql.h`
  - `src/shared/ThreadPool.cpp`
- History anchors:
  - [history_copilot.md:5519](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:5519)
  - [history_copilot.md:6293](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6293)
  - [history_copilot.md:6373](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6373)

### 6. `SayAction` move-only packet compile fix
- Fix `WorldPacket` move/copy misuse in PlayerBots.
- File:
  - `src/modules/PlayerBots/playerbot/strategy/actions/SayAction.cpp`
- History anchor:
  - [history_copilot.md:5480](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:5480)

### 7. Teleport cache startup query/schema fix
- Fix startup failure in random teleport cache generation caused by bad creature template column names.
- File:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- History anchor:
  - [history_copilot.md:6660](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6660)

### 8. Auction house mirror crash fix
- Fix `AuctionHouseMgr::GetAuctionsMap(uint32 type)` so it maps type -> real DBC auction house entry instead of passing `nullptr`.
- Make `MirrorAh()` skip missing house maps cleanly.
- Files:
  - `src/game/AuctionHouse/AuctionHouseMgr.h`
  - `src/game/AuctionHouse/AuctionHouseMgr.cpp`
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- History anchors:
  - [history_copilot.md:6773](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6773)
  - [history_copilot.md:6787](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:6787)

### 9. `AiPlayerbot.*` config block added to active runtime configs
- Add the actual `AiPlayerbot.*` settings the module reads.
- This was needed because old configs only had legacy `PlayerBot.*` keys.
- Files:
  - `etc/aiplayerbot.conf`
  - `bin/aiplayerbot.conf`
  - in some stages `etc/mangosd.conf` / `bin/mangosd.conf` also got matching config fixes
- History anchors:
  - [history_copilot.md:35629](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35629)
  - [history_copilot.md:35788](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35788)
  - [history_copilot.md:35808](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35808)

### 10. Random teleport fallback fix
- If RPG teleport locations are empty, fall back to normal level-based teleporting instead of spamming "no locations available."
- File:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- History anchors:
  - [history_copilot.md:35732](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35732)
  - [history_copilot.md:35746](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35746)

### 11. Random bot creation/class-race / startup creation fixes
- Multiple fixes landed here:
  - class/race creation issues
  - startup creation cleanup
  - Night Elf racial / starter learn crashes
  - probe logging later removed after validation
- Files repeatedly touched:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotFactory.cpp`
  - `src/game/Objects/Player.cpp`
  - `src/game/Spells/SpellAuras.cpp`
  - `src/game/ScriptMgr.cpp`
- History anchors:
  - [history_copilot.md:35877](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35877)
  - [history_copilot.md:35954](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35954)
  - [history_copilot.md:36689](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:36689)
  - [history_copilot.md:36748](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:36748)
  - [history_copilot.md:37001](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37001)

### 12. Off-world spell/racial startup crash fixes
- Generalize startup bot-creation crash handling for spells learned while the bot is not safely in-world yet.
- Files:
  - `src/game/Objects/Player.cpp`
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotFactory.cpp`
- History anchors:
  - [history_copilot.md:36951](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:36951)
  - [history_copilot.md:36999](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:36999)

### 13. Null / stale world-object safety around hostile scans
- First guard pass:
  - `src/game/Objects/Object.cpp`
  - `src/modules/PlayerBots/playerbot/PlayerbotAI.cpp`
- Then stale loot-object refreshes:
  - `src/modules/PlayerBots/playerbot/strategy/actions/AddLootAction.cpp`
- Core idea:
  - re-resolve `WorldObject*` from GUID immediately before hostile/range checks
  - never call shared visibility/hostile helpers with stale/null observer pointers
- History anchors:
  - [history_copilot.md:37110](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37110)
  - [history_copilot.md:37252](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37252)
  - [history_copilot.md:38250](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:38250)
  - [history_copilot.md:39532](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39532)

### 14. Starter inventory overflow data fix
- Fix bad starter item amounts for non-stackable thrown weapons causing character creation failure / inventory-full errors.
- Files:
  - `sql/database_updates/20260604152500_world.sql`
- History anchors:
  - [history_copilot.md:37458](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37458)
  - [history_copilot.md:37474](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37474)

### 15. `ai_playerbot_random_bots` table / upsert / deadlock fixes
- Replace `DELETE + INSERT` with upsert logic.
- Add proper uniqueness/index shape for logical bot event rows.
- Files:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
  - `src/modules/PlayerBots/sql/characters/ai_playerbot_random_bots.sql`
  - `sql/database_updates/20260604161000_characters.sql`
  - `sql/char_updates/20260604161000_characters.sql`
- History anchors:
  - [history_copilot.md:37531](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37531)
  - [history_copilot.md:37661](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:37661)

### 16. Async bot-login chat-channel crash fix
- Prevent async bot login from mutating channel membership state.
- Practical result from history:
  - remove/skip auto `JoinChatChannels()` from the async login path
- File:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
- History anchors:
  - [history_copilot.md:38901](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:38901)
  - [history_copilot.md:38947](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:38947)

### 17. Level-based high-level bot placement filter
- Prevent high-level bots from teleporting into trivially low-level zones.
- File:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
- History anchors:
  - [history_copilot.md:39649](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39649)
  - [history_copilot.md:39673](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39673)

### 18. Benchmark harness
- Add and refine repeatable random-bot benchmark tooling.
- File:
  - `tools/bench_randombots.sh`
- Bench harness changes included:
  - detached launch
  - staged targets
  - summary reporting
  - cleanup
  - live metric checks
- History anchors:
  - [history_copilot.md:39724](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:39724)
  - [history_copilot.md:41282](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41282)
  - [history_copilot.md:41825](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41825)
  - [history_copilot.md:44823](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:44823)

### 19. Random bot scheduler / login ceiling / pending-login accounting fixes
- Big cluster of fixes in `RandomPlayerbotMgr.cpp` and `PlayerbotMgr.cpp`.
- Included:
  - targeted login diagnostics
  - gate probes
  - not counting pending logins as already-online
  - not treating off-world cached bot pointers as online
  - fixing stalls around ~44 / ~116 / ~120 online
  - login retry / retry poison cleanup
- Files:
  - `src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.h`
  - `etc/aiplayerbot.conf`
  - `tools/bench_randombots.sh`
- History anchors:
  - [history_copilot.md:41330](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41330)
  - [history_copilot.md:41512](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41512)
  - [history_copilot.md:41731](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:41731)
  - [history_copilot.md:42595](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:42595)
  - [history_copilot.md:43142](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:43142)
  - [history_copilot.md:43216](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:43216)
  - [history_copilot.md:44245](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:44245)

### 20. Failed bot login / failed enter world cleanup
- Fix repeated retries of bots that fail to enter world.
- Fix flag clearing / callback ordering around failed login.
- Add retry avoidance / cooldown behavior for pathological GUIDs.
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
  - `src/game/Handlers/CharacterHandler.cpp`
- History anchors:
  - [history_copilot.md:50119](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:50119)
  - [history_copilot.md:51671](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:51671)
  - [history_copilot.md:55207](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:55207)
  - [history_copilot.md:55535](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:55535)
  - [history_copilot.md:55603](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:55603)

### 21. World-thread handoff for bot login callback
- Move expensive / unsafe post-login work out of the async DB callback and drain it on the world thread.
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.h`
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
- This was one of the biggest stability steps in the replay.
- History anchors:
  - visible in the later login-callback fixes cluster around [history_copilot.md:44245](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:44245)

### 22. First-world-tick bot creation instead of early startup create
- Delay random bot creation until the first normal world tick rather than deep in world setup.
- Files:
  - `src/modules/PlayerBots/playerbot/HostHooks.cpp`
  - `src/game/World.h`
- This was part of the replay stabilization from the later history-backed phase.

### 23. Script ID guard during bot creation / world spawn
- Guard bad or out-of-range script IDs instead of hard crashing during bot creation/world startup.
- File:
  - `src/game/ScriptMgr.cpp`
- History anchor:
  - [history_copilot.md:35954](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:35954)

### 24. Teleport ACK counter capture / reuse
- Capture the real outgoing teleport counter and echo it back in the synthetic bot ACK instead of hardcoded `0`.
- Files:
  - `src/modules/PlayerBots/playerbot/PlayerbotAI.cpp`
  - `src/modules/PlayerBots/playerbot/PlayerbotAI.h`
  - `src/modules/PlayerBots/playerbot/PlayerbotMgr.cpp`
- History anchors:
  - [history_copilot.md:2478](/home/zuppier/tw/server_dev/tortoise-wow-pr79/history_copilot.md:2478) for the original concept
  - replay stabilization later reused the same fix pattern

### 25. Shared value locking
- Lock calculated/single-calculated value access to avoid concurrent reads/writes under higher bot counts.
- File:
  - `src/modules/PlayerBots/playerbot/strategy/Value.h`
- This was part of the replay’s 2k stabilization pass.

### 26. Trigger / RPG relevance safety
- Prevent `TriggerNode::getFirstRelevance()` crashes by caching first relevance and using null-safe handler access.
- Files:
  - `src/modules/PlayerBots/playerbot/strategy/Trigger.h`
  - `src/modules/PlayerBots/playerbot/strategy/Trigger.cpp`
  - `src/modules/PlayerBots/playerbot/strategy/actions/ChooseRpgTargetAction.cpp`
- This was part of the replay stabilization after the benchmark section.

### 27. Bot config / realm config drift fixes
- Keep `bin` and `etc` copies aligned.
- Replay-specific fixes included:
  - `bin/mangosd.conf` realm ID / world port sync
  - `aiplayerbot.conf` sync between `bin` and `etc`
- This matters because several failures were actually config drift, not code defects.

## Earlier Unrelated Work In The Same History

These appeared in the same chat file but belong to other trees, not the PR79 replay target:

### A. Earlier `tortoise-wow` custom companion-runtime work
- new `src/game/Playerbot/*` skeleton runtime
- `Player` / `WorldSession` bot packet plumbing
- GM add/remove bridge commands
- persistence system under `src/game/Playerbot/PlayerbotPersistence.*`
- socketless runtime teleport ACK fix in the older tree
- migration folder split `sql/char_updates`

### B. `tortoise-wow (paladin)` work
- Seal of Command twisting feature
- related `Unit.cpp`, `SpellModMgr.cpp`, `SpellAuras.cpp`, `CMakeLists.txt` work

## Best Replay Order

If we reapply in a clean replay again, the safest order is:
1. Interface import
2. CMake/build/install fixes
3. Warden soft-fail
4. MySQL header compatibility
5. PlayerBots config generation / active config sync
6. Teleport cache + AH mirror startup crash fixes
7. ScriptMgr / first-world-tick bot creation
8. Starter item SQL + random bot event table SQL
9. Async login / channel join / world-thread handoff fixes
10. Scheduler / pending-login / retry-loop fixes
11. Stale pointer / hostile scan / value-lock / trigger safety fixes
12. Benchmark harness and config tuning

## Current Recommendation

Use this file as the checklist, and replay only the `PR79 / Replay Relevant` section.
Do **not** re-import the earlier `tortoise-wow` custom companion runtime or the paladin work into the replay tree.
