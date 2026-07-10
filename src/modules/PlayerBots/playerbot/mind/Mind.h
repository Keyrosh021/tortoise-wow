#pragma once

#include "ObjectGuid.h"

class Player;
class PlayerbotAI;
class Unit;
class Creature;

namespace mind
{
    // What a single mind step decided this tick.
    struct Verdict
    {
        bool handled = true;    // false -> arbiter falls through to the next goal / engine
        bool executed = false;  // did an observable action happen (measure ACTIONS, not intentions)
        uint32 sleepMs = 500;   // AI update delay to set when handled
    };

    enum class Goal : uint8
    {
        None = 0,
        Combat,     // fight the pinned target (engine runs the in-range rotation)
        Loot,       // corpse/node owed within range -> loot chain first
        Social,     // grouped non-leader: stick to and assist the leader
        Errand,     // quest pickup/turn-in or town duty at a committed NPC
        Journey,    // walk to content: quest zone / grind camp / dispersal
    };

    // The bot intent layer. One instance per bot, owned by PlayerbotAI.
    //
    // Owns every LOCOMOTION and PURSUIT decision for autonomous random bots:
    // where to go, what to chase, what happens the instant the current task
    // resolves. Holds ONE goal at a time and commits to it (no per-tick
    // re-decide). Stationary/reactive features (buffs, food, loot dance,
    // quest dialogs, trade, rolls) and in-range combat rotations keep running
    // on the slimmed legacy engine, driven by the mind's decisions.
    class BotMind
    {
    public:
        BotMind(PlayerbotAI* ai, Player* bot);

        // Busy hold: taxi / teleport / mid-cast. When true the caller skips
        // BOTH the engine pass and the mind step this tick (re-running the
        // engine mid-cast cancels the cast — the FSM's short-circuit, kept).
        bool BusyHold();

        // Non-combat proactive step. Called every non-busy tick, AFTER the
        // slimmed engine's reactive pass. Owns the update delay outright:
        // engine bookkeeping actions set multi-second action durations as
        // their anti-rerun mechanism, and honoring them put the fleet to
        // sleep (measured 61% idle) — the mind is the tempo authority.
        bool Step(bool minimal);

        // Combat step. Returns true when the mind handled the tick (chase /
        // reposition / target pin); false -> caller runs the combat engine
        // tick (in-range rotation, class features). Always pins the target.
        bool CombatStep(bool minimal, bool* executed);

        // Names of proactive movement strategies stripped from the
        // non-combat engine for mind bots (the churn sources the mind replaces).
        static const char** StrippedNonCombatStrategies();

        Goal CurrentGoal() const { return goal; }

    private:
        // ---- arbiter (Mind.cpp)
        Verdict Arbitrate(uint32 now, bool minimal);
        void SetGoal(Goal g, uint32 now);
        uint32 Commit(uint32 now, uint32 forMs);   // returns sleep suggestion

        // ---- perception (Percept.cpp)
        Unit* PinnedOrBestTarget(uint32 now);      // sticky kill-target choice (no flip-flop)
        bool  IsUsableKillTarget(Unit* u) const;   // alive, hostile, xp-worthy or quest credit, not tapped-away
        bool  KillHelpsQuest(uint32 creatureEntry) const;
        Creature* LootOwed(uint32 now);            // nearest corpse we hold rights on
        bool  IsBlacklisted(const ObjectGuid& g, uint32 now) const;
        void  AddBlacklist(const ObjectGuid& g, uint32 now, uint32 forMs);

        bool SeenByPlayer(float range) const;      // Journey.cpp; gates every teleport/blink
        bool QuestObjectiveDest(uint32 now, float& outX, float& outY, float& outZ); // Journey.cpp

        // ---- movement (Move.cpp)
        bool MoveTowards(float x, float y, float z, uint32 now);  // pathfinding step-walk (far dests hop)
        bool MovingBlocked(uint32 now);            // the single stuck detector (2-strike)
        void ResetStuck(uint32 now);

        // ---- executors
        Verdict StepCombatChase(uint32 now);       // Combat.cpp
        Verdict StepLoot(uint32 now);              // Combat.cpp (kill -> loot chain lives together)
        Verdict StepSocial(uint32 now);            // Social.cpp
        Verdict StepErrand(uint32 now);            // Errands.cpp
        Verdict StepJourney(uint32 now);           // Journey.cpp

        PlayerbotAI* ai;
        Player* bot;

        Goal goal = Goal::None;
        uint32 goalSince = 0;
        uint32 commitUntil = 0;        // no re-decision before this while progress verifies

        // target commitment (the anti-flip-flop core)
        ObjectGuid targetGuid;         // pinned kill target
        ObjectGuid nextTargetGuid;     // queued DURING combat: what we do after this mob dies
        uint32 targetScanAt = 0;

        // loot
        ObjectGuid lootGuid;
        uint32 lootScanAt = 0;

        // combat chase progress (replaces the static CdTrack map)
        ObjectGuid chaseTgt;
        float chaseLastDist = 0.f;
        uint32 chaseProgressAt = 0;
        uint32 nextTaskScanAt = 0;

        // stuck detection (one instance, used by whichever executor moves)
        float lastX = 0.f, lastY = 0.f;
        uint32 lastMoveCheck = 0;
        uint8 stuckStrikes = 0;

        // journey. dest is always a RAW cache/objective point (valid ground Z
        // — teleports land here); the scatter is applied only to the walking
        // aim so bots spread within a camp without airborne/rooftop landings.
        float destX = 0.f, destY = 0.f, destZ = 0.f;
        float scatterAng = 0.f, scatterOff = 0.f;
        uint32 destPickAt = 0;
        bool destIsObjective = false;  // current dest aims at a quest objective area (telemetry)
        // objective-directed journey cache: nearest unfinished quest-objective
        // area (same map, walkable range). Resolving walks the immutable
        // travel destination map for every quest in the log, so the answer is
        // cached and re-resolved at most every ~45s per bot.
        float questDestX = 0.f, questDestY = 0.f, questDestZ = 0.f;
        uint32 questDestMapId = 0;
        uint32 questDestResolveAt = 0;
        bool questDestFound = false;
        // productivity clock: last time this bot did something that earns
        // xp/loot/quests. A journeying bot that stays unproductive too long
        // is in dead terrain (grey mobs / only enemy bots around) and
        // relocates to level-matched content (unseen only).
        uint32 lastProductiveAt = 0;

        // social: how long we've been holding next to an idle leader, and
        // whether this tick is leashed (local activity ok, no journeying off)
        uint32 socialHoldSince = 0;
        bool socialLeashed = false;

        // errands
        ObjectGuid errandNpc;
        uint32 errandStageAt = 0;
        uint32 nextErrandScanAt = 0;
        uint32 maintenanceAt = 0;      // rate-limited engine maintenance burst (learn spells etc.)

        // small unreachable-object blacklist (targets, corpses, camps)
        struct BlEntry { ObjectGuid g; uint32 until = 0; };
        BlEntry blacklist[8];
        uint8 blCursor = 0;
    };
}
