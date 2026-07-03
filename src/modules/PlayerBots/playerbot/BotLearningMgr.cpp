#include "playerbot/BotLearningMgr.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "PerfStats.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <sstream>
#include <thread>

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
        "`reward` FLOAT NOT NULL DEFAULT 0,"
        "`cohort` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`fight_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`id`),"
        "KEY `idx_combat_sample_fight` (`fight_id`),"
        "KEY `idx_combat_sample_context` (`combat_context`, `class`, `spec_tab`, `level`, `map_id`, `zone_id`),"
        "KEY `idx_combat_sample_target` (`target_entry`, `class`, `spec_tab`, `level`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

    // Per-decision (state, action) rows. Join to ai_playerbot_combat_sample on
    // fight_id to attach that fight's reward -> (state, action, reward) tuples for
    // learning a per-class rotation POLICY (best action per state).
    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_decision_sample` ("
        "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`logged_at` DATETIME NOT NULL,"
        "`fight_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`bot_guid` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`class` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`spec_tab` TINYINT NOT NULL DEFAULT -1,"
        "`level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`cohort` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`health_pct` TINYINT UNSIGNED NOT NULL DEFAULT 100,"
        "`power_pct` TINYINT UNSIGNED NOT NULL DEFAULT 100,"
        "`target_health_pct` TINYINT UNSIGNED NOT NULL DEFAULT 100,"
        "`attacker_count` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`in_melee` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`level_delta` TINYINT NOT NULL DEFAULT 0,"
        "`action` VARCHAR(48) NOT NULL DEFAULT '',"
        "PRIMARY KEY (`id`),"
        "KEY `idx_decision_fight` (`fight_id`),"
        "KEY `idx_decision_policy` (`class`, `spec_tab`, `action`)"
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
            state.fightId = nextFightId++;   // links this fight's decisions <-> reward
            state.lastDecisionMs = 0;
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

            // REWARD (the learning signal). A "good fight" = survived, killed
            // something worthwhile, kept HP, finished fast, vs a comparable-or-harder
            // target, without thrashing between targets. Tunable; this is the fitness
            // every offline learner / cohort comparison optimizes against.
            {
                float reward = 0.0f;
                if (sample.deathObserved)
                    reward -= 100.0f;                       // dying is the worst outcome
                else if (sample.endedAlive)
                    reward += 30.0f;                        // survived
                if (sample.xpDelta > 0)
                    reward += 40.0f;                        // actually killed something worthwhile
                reward += 0.30f * sample.minHealthPct;      // health kept (0..+30)
                const float secs = sample.durationMs / 1000.0f;
                if (secs <= 5.0f)
                    reward += 10.0f;                        // fast kill
                else if (secs >= 30.0f)
                    reward -= 20.0f;                        // drawn-out = struggling
                else
                    reward += 10.0f - 30.0f * ((secs - 5.0f) / 25.0f);
                const int levelGap = int32(sample.targetLevel) - int32(sample.level);
                if (sample.xpDelta > 0 && levelGap > 0)
                    reward += 5.0f * float(std::min(levelGap, 5));  // beat a higher-level mob
                if (sample.targetSwitches > 3)
                    reward -= 2.0f * float(sample.targetSwitches - 3); // target thrash
                sample.reward = reward;
            }
            sample.cohort = uint8(BotExperiment::Cohort(guid));
            sample.fightId = state.fightId;

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

void BotLearningMgr::QueueDecisionSample(DecisionSample const& sample)
{
    // Decisions are higher-volume than fights (~1/sec/bot) -> allow a larger buffer.
    const uint32 maxQueue = std::max<uint32>(100, sPlayerbotAIConfig.learningMaxQueue) * 4;
    if (decisionSampleQueue.size() >= maxQueue)
    {
        ++droppedDecisionSamples;
        return;
    }
    decisionSampleQueue.push_back(sample);
}

