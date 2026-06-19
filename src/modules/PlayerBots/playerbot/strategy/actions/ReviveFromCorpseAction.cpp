
#include "playerbot/playerbot.h"
#include "ReviveFromCorpseAction.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/FleeManager.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/DeadValues.h"
#include <unordered_map>

using namespace ai;

namespace
{
    struct DeadRecoveryProgress
    {
        uint32 corpseGhostTime = 0;
        uint32 botMapId = 0;
        uint32 corpseMapId = 0;
        float bestDistance = std::numeric_limits<float>::max();
        time_t lastProgressTime = 0;
        time_t startTime = 0;
        time_t lastMoveFailLogTime = 0;
    };

    std::unordered_map<uint32, DeadRecoveryProgress> sCorpseRecoveryProgress;
    std::unordered_map<uint32, DeadRecoveryProgress> sSpiritRecoveryProgress;

    DeadRecoveryProgress& UpdateDeadRecoveryProgress(std::unordered_map<uint32, DeadRecoveryProgress>& stateMap, Player* bot, Corpse* corpse, float distance)
    {
        const uint32 botGuid = bot->GetGUIDLow();
        const uint32 ghostTime = corpse ? corpse->GetGhostTime() : 0;
        const time_t now = time(nullptr);

        DeadRecoveryProgress& state = stateMap[botGuid];
        const uint32 botMapId = bot->GetMapId();
        const uint32 corpseMapId = corpse ? corpse->GetMapId() : botMapId;
        if (!state.startTime || state.corpseGhostTime != ghostTime || state.botMapId != botMapId || state.corpseMapId != corpseMapId)
        {
            state.corpseGhostTime = ghostTime;
            state.botMapId = botMapId;
            state.corpseMapId = corpseMapId;
            state.bestDistance = distance;
            state.lastProgressTime = now;
            state.startTime = now;
            state.lastMoveFailLogTime = 0;
            return state;
        }

        if (distance + 1.0f < state.bestDistance)
        {
            state.bestDistance = distance;
            state.lastProgressTime = now;
        }

        return state;
    }

    void ClearDeadRecoveryProgress(Player* bot)
    {
        if (!bot)
            return;

        const uint32 botGuid = bot->GetGUIDLow();
        sCorpseRecoveryProgress.erase(botGuid);
        sSpiritRecoveryProgress.erase(botGuid);
    }

    void ResetDeadRecoveryTargets(PlayerbotAI* ai, Player* bot)
    {
        if (!ai || !bot)
            return;

        AiObjectContext* context = ai->GetAiObjectContext();
        context->GetValue<Unit*>("current target")->Set(nullptr);
        context->GetValue<Unit*>("enemy player target")->Set(nullptr);
        context->GetValue<Unit*>("pull target")->Set(nullptr);
        context->GetValue<ObjectGuid>("attack target")->Set(ObjectGuid());
        bot->SetSelectionGuid(ObjectGuid());
    }

    bool TeleportGhostTo(PlayerbotAI* ai, Player* bot, const WorldPosition& destination, const char* eventName, float radius = 4.0f)
    {
        if (!ai || !bot)
            return false;

        WorldPosition movePosition = destination;
        if (radius > 0.0f)
            movePosition.GetReachableRandomPointOnGround(bot, radius, urand(0, 1));

        if (!movePosition)
            movePosition = destination;

        bot->GetMotionMaster()->Clear();
        const bool teleported = bot->TeleportTo(movePosition.getMapId(), movePosition.getX(), movePosition.getY(), movePosition.getZ(), movePosition.getO(), 0);
        if (teleported)
        {
            if (bot->isRealPlayer())
                bot->SendHeartBeat();

            std::ostringstream out;
            out << "map=" << movePosition.getMapId()
                << " x=" << movePosition.getX()
                << " y=" << movePosition.getY()
                << " z=" << movePosition.getZ();
            sPlayerbotAIConfig.logEvent(ai, eventName, "", out.str());
        }

        return teleported;
    }

