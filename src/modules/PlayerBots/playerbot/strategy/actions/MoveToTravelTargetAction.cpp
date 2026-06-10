
#include "playerbot/playerbot.h"
#include "MoveToTravelTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "Maps/PathFinder.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include <cmath>
#include <iomanip>
#include <mutex>
#include <unordered_map>

using namespace ai;

namespace
{
    struct HumanLikeTravelState
    {
        time_t nextPause = 0;
        time_t nextJump = 0;
        time_t nextTrace = 0;
    };

    std::mutex s_humanLikeTravelMutex;
    std::unordered_map<uint32, HumanLikeTravelState> s_humanLikeTravelStates;
}

bool MoveToTravelTargetAction::Execute(Event& event)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    if (!ai->HasRealPlayerMaster() && !ai->HasActivePlayerMaster())
    {
        bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);
        bot->SetWalk(false, true);
    }

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
    {
        ai->TellDebug(ai->GetMaster(), "The target is ready to travel start now.", "debug travel");
        target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
    }

    target->CheckStatus();

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
    {
        if (QuestRelationTravelDestination* questDest = dynamic_cast<QuestRelationTravelDestination*>(target->GetDestination()))
        {
            ObjectGuid targetGuid;
            if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
            {
                if (WorldObject* wo = guidPos->GetWorldObject(bot->GetInstanceId()))
                    targetGuid = wo->GetObjectGuid();
            }

            if (targetGuid)
                bot->SetSelectionGuid(targetGuid);

            bool didQuestWork = false;
            if (questDest->GetRelation() == 0)
                didQuestWork = ai->DoSpecificAction("accept quest", Event("quest travel", "*", bot), true);
            else
                didQuestWork = ai->DoSpecificAction("talk to quest giver", targetGuid ? Event("quest travel", targetGuid, bot) : Event("quest travel", "", bot), true);

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "source=move-to-travel"
                    << " entry=" << questDest->GetEntry()
                    << " questId=" << questDest->GetQuestId()
                    << " relation=" << (uint32)questDest->GetRelation()
                    << " guid=" << (targetGuid ? targetGuid.GetRawValue() : 0)
                    << " didQuestWork=" << (didQuestWork ? 1 : 0);
                sPlayerbotAIConfig.logEvent(ai, "QuestTravelWorkAction", target->GetDestination()->GetTitle(), out.str());
            }

            target->SetStatus(didQuestWork ? TravelStatus::TRAVEL_STATUS_EXPIRED : TravelStatus::TRAVEL_STATUS_COOLDOWN);
            context->ClearValues("no active travel destinations");
            RESET_AI_VALUE(bool, "travel target active");
            return didQuestWork;
        }
    }

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL)
        return true;

    WorldPosition botLocation(bot);
    WorldPosition location = *target->GetPosition();
    
    Group* group = bot->GetGroup();
    if (ai->IsGroupLeader() && !urand(0, 1) && !bot->IsInCombat())
    {        
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member == bot)
                continue;

            if (!member->IsAlive())
                continue;

            if (!member->IsMoving())
                continue;

            if (member->GetPlayerbotAI() &&
                !(member->GetPlayerbotAI()->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || member->GetPlayerbotAI()->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
                continue;

            WorldPosition memberPos(member);
            WorldPosition targetPos = *target->GetPosition();

            float memberDistance = std::min(botLocation.distance(memberPos), location.distance(memberPos));

            if (memberDistance < 50.0f)
                continue;
            if (memberDistance > sPlayerbotAIConfig.reactDistance * 20)
                continue;

           // float memberAngle = botLocation.getAngleBetween(targetPos, memberPos);

           // if (botLocation.getMapId() == targetPos.getMapId() && botLocation.getMapId() == memberPos.getMapId() && memberAngle < M_PI_F / 2) //We are heading that direction anyway.
           //     continue;

            if (!urand(0, 5))
            {
                std::ostringstream out;
                if ((ai->GetMaster() && !bot->GetGroup()->IsMember(ai->GetMaster()->GetObjectGuid())) || !ai->HasActivePlayerMaster())
                    out << "Waiting a bit for ";
                else
                    out << "Please hurry up ";

                out << member->GetName();

                if (bot->GetPlayerbotAI() && !ai->HasActivePlayerMaster())
                {
                    out << " who is " << round(memberDistance) << "y away";
                    if (!memberPos.getAreaName().empty())
                        out << " in " << memberPos.getAreaName();
                }

                ai->TellPlayerNoFacing(GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            }

            // Introduce a random delay between 80% and 120% of maxWaitForMove to make waiting more natural
            uint32 randomDelay = sPlayerbotAIConfig.maxWaitForMove * (urand(80, 120) / 100.0f);
            target->SetExpireIn(target->GetTimeLeft() + randomDelay);

            SetDuration(randomDelay);

            sPlayerbotAIConfig.logEvent(ai, "HumanLikePause", member->GetName(), std::to_string(randomDelay));

            // Occasionally face the member and perform an emote
            if (urand(0, 3) == 0) { // 25% chance to emote
                bot->SetFacingToObject(member);
                uint32 emoteChoice = urand(0, 2);
                switch (emoteChoice) {
                    case 0:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_POINT);
                        break;
                    case 1:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                        break;
                    case 2:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_EXCLAMATION);
                        break;
                }
            }

            return true;
        }
    }

    float x = location.getX();
    float y = location.getY();
    float z = location.getZ();
    float mapId = location.getMapId();

    const bool humanLikeTravel = false;
    time_t now = time(0);
    HumanLikeTravelState humanState;
    if (humanLikeTravel)
    {
        std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
        humanState = s_humanLikeTravelStates[bot->GetGUIDLow()];
    }

    if (humanLikeTravel)
    {
        if (humanState.nextTrace <= now)
        {
            std::ostringstream trace;
            trace << "combat=" << (bot->IsInCombat() ? 1 : 0)
                  << " mounted=" << (bot->IsMounted() ? 1 : 0)
                  << " sameMap=" << (botLocation.getMapId() == location.getMapId() ? 1 : 0)
                  << " sq2d=" << std::fixed << std::setprecision(2) << botLocation.sqDistance2d(location);

            sPlayerbotAIConfig.logEvent(
                ai,
                "HumanLikeTravelTrace",
                target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
                trace.str());

            std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
            s_humanLikeTravelStates[bot->GetGUIDLow()].nextTrace = now + 15;
        }
    }

    if (botLocation.getMapId() == location.getMapId() && botLocation.sqDistance2d(location) < 10000.0f)
    {
        if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Moving to ";
            out << target->GetDestination()->GetTitle();
            if (!(*target->GetPosition() == WorldPosition()))
            {
                out << " at " << uint32(target->GetPosition()->distance(bot)) << "y";
            }
            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " for " << (target->GetTimeLeft() / 1000) << "s";
            if (target->GetRetryCount(true))
                out << " (move retry: " << target->GetRetryCount(true) << ")";
            else if (target->GetRetryCount(false))
                out << " (retry: " << target->GetRetryCount(false) << ")";
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    if (humanLikeTravel && !bot->IsInCombat() && !bot->IsMounted())
    {
        sPlayerbotAIConfig.logEvent(
            ai,
            "HumanLikeTravelEligible",
            target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
            "eligible");

        if (humanState.nextPause <= now && frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikePauseChance)
        {
            uint32 pauseMs = urand(sPlayerbotAIConfig.humanLikePauseMinMs, sPlayerbotAIConfig.humanLikePauseMaxMs);
            SetDuration(pauseMs);
            sPlayerbotAIConfig.logEvent(ai, "HumanLikePause", target->GetDestination() ? target->GetDestination()->GetTitle() : "travel", std::to_string(pauseMs));

            {
                std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
                HumanLikeTravelState& state = s_humanLikeTravelStates[bot->GetGUIDLow()];
                state.nextPause = now + (sPlayerbotAIConfig.humanLikePauseCooldownMs / 1000);
            }

            return true;
        }

        if (frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikePathJitterChance)
        {
            float dx = location.getX() - botLocation.getX();
            float dy = location.getY() - botLocation.getY();
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.01f)
            {
                dx /= len;
                dy /= len;
                float lateralX = -dy;
                float lateralY = dx;

                float forwardJitter = frand(-sPlayerbotAIConfig.humanLikePathForwardJitterRadius,
                                            sPlayerbotAIConfig.humanLikePathForwardJitterRadius);
                float lateralJitter = frand(-sPlayerbotAIConfig.humanLikePathJitterRadius,
                                            sPlayerbotAIConfig.humanLikePathJitterRadius);

                x += dx * forwardJitter + lateralX * lateralJitter;
                y += dy * forwardJitter + lateralY * lateralJitter;

                sPlayerbotAIConfig.logEvent(
                    ai,
                    "HumanLikePathJitter",
                    std::to_string(forwardJitter),
                    std::to_string(lateralJitter));
            }
        }

        if (humanState.nextJump <= now)
        {
            if (frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikeSpinChance)
            {
                float spinOffset = frand(0.9f * static_cast<float>(M_PI), 1.8f * static_cast<float>(M_PI));
                float wobble = frand(-0.45f, 0.45f);
                bot->SetFacingTo(bot->GetOrientation() + spinOffset + wobble);
                sPlayerbotAIConfig.logEvent(ai, "HumanLikeSpin", std::to_string(spinOffset), std::to_string(wobble));
            }

            if (frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikeJumpChance)
            {
                ai->DoSpecificAction("jump::random", Event(), true);
            }

            {
                std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
                HumanLikeTravelState& state = s_humanLikeTravelStates[bot->GetGUIDLow()];
                state.nextJump = now + (sPlayerbotAIConfig.humanLikeJumpCooldownMs / 1000);
            }
        }
    }

    bool canMove = MoveTo(mapId, x, y, z, false, false);

    if (!canMove)
    {
        target->IncRetry(true);

        if (target->IsMaxRetry(true))
        {
            ai->TellDebug(ai->GetMaster(), "The target is cooling down because we failed to move to it a few times in a row.", "debug travel");
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=move-failed"
                    << " retries=max"
                    << " status=" << static_cast<uint32>(target->GetStatus())
                    << " destination=" << (target->GetDestination() ? target->GetDestination()->GetTitle() : "none")
                    << " botMap=" << bot->GetMapId()
                    << " targetMap=" << mapId
                    << " targetX=" << x
                    << " targetY=" << y
                    << " targetZ=" << z;
                sPlayerbotAIConfig.logEvent(ai, "TravelCooldownTrace", target->GetDestination() ? target->GetDestination()->GetTitle() : "travel", out.str());
            }
            target->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);      
            target->SetForced(false);
        }
    }
    else
    {
        target->DecRetry(true);
        WaitForReach(std::max<float>(botLocation.distance(WorldPosition(mapId, x, y, z)), 1.0f));
    }

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        WorldPosition* pos = target->GetPosition();
        GuidPosition* guidP = dynamic_cast<GuidPosition*>(pos);

        std::string name = (guidP && guidP->GetWorldObject(bot->GetInstanceId())) ? chat->formatWorldobject(guidP->GetWorldObject(bot->GetInstanceId())) : "travel target";

        if (mapId == bot->GetMapId())
        {
            ai->Poi(x, y, name);
        }
        else
        {
            LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
            if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(location) < 20.0f)
            {
                for (auto& p : lastMove.lastPath.getPointPath())
                {
                    if (p.getMapId() == bot->GetMapId())
                        ai->Poi(p.getX(), p.getY(), name);
                }
            }
        }
    }
     
    return canMove;
}

