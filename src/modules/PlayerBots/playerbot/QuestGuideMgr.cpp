#include "playerbot/QuestGuideMgr.h"

#include "playerbot/playerbot.h"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

using namespace ai;

INSTANTIATE_SINGLETON_1(ai::QuestGuideMgr);

namespace
{
    constexpr uint32 CROSS_RACE_ROUTE_CHANCE = 22;
    constexpr int32 CROSS_RACE_HUB_SCORE_PENALTY = 350;

    uint32 GetTeamKey(Player* bot)
    {
        if (!bot)
            return 0;

        if (bot->GetTeam() == ALLIANCE)
            return 1;

        if (bot->GetTeam() == HORDE)
            return 2;

        return 0;
    }

    uint32 GetRaceMask(Player* bot)
    {
        return bot ? (1u << (bot->getRace() - 1)) : 0;
    }

    uint32 GetClassMask(Player* bot)
    {
        return bot ? (1u << (bot->getClass() - 1)) : 0;
    }

    uint32 StableRouteRoll(Player* bot)
    {
        uint32 value = bot->GetGUIDLow() ^ 0x9E3779B9u;
        value ^= value >> 16;
        value *= 2246822519u;
        value ^= value >> 13;
        return value % 100;
    }

    uint32 StableRouteIndex(Player* bot, uint32 count)
    {
        if (!count)
            return 0;

        uint32 value = bot->GetGUIDLow() ^ 0x85EBCA6Bu;
        value ^= value >> 15;
        value *= 3266489917u;
        value ^= value >> 16;
        return value % count;
    }
}

void QuestGuideMgr::Load()
{
    std::lock_guard<std::mutex> guard(loadMutex);

    if (loaded)
        return;

    loaded = true;
    hubs.clear();

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_quest_hub` ("
        "`id` INT UNSIGNED NOT NULL,"
        "`name` VARCHAR(100) NOT NULL DEFAULT '',"
        "`faction` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`race_mask` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`class_mask` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        "`max_level` TINYINT UNSIGNED NOT NULL DEFAULT 60,"
        "`map_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`area_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`x` FLOAT NOT NULL DEFAULT 0,"
        "`y` FLOAT NOT NULL DEFAULT 0,"
        "`z` FLOAT NOT NULL DEFAULT 0,"
        "`priority` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`next_hub_ids` VARCHAR(255) NOT NULL DEFAULT '',"
        "PRIMARY KEY (`id`)"
        ")");

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_quest_hub_quest` ("
        "`hub_id` INT UNSIGNED NOT NULL,"
        "`quest_id` INT UNSIGNED NOT NULL,"
        "`role` VARCHAR(24) NOT NULL DEFAULT 'pickup',"
        "`priority` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`required` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`chain_group` VARCHAR(64) NOT NULL DEFAULT '',"
        "PRIMARY KEY (`hub_id`, `quest_id`, `role`)"
        ")");

    std::unordered_map<uint32, size_t> hubIndex;
    if (auto result = WorldDatabase.PQuery(
            "SELECT `id`, `name`, `faction`, `race_mask`, `class_mask`, `min_level`, `max_level`, "
            "`map_id`, `area_id`, `x`, `y`, `z`, `priority`, `next_hub_ids` "
            "FROM `ai_playerbot_quest_hub` ORDER BY `priority` DESC, `id` ASC"))
    {
        do
        {
            Field* fields = result->Fetch();

            QuestGuideHub hub;
            hub.id = fields[0].GetUInt32();
            hub.name = fields[1].GetString();
            hub.faction = fields[2].GetUInt32();
            hub.raceMask = fields[3].GetUInt32();
            hub.classMask = fields[4].GetUInt32();
            hub.minLevel = fields[5].GetUInt32();
            hub.maxLevel = fields[6].GetUInt32();
            hub.mapId = fields[7].GetUInt32();
            hub.areaId = fields[8].GetUInt32();
            hub.x = fields[9].GetFloat();
            hub.y = fields[10].GetFloat();
            hub.z = fields[11].GetFloat();
            hub.priority = fields[12].GetUInt32();
            hub.nextHubIds = fields[13].GetString();

            hubIndex[hub.id] = hubs.size();
            hubs.push_back(hub);
        } while (result->NextRow());
    }

    if (auto result = WorldDatabase.PQuery(
            "SELECT `hub_id`, `quest_id`, `role`, `priority`, `required`, `chain_group` "
            "FROM `ai_playerbot_quest_hub_quest` ORDER BY `hub_id` ASC, `priority` DESC, `quest_id` ASC"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 hubId = fields[0].GetUInt32();
            auto itr = hubIndex.find(hubId);
            if (itr == hubIndex.end())
                continue;

            QuestGuideQuest quest;
            quest.questId = fields[1].GetUInt32();
            quest.role = fields[2].GetString();
            quest.priority = fields[3].GetUInt32();
            quest.required = fields[4].GetUInt32() != 0;
            quest.chainGroup = fields[5].GetString();

            hubs[itr->second].quests.push_back(quest);
        } while (result->NextRow());
    }

    uint32 questCount = 0;
    for (const QuestGuideHub& hub : hubs)
        questCount += hub.quests.size();

    sLog.outString(">> Loaded %u AI playerbot quest guide hubs and %u guide quests.", (uint32)hubs.size(), questCount);
}

