#include "playerbot/BotLearningMgr.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "PerfStats.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace ai;

INSTANTIATE_SINGLETON_1(ai::BotLearningMgr);

namespace
{
    constexpr uint32 TASK_SAMPLE_START_SPREAD_MS = 60 * IN_MILLISECONDS;

    struct QuestCounters
    {
        uint32 active = 0;
        uint32 complete = 0;
        uint32 rewarded = 0;
    };

    QuestCounters CountQuestCounters(Player* bot)
    {
        QuestCounters counters;
        if (!bot)
            return counters;

        for (QuestStatusMap::value_type const& entry : bot->getQuestStatusMap())
        {
            QuestStatusData const& status = entry.second;
            if (status.m_status == QUEST_STATUS_NONE)
                continue;

            if (status.m_rewarded)
            {
                ++counters.rewarded;
                continue;
            }

            ++counters.active;
            if (status.m_status == QUEST_STATUS_COMPLETE)
                ++counters.complete;
        }

        return counters;
    }

    int32 SignedDelta(uint32 current, uint32 previous)
    {
        if (current >= previous)
            return int32(current - previous);

        return -int32(previous - current);
    }

    uint32 GetUnitEntry(Unit* unit)
    {
        if (!unit)
            return 0;

        if (Creature* creature = unit->ToCreature())
            return creature->GetEntry();

        return 0;
    }

    std::string GetTaskType(PlayerbotAI* ai, Player* bot, bool isCombat, bool isDungeon, bool isRaid, bool isBattleground, QuestCounters const& quests)
    {
        if (!bot || !bot->IsAlive())
            return "dead";

        if (isBattleground || bot->InBattleGround())
            return isCombat ? "pvp_combat" : "pvp";

        if (isRaid)
            return isCombat ? "raid_combat" : "raid";

        if (isDungeon)
            return isCombat ? "dungeon_combat" : "dungeon";

        if (isCombat)
            return "combat";

        if (bot->GetLootGuid())
            return "loot";

        if (bot->IsTaxiFlying() || bot->IsBeingTeleported())
            return "travel";

        if (quests.active || quests.complete)
            return "questing";

        if (ai && ai->HasRealPlayerMaster())
            return "assist_master";

        return "rpg";
    }

    std::string GetCombatContext(bool isDungeon, bool isRaid, bool isBattleground)
    {
        if (isBattleground)
            return "pvp";
        if (isRaid)
            return "raid";
        if (isDungeon)
            return "dungeon";
        return "world";
    }
}

