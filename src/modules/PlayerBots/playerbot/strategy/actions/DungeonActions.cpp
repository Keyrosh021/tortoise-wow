#include "DungeonActions.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/strategy/AiObjectContext.h"
#include "playerbot/PlayerbotAI.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"

using namespace ai;

bool MoveAwayFromHazard::Execute(Event& event)
{
    const std::list<HazardPosition>& hazards = AI_VALUE(std::list<HazardPosition>, "hazards");

    // Get the closest hazard to move away from
    const HazardPosition* closestHazard = nullptr;
    float closestHazardDistance = 9999.0f;
    for (const HazardPosition& hazard : hazards)
    {
        const WorldPosition& hazardPosition = hazard.first;
        const float distance = bot->GetDistance(hazardPosition.getX(), hazardPosition.getY(), hazardPosition.getZ());
        if (distance < closestHazardDistance)
        {
            closestHazardDistance = distance;
            closestHazard = &hazard;
        }
    }

    if (closestHazard)
    {
        // Check if the bot is inside the closest hazard
        const float hazardRadius = closestHazard->second;
        if (closestHazardDistance <= hazardRadius)
        {
            float angle = 0.0f;
            const WorldPosition initialPosition(closestHazard->first);
            const float distance = frand(hazardRadius, hazardRadius * 1.5f);

            Unit* currentTarget = AI_VALUE(Unit*, "current target");
            if (currentTarget)
            {
                const int8 startDir = urand(0, 1) * 2 - 1;
                const WorldPosition targetPosition(currentTarget);
                angle = targetPosition.getAngleTo(initialPosition) + (0.5 * M_PI_F * startDir);
            }
            else
            {
                angle = frand(0, M_PI_F * 2.0f);
            }

            const uint8 attempts = 10;
            float angleIncrement = (float)((2 * M_PI) / attempts);

            for (uint8 i = 0; i < attempts; i++)
            {
                WorldPosition point = initialPosition + WorldPosition(0, distance * cos(angle), distance * sin(angle), 1.0f);
                point.setZ(point.getHeight());

                // Check if the point is not near other hazards
                if (!IsHazardNearby(point, hazards))
                {
                    if (bot->IsWithinLOS(point.getX(), point.getY(), point.getZ() + bot->GetCollisionHeight()) && initialPosition.canPathTo(point, bot))
                    {
                        if (ai->HasStrategy("debug move", BotState::BOT_STATE_COMBAT))
                        {
                            bot->SummonCreature(15631, point.getX(), point.getY(), point.getZ(), 0.0f, TEMPSPAWN_TIMED_DESPAWN, 5000.0f);
                        }

                        if (MoveTo(bot->GetMapId(), point.getX(), point.getY(), point.getZ(), false, IsReaction(), false, true))
                        {
                            if (IsReaction())
                            {
                                WaitForReach(point.distance(initialPosition));
                            }

                            return true;
                        }
                    }
                }

                if (ai->HasStrategy("debug move", BotState::BOT_STATE_COMBAT))
                {
                    bot->SummonCreature(1, point.getX(), point.getY(), point.getZ(), 0.0f, TEMPSPAWN_TIMED_DESPAWN, 5000.0f);
                }

                angle += angleIncrement;
            }
        }
    }

    return false;
}

bool MoveAwayFromHazard::isPossible()
{
    if (MovementAction::isPossible())
    {
        return ai->CanMove();
    }

    return false;
}

bool MoveAwayFromHazard::IsHazardNearby(const WorldPosition& point, const std::list<HazardPosition>& hazards) const
{
    for (const HazardPosition& hazard : hazards)
    {
        const float hazardRange = hazard.second;
        const WorldPosition& hazardPosition = hazard.first;
        const float distance = point.distance(hazardPosition);
        if (distance < hazardRange)
        {
            return true;
        }
    }

    return false;
}