    bool ShouldPhysicallyRunToCorpse(PlayerbotAI* ai, Player* bot, Player* master, const WorldPosition& botPos, const WorldPosition& corpsePos)
    {
        if (!ai || !bot)
            return false;

        if (ai->HasActivePlayerMaster() || ai->HasRealPlayerMaster())
            return true;

        if (master && master != bot && !master->GetPlayerbotAI())
            return true;

        return ai->HasPlayerNearby(botPos) || ai->HasPlayerNearby(corpsePos);
    }
}

bool ReviveFromCorpseAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Player* master = ai->GetGroupMaster();
    Corpse* corpse = bot->GetCorpse();

    // follow master when master revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && master && !corpse && sServerFacade.IsAlive(bot))
    {
        if (sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.farDistance))
        {
            std::string defaultMovementStrategy = ai->GetDefaultMovementStrategy();

            if (!ai->HasStrategy(defaultMovementStrategy, BotState::BOT_STATE_NON_COMBAT))
            {
                ai->TellPlayerNoFacing(requester, "Welcome back!", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
                ai->ChangeStrategy("+" + defaultMovementStrategy + ",-stay", BotState::BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) > time(nullptr))
        return false;

    if (master)
    {
        //Revive with master.
        if (bot != master && sServerFacade.UnitIsDead(master) && master->GetCorpse() && sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.farDistance))
            return false;
    }

    sLog.outDetail("Bot #%d %s:%d <%s> revives at body", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

    ai->StopMoving();
    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetObjectGuid();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    if (!sServerFacade.IsAlive(bot) && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) && !ai->HasActivePlayerMaster())
    {
        WorldPosition botPos(bot), corpsePos(corpse);
        const float dist2d = sServerFacade.GetDistance2d(bot, corpse);
        const float dist3d = botPos.distance(corpsePos);
        const float dz = fabs(botPos.getZ() - corpsePos.getZ());

        // The core reclaim handler rejects some ghosts that are horizontally at
        // their corpse but vertically separated by terrain/caves. For unattended
        // bots, finish the reclaim directly instead of looping on the same action.
        if (botPos.getMapId() == corpsePos.getMapId() && dist2d <= CORPSE_RECLAIM_RADIUS)
        {
            bot->ResurrectPlayer(bot->InBattleGround() ? 1.0f : 0.5f);
            if (sServerFacade.IsAlive(bot))
            {
                bot->SpawnCorpseBones();
                std::ostringstream out;
                out << "dist2d=" << dist2d
                    << " dist3d=" << dist3d
                    << " dz=" << dz;
                sPlayerbotAIConfig.logEvent(ai, "ReviveFromCorpseDirectFallback", "", out.str());
            }
            else
            {
                std::ostringstream out;
                out << "dist2d=" << dist2d
                    << " dist3d=" << dist3d
                    << " dz=" << dz
                    << " hardcore=" << (bot->IsHardcore() ? 1 : 0);
                sPlayerbotAIConfig.logEvent(ai, "ReviveFromCorpseDirectFailed", "", out.str());
            }
        }
    }

    if (!sServerFacade.IsAlive(bot))
    {
        WorldPosition botPos(bot), corpsePos(corpse);
        std::ostringstream out;
        out << "dist2d=" << sServerFacade.GetDistance2d(bot, corpse)
            << " dist3d=" << botPos.distance(corpsePos)
            << " dz=" << fabs(botPos.getZ() - corpsePos.getZ())
            << " ghost=" << (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0)
            << " hardcore=" << (bot->IsHardcore() ? 1 : 0);
        sPlayerbotAIConfig.logEvent(ai, "ReviveFromCorpseStillDead", "", out.str());
        return true;
    }

    ClearDeadRecoveryProgress(bot);

    sPlayerbotAIConfig.logEvent(ai, "ReviveFromCorpseAction");

    return true;
}