void BotLearningMgr::Load()
{
    std::lock_guard<std::mutex> guard(loadMutex);

    if (loaded)
        return;

    loaded = true;
    blackspots.clear();
    objectiveStats.clear();

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_learned_blackspot` ("
        "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`source_run_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`blackspot_type` VARCHAR(32) NOT NULL DEFAULT 'path',"
        "`map_id` INT NOT NULL DEFAULT -1,"
        "`bucket_x` INT NOT NULL DEFAULT 0,"
        "`bucket_y` INT NOT NULL DEFAULT 0,"
        "`radius` FLOAT NOT NULL DEFAULT 25,"
        "`reason` VARCHAR(128) NOT NULL DEFAULT '',"
        "`hit_count` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`penalty` FLOAT NOT NULL DEFAULT 1,"
        "`confidence` FLOAT NOT NULL DEFAULT 0,"
        "`first_seen_at` DATETIME NOT NULL,"
        "`last_seen_at` DATETIME NOT NULL,"
        "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        "PRIMARY KEY (`id`),"
        "UNIQUE KEY `idx_blackspot_unique` (`blackspot_type`, `map_id`, `bucket_x`, `bucket_y`, `reason`),"
        "KEY `idx_blackspot_lookup` (`enabled`, `map_id`, `bucket_x`, `bucket_y`),"
        "KEY `idx_blackspot_reason` (`blackspot_type`, `reason`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

    EnsureTelemetryTables();

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_objective_stats` ("
        "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`source_run_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`objective_type` VARCHAR(32) NOT NULL DEFAULT '',"
        "`quest_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`entry` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`name` VARCHAR(128) NOT NULL DEFAULT '',"
        "`map_id` INT NOT NULL DEFAULT -1,"
        "`bucket_x` INT NOT NULL DEFAULT 0,"
        "`bucket_y` INT NOT NULL DEFAULT 0,"
        "`attempt_count` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`success_count` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`failure_count` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`avg_seconds` FLOAT NOT NULL DEFAULT 0,"
        "`pain_score` FLOAT NOT NULL DEFAULT 0,"
        "`learned_penalty` FLOAT NOT NULL DEFAULT 0,"
        "`last_failure_reason` VARCHAR(128) NOT NULL DEFAULT '',"
        "`first_seen_at` DATETIME NOT NULL,"
        "`last_seen_at` DATETIME NOT NULL,"
        "PRIMARY KEY (`id`),"
        "KEY `idx_objective_lookup` (`objective_type`, `quest_id`, `entry`),"
        "KEY `idx_objective_area` (`map_id`, `bucket_x`, `bucket_y`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

    if (auto result = WorldDatabase.PQuery(
            "SELECT `blackspot_type`, `map_id`, `bucket_x`, `bucket_y`, `radius`, `reason`, `hit_count`, `penalty`, `confidence` "
            "FROM `ai_playerbot_learned_blackspot` "
            "WHERE `enabled` = 1 AND `confidence` >= 0.65 AND `penalty` > 0 "
            "ORDER BY `confidence` DESC, `hit_count` DESC LIMIT 2000"))
    {
        do
        {
            Field* fields = result->Fetch();

            LearnedBlackspot spot;
            spot.type = fields[0].GetString();
            spot.mapId = fields[1].GetInt32();
            spot.bucketX = fields[2].GetInt32();
            spot.bucketY = fields[3].GetInt32();
            spot.radius = fields[4].GetFloat();
            spot.reason = fields[5].GetString();
            spot.hitCount = fields[6].GetUInt32();
            spot.penalty = fields[7].GetFloat();
            spot.confidence = fields[8].GetFloat();

            blackspots.push_back(spot);
        } while (result->NextRow());
    }

    if (auto result = WorldDatabase.PQuery(
            "SELECT `objective_type`, `quest_id`, `entry`, `name`, `learned_penalty`, `failure_count` "
            "FROM `ai_playerbot_objective_stats` "
            "WHERE `learned_penalty` > 0 AND `failure_count` >= 20 "
            "ORDER BY `failure_count` DESC LIMIT 2000"))
    {
        do
        {
            Field* fields = result->Fetch();

            LearnedObjectiveStat stat;
            stat.type = fields[0].GetString();
            stat.questId = fields[1].GetUInt32();
            stat.entry = fields[2].GetUInt32();
            stat.name = fields[3].GetString();
            stat.penalty = fields[4].GetFloat();
            stat.failureCount = fields[5].GetUInt32();

            objectiveStats.push_back(stat);
        } while (result->NextRow());
    }

    sLog.outString(">> Loaded %u AI playerbot learned blackspots and %u objective stats.",
        (uint32)blackspots.size(), (uint32)objectiveStats.size());
}

void BotLearningMgr::Reload()
{
    std::lock_guard<std::mutex> guard(loadMutex);
    loaded = false;
    blackspots.clear();
    objectiveStats.clear();
}

bool BotLearningMgr::HasData()
{
    if (!loaded)
        Load();

    return !blackspots.empty() || !objectiveStats.empty();
}