bool MoveAwayFromCreature::Execute(Event& event)
{
    // Get the active attacking creatures
    std::list<Creature*> creatures;
    size_t closestCreatureIdx = 0;
    float closestCreatureDistance = 9999.0f;

    // Iterate through the near creatures
    std::list<Unit*> units;
    MaNGOS::AllCreaturesOfEntryInRangeCheck u_check(bot, creatureID, range);
    MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(units, u_check);
    Cell::VisitAllObjects(bot, searcher, range);
    for (Unit* unit : units)
    {
        Creature* creature = (Creature*)unit;
        if (creature)
        {
            creatures.push_back(creature);

            // Get the closest creature to the bot
            const float distance = bot->GetDistance(creature);
            if (distance < closestCreatureDistance)
            {
                closestCreatureDistance = distance;
                closestCreatureIdx = creatures.size() - 1;
            }
        }
    }

    if (creatures.empty())
    {
        return false;
    }

    const std::list<HazardPosition>& hazards = AI_VALUE(std::list<HazardPosition>, "hazards");

    // Get the closest creature reference
    auto it = creatures.begin();
    advance(it, closestCreatureIdx);
    Creature* closestCreature = *it;
    // Remove the closest creature from the list to prevent checking it twice
    creatures.erase(it);

    // Generate the initial angle directly behind the bot looking at the closest creature
    const WorldPosition botPosition(bot);
    const WorldPosition creaturePosition(closestCreature);
    float angleLeft = creaturePosition.getAngleTo(botPosition);
    float angleRight = angleLeft;

    const uint8 attempts = 20;
    const uint8 halfAtempts = (uint8)(attempts * 0.5f);
    float angleIncrement = (float)((M_PI) / halfAtempts);

    const float sizeFactor = bot->GetCombatReach() + closestCreature->GetCombatReach();
    const float distance = (range + sizeFactor);

    for (uint8 i = 0; i < halfAtempts; i++)
    {
        WorldPosition* validPoint = nullptr;

        // Calculate a point to the left and right
        WorldPosition pointLeft = creaturePosition + WorldPosition(0, distance * cos(angleLeft), distance * sin(angleLeft), 1.0f);
        pointLeft.setZ(pointLeft.getHeight());
        WorldPosition pointRight = creaturePosition + WorldPosition(0, distance * cos(angleRight), distance * sin(angleRight), 1.0f);
        pointRight.setZ(pointRight.getHeight());

        if (IsValidPoint(pointLeft, creatures, hazards))
        {
            validPoint = &pointLeft;
        }
        else if (IsValidPoint(pointRight, creatures, hazards))
        {
            validPoint = &pointRight;
        }

        if (validPoint)
        {
            if (ai->HasStrategy("debug move", BotState::BOT_STATE_COMBAT))
            {
                bot->SummonCreature(15631, validPoint->getX(), validPoint->getY(), validPoint->getZ(), 0.0f, TEMPSPAWN_TIMED_DESPAWN, 5000.0f);
            }

            if (MoveTo(bot->GetMapId(), validPoint->getX(), validPoint->getY(), validPoint->getZ(), false, IsReaction(), false, true))
            {
                if (IsReaction())
                {
                    WaitForReach(validPoint->distance(botPosition));
                }

                return true;
            }
        }

        if (ai->HasStrategy("debug move", BotState::BOT_STATE_COMBAT))
        {
            bot->SummonCreature(1, pointLeft.getX(), pointLeft.getY(), pointLeft.getZ(), 0.0f, TEMPSPAWN_TIMED_DESPAWN, 5000.0f);
            bot->SummonCreature(1, pointRight.getX(), pointRight.getY(), pointRight.getZ(), 0.0f, TEMPSPAWN_TIMED_DESPAWN, 5000.0f);
        }

        angleLeft += angleIncrement;
        angleRight -= angleIncrement;
    }

    return false;
}

bool MoveAwayFromCreature::isPossible()
{
    if (MovementAction::isPossible())
    {
        return ai->CanMove();
    }

    return false;
}

bool MoveAwayFromCreature::IsValidPoint(const WorldPosition& point, const std::list<Creature*>& creatures, const std::list<HazardPosition>& hazards)
{
    // Check if the point is not near other game objects
    if (!HasCreaturesNearby(point, creatures) && !IsHazardNearby(point, hazards))
    {
        if (bot->IsWithinLOS(point.getX(), point.getY(), point.getZ() + bot->GetCollisionHeight()))
        {
            const WorldPosition botPosition(bot);
            return botPosition.canPathTo(point, bot);
        }
    }

    return false;
}

bool MoveAwayFromCreature::HasCreaturesNearby(const WorldPosition& point, const std::list<Creature*>& creatures) const
{
    for (const Creature* creature : creatures)
    {
        const float distance = creature->GetDistance(point.getX(), point.getY(), point.getZ());
        if (distance <= range)
        {
            return true;
        }
    }

    return false;
}

