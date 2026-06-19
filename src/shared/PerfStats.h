#ifndef PERFSTATS_H
#define PERFSTATS_H

#include <atomic>
#include <cstdint>

namespace PerfStats
{
    enum BotPressureLevel : uint32_t
    {
        BOT_PRESSURE_NORMAL = 0,
        BOT_PRESSURE_PRESSURE = 1,
        BOT_PRESSURE_CRITICAL = 2
    };

    enum BotPressureWorkType : uint32_t
    {
        BOT_PRESSURE_WORK_QUEST_PLANNER = 0,
        BOT_PRESSURE_WORK_VISIBLE_OBJECTIVE_ATTACK = 1,
        BOT_PRESSURE_WORK_FORCE_CHASE = 2,
        BOT_PRESSURE_WORK_STALE_TARGET_RECOVER = 3,
        BOT_PRESSURE_WORK_PATH_RETRY = 4
    };

    extern int g_totalUnits;
    extern int g_totalCreatures;
    extern int g_totalPets;
    extern int g_totalPlayers;
    extern int g_totalCorpses;
    extern int g_totalItems;
    extern int g_totalGameObjects;
    extern int g_totalDynamicObjects;
    extern int g_totalQueryResults;
    extern int g_totalMaps;

    extern int g_slowestMapId;
    extern int g_slowestMapUpdateTime;
    extern int g_slowestMapInstanceId;
    extern int g_slowestMapPlayers;
    extern int g_slowestMapActiveNonPlayers;
    extern int g_slowestMapPendingObjectUpdates;
    extern int g_slowestMapPendingRelocations;
    extern int g_slowestMapWaitIterations;
    extern int g_slowestMapSessionsMs;
    extern int g_slowestMapPlayersMs;
    extern int g_slowestMapCellsMs;
    extern int g_slowestMapObjectsMs;
    extern int g_slowestMapRelocationMs;
    extern int g_slowestMapPlayers2Ms;
    extern int g_slowestMapWaitMs;
    extern int g_slowestMapWorkId;
    extern int g_slowestMapWorkTime;
    extern int g_slowestMapWorkInstanceId;
    extern int g_slowestMapWorkPlayers;
    extern int g_slowestMapWorkActiveNonPlayers;
    extern int g_slowestMapWorkPendingObjectUpdates;
    extern int g_slowestMapWorkPendingRelocations;
    extern int g_slowestMapWorkWaitIterations;
    extern int g_slowestMapWorkSessionsMs;
    extern int g_slowestMapWorkPlayersMs;
    extern int g_slowestMapWorkCellsMs;
    extern int g_slowestMapWorkObjectsMs;
    extern int g_slowestMapWorkRelocationMs;
    extern int g_slowestMapWorkPlayers2Ms;
    extern int g_slowestMapWorkWaitMs;

    extern std::atomic<uint64_t> g_travelQuestRequests;
    extern std::atomic<uint64_t> g_travelQuestRequestMs;
    extern std::atomic<uint64_t> g_travelQuestRequestMaxMs;
    extern std::atomic<uint64_t> g_travelQuestFetches;
    extern std::atomic<uint64_t> g_travelQuestStrictPartitionCalls;
    extern std::atomic<uint64_t> g_travelQuestSoftPartitionCalls;
    extern std::atomic<uint64_t> g_travelQuestBroadPartitionCalls;
    extern std::atomic<uint64_t> g_travelQuestAsyncJobs;
    extern std::atomic<uint64_t> g_travelQuestAsyncMs;
    extern std::atomic<uint64_t> g_travelQuestAsyncMaxMs;
    extern std::atomic<uint64_t> g_travelQuestAsyncEmptyJobs;
    extern std::atomic<uint64_t> g_travelQuestAsyncResultPoints;
    extern std::atomic<uint64_t> g_travelPartitionsCalls;
    extern std::atomic<uint64_t> g_travelPartitionsMs;
    extern std::atomic<uint64_t> g_travelPartitionsMaxMs;
    extern std::atomic<uint64_t> g_travelPartitionsDestinations;
    extern std::atomic<uint64_t> g_travelPartitionsResultPoints;
    extern std::atomic<uint64_t> g_travelPartitionsEmptyCalls;
    extern std::atomic<uint64_t> g_travelQuestPartitionCalls;
    extern std::atomic<uint64_t> g_travelQuestPartitionMs;
    extern std::atomic<uint64_t> g_playerbotPlannerChecks;
    extern std::atomic<uint64_t> g_playerbotPlannerAllowed;
    extern std::atomic<uint64_t> g_playerbotPlannerDeferred;
    extern std::atomic<uint64_t> g_playerbotPlannerFastLane;
    extern std::atomic<uint32_t> g_botPressureLevel;
    extern std::atomic<uint32_t> g_botPressureScore;
    extern std::atomic<uint64_t> g_botPressureUpdates;
    extern std::atomic<uint64_t> g_botPressureDeferredQuest;
    extern std::atomic<uint64_t> g_botPressureDeferredVisibleObjective;
    extern std::atomic<uint64_t> g_botPressureDeferredForceChase;
    extern std::atomic<uint64_t> g_botPressureDeferredStaleRecover;
    extern std::atomic<uint64_t> g_botPressureDeferredPath;