void BotLearningMgr::EnsureTelemetryTables()
{
    if (telemetryTablesReady)
        return;

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_task_sample` ("
        "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`sampled_at` DATETIME NOT NULL,"
        "`bot_guid` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`account_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`race` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`class` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`spec_tab` TINYINT NOT NULL DEFAULT -1,"
        "`level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`map_id` INT NOT NULL DEFAULT -1,"
        "`instance_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`zone_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`area_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`bucket_x` INT NOT NULL DEFAULT 0,"
        "`bucket_y` INT NOT NULL DEFAULT 0,"
        "`task_type` VARCHAR(32) NOT NULL DEFAULT '',"
        "`is_random_bot` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`has_real_master` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`group_size` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_group_leader` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_dungeon` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_raid` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_battleground` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_combat` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_alive` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        "`health_pct` FLOAT NOT NULL DEFAULT 0,"
        "`power_pct` FLOAT NOT NULL DEFAULT 0,"
        "`elapsed_ms` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`level_delta` INT NOT NULL DEFAULT 0,"
        "`xp_delta` INT NOT NULL DEFAULT 0,"
        "`money_delta` INT NOT NULL DEFAULT 0,"
        "`active_quests` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`complete_quests` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`rewarded_quests` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`active_quest_delta` INT NOT NULL DEFAULT 0,"
        "`complete_quest_delta` INT NOT NULL DEFAULT 0,"
        "`target_entry` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_health_pct` FLOAT NOT NULL DEFAULT 0,"
        "`notes` VARCHAR(255) NOT NULL DEFAULT '',"
        "PRIMARY KEY (`id`),"
        "KEY `idx_task_sample_bot_time` (`bot_guid`, `sampled_at`),"
        "KEY `idx_task_sample_context` (`task_type`, `class`, `spec_tab`, `level`, `map_id`, `zone_id`),"
        "KEY `idx_task_sample_lod` (`map_id`, `bucket_x`, `bucket_y`, `task_type`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_combat_sample` ("
        "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`ended_at` DATETIME NOT NULL,"
        "`bot_guid` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`race` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`class` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`spec_tab` TINYINT NOT NULL DEFAULT -1,"
        "`level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`map_id` INT NOT NULL DEFAULT -1,"
        "`instance_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`zone_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`area_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`bucket_x` INT NOT NULL DEFAULT 0,"
        "`bucket_y` INT NOT NULL DEFAULT 0,"
        "`combat_context` VARCHAR(32) NOT NULL DEFAULT '',"
        "`group_size` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_dungeon` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_raid` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`is_battleground` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`duration_ms` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_entry` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_is_player` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_switches` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`min_health_pct` FLOAT NOT NULL DEFAULT 100,"
        "`min_power_pct` FLOAT NOT NULL DEFAULT 100,"
        "`ended_alive` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        "`death_observed` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`level_delta` INT NOT NULL DEFAULT 0,"
        "`xp_delta` INT NOT NULL DEFAULT 0,"
        "`money_delta` INT NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`id`),"
        "KEY `idx_combat_sample_context` (`combat_context`, `class`, `spec_tab`, `level`, `map_id`, `zone_id`),"
        "KEY `idx_combat_sample_target` (`target_entry`, `class`, `spec_tab`, `level`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_lod_projection_stat` ("
        "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`model_key` VARCHAR(64) NOT NULL DEFAULT 'baseline',"
        "`task_type` VARCHAR(32) NOT NULL DEFAULT '',"
        "`class` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`spec_tab` TINYINT NOT NULL DEFAULT -1,"
        "`level_min` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`level_max` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`map_id` INT NOT NULL DEFAULT -1,"
        "`zone_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`group_size_min` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`group_size_max` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`sample_count` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`xp_per_hour` FLOAT NOT NULL DEFAULT 0,"
        "`money_per_hour` FLOAT NOT NULL DEFAULT 0,"
        "`quest_complete_per_hour` FLOAT NOT NULL DEFAULT 0,"
        "`death_per_hour` FLOAT NOT NULL DEFAULT 0,"
        "`confidence` FLOAT NOT NULL DEFAULT 0,"
        "`updated_at` DATETIME NOT NULL,"
        "PRIMARY KEY (`id`),"
        "KEY `idx_lod_projection_lookup` (`model_key`, `task_type`, `class`, `spec_tab`, `level_min`, `level_max`, `map_id`, `zone_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

    telemetryTablesReady = true;
}