void BotLearningMgr::RecordCombatDecision(PlayerbotAI* ai, std::string const& action)
{
    if (!sPlayerbotAIConfig.learningTelemetryEnabled || !ai || action.empty())
        return;
    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat())
        return;

    const uint32 guid = bot->GetGUIDLow();
    const uint32 now = WorldTimer::getMSTime();

    // Throttle to ~1 decision/sec/bot and grab the current fightId (under the lock).
    uint32 fightId = 0;
    {
        std::lock_guard<std::mutex> guard(telemetryMutex);
        auto it = telemetryStates.find(guid);
        if (it == telemetryStates.end() || !it->second.combatActive || !it->second.fightId)
            return;
        BotTelemetryState& state = it->second;
        if (state.lastDecisionMs && WorldTimer::getMSTimeDiff(state.lastDecisionMs, now) < 1000)
            return;
        state.lastDecisionMs = now;
        fightId = state.fightId;
    }

    Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();

    DecisionSample s;
    s.fightId = fightId;
    s.botGuid = guid;
    s.clazz = bot->getClass();
    s.specTab = int8(AiFactory::GetPlayerSpecTab(bot));
    s.level = bot->GetLevel();
    s.cohort = uint8(BotExperiment::Cohort(guid));
    s.healthPct = uint8(bot->GetHealthPercent());
    s.powerPct = uint8(bot->GetPowerPercent());
    s.targetHealthPct = target ? uint8(target->GetHealthPercent()) : uint8(0);
    s.attackerCount = ai->GetAiObjectContext()->GetValue<uint8>("attackers count")->Get();
    s.inMelee = target && bot->CanReachWithMeleeAutoAttack(target);
    s.levelDelta = target ? int8(int32(target->GetLevel()) - int32(bot->GetLevel())) : int8(0);
    s.action = action;

    {
        std::lock_guard<std::mutex> guard(telemetryMutex);
        QueueDecisionSample(s);
    }
}

void BotLearningMgr::ComputeAndLoadPolicy()
{
    if (!sPlayerbotAIConfig.learnedPolicyEnabled)
        return;

    // Per (class, HP bucket, MANA bucket, action): average fight reward when that
    // action was used, and how many times. Buckets = 0 (<34%), 1 (34-66%), 2 (>=67%).
    // Mana state lets the policy learn e.g. "OOM caster -> melee/wand, stop casting".
    // NOTE: attacker_count is an UNSIGNED column; the old expression "attacker_count - 1" UNDERFLOWED
    // for 0 and made MariaDB abort the whole query (ERROR 1690) -> Query() returned null -> the policy
    // was NEVER computed even once, despite 24M+ recorded samples ("recording but not learning").
    // IF(>1,1,0) is the same bucket without the unsigned arithmetic.
    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT d.class, LEAST(2, FLOOR(d.health_pct/34)) AS hpb, "
        "LEAST(2, FLOOR(d.power_pct/34)) AS mpb, IF(d.attacker_count > 1, 1, 0) AS atkb, "
        "d.action, AVG(c.reward) AS avg_reward, COUNT(*) AS n "
        "FROM ai_playerbot_decision_sample d "
        "JOIN ai_playerbot_combat_sample c ON c.fight_id = d.fight_id "
        "GROUP BY d.class, hpb, mpb, atkb, d.action HAVING n >= 8"));
    if (!result)
    {
        sLog.outError("BotLearningMgr: policy query returned no result (SQL error or no data)");
        return;
    }

    auto bucketKey = [](uint32 cls, uint32 hpb, uint32 mpb, uint32 atkb)
    { return std::to_string(cls) + "|" + std::to_string(hpb) + "|" + std::to_string(mpb) + "|" + std::to_string(atkb); };

    struct Row { uint32 cls; uint32 hpb; uint32 mpb; uint32 atkb; std::string action; double avg; uint32 n; };
    std::vector<Row> rows;
    std::map<std::string, std::pair<double, double>> bucketAcc; // bucketKey -> (sum avg*n, sum n)
    do
    {
        Field* f = result->Fetch();
        Row r;
        r.cls = f[0].GetUInt32();
        r.hpb = f[1].GetUInt32();
        r.mpb = f[2].GetUInt32();
        r.atkb = f[3].GetUInt32();
        r.action = f[4].GetCppString();
        r.avg = f[5].GetFloat();
        r.n = f[6].GetUInt32();
        rows.push_back(r);
        auto& acc = bucketAcc[bucketKey(r.cls, r.hpb, r.mpb, r.atkb)];
        acc.first += r.avg * r.n;
        acc.second += r.n;
    } while (result->NextRow());

    // Bonus = how far this action's avg reward is from the bucket's mean reward,
    // scaled and clamped so it nudges (not overrides) the hand-coded relevances.
    std::unordered_map<std::string, float> fresh;
    const float SCALE = 0.12f, MAXB = 8.0f;
    for (Row const& r : rows)
    {
        auto& acc = bucketAcc[bucketKey(r.cls, r.hpb, r.mpb, r.atkb)];
        const double mean = acc.second > 0 ? acc.first / acc.second : 0.0;
        float bonus = float((r.avg - mean) * SCALE);
        bonus = std::max(-MAXB, std::min(MAXB, bonus));
        fresh[bucketKey(r.cls, r.hpb, r.mpb, r.atkb) + "|" + r.action] = bonus;
    }

    // LEARNED SPEC (theorycrafting from the fleet's own outcomes): avg fight reward per class+spec
    // over the newest samples. Feeds InitTalentsTree so new spec rolls bias toward what actually
    // wins fights, while exploration keeps the other specs generating data. Same bounded-id trick.
    std::array<int, 16> bestSpec;
    bestSpec.fill(-1);
    {
        std::array<double, 16 * 3> bestAvg;
        bestAvg.fill(-1e9);
        std::unique_ptr<QueryResult> specRes(WorldDatabase.Query(
            "SELECT class, spec_tab, AVG(reward) AS r, COUNT(*) AS n "
            "FROM ai_playerbot_combat_sample "
            "WHERE id > (SELECT MAX(id) FROM ai_playerbot_combat_sample) - 100000 "
            "GROUP BY class, spec_tab HAVING n >= 300"));
        if (specRes)
        {
            do
            {
                Field* f = specRes->Fetch();
                const uint32 cls = f[0].GetUInt32();
                const uint32 tab = f[1].GetUInt32();
                const double avg = f[2].GetFloat();
                if (cls < 16 && tab < 3 && avg > bestAvg[cls * 3])
                {
                    bestAvg[cls * 3] = avg;
                    bestSpec[cls] = (int)tab;
                }
            } while (specRes->NextRow());
        }
    }

    const size_t entries = fresh.size();
    {
        std::lock_guard<std::mutex> guard(policyMutex);
        policyBonus.swap(fresh);
        learnedBestSpec = bestSpec;
        learnedSpecReady = true;
    }
    // Provable heartbeat: this line in the server log is the evidence the fleet is actually LEARNING
    // (recompute ran + N state/action bonuses now steer action relevance).
    sLog.outString("BotLearningMgr: learned policy loaded, %zu state/action bonuses from %zu aggregate rows"
        " (learned specs: w=%d p=%d h=%d m=%d l=%d)",
        entries, rows.size(), bestSpec[1], bestSpec[5], bestSpec[3], bestSpec[8], bestSpec[9]);
}

