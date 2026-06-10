
#include "playerbot/playerbot.h"
#include "MoveToRpgTargetAction.h"
#include "ChooseRpgTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/strategy/values/PossibleRpgTargetsValue.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/TravelMgr.h"

using namespace ai;

namespace
{
    bool BuildRpgApproachPosition(Player* bot, WorldObject* wo, WorldPosition& movePos)
    {
        if (!bot || !wo)
            return false;

        const float baseAngle = wo->GetAngle(bot);
        const uint32 seed = bot->GetGUIDLow() ^ wo->GetGUIDLow();
        const float angleOffset = ((seed % 9) - 4) * (static_cast<float>(M_PI) / 10.0f);
        const float distance = std::max(0.8f, std::min(2.25f, INTERACTION_DISTANCE * 0.75f));
        const float angle = baseAngle + angleOffset;

        movePos = WorldPosition(
            wo->GetMapId(),
            wo->GetPositionX() + std::cos(angle) * distance,
            wo->GetPositionY() + std::sin(angle) * distance,
            wo->GetPositionZ());

        return true;
    }
}

bool MoveToRpgTargetAction::Execute(Event& event)
{
    GuidPosition guidP = AI_VALUE(GuidPosition, "rpg target");
    Unit* unit = ai->GetUnit(guidP);
    GameObject* go = ai->GetGameObject(guidP);
    Player* player = guidP.GetPlayer();

    WorldObject* wo;
    if (unit)
    {
        wo = unit;
    }
    else if(go)
        wo = go;
    else
        return false;

    if (guidP.IsPlayer())
    {
        Player* player = guidP.GetPlayer();

        if (player && ai->IsSafe(player) && player->GetPlayerbotAI())
        {
            GuidPosition guidPP = PAI_VALUE(GuidPosition, "rpg target");

            if (guidPP.IsPlayer())
            {
                AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

                RESET_AI_VALUE(GuidPosition, "rpg target");

                if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
                {
                    ai->TellPlayerNoFacing(GetMaster(), "Rpg player target is targeting me. Drop target");
                }
                return false;
            }
        }
    }

    if (unit && unit->IsMoving() && !urand(0, 20) && guidP.sqDistance2d(bot) < INTERACTION_DISTANCE * INTERACTION_DISTANCE * 2)
    {
        AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

        RESET_AI_VALUE(GuidPosition,"rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Rpg target is moving. Random drop target.");
        }
        return false;
    }

    if (!CanFreeMoveValue::CanFreeMoveTo(ai, wo))
    {
        AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

        RESET_AI_VALUE(GuidPosition, "rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Rpg target is far from mater. Random drop target.");
        }
        return false;
    }

    if (guidP.distance(bot) > sPlayerbotAIConfig.reactDistance * 2)
    {
        AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

        RESET_AI_VALUE(GuidPosition, "rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Rpg target is beyond react distance. Drop target");
        }
        return false;
    }

    if (guidP.IsGameObject() && guidP.sqDistance2d(bot) < INTERACTION_DISTANCE * INTERACTION_DISTANCE && guidP.distance(bot) > INTERACTION_DISTANCE * 1.5 && !urand(0, 5))
    {
        AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

        RESET_AI_VALUE(GuidPosition, "rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Under/above object drop rpg target");
        }
        return false;
    }

    if (!urand(0, 50))
    {
        AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

        RESET_AI_VALUE(GuidPosition, "rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Random drop rpg target");
        }
        return false;
    }

    float x = wo->GetPositionX();
    float y = wo->GetPositionY();
    float z = wo->GetPositionZ();
    float mapId = wo->GetMapId();

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        std::string name = chat->formatWorldobject(wo);

        ai->Poi(x, y, name);
    }
    // Never use special indoor RPG walk behavior for autonomous bots.
    // It produces the unnatural slow crawl the user is seeing around buildings/NPC hubs.
    bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);
    bot->SetWalk(false, !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster());

    WorldPosition movePos(mapId, x, y, z);
    BuildRpgApproachPosition(bot, wo, movePos);
    
    if (movePos.distance(bot) < sPlayerbotAIConfig.sightDistance)
    {
        if (!movePos.ClosestCorrectPoint(5.0f, 5.0f, bot->GetInstanceId()) || abs(movePos.getZ()- z) > 10.0f)
        {
            ai->TellDebug(GetMaster(), "Can not path to desired location around " + chat->formatWorldobject(guidP.GetWorldObject(bot->GetInstanceId())) + " trying again later.", "debug move");

            return false;
        }
    }

    bool couldMove;

    if (unit && unit->IsMoving() && bot->GetDistance(unit) < INTERACTION_DISTANCE * 2 && unit->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {

        Creature* creature = static_cast<Creature*>(unit);


        if (creature)
            if (uint32 pauseTimer = creature->GetInteractionPauseTimer())
                creature->GetMotionMaster()->PauseWaypoints(pauseTimer);
    }
        couldMove = MoveTo(mapId, movePos.getX(), movePos.getY(), movePos.getZ(), false, false, false, false, false);

    if (!couldMove && movePos.distance(bot) > INTERACTION_DISTANCE)
    {
        AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));

        RESET_AI_VALUE(GuidPosition, "rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Could not move to rpg target. Drop rpg target");
        }

        return false;
    }

    if ((ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT)) && guidP.GetWorldObject(bot->GetInstanceId()))
    {
        if (couldMove)
        {
            std::ostringstream out;
            out << "Heading to: ";
            out << chat->formatWorldobject(guidP.GetWorldObject(bot->GetInstanceId()));
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
        else
        {
            std::ostringstream out;
            out << "Near: ";
            out << chat->formatWorldobject(guidP.GetWorldObject(bot->GetInstanceId()));
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    if (couldMove)
        WaitForReach(std::max<float>(movePos.distance(bot), 1.0f));

    return couldMove;
}

bool MoveToRpgTargetAction::isUseful()
{
    GuidPosition guidP = AI_VALUE(GuidPosition, "rpg target");
    WorldPosition oldPosition = guidP;

    if (!guidP)
        return false;

    WorldObject* wo = guidP.GetWorldObject(bot->GetInstanceId());

    if (!wo)
    {
        RESET_AI_VALUE(GuidPosition, "rpg target");

        if (ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT))
        {
            ai->TellPlayerNoFacing(GetMaster(), "Target could not be found. Drop rpg target");
        }
    }

    if(MEM_AI_VALUE(WorldPosition, "current position")->LastChangeDelay() < 60)
        if (bot->IsMoving() && bot->GetMotionMaster() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            return false;

    if (AI_VALUE(bool, "travel target traveling"))
        return false;

    if (AI_VALUE2(float, "distance", "rpg target") < INTERACTION_DISTANCE)
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    if (AI_VALUE(bool, "has available loot"))
    {
        LootObject lootObject = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);
        if (lootObject.IsLootPossible(bot))
            return false;
    }

    return true;
}