void BotLearningMgr::RecordBotTelemetry(PlayerbotAI* ai, uint32 /*elapsed*/)
{
    if (!sPlayerbotAIConfig.learningTelemetryEnabled)
        return;

    if (!ai)
        return;

    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInWorld())
        return;

    const uint32 guid = bot->GetGUIDLow();
    const uint32 now = WorldTimer::getMSTime();
    if (!guid)
        return;

    std::lock_guard<std::mutex> guard(telemetryMutex);
    EnsureTelemetryTables();

    BotTelemetryState& state = telemetryStates[guid];
    const uint8 level = bot->GetLevel();
    const uint32 xp = bot->GetUInt32Value(PLAYER_XP);
    const uint32 money = bot->GetMoney();
    QuestCounters questCounters = CountQuestCounters(bot);

    Map* map = bot->GetMap();
    const bool isDungeon = map && map->IsDungeon();
    const bool isRaid = map && map->IsRaid();
    const bool isBattleground = (map && map->IsBattleGround()) || bot->InBattleGround();
    const bool isCombat = bot->IsAlive() && sServerFacade.IsInCombat(bot);
    const uint8 groupSize = bot->GetGroup() ? std::min<uint32>(bot->GetGroup()->GetMembersCount(), 255u) : 0u;
    const int8 specTab = int8(AiFactory::GetPlayerSpecTab(bot));
    const int32 bucketX = BucketCoord(bot->GetPositionX());
    const int32 bucketY = BucketCoord(bot->GetPositionY());

    if (!state.initialized)
    {
        state.initialized = true;
        state.nextSampleMs = now + (guid % TASK_SAMPLE_START_SPREAD_MS);
        state.lastSampleMs = now;
        state.lastLevel = level;
        state.lastXp = xp;
        state.lastMoney = money;
        state.lastActiveQuests = questCounters.active;
        state.lastCompleteQuests = questCounters.complete;
        state.lastRewardedQuests = questCounters.rewarded;
        return;
    }

    Unit* target = bot->GetVictim();
    if (!target && bot->GetSelectionGuid())
        target = ai->GetUnit(bot->GetSelectionGuid());

    const uint32 targetEntry = GetUnitEntry(target);
    const uint8 targetLevel = target ? std::min<uint32>(target->GetLevel(), 255u) : 0u;
    const float targetHealthPct = target ? target->GetHealthPercent() : 0.0f;
    const uint32 targetGuid = target ? target->GetGUIDLow() : 0u;
    const bool targetIsPlayer = target && target->IsPlayer();
    const float healthPct = bot->IsAlive() ? bot->GetHealthPercent() : 0.0f;
    const float powerPct = bot->IsAlive() ? bot->GetPowerPercent() : 0.0f;

    if (isCombat)
    {
        if (!state.combatActive)
        {
            state.combatActive = true;
            state.combatStartMs = now;
            state.combatStartLevel = level;
            state.combatStartXp = xp;
            state.combatStartMoney = money;
            state.combatTargetEntry = targetEntry;
            state.combatTargetLevel = targetLevel;
            state.combatTargetIsPlayer = targetIsPlayer;
            state.combatTargetGuid = targetGuid;
            state.combatTargetSwitches = 0;
            state.combatMinHealthPct = healthPct;
            state.combatMinPowerPct = powerPct;
            state.combatDeathObserved = !bot->IsAlive();
        }
        else
        {
            if (targetGuid && state.combatTargetGuid && targetGuid != state.combatTargetGuid)
                ++state.combatTargetSwitches;

            if (targetGuid)
            {
                state.combatTargetGuid = targetGuid;
                state.combatTargetEntry = targetEntry;
                state.combatTargetLevel = targetLevel;
                state.combatTargetIsPlayer = targetIsPlayer;
            }

            state.combatMinHealthPct = std::min(state.combatMinHealthPct, healthPct);
            state.combatMinPowerPct = std::min(state.combatMinPowerPct, powerPct);
            state.combatDeathObserved = state.combatDeathObserved || !bot->IsAlive();
        }
    }
    else if (state.combatActive)
    {
        const uint32 durationMs = std::max<uint32>(1, WorldTimer::getMSTimeDiff(state.combatStartMs, now));
        if (sPlayerbotAIConfig.learningCombatSampleRate >= 1.0f || frand(0.0f, 1.0f) <= sPlayerbotAIConfig.learningCombatSampleRate)
        {
            CombatSample sample;
            sample.botGuid = guid;
            sample.race = bot->getRace();
            sample.clazz = bot->getClass();
            sample.specTab = specTab;
            sample.level = level;
            sample.mapId = bot->GetMapId();
            sample.instanceId = bot->GetInstanceId();
            sample.zoneId = bot->GetZoneId();
            sample.areaId = bot->GetAreaId();
            sample.bucketX = bucketX;
            sample.bucketY = bucketY;
            sample.combatContext = GetCombatContext(isDungeon, isRaid, isBattleground);
            sample.groupSize = groupSize;
            sample.isDungeon = isDungeon;
            sample.isRaid = isRaid;
            sample.isBattleground = isBattleground;
            sample.durationMs = durationMs;
            sample.targetEntry = state.combatTargetEntry;
            sample.targetLevel = state.combatTargetLevel;
            sample.targetIsPlayer = state.combatTargetIsPlayer;
            sample.targetSwitches = state.combatTargetSwitches;
            sample.minHealthPct = state.combatMinHealthPct;
            sample.minPowerPct = state.combatMinPowerPct;
            sample.endedAlive = bot->IsAlive();
            sample.deathObserved = state.combatDeathObserved;
            sample.levelDelta = int32(level) - int32(state.combatStartLevel);
            sample.xpDelta = SignedDelta(xp, state.combatStartXp);
            sample.moneyDelta = SignedDelta(money, state.combatStartMoney);
            QueueCombatSample(sample);
        }

        state.combatActive = false;
    }

    const uint32 taskSampleInterval = std::max<uint32>(1000, sPlayerbotAIConfig.learningTaskSampleIntervalMs);
    const bool sampleDue = state.nextSampleMs <= now || WorldTimer::getMSTimeDiff(now, state.nextSampleMs) > taskSampleInterval * 4;
    if (!sampleDue)
        return;

    const uint32 elapsedMs = std::max<uint32>(1, WorldTimer::getMSTimeDiff(state.lastSampleMs, now));
    const std::string taskType = GetTaskType(ai, bot, isCombat, isDungeon, isRaid, isBattleground, questCounters);

    TaskSample sample;
    sample.botGuid = guid;
    sample.accountId = sObjectMgr.GetPlayerAccountIdByGUID(bot->GetObjectGuid());
    sample.race = bot->getRace();
    sample.clazz = bot->getClass();
    sample.specTab = specTab;
    sample.level = level;
    sample.mapId = bot->GetMapId();
    sample.instanceId = bot->GetInstanceId();
    sample.zoneId = bot->GetZoneId();
    sample.areaId = bot->GetAreaId();
    sample.bucketX = bucketX;
    sample.bucketY = bucketY;
    sample.taskType = taskType;
    sample.isRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
    sample.hasRealMaster = ai->HasRealPlayerMaster();
    sample.groupSize = groupSize;
    sample.isGroupLeader = ai->IsGroupLeader();
    sample.isDungeon = isDungeon;
    sample.isRaid = isRaid;
    sample.isBattleground = isBattleground;
    sample.isCombat = isCombat;
    sample.isAlive = bot->IsAlive();
    sample.healthPct = healthPct;
    sample.powerPct = powerPct;
    sample.elapsedMs = elapsedMs;
    sample.levelDelta = int32(level) - int32(state.lastLevel);
    sample.xpDelta = SignedDelta(xp, state.lastXp);
    sample.moneyDelta = SignedDelta(money, state.lastMoney);
    sample.activeQuests = questCounters.active;
    sample.completeQuests = questCounters.complete;
    sample.rewardedQuests = questCounters.rewarded;
    sample.activeQuestDelta = int32(questCounters.active) - int32(state.lastActiveQuests);
    sample.completeQuestDelta = int32(questCounters.complete) - int32(state.lastCompleteQuests);
    sample.targetEntry = targetEntry;
    sample.targetLevel = targetLevel;
    sample.targetHealthPct = targetHealthPct;
    QueueTaskSample(sample);

    state.lastSampleMs = now;
    state.nextSampleMs = now + taskSampleInterval + (guid % 5000);
    state.lastLevel = level;
    state.lastXp = xp;
    state.lastMoney = money;
    state.lastActiveQuests = questCounters.active;
    state.lastCompleteQuests = questCounters.complete;
    state.lastRewardedQuests = questCounters.rewarded;
}