void QuestGuideMgr::Reload()
{
    std::lock_guard<std::mutex> guard(loadMutex);
    loaded = false;
    hubs.clear();
}

bool QuestGuideMgr::HasGuideData()
{
    Load();
    return !hubs.empty();
}

const QuestGuideHub* QuestGuideMgr::FindHub(uint32 hubId) const
{
    for (const QuestGuideHub& hub : hubs)
        if (hub.id == hubId)
            return &hub;

    return nullptr;
}

std::vector<uint32> QuestGuideMgr::GetQuestGiverQuestIds(Player* bot, uint32 maxCount, std::string* debugTrace)
{
    Load();

    std::vector<const QuestGuideHub*> candidateHubs;
    candidateHubs.reserve(hubs.size());
    for (const QuestGuideHub& hub : hubs)
        candidateHubs.push_back(&hub);

    return GetQuestGiverQuestIdsFromHubs(bot, candidateHubs, maxCount, debugTrace);
}

std::vector<uint32> QuestGuideMgr::GetQuestGiverQuestIdsForHub(Player* bot, uint32 hubId, uint32 maxCount, std::string* debugTrace)
{
    Load();

    std::vector<const QuestGuideHub*> candidateHubs;
    if (const QuestGuideHub* hub = FindHub(hubId))
        candidateHubs.push_back(hub);

    return GetQuestGiverQuestIdsFromHubs(bot, candidateHubs, maxCount, debugTrace);
}

