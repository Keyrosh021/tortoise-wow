# Autonomous group system — formation + follow + assist (make bots not fall behind, be active/helping)

## Why
Only 0.7% of bots group; 0 run dungeons. No group content -> world feels empty of real-player-style play.
User: bots must not stand behind in dungeons/raids, be active + helping, stick, snappy.

## Increment 1: grinding groups (simpler than dungeons, delivers follow+assist+stickiness)
### Group formation manager (server-side, RandomPlayerbotMgr tick, throttled)
- Periodically scan the ACTIVE cohort for autonomous ungrouped bots that are close together (same
  zone/area, within ~60y) and level-similar (±3). Form a party of 3-5 with a designated leader
  (highest level, or a tank-capable class). Real Group via existing Group API. Config-gated + capped
  (only form a few groups/min; only in the active cohort so parked bots unaffected).
- Prefer role balance when available (1 tank-capable, 1 healer, rest dps) but don't require it.

### Follow/assist director (for autonomous bot in a group, NOT the leader)
New branch in DoNextAction hooks (gate: IsRandomBot, in group, not leader, !HasRealPlayerMaster):
- leader in combat / has victim -> ASSIST: my target := leader's victim; engage via combat-director
  chase logic (focus fire). snappy.
- else dist-to-leader > ~12y -> MoveFollow(leader); if > ~50y or stuck Ns -> teleport to leader
  (catch up; in-group teleport is acceptable and better than falling behind). snappy delay.
- else hold near leader.

### Guards
- fall-behind guard: >Xy from leader for >Ns -> teleport to leader.
- leader-validity: leader gone/offline -> disband or promote.
- no-orphan: bot stuck in a group whose leader is parked/dead -> leave.

## Increment 2 (stretch): dungeon runs
- Reuse dungeon-entrance loiterers: when enough level-appropriate bots gathered at an entrance, form a
  role-balanced party, leader walks to the areatrigger, all zone in (queued teleport to instance dest).
- In-instance: leader proximity-pulls trash packs sequentially; followers assist+follow; heal/tank via
  existing class logic. No scripted boss mechanics (out of scope) -- brute-force clear.
- This makes dungeons actually run (players would SEE groups inside).

## Test
- lead.py / observer: do bots form parties (roster), stick to leader (dist stays <15y), focus-fire
  (all attack same target), not idle. Group combat telemetry. Stability + no crash each deploy.
- server: count autonomous groups formed (up from 0.7%), bots-in-instances (up from 0).