void BotLearningMgr::QueueTaskSample(TaskSample const& sample)
{
    const uint32 maxQueue = std::max<uint32>(100, sPlayerbotAIConfig.learningMaxQueue);
    if (taskSampleQueue.size() + combatSampleQueue.size() >= maxQueue)
    {
        ++droppedTaskSamples;
        return;
    }

    taskSampleQueue.push_back(sample);
}

void BotLearningMgr::QueueCombatSample(CombatSample const& sample)
{
    const uint32 maxQueue = std::max<uint32>(100, sPlayerbotAIConfig.learningMaxQueue);
    if (taskSampleQueue.size() + combatSampleQueue.size() >= maxQueue)
    {
        if (!taskSampleQueue.empty())
            taskSampleQueue.pop_front();
        else
        {
            ++droppedCombatSamples;
            return;
        }
    }

    combatSampleQueue.push_back(sample);
}

void BotLearningMgr::FlushTelemetry()
{
    if (!sPlayerbotAIConfig.learningTelemetryEnabled)
        return;

    const uint32 now = WorldTimer::getMSTime();
    const uint32 flushInterval = std::max<uint32>(1000, sPlayerbotAIConfig.learningFlushIntervalMs);
    if (lastTelemetryFlushMs && WorldTimer::getMSTimeDiff(lastTelemetryFlushMs, now) < flushInterval)
        return;

    std::vector<TaskSample> taskSamples;
    std::vector<CombatSample> combatSamples;
    const uint32 maxRows = std::max<uint32>(1, sPlayerbotAIConfig.learningFlushMaxRows);
    uint32 droppedTasks = 0;
    uint32 droppedCombats = 0;
    uint32 queuedTasks = 0;
    uint32 queuedCombats = 0;

    {
        std::lock_guard<std::mutex> guard(telemetryMutex);
        lastTelemetryFlushMs = now;

        const uint32 combatTake = std::min<uint32>(maxRows, combatSampleQueue.size());
        combatSamples.reserve(combatTake);
        for (uint32 i = 0; i < combatTake; ++i)
        {
            combatSamples.push_back(combatSampleQueue.front());
            combatSampleQueue.pop_front();
        }

        const uint32 taskTake = std::min<uint32>(maxRows, taskSampleQueue.size());
        taskSamples.reserve(taskTake);
        for (uint32 i = 0; i < taskTake; ++i)
        {
            taskSamples.push_back(taskSampleQueue.front());
            taskSampleQueue.pop_front();
        }

        droppedTasks = droppedTaskSamples;
        droppedCombats = droppedCombatSamples;
        droppedTaskSamples = 0;
        droppedCombatSamples = 0;
        queuedTasks = taskSampleQueue.size();
        queuedCombats = combatSampleQueue.size();
    }

    if (taskSamples.empty() && combatSamples.empty())
        return;

    EnsureTelemetryTables();
    const uint32 start = WorldTimer::getMSTime();
    const uint32 combatFlushed = FlushCombatSamples(combatSamples);
    const uint32 taskFlushed = FlushTaskSamples(taskSamples);
    const uint32 elapsed = WorldTimer::getMSTimeDiffToNow(start);
    PerfStats::RecordLearningTelemetryFlush(elapsed, taskFlushed, combatFlushed, droppedTasks, droppedCombats, queuedTasks, queuedCombats);

    if ((droppedTasks || droppedCombats || elapsed >= 50) && sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        sPlayerbotAIConfig.log(
            "bot_events.csv",
            "%s+00,%s,%s,POINT(0.00 0.00),0,0,0.00,%s,%s",
            sPlayerbotAIConfig.GetTimestampStr().c_str(), "SYSTEM", "LearningTelemetryFlush",
            std::to_string(taskFlushed + combatFlushed).c_str(),
            ("tasks=" + std::to_string(taskFlushed) + " combats=" + std::to_string(combatFlushed) +
                " droppedTasks=" + std::to_string(droppedTasks) + " droppedCombats=" + std::to_string(droppedCombats) +
                " ms=" + std::to_string(elapsed) + " queuedTasks=" + std::to_string(queuedTasks) +
                " queuedCombats=" + std::to_string(queuedCombats)).c_str());
    }
}

