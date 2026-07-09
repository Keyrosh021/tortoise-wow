#pragma once

#include "Common.h"

#include <map>
#include <string>
#include <vector>

// Encounter knowledge loaded from ai_playerbot_encounter / ai_playerbot_encounter_mechanic
// (seeded from src/scripts boss-script extraction, human-reviewed). Consumed by the generic
// DB-driven dungeon triggers so per-boss behavior no longer needs hardcoded strategy classes.
namespace ai
{
    struct EncounterMechanic
    {
        uint32 spellId = 0;
        std::string mechanicType;   // aoe_avoid|spread|dispel|interrupt|adds_first|tank_face_away|tank_damage_spike|expect_knockback|hazard_object|hazard_creature
        std::string targetType;     // self|victim|random|zone
        float radius = 0.0f;
        uint8 phaseBelowHp = 100;
        std::string roleTank;
        std::string roleHealer;
        std::string roleDps;
        uint32 hazardEntry = 0;     // for hazard_object/hazard_creature rows spellId doubles as the entry
    };

    class EncounterKnowledgeMgr
    {
    public:
        void Load();
        bool IsKnownBoss(uint32 creatureEntry) const;
        std::vector<EncounterMechanic> const* GetMechanics(uint32 creatureEntry) const;

    private:
        bool loaded = false;
        std::map<uint32, std::vector<EncounterMechanic>> mechanicsByBoss;
    };
}

extern ai::EncounterKnowledgeMgr sEncounterKnowledgeMgr;
