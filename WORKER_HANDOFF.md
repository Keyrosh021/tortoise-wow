# Playerbot Quality Project — Worker Handoff

You are taking over autonomous work on a **Turtle WoW (mangos-zero, WoW 1.12) private server with playerbots**. The mission: make the bots behave like **real human players — constantly active, no standing around, no obvious "bot tells"** — validated by MEASUREMENT and by what's visible in-game. This is a **goal-driven, no-stopping** task: keep working, experiment, measure, iterate. Do NOT ask the human what to do next; you are in charge.

---

## 1. THE GOAL (from the human, verbatim intent)

Bots should "machine-gun" activities one after another — pull the next mob before the current dies, immediately act on a chosen target, ~80% movement uptime, <10% idle. Real-player profile targets:
- **idle < 10%**, **movement uptime ~80%**, **active% ~90%**
- APM 30–45 (BUT SEE CAVEAT below — this metric has a lower ceiling for bots)
- Quest completion every 4–6 min; XP/hr ~18–25k
- Fast players win by eliminating downtime + travel, not faster combat.

**HARD RULE — MEASURE ACTIONS, NOT INTENTIONS.** An "action" = externally visible to another player (spell cast that fires, melee swing, real movement/position change, loot taken, emote). NOT visible / must NOT count: choosing a target, "rpg work", picking travel, filler `drink`/`add all loot`/`choose rpg target` that return true but do nothing. Guard every metric against false-positives that make numbers look good while bots stand around. (The built-in `apm` field is BROKEN — it counts any action returning true, fillers included: PlayerbotAI.cpp:4228. Use the supervisor trace instead.)

**APM CAVEAT:** the honest visible-action APM (casts+swings+loots) ceilings around ~10–15 even for great bots, because Classic combat is slow (1.5s GCD) and bots don't generate movement-keystroke APM the way humans do. The human's "30–45 APM" is mostly movement keystrokes. So the TRUE machine-gun metric is **active%** (move+cast+melee+loot, target ~90%) and **idle%** (target <10%), NOT the raw APM number.

---

## 2. REPO / PROJECT LOCATION

- **Repo:** `/home/zuppier/tw/server_dev/tortoise-wow-pr79-replay`
- **Branch:** `bots`   **Commit at handoff:** `76c0332f` (+ uncommitted working changes from this session — see §7)
- **Core is C++ (mangos-zero).** Playerbot module: `src/modules/PlayerBots/playerbot/`
- **Build dir:** `build/`   **Runtime dir:** `bin/` (has `mangosd`, `realmd`, `*.conf`, extractor tools)
- **Logs:** `logs/`   **Navmesh/collision data:** `data/mmaps`, `data/vmaps`, `data/maps`
- **Memory (persistent facts):** `/home/zuppier/.claude/projects/-home-zuppier-tw-server-dev/memory/` (index in `MEMORY.md`)

---

## 3. INFRASTRUCTURE — HOW TO RUN EVERYTHING

The box sometimes reboots/crashes; when it does, BOTH MariaDB and mangosd are down. Bring them up in order.

### 3a. MariaDB (user-level instance — start FIRST)
```bash
cd /home/zuppier/tw/server_dev/tortoise-wow-pr79-replay/db
bash start_mariadb_tortoise.sh      # idempotent; starts mariadbd on 127.0.0.1:3306
```
- Creds: **user `root`, pass `mangos`**, TCP `127.0.0.1:3306`, socket `db/dbs/tortoise/run/mariadb.sock`
- Databases: `tw_logon` (realmd), `tw_world`, `tw_characters`. **Learning/telemetry tables live in `tw_world`.**
- Check: `mysqladmin -h127.0.0.1 -P3306 -uroot -pmangos ping`

