#include "playerbot/playerbot.h"
#include "playerbot/EncounterKnowledgeMgr.h"

#include "Database/DatabaseEnv.h"

ai::EncounterKnowledgeMgr sEncounterKnowledgeMgr;

namespace ai
{

void EncounterKnowledgeMgr::Load()
{
    if (loaded)
        return;
    loaded = true;

    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS ai_playerbot_encounter ("
        "id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY, map_id INT UNSIGNED NOT NULL,"
        "boss_entry INT UNSIGNED NOT NULL, boss_name VARCHAR(64) NOT NULL,"
        "instance_name VARCHAR(64) NOT NULL, is_raid TINYINT NOT NULL DEFAULT 0,"
        "review_status VARCHAR(16) NOT NULL DEFAULT 'draft', UNIQUE KEY uk_boss (boss_entry))");
    WorldDatabase.PExecute(
        "CREATE TABLE IF NOT EXISTS ai_playerbot_encounter_mechanic ("
        "id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY, boss_entry INT UNSIGNED NOT NULL,"
        "spell_id INT UNSIGNED NOT NULL DEFAULT 0, mechanic_type VARCHAR(32) NOT NULL,"
        "target_type VARCHAR(16) NOT NULL DEFAULT 'victim', radius FLOAT NOT NULL DEFAULT 0,"
        "phase_below_hp TINYINT UNSIGNED NOT NULL DEFAULT 100,"
        "role_tank VARCHAR(64) NOT NULL DEFAULT '', role_healer VARCHAR(64) NOT NULL DEFAULT '',"
        "role_dps VARCHAR(64) NOT NULL DEFAULT '', KEY idx_boss (boss_entry))");

    uint32 count = 0;
    if (auto result = WorldDatabase.PQuery(
            "SELECT boss_entry, spell_id, mechanic_type, target_type, radius, phase_below_hp,"
            " role_tank, role_healer, role_dps FROM ai_playerbot_encounter_mechanic"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bossEntry = fields[0].GetUInt32();
            EncounterMechanic mech;
            mech.spellId = fields[1].GetUInt32();
            mech.mechanicType = fields[2].GetCppString();
            mech.targetType = fields[3].GetCppString();
            mech.radius = fields[4].GetFloat();
            mech.phaseBelowHp = fields[5].GetUInt8();
            mech.roleTank = fields[6].GetCppString();
            mech.roleHealer = fields[7].GetCppString();
            mech.roleDps = fields[8].GetCppString();
            if (mech.mechanicType == "hazard_object" || mech.mechanicType == "hazard_creature")
                mech.hazardEntry = mech.spellId;   // entry stored in spell_id column for hazard rows
            mechanicsByBoss[bossEntry].push_back(mech);
            ++count;
        } while (result->NextRow());
    }
    sLog.outString("EncounterKnowledgeMgr: loaded %u mechanics for %u bosses",
        count, (uint32)mechanicsByBoss.size());
}

bool EncounterKnowledgeMgr::IsKnownBoss(uint32 creatureEntry) const
{
    return mechanicsByBoss.count(creatureEntry) != 0;
}

std::vector<EncounterMechanic> const* EncounterKnowledgeMgr::GetMechanics(uint32 creatureEntry) const
{
    auto it = mechanicsByBoss.find(creatureEntry);
    return it != mechanicsByBoss.end() ? &it->second : nullptr;
}

}