bool FindCorpseAction::Execute(Event& event)
{
    if (bot->InBattleGround())
        return false;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return ai->DoSpecificAction("auto release", event, true);

    Player* master = ai->GetGroupMaster();
    if (master)
    {
        if (!master->GetPlayerbotAI() && sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.farDistance))
        {
            return false;
        }
    }
    ResetDeadRecoveryTargets(ai, bot);

    WorldPosition botPos(bot), corpsePos(corpse), moveToPos = corpsePos, masterPos(master);
    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);

    //If player fell through terrain move corpse to player position.
    if (bot->isRealPlayer() && botPos.getMapId() == moveToPos.getMapId())
    {
        //Try to correct the position upward.
        if (!moveToPos.ClosestCorrectPoint(5.0f, 500.0f, bot->GetInstanceId()))
        {
            //Revive in place.
            corpse->Relocate(botPos.getX(), botPos.getY(), botPos.getZ());
            corpsePos = corpse;
            corpseDist = botPos.distance(corpsePos);
        }
        else
        {
            corpse->Relocate(moveToPos.getX(), moveToPos.getY(), moveToPos.getZ());
            corpsePos = corpse;
            corpseDist = botPos.distance(corpsePos);
        }
    }

    int64 deadTime = time(nullptr) - corpse->GetGhostTime();
    DeadRecoveryProgress& progress = UpdateDeadRecoveryProgress(sCorpseRecoveryProgress, bot, corpse, corpseDist);
    const bool physicalCorpseRun = ShouldPhysicallyRunToCorpse(ai, bot, master, botPos, corpsePos);

    bool moveToMaster = master && master != bot && masterPos.fDist(corpsePos) < reclaimDist;

    // If we're already in reclaim range, hand off directly to the actual revive path
    // instead of idling forever on "find corpse" with no movement.
    if (corpseDist < reclaimDist)
    {
        if (moveToMaster) //We are near master.
        {
            if (botPos.fDist(masterPos) < sPlayerbotAIConfig.spellDistance)
                return ai->DoSpecificAction("revive from corpse", Event(), true) || true;
        }
        else
        {
            std::list<ObjectGuid> units = AI_VALUE(std::list<ObjectGuid>, "possible targets no los");

            if (botPos.getUnitsAggro(units, bot) == 0) //There are no mobs near.
                return ai->DoSpecificAction("revive from corpse", Event(), true) || true;

            if (!ai->HasActivePlayerMaster() && deadTime > 30)
            {
                sPlayerbotAIConfig.logEvent(ai, "DeadCorpseReclaimForced", "", "reason=in-range-timeout");
                return ai->DoSpecificAction("revive from corpse", Event(), true) || true;
            }
        }
    }

    //If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.reactDistance)
    {
        if (moveToMaster)
        {
            if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
            {
                std::ostringstream out;
                out << "Moving to revive near master.";
                ai->TellPlayerNoFacing(GetMaster(), out);
            }
            moveToPos = masterPos;
        }
        else
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                {
                    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    {
                        std::ostringstream out;
                        out << "Moving to revive some where safe.";
                        ai->TellPlayerNoFacing(GetMaster(), out);
                    }
                    moveToPos = WorldPosition(moveToPos.getMapId(), rx, ry, rz, 0.0);
                }
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                {
                    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    {
                        std::ostringstream out;
                        out << "Moving to revive at corpse.";
                        ai->TellPlayerNoFacing(GetMaster(), out);
                    }
                    moveToPos = corpsePos;
                }
            }
        }
    }
    else
    {
        if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Moving towards corpse.";
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    // Actual moving part. Ghosts should physically path back to the corpse when possible
    // instead of idling until a simulated travel delay expires.
    bool moved = false;

    if (!physicalCorpseRun && corpseDist > reclaimDist)
    {
        std::ostringstream out;
        out << "dist=" << corpseDist
            << " playerNearBot=" << (ai->HasPlayerNearby(botPos) ? 1 : 0)
            << " playerNearCorpse=" << (ai->HasPlayerNearby(corpsePos) ? 1 : 0)
            << " activeMaster=" << (ai->HasActivePlayerMaster() ? 1 : 0)
            << " realMaster=" << (ai->HasRealPlayerMaster() ? 1 : 0);
        sPlayerbotAIConfig.logEvent(ai, "DeadCorpseUnobservedTeleport", "", out.str());

        return TeleportGhostTo(ai, bot, corpsePos, "DeadCorpseUnstuckTeleport", reclaimDist > 5.0f ? reclaimDist - 2.0f : 3.0f);
    }

    if (!ai->AllowActivity(DETAILED_MOVE_ACTIVITY) && !ai->HasPlayerNearby(moveToPos))
    {
        uint32 delay = sServerFacade.GetDistance2d(bot, corpse) / bot->GetSpeed(MOVE_RUN);
        delay = std::min(delay, uint32(10 * MINUTE));

        if (deadTime <= delay)
            return true;
    }

    if (!moved)
    {
#ifndef MANGOSBOT_ZERO
        if (bot->IsMovingIgnoreFlying())
            moved = true;
#else
        if (bot->IsMoving())
            moved = true;
#endif
        else
        {

            moved = MoveTo(moveToPos.getMapId(), moveToPos.getX(), moveToPos.getY(), moveToPos.getZ(), false, false);

            if (!moved &&
                !ai->HasActivePlayerMaster() &&
                AI_VALUE(bool, "should spirit healer")) // Only give up on corpse-running when the broader policy agrees.
            {
                moved = ai->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    const time_t noProgressSecs = time(nullptr) - progress.lastProgressTime;
    if (!moved && !physicalCorpseRun)
    {
        const time_t now = time(nullptr);
        if (!progress.lastMoveFailLogTime || now - progress.lastMoveFailLogTime >= 15)
        {
            std::ostringstream out;
            out << "dist=" << corpseDist
                << " bestDist=" << progress.bestDistance
                << " noProgressSec=" << noProgressSecs
                << " botMap=" << botPos.getMapId()
                << " corpseMap=" << corpsePos.getMapId()
                << " playerNearBot=" << (ai->HasPlayerNearby(botPos) ? 1 : 0)
                << " playerNearCorpse=" << (ai->HasPlayerNearby(corpsePos) ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "DeadCorpseMoveFailed", "", out.str());
            progress.lastMoveFailLogTime = now;
        }

        if (corpseDist > reclaimDist &&
            !ai->HasPlayerNearby(botPos) &&
            !ai->HasPlayerNearby(corpsePos) &&
            noProgressSecs >= 5)
        {
            std::ostringstream out;
            out << "dist=" << corpseDist
                << " bestDist=" << progress.bestDistance
                << " noProgressSec=" << noProgressSecs;
            sPlayerbotAIConfig.logEvent(ai, "DeadCorpsePathRejectedTeleport", "", out.str());

            if (TeleportGhostTo(ai, bot, corpsePos, "DeadCorpseUnstuckTeleport", reclaimDist > 5.0f ? reclaimDist - 2.0f : 3.0f))
                return true;
        }
    }

    if (
        !physicalCorpseRun &&
        corpseDist > reclaimDist &&
        noProgressSecs >= (moved ? 60 : 30) &&
        !ai->HasPlayerNearby(botPos) &&
        !ai->HasPlayerNearby(corpsePos))
    {
        std::ostringstream out;
        out << "dist=" << corpseDist
            << " bestDist=" << progress.bestDistance
            << " stallSec=" << noProgressSecs;
        sPlayerbotAIConfig.logEvent(ai, "DeadCorpseProgressStall", "", out.str());

        if (TeleportGhostTo(ai, bot, corpsePos, "DeadCorpseUnstuckTeleport", reclaimDist > 5.0f ? reclaimDist - 2.0f : 3.0f))
            return true;
    }

    return moved;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    return sServerFacade.UnitIsDead(bot) && bot->GetCorpse() && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
}

bool SpiritHealerAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        ai->TellPlayerNoFacing(requester, "I am not a spirit");
        return false;
    }
    ResetDeadRecoveryTargets(ai, bot);

    if (!AI_VALUE(bool, "should spirit healer"))
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            sPlayerbotAIConfig.logEvent(ai, "SpiritHealerDeferred", "", "reason=corpse-preferred");
        return false;
    }

    uint32 dCount = AI_VALUE(uint32, "death count");
    GuidPosition grave = AI_VALUE(GuidPosition, "best graveyard");

    //something went wrong
    if (!grave)
    {
        //prevent doing weird stuff OR GOING TO 0,0,0
        sLog.outDetail(
            "ERROR: no graveyard in SpiritHealerAction for bot #%d %s:%d <%s>, evacuating to prevent weird behavior",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
        ai->DoSpecificAction("repop");
        return false;
    }

    if (grave && grave.fDist(bot) < sPlayerbotAIConfig.sightDistance)
    {
        bool foundSpiritHealer = false;
        std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
        for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = ai->GetUnit(*i);
            if (unit && unit->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_SPIRITHEALER))
            {
                foundSpiritHealer = true;
                break;
            }
        }

        if (!foundSpiritHealer)
        {
            sLog.outDetail("Bot #%d %s:%d <%s> can't find a spirit healer", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
            ai->TellPlayerNoFacing(requester, "Cannot find any spirit healer nearby");
        }


        sLog.outDetail("Bot #%d %s:%d <%s> revives at spirit healer", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
        PlayerbotChatHandler ch(bot);
        bot->ResurrectPlayer(0.5f, !ai->HasCheat(BotCheatMask::repair));
        bot->DurabilityLossAll(0.25f, true);

        bot->SpawnCorpseBones();
        bot->SaveToDB();
        context->GetValue<Unit*>("current target")->Set(nullptr);
        bot->SetSelectionGuid(ObjectGuid());
        ClearDeadRecoveryProgress(bot);
        ai->TellPlayer(requester, BOT_TEXT("hello"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        sPlayerbotAIConfig.logEvent(ai, "ReviveFromSpiritHealerAction");

        return true;
    }

    bool shouldTeleportToGY = false;

    const int64 deadTime = time(nullptr) - corpse->GetGhostTime();
    const float graveDistance = WorldPosition(bot).distance(WorldPosition(grave));
    DeadRecoveryProgress& progress = UpdateDeadRecoveryProgress(sSpiritRecoveryProgress, bot, corpse, graveDistance);

    shouldTeleportToGY = deadTime > uint32(10 * MINUTE);

    // Check if we can teleport to the graveyard when nobody is looking
    if (!shouldTeleportToGY && !ai->AllowActivity(DETAILED_MOVE_ACTIVITY) && !ai->HasPlayerNearby(WorldPosition(grave)))
    {
        //Time a bot would take to travel to it's corpse.
        uint32 delay = sServerFacade.GetDistance2d(bot, corpse) / bot->GetSpeed(MOVE_RUN);
        delay = std::min(delay, uint32(10 * MINUTE));

        shouldTeleportToGY = deadTime > delay;
    }

    const time_t noProgressSecs = time(nullptr) - progress.lastProgressTime;
    if (!shouldTeleportToGY &&
        !ai->HasActivePlayerMaster() &&
        graveDistance > sPlayerbotAIConfig.spellDistance &&
        noProgressSecs >= 60 &&
        !ai->HasPlayerNearby(WorldPosition(grave)))
    {
        std::ostringstream out;
        out << "dist=" << graveDistance
            << " bestDist=" << progress.bestDistance
            << " stallSec=" << noProgressSecs;
        sPlayerbotAIConfig.logEvent(ai, "DeadSpiritHealerProgressStall", "", out.str());
        shouldTeleportToGY = true;
    }

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Moving towards graveyard.";
        ai->TellPlayerNoFacing(GetMaster(), out);
    }

    if (shouldTeleportToGY)
    {
        return TeleportGhostTo(ai, bot, WorldPosition(grave), "DeadSpiritHealerTeleport");
    }
    bool moved = MoveTo(grave.getMapId(), grave.getX(), grave.getY(), grave.getZ(), false, false);
    if (!moved &&
        !ai->HasActivePlayerMaster() &&
        !ai->HasPlayerNearby(WorldPosition(grave)))
    {
        std::ostringstream out;
        out << "dist=" << graveDistance
            << " bestDist=" << progress.bestDistance
            << " deadTimeSec=" << deadTime
            << " moveFailed=1";
        sPlayerbotAIConfig.logEvent(ai, "DeadSpiritHealerMoveFailed", "", out.str());
        return TeleportGhostTo(ai, bot, WorldPosition(grave), "DeadSpiritHealerTeleportFallback");
    }

    return moved;
}

bool SpiritHealerAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    return bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
}