bool MoveToTravelTargetAction::isUseful()
{
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    if (!AI_VALUE(bool, "travel target traveling") && AI_VALUE(TravelTarget*, "travel target")->GetStatus() != TravelStatus::TRAVEL_STATUS_READY)
        return false;

    if (bot->IsTaxiFlying())
        return false;

    if (MEM_AI_VALUE(WorldPosition, "current position")->LastChangeDelay() < 10)
#ifndef MANGOSBOT_ZERO
        if (bot->IsMovingIgnoreFlying())
            return false;
#else
        if (bot->IsMoving())
            return false;
#endif

    if (!AI_VALUE(bool, "can move around"))
        return false;

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

    if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT))
    {
        auto conditions = travelTarget->GetConditions();
        for (auto& cond : conditions)
        {
            if (cond == "should travel named::guild order")
                return false;
        }
    }

    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetObjectGuid()))
        if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT))
            if (!travelTarget->IsForced())
                return false;

    WorldPosition travelPos(*travelTarget->GetPosition());

    if (travelPos.isDungeon() && bot->GetGroup() && bot->GetGroup()->IsLeader(bot->GetObjectGuid()) && sTravelMgr.MapTransDistance(bot, travelPos, true) < sPlayerbotAIConfig.sightDistance && !AI_VALUE2(bool, "group and", "near leader"))
        return false;
     
    if (AI_VALUE(bool, "has available loot"))
    {
        LootObject lootObject = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);
        if (lootObject.IsLootPossible(bot))
            return false;
    }

    if (!travelTarget->IsForced())
        if (!CanFreeMoveValue::CanFreeMoveTo(ai, *travelTarget->GetPosition()))
            return false;

    return true;
}