std::vector<uint32> QuestGuideMgr::GetQuestGiverQuestIdsFromHubs(Player* bot, const std::vector<const QuestGuideHub*>& candidateHubs, uint32 maxCount, std::string* debugTrace)
{
    Load();

    struct ScoredQuest
    {
        int32 score = 0;
        uint32 questId = 0;
        uint32 hubId = 0;
        std::string hubName;
        bool crossRace = false;
    };

    std::vector<ScoredQuest> scoredQuests;
    std::set<uint32> seenQuests;
    uint32 matchedHubs = 0;
    uint32 crossRaceHubs = 0;
    uint32 pickupCandidates = 0;
    uint32 validCandidates = 0;
    uint32 topHubId = 0;
    std::string topHubName;
    int32 topHubScore = std::numeric_limits<int32>::min();

    for (const QuestGuideHub* hubPtr : candidateHubs)
    {
        if (!hubPtr)
            continue;

        const QuestGuideHub& hub = *hubPtr;
        if (!MatchesBot(hub, bot))
            continue;

        ++matchedHubs;
        bool crossRace = IsCrossRaceCandidate(hub, bot);
        if (crossRace)
            ++crossRaceHubs;

        int32 hubScore = ScoreHub(hub, bot);
        if (hubScore > topHubScore)
        {
            topHubScore = hubScore;
            topHubId = hub.id;
            topHubName = hub.name;
        }

        for (const QuestGuideQuest& guideQuest : hub.quests)
        {
            if (!IsPickupRole(guideQuest.role))
                continue;

            ++pickupCandidates;

            if (!seenQuests.insert(guideQuest.questId).second)
                continue;

            Quest const* quest = sObjectMgr.GetQuestTemplate(guideQuest.questId);
            if (!quest)
                continue;

            if (bot->GetQuestRewardStatus(guideQuest.questId))
                continue;

            QuestStatus status = bot->GetQuestStatus(guideQuest.questId);
            if (status != QUEST_STATUS_NONE && status != QUEST_STATUS_AVAILABLE)
                continue;

            if (!bot->CanTakeQuest(quest, false))
                continue;

            ++validCandidates;
            scoredQuests.push_back({ hubScore + (int32)guideQuest.priority, guideQuest.questId, hub.id, hub.name, crossRace });
        }
    }

    std::sort(scoredQuests.begin(), scoredQuests.end(), [](const ScoredQuest& left, const ScoredQuest& right)
    {
        if (left.score != right.score)
            return left.score > right.score;

        return left.questId < right.questId;
    });

    std::vector<uint32> questIds;
    for (const auto& scoredQuest : scoredQuests)
    {
        questIds.push_back(scoredQuest.questId);
        if (questIds.size() >= maxCount)
            break;
    }

    if (debugTrace)
    {
        std::ostringstream out;
        out << "guideLoaded=" << (hubs.empty() ? 0 : 1)
            << " hubs=" << hubs.size()
            << " candidateHubs=" << candidateHubs.size()
            << " matchedHubs=" << matchedHubs
            << " crossRaceHubs=" << crossRaceHubs
            << " pickupCandidates=" << pickupCandidates
            << " validCandidates=" << validCandidates
            << " selected=" << questIds.size()
            << " nativeMask=" << GetNativeRouteMask(bot)
            << " preferredMask=" << GetPreferredRouteMask(bot);

        if (topHubId)
            out << " topHub=" << topHubId << ":" << topHubName << " topHubScore=" << topHubScore;

        if (!scoredQuests.empty())
        {
            out << " firstQuest=" << scoredQuests.front().questId
                << " firstHub=" << scoredQuests.front().hubId << ":" << scoredQuests.front().hubName
                << " firstScore=" << scoredQuests.front().score
                << " firstCrossRace=" << (scoredQuests.front().crossRace ? 1 : 0);
        }

        *debugTrace = out.str();
    }

    return questIds;
}

