#include "PerfStats.h"

namespace PerfStats
{
    int g_totalUnits;
    int g_totalCreatures;
    int g_totalPets;
    int g_totalPlayers;
    int g_totalCorpses;
    int g_totalItems;
    int g_totalGameObjects;
    int g_totalDynamicObjects;
    int g_totalQueryResults;
    int g_totalMaps;

    int g_slowestMapId;
    int g_slowestMapUpdateTime;
    int g_slowestMapInstanceId;
    int g_slowestMapPlayers;
    int g_slowestMapActiveNonPlayers;
    int g_slowestMapPendingObjectUpdates;
    int g_slowestMapPendingRelocations;
    int g_slowestMapWaitIterations;
    int g_slowestMapSessionsMs;
    int g_slowestMapPlayersMs;
    int g_slowestMapCellsMs;
    int g_slowestMapObjectsMs;
    int g_slowestMapRelocationMs;
    int g_slowestMapPlayers2Ms;
    int g_slowestMapWaitMs;
    int g_slowestMapWorkId;
    int g_slowestMapWorkTime;
    int g_slowestMapWorkInstanceId;
    int g_slowestMapWorkPlayers;
    int g_slowestMapWorkActiveNonPlayers;
    int g_slowestMapWorkPendingObjectUpdates;
    int g_slowestMapWorkPendingRelocations;
    int g_slowestMapWorkWaitIterations;
    int g_slowestMapWorkSessionsMs;
    int g_slowestMapWorkPlayersMs;
    int g_slowestMapWorkCellsMs;
    int g_slowestMapWorkObjectsMs;
    int g_slowestMapWorkRelocationMs;
    int g_slowestMapWorkPlayers2Ms;
    int g_slowestMapWorkWaitMs;

    std::atomic<uint64_t> g_travelQuestRequests;
    std::atomic<uint64_t> g_travelQuestRequestMs;
    std::atomic<uint64_t> g_travelQuestRequestMaxMs;
    std::atomic<uint64_t> g_travelQuestFetches;
    std::atomic<uint64_t> g_travelQuestStrictPartitionCalls;
    std::atomic<uint64_t> g_travelQuestSoftPartitionCalls;
    std::atomic<uint64_t> g_travelQuestBroadPartitionCalls;
    std::atomic<uint64_t> g_travelQuestAsyncJobs;
    std::atomic<uint64_t> g_travelQuestAsyncMs;
    std::atomic<uint64_t> g_travelQuestAsyncMaxMs;
    std::atomic<uint64_t> g_travelQuestAsyncEmptyJobs;
    std::atomic<uint64_t> g_travelQuestAsyncResultPoints;
    std::atomic<uint64_t> g_travelPartitionsCalls;
    std::atomic<uint64_t> g_travelPartitionsMs;
    std::atomic<uint64_t> g_travelPartitionsMaxMs;
    std::atomic<uint64_t> g_travelPartitionsDestinations;
    std::atomic<uint64_t> g_travelPartitionsResultPoints;
    std::atomic<uint64_t> g_travelPartitionsEmptyCalls;
    std::atomic<uint64_t> g_travelQuestPartitionCalls;
    std::atomic<uint64_t> g_travelQuestPartitionMs;
    std::atomic<uint64_t> g_playerbotPlannerChecks;
    std::atomic<uint64_t> g_playerbotPlannerAllowed;
    std::atomic<uint64_t> g_playerbotPlannerDeferred;
    std::atomic<uint64_t> g_playerbotPlannerFastLane;
    std::atomic<uint32_t> g_botPressureLevel;
    std::atomic<uint32_t> g_botPressureScore;
    std::atomic<uint64_t> g_botPressureUpdates;
    std::atomic<uint64_t> g_botPressureDeferredQuest;
    std::atomic<uint64_t> g_botPressureDeferredVisibleObjective;
    std::atomic<uint64_t> g_botPressureDeferredForceChase;
    std::atomic<uint64_t> g_botPressureDeferredStaleRecover;
    std::atomic<uint64_t> g_botPressureDeferredPath;
    std::atomic<uint64_t> g_updatePlayersCalls;
    std::atomic<uint64_t> g_updatePlayersMs;
    std::atomic<uint64_t> g_updatePlayersMaxMs;
    std::atomic<uint64_t> g_updatePlayersProcessed;
    std::atomic<uint64_t> g_updatePlayersSkipped;
    std::atomic<uint64_t> g_updatePlayersBotProcessed;
    std::atomic<uint64_t> g_updatePlayersBotSkipped;
    std::atomic<uint64_t> g_updatePlayersWaitBotOnlySkipped;
    std::atomic<uint64_t> g_playerUpdateCalls;
    std::atomic<uint64_t> g_playerUpdateMs;
    std::atomic<uint64_t> g_playerUpdateMaxMs;
    std::atomic<uint64_t> g_playerUpdateBotCalls;
    std::atomic<uint64_t> g_playerUpdateBotMs;
    std::atomic<uint64_t> g_playerUpdateBotMaxMs;
    std::atomic<uint64_t> g_playerbotHookCalls;
    std::atomic<uint64_t> g_playerbotHookMs;
    std::atomic<uint64_t> g_playerbotHookMaxMs;
    std::atomic<uint64_t> g_playerbotHookSuppressedCalls;
    std::atomic<uint64_t> g_playerbotHookSuppressedElapsedMs;
    std::atomic<uint64_t> g_playerbotAiCalls;
    std::atomic<uint64_t> g_playerbotAiMs;
    std::atomic<uint64_t> g_playerbotAiMaxMs;
    std::atomic<uint64_t> g_playerbotAiVisibleCalls;
    std::atomic<uint64_t> g_playerbotAiVisibleMs;
    std::atomic<uint64_t> g_playerbotAiVisibleMaxMs;
    std::atomic<uint64_t> g_playerbotAiBackgroundCalls;
    std::atomic<uint64_t> g_playerbotAiBackgroundMs;
    std::atomic<uint64_t> g_playerbotAiBackgroundMaxMs;
    std::atomic<uint64_t> g_playerbotMgrCalls;
    std::atomic<uint64_t> g_playerbotMgrMs;
    std::atomic<uint64_t> g_playerbotMgrMaxMs;
    std::atomic<uint64_t> g_learningFlushCalls;
    std::atomic<uint64_t> g_learningFlushMs;
    std::atomic<uint64_t> g_learningFlushMaxMs;
    std::atomic<uint64_t> g_learningTaskRowsFlushed;
    std::atomic<uint64_t> g_learningCombatRowsFlushed;
    std::atomic<uint64_t> g_learningTaskRowsDropped;
    std::atomic<uint64_t> g_learningCombatRowsDropped;
    std::atomic<uint64_t> g_learningTaskQueueSize;
    std::atomic<uint64_t> g_learningCombatQueueSize;
    thread_local bool g_suppressPlayerbotHooks;