### 3b. The server supervisor (auto-restart loop for mangosd)
mangosd is run under a **detached bash `while true` loop** that respawns it on exit (`tail -f /dev/null | ./bin/mangosd >> boot.log`). If the supervisor is dead (check: `pgrep -f 'while true.*mangosd'`), relaunch it:
```bash
cd /home/zuppier/tw/server_dev/tortoise-wow-pr79-replay
setsid bash -c 'cd /home/zuppier/tw/server_dev/tortoise-wow-pr79-replay; while true; do echo "=== START $(date +%T) ===" >> boot.log; tail -f /dev/null | ./bin/mangosd >> boot.log 2>&1; echo "=== EXIT $? $(date +%T) ===" >> boot.log; sleep 3; done' >/dev/null 2>&1 &
disown
```
- **World port: 8091** (mangosd). Realmd is separate (`realmd`, port 3724) — usually already running.
- The real player account is **Koorosh** (account 4).
- **mangosd's process `comm` is "MainThread", NOT "mangosd"** — `pgrep -x mangosd` FAILS. Also VS Code's node procs ALSO show comm=MainThread. **Resolve the live mangosd reliably with `fuser bin/mangosd`.**

### 3c. Verify it's up
```bash
ss -ltn | grep 8091            # world port open == accepting logins
fuser bin/mangosd              # live mangosd pid
tail -f boot.log               # boot progress
```

---

## 4. BUILD & DEPLOY WORKFLOW (follow exactly)

**Build (incremental):**
```bash
cd /home/zuppier/tw/server_dev/tortoise-wow-pr79-replay
cmake --build build -j 6 --target mangosd 2>&1 | grep -iE "error:|Built target mangosd"
```

**Deploy — USE `cmake --install`, NOT manual `cp`** (the human corrected this; manual cp races the supervisor + hits "text file busy"). A running mangosd holds `bin/mangosd` busy, so you MUST stop it first. Full safe restart:
```bash
SUP=$(pgrep -f 'while true.*mangosd' | head -1)
MG=$(fuser bin/mangosd 2>/dev/null | tr -d ' ')
kill -STOP $SUP                      # freeze supervisor so it can't respawn mid-swap
kill -TERM $MG                       # graceful shutdown
for i in $(seq 1 18); do kill -0 $MG 2>/dev/null || break; sleep 1; done
kill -0 $MG 2>/dev/null && kill -9 $MG   # SEE NOTE
for i in $(seq 1 15); do st=$(ps -o stat= -p $MG 2>/dev/null|tr -d ' '); [ -z "$st" ] || [ "${st:0:1}" = "Z" ] && break; sleep 1; done
cmake --install build 2>&1 | grep -i "Installing.*mangosd"
md5sum bin/mangosd build/src/mangosd/mangosd   # confirm identical
kill -CONT $SUP                      # resume supervisor -> it respawns the NEW binary
```
- **NOTE on graceful shutdown:** it HANGS at "Stopping network threads..." for MINUTES *after* the world-save completes. The world/player save happens BEFORE that line, so `kill -9` once you see "Stopping network threads" (or after ~18s) is SAFE. Don't wait it out.
- `cmake --install` only writes `.conf.dist` templates, NEVER the live `.conf` files — your config edits are safe.
- Boot takes ~1–3 min (loads 7000+ bot chars, builds travel nodes). Wait for port 8091.
- **Shell has `set -e` behavior in some scripts:** guard `pkill`/`kill` no-match with `|| true`.

---

## 5. THE MEASUREMENT SYSTEM (this is how you know if a fix worked)

### Config (bin/aiplayerbot.conf) — currently:
- `MinRandomBots=50 MaxRandomBots=50` (deliberately SMALL so every bot is watchable)
- `SupervisorMode=1` — keeps the whole 50-bot fleet at FULL foreground cadence even with NO player online, and writes the honest per-second trace. (Custom flag added this session.)
- `EnableActionLog=1`, `LODColdUpdateMs=0` (LOD dormancy off), `AllowedLogFiles=bot_events.csv`

### `logs/supervisor.csv` — THE honest metric (written by `SupervisorTrack` in PlayerbotAI.cpp)
Per-second per-bot classification of EXTERNALLY-VISIBLE state, plus 60s summaries:
- Row type `S` (per second): `ts,S,name,Llvl,mMAP,x|y,vis=STATE,intend=ACTION,combatN,manaN`
  - `vis=` is one of: **MOVE** (real position delta >1.5y), **CAST** (spell in progress), **MELEE** (in combat, victim in reach), **LOOT**, **REGEN** (sitting/eat/drink), **DEAD**, **IDLE** (none of the above = the enemy). `intend=` is the last-executed action name (the INTENTION — compare against `vis` to catch "intends X but visibly IDLE").