uint32 BotLearningMgr::FlushCombatSamples(std::vector<CombatSample> const& samples)
{
    if (samples.empty())
        return 0;

    std::ostringstream sql;
    sql << "INSERT INTO `ai_playerbot_combat_sample` "
        << "(`ended_at`,`bot_guid`,`race`,`class`,`spec_tab`,`level`,`map_id`,`instance_id`,`zone_id`,`area_id`,`bucket_x`,`bucket_y`,"
        << "`combat_context`,`group_size`,`is_dungeon`,`is_raid`,`is_battleground`,`duration_ms`,`target_entry`,`target_level`,"
        << "`target_is_player`,`target_switches`,`min_health_pct`,`min_power_pct`,`ended_alive`,`death_observed`,`level_delta`,`xp_delta`,`money_delta`) VALUES ";

    for (size_t i = 0; i < samples.size(); ++i)
    {
        CombatSample const& s = samples[i];
        if (i)
            sql << ',';

        sql << "(NOW()," << s.botGuid << ',' << uint32(s.race) << ',' << uint32(s.clazz) << ',' << int32(s.specTab) << ','
            << uint32(s.level) << ',' << s.mapId << ',' << s.instanceId << ',' << s.zoneId << ',' << s.areaId << ','
            << s.bucketX << ',' << s.bucketY << ",'" << s.combatContext << "'," << uint32(s.groupSize) << ','
            << (s.isDungeon ? 1 : 0) << ',' << (s.isRaid ? 1 : 0) << ',' << (s.isBattleground ? 1 : 0) << ','
            << s.durationMs << ',' << s.targetEntry << ',' << uint32(s.targetLevel) << ',' << (s.targetIsPlayer ? 1 : 0) << ','
            << s.targetSwitches << ',' << s.minHealthPct << ',' << s.minPowerPct << ',' << (s.endedAlive ? 1 : 0) << ','
            << (s.deathObserved ? 1 : 0) << ',' << s.levelDelta << ',' << s.xpDelta << ',' << s.moneyDelta << ')';
    }

    WorldDatabase.Execute(sql.str().c_str());
    return samples.size();
}