QuestGuideHubIntent QuestGuideMgr::SelectNextHub(Player* bot, std::string* debugTrace)
{
    Load();

    QuestGuideHubIntent best;
    uint32 matchedHubs = 0;
    uint32 viableHubs = 0;
    uint32 routeHubs = 0;
    uint32 crossRaceHubs = 0;
    uint32 rejectedCompleteHubs = 0;
    uint32 rejectedEmptyHubs = 0;

    const uint32 nativeRouteMask = GetNativeRouteMask(bot);
    const uint32 preferredRouteMask = GetPreferredRouteMask(bot);

    for (const QuestGuideHub& hub : hubs)
    {
        if (!MatchesBot(hub, bot))
            continue;

        ++matchedHubs;

        QuestGuideHubIntent intent;
        intent.valid = true;
        intent.hubId = hub.id;
        intent.hubName = hub.name;
        intent.mapId = hub.mapId;
        intent.areaId = hub.areaId;
        intent.x = hub.x;
        intent.y = hub.y;
        intent.z = hub.z;
        intent.nativeRouteMask = nativeRouteMask;
        intent.preferredRouteMask = preferredRouteMask;
        intent.crossRace = IsCrossRaceCandidate(hub, bot);
        intent.nextHubIds = hub.nextHubIds;
        intent.score = ScoreHub(hub, bot);

        if (hub.raceMask && (hub.raceMask & preferredRouteMask))
        {
            intent.score += 125;
            ++routeHubs;
        }

        if (hub.raceMask && (hub.raceMask & nativeRouteMask))
            intent.score += 75;

        if (intent.crossRace)
            ++crossRaceHubs;

        for (const QuestGuideQuest& guideQuest : hub.quests)
        {
            Quest const* quest = sObjectMgr.GetQuestTemplate(guideQuest.questId);
            if (!quest)
                continue;

            if (guideQuest.required)
                ++intent.requiredQuests;

            if (bot->GetQuestRewardStatus(guideQuest.questId))
            {
                ++intent.rewardedQuests;
                if (guideQuest.required)
                    ++intent.rewardedRequiredQuests;
                continue;
            }

            QuestStatus status = bot->GetQuestStatus(guideQuest.questId);
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_FAILED)
            {
                ++intent.activeQuests;
                intent.score += guideQuest.required ? 95 : 65;
                continue;
            }

            if (!IsPickupRole(guideQuest.role))
                continue;

            if (status != QUEST_STATUS_NONE && status != QUEST_STATUS_AVAILABLE)
                continue;

            if (!bot->CanTakeQuest(quest, false))
                continue;

            ++intent.pickupCandidates;
            intent.score += 30 + std::min<uint32>(guideQuest.priority / 100, 12);
        }

        if (intent.requiredQuests && intent.rewardedRequiredQuests >= intent.requiredQuests)
        {
            intent.score -= 250;
            ++rejectedCompleteHubs;
        }

        if (!intent.pickupCandidates && !intent.activeQuests)
        {
            intent.score -= 400;
            ++rejectedEmptyHubs;
        }

        if (intent.pickupCandidates)
            intent.reason = "pickup";
        else if (intent.activeQuests)
            intent.reason = "active";
        else if (intent.rewardedQuests)
            intent.reason = "advance";
        else
            intent.reason = "route";

        ++viableHubs;
        if (!best.valid || intent.score > best.score ||
            (intent.score == best.score && intent.hubId < best.hubId))
        {
            best = intent;
        }
    }

    if (debugTrace)
    {
        std::ostringstream out;
        out << "guideLoaded=" << (hubs.empty() ? 0 : 1)
            << " hubs=" << hubs.size()
            << " matchedHubs=" << matchedHubs
            << " viableHubs=" << viableHubs
            << " routeHubs=" << routeHubs
            << " crossRaceHubs=" << crossRaceHubs
            << " rejectedCompleteHubs=" << rejectedCompleteHubs
            << " rejectedEmptyHubs=" << rejectedEmptyHubs
            << " nativeMask=" << nativeRouteMask
            << " preferredMask=" << preferredRouteMask;

        if (best.valid)
        {
            out << " selectedHub=" << best.hubId << ":" << best.hubName
                << " score=" << best.score
                << " reason=" << best.reason
                << " pickups=" << best.pickupCandidates
                << " active=" << best.activeQuests
                << " rewarded=" << best.rewardedQuests
                << " required=" << best.requiredQuests
                << " rewardedRequired=" << best.rewardedRequiredQuests
                << " crossRace=" << (best.crossRace ? 1 : 0)
                << " area=" << best.areaId
                << " next=" << best.nextHubIds;
        }

        *debugTrace = out.str();
    }

    return best;
}

