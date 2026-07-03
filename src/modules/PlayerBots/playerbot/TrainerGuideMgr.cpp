#include "playerbot/TrainerGuideMgr.h"

#include "playerbot/playerbot.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_set>
#include <unordered_map>

using namespace ai;

INSTANTIATE_SINGLETON_1(ai::TrainerGuideMgr);

namespace
{
    bool GetTrainerSpellRequirement(TrainerType trainerType, CreatureInfo const* trainerInfo, TrainerSpell const& trainerSpell, uint32& requirement)
    {
        if (!trainerInfo)
            return false;

        switch (trainerType)
        {
        case TRAINER_TYPE_CLASS:
        case TRAINER_TYPE_PETS:
            requirement = trainerInfo->TrainerClass;
            return requirement != 0;
        case TRAINER_TYPE_MOUNTS:
            requirement = trainerInfo->TrainerRace;
            return requirement != 0;
        case TRAINER_TYPE_TRADESKILLS:
            if (trainerSpell.reqSkill)
            {
                requirement = trainerSpell.reqSkill;
                return true;
            }
            else
            {
#ifdef MANGOSBOT_ZERO
                SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(trainerSpell.learnedSpell);
#else
                SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(trainerSpell.learnedSpell[0]);
#endif

                if (!spell)
                    return false;

                requirement = spell->EffectMiscValue[1];
                return requirement != 0;
            }
        default:
            return false;
        }
    }
}

void TrainerGuideMgr::Load()
{
    std::lock_guard<std::mutex> guard(loadMutex);

    if (loaded)
        return;

    loaded = true;
    entries.clear();

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS `ai_playerbot_trainer_guide` ("
        "`trainer_type` TINYINT UNSIGNED NOT NULL,"
        "`requirement` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`trainer_entry` MEDIUMINT UNSIGNED NOT NULL,"
        "`priority` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        "`note` VARCHAR(100) NOT NULL DEFAULT '',"
        "PRIMARY KEY (`trainer_type`, `requirement`, `trainer_entry`),"
        "KEY `idx_type_req_priority` (`trainer_type`, `requirement`, `enabled`, `priority`)"
        ")");

    // Seed a routing index from canonical trainer templates. The table remains
    // editable, so bad/slow trainer routes can be disabled or re-prioritized.
    WorldDatabase.PExecute(
        "INSERT IGNORE INTO `ai_playerbot_trainer_guide` "
        "(`trainer_type`, `requirement`, `trainer_entry`, `priority`, `enabled`, `note`) "
        "SELECT `trainer_type`, "
        "CASE "
        "WHEN `trainer_type` IN (0, 3) THEN `trainer_class` "
        "WHEN `trainer_type` = 1 THEN `trainer_race` "
        "ELSE 0 END AS `requirement`, "
        "`entry`, 0, 1, LEFT(`name`, 100) "
        "FROM `creature_template` "
        "WHERE `trainer_type` IN (0, 1, 2, 3) "
        "AND (`trainer_type` = 2 OR `trainer_class` <> 0 OR `trainer_race` <> 0)");

    SeedDiscoveredTrainerRequirements();

    if (auto result = WorldDatabase.PQuery(
            "SELECT `trainer_type`, `requirement`, `trainer_entry`, `priority`, `enabled` "
            "FROM `ai_playerbot_trainer_guide` "
            "WHERE `enabled` <> 0 "
            "ORDER BY `trainer_type` ASC, `requirement` ASC, `priority` DESC, `trainer_entry` ASC"))
    {
        do
        {
            Field* fields = result->Fetch();

            TrainerGuideEntry entry;
            entry.trainerType = static_cast<TrainerType>(fields[0].GetUInt32());
            entry.requirement = fields[1].GetUInt32();
            entry.trainerEntry = fields[2].GetInt32();
            entry.priority = fields[3].GetUInt32();
            entry.enabled = fields[4].GetUInt32() != 0;

            entries.push_back(entry);
        } while (result->NextRow());
    }

    sLog.outString(">> Loaded %u AI playerbot trainer guide entries.", (uint32)entries.size());
}

