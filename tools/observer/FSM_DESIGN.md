# Deterministic FSM brain for random near-player bots — design

## Why (measured, 2026-07-08, active cohort n=2500 snapshots, observer online)
- moving 15%, combat 6%, target=none 37%
- apm median 14, p90 162, **max 966** (16 decisions/sec, zero output)
- **25% thrash**: apm>=20 AND moving=0 AND combat=0
- travelStatus: 56% COOLDOWN, 11% EXPIRED, 10% READY, 2.5% TRAVEL, ~0% WORK
- TWO thrash modes:
  1. IDLE-THRASH (fountain): mv0 cmb0, no valid target / grey target, re-deciding.
  2. COMBAT-THRASH (worst, apm 700-966): cmb1 mv0 — in a fight, target never reached, re-deciding
     every tick with no movement and no landed hit.

## Core principle
The current engine can CHOOSE an action that never EXECUTES and then re-choose it forever (no
execute-or-fail damper). The FSM replaces the top-level non-combat/idle decision for random bots
(NOT master-driven bots, NOT scripted combat rotations) with explicit states whose transitions are
gated on OBSERVED execution, not on relevance scores.

## States (random, non-master bot, awake/active cohort only)
- COMBAT      : has a live hostile target or is being attacked.
- LOOT        : combat just ended and a lootable corpse is owed to this bot.
- GRIND       : no combat; a level-appropriate hostile mob exists within engage radius.
- TRAVEL      : no local mob; move toward a chosen content destination (grind camp / quest poi).
- RELOCATE    : stuck (no actionable content reachable for T seconds) -> pick a new content anchor
                and (out of player sight) fast-path there; (in sight) walk/road to it.
- CITY/REST   : designated city resident or needs to eat/drink -> social/idle behavior.

## Execute-or-fail contract (the anti-thrash core)
Every state does ONE concrete thing per decision and then SETS A COMMIT WINDOW during which the FSM
does NOT re-decide — it only checks whether the commit produced the expected observable change:
- COMBAT: if target within melee/spell range and LoS -> ensure auto-attack on + run class rotation;
  commit ~= swing timer / GCD. If OUT of range -> issue ONE chase (MovePoint to target) and commit
  for maxWaitForMove; verify position delta toward target next check. If no progress toward target
  for N checks (unreachable / evading) -> DROP target, blacklist its guid briefly, exit COMBAT.
  HARD RULE: never re-issue chase/rotation more than once per commit window (kills apm 966).
- GRIND: select nearest level-appropriate claimable mob (reuse existing target value). Issue ONE
  pull (move into range or ranged pull) and commit. If pull doesn't start combat within window ->
  next candidate; after K failures -> TRAVEL.
- TRAVEL: MovePoint toward destination; commit for the ETA slice. Verify displacement each check;
  if displacement ~0 for 2 checks -> the point is unroutable -> RELOCATE.
- RELOCATE: choose a spawn-dense, level-appropriate anchor (from creature spawn table, not the stale
  TravelMgr points). Verify arrival; this is the escape hatch that guarantees a bot always ends up
  somewhere it CAN act.

## Anti-thrash damper (applies to all states)
Maintain per-bot lastDecisionMs + commitUntilMs + lastObservable{pos,targetGuid,inCombat}. The FSM's
tick early-returns while now < commitUntilMs UNLESS a state-break event fires (took damage, target
died, arrived). This bounds decisions to a few/sec MAX regardless of relevance churn.

## Acceptance metric (measure with the headless observer + bot_events)
Active near-player cohort after fix: moving+combat should TRACK apm (no apm-with-zero-output);
THRASH% (apm>=20 & mv0 & cmb0) -> near 0; combat-thrash (cmb1 & mv0 & apm>200) -> ~0;
visible moving% up from 15%, combat% up from 6%; travelStatus WORK > 0.

## Insertion point / API — FILL FROM ENGINE MAP (Explore agent running)
- where to hook before the relevance engine in UpdateAIInternal for random bots
- Action::Execute/isUseful/isPossible contract; how MovePoint returns true/false
- AI_VALUE names: nearest attackable, current target, travel target, is moving, can path
- keep scripted class COMBAT rotation actions (don't rewrite spell logic) — FSM only owns
  target/move/state selection, delegates the actual rotation to existing combat actions.
