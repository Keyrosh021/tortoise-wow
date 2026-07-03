
#include "playerbot/playerbot.h"
#include "MoveToRpgTargetAction.h"
#include "ChooseRpgTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/strategy/values/PossibleRpgTargetsValue.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/TravelMgr.h"
#include <mutex>
#include <unordered_map>

using namespace ai;

namespace
{
    // RPG-MOVE FREEZE BREAKER (same defect + same fix as travel): "move to rpg target" can "succeed"
    // (a MovePoint is issued) toward an rpg target the bot can't path to, so it stands "moving" forever
    // while the rpg target churns. Key on the BOT's NET DISPLACEMENT over wall-clock (NOT the guid,
    // which resets every tick): physically moved <8y in 8s -> stuck.
    bool RpgMoveStuck(Player* bot, bool legitimatelyStationary = false)
    {
        if (!bot)
            return false;
        struct St { float x; float y; uint32 sinceMs; };
        constexpr uint32 WINDOW_MS = 8000;
        constexpr float MIN_DISP = 8.0f;
        static std::mutex mx;
        static std::unordered_map<uint32, St> track;
        const uint32 now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        // Legit stationary states reset the window instead of aging it (see travel breaker: the
        // window aging through combat made the first post-fight drink read as an 8s+ freeze).
        if (!s.sinceMs || legitimatelyStationary)
        {
            s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.sinceMs = now;
            return false;
        }
        const float dx = bot->GetPositionX() - s.x, dy = bot->GetPositionY() - s.y;
        if (dx * dx + dy * dy >= MIN_DISP * MIN_DISP)
        {
            s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.sinceMs = now;
            return false;
        }
        return (now - s.sinceMs) >= WINDOW_MS;
    }

    bool IsProgressionTravelStatus(TravelStatus status)
    {
        return status == TravelStatus::TRAVEL_STATUS_READY ||
               status == TravelStatus::TRAVEL_STATUS_TRAVEL;
    }

    bool HasProgressionTravelTarget(TravelTarget* travelTarget)
    {
        if (!travelTarget || !travelTarget->IsActive() || !travelTarget->GetDestination() || travelTarget->GetEntry() == 0)
            return false;

        return IsProgressionTravelStatus(travelTarget->GetStatus());
    }

    bool ShouldYieldRpgMovement(Player* bot, TravelTarget* travelTarget)
    {
        return HasProgressionTravelTarget(travelTarget);
    }

    void LogRpgTargetDroppedForProgression(PlayerbotAI* ai, GuidPosition const& guidP, TravelTarget* travelTarget, char const* phase)
    {
        if (!sPlayerbotAIConfig.hasLog("bot_events.csv") || urand(1, 20) != 1)
            return;

        std::ostringstream out;
        out << "phase=" << phase;
        if (travelTarget)
        {
            out << " travelStatus=" << static_cast<uint32>(travelTarget->GetStatus())
                << " travelEntry=" << travelTarget->GetEntry()
                << " travel=" << (travelTarget->GetDestination() ? travelTarget->GetDestination()->GetTitle() : "none");
        }

        sPlayerbotAIConfig.logEvent(ai, "RpgTargetDroppedForProgress", guidP ? guidP.to_string() : "none", out.str());
    }

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
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    if (ShouldYieldRpgMovement(bot, travelTarget))
    {
        GuidPosition guidP = AI_VALUE(GuidPosition, "rpg target");
        if (guidP)
            AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(guidP);
        RESET_AI_VALUE(GuidPosition, "rpg target");
        LogRpgTargetDroppedForProgression(ai, guidP, travelTarget, "execute");
        return false;
    }

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

    // Frozen while "moving" to an unreachable rpg target for 8s+ -> drop it and re-pick instead of
    // standing forever (measured as the #2 idle cause after travel freezes).
    const bool rpgHoldLegit = sServerFacade.IsInCombat(bot) ||
        bot->IsNonMeleeSpellCasted(false) ||
        bot->getStandState() != UNIT_STAND_STATE_STAND ||
        bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    if (rpgHoldLegit)
        RpgMoveStuck(bot, true); // reset the freeze window; do not age it through the hold
    else if (couldMove && RpgMoveStuck(bot))
    {
        AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(AI_VALUE(GuidPosition, "rpg target"));
        RESET_AI_VALUE(GuidPosition, "rpg target");
        return false;
    }

    if (couldMove)
        WaitForReach(std::max<float>(movePos.distance(bot), 1.0f));

    return couldMove;
}

bool MoveToRpgTargetAction::isUseful()
{
    // POST-FLEE HOLD (same as travel): resuming rpg movement right after a flee walked the bot back
    // through the mob it escaped -> re-aggro ping-pong. Wait out the leash + recover first.
    {
        const time_t lastFlee = AI_VALUE(LastMovement&, "last movement").lastFlee;
        if (lastFlee && time(0) - lastFlee < 10 && bot->GetHealthPercent() < 90.0f)
            return false;
    }

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

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    if (ShouldYieldRpgMovement(bot, travelTarget))
    {
        if (guidP)
            AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target").insert(guidP);
        RESET_AI_VALUE(GuidPosition, "rpg target");
        LogRpgTargetDroppedForProgression(ai, guidP, travelTarget, "isUseful");
        return false;
    }

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
