#pragma once

#include "Common.h"
#include "playerbot/TravelMgr.h"
#include <deque>
#include <mutex>
#include <string>
#include <array>
#include <unordered_map>
#include <vector>

class Player;
class PlayerbotAI;

namespace ai
{
    struct LearnedBlackspot
    {
        std::string type;
        int32 mapId = -1;
        int32 bucketX = 0;
        int32 bucketY = 0;
        float radius = 25.0f;
        std::string reason;
        uint32 hitCount = 0;
        float penalty = 0.0f;
        float confidence = 0.0f;
    };

    struct LearnedObjectiveStat
    {
        std::string type;
        uint32 questId = 0;
        uint32 entry = 0;
        std::string name;
        float penalty = 0.0f;
        uint32 failureCount = 0;
    };

    class BotLearningMgr
    {
    public:
        BotLearningMgr() = default;

        void Load();
        void Reload();
        bool HasData();

        float GetTravelPenalty(Player* bot, TravelDestination* destination, WorldPosition const* position, std::string* reason = nullptr);
        void RecordBotTelemetry(PlayerbotAI* ai, uint32 elapsed);
        // Sampled (~1/sec) per-bot combat decision: captures the bot's state + the
        // action it just took, tagged with the current fightId so it can be joined to
        // the fight's reward. Cheap no-op when not in combat / not the bot's turn to sample.
        void RecordCombatDecision(PlayerbotAI* ai, std::string const& action);
        void FlushTelemetry();

        // ---- Learned policy (closes the loop: logged (state,action,reward) -> action bias) ----
        // Aggregates the decision+reward data into an in-memory action-value table
        // (per class + HP bucket: how much above/below the local average reward an action
        // is). Recomputed periodically from FlushTelemetry as data accumulates.
        void ComputeAndLoadPolicy();
        // Bounded relevance nudge for `action` given the bot's class + current state
        // (HP, mana, and whether it's outnumbered: attackerBucket 0 = <=1 attacker, 1 =
        // 2+). The engine adds this (scaled by the bot's experiment cohort strength) to
        // the action's relevance, biasing the rotation toward what historically wins. 0
        // if no data / disabled. Cheap: one hashed lookup. The engine computes
        // attackerBucket once per tick (not per action) to keep this off the hot path.
        float GetActionRelevanceBonus(Player* bot, uint8 attackerBucket, std::string const& action);

        // LEARNED SPEC: best-performing talent tab for a class, derived from the fleet's own fight
        // rewards (avg reward by class+spec_tab over the newest samples, recomputed with the policy).
        // Returns -1 when there is not enough data; caller falls back to config probabilities.
        // Used by PlayerbotFactory::InitTalentsTree to bias NEW spec rolls toward what wins,
        // with exploration preserved so all specs keep generating data.
        int GetLearnedBestSpec(uint8 cls);

    private:
        struct TaskSample
        {
            uint32 botGuid = 0;
            uint32 accountId = 0;
            uint8 race = 0;
            uint8 clazz = 0;
            int8 specTab = -1;
            uint8 level = 0;
            uint32 mapId = 0;
            uint32 instanceId = 0;
            uint32 zoneId = 0;
            uint32 areaId = 0;
            int32 bucketX = 0;
            int32 bucketY = 0;
            std::string taskType;
            bool isRandomBot = false;
            bool hasRealMaster = false;
            uint8 groupSize = 0;
            bool isGroupLeader = false;
            bool isDungeon = false;
            bool isRaid = false;
            bool isBattleground = false;
            bool isCombat = false;
            bool isAlive = false;
            float healthPct = 0.0f;
            float powerPct = 0.0f;
            uint32 elapsedMs = 0;
            int32 levelDelta = 0;
            int32 xpDelta = 0;
            int32 moneyDelta = 0;
            uint32 activeQuests = 0;
            uint32 completeQuests = 0;
            uint32 rewardedQuests = 0;
            int32 activeQuestDelta = 0;
            int32 completeQuestDelta = 0;
            uint32 targetEntry = 0;
            uint8 targetLevel = 0;
            float targetHealthPct = 0.0f;
        };

        struct CombatSample
        {
            uint32 botGuid = 0;
            uint8 race = 0;
            uint8 clazz = 0;
            int8 specTab = -1;
            uint8 level = 0;
            uint32 mapId = 0;
            uint32 instanceId = 0;
            uint32 zoneId = 0;
            uint32 areaId = 0;
            int32 bucketX = 0;
            int32 bucketY = 0;
            std::string combatContext;
            uint8 groupSize = 0;
            bool isDungeon = false;
            bool isRaid = false;
            bool isBattleground = false;
            uint32 durationMs = 0;
            uint32 targetEntry = 0;
            uint8 targetLevel = 0;
            bool targetIsPlayer = false;
            uint32 targetSwitches = 0;
            float minHealthPct = 0.0f;
            float minPowerPct = 0.0f;
            bool endedAlive = false;
            bool deathObserved = false;
            int32 levelDelta = 0;
            int32 xpDelta = 0;
            int32 moneyDelta = 0;
            // Single fitness score for this fight (computed from the outcome fields
            // above). This is the LEARNING SIGNAL: every fight becomes a labeled
            // training example (context -> reward). Higher = better fight.
            float reward = 0.0f;
            // A/B EXPERIMENT cohort (bot guid % cohorts). Each cohort runs a different
            // parameter value this run; comparing avg reward by cohort = what we learn.
            uint8 cohort = 0;
            // Links this fight's REWARD to the per-decision (state,action) rows logged
            // during it (DecisionSample.fightId). Join on it to get (state,action,reward).
            uint32 fightId = 0;
        };

