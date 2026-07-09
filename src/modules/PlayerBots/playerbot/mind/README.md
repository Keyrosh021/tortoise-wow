# mind/ — the bot intent layer (decision-making rewrite)

One deterministic brain per bot that OWNS every top-level decision for
autonomous random bots, replacing the relevance-engine churn and the two
inline FSMs (AutonomousFsmTick / CombatFsmTick) that were bolted into
PlayerbotAI.cpp. Human-mastered alt bots stay on the legacy engine untouched.

NO FEATURE IS REMOVED. The existing, proven execution machinery — class
rotations, loot dance, quest-giver dialogs, vendor/AH/RPG interactions —
stays and is driven THROUGH the mind. What gets replaced is the part that
made bots feel random and pointless: the trigger/relevance loop that
re-decides every tick, flip-flops targets, stalls chases, and leaves
dead-air after kills. The mind gives each bot a current GOAL, commits to
it, and executes it fluidly.

Master switch: `AiPlayerbot.Mind` (default 1). Rollback: set 0 → legacy path
(inline FSMs + relevance engine) runs exactly as before.

## Principles
- ONE goal at a time, held with intent. A decision is re-evaluated only when
  its observable effect resolves, it completes, or a break event fires
  (attacked, target died, arrived). No per-tick re-decide, no thrash.
- Proactive next-task: while executing goal N the mind already knows N+1
  (kill → loot this corpse → next target) — zero post-kill rethink dead-air.
- Every handled tick sets the AI update delay: a bot that decided something
  SLEEPS until its next meaningful check (no apm explosions).
- Measure ACTIONS not intentions: telemetry counts casts/loots/kills/quest
  turn-ins, never picks.
- Per-bot state lives in the BotMind object (owned by PlayerbotAI). No static
  guid-keyed maps, no cross-bot locks (map threads run bots in parallel).
- Execution reuse, decision ownership: interactions run through the existing
  Action bodies via the DoSpecificAction bridge (single named action, NO
  relevance engine) or direct primitives (MotionMaster, CastSpell). The
  in-range combat rotation stays on the per-class combat engine — class
  competence is a feature, and it works; what the mind owns in combat is
  target choice, stickiness, positioning, and what happens the instant the
  target dies.

## Files
- `Mind.h/.cpp`      BotMind: tick entry + goal arbiter + commit windows
- `Percept.h/.cpp`   throttled world snapshot: best kill target, loot owed,
                     quest givers, rest need, group state
- `Move.h/.cpp`      movement primitives + the single stuck-detector
- `Combat.h/.cpp`    combat executor: engage/chase/stick, assist, next-task
                     queueing; in-range rotation delegated to combat engine
- `Journey.h/.cpp`   where-to-be: grind camps, level-band dispersal, zone fit
- `Errands.h/.cpp`   quest pickup/turn-in intent + town duties (vendor,
                     repair, train) driven through existing actions
- `Social.h/.cpp`    group member behavior: accept invites, follow, assist
- `MindLog.h/.cpp`   logs/mind.csv per-minute counters + per-bot trace hooks

## Goal arbiter (strict priority, evaluated top-down)
1. BUSY     taxi / teleport / mid-cast → sleep
2. COMBAT   in combat or attacked → Combat executor
3. LOOT     corpse owed within range → loot chain first     (Goal 4)
4. REST     hp/mana low out of combat → eat/drink
5. SOCIAL   grouped non-leader → follow/assist leader
6. ERRAND   quest turn-in/pickup or town duty nearby
7. JOURNEY  best target / walk to content — never stand idle, never pointless

Each executor returns Verdict{handled, sleepMs}; the arbiter enforces commit
windows and writes telemetry.