int BotLearningMgr::GetLearnedBestSpec(uint8 cls)
{
    if (!sPlayerbotAIConfig.learnedPolicyEnabled || cls >= 16)
        return -1;
    std::lock_guard<std::mutex> guard(policyMutex);
    if (!learnedSpecReady)
        return -1;
    return learnedBestSpec[cls];
}

float BotLearningMgr::GetActionRelevanceBonus(Player* bot, uint8 attackerBucket, std::string const& action)
{
    if (!sPlayerbotAIConfig.learnedPolicyEnabled || !bot || action.empty())
        return 0.0f;
    uint32 hpb = uint32(bot->GetHealthPercent()) / 34u;
    if (hpb > 2u) hpb = 2u;
    uint32 mpb = uint32(bot->GetPowerPercent()) / 34u;
    if (mpb > 2u) mpb = 2u;
    const uint32 atkb = attackerBucket > 1u ? 1u : attackerBucket;
    const std::string key = std::to_string(bot->getClass()) + "|" + std::to_string(hpb) + "|" +
                            std::to_string(mpb) + "|" + std::to_string(atkb) + "|" + action;
    std::lock_guard<std::mutex> guard(policyMutex);
    auto it = policyBonus.find(key);
    return it != policyBonus.end() ? it->second : 0.0f;
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
    std::vector<DecisionSample> decisionSamples;
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

        // Decisions are higher-volume; drain up to 4x maxRows per flush.
        const uint32 decisionTake = std::min<uint32>(maxRows * 4, decisionSampleQueue.size());
        decisionSamples.reserve(decisionTake);
        for (uint32 i = 0; i < decisionTake; ++i)
        {
            decisionSamples.push_back(decisionSampleQueue.front());
            decisionSampleQueue.pop_front();
        }

        droppedTasks = droppedTaskSamples;
        droppedCombats = droppedCombatSamples;
        droppedTaskSamples = 0;
        droppedCombatSamples = 0;
        queuedTasks = taskSampleQueue.size();
        queuedCombats = combatSampleQueue.size();
    }

    if (taskSamples.empty() && combatSamples.empty() && decisionSamples.empty())
        return;

    EnsureTelemetryTables();
    const uint32 start = WorldTimer::getMSTime();
    const uint32 combatFlushed = FlushCombatSamples(combatSamples);
    const uint32 taskFlushed = FlushTaskSamples(taskSamples);
    FlushDecisionSamples(decisionSamples);
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

    // Rebuild the learned policy from the accumulated data every ~5 min, so bots get
    // smarter DURING the run as more fights are logged (online-ish learning).
    // OFF-THREAD: even bounded, the aggregation takes minutes on 24M+ rows -- running it inline
    // here would hard-freeze the world thread every cycle. The DB layer is already used from the
    // parallel continent-update threads, so a detached worker is safe; the result swaps in under
    // policyMutex. A guard flag prevents overlapping recomputes.
    // BOOT-TIME ONLY. The DB layer serializes on ONE connection, so even an off-thread policy query
    // BLOCKS every world-thread DB call while it runs -- the 2M-row version ran 4+ minutes and
    // hard-froze the world mid-run (caught live at 01:03: world logs stopped, query at 274s "Sending
    // data", the user felt it as a stall+crash on login). Even bounded to 150k rows it takes ~46s on
    // cold cache. So: compute the policy ONCE, shortly after startup (world barely ticking, nobody
    // online) and keep it static for the run. Samples accumulate all run and feed the NEXT boot --
    // still per-run refinement, zero runtime freeze risk.
    if (sPlayerbotAIConfig.learnedPolicyEnabled && !lastPolicyComputeMs)
    {
        lastPolicyComputeMs = now;
        static std::atomic<bool> policyComputeRunning{false};
        bool expected = false;
        if (policyComputeRunning.compare_exchange_strong(expected, true))
        {
            std::thread([this]()
            {
                ComputeAndLoadPolicy();
                policyComputeRunning.store(false);
            }).detach();
        }
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
        << "`target_is_player`,`target_switches`,`min_health_pct`,`min_power_pct`,`ended_alive`,`death_observed`,`level_delta`,`xp_delta`,`money_delta`,`reward`,`cohort`,`fight_id`) VALUES ";

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
            << (s.deathObserved ? 1 : 0) << ',' << s.levelDelta << ',' << s.xpDelta << ',' << s.moneyDelta << ',' << s.reward << ',' << uint32(s.cohort) << ',' << s.fightId << ')';
    }

    WorldDatabase.Execute(sql.str().c_str());
    return samples.size();
}