        // One sampled COMBAT DECISION: the bot's state + the action it took. Joined to
        // CombatSample by fightId to attach the fight's reward -> (state, action, reward)
        // tuples = the dataset for learning a per-class rotation POLICY (which action
        // wins in which state) instead of hand-coded triggers.
        struct DecisionSample
        {
            uint32 fightId = 0;
            uint32 botGuid = 0;
            uint8 clazz = 0;
            int8 specTab = -1;
            uint8 level = 0;
            uint8 cohort = 0;
            uint8 healthPct = 100;        // bot HP%
            uint8 powerPct = 100;         // bot mana/energy/rage %
            uint8 targetHealthPct = 100;  // current target HP%
            uint8 attackerCount = 0;      // mobs attacking the bot
            bool inMelee = false;         // in melee range of the target
            int8 levelDelta = 0;          // target level - bot level
            std::string action;           // executed action name
        };

        struct BotTelemetryState
        {
            bool initialized = false;
            uint32 nextSampleMs = 0;
            uint32 lastSampleMs = 0;
            uint8 lastLevel = 0;
            uint32 lastXp = 0;
            uint32 lastMoney = 0;
            uint32 lastActiveQuests = 0;
            uint32 lastCompleteQuests = 0;
            uint32 lastRewardedQuests = 0;

            bool combatActive = false;
            uint32 combatStartMs = 0;
            uint8 combatStartLevel = 0;
            uint32 combatStartXp = 0;
            uint32 combatStartMoney = 0;
            uint32 combatTargetEntry = 0;
            uint8 combatTargetLevel = 0;
            bool combatTargetIsPlayer = false;
            uint32 combatTargetGuid = 0;
            uint32 combatTargetSwitches = 0;
            float combatMinHealthPct = 100.0f;
            float combatMinPowerPct = 100.0f;
            bool combatDeathObserved = false;
            uint32 fightId = 0;          // unique id for the current fight (decision<->reward link)
            uint32 lastDecisionMs = 0;   // throttle decision sampling to ~1/sec per bot
        };

        int32 BucketCoord(float value, float size = 25.0f) const;
        float GetBlackspotPenalty(WorldPosition const* position, std::string const& type, std::string* reason) const;
        float GetObjectivePenalty(TravelDestination* destination, std::string* reason) const;
        void EnsureTelemetryTables();
        void QueueTaskSample(TaskSample const& sample);
        void QueueCombatSample(CombatSample const& sample);
        void QueueDecisionSample(DecisionSample const& sample);
        uint32 FlushTaskSamples(std::vector<TaskSample> const& samples);
        uint32 FlushCombatSamples(std::vector<CombatSample> const& samples);
        uint32 FlushDecisionSamples(std::vector<DecisionSample> const& samples);

        std::mutex loadMutex;
        std::mutex telemetryMutex;
        bool loaded = false;
        bool telemetryTablesReady = false;
        std::vector<LearnedBlackspot> blackspots;
        std::vector<LearnedObjectiveStat> objectiveStats;
        std::unordered_map<uint32, BotTelemetryState> telemetryStates;
        std::deque<TaskSample> taskSampleQueue;
        std::deque<CombatSample> combatSampleQueue;
        std::deque<DecisionSample> decisionSampleQueue;
        uint32 lastTelemetryFlushMs = 0;
        uint32 droppedTaskSamples = 0;
        uint32 droppedCombatSamples = 0;
        uint32 droppedDecisionSamples = 0;
        uint32 nextFightId = 1;

        // Learned action-value table: key "class|hpBucket|action" -> bounded relevance
        // bonus. Read by GetActionRelevanceBonus (engine, hot path), rebuilt by
        // ComputeAndLoadPolicy. Guarded by its own mutex so engine reads don't contend
        // on telemetryMutex.
        std::unordered_map<std::string, float> policyBonus;
        std::mutex policyMutex;
        // class -> best spec tab (0-2) by avg fight reward; -1 = insufficient data. Guarded by policyMutex.
        std::array<int, 16> learnedBestSpec{};
        bool learnedSpecReady = false;
        uint32 lastPolicyComputeMs = 0;
    };
}

#define sBotLearningMgr MaNGOS::Singleton<ai::BotLearningMgr>::Instance()
