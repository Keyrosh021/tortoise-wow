# Bot behavior study — why bots get stuck & why they die

Investigation run overnight 2026-06-28 at **2000 bots** (stable count; 5000 crash-loops — see
[memory: bot-server-findings]). Data-driven: new instrumentation logs every bot death and a
fleet-wide per-bot state snapshot every 60s. Code mechanisms confirmed by reading the source.
**Nothing about bot *behavior* was changed — this run only added observability.**

> Numbers below are from the first ~30-minute sample and are refreshed at the bottom
> (`## Refreshed numbers`) as more data accumulates. Directions are already firm.

---

## TL;DR

**Why bots stand still (the "60–96% idle" you saw):** the single biggest cause is the
**travel COOLDOWN lock**. When a bot can't find a travel destination — common for low-level bots
packed into starter zones — the travel system parks it in `TRAVEL_STATUS_COOLDOWN` for **up to 5
minutes**, and *during that cooldown it deliberately blocks RPG/grind and movement*. So the bot
just stands there, at full HP/mana, until the timer expires — then often fails to find a
destination again and re-locks. ~22% of the whole fleet is in this state at any moment, and it is
**not** activity-throttling (that's disabled) and **not** regen (bots are at full resources).

**Why bots die (~22% dead at any time):** two distinct things, and separating them matters —
- **Death loops (31% of all deaths from just 84 bots):** a handful of bots are stuck dying over
  and over in the same spot — overwhelmingly **drowning/falling in Redridge (zone 46): 942 env
  deaths from only 4 level-10–11 bots**, one of which (Leandro) died **262 times** at one lake
  position. Plus some bots re-pulling the same elite forever. These few bots inflate the raw death
  count ~3× and the "avg killer level" stat.
- **The normal fleet (the other 1257 bots, 69% of deaths):** dies to roughly **even-level** mobs
  (avg killer only **+0.9**), **64% 1v1**, and — the key lever — **literally never flees** (0 of
  7,440 normal deaths happened while fleeing). They fight losing 1v1s to the death and sometimes
  over-pull (36% had ≥2 attackers).

Both problems are **structural behavior changes to fix** — left for you to decide on (below).

---

## How this was measured (instrumentation added this run)

All observability-only, gated to bots, thread-safe:

- **`src/game/Objects/Unit.cpp`** — `Unit::Kill()` hook → `logs/bot_deaths.csv`: one row per bot
  death with killer type/name/level, level delta, # attackers, fleeing flag, group size, zone, pos.
- **`src/modules/PlayerBots/playerbot/RandomPlayerbotMgr.cpp`** — `LogBotDiagSample()` (every 60s) →
  - `logs/bot_diag.csv`: per-bot row (level, class, zone, hp%, mana%, alive/combat/moving/sit,
    **travel status**, move/extend retry, distance-to-target).
  - `logs/bot_fleet.csv`: one aggregate row (counts per state).
  - reads only race-tolerant fields + the long-lived TravelTarget status/dest (never the
    `lastAction` string), so it can't crash the map threads.
- **`logs/nearby_census.csv`** (existing) — what bots near *your* character are doing, with the
  idle breakdown (rest/lowhp/lowmana/STUCK).

Analysis tools:
- `python3 tools/bot_deaths_report.py [N]` — death causes.
- `python3 tools/bot_stuck_report.py` — fleet composition + per-bot snapshot + **dwell analysis**
  (how many consecutive minutes bots stay locked in cooldown).
- `python3 tools/nearby_census.py [--watch]` — live near-you census.

---

## 1. Why bots are STUCK (standing still at full HP/mana)

### Fleet composition (measured, ~2000 bots)
| state | % | meaning |
|---|---|---|
| dead | ~23% | (see §2) |
| **cooldown** | **~22%** | travel COOLDOWN — standing, blocked from doing anything |
| combat | ~19% | actually fighting |
| moving | ~13% | traveling |
| travel_notmoving | ~8% | has a TRAVEL goal but not moving toward it |
| work / prepare / expired / rest / idle_other | ~15% | misc |

"Actively busy" (combat+moving) is only ~34%. The biggest idle bucket is **cooldown**.

### Root cause #1 — the travel COOLDOWN lock (the dominant one)
- `ChooseTravelTargetAction` fails to find any active destination → sets a `NullTravelDestination`
  with `TRAVEL_STATUS_COOLDOWN`, default **5 minutes** (15s for some low-level quest cases).
  `ChooseTravelTargetAction.cpp:187-189`.
- During COOLDOWN the bot is deliberately idle:
  - `MoveToTravelTargetAction` won't run unless status is TRAVEL/READY — `MoveToTravelTargetAction.cpp:1263`.
  - **RPG/grind is explicitly excluded during COOLDOWN** — `RpgTriggers.cpp:20-21`.
  - `TravelTarget::IsActive()` returns **true** for COOLDOWN, which blocks the "request new travel"
    trigger from firing — `TravelMgr.h` (IsActive: NONE/EXPIRED/PREPARE → false, everything else → true).
- Net: bot can't move, can't grind, can't re-pick — it just waits out the timer, then frequently
  fails to find a destination again. This is why `no_goal≈0` in the data: the "no destination"
  case manifests as **COOLDOWN**, not an idle "no goal" state.
- *Why destinations fail for low-level bots:* starter zones are crowded with bots and the
  destination queries are level/zone-filtered, so a level-8 bot often has zero valid quest/grind
  travel targets within reach.

### Root cause #2 — `travel_notmoving` (~8%)
Bots in `TRAVEL_STATUS_TRAVEL` with a destination **hundreds of yards away** but **moveRetry=0**
(haven't even attempted to move). Example captured: bot 732y from its target, status=TRAVEL, not
moving. This is a movement-commit gap distinct from pathing failure (which would show moveRetry>0
and end in cooldown via `MoveToTravelTargetAction.cpp:1143-1165`). Needs a focused look.

### Ruled out
- **Activity throttling / LOD** — `DisableActivityPriorities=1` makes `GetPriorityType()` return
  the always-active type for every bot (`PlayerbotAI.cpp:12820`), so `AllowActivity()` is always
  true. LOD cold-dormancy is also paused in code. Throttling is **not** the cause.
- **Regenerating (eat/drink)** — census idle breakdown shows ~96% of idle bots at **full** HP/mana
  (`rest`/`lowhp`/`lowmana` all ~0). They're not resting; they're locked.

---

## 2. Why bots DIE (~23% dead at any time)

### Death data (~30 min, 9.7k deaths) — **split into two populations**
The raw aggregate (avg killer +4.4, 88% creature / 8% env) is **misleading** until you separate
the death-loopers from the normal fleet:

| | bots | deaths | share |
|---|---|---|---|
| **loopers** (>20 deaths each) | 84 | 3,353 | **31%** |
| normal fleet | 1,257 | 7,440 | 69% |

### Root cause #1 — death LOOPS (31% of deaths, a few bots, fixable bug)
- **Environmental drown/fall loops in Redridge (zone 46): 942 env deaths from just 4 bots**
  (level 10-11), clustered at lake positions (~ -7900,-1300). One bot (Leandro, lvl 11) died
  **262×**. The corpse/graveyard returns the ghost to a spot where it walks back into deep
  water / off a ledge and dies again — there is no hazard/water/cliff guard, and the
  "fell-through-terrain → move corpse to safe spot" rescue only runs for *real* players
  (`ReviveFromCorpseAction.cpp:720-737`, gated on `isRealPlayer()`).
- Plus some bots re-pulling the same **elite** forever after revive (55% of repeat deaths are to
  the *same creature entry* as the previous death → revive → re-engage the exact thing that just
  killed it).

### Root cause #2 — the normal fleet never flees
- Excluding loopers, normal bots die to **~even-level mobs (avg +0.9)**, **64% 1v1**, and **0 of
  7,440** were fleeing. Flee/panic requires **all** of: HP < ~20%, low mana, and (>1 attacker OR
  target healthier) — `GenericTriggers.cpp` PanicTrigger / OutNumberedTrigger (50% HP gate). In a
  1v1 those rarely all hold, so the bot fights to the death. There's no kite / "drink when safe"
  disengage loop.
- Over-pulling is secondary (36% had ≥2 attackers). Over-*leveling* is minor for normal bots
  (+0.9 avg) — the +4 grind cap (`GrindTargetValue.cpp:307`) mostly hurts the loopers/elites, not
  the broad fleet.

### Net death story
Most fleet deaths = even-level 1v1s lost because **bots never disengage**. The eye-popping raw
death rate (~320/min) is ~⅓ a handful of bots in environmental death loops (Redridge lake).

---

## Recommended next steps — **your call (structural behavior changes)**

Ranked by expected impact. None implemented — these change behavior, which you said to decide on.

**For "stuck":**
1. **Shorten the NullTravelDestination cooldown** from 5 min → ~30–60s for solo random bots, and/or
   **allow RPG/grind during COOLDOWN** (un-block `RpgTriggers.cpp:20-21`). This alone should melt
   most of the 22% cooldown idle — bots would grind nearby instead of freezing.
2. **Guarantee a fallback destination** for low-level starter-zone bots (a nearby grind spot) so
   `ChooseTravelTargetAction` rarely returns "nothing," avoiding the lock entirely.
3. Investigate `travel_notmoving` (status=TRAVEL, moveRetry=0, far from target) — a movement-commit
   gap worth a targeted trace.

**For "dying":**
4. **Kill the death loops first (biggest single win — 31% of deaths from 84 bots).**
   - Environmental: after death, detect when a bot's corpse/return path is in/over a hazard
     (deep water, fall) and relocate the corpse to safe ground for bots too (extend the
     `isRealPlayer()`-only rescue at `ReviveFromCorpseAction.cpp:720-737`). The 4 Redridge
     drown-bots alone are ~900 deaths.
   - Add a post-revive "if I just died ≥N times in M minutes at ~this spot, don't return here /
     don't re-pull the same entry" guard (55% of repeat deaths are to the same mob).
5. **Make flee actually trigger** (the broad-fleet lever) — drop the multi-condition AND-gate so a
   bot disengages on low HP in a losing 1v1 (not only when *also* low-mana *and* outnumbered).
   0 of 7,440 normal deaths fled.
6. Lower the grind level cap +4 → +1/+2 (`GrindTargetValue.cpp:307`) — modest for the normal fleet
   (avg killer +0.9) but helps the elite-loopers and the ~18% of normal deaths to ≥3-higher mobs.
7. A "rails"/spread pass so low-level bots aren't all piled into the same starter zones — helps
   both stuck *and* deaths (crowding starves travel destinations → cooldown lock, and concentrates
   danger). The fleet skews ~lvl 9-10 all crammed into the same handful of zones.

---

## Appendix — reproduce
```
python3 tools/bot_deaths_report.py        # death causes
python3 tools/bot_stuck_report.py          # fleet composition + dwell analysis
python3 tools/nearby_census.py --watch     # live, near your character
```
Raw data: `logs/bot_deaths.csv`, `logs/bot_diag.csv`, `logs/bot_fleet.csv`.

## Refreshed numbers
_Larger sample: ~30 min at 2000 bots, **9,688 deaths**, 27 fleet snapshots (~49k per-bot rows).
Avg fleet level held flat at **~9.6** — the fleet skews low-level and isn't progressing (it dies +
idles instead of leveling), which is why both problems concentrate in starter zones._

**Death count is inflated ~3× by death-loopers.** Of 10,793 deaths from 1,341 bots: **84 bots
(>20 deaths each) account for 31%** — they revive, return to the same spot, and die again (max:
one bot **254–262×**). 55% of all repeat deaths are to the **same creature entry** as the prior
death.

**Deaths — raw vs normal-fleet (loopers removed):**
- raw: 88.7% creature, **8.4% environment** (almost all = 4 Redridge drown-bots), avg killer +4.4.
- **normal fleet (1,257 bots, 7,440 deaths): 98% creature, ~0% env, avg killer +0.9, 64% 1v1,
  36% outnumbered, 0% fleeing, ~5.9 deaths/bot/30min.** ← the true fleet picture.
- top death zones: Redridge(46, mostly the 4 drowners), Tirisfal(85), Durotar(14), Elwynn(12),
  Westfall(40), Mulgore(215). Top normal killers: Harvest Watcher L14-15, Prowler L10, Vampiric
  Duskbat L8-9, Prairie Stalker/Wolf — starter-zone aggro mobs ~at bot level.

**Stuck / fleet composition (27 snapshots, ~1906 avg):**
- **45% alive-but-idle**, **30.6% busy** (combat+moving), **22% dead**.
- biggest idle bucket: **cooldown 17%**, then **travel_notmoving 9.8%**, expired 5.8%.
- **Dwell (the proof of the lock):** longest continuous cooldown streak per bot —
  **median 2 min, p90 5 min, max 26 min**; **33% of bots were locked ≥3 consecutive minutes**.
  A bot spends median **12%** (p90 **43%**) of its life frozen in cooldown, and is busy only
  median **29%** of the time.
- `travel_notmoving`: status=TRAVEL, **moveRetry=0** (not a pathing failure — only 9 stuck bots had
  moveRetry>0), targets 60–5000y away while standing still. Distinct movement-commit bug.

**Two new/confirmed leads for the next-steps list:**
- **Environmental deaths (8.4%)** — add a hazard/water/cliff guard to movement/positioning.
- **Death-loop on revive** — after reviving, bots re-engage the same lethal mob; a short
  post-revive "don't re-pull the thing that just killed me / back off" would cut the turnover.

_Server note: 2 surge-SIGSEGV/SIGABRT auto-restarts during the window (the documented login-surge
crashes), each recovered to 2000. Steady state is stable._

### ⚠️ Both problems are DEGENERATIVE (the most important finding — from 1.3h of data)
They are not steady-state; they **compound** as the run continues. Bots that fall into a stuck
cooldown or a death loop tend to **stay** there, so the fleet slowly rots:
| metric | @30 min | @1.3 h |
|---|---|---|
| death-loopers' share of all deaths | 31% | **65%** (384 bots) |
| worst single bot's death count | 262× | **796×** |
| bots locked in cooldown ≥3 consecutive min | 33% | **54%** |
| longest cooldown streak (max) | 26 min | **76 min** |
| normal-fleet avg killer level delta | +0.9 | **−0.2** (even *below* bot level) |

Implication: the longer the server runs, the more bots accrete into permanently-idle or
permanently-dying states. So these fixes aren't just polish — without them, a long-lived fleet
trends toward mostly-idle/mostly-dead. The −0.2 normal-fleet killer delta also finalizes that
normal deaths are **not** an over-level problem at all — it's purely **fight-to-death, never
disengage** (0 of ~9.6k fled), plus the concentrated death-loop bug.