bool MoveAwayFromCreature::IsHazardNearby(const WorldPosition& point, const std::list<HazardPosition>& hazards) const
{
    for (const HazardPosition& hazard : hazards)
    {
        const float hazardRange = hazard.second;
        const WorldPosition& hazardPosition = hazard.first;
        const float distance = point.distance(hazardPosition);
        if (distance < hazardRange)
        {
            return true;
        }
    }

    return false;
}
bool ai::RunOutOfGroupAction::Execute(Event& event)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    float spread = 15.0f;
    if (float stored = AI_VALUE(float, "run out distance"))
        spread = stored;

    // centroid of the other members
    float cx = 0, cy = 0; uint32 n = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;
        cx += member->GetPositionX(); cy += member->GetPositionY(); ++n;
    }
    if (!n)
        return false;
    cx /= n; cy /= n;

    // straight away from the centroid, try a few angles for a pathable point
    float away = atan2(bot->GetPositionY() - cy, bot->GetPositionX() - cx);
    for (uint8 k = 0; k < 5; ++k)
    {
        const float a = away + ((k % 2) ? -1.0f : 1.0f) * (float)(k / 2) * (M_PI_F / 4.0f);
        const float tx = bot->GetPositionX() + cosf(a) * spread;
        const float ty = bot->GetPositionY() + sinf(a) * spread;
        float tz = bot->GetPositionZ();
        bot->UpdateAllowedPositionZ(tx, ty, tz);
        if (MoveTo(bot->GetMapId(), tx, ty, tz, false, false))
            return true;
    }
    return false;
}

bool ai::TankFaceAwayAction::Execute(Event& event)
{
    Unit* victim = bot->GetVictim();
    Group* group = bot->GetGroup();
    if (!victim || !group)
        return false;

    float cx = 0, cy = 0; uint32 n = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;
        cx += member->GetPositionX(); cy += member->GetPositionY(); ++n;
    }
    if (!n)
        return false;
    cx /= n; cy /= n;

    // far side of the boss: away from the centroid, at melee range
    float dx = victim->GetPositionX() - cx, dy = victim->GetPositionY() - cy;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.1f)
        return false;
    const float melee = 3.5f;
    float tx = victim->GetPositionX() + (dx / len) * melee;
    float ty = victim->GetPositionY() + (dy / len) * melee;
    float tz = victim->GetPositionZ();
    bot->UpdateAllowedPositionZ(tx, ty, tz);
    return MoveTo(bot->GetMapId(), tx, ty, tz, false, false);
}

bool ai::TauntSwapAction::Execute(Event& event)
{
    ObjectGuid bossGuid = AI_VALUE(ObjectGuid, "attack target");
    Unit* boss = ai->GetUnit(bossGuid);
    if (!boss || !boss->IsAlive())
        return false;

    // pick up the boss as our target and taunt with the class tool
    bot->SetSelectionGuid(bossGuid);
    SET_AI_VALUE(Unit*, "current target", boss);

    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            if (ai->CastSpell("taunt", boss))
                return true;
            return ai->CastSpell("mocking blow", boss);
        case CLASS_DRUID:
            if (ai->CastSpell("growl", boss))
                return true;
            break;
        case CLASS_PALADIN:
            // vanilla paladin has no taunt: build threat instead
            return ai->CastSpell("holy shield", bot) || ai->CastSpell("consecration", bot);
        default:
            break;
    }
    return false;
}

bool ai::StagedTankHoldAction::Execute(Event& event)
{
    Unit* boss = ai->GetUnit(AI_VALUE(ObjectGuid, "attack target"));
    if (!boss)
        return false;
    float stage = 18.0f;
    if (float stored = AI_VALUE(float, "run out distance"))
        stage = stored;

    float a = atan2(bot->GetPositionY() - boss->GetPositionY(), bot->GetPositionX() - boss->GetPositionX());
    float tx = boss->GetPositionX() + cosf(a) * stage;
    float ty = boss->GetPositionY() + sinf(a) * stage;
    float tz = boss->GetPositionZ();
    bot->UpdateAllowedPositionZ(tx, ty, tz);
    return MoveTo(bot->GetMapId(), tx, ty, tz, false, false);
}
