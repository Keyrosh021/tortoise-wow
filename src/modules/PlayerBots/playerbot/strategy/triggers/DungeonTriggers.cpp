
#include "playerbot/playerbot.h"
#include "DungeonTriggers.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/AiObjectContext.h"
#include "playerbot/strategy/values/HazardsValue.h"
#include "playerbot/strategy/actions/MovementActions.h"
#include "playerbot/EncounterKnowledgeMgr.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"

using namespace ai;

bool EnterDungeonTrigger::IsActive()
{
    // Don't trigger if strategy already set
    if (!ai->HasStrategy(dungeonStrategy, BotState::BOT_STATE_COMBAT))
    {
        // If the bot is ready
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            // If the bot is on the specified dungeon
            Map* map = bot->GetMap();
            if (map && (map->IsDungeon() || map->IsRaid()))
            {
                return map->GetId() == mapID;
            }
        }
    }

    return false;
}

bool LeaveDungeonTrigger::IsActive()
{
    // Don't trigger if strategy already unset
    if (ai->HasStrategy(dungeonStrategy, BotState::BOT_STATE_COMBAT))
    {
        // If the bot is ready
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            // If the bot is not on the specified dungeon
            return bot->GetMapId() != mapID;
        }
    }

    return false;
}

bool StartBossFightTrigger::IsActive()
{
    // Don't trigger if strategy already set
    if (!ai->HasStrategy(bossStrategy, BotState::BOT_STATE_COMBAT))
    {
        // If the bot is ready
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            AiObjectContext* context = ai->GetAiObjectContext();
            const std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
            for (const ObjectGuid& attackerGuid : attackers)
            {
                Unit* attacker = ai->GetUnit(attackerGuid);
                if (attacker && attacker->GetEntry() == bossID)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool EndBossFightTrigger::IsActive()
{
    // Don't trigger if strategy already unset
    if (ai->HasStrategy(bossStrategy, BotState::BOT_STATE_COMBAT))
    {
        // We consider the fight is over if not in combat
        return !ai->IsStateActive(BotState::BOT_STATE_COMBAT);
    }

    return false;
}

bool CloseToHazardTrigger::IsActive()
{
    // If the bot is ready
    bool closeToHazard = false;
    if (bot->IsInWorld() && !bot->IsBeingTeleported())
    {
        const std::list<ObjectGuid>& possibleHazards = GetPossibleHazards();
        for (const ObjectGuid& possibleHazardGuid : possibleHazards)
        {
            if (IsHazardValid(possibleHazardGuid))
            {
                // Check if the bot is inside the hazard
                const float distanceToHazard = GetDistanceToHazard(possibleHazardGuid);
                if (distanceToHazard <= hazardRadius)
                {
                    closeToHazard = true;
                }
            }

            // Cache the hazards
            Hazard hazard(possibleHazardGuid, hazardDuration, hazardRadius);
            SET_AI_VALUE(Hazard, "add hazard", std::move(hazard));
        }
    }

    // Don't trigger if the bot is moving
    if (closeToHazard)
    {
        const Action* lastExecutedAction = ai->GetLastExecutedAction(BotState::BOT_STATE_COMBAT);
        if (lastExecutedAction)
        {
            const MovementAction* movementAction = dynamic_cast<const MovementAction*>(lastExecutedAction);
            if (movementAction)
            {
                closeToHazard = false;
            }
        }
    }

    return closeToHazard;
}

bool CloseToHazardTrigger::IsHazardValid(const ObjectGuid& hazzardGuid)
{
    if (hazzardGuid.IsGameObject())
    {
        return ai->GetGameObject(hazzardGuid) != nullptr;
    }
    else if (hazzardGuid.IsCreature())
    {
        return ai->GetCreature(hazzardGuid) != nullptr;
    }

    return false;
}

float CloseToHazardTrigger::GetDistanceToHazard(const ObjectGuid& hazzardGuid)
{
    if (hazzardGuid.IsGameObject())
    {
        GameObject* gameObjectHazard = ai->GetGameObject(hazzardGuid);
        if (gameObjectHazard)
        {
            return bot->GetDistance(gameObjectHazard) + gameObjectHazard->GetObjectBoundingRadius();
        }
    }
    else if (hazzardGuid.IsCreature())
    {
        Creature* creatureHazard = ai->GetCreature(hazzardGuid);
        if (creatureHazard)
        {
            return bot->GetDistance(creatureHazard, true, DIST_CALC_COMBAT_REACH);
        }
    }

    return 99999999.0f;
}

std::list<ObjectGuid> CloseToGameObjectHazardTrigger::GetPossibleHazards()
{
    // This game objects have a maximum range equal to the sight distance on config file (default 60 yards)
    return AI_VALUE2(std::list<ObjectGuid>, "nearest game objects no los", gameObjectID);
}

std::list<ObjectGuid> CloseToCreatureHazardTrigger::GetPossibleHazards()
{
    std::list<ObjectGuid> possibleHazards;

    std::list<Unit*> creatures;
    MaNGOS::AllCreaturesOfEntryInRangeCheck u_check(bot, creatureID, hazardRadius);
    MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(creatures, u_check);
    Cell::VisitAllObjects(bot, searcher, hazardRadius);
    for (Unit* unit : creatures)
    {
        possibleHazards.push_back(unit->GetObjectGuid());
    }

    return possibleHazards;
}

bool CloseToCreatureHazardTrigger::IsHazardValid(const ObjectGuid& hazzardGuid)
{
    Creature* creatureHazard = ai->GetCreature(hazzardGuid);
    if (creatureHazard)
    {
        // Check if the creature is not targeting the bot
        if (!creatureHazard->GetVictim() || (creatureHazard->GetVictim()->GetObjectGuid() != bot->GetObjectGuid()))
        {
            return true;
        }
    }

    return false;
}

std::list<ObjectGuid> CloseToHostileCreatureHazardTrigger::GetPossibleHazards()
{
    std::list<ObjectGuid> possibleHazards;
    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");

    for (const ObjectGuid& attackerGuid : attackers)
    {
        Creature* attacker = ai->GetCreature(attackerGuid);
        if (attacker && attacker->GetEntry() == creatureID)
        {
            possibleHazards.push_back(attackerGuid);
        }
    }

    return possibleHazards;
}

bool CloseToCreatureTrigger::IsActive()
{
    // If the bot is ready
    if (bot->IsInWorld() && !bot->IsBeingTeleported())
    {
        AiObjectContext* context = ai->GetAiObjectContext();

        // Iterate through the near creatures
        std::list<Unit*> creatures;
        MaNGOS::AllCreaturesOfEntryInRangeCheck u_check(bot, creatureID, range);
        MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(creatures, u_check);
        Cell::VisitAllObjects(bot, searcher, range);
        for (Unit* unit : creatures)
        {
            Creature* creature = (Creature*)unit;
            if (creature)
            {
                // Check if the bot is not being targeted by the creature
                if (!creature->GetVictim() || (creature->GetVictim()->GetObjectGuid() != bot->GetObjectGuid()))
                {
                    // See if the creature is within the specified distance
                    if (bot->IsWithinDist(creature, range))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool ItemReadyTrigger::IsActive()
{
    // Check if the bot has the item or if it has cheats enabled
    if (bot->HasItemCount(itemID, 1) || ai->HasCheat(BotCheatMask::item))
    {
        const ItemPrototype* proto = sObjectMgr.GetItemPrototype(itemID);
        if (proto)
        {
            // Check if the item is in cooldown
            bool inCooldown = false;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                if (proto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                {
                    continue;
                }

                const uint32 spellID = proto->Spells[i].SpellId;
                if (spellID > 0)
                {
                    if (!sServerFacade.IsSpellReady(bot, spellID) ||
                        !sServerFacade.IsSpellReady(bot, spellID, itemID))
                    {
                        inCooldown = true;
                        break;
                    }
                }
            }

            return !inCooldown;
        }
    }

    return false;
}

bool ItemBuffReadyTrigger::IsActive()
{
    if (!bot->HasAura(buffID))
    {
        return ItemReadyTrigger::IsActive();
    }

    return false;
}
bool DbEncounterHazardTrigger::IsActive()
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported())
        return false;
    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;

    // find an engaged known boss among attackers
    std::vector<EncounterMechanic> const* mechanics = nullptr;
    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& guid : attackers)
    {
        if (Unit* unit = ai->GetUnit(guid))
        {
            mechanics = sEncounterKnowledgeMgr.GetMechanics(unit->GetEntry());
            if (mechanics)
                break;
        }
    }
    if (!mechanics)
        return false;

    bool closeToHazard = false;
    for (EncounterMechanic const& mech : *mechanics)
    {
        if (!mech.hazardEntry || mech.radius <= 0.0f)
            continue;

        std::list<ObjectGuid> found;
        if (mech.mechanicType == "hazard_object")
        {
            found = AI_VALUE2(std::list<ObjectGuid>, "nearest game objects no los", mech.hazardEntry);
        }
        else // hazard_creature
        {
            std::list<Unit*> creatures;
            MaNGOS::AllCreaturesOfEntryInRangeCheck u_check(bot, mech.hazardEntry, mech.radius);
            MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(creatures, u_check);
            Cell::VisitAllObjects(bot, searcher, mech.radius);
            for (Unit* unit : creatures)
                found.push_back(unit->GetObjectGuid());
        }

        for (const ObjectGuid& hazardGuid : found)
        {
            WorldObject* wo = hazardGuid.IsGameObject()
                ? (WorldObject*)ai->GetGameObject(hazardGuid)
                : (WorldObject*)ai->GetUnit(hazardGuid);
            if (!wo)
                continue;
            if (bot->GetDistance(wo) <= mech.radius)
                closeToHazard = true;

            Hazard hazard(hazardGuid, 60, mech.radius);
            SET_AI_VALUE(Hazard, "add hazard", std::move(hazard));
        }
    }

    // don't re-trigger while already executing an escape move (same rule as the base kit)
    if (closeToHazard)
        if (const Action* last = ai->GetLastExecutedAction(BotState::BOT_STATE_COMBAT))
            if (dynamic_cast<const MovementAction*>(last))
                closeToHazard = false;

    return closeToHazard;
}

bool DbRunOutDebuffTrigger::IsActive()
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return false;
    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;

    std::vector<EncounterMechanic> const* mechanics = nullptr;
    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& guid : attackers)
        if (Unit* unit = ai->GetUnit(guid))
            if ((mechanics = sEncounterKnowledgeMgr.GetMechanics(unit->GetEntry())))
                break;
    if (!mechanics)
        return false;

    for (EncounterMechanic const& mech : *mechanics)
    {
        if (mech.mechanicType != "run_out_debuff" || !mech.spellId)
            continue;
        if (bot->HasAura(mech.spellId))
        {
            // stash the required spread distance for the action
            SET_AI_VALUE(float, "run out distance", std::max(10.0f, mech.radius));
            return true;
        }
    }
    return false;
}

bool DbAvoidFrontalTrigger::IsActive()
{
    if (!bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat())
        return false;
    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;
    if (ai->IsTank(bot))
        return false;                                    // the tank belongs in front

    Unit* victim = bot->GetVictim();
    if (!victim)
        return false;
    std::vector<EncounterMechanic> const* mechanics = sEncounterKnowledgeMgr.GetMechanics(victim->GetEntry());
    if (!mechanics)
        return false;

    bool frontalDanger = false;
    for (EncounterMechanic const& mech : *mechanics)
        if (mech.mechanicType == "avoid_frontal" || mech.mechanicType == "tank_face_away")
            { frontalDanger = true; break; }
    if (!frontalDanger)
        return false;

    // in the boss's frontal 90-degree arc and close enough to be hit
    return victim->HasInArc(bot, M_PI_F / 2.0f) && bot->GetDistance(victim) < 15.0f;
}

bool DbTankFaceAwayTrigger::IsActive()
{
    if (!bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat() || !ai->IsTank(bot))
        return false;
    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;
    Unit* victim = bot->GetVictim();
    if (!victim || victim->GetVictim() != bot)
        return false;                                    // only when the boss is actually on us
    std::vector<EncounterMechanic> const* mechanics = sEncounterKnowledgeMgr.GetMechanics(victim->GetEntry());
    if (!mechanics)
        return false;
    bool faceAway = false;
    for (EncounterMechanic const& mech : *mechanics)
        if (mech.mechanicType == "tank_face_away" || mech.mechanicType == "avoid_frontal")
            { faceAway = true; break; }
    if (!faceAway)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;
        if (member->GetDistance(victim) < 20.0f && victim->HasInArc(member, M_PI_F / 2.0f))
            return true;                                 // someone is standing in the breath
    }
    return false;
}

bool DbTankSwapTrigger::IsActive()
{
    if (!bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat() || !ai->IsTank(bot))
        return false;
    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;

    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& guid : attackers)
    {
        Unit* boss = ai->GetUnit(guid);
        if (!boss || !boss->IsAlive())
            continue;
        Unit* mt = boss->GetVictim();
        if (!mt || mt == bot)
            continue;                                    // we already have it (or no tank)
        std::vector<EncounterMechanic> const* mechanics = sEncounterKnowledgeMgr.GetMechanics(boss->GetEntry());
        if (!mechanics)
            continue;
        for (EncounterMechanic const& mech : *mechanics)
        {
            if (mech.mechanicType != "tank_swap" || !mech.spellId)
                continue;
            uint32 threshold = std::max(1u, (uint32)mech.radius);
            SpellAuraHolder* holder = mt->GetSpellAuraHolder(mech.spellId);
            if (!holder || holder->GetStackAmount() < threshold)
                continue;
            // don't ping-pong: only taunt if OUR stacks are safely lower
            SpellAuraHolder* mine = bot->GetSpellAuraHolder(mech.spellId);
            if (mine && mine->GetStackAmount() + 1 >= threshold)
                continue;

            // 4H CORNER ASSIGNMENT (v2): with multiple swap-bosses up (Four Horsemen), taunts
            // rotate ORDERLY — each tank takes the next boss in the ring (both sorted by guid),
            // not whichever trips first. Single-boss fights (BWL drakes) skip this: any off-tank
            // with clean stacks may taunt.
            std::vector<ObjectGuid> swapBosses;
            for (const ObjectGuid& ag : attackers)
                if (Unit* au = ai->GetUnit(ag))
                    if (au->IsAlive())
                        if (std::vector<EncounterMechanic> const* am = sEncounterKnowledgeMgr.GetMechanics(au->GetEntry()))
                            for (EncounterMechanic const& amech : *am)
                                if (amech.mechanicType == "tank_swap") { swapBosses.push_back(ag); break; }
            if (swapBosses.size() > 1 && bot->GetGroup())
            {
                std::sort(swapBosses.begin(), swapBosses.end());
                std::vector<ObjectGuid> tanks;
                for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->getSource())
                        if (member->IsInWorld() && ai->IsTank(member))
                            tanks.push_back(member->GetObjectGuid());
                std::sort(tanks.begin(), tanks.end());
                if (!tanks.empty())
                {
                    uint32 myIdx = std::find(tanks.begin(), tanks.end(), bot->GetObjectGuid()) - tanks.begin();
                    uint32 bossIdx = std::find(swapBosses.begin(), swapBosses.end(), boss->GetObjectGuid()) - swapBosses.begin();
                    if (myIdx >= tanks.size())
                        continue;                        // not in the tank ring
                    // rotate: tank i takes boss (i + rotationStep) — step = stacks-driven ring shift
                    if ((bossIdx % tanks.size()) != ((myIdx + 1) % tanks.size()))
                        continue;                        // not my next corner
                }
            }

            SET_AI_VALUE(ObjectGuid, "attack target", boss->GetObjectGuid());
            return true;
        }
    }
    return false;
}