    namespace
    {
        void AtomicMax(std::atomic<uint64_t>& target, uint64_t value)
        {
            uint64_t current = target.load(std::memory_order_relaxed);
            while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
            {
            }
        }
    }

    void RecordTravelQuestRequest(uint32_t elapsedMs, uint32_t fetches)
    {
        g_travelQuestRequests.fetch_add(1, std::memory_order_relaxed);
        g_travelQuestRequestMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        g_travelQuestFetches.fetch_add(fetches, std::memory_order_relaxed);
        AtomicMax(g_travelQuestRequestMaxMs, elapsedMs);
    }

    void RecordTravelQuestPlanCalls(uint32_t strictCalls, uint32_t softCalls, uint32_t broadCalls)
    {
        g_travelQuestStrictPartitionCalls.fetch_add(strictCalls, std::memory_order_relaxed);
        g_travelQuestSoftPartitionCalls.fetch_add(softCalls, std::memory_order_relaxed);
        g_travelQuestBroadPartitionCalls.fetch_add(broadCalls, std::memory_order_relaxed);
    }

    void RecordTravelQuestAsyncJob(uint32_t elapsedMs, bool empty, uint32_t resultPoints)
    {
        g_travelQuestAsyncJobs.fetch_add(1, std::memory_order_relaxed);
        g_travelQuestAsyncMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        g_travelQuestAsyncResultPoints.fetch_add(resultPoints, std::memory_order_relaxed);
        if (empty)
            g_travelQuestAsyncEmptyJobs.fetch_add(1, std::memory_order_relaxed);
        AtomicMax(g_travelQuestAsyncMaxMs, elapsedMs);
    }

    void RecordTravelPartitions(uint32_t elapsedMs, uint32_t destinations, uint32_t resultPoints, bool empty, bool questPurpose)
    {
        g_travelPartitionsCalls.fetch_add(1, std::memory_order_relaxed);
        g_travelPartitionsMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        g_travelPartitionsDestinations.fetch_add(destinations, std::memory_order_relaxed);
        g_travelPartitionsResultPoints.fetch_add(resultPoints, std::memory_order_relaxed);
        if (empty)
            g_travelPartitionsEmptyCalls.fetch_add(1, std::memory_order_relaxed);
        if (questPurpose)
        {
            g_travelQuestPartitionCalls.fetch_add(1, std::memory_order_relaxed);
            g_travelQuestPartitionMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        }
        AtomicMax(g_travelPartitionsMaxMs, elapsedMs);
    }