void TrainerGuideMgr::SeedDiscoveredTrainerRequirements()
{
    std::unordered_map<uint32, std::vector<CreatureInfo const*>> trainerTemplateIds;

    for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
    {
        CreatureInfo const* creatureInfo = sCreatureStorage.LookupEntry<CreatureInfo>(id);
        if (!creatureInfo)
            continue;

        if (!creatureInfo->TrainerType && !creatureInfo->TrainerClass)
            continue;

        if (creatureInfo->TrainerTemplateId)
            trainerTemplateIds[creatureInfo->TrainerTemplateId].push_back(creatureInfo);
        else
            trainerTemplateIds[id].push_back(creatureInfo);
    }

    std::set<std::tuple<uint32, uint32, int32>> discovered;
    for (const auto& [templateOrEntryId, trainers] : trainerTemplateIds)
    {
        TrainerSpellData const* trainerSpells = sObjectMgr.GetNpcTrainerTemplateSpells(templateOrEntryId);
        if (!trainerSpells)
            trainerSpells = sObjectMgr.GetNpcTrainerSpells(templateOrEntryId);

        if (!trainerSpells || trainers.empty())
            continue;

        CreatureInfo const* firstTrainer = trainers.front();
        TrainerType trainerType = static_cast<TrainerType>(firstTrainer->TrainerType);
        if (trainerType != TRAINER_TYPE_TRADESKILLS)
            continue;

        for (const auto& [id, trainerSpell] : trainerSpells->spellList)
        {
            uint32 requirement = 0;
            if (!GetTrainerSpellRequirement(trainerType, firstTrainer, trainerSpell, requirement))
                continue;

            for (CreatureInfo const* trainer : trainers)
                if (trainer)
                    discovered.insert({static_cast<uint32>(trainerType), requirement, trainer->Entry});
        }
    }

    for (const auto& [trainerType, requirement, trainerEntry] : discovered)
    {
        WorldDatabase.PExecute(
            "INSERT IGNORE INTO `ai_playerbot_trainer_guide` "
            "(`trainer_type`, `requirement`, `trainer_entry`, `priority`, `enabled`, `note`) "
            "VALUES (%u, %u, %u, 10, 1, 'auto skill route')",
            trainerType, requirement, static_cast<uint32>(trainerEntry));
    }
}

void TrainerGuideMgr::Reload()
{
    std::lock_guard<std::mutex> guard(loadMutex);
    loaded = false;
    entries.clear();
}

bool TrainerGuideMgr::HasGuideData()
{
    Load();
    return !entries.empty();
}

uint32 TrainerGuideMgr::GetRequirement(Player* bot, TrainerType trainerType) const
{
    if (!bot)
        return 0;

    switch (trainerType)
    {
    case TRAINER_TYPE_CLASS:
    case TRAINER_TYPE_PETS:
        return bot->getClass();
    case TRAINER_TYPE_MOUNTS:
        return bot->getRace();
    case TRAINER_TYPE_TRADESKILLS:
    default:
        return 0;
    }
}

std::vector<int32> TrainerGuideMgr::FilterTrainerEntries(Player* bot, TrainerType trainerType, const std::vector<int32>& eligibleEntries, uint32 maxCount)
{
    std::vector<uint32> requirementHints;
    return FilterTrainerEntries(bot, trainerType, eligibleEntries, requirementHints, maxCount);
}

std::vector<int32> TrainerGuideMgr::FilterTrainerEntries(Player* bot, TrainerType trainerType, const std::vector<int32>& eligibleEntries, const std::vector<uint32>& requirementHints, uint32 maxCount)
{
    Load();

    if (eligibleEntries.empty() || entries.empty())
        return eligibleEntries;

    std::unordered_set<int32> eligible(eligibleEntries.begin(), eligibleEntries.end());
    std::unordered_set<uint32> requirements(requirementHints.begin(), requirementHints.end());
    if (requirements.empty())
    {
        uint32 requirement = GetRequirement(bot, trainerType);
        if (requirement)
            requirements.insert(requirement);
    }

    auto rankEntries = [&](bool allowWildcard)
    {
        std::set<int32> seen;
        std::vector<std::pair<uint32, int32>> ranked;

        for (const TrainerGuideEntry& entry : entries)
        {
            if (!entry.enabled || entry.trainerType != trainerType)
                continue;

            bool exactRequirement = requirements.find(entry.requirement) != requirements.end();
            if (!exactRequirement && (!allowWildcard || entry.requirement != 0))
                continue;

            if (eligible.find(entry.trainerEntry) == eligible.end())
                continue;

            if (!seen.insert(entry.trainerEntry).second)
                continue;

            ranked.push_back({entry.priority, entry.trainerEntry});
        }

        std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right)
        {
            if (left.first != right.first)
                return left.first > right.first;

            return left.second < right.second;
        });

        std::vector<int32> filtered;
        filtered.reserve(std::min<uint32>(maxCount, ranked.size()));
        for (const auto& entry : ranked)
        {
            filtered.push_back(entry.second);
            if (filtered.size() >= maxCount)
                break;
        }

        return filtered;
    };

    std::vector<int32> filtered = rankEntries(false);
    if (filtered.empty())
    {
        bool allowWildcard = trainerType != TRAINER_TYPE_TRADESKILLS || !requirements.empty();
        filtered = rankEntries(allowWildcard);
    }

    return filtered.empty() ? eligibleEntries : filtered;
}