bool DbStagedTanksTrigger::IsActive()
{
    if (!bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat() || !ai->IsTank(bot))
        return false;
    if (!bot->GetMap() || !bot->GetMap()->IsRaid())
        return false;

    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& guid : attackers)
    {
        Unit* boss = ai->GetUnit(guid);
        if (!boss || !boss->IsAlive())
            continue;
        if (boss->GetVictim() == bot)
            return false;                                // we're the active tank: stay in
        std::vector<EncounterMechanic> const* mechanics = sEncounterKnowledgeMgr.GetMechanics(boss->GetEntry());
        if (!mechanics)
            continue;
        for (EncounterMechanic const& mech : *mechanics)
        {
            if (mech.mechanicType != "staged_tanks")
                continue;
            float stage = std::max(12.0f, mech.radius > 0 ? mech.radius * 0.6f : 18.0f);
            if (bot->GetDistance(boss) < stage - 2.0f)
            {
                SET_AI_VALUE(ObjectGuid, "attack target", boss->GetObjectGuid());
                SET_AI_VALUE(float, "run out distance", stage);
                return true;                             // too close while off-duty: back out
            }
        }
    }
    return false;
}

bool DbMarkSeparationTrigger::IsActive()
{
    if (!bot->IsInWorld() || !bot->IsAlive() || !bot->GetMap() || !bot->GetMap()->IsRaid())
        return false;

    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& guid : attackers)
    {
        Unit* boss = ai->GetUnit(guid);
        if (!boss || !boss->IsAlive() || boss->GetVictim() == bot)
            continue;                                    // our own tank duty handles our boss
        if (bot->GetDistance(boss) > 22.0f)
            continue;
        std::vector<EncounterMechanic> const* mechanics = sEncounterKnowledgeMgr.GetMechanics(boss->GetEntry());
        if (!mechanics)
            continue;
        for (EncounterMechanic const& mech : *mechanics)
        {
            if (mech.mechanicType != "tank_swap" || !mech.spellId)
                continue;
            // near boss B while carrying a DIFFERENT boss's mark?
            std::list<ObjectGuid> all = attackers;
            for (const ObjectGuid& other : all)
            {
                Unit* otherBoss = ai->GetUnit(other);
                if (!otherBoss || otherBoss == boss)
                    continue;
                std::vector<EncounterMechanic> const* om = sEncounterKnowledgeMgr.GetMechanics(otherBoss->GetEntry());
                if (!om)
                    continue;
                for (EncounterMechanic const& omech : *om)
                    if (omech.mechanicType == "tank_swap" && omech.spellId != mech.spellId &&
                        bot->HasAura(omech.spellId))
                    {
                        SET_AI_VALUE(ObjectGuid, "attack target", boss->GetObjectGuid());
                        SET_AI_VALUE(float, "run out distance", 25.0f);
                        return true;                     // back away from boss B
                    }
            }
        }
    }
    return false;
}