- Row type `M` (per 60s): `...,M,name,...,secs=N,apm~N,castsPM,swingsPM,lootsPM,move%,cast%,melee%,loot%,regen%,idle%,active%,combat%,netMove` (`netMove`=net displacement over the window, so "moving flag but stuck in place" can't fake movement).

**Aggregate idle%/move% example (latest M per bot, from a time window):**
```bash
awk -F, '$1>="2026-07-01 17:00" && $2=="M"{n=$3;row[n]=$0}
END{for(k in row){c++;l=row[k];i=m=0;split(l,a,",");for(j=1;j<=length(a);j++){t=a[j];
 if(t~/idle%=/){x=t;sub(/.*idle%=/,"",x);i=x}else if(t~/move%=/){x=t;sub(/.*move%=/,"",x);m=x}}si+=i;sm+=m}
 printf "idle%%=%.0f move%%=%.0f (bots=%d)\n",si/c,sm/c,c}' logs/supervisor.csv
```

### MEASUREMENT DISCIPLINE (learned the hard way this session)
- **8-minute post-restart snapshots have ±5–7 point NOISE** (boot-churn: 50 bots respawning/corpse-running). This is LARGER than most fix effects. Do NOT trust a single 8-min snapshot to attribute a fix.
- **Use LONG windows (20–45 min, past boot-churn) and/or CLEAN CUMULATIVE signals.** The best clean signals are in `bot_events.csv` `BotStatsSnapshot` rows: **`money=`** (gold — cumulative, zero noise) and **`xp=`/`level=`** (progression). If gold/xp are RISING, the bot is genuinely looting/killing. Flat = broken/stuck.
- **Prove each fix FIRES** before believing it. Custom events are logged to `bot_events.csv` (see §6), and several fixes write always-on files (`logs/freeze_fix.csv`, `logs/antiidle.csv`, `logs/botz_fix.csv`).

### Key log files
- `logs/supervisor.csv` — honest per-second/per-minute visible-activity trace (PRIMARY).
- `logs/bot_events.csv` — `BotStatsSnapshot` (money/xp/quests/moving/combat per bot), plus whitelisted custom events.
- `logs/bot_deaths.csv` — every death w/ killer. **WARNING: time-only timestamps (no date) + huge cross-run history — do NOT time-filter it naively.**
- `logs/freeze_fix.csv`, `logs/antiidle.csv`, `logs/botz_fix.csv` — always-on proof that FreezeNudge / AntiIdle / underground-Z fixes fired.
- `logs/bot_thoughts.log` — rich per-idle-bot "why am I stuck" line (only near a real player).

### Event logging gate
Custom events via `sPlayerbotAIConfig.logEvent(...)` are DOUBLE-gated: `hasLog("bot_events.csv")` (on) AND a hardcoded whitelist in `PlayerbotAIConfig.cpp::shouldLogBotEvent`. To make a NEW event appear, add its name to the `kept[]` array there. Currently whitelisted: BotStatsSnapshot, HostileCastDiag, SpellCastFailedTrace, TravelFrozenAbandoned, PostReviveSafetyHop, FleeStraightLineEscape, MovementPathRejected, RpgTargetPathRejectedDrop.

---

## 6. HARD CONSTRAINTS / RULES (do not violate)

1. **NEVER run the mmap/navmesh generator (`MoveMapGen`) on THIS machine — it crashes the whole box.** Navmesh regen is done on a SEPARATE Windows machine and copied over. (bigBaseUnit navmesh is already live; see §7.)
2. **NEVER teleport a bot where a real player can see it** (immersion). All teleport-based unstuck (FreezeNudge, post-revive hop) is already gated on `!ai->HasPlayerNearby()`. Keep it that way. A stuck bot standing looks like a player pausing; a teleporting bot is unmistakably a bot.
3. **Measure ACTIONS not INTENTIONS** (see §1). Don't fool yourself with the broken `apm` field or noisy 8-min snapshots.
4. **Don't churn restarts.** Each restart re-injects boot-churn noise. Batch related fixes, deploy once, measure over a long window.
5. **Don't restart mid-watch if the human is actively watching** without telling them (they get disconnected). If autonomous (no human online), restart freely.
6. Revert failed experiments cleanly (this session reverted a "reliable-roam straight-line walk" that raised DEAD 15→18% by walking bots into mobs — see §7).

---

## 7. WHAT'S BEEN DONE (this session — working changes on top of commit 76c0332f, NOT yet committed)

**Proven working (verify via the cited signal):**
- **LOOT FIX (biggest recent win)** — `src/modules/.../strategy/actions/LootAction.cpp`. Bots earned **ZERO gold** because `HasActiveCombatPressure` counted a merely-queued next grind target as "combat," so loot was deferred AND the deferral branch *permanently abandoned* the corpse (`Remove(guid)`). Fixed: (a) a queued target no longer blocks looting (removed HasAttachedLiveTarget + HasLiveHostileTarget checks, kept combat-flag + has-attackers), (b) combat deferral now KEEPS the corpse to loot at downtime. RESULT: fleet went 0% → 31%+ gaining gold (climbing). VERIFY via `money=` rising in BotStatsSnapshot.
- **FreezeNudge** (PlayerbotAI.cpp `FreezeNudge`) — physically NearTeleportTo's a genuinely-stuck bot (net disp <8y for >12s) to nearby valid ground. Gated `!HasPlayerNearby`. Fires heavily (proves how stuck the fleet is). Logs `logs/freeze_fix.csv`.
- **Flee straight-line escape** (MovementActions.cpp, autonomous flee branch) — when pathfinding flee fails (cornered/off-mesh), bot runs straight away (noPath spline, animated, immersion-safe). Cut death-loops. Event `FleeStraightLineEscape`.
- **Travel/RPG freeze breakers** (MoveToTravelTargetAction.cpp `TravelMoveStuck`, MoveToRpgTargetAction.cpp `RpgMoveStuck`) — bot-keyed net-displacement; abandon+blacklist a destination the bot can't path to (was: stood "moving" 35–69s). Event `TravelFrozenAbandoned`.
- **Post-revive safety hop** (ReviveFromCorpseAction.cpp) — a solo bot that reses on top of its killer hops ~25y away (validated ground). Gated `!HasPlayerNearby`. Event `PostReviveSafetyHop`.
- **Root float fix** (Unit.cpp `UpdateSplineMovement`) — on spline arrival, settle a bot to the walkable surface (bots have no gravity, so navmesh endpoints left them floating → broke next pathfind). Bots-only, exempts taxi/fly/swim/transport.
- **Underground Z-correction** (PlayerbotAI.cpp `CorrectBotZ`) — snap frozen-underground bots to terrain. Logs `botz_fix.csv`.
- **Anti-idle** (PlayerbotAI.cpp `AntiIdleAction`) — idle bot (>5s, no active travel, not combat/cast/loot/sit) is forced to `attack anything` else `move random`. Logs `logs/antiidle.csv`. NOTE: a straight-line-WALK fallback was tried and REVERTED (raised DEAD).
- **No fake idle jumps** — `RpgStrategy.cpp::RpgJumpStrategy` "random jump" trigger DISABLED. The jump rendered as a teleport-float (`JumpAction::DoJump` sends the jump packet then `bot->Relocate(highestPoint)` snaps the server pos to the arc apex — observers see a bot pop up and freeze mid-air). A REAL jump needs a parabolic `MoveSplineInit::SetParabolic` rewrite of DoJump (not done — riskier).
- **Supervisor mode + honest trace** — `SupervisorTrack`, `AiPlayerbot.SupervisorMode` config.
- **bigBaseUnit navmesh** — `data/mmaps` was regenerated with `--bigBaseUnit true` on the Windows box and swapped in (old default backed up at `data/mmaps.default_backup`). Gave ~19% fewer stuck events. LIVE.

**Reverted / not done:** reliable-roam straight-line walk (DEAD regression); real parabolic jump; travel-router rewrite (tried 3x, all noise/regression — it's tangled with navmesh gaps).

---

## 8. CURRENT STATE (clean, low-noise signals)

- **idle ~18–21%, move ~46–50%, DEAD ~11%** (long-window; DEAD is lower than noisy snapshots suggested).
- **Gold: 0% → 31% of fleet gaining (climbing)** — loot fixed.
- **XP: ~47% of fleet gaining** — about HALF the fleet actively grinds/kills fine.
- **Quests: ~4% progressing (2/51)** — QUESTING IS BROKEN. This is the deep root of the stuck half.
- Combat-idle turned out to be mostly NORMAL GCD gaps (nukes are NOT failing) — not a big lever.

**Diagnosis:** ~half the fleet grinds fine (kills, now loots, gains XP). The other half is stuck/idle/traveling because the **travel-router can't reliably route bots to quest objectives** (many objectives are behind navmesh gaps → router thrashes → COOLDOWN/EXPIRED with null destination → bot idles). Bots can GRIND but can't QUEST.

---

## 9. WHAT TO WORK ON NEXT (prioritized)

1. **Make the stuck (non-questing) half GRIND productively** instead of idle-looping the travel-router. The working half already grinds. When a bot's travel target is COOLDOWN/EXPIRED/null-destination for a while, it should commit to killing nearby level-appropriate mobs (moving to them, safely) rather than looping `choose travel target`. CAUTION: a naive "force attack-anything / straight-line walk" was tried and raised DEAD (walked bots into packs/red mobs). Do it SAFELY: only engage single, level-appropriate, reachable mobs; don't walk through collision. Measure on XP-gain% and idle% over a LONG window.
2. **Travel-router / questing** (the deep root, ~2500 idle-sec, quests ~4% progressing). Highest value but hardest — tangled with navmesh gaps. Understand `TravelMgr`, `ChooseTravelTargetAction`, `RequestQuestTravelTargetAction`, `MoveToTravelTargetAction`, and why targets go COOLDOWN with `NullTravelDestination` (TravelMgr.h:146). A per-bot displacement-based "this objective is unreachable, permanently skip it and pick another" already exists but re-picks loop when ALL nearby objectives are unreachable. Consider: level-appropriate zone routing so low-level bots don't get sent toward far/high objectives at all.
3. **Reduce DEAD ~11%**: death-loops (bot reses near killer in a too-high zone). Root is bots in level-inappropriate zones (e.g., L11 bot in Duskwood dying to L30 mobs) — a routing problem (#2). Post-revive hop + flee-escape already help.
4. **Real parabolic jump** (optional polish): rewrite `JumpAction::DoJump` (MovementActions.cpp ~5812) to use a parabolic MoveSpline so jumps animate instead of teleport-float. Then re-enable RpgJumpStrategy.

**Cadence:** pick ONE lever → implement carefully → build → deploy (one restart) → let it run 20–45 min → measure on the CLEAN signal (gold/xp cumulative, or long-window idle/move) → keep if it helps, revert if not. Prove it FIRES (event log / always-on csv). Don't churn. Don't crash the box. Don't teleport near players.

---

## 10. QUICK REFERENCE
- Repo: `/home/zuppier/tw/server_dev/tortoise-wow-pr79-replay` (branch `bots`)
- Start DB: `cd db && bash start_mariadb_tortoise.sh`  (root/mangos, 127.0.0.1:3306)
- Start server: relaunch the detached supervisor loop (§3b); world port 8091
- Find live mangosd: `fuser bin/mangosd`  (NOT pgrep — comm is "MainThread")
- Build: `cmake --build build -j6 --target mangosd`
- Deploy: freeze supervisor → SIGTERM/kill mangosd → `cmake --install build` → resume supervisor (§4)
- Honest metrics: `logs/supervisor.csv` (idle%/move%/active%), `bot_events.csv` money=/xp= (clean cumulative)
- Playerbot code: `src/modules/PlayerBots/playerbot/`
- Memory/notes: `~/.claude/projects/-home-zuppier-tw-server-dev/memory/` (MEMORY.md index)

---
## UPDATE 2026-07-01 evening (session continuation)

Official goal numbers live in `Classic_WoW_Leveling_Performance_Report.docx` (repo root): XP/hr 10-15k (L10-20), kills/hr 180-300, deaths/hr 0.05-0.15, gold 0.5-2g/hr, downtime 20-35%.

**New roots found + fixed (all deployed, each proven via its log channel):**
1. **Gear death spiral** — 37/50 bots had 0%-durability gear (fists only). `AutoRepair` in supervisor tick (logs `repair:` in antiidle.csv). Companion: engage-gate (no new fights <55% HP), ChainPullNext (instant next target post-kill, logs `chain`).
2. **Hunters shot dry** — no ammo refill existed. `AutoAmmo` (logs `ammo:`).
3. **Learning NEVER ran** — SQL unsigned underflow (ERROR 1690) since forever; also unbounded query takes 4-6 min. Fixed: IF(attacker_count>1,1,0), bounded to newest 2M samples, detached thread. Heartbeat: "learned policy loaded, N state/action bonuses" in boot.log every ~5 min.
4. **MotionMaster::MovePath is an EMPTY STUB** — ChaseTo (ranged approach) silently never moved bots. Fixed with real MovePoint dispatch. VERIFIED: casters now walk into range and chain-cast. See memory bot-noop-stub-family.
5. **Casters passive at 5-15% mana** — wand ("shoot") only fired below 5%. Added "low mana"->shoot for priest+warlock.
6. Pull fixes: solo pull cap 30y (was 450y!), pull aborts when attacked mid-pull, "return to pull position" disabled for solo bots (evade-reset loop).
7. Grind: unkillable-HP-pool filter (Training Dummies 1.1M HP), solo level-cap +2 (was +4), opportunistic path-kill <=20y while traveling (skipGrindTargetScan removed).
8. Immersion: no teleports near players (FreezeNudge/hop gated), RPG random-jump disabled (renders as teleport-float), HumanLike path jitter ON (pause/spin/jump zeroed) in aiplayerbot.conf.
9. GhostRescue (stuck ghost -> teleport to corpse, invisible to living), HopelessFightBreaker (60s no-damage -> give up).

**Current standing (30-min windows):** XP/hr ~860-890 avg (was ~320), deaths ~9.3/bot-hr (was 10.3 — STILL the #1 gap vs 0.05-0.15 target), downtime ~35-39%. Death drivers left: aggro-while-traveling through too-high zones (routing problem: L15 aggroes L20 wolves, L10s die to L60 guards), multi-add swarms (40% of deaths), flee can't outrun mobs.

**Measurement discipline:** use per-bot cumulative XP/gold from BotStatsSnapshot + DEAD-transitions in supervisor.csv. bot_deaths.csv timestamps are TIME-ONLY with multi-day history — never time-filter naively. 8-min windows have ±5-7pt noise; use 30-min.

## UPDATE 2026-07-02 ~15:45 — THE #1 XP LEVER, precisely diagnosed
XP slid 959->499 (L10-19) all day while gold/deaths improved. ROOT (verified via zone query + live router probe):
**bots outlevel their starter zones and never migrate.** 28 L10-19 bots in Durotar (1-10 zone, grey mobs = no XP).
The QuestGuideMgr hub-director EXISTS + is wired (38 level-banded hubs, SelectNextHub at ChooseTravelTargetAction:2017),
but outleveled bots keep getting routed to STALE LOCAL LOW-LEVEL QUESTS still in their logs (probe: Durotar L12+ bots
with travelEntry=10556/3121, local mobs) — quest-log completion outranks hub migration.

**NEXT FIX (spec)**: quest-log hygiene — bots must ABANDON grey/trivial quests (quest level <= botLevel - 7, tune),
like real players do, freeing the log so the hub-director routes them to the next level-appropriate hub.
The action "clean quest log" already exists (seen firing) — find CleanQuestLogAction and add/tune the grey-drop rule.
CAUTION (learned via loop-breaker v1-v3): verify with a 40-min XP window; do not drop quests that are complete-awaiting-turn-in
(status=1 rewarded=0 — turn those in for the XP instead). After this, XP should climb with zone-appropriate kills.
Measurement notes: bot_events.csv truncates each boot; engagement-gap fat tail p75=38s; combat share ~11-13% vs 30-47% target.