    extern std::atomic<uint64_t> g_updatePlayersCalls;
    extern std::atomic<uint64_t> g_updatePlayersMs;
    extern std::atomic<uint64_t> g_updatePlayersMaxMs;
    extern std::atomic<uint64_t> g_updatePlayersProcessed;
    extern std::atomic<uint64_t> g_updatePlayersSkipped;
    extern std::atomic<uint64_t> g_updatePlayersBotProcessed;
    extern std::atomic<uint64_t> g_updatePlayersBotSkipped;
    extern std::atomic<uint64_t> g_updatePlayersWaitBotOnlySkipped;
    extern std::atomic<uint64_t> g_playerUpdateCalls;
    extern std::atomic<uint64_t> g_playerUpdateMs;
    extern std::atomic<uint64_t> g_playerUpdateMaxMs;
    extern std::atomic<uint64_t> g_playerUpdateBotCalls;
    extern std::atomic<uint64_t> g_playerUpdateBotMs;
    extern std::atomic<uint64_t> g_playerUpdateBotMaxMs;
    extern std::atomic<uint64_t> g_playerbotHookCalls;
    extern std::atomic<uint64_t> g_playerbotHookMs;
    extern std::atomic<uint64_t> g_playerbotHookMaxMs;
    extern std::atomic<uint64_t> g_playerbotHookSuppressedCalls;
    extern std::atomic<uint64_t> g_playerbotHookSuppressedElapsedMs;
    extern std::atomic<uint64_t> g_playerbotAiCalls;
    extern std::atomic<uint64_t> g_playerbotAiMs;
    extern std::atomic<uint64_t> g_playerbotAiMaxMs;
    extern std::atomic<uint64_t> g_playerbotAiVisibleCalls;
    extern std::atomic<uint64_t> g_playerbotAiVisibleMs;
    extern std::atomic<uint64_t> g_playerbotAiVisibleMaxMs;
    extern std::atomic<uint64_t> g_playerbotAiBackgroundCalls;
    extern std::atomic<uint64_t> g_playerbotAiBackgroundMs;
    extern std::atomic<uint64_t> g_playerbotAiBackgroundMaxMs;
    extern std::atomic<uint64_t> g_playerbotMgrCalls;
    extern std::atomic<uint64_t> g_playerbotMgrMs;
    extern std::atomic<uint64_t> g_playerbotMgrMaxMs;
    extern std::atomic<uint64_t> g_learningFlushCalls;
    extern std::atomic<uint64_t> g_learningFlushMs;
    extern std::atomic<uint64_t> g_learningFlushMaxMs;
    extern std::atomic<uint64_t> g_learningTaskRowsFlushed;
    extern std::atomic<uint64_t> g_learningCombatRowsFlushed;
    extern std::atomic<uint64_t> g_learningTaskRowsDropped;
    extern std::atomic<uint64_t> g_learningCombatRowsDropped;
    extern std::atomic<uint64_t> g_learningTaskQueueSize;
    extern std::atomic<uint64_t> g_learningCombatQueueSize;

    void RecordTravelQuestRequest(uint32_t elapsedMs, uint32_t fetches);
    void RecordTravelQuestPlanCalls(uint32_t strictCalls, uint32_t softCalls, uint32_t broadCalls);
    void RecordTravelQuestAsyncJob(uint32_t elapsedMs, bool empty, uint32_t resultPoints);
    void RecordTravelPartitions(uint32_t elapsedMs, uint32_t destinations, uint32_t resultPoints, bool empty, bool questPurpose);
    void RecordPlayerbotPlanner(bool allowed, bool fastLane);
    void UpdateBotPressure(uint32_t worldUpdateMs, uint32_t mapMgrUpdateMs, uint32_t averageDiffMs, uint32_t currentDiffMs);
    uint32_t GetBotPressureLevel();
    uint32_t GetBotPressureScore();
    void RecordBotPressureDeferred(BotPressureWorkType workType);
    void ResetTravelStats();
    void RecordUpdatePlayers(uint32_t elapsedMs, uint32_t processed, uint32_t skipped, uint32_t botProcessed, uint32_t botSkipped);
    void RecordUpdatePlayersWaitBotOnlySkipped();
    void RecordPlayerUpdate(uint32_t elapsedMs, bool bot);
    void RecordPlayerbotHook(uint32_t elapsedMs);
    void RecordPlayerbotHookSuppressed(uint32_t elapsedMs);
    void RecordPlayerbotAi(uint32_t elapsedMs);
    void RecordPlayerbotAiLod(uint32_t elapsedMs, bool visible);
    void RecordPlayerbotMgr(uint32_t elapsedMs);
    void ResetPlayerUpdateStats();
    void RecordLearningTelemetryFlush(uint32_t elapsedMs, uint32_t taskRows, uint32_t combatRows, uint32_t droppedTasks, uint32_t droppedCombats, uint32_t queuedTasks, uint32_t queuedCombats);
    void ResetLearningTelemetryStats();

    extern thread_local bool g_suppressPlayerbotHooks;
};

#endif