    void RecordPlayerbotPlanner(bool allowed, bool fastLane)
    {
        g_playerbotPlannerChecks.fetch_add(1, std::memory_order_relaxed);
        if (allowed)
            g_playerbotPlannerAllowed.fetch_add(1, std::memory_order_relaxed);
        else
            g_playerbotPlannerDeferred.fetch_add(1, std::memory_order_relaxed);
        if (fastLane)
            g_playerbotPlannerFastLane.fetch_add(1, std::memory_order_relaxed);
    }

    void UpdateBotPressure(uint32_t worldUpdateMs, uint32_t mapMgrUpdateMs, uint32_t averageDiffMs, uint32_t currentDiffMs)
    {
        int32_t delta = -1;

        if (currentDiffMs >= 250 || mapMgrUpdateMs >= 250 || worldUpdateMs >= 300)
            delta = 8;
        else if (currentDiffMs >= 180 || mapMgrUpdateMs >= 150 || averageDiffMs >= 160)
            delta = 4;
        else if (currentDiffMs >= 120 || mapMgrUpdateMs >= 90 || averageDiffMs >= 120)
            delta = 2;

        uint32_t current = g_botPressureScore.load(std::memory_order_relaxed);
        uint32_t desired = 0;
        do
        {
            int32_t next = static_cast<int32_t>(current) + delta;
            if (next < 0)
                next = 0;
            else if (next > 30)
                next = 30;
            desired = static_cast<uint32_t>(next);
        } while (!g_botPressureScore.compare_exchange_weak(current, desired, std::memory_order_relaxed));

        uint32_t level = BOT_PRESSURE_NORMAL;
        if (desired >= 18)
            level = BOT_PRESSURE_CRITICAL;
        else if (desired >= 8)
            level = BOT_PRESSURE_PRESSURE;

        g_botPressureLevel.store(level, std::memory_order_relaxed);
        g_botPressureUpdates.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t GetBotPressureLevel()
    {
        return g_botPressureLevel.load(std::memory_order_relaxed);
    }

    uint32_t GetBotPressureScore()
    {
        return g_botPressureScore.load(std::memory_order_relaxed);
    }

    void RecordBotPressureDeferred(BotPressureWorkType workType)
    {
        switch (workType)
        {
            case BOT_PRESSURE_WORK_QUEST_PLANNER:
                g_botPressureDeferredQuest.fetch_add(1, std::memory_order_relaxed);
                break;
            case BOT_PRESSURE_WORK_VISIBLE_OBJECTIVE_ATTACK:
                g_botPressureDeferredVisibleObjective.fetch_add(1, std::memory_order_relaxed);
                break;
            case BOT_PRESSURE_WORK_FORCE_CHASE:
                g_botPressureDeferredForceChase.fetch_add(1, std::memory_order_relaxed);
                break;
            case BOT_PRESSURE_WORK_STALE_TARGET_RECOVER:
                g_botPressureDeferredStaleRecover.fetch_add(1, std::memory_order_relaxed);
                break;
            case BOT_PRESSURE_WORK_PATH_RETRY:
                g_botPressureDeferredPath.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    void ResetTravelStats()
    {
        g_travelQuestRequests.store(0, std::memory_order_relaxed);
        g_travelQuestRequestMs.store(0, std::memory_order_relaxed);
        g_travelQuestRequestMaxMs.store(0, std::memory_order_relaxed);
        g_travelQuestFetches.store(0, std::memory_order_relaxed);
        g_travelQuestStrictPartitionCalls.store(0, std::memory_order_relaxed);
        g_travelQuestSoftPartitionCalls.store(0, std::memory_order_relaxed);
        g_travelQuestBroadPartitionCalls.store(0, std::memory_order_relaxed);
        g_travelQuestAsyncJobs.store(0, std::memory_order_relaxed);
        g_travelQuestAsyncMs.store(0, std::memory_order_relaxed);
        g_travelQuestAsyncMaxMs.store(0, std::memory_order_relaxed);
        g_travelQuestAsyncEmptyJobs.store(0, std::memory_order_relaxed);
        g_travelQuestAsyncResultPoints.store(0, std::memory_order_relaxed);
        g_travelPartitionsCalls.store(0, std::memory_order_relaxed);
        g_travelPartitionsMs.store(0, std::memory_order_relaxed);
        g_travelPartitionsMaxMs.store(0, std::memory_order_relaxed);
        g_travelPartitionsDestinations.store(0, std::memory_order_relaxed);
        g_travelPartitionsResultPoints.store(0, std::memory_order_relaxed);
        g_travelPartitionsEmptyCalls.store(0, std::memory_order_relaxed);
        g_travelQuestPartitionCalls.store(0, std::memory_order_relaxed);
        g_travelQuestPartitionMs.store(0, std::memory_order_relaxed);
        g_playerbotPlannerChecks.store(0, std::memory_order_relaxed);
        g_playerbotPlannerAllowed.store(0, std::memory_order_relaxed);
        g_playerbotPlannerDeferred.store(0, std::memory_order_relaxed);
        g_playerbotPlannerFastLane.store(0, std::memory_order_relaxed);
        g_botPressureUpdates.store(0, std::memory_order_relaxed);
        g_botPressureDeferredQuest.store(0, std::memory_order_relaxed);
        g_botPressureDeferredVisibleObjective.store(0, std::memory_order_relaxed);
        g_botPressureDeferredForceChase.store(0, std::memory_order_relaxed);
        g_botPressureDeferredStaleRecover.store(0, std::memory_order_relaxed);
        g_botPressureDeferredPath.store(0, std::memory_order_relaxed);
    }

    void RecordUpdatePlayers(uint32_t elapsedMs, uint32_t processed, uint32_t skipped, uint32_t botProcessed, uint32_t botSkipped)
    {
        g_updatePlayersCalls.fetch_add(1, std::memory_order_relaxed);
        g_updatePlayersMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        g_updatePlayersProcessed.fetch_add(processed, std::memory_order_relaxed);
        g_updatePlayersSkipped.fetch_add(skipped, std::memory_order_relaxed);
        g_updatePlayersBotProcessed.fetch_add(botProcessed, std::memory_order_relaxed);
        g_updatePlayersBotSkipped.fetch_add(botSkipped, std::memory_order_relaxed);
        AtomicMax(g_updatePlayersMaxMs, elapsedMs);
    }

    void RecordUpdatePlayersWaitBotOnlySkipped()
    {
        g_updatePlayersWaitBotOnlySkipped.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordPlayerUpdate(uint32_t elapsedMs, bool bot)
    {
        g_playerUpdateCalls.fetch_add(1, std::memory_order_relaxed);
        g_playerUpdateMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        AtomicMax(g_playerUpdateMaxMs, elapsedMs);
        if (bot)
        {
            g_playerUpdateBotCalls.fetch_add(1, std::memory_order_relaxed);
            g_playerUpdateBotMs.fetch_add(elapsedMs, std::memory_order_relaxed);
            AtomicMax(g_playerUpdateBotMaxMs, elapsedMs);
        }
    }

    void RecordPlayerbotHook(uint32_t elapsedMs)
    {
        g_playerbotHookCalls.fetch_add(1, std::memory_order_relaxed);
        g_playerbotHookMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        AtomicMax(g_playerbotHookMaxMs, elapsedMs);
    }

    void RecordPlayerbotHookSuppressed(uint32_t elapsedMs)
    {
        g_playerbotHookSuppressedCalls.fetch_add(1, std::memory_order_relaxed);
        g_playerbotHookSuppressedElapsedMs.fetch_add(elapsedMs, std::memory_order_relaxed);
    }

    void RecordPlayerbotAi(uint32_t elapsedMs)
    {
        g_playerbotAiCalls.fetch_add(1, std::memory_order_relaxed);
        g_playerbotAiMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        AtomicMax(g_playerbotAiMaxMs, elapsedMs);
    }

    void RecordPlayerbotAiLod(uint32_t elapsedMs, bool visible)
    {
        RecordPlayerbotAi(elapsedMs);

        if (visible)
        {
            g_playerbotAiVisibleCalls.fetch_add(1, std::memory_order_relaxed);
            g_playerbotAiVisibleMs.fetch_add(elapsedMs, std::memory_order_relaxed);
            AtomicMax(g_playerbotAiVisibleMaxMs, elapsedMs);
        }
        else
        {
            g_playerbotAiBackgroundCalls.fetch_add(1, std::memory_order_relaxed);
            g_playerbotAiBackgroundMs.fetch_add(elapsedMs, std::memory_order_relaxed);
            AtomicMax(g_playerbotAiBackgroundMaxMs, elapsedMs);
        }
    }

    void RecordPlayerbotMgr(uint32_t elapsedMs)
    {
        g_playerbotMgrCalls.fetch_add(1, std::memory_order_relaxed);
        g_playerbotMgrMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        AtomicMax(g_playerbotMgrMaxMs, elapsedMs);
    }

    void ResetPlayerUpdateStats()
    {
        g_updatePlayersCalls.store(0, std::memory_order_relaxed);
        g_updatePlayersMs.store(0, std::memory_order_relaxed);
        g_updatePlayersMaxMs.store(0, std::memory_order_relaxed);
        g_updatePlayersProcessed.store(0, std::memory_order_relaxed);
        g_updatePlayersSkipped.store(0, std::memory_order_relaxed);
        g_updatePlayersBotProcessed.store(0, std::memory_order_relaxed);
        g_updatePlayersBotSkipped.store(0, std::memory_order_relaxed);
        g_updatePlayersWaitBotOnlySkipped.store(0, std::memory_order_relaxed);
        g_playerUpdateCalls.store(0, std::memory_order_relaxed);
        g_playerUpdateMs.store(0, std::memory_order_relaxed);
        g_playerUpdateMaxMs.store(0, std::memory_order_relaxed);
        g_playerUpdateBotCalls.store(0, std::memory_order_relaxed);
        g_playerUpdateBotMs.store(0, std::memory_order_relaxed);
        g_playerUpdateBotMaxMs.store(0, std::memory_order_relaxed);
        g_playerbotHookCalls.store(0, std::memory_order_relaxed);
        g_playerbotHookMs.store(0, std::memory_order_relaxed);
        g_playerbotHookMaxMs.store(0, std::memory_order_relaxed);
        g_playerbotHookSuppressedCalls.store(0, std::memory_order_relaxed);
        g_playerbotHookSuppressedElapsedMs.store(0, std::memory_order_relaxed);
        g_playerbotAiCalls.store(0, std::memory_order_relaxed);
        g_playerbotAiMs.store(0, std::memory_order_relaxed);
        g_playerbotAiMaxMs.store(0, std::memory_order_relaxed);
        g_playerbotAiVisibleCalls.store(0, std::memory_order_relaxed);
        g_playerbotAiVisibleMs.store(0, std::memory_order_relaxed);
        g_playerbotAiVisibleMaxMs.store(0, std::memory_order_relaxed);
        g_playerbotAiBackgroundCalls.store(0, std::memory_order_relaxed);
        g_playerbotAiBackgroundMs.store(0, std::memory_order_relaxed);
        g_playerbotAiBackgroundMaxMs.store(0, std::memory_order_relaxed);
        g_playerbotMgrCalls.store(0, std::memory_order_relaxed);
        g_playerbotMgrMs.store(0, std::memory_order_relaxed);
        g_playerbotMgrMaxMs.store(0, std::memory_order_relaxed);
    }

    void RecordLearningTelemetryFlush(uint32_t elapsedMs, uint32_t taskRows, uint32_t combatRows, uint32_t droppedTasks, uint32_t droppedCombats, uint32_t queuedTasks, uint32_t queuedCombats)
    {
        g_learningFlushCalls.fetch_add(1, std::memory_order_relaxed);
        g_learningFlushMs.fetch_add(elapsedMs, std::memory_order_relaxed);
        g_learningTaskRowsFlushed.fetch_add(taskRows, std::memory_order_relaxed);
        g_learningCombatRowsFlushed.fetch_add(combatRows, std::memory_order_relaxed);
        g_learningTaskRowsDropped.fetch_add(droppedTasks, std::memory_order_relaxed);
        g_learningCombatRowsDropped.fetch_add(droppedCombats, std::memory_order_relaxed);
        g_learningTaskQueueSize.store(queuedTasks, std::memory_order_relaxed);
        g_learningCombatQueueSize.store(queuedCombats, std::memory_order_relaxed);
        AtomicMax(g_learningFlushMaxMs, elapsedMs);
    }

    void ResetLearningTelemetryStats()
    {
        g_learningFlushCalls.store(0, std::memory_order_relaxed);
        g_learningFlushMs.store(0, std::memory_order_relaxed);
        g_learningFlushMaxMs.store(0, std::memory_order_relaxed);
        g_learningTaskRowsFlushed.store(0, std::memory_order_relaxed);
        g_learningCombatRowsFlushed.store(0, std::memory_order_relaxed);
        g_learningTaskRowsDropped.store(0, std::memory_order_relaxed);
        g_learningCombatRowsDropped.store(0, std::memory_order_relaxed);
        g_learningTaskQueueSize.store(0, std::memory_order_relaxed);
        g_learningCombatQueueSize.store(0, std::memory_order_relaxed);
    }
};
