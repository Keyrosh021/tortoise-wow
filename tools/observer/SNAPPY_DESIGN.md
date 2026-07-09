# Snappy bot director — "less thinking, more acting"

## Mandate (user, 2026-07-09)
Bots think too much and don't act; brains "shut off for a few seconds." Want: snappy, sticky combat;
stick to targets; don't fall behind in dungeons/raids; be active + helping; simplify thinking/combat/
movement; real purposeful actions not random jolts; add watchdogs/guards/managers for STEADY operation.

## Observed problems (this session)
- IN-COMBAT thrash: apm up to 773, moving=0 -- in a fight but not chasing/hitting the target.
- Action gaps of 9-41s where a bot does nothing visible.
- Bots AT a mob camp wandering with NO target instead of engaging the mobs in front of them.
- Grouped bots don't follow / fall behind; 0.7% of bots group at all; 0 in instances.

## Principle
Replace churn with a small deterministic director that, EVERY tick for an autonomous random bot,
produces ONE concrete action and a SHORT delay. Never leave the bot with no action and a long delay.
Class ROTATION (which spell) stays with existing combat code (it works, grp4 proved it) -- the director
owns TARGET + MOVEMENT + "ensure attacking/chasing", and delegates rotation ONCE per GCD when in range.

## Unified state machine (autonomous random bot; extends AutonomousFsmTick to combat + follow)
Evaluate top-down each tick:
1. PENDING INVITE -> delegate (accept immediately).  [already shipped]
2. FOLLOW/ASSIST (grouped, not leader):
   - leader in combat -> attack leader's target (assist/focus-fire). short delay ~GCD.
   - else if far from leader (> ~15y) -> move to leader; if very far (> ~60y) or stuck -> teleport to
     leader (in-instance catch-up). short delay.
   - else -> hold near leader. This is the "don't fall behind in dungeons/raids" fix.
3. COMBAT (IsInCombat OR attackers OR a live hostile target): STICK to target.
   - target := current victim, else nearest attacker, else nearest hostile in range.
   - in melee/spell range + LoS -> ensure auto-attack ON; run rotation ONCE (delegate); delay ~GCD.
   - out of range -> MoveChase(target) (persistent); delay ~250-350ms; re-check.
   - GUARD: no progress toward target AND target not dying for N s -> switch to nearest other hostile;
     if none reachable -> break combat (drop target, blacklist briefly).
4. LOOT owed -> loot.  [shipped]
5. GRIND (not in combat; hostile mob within engage range) -> engage nearest NOW (snappy pull). This is
   "attack what's in front of you." delay ~GCD after pull.
6. TRAVEL (active dest) -> move, verify displacement.  [shipped]
7. RELOCATE / LOOK-ALIVE / IDLE.  [shipped]

## Watchdogs / guards (STEADY operation)
- Combat stick-guard: if in combat, target valid, out of range, and MotionMaster NOT chasing -> force MoveChase.
- No-action guard: in combat + 0 swings/casts for >2s + target in range -> force attack/rotation.
- Fall-behind guard: grouped + >Xy from leader for >Ns -> teleport to leader.
- Stuck-chase guard: chasing but net displacement ~0 for >Ns -> target unreachable -> switch/break.
- Snappy-delay guard: NEVER set aiInternalUpdateDelay > ~400ms while in combat or chasing.

## Test loop (every change)
- cohort_stats.py: combat% up (from ~17%), moving-with-target up, apm sane (no 700+ zero-output).
- observer: watch a bot fight -> it should chase-then-swing continuously, stick to one target, no multi-s gaps.
- lead.py recruit-at-camp: grouped bots assist leader target + stay close.
- Stability: uptime, heartbeat, no crash cores after each deploy.