uint32 BotLearningMgr::FlushTaskSamples(std::vector<TaskSample> const& samples)
{
    if (samples.empty())
        return 0;

    std::ostringstream sql;
    sql << "INSERT INTO `ai_playerbot_task_sample` "
        << "(`sampled_at`,`bot_guid`,`account_id`,`race`,`class`,`spec_tab`,`level`,`map_id`,`instance_id`,`zone_id`,`area_id`,`bucket_x`,`bucket_y`,"
        << "`task_type`,`is_random_bot`,`has_real_master`,`group_size`,`is_group_leader`,`is_dungeon`,`is_raid`,`is_battleground`,`is_combat`,`is_alive`,"
        << "`health_pct`,`power_pct`,`elapsed_ms`,`level_delta`,`xp_delta`,`money_delta`,`active_quests`,`complete_quests`,`rewarded_quests`,"
        << "`active_quest_delta`,`complete_quest_delta`,`target_entry`,`target_level`,`target_health_pct`,`notes`) VALUES ";

    for (size_t i = 0; i < samples.size(); ++i)
    {
        TaskSample const& s = samples[i];
        if (i)
            sql << ',';

        sql << "(NOW()," << s.botGuid << ',' << s.accountId << ',' << uint32(s.race) << ',' << uint32(s.clazz) << ','
            << int32(s.specTab) << ',' << uint32(s.level) << ',' << s.mapId << ',' << s.instanceId << ',' << s.zoneId << ','
            << s.areaId << ',' << s.bucketX << ',' << s.bucketY << ",'" << s.taskType << "'," << (s.isRandomBot ? 1 : 0) << ','
            << (s.hasRealMaster ? 1 : 0) << ',' << uint32(s.groupSize) << ',' << (s.isGroupLeader ? 1 : 0) << ','
            << (s.isDungeon ? 1 : 0) << ',' << (s.isRaid ? 1 : 0) << ',' << (s.isBattleground ? 1 : 0) << ','
            << (s.isCombat ? 1 : 0) << ',' << (s.isAlive ? 1 : 0) << ',' << s.healthPct << ',' << s.powerPct << ','
            << s.elapsedMs << ',' << s.levelDelta << ',' << s.xpDelta << ',' << s.moneyDelta << ',' << s.activeQuests << ','
            << s.completeQuests << ',' << s.rewardedQuests << ',' << s.activeQuestDelta << ',' << s.completeQuestDelta << ','
            << s.targetEntry << ',' << uint32(s.targetLevel) << ',' << s.targetHealthPct << ",'')";
    }

    WorldDatabase.Execute(sql.str().c_str());
    return samples.size();
}