bool QuestGuideMgr::MatchesBot(const QuestGuideHub& hub, Player* bot) const
{
    if (!bot)
        return false;

    uint32 teamKey = GetTeamKey(bot);
    if (hub.faction && teamKey && hub.faction != teamKey)
        return false;

    if (hub.raceMask && !(hub.raceMask & GetRaceMask(bot)) && !IsCrossRaceCandidate(hub, bot))
        return false;

    if (hub.classMask && !(hub.classMask & GetClassMask(bot)))
        return false;

    uint32 level = bot->GetLevel();
    if (hub.minLevel && level + 2 < hub.minLevel)
        return false;

    if (hub.maxLevel && level > hub.maxLevel + 4)
        return false;

    return true;
}

bool QuestGuideMgr::IsCrossRaceCandidate(const QuestGuideHub& hub, Player* bot) const
{
    if (!bot || !hub.raceMask || (hub.raceMask & GetRaceMask(bot)))
        return false;

    // Cross-race guide use is only for same-faction flavor, not faction leakage.
    if (!hub.faction || hub.faction != GetTeamKey(bot))
        return false;

    return hub.raceMask == GetPreferredRouteMask(bot);
}

uint32 QuestGuideMgr::GetNativeRouteMask(Player* bot) const
{
    if (!bot)
        return 0;

    switch (bot->getRace())
    {
        case RACE_HUMAN:
            return 1;
        case RACE_DWARF:
        case RACE_GNOME:
            return 68;
        case RACE_NIGHTELF:
            return 8;
        case RACE_HIGH_ELF:
            return 512;
        case RACE_ORC:
        case RACE_TROLL:
            return 130;
        case RACE_GOBLIN:
            return 256;
        case RACE_TAUREN:
            return 32;
        case RACE_UNDEAD:
            return 16;
        default:
            return GetRaceMask(bot);
    }
}

uint32 QuestGuideMgr::GetPreferredRouteMask(Player* bot) const
{
    uint32 nativeRouteMask = GetNativeRouteMask(bot);
    if (!bot || StableRouteRoll(bot) >= CROSS_RACE_ROUTE_CHANCE)
        return nativeRouteMask;

    if (bot->GetTeam() == ALLIANCE)
    {
        static const uint32 allianceRoutes[] = { 1, 68, 8, 512 };
        uint32 candidates[3] = { 0, 0, 0 };
        uint32 count = 0;

        for (uint32 route : allianceRoutes)
        {
            if (route != nativeRouteMask)
                candidates[count++] = route;
        }

        return candidates[StableRouteIndex(bot, count)];
    }

    if (bot->GetTeam() == HORDE)
    {
        static const uint32 hordeRoutes[] = { 130, 32, 16, 256 };
        uint32 candidates[3] = { 0, 0, 0 };
        uint32 count = 0;

        for (uint32 route : hordeRoutes)
        {
            if (route != nativeRouteMask)
                candidates[count++] = route;
        }

        return candidates[StableRouteIndex(bot, count)];
    }

    return nativeRouteMask;
}

bool QuestGuideMgr::IsPickupRole(const std::string& role) const
{
    return role.empty() ||
        role == "pickup" ||
        role == "starter" ||
        role == "breadcrumb" ||
        role == "exit";
}

int32 QuestGuideMgr::ScoreHub(const QuestGuideHub& hub, Player* bot) const
{
    int32 score = (int32)hub.priority;

    if (!bot)
        return score;

    uint32 level = bot->GetLevel();
    if (hub.minLevel <= level && (!hub.maxLevel || level <= hub.maxLevel))
        score += 100;

    if (hub.areaId && (bot->GetAreaId() == hub.areaId || bot->GetZoneId() == hub.areaId))
        score += 75;

    if (hub.mapId == bot->GetMapId())
        score += 25;

    if (hub.raceMask && !(hub.raceMask & GetRaceMask(bot)))
        score -= CROSS_RACE_HUB_SCORE_PENALTY;

    return score;
}