uint32 BotLearningMgr::FlushDecisionSamples(std::vector<DecisionSample> const& samples)
{
    if (samples.empty())
        return 0;

    std::ostringstream sql;
    sql << "INSERT INTO `ai_playerbot_decision_sample` "
        << "(`logged_at`,`fight_id`,`bot_guid`,`class`,`spec_tab`,`level`,`cohort`,`health_pct`,`power_pct`,"
        << "`target_health_pct`,`attacker_count`,`in_melee`,`level_delta`,`action`) VALUES ";

    for (size_t i = 0; i < samples.size(); ++i)
    {
        DecisionSample const& s = samples[i];
        if (i)
            sql << ',';

        std::string action = s.action;        // internal name; strip any quote/backslash just in case
        for (char& c : action)
            if (c == '\'' || c == '\\') c = ' ';

        sql << "(NOW()," << s.fightId << ',' << s.botGuid << ',' << uint32(s.clazz) << ',' << int32(s.specTab) << ','
            << uint32(s.level) << ',' << uint32(s.cohort) << ',' << uint32(s.healthPct) << ',' << uint32(s.powerPct) << ','
            << uint32(s.targetHealthPct) << ',' << uint32(s.attackerCount) << ',' << (s.inMelee ? 1 : 0) << ','
            << int32(s.levelDelta) << ",'" << action << "')";
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