int32 BotLearningMgr::BucketCoord(float value, float size) const
{
    return int32(std::round(value / size) * size);
}

float BotLearningMgr::GetBlackspotPenalty(WorldPosition const* position, std::string const& type, std::string* reason) const
{
    if (!position)
        return 0.0f;

    const int32 bucketX = BucketCoord(position->getX());
    const int32 bucketY = BucketCoord(position->getY());
    const uint32 mapId = position->getMapId();
    float penalty = 0.0f;
    std::string bestReason;

    for (LearnedBlackspot const& spot : blackspots)
    {
        if (!type.empty() && spot.type != type)
            continue;

        if (spot.mapId >= 0 && uint32(spot.mapId) != mapId)
            continue;

        const int32 dx = std::abs(bucketX - spot.bucketX);
        const int32 dy = std::abs(bucketY - spot.bucketY);
        const float radius = std::max(spot.radius, 25.0f);
        if (dx > radius || dy > radius)
            continue;

        const float adjusted = spot.penalty * std::max(0.25f, spot.confidence);
        if (adjusted > penalty)
        {
            penalty = adjusted;
            bestReason = spot.type + ":" + spot.reason;
        }
    }

    if (reason && !bestReason.empty())
        *reason = bestReason;

    return penalty;
}

float BotLearningMgr::GetObjectivePenalty(TravelDestination* destination, std::string* reason) const
{
    if (!destination)
        return 0.0f;

    uint32 questId = 0;
    if (QuestTravelDestination* questDestination = dynamic_cast<QuestTravelDestination*>(destination))
        questId = questDestination->GetQuestId();

    const uint32 entry = std::abs(destination->GetEntry());
    float penalty = 0.0f;
    std::string bestReason;

    for (LearnedObjectiveStat const& stat : objectiveStats)
    {
        if (!stat.entry && !stat.questId)
            continue;

        if (stat.entry && (!entry || stat.entry != entry))
            continue;

        if (stat.questId && (!questId || stat.questId != questId))
            continue;

        if (stat.penalty > penalty)
        {
            penalty = stat.penalty;
            bestReason = stat.type + ":" + stat.name;
        }
    }

    if (reason && !bestReason.empty())
        *reason = bestReason;

    return penalty;
}

float BotLearningMgr::GetTravelPenalty(Player* /*bot*/, TravelDestination* destination, WorldPosition const* position, std::string* reason)
{
    if (!loaded)
        Load();

    if (!destination || !position)
        return 0.0f;

    std::string locationReason;
    std::string destinationReason;
    std::string objectiveReason;

    const float locationPenalty = GetBlackspotPenalty(position, "path_blackspot", &locationReason);
    const float destinationPenalty = GetBlackspotPenalty(position, "destination_blackspot", &destinationReason);
    const float objectivePenalty = GetObjectivePenalty(destination, &objectiveReason);

    float penalty = 0.0f;
    if (locationPenalty > penalty)
    {
        penalty = locationPenalty;
        if (reason)
            *reason = locationReason;
    }

    if (destinationPenalty > penalty)
    {
        penalty = destinationPenalty;
        if (reason)
            *reason = destinationReason;
    }

    if (objectivePenalty > penalty)
    {
        penalty = objectivePenalty;
        if (reason)
            *reason = objectiveReason;
    }

    return penalty;
}
