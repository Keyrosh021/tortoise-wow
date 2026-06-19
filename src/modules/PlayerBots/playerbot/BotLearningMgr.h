#pragma once

#include "Common.h"
#include "playerbot/TravelMgr.h"
#include <deque>
#include <mutex>
#include <string>
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
        void FlushTelemetry();

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
        };

        int32 BucketCoord(float value, float size = 25.0f) const;
        float GetBlackspotPenalty(WorldPosition const* position, std::string const& type, std::string* reason) const;
        float GetObjectivePenalty(TravelDestination* destination, std::string* reason) const;
        void EnsureTelemetryTables();
        void QueueTaskSample(TaskSample const& sample);
        void QueueCombatSample(CombatSample const& sample);
        uint32 FlushTaskSamples(std::vector<TaskSample> const& samples);
        uint32 FlushCombatSamples(std::vector<CombatSample> const& samples);

        std::mutex loadMutex;
        std::mutex telemetryMutex;
        bool loaded = false;
        bool telemetryTablesReady = false;
        std::vector<LearnedBlackspot> blackspots;
        std::vector<LearnedObjectiveStat> objectiveStats;
        std::unordered_map<uint32, BotTelemetryState> telemetryStates;
        std::deque<TaskSample> taskSampleQueue;
        std::deque<CombatSample> combatSampleQueue;
        uint32 lastTelemetryFlushMs = 0;
        uint32 droppedTaskSamples = 0;
        uint32 droppedCombatSamples = 0;
    };
}

#define sBotLearningMgr MaNGOS::Singleton<ai::BotLearningMgr>::Instance()
