
#include "playerbot/playerbot.h"
#include "playerbot/LootObjectStack.h"
#include "ChooseTravelTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/GuildValues.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/QuestGuideMgr.h"
#include "playerbot/BotLearningMgr.h"
#include "Guild/GuildMgr.h"
#include "PerfStats.h"
#include "Timer.h"
#include <algorithm>
#include <iomanip>

using namespace ai;

namespace
{
    std::string QuestObjectiveGuidSkipQualifier(ObjectGuid guid)
    {
        return "quest objective guid skip::" + std::to_string(guid.GetRawValue());
    }

    // Zero-crash travel: run a travel-destination computation SYNCHRONOUSLY on the calling
    // (bot-AI) thread and hand back an ALREADY-READY future, so the existing valid()/
    // wait_for(0)/get() consumer is unchanged -- but NO background thread is spawned. This
    // replaces the unbounded std::async(std::launch::async)-per-travel-request that (a) spawned
    // an OS thread per request -> thread exhaustion (SIGABRT std::system_error) and (b) ran
    // sTravelMgr.GetPartitions (terrain/vmap lazy-load = not thread-safe) concurrently with bot
    // AI -> heap corruption (SIGSEGV). With map updates serialized (MapUpdate.Continents.
    // SingleThread=1) ALL bot AI -- travel included -- now runs single-threaded = zero
    // concurrency = zero concurrency crashes, by construction.
    template<typename Fn>
    FutureDestinations RunDestinationsSync(Fn&& fn)
    {
        std::promise<PartitionedTravelList> p;
        try { p.set_value(fn()); }
        catch (...) { p.set_exception(std::current_exception()); }
        return p.get_future();
    }
}

inline std::string GetTravelPurposeName(std::string purpose)
{
    if (Qualified::isValidNumberString(purpose) && TravelDestinationPurposeName.find(TravelDestinationPurpose(stoi(purpose))) != TravelDestinationPurposeName.end())
        return TravelDestinationPurposeName.at(TravelDestinationPurpose(stoi(purpose)));

    if (purpose.empty())
        return "quest";

    return purpose;
}

bool ChooseTravelTargetAction::Execute(Event& event)
{
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

    if(travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
    FutureDestinations* futureDestinations = AI_VALUE(FutureDestinations*, "future travel destinations");
    std::string futureTravelPurpose = AI_VALUE2(std::string, "manual string", "future travel purpose");
    std::string futureTravelPurposeName = GetTravelPurposeName(futureTravelPurpose);
    uint32 targetRelevance = AI_VALUE2(int, "manual int", "future travel relevance");
    const bool autonomousRandomBot = !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster();
    const bool lowLevelQuestBot = autonomousRandomBot && bot->GetLevel() <= 12 && futureTravelPurpose == "quest";

    if (!futureDestinations->valid())
    {
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
        context->ClearValues("no active travel destinations");        
        return false;
    }

    if (futureDestinations->wait_for(std::chrono::seconds(0)) == std::future_status::timeout)
        return false;

    PartitionedTravelList destinationList = futureDestinations->get();

    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);

    if (futureTravelPurpose == "quest" && sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        size_t totalPoints = 0;
        for (auto& [partition, travelPointList] : destinationList)
            totalPoints += travelPointList.size();

        std::ostringstream out;
        out << "dests=" << destinationList.size()
            << " points=" << totalPoints
            << " relevance=" << targetRelevance
            << " detail=" << AI_VALUE2(std::string, "manual string", "future travel detail");
        sPlayerbotAIConfig.logEvent(ai, "QuestAsyncResult", futureTravelPurposeName, out.str());
    }

    ai->TellDebug(ai->GetMaster(), "Got " + std::to_string(destinationList.size()) + " new destination ranges for " + futureTravelPurposeName, "debug travel");

    TravelTarget newTarget = TravelTarget(ai);

    if (futureTravelPurpose == "pvp")
        newTarget.SetForced(true);

    if (AI_VALUE2(std::string, "manual string", "future travel condition") == "should travel named::guild meeting")
    {
        newTarget.SetForced(true);
        newTarget.SetRelevance(std::max<uint32>(targetRelevance, 199u));
    }
    else if (AI_VALUE2(std::string, "manual string", "future travel condition") == "should travel named::guild order")
    {
        newTarget.SetForced(true);
        newTarget.SetRelevance(std::max<uint32>(targetRelevance, 198u));
    }
    else
    {
        newTarget.SetRelevance(targetRelevance);
    }

    std::string primaryFailureReason;
    if (!SetBestTarget(requester, &newTarget, destinationList, true, &primaryFailureReason))
    {
        const bool allowInactiveFallback =
            futureTravelPurpose != "quest" ||
            (futureTravelPurpose == "quest" && lowLevelQuestBot);
        std::string fallbackFailureReason;
        if (allowInactiveFallback && SetBestTarget(requester, &newTarget, destinationList, false, &fallbackFailureReason))
        {
            if (futureTravelPurpose == "quest")
            {
                const bool forceInactiveQuestTravel = lowLevelQuestBot;
                if (forceInactiveQuestTravel)
                    newTarget.SetForced(true);

                std::ostringstream out;
                out << "level=" << (uint32)bot->GetLevel()
                    << " purpose=" << futureTravelPurposeName
                    << " lowLevelQuestBot=" << (lowLevelQuestBot ? 1 : 0)
                    << " forcedTravel=" << (forceInactiveQuestTravel ? 1 : 0)
                    << " primaryReason=" << (primaryFailureReason.empty() ? "unknown" : primaryFailureReason);
                sPlayerbotAIConfig.logEvent(ai, "QuestInactiveFallbackUsed", futureTravelPurposeName, out.str());
            }

            setNewTarget(requester, &newTarget, travelTarget);
            return true;
        }

        size_t totalPoints = 0;
        for (auto& [partition, travelPointList] : destinationList)
            totalPoints += travelPointList.size();

        sPlayerbotAIConfig.logEvent(
            ai,
            "TravelTargetSelectFailed",
            futureTravelPurposeName,
            "dests=" + std::to_string(destinationList.size()) + " points=" + std::to_string(totalPoints) +
                " inactiveFallback=" + std::to_string(allowInactiveFallback ? 1 : 0) +
                " detail=" + AI_VALUE2(std::string, "manual string", "future travel detail") +
                " primaryReason=" + (primaryFailureReason.empty() ? "unknown" : primaryFailureReason) +
                " fallbackReason=" + (fallbackFailureReason.empty() ? "n/a" : fallbackFailureReason));

        sTravelMgr.SetNullTravelTarget(travelTarget);
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
        const bool autonomousRandomBot = !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster();
        if (lowLevelQuestBot)
        {
            // Do not strand starter-zone bots in a 5-minute null-travel penalty:
            // let them broaden their quest giver search and retry quickly.
            travelTarget->SetExpireIn(5000);
            context->ClearValues("no active travel destinations");

            std::ostringstream out;
            out << "reason=low-level-quest-retry"
                << " level=" << (uint32)bot->GetLevel()
                << " cooldownMs=" << travelTarget->GetTimeLeft();
            sPlayerbotAIConfig.logEvent(ai, "QuestTravelRetryWindow", futureTravelPurposeName, out.str());
        }
        else
        {
            if (autonomousRandomBot)
            {
                travelTarget->SetExpireIn(5000);
                context->ClearValues("no active travel destinations");

                std::ostringstream out;
                out << "purpose=" << futureTravelPurposeName
                    << " cooldownMs=" << travelTarget->GetTimeLeft()
                    << " primaryReason=" << (primaryFailureReason.empty() ? "unknown" : primaryFailureReason);
                sPlayerbotAIConfig.logEvent(ai, "NullTravelRetryWindow", futureTravelPurposeName, out.str());
            }
            SET_AI_VALUE2(bool, "no active travel destinations", futureTravelPurpose, true);
        }
        ai->TellDebug(ai->GetMaster(), "No target set", "debug travel");
        return false;
    }

    setNewTarget(requester, &newTarget, travelTarget);
    
    return true;
}

bool ChooseTravelTargetAction::isUseful()
{
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    if (AI_VALUE(bool, "travel target active"))
        return false;

    return true;
}

void ChooseTravelTargetAction::setNewTarget(Player* requester, TravelTarget* newTarget, TravelTarget* oldTarget)
{
    std::string purpose = "unknown";
    if (newTarget && newTarget->GetDestination())
    {
        auto it = TravelDestinationPurposeName.find(newTarget->GetDestination()->GetPurpose());
        if (it != TravelDestinationPurposeName.end())
            purpose = it->second;
    }

    if (CanFreeMoveValue::CanFreeMoveTo(ai, newTarget->GetPosStr()))
        ReportTravelTarget(bot, requester, newTarget, oldTarget);

    //If we are heading to a creature/npc clear it from the ignore list. 
    if (oldTarget && oldTarget == newTarget && newTarget->GetEntry())
    {
        std::set<ObjectGuid>& ignoreList = context->GetValue<std::set<ObjectGuid>&>("ignore rpg target")->Get();

        for (auto& i : ignoreList)
        {
            if (i.GetEntry() == newTarget->GetEntry())
            {
                ignoreList.erase(i);
            }
        }

        context->GetValue<std::set<ObjectGuid>&>("ignore rpg target")->Set(ignoreList);
    }

    //Actually apply the new target to the travel target used by the bot.
    oldTarget->CopyTarget(newTarget);

    const bool isNullDestination = oldTarget->GetDestination() && typeid(*oldTarget->GetDestination()) == typeid(NullTravelDestination);
    if (isNullDestination)
    {
        // Treat null travel as a real cooldown target so we do not immediately churn back
        // into request/choose/reset loops on every non-combat tick.
        oldTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);

        if (newTarget && newTarget->GetDestination())
        {
            sPlayerbotAIConfig.logEvent(
                ai,
                "TravelIdleCooldown",
                newTarget->GetDestination()->GetTitle(),
                purpose + "|" + newTarget->GetDestination()->GetShortName());
        }

        RESET_AI_VALUE(GuidPosition,"rpg target");
        RESET_AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target");
        RESET_AI_VALUE(ObjectGuid,"attack target");
        SET_AI_VALUE2(std::string, "manual string", "future travel detail", std::string());
        return;
    }

    if (oldTarget->IsForced()) //Make sure travel goes into cooldown after getting to the destination.
        oldTarget->SetExpireIn(HOUR * IN_MILLISECONDS);

    if(!AI_VALUE2(std::string, "manual string", "future travel condition").empty())
        AI_VALUE(TravelTarget*, "travel target")->SetConditions({ AI_VALUE2(std::string, "manual string", "future travel condition")});

    if (QuestObjectiveTravelDestination* dest = dynamic_cast<QuestObjectiveTravelDestination*>(oldTarget->GetDestination()))
    {
        std::string condition = "group or::{following party,need quest objective::{" + std::to_string(dest->GetQuestId()) + "," + std::to_string((uint8)dest->GetObjective()) + "}}";
        oldTarget->AddCondition(condition);
    }
    else if (QuestRelationTravelDestination* dest = dynamic_cast<QuestRelationTravelDestination*>(oldTarget->GetDestination()))
    {
        const bool skipForcedQuestRelationCondition = oldTarget->IsForced();
        if (skipForcedQuestRelationCondition)
        {
            sPlayerbotAIConfig.logEvent(
                ai,
                "QuestForcedConditionBypass",
                newTarget->GetDestination()->GetTitle(),
                "entry=" + std::to_string(dest->GetEntry()) +
                    " purpose=" + std::string(dest->GetPurpose() == TravelDestinationPurpose::QuestGiver ? "giver" : "taker"));
        }

        if (!skipForcedQuestRelationCondition)
        {
        std::string condition, qualifier = std::to_string(dest->GetEntry());
        if (dest->GetPurpose() == TravelDestinationPurpose::QuestGiver)

            condition = "group or::{following party,or::{can accept quest npc::" + qualifier + ",can accept quest low level npc::" + qualifier + "}}";
        else
            condition = "group or::{following party,can turn in quest npc::" + qualifier + "}";

        oldTarget->AddCondition(condition);
        }
    }

    oldTarget->SetStatus(TravelStatus::TRAVEL_STATUS_READY);

    if (newTarget && newTarget->GetDestination())
    {
        sPlayerbotAIConfig.logEvent(
            ai,
            "TravelTargetSelected",
            newTarget->GetDestination()->GetTitle(),
            purpose + "|" + newTarget->GetDestination()->GetShortName());

        if (AI_VALUE2(std::string, "manual string", "future travel purpose") == "quest")
        {
            std::ostringstream out;
            out << "purpose=" << purpose
                << " short=" << newTarget->GetDestination()->GetShortName()
                << " entry=" << newTarget->GetEntry()
                << " status=" << (uint32)oldTarget->GetStatus()
                << " forced=" << (oldTarget->IsForced() ? 1 : 0)
                << " relevance=" << oldTarget->GetRelevance()
                << " point=" << newTarget->GetPosStr()
                << " detail=" << AI_VALUE2(std::string, "manual string", "future travel detail");
            sPlayerbotAIConfig.logEvent(
                ai,
                "QuestTravelSelected",
                newTarget->GetDestination()->GetTitle(),
                out.str());
        }

        if (purpose == "QuestTaker")
        {
            std::ostringstream out;
            out << "entry=" << newTarget->GetEntry()
                << " retries=" << newTarget->GetRetryCount(false)
                << " status=" << (uint32)oldTarget->GetStatus()
                << " point=" << newTarget->GetPosStr();
            sPlayerbotAIConfig.logEvent(
                ai,
                "QuestTurnInTravelSelected",
                newTarget->GetDestination()->GetTitle(),
                out.str());
        }
    }

    //Clear rpg and attack/grind target. We want to travel, not hang around some more.
    RESET_AI_VALUE(GuidPosition,"rpg target");
    RESET_AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target");
    RESET_AI_VALUE(ObjectGuid,"attack target");
    RESET_AI_VALUE(bool, "travel target active");
    context->ClearValues("no active travel destinations");
    SET_AI_VALUE2(std::string, "manual string", "future travel detail", std::string());
};

//Tell the master what travel target we are moving towards.
//This should at some point be rewritten to be denser or perhaps logic moved to ->getTitle()
void ChooseTravelTargetAction::ReportTravelTarget(Player* bot, Player* requester, TravelTarget* newTarget, TravelTarget* oldTarget)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    TravelDestination* destination = newTarget->GetDestination();

    TravelDestination* oldDestination;

    if (oldTarget)
        oldDestination = oldTarget->GetDestination();

    std::ostringstream out;

    if (newTarget->IsForced())
        out << "(Forced) ";
        
    std::string futureTravelPurpose = AI_VALUE2(std::string, "manual string", "future travel purpose");
    std::string futureTravelPurposeName = GetTravelPurposeName(futureTravelPurpose);

    std::string futureTravelCondition = AI_VALUE2(std::string, "manual string", "future travel condition");
    bool isGuildMeeting = futureTravelCondition == "should travel named::guild meeting";

    std::string futureTravelDetail = AI_VALUE2(std::string, "manual string", "future travel detail");

    std::string shortName = destination->GetShortName();    

    if (typeid(*destination) == typeid(NullTravelDestination))
    {
        out.clear();
        if (!oldDestination || typeid(*oldDestination) != typeid(NullTravelDestination))
            out << "Nowhere to travel. Idling a bit.";
    }
    else
    {
        if (newTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
        {
            out << "Currently";

            if (newTarget->GetPosition() && !newTarget->GetPosition()->getAreaName().empty())
            {
                if (destination->DistanceTo(bot) < 100.0f)
                    out << " in ";
                else
                    out << " near ";

                out << newTarget->GetPosition()->getAreaName();
            }
            else
                out << " traveling";
        }
        else
        {
            if (bot->GetGroup() && !ai->IsGroupLeader() && (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT)))
                out << "I want to travel";
            else if (newTarget->IsGroupCopy() && newTarget->GetGroupmember().GetPlayer())
                out << "Taking " << newTarget->GetGroupmember().GetPlayer()->GetName();
            else if (oldDestination && oldDestination == destination)
                out << "Continuing";
            else
                out << "Traveling";

            if (newTarget->GetPosition())
            {
                out << " " << round(newTarget->Distance(bot)) << "y";
                if (!newTarget->GetPosition()->getAreaName().empty())
                    out << " to " << newTarget->GetPosition()->getAreaName();
            }
        }

        if (shortName.find("quest") == 0)
        {
            QuestTravelDestination* QuestDestination = (QuestTravelDestination*)destination;
            out << " for " << QuestDestination->QuestTravelDestination::GetTitle();
            out << " to " << QuestDestination->GetTitle();
        }
        else if (shortName == "rpg")
        {
            out << " to " << destination->GetTitle();

            if (futureTravelPurpose == "city")
                out << " to hang around in the city";
            else if (futureTravelPurpose == "tabard")
                out << " to buy a tabard";
            else if (futureTravelPurpose == "petition")
                out << " to hand in a petition";
            else
                out << " to roleplay";
        }
        else
        {
            out << " to " << destination->GetTitle();
        }
    }

    if (newTarget->GetRetryCount(false))
        out << " (retry " << newTarget->GetRetryCount(false) << "/5)";
    if (out.str().empty())
        return;

    if (!isGuildMeeting)
        ai->TellPlayerNoFacing(requester, out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_TALK, false);

    if (!futureTravelDetail.empty())
        ai->TellDebug(requester, "Farming item: " + futureTravelDetail + " from " + destination->GetTitle(), "debug travel");

    std::string message = out.str().c_str();

    if (sPlayerbotAIConfig.hasLog("travel_map.csv"))
    {
        WorldPosition botPos(bot);
        WorldPosition destPos = *newTarget->GetPosition();

        std::ostringstream out;
        out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
        out << bot->GetName() << ",";
        out << std::fixed << std::setprecision(2);

        out << std::to_string(bot->getRace()) << ",";
        out << std::to_string(bot->getClass()) << ",";
        float subLevel = ai->GetLevelFloat();

        out << subLevel << ",";

        if (!destPos)
            destPos = botPos;

        botPos.printWKT({ botPos,destPos }, out, 1);

        if (typeid(*destination) == typeid(NullTravelDestination))
            out << "0,";
        else
            out << round(newTarget->GetDestination()->DistanceTo(botPos)) << ",";

        out << "new," << "\"" << destination->GetTitle() << "\",\"" << message << "\"";

        out << "," << futureTravelPurposeName;

        sPlayerbotAIConfig.log("travel_map.csv", out.str().c_str());        
    }
}

inline std::string PrintPartion(uint32 sqPartition)
{
    uint32 prevPartition = 0;
    for (auto& partition : travelPartitions)
    {
        if (sqrt(sqPartition) == partition)
            return std::to_string(prevPartition) + "-" + std::to_string(partition);

        prevPartition = partition;
    }

    return "> " + std::to_string(prevPartition);
}

//Sets the target to the best destination.
bool ChooseTravelTargetAction::SetBestTarget(Player* requester, TravelTarget* target, PartitionedTravelList& partitionedList, bool onlyActive, std::string* failureReason)
{
    bool distanceCheck = true;
    std::unordered_map<TravelDestination*, bool> isActive;
    size_t consideredPoints = 0;
    size_t duplicateInactiveSkips = 0;
    size_t inactiveSkips = 0;
    size_t phaseMismatchSkips = 0;
    size_t bannedEntrySkips = 0;
    size_t learnedPenaltySkips = 0;
    size_t partitionSkipBreaks = 0;
    float strongestLearnedPenalty = 0.0f;
    std::string strongestLearnedReason;

    bool hasTarget = false;

    for (auto& [partition, travelPointList] : partitionedList)
    {
        ai->TellDebug(requester, "Found " + std::to_string(travelPointList.size()) + " points at range " + PrintPartion(partition), "debug travel");

        // Order candidate points nearest-first, BUT give each bot a small per-bot, per-point
        // spread bias so the fleet fans out across ALL of the objective's spawn points instead
        // of every bot piling onto the single closest one (then sitting idle when it's
        // depleted). The bias is deterministic per (bot, point) and bounded to ~SPREAD_BAND
        // yards, so bots still stay near — they just settle on DIFFERENT nearby spawn points.
        // (The objective already carries every spawn point; this is the missing distribution.)
        // Learned penalty still biases the order when available.
        {
            const bool hasLearning = sBotLearningMgr.HasData();
            const uint32 botSeed = bot ? bot->GetGUIDLow() : 0;
            auto spreadBias = [botSeed](WorldPosition* p) -> float
            {
                if (!p) return 0.0f;
                // hash the point's ~4yd cell with the bot's guid -> stable pseudo-random [0,1)
                uint64 h = ((uint64)(int32)(p->getX() * 0.25f) * 73856093ULL)
                         ^ ((uint64)(int32)(p->getY() * 0.25f) * 19349663ULL)
                         ^ ((uint64)botSeed * 0x9E3779B97F4A7C15ULL);
                h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 27;
                const float SPREAD_BAND = 90.0f;
                return (float)(h % 1000) / 1000.0f * SPREAD_BAND;
            };
            std::stable_sort(travelPointList.begin(), travelPointList.end(),
                [this, hasLearning, &spreadBias](TravelPoint const& left, TravelPoint const& right)
                {
                    float leftKey  = std::get<float>(left)  + spreadBias(std::get<WorldPosition*>(left));
                    float rightKey = std::get<float>(right) + spreadBias(std::get<WorldPosition*>(right));

                    if (hasLearning)
                    {
                        leftKey  += sBotLearningMgr.GetTravelPenalty(bot, std::get<TravelDestination*>(left),  std::get<WorldPosition*>(left))  * 500.0f;
                        rightKey += sBotLearningMgr.GetTravelPenalty(bot, std::get<TravelDestination*>(right), std::get<WorldPosition*>(right)) * 500.0f;
                    }

                    return leftKey < rightKey;
                });
        }

        for (auto& [destination, position, distance] : travelPointList)
        {
            ++consideredPoints;

            if (destination && destination->GetEntry() == 81030)
            {
                ++bannedEntrySkips;
                continue;
            }

            if (onlyActive && !target->IsForced() && isActive.find(destination) != isActive.end() && !isActive[destination])
            {
                ++duplicateInactiveSkips;
                continue;
            }

            if (QuestRelationTravelDestination* questRelationDest = dynamic_cast<QuestRelationTravelDestination*>(destination))
            {
                std::ostringstream skipQualifier;
                skipQualifier << "quest relation skip::" << questRelationDest->GetEntry() << ":" << questRelationDest->GetQuestId()
                              << ":" << static_cast<uint32>(questRelationDest->GetRelation());
                if (AI_VALUE2(time_t, "manual time", skipQualifier.str()) > time(0))
                {
                    ++duplicateInactiveSkips;
                    continue;
                }

                std::ostringstream entrySkipQualifier;
                entrySkipQualifier << "quest relation entry skip::" << questRelationDest->GetEntry() << ":"
                                   << static_cast<uint32>(questRelationDest->GetRelation());
                if (AI_VALUE2(time_t, "manual time", entrySkipQualifier.str()) > time(0))
                {
                    ++duplicateInactiveSkips;
                    continue;
                }
            }

            if (GuidPosition* guidP = dynamic_cast<GuidPosition*>(position))
            {
                if (AI_VALUE2(time_t, "manual time", QuestObjectiveGuidSkipQualifier(*guidP)) > time(0))
                {
                    ++duplicateInactiveSkips;
                    continue;
                }
            }

            if (distanceCheck) //Check if we have moved significantly after getting the destinations.
            {
                WorldPosition center(requester ? requester : bot);
                if (position->distance(center) > distance * 2 && position->distance(center) > 100)
                {
                    ai->TellDebug(requester, "We had some destinations but we moved too far since. Trying to get a new list.", "debug travel");
                    if (failureReason)
                    {
                        std::ostringstream out;
                        out << "moved-too-far considered=" << consideredPoints
                            << " inactive=" << inactiveSkips
                            << " duplicateInactive=" << duplicateInactiveSkips
                            << " phaseMismatch=" << phaseMismatchSkips
                            << " banned=" << bannedEntrySkips
                            << " learnedPenalty=" << learnedPenaltySkips
                            << " partitionSkips=" << partitionSkipBreaks;
                        *failureReason = out.str();
                    }
                    return false;
                }

                distanceCheck = false;
            }

            bool destinationActive = target->IsForced() || !onlyActive;
            if (!destinationActive)
                destinationActive = (isActive[destination] = destination->IsActive(bot, PlayerTravelInfo(bot)));

            if (destinationActive)
            {
                // (removed) the old 9% "skip to a longer partition" jump deliberately sent bots
                // to a farther target even when a good near one was found — the user observed bots
                // running past a mob in front of them to a far one (more travel time + snagging).
                // With nearest-first ordering we now commit to the closest active target.

#ifdef MANGOSBOT_TWO
                if (GuidPosition* guidP = static_cast<GuidPosition*>(position))
                {
                    if (!bot->InSamePhase(guidP->GetPhaseMask()))
                    {
                        ai->TellDebug(requester, "Not same phase: " + destination->GetTitle() + " " + std::to_string(round(destination->DistanceTo(bot))) + "y", "debug travel");
                        ++phaseMismatchSkips;
                        continue;
                    }
                }
#endif

                std::string learnedReason;
                const float learnedPenalty = sBotLearningMgr.GetTravelPenalty(bot, destination, position, &learnedReason);
                if (learnedPenalty > strongestLearnedPenalty)
                {
                    strongestLearnedPenalty = learnedPenalty;
                    strongestLearnedReason = learnedReason;
                }

                if (!target->IsForced() && learnedPenalty >= 2.5f && travelPointList.size() > 1)
                {
                    ++learnedPenaltySkips;
                    if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 25) == 1)
                    {
                        std::ostringstream out;
                        out << "penalty=" << std::fixed << std::setprecision(2) << learnedPenalty
                            << " reason=" << learnedReason
                            << " entry=" << (destination ? destination->GetEntry() : 0)
                            << " title=" << (destination ? destination->GetTitle() : "none")
                            << " point=" << (position ? position->print() : "none");
                        sPlayerbotAIConfig.logEvent(ai, "LearnedTravelPenaltySkip", destination ? destination->GetTitle() : "travel", out.str());
                    }
                    continue;
                }

                target->SetTarget(destination, position);
                hasTarget = true;
                break;
            }
            else
            {
                ai->TellDebug(requester, "Not active: " + destination->GetTitle() + " " + std::to_string((uint32)round(destination->DistanceTo(bot))) + "y", "debug travel");
                ++inactiveSkips;
            }

        }

        if (hasTarget)
            break;
    }         
     
    if(hasTarget)
        ai->TellDebug(requester, "Point at " + std::to_string(uint32(target->Distance(bot))) + "y selected.", "debug travel");
    else if (failureReason)
    {
        std::ostringstream out;
        out << "no-target considered=" << consideredPoints
            << " inactive=" << inactiveSkips
            << " duplicateInactive=" << duplicateInactiveSkips
            << " phaseMismatch=" << phaseMismatchSkips
            << " banned=" << bannedEntrySkips
            << " learnedPenalty=" << learnedPenaltySkips
            << " maxLearnedPenalty=" << std::fixed << std::setprecision(2) << strongestLearnedPenalty
            << " learnedReason=" << strongestLearnedReason
            << " partitionSkips=" << partitionSkipBreaks
            << " onlyActive=" << (onlyActive ? 1 : 0)
            << " forced=" << (target->IsForced() ? 1 : 0);
        *failureReason = out.str();
    }

    return hasTarget;
}

std::vector<std::string> split(const std::string& s, char delim);
char* strstri(const char* haystack, const char* needle);

//Find a destination based on (part of) it's name. Includes zones, ncps and mobs. Picks the closest one that matches.
DestinationList ChooseTravelTargetAction::FindDestination(PlayerTravelInfo info, std::string name, bool zones, bool npcs, bool quests, bool mobs, bool bosses, bool gather)
{
    DestinationList dests;

    //Quests
    if (quests)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::QuestGiver, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Zones
    if (zones)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::Explore, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Npcs
    if (npcs)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GenericRpg, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Mobs
    if (mobs)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::Grind, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Bosses
    if (bosses)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::Boss, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Gather
    if (gather)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GatherSkinning, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }

        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GatherMining, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }

        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GatherHerbalism, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    if (dests.empty())
        return {};

    return dests;
};

bool ChooseGroupTravelTargetAction::Execute(Event& event)
{
    std::vector<ObjectGuid> groupPlayers;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        if (ref->getSource() != bot)
        {
            groupPlayers.push_back(ref->getSource()->GetObjectGuid());
        }
    }

    std::shuffle(groupPlayers.begin(), groupPlayers.end(), *GetRandomGenerator());

    PlayerTravelInfo info(bot);

    std::vector<TravelTarget*> groupTargets;

    PartitionedTravelList travelList;

    std::unordered_map<TravelDestination*, std::vector<std::string>> conditions;
    std::unordered_map<TravelDestination*, Player*> playerDesitnations;

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    //Find targets of the group.
    for (auto& member : groupPlayers)
    {
        Player* player = sObjectMgr.GetPlayer(member);

        if (!player)
            continue;

        if (!ai->IsSafe(player))
            continue;

        if (!player->GetPlayerbotAI())
            continue;

        if (!player->GetPlayerbotAI()->GetAiObjectContext())
            continue;

        TravelTarget* groupTarget = PAI_VALUE(TravelTarget*, "travel target");

        if (groupTarget->IsGroupCopy())
            continue;

        if (!groupTarget->IsActive())
            continue;

        if (groupTarget->IsForced())
            continue;

        if (!groupTarget->GetDestination()->IsActive(player, PlayerTravelInfo(player)) || !groupTarget->IsConditionsActive())
        {
            player->GetPlayerbotAI()->TellDebug(requester,"Target is cooling down because a group member found it to be inactive.", "debug travel");
            groupTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
            continue;
        }

        groupTargets.push_back(groupTarget);        
        playerDesitnations[groupTarget->GetDestination()] = player;
        conditions[groupTarget->GetDestination()] = groupTarget->GetConditions();
    }

    std::sort(groupTargets.begin(), groupTargets.end(), [](TravelTarget* i, TravelTarget* j) {return i->GetRelevance() > j->GetRelevance(); });

    ai->TellDebug(requester, std::to_string(groupTargets.size()) + " group targets found.", "debug travel");

    for (auto& groupTarget : groupTargets)
    {
        travelList[0].push_back(TravelPoint(groupTarget->GetDestination(), groupTarget->GetPosition(), groupTarget->GetPosition()->distance(bot)));

        ai->TellDebug(requester, playerDesitnations[groupTarget->GetDestination()]->GetName() + std::string(": ") + groupTarget->GetDestination()->GetShortName() + std::string(" (") + std::to_string(groupTarget->GetRelevance()) + std::string(")"), "debug travel");
    }

    if (travelList[0].empty())
        return false;

    TravelTarget* oldTarget = AI_VALUE(TravelTarget*, "travel target");

    TravelTarget newTarget = TravelTarget(ai);

    if (!SetBestTarget(requester, &newTarget, travelList) && !SetBestTarget(requester, &newTarget, travelList, false))
        return false;
    
    newTarget.SetGroupCopy(playerDesitnations[newTarget.GetDestination()]);

    setNewTarget(requester, &newTarget, oldTarget);

    oldTarget->SetConditions(conditions[newTarget.GetDestination()]);

    return true;
}

bool ChooseGroupTravelTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!bot->GetGroup())
        return false;

    if (!ChooseTravelTargetAction::isUseful())
        return false;

    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    if (urand(0, 100) < 50)
        return false;

    return true;
}

bool RefreshTravelTargetAction::Execute(Event& event)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target)
        return false;

    TravelDestination* oldDestination = target->GetDestination();
    WorldPosition* oldPosition = target->GetPosition();

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    if (target->IsMaxRetry(false))
    {
        ai->TellDebug(requester, "Old destination was tried too many times.", "debug travel");
        return false;
    }

    if (!oldDestination || !oldPosition) //Does this target have a destination and current point?
        return false;

    if (!target->IsDestinationActive()) //Is the destination still valid?
    {
        ai->TellDebug(requester, "Old destination was no longer valid.", "debug travel");
        return false;
    }

    PlayerTravelInfo info(bot);
    
    WorldPosition* newPosition = nullptr;

    for (uint8 i = 0; i < 5; i++)
    {
        std::list<uint8> chancesToGoFar = { 10,20,90 }; //Closest map, grid, cell.
        newPosition = oldDestination->GetNextPoint(*oldPosition, chancesToGoFar);
        if (newPosition && sTravelMgr.IsLocationLevelValid(*newPosition, info))
            break;        

        newPosition = nullptr;
    }

    if (!newPosition)
    {
        ai->TellDebug(requester, "No new locations found for old destination.", "debug travel");
        return false;
    }

    SET_AI_VALUE2(bool, "manual bool", "is travel refresh", true);
    bool conditionsStillActive = AI_VALUE(TravelTarget*, "travel target")->IsConditionsActive(true);
    RESET_AI_VALUE2(bool, "manual bool", "is travel refresh");

    if (!conditionsStillActive)
        return false;

    target->SetTarget(oldDestination, newPosition);

    target->SetStatus(TravelStatus::TRAVEL_STATUS_READY);
    target->IncRetry(false);

    RESET_AI_VALUE(bool, "travel target active");    
    context->ClearValues("no active travel destinations");
    SET_AI_VALUE2(std::string, "manual string", "future travel detail", std::string());

    ai->TellDebug(requester, "Refreshed travel target", "debug travel");
    ReportTravelTarget(bot, requester, target, target);

    return false;
}

bool RefreshTravelTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!ChooseTravelTargetAction::isUseful())
        return false;

    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target)
        return false;

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    if (!WorldPosition(bot).isOverworld())
        return false;

    if (urand(1, 100) <= 10)
        return false;

    TravelDestination* destination = target->GetDestination();
    if (!destination || !destination->IsActive(bot, PlayerTravelInfo(bot)))
        return false;

    return true;
}

bool ResetTargetAction::Execute(Event& event)
{
    TravelTarget* oldTarget = AI_VALUE(TravelTarget*, "travel target");

    context->ClearValues("no active travel destinations");

    TravelTarget newTarget = TravelTarget(ai);
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    setNewTarget(requester, &newTarget, oldTarget);

    oldTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
    oldTarget->SetExpireIn(60000); //1 minute;

    ai->TellDebug(requester, "Cleared travel target fetches", "debug travel");

    return true;
}

bool ResetTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!ChooseTravelTargetAction::isUseful())
        return false;

    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    return true;
}

bool RequestTravelTargetAction::Execute(Event& event)
{
    TravelDestinationPurpose actionPurpose = TravelDestinationPurpose(stoi(getQualifier()));

    WorldPosition center = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);

    ai->TellDebug(ai->GetMaster(), "Getting new destination ranges for " + TravelDestinationPurposeName.at(actionPurpose), "debug travel");

    *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, purpose = actionPurpose]() { return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)purpose); });

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", getQualifier());
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", event.getSource());
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);

    return true;
}

bool RequestTravelTargetAction::isUseful() {
    if (bot->InBattleGround())
        return false;

    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    if (AI_VALUE(bool, "travel target active"))
        return false;

    TravelTarget* currentTarget = AI_VALUE(TravelTarget*, "travel target");
    if (currentTarget && currentTarget->GetDestination() &&
        typeid(*currentTarget->GetDestination()) == typeid(NullTravelDestination) &&
        currentTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
        return false;

    if (AI_VALUE2(bool, "no active travel destinations", (getQualifier().empty() ? "quest" : getQualifier())))
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    if (!isAllowed())
    {
        ai->TellDebug(ai->GetMaster(), "Skipped " + GetTravelPurposeName(qualifier) + " because of skip chance", "debug travel");
        return false;
    }

    return true;
}

bool RequestTravelTargetAction::isAllowed() const
{
    TravelDestinationPurpose actionPurpose = TravelDestinationPurpose(stoi(getQualifier()));

    switch (actionPurpose)
    {
    case TravelDestinationPurpose::Repair:
    case TravelDestinationPurpose::Vendor:
    case TravelDestinationPurpose::AH:
        return urand(1, 100) < 90;
    case TravelDestinationPurpose::Mail:
        if (!AI_VALUE(bool, "should get money"))
            return urand(1, 100) < 30;
        else
            return true;
    case TravelDestinationPurpose::GatherSkinning:
    case TravelDestinationPurpose::GatherMining:
    case TravelDestinationPurpose::GatherHerbalism:
    case TravelDestinationPurpose::GatherFishing:
        if (bot->GetGroup())
            return urand(1, 100) < 50;
        else
            return urand(1, 100) < 90;
    case TravelDestinationPurpose::Boss:
        return urand(1, 100) < 50;
    case TravelDestinationPurpose::Explore:
        return urand(1, 100) < 10;
    case TravelDestinationPurpose::GenericRpg:
        return urand(1, 100) < 50;
    case TravelDestinationPurpose::Grind:
        return true;
    default:
        return true;
    }
}

bool RequestNamedTravelTargetAction::Execute(Event& event)
{
    std::string travelName = getQualifier();

    WorldPosition center = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);

    ai->TellDebug(ai->GetMaster(), "Getting new destination ranges for travel " + getQualifier(), "debug travel");

    if (travelName == "pvp")
    {
        std::string WorldPvpLocation;

        //Number between 0 and 100 synced for all bots that shifts 1 every 10 minutes.
        uint32 pvpLocationNumber = ai->GetFixedBotNumber(BotTypeNumber::WORLD_PVP_LOCATION, 100, 0.1f, true);

        if (pvpLocationNumber < 20) //First 200 minutes
            WorldPvpLocation = "Tarren Mill";
        else if (pvpLocationNumber >= 20 && pvpLocationNumber < 40) //Second 200 minutes
            WorldPvpLocation = "The Barrens";
        else if (pvpLocationNumber >= 40 && pvpLocationNumber < 60) //Third 200 minutes
            WorldPvpLocation = "Silithus";
        else if (pvpLocationNumber >= 60 && pvpLocationNumber < 80) //Fourth 200 minutes
            WorldPvpLocation = "Eastern Plaguelands";
        else                                                        //Last 200 minutes
            WorldPvpLocation = "Strangletorn Vale";

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [travelInfo = PlayerTravelInfo(bot), center, WorldPvpLocation]()
            {
                PartitionedTravelList list;
                for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, WorldPvpLocation, true, false, false, false, false, false))
                {
                    std::list<uint8> chancesToGoFar = { 10,50,90 }; //Closest map, grid, cell.
                    WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);

                    if (!point)
                        continue;

                    list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                }

                return list;
            }
        );
    }
    else if (travelName == "guild meeting")
    {
        // Parse guild MOTD for the meeting time.
        // Meeting: <location> <start time> <end time>
        std::string meetingLocation;
        if (bot->GetGuildId())
        {
            Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
            if (guild)
            {
                std::string motd = guild->GetMOTD();
                auto pos = motd.find("Meeting:");
                if (pos != std::string::npos)
                {
                    std::string body = motd.substr(pos + 8);
                    body.erase(body.begin(), std::find_if(body.begin(), body.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                    std::vector<std::string> tokens;
                    { std::istringstream iss(body); std::string t; while (iss >> t) tokens.push_back(t); }
                    if (tokens.size() >= 3)
                    {
                        tokens.pop_back(); // end time
                        tokens.pop_back(); // start time
                        std::ostringstream loc;
                        for (size_t i = 0; i < tokens.size(); ++i) { if (i) loc << " "; loc << tokens[i]; }
                        meetingLocation = loc.str();
                    }
                }
            }
        }

        if (meetingLocation.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No meeting location found in guild MOTD", "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [travelInfo = PlayerTravelInfo(bot), center, meetingLocation]()
            {
                PartitionedTravelList list;
                for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, meetingLocation, true, false, false, false, false, false))
                {
                    std::list<uint8> chancesToGoFar = { 10,50,90 };
                    WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);

                    if (!point)
                        continue;

                    list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                }

                return list;
            }
        );
    }
    else if (travelName == "guild order")
    {
        GuildOrder order = AI_VALUE(GuildOrder, "guild order");

        if (!order.IsTravelOrder())
        {
            ai->TellDebug(ai->GetMaster(), "No valid guild travel order found", "debug travel");
            return false;
        }

        std::string orderTarget = order.target;

        ai->TellDebug(ai->GetMaster(), "Guild order: " + order.GetTypeName() + " " + orderTarget, "debug travel");

        if (order.type == GuildOrderType::QuestReward)
        {
            uint32 questId = order.questId;
            if (!questId)
            {
                ai->TellDebug(ai->GetMaster(), "QuestReward order has no questId", "debug travel");
                return false;
            }

            QuestStatus questStatus = bot->GetQuestStatus(questId);
            bool questComplete = false;
            bool questInProgress = false;

            if (questStatus == QUEST_STATUS_COMPLETE)
                questComplete = true;
            else if (questStatus == QUEST_STATUS_INCOMPLETE)
                questInProgress = true;

            if (!questComplete && questStatus == QUEST_STATUS_INCOMPLETE)
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (quest && bot->CanRewardQuest(quest, false))
                    questComplete = true;
            }

            std::vector<int32> objectiveEntries;
            std::vector<int32> questGiverEntries;
            std::vector<int32> questTakerEntries;

            if (questInProgress && !questComplete)
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (quest)
                {
                    for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
                    {
                        std::vector<std::string> qualifier = { std::to_string(questId), std::to_string(objective) };
                        if (!AI_VALUE2(bool, "need quest objective", Qualified::MultiQualify(qualifier, ",")))
                            continue;

                        if (quest->ReqCreatureOrGOId[objective])
                            objectiveEntries.push_back(quest->ReqCreatureOrGOId[objective]);

                        if (quest->ReqItemId[objective])
                        {
                            std::list<int32> dropList = GAI_VALUE2(std::list<int32>, "item drop list", quest->ReqItemId[objective]);
                            for (int32 entry : dropList)
                                objectiveEntries.push_back(entry);

                            std::list<int32> vendorList = GAI_VALUE2(std::list<int32>, "item vendor list", quest->ReqItemId[objective]);
                            for (int32 entry : vendorList)
                                objectiveEntries.push_back(entry);
                        }
                    }
                }
            }

            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async,
                [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, questId,
                questComplete, questInProgress, objectiveEntries]()
                {
                    PartitionedTravelList list;

                    Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                    if (!quest)
                        return list;

                    if (questComplete)
                    {
                        PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                            (uint32)TravelDestinationPurpose::QuestTaker, {}, false, 1000000.0f);
                        for (auto& [partition, points] : subList)
                        {
                            for (auto& point : points)
                            {
                                QuestTravelDestination* questDest = dynamic_cast<QuestTravelDestination*>(std::get<TravelDestination*>(point));
                                if (questDest && questDest->GetQuestId() == questId)
                                    list[partition].push_back(point);
                            }
                        }
                    }
                    else if (questInProgress && !objectiveEntries.empty())
                    {
                        uint32 allObjectiveFlags = (uint32)TravelDestinationPurpose::QuestAllObjective;
                        PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                            allObjectiveFlags, objectiveEntries, false, 1000000.0f);
                        for (auto& [partition, points] : subList)
                            list[partition].insert(list[partition].end(), points.begin(), points.end());

                        if (list.empty())
                        {
                            subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                                (uint32)TravelDestinationPurpose::Grind, objectiveEntries, false, 1000000.0f);
                            for (auto& [partition, points] : subList)
                                list[partition].insert(list[partition].end(), points.begin(), points.end());
                        }
                    }
                    else
                    {
                        PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                            (uint32)TravelDestinationPurpose::QuestGiver, {}, false, 1000000.0f);
                        for (auto& [partition, points] : subList)
                        {
                            for (auto& point : points)
                            {
                                QuestTravelDestination* questDest = dynamic_cast<QuestTravelDestination*>(std::get<TravelDestination*>(point));
                                if (questDest && questDest->GetQuestId() == questId)
                                    list[partition].push_back(point);
                            }
                        }
                    }

                    return list;
                }
            );

            SET_AI_VALUE2(std::string, "manual string", "future travel detail", orderTarget);
        }
        else if (order.type == GuildOrderType::Farm || order.type == GuildOrderType::Kill)
        {
            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [travelInfo = PlayerTravelInfo(bot), center, orderTarget, partitions = travelPartitions]()
                {
                    PartitionedTravelList list;

                    uint32 foundItemId = GuildOrderValue::FindItemByName(orderTarget);

                    if (foundItemId)
                    {
                        std::list<int32> dropEntries = GAI_VALUE2(std::list<int32>, "item drop list", foundItemId);

                        if (!dropEntries.empty())
                        {
                            std::vector<int32> gatherEntries, mobEntries;
                            for (int32 entry : dropEntries)
                            {
                                if (entry < 0)
                                    gatherEntries.push_back(entry);
                                else
                                    mobEntries.push_back(entry);
                            }

                            // Check which gathering skills the bot actually has.
                            bool hasHerbalism = travelInfo.GetCurrentSkill(SKILL_HERBALISM) > 0;
                            bool hasMining = travelInfo.GetCurrentSkill(SKILL_MINING) > 0;
                            bool hasSkinning = travelInfo.GetCurrentSkill(SKILL_SKINNING) > 0;
                            bool hasAnyGathering = hasHerbalism || hasMining || hasSkinning;

                            // Bot has a gathering skill: prioritize gather nodes.
                            if (!gatherEntries.empty() && hasAnyGathering)
                            {
                                // Only query gather purposes the bot can actually use.
                                if (hasHerbalism)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherHerbalism, gatherEntries, true);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (hasMining)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherMining, gatherEntries, true);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (hasSkinning)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherSkinning, gatherEntries, true);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                            }

                            // If entry-based gather lookup failed, try unfiltered gather by purpose
                            // (the travel manager may index nodes by their own entry, not drop-source entry).
                            if (list.empty() && hasAnyGathering && !gatherEntries.empty())
                            {
                                if (hasHerbalism)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherHerbalism);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (list.empty() && hasMining)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherMining);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (list.empty() && hasSkinning)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherSkinning);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                            }

                            // Fall back to mob drops only if no gather nodes were found or bot has no gathering skill.
                            if (list.empty() && !mobEntries.empty() && !hasAnyGathering)
                            {
                                uint32 mobPurpose = (uint32)TravelDestinationPurpose::Grind;

                                list = sTravelMgr.GetPartitions(center, partitions, travelInfo, mobPurpose, mobEntries, false);
                            }
                        }
                    }

                    // Fall back by name: if bot has gathering skills, try gather-only first.
                    if (list.empty())
                    {
                        bool hasHerbalism = travelInfo.GetCurrentSkill(SKILL_HERBALISM) > 0;
                        bool hasMining = travelInfo.GetCurrentSkill(SKILL_MINING) > 0;
                        bool hasSkinning = travelInfo.GetCurrentSkill(SKILL_SKINNING) > 0;
                        bool hasAnyGathering = hasHerbalism || hasMining || hasSkinning;

                        // Try gather nodes by name first if bot can gather.
                        if (hasAnyGathering)
                        {
                            for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, orderTarget, false, false, false, false, false, true))
                            {
                                std::list<uint8> chancesToGoFar = { 10,50,90 };
                                WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);
                                if (!point) continue;
                                list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                            }
                        }

                        // If still empty, fall back to mobs, bosses and gather nodes.
                        if (list.empty())
                        {
                            bool includeMobs = !hasAnyGathering;
                            for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, orderTarget, false, includeMobs, false, includeMobs, includeMobs, true))
                            {
                                std::list<uint8> chancesToGoFar = { 10,50,90 };
                                WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);
                                if (!point) continue;
                                list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                            }
                        }
                    }

                    return list;
                }
            );
        }
        else if (order.type == GuildOrderType::Explore)
        {
            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [travelInfo = PlayerTravelInfo(bot), center, orderTarget]()
                {
                    PartitionedTravelList list;
                    for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, orderTarget, true, false, false, false, false, false))
                    {
                        std::list<uint8> chancesToGoFar = { 10,50,90 };
                        WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);
                        if (!point) continue;
                        list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                    }

                    return list;
                }
            );
        }
        else if (order.type == GuildOrderType::AuctionHouse)
        {
            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
                {
                    PartitionedTravelList list = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GenericRpg);

                    for (auto& [partition, travelPoints] : list)
                    {
                        travelPoints.erase(std::remove_if(travelPoints.begin(), travelPoints.end(), [](TravelPoint point)
                            {
                                EntryTravelDestination* dest = (EntryTravelDestination*)std::get<TravelDestination*>(point);
                                if (!dest->GetCreatureInfo())
                                    return true;

                                if (dest->GetCreatureInfo()->NpcFlags & UNIT_NPC_FLAG_AUCTIONEER)
                                    return false;

                                return true;
                            }), travelPoints.end());
                    }
                    return list;
                });
        }
        else
        {
            return false;
        }

        SET_AI_VALUE2(std::string, "manual string", "future travel detail", orderTarget);
    }
    else if (travelName.find("trainer") == 0)
    {
        TrainerType type = TRAINER_TYPE_CLASS;

        if (travelName == "trainer mount")
            type = TRAINER_TYPE_MOUNTS;
        if (travelName == "trainer trade")
            type = TRAINER_TYPE_TRADESKILLS;
        if (travelName == "trainer pet")
            type = TRAINER_TYPE_PETS;

        std::vector<int32> trainerEntries = AI_VALUE2(std::vector <int32>, "available trainers", type);

        if (trainerEntries.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No trainer entries found for " + getQualifier(), "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [entries = trainerEntries, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::Trainer, entries, false);
            });
    }
    else if (travelName == "mount")
    {
        std::vector<int32> mountVendorEntries = AI_VALUE(std::vector <int32>, "available mount vendors");

        if (mountVendorEntries.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No vendor entries found for " + getQualifier(), "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [entries = mountVendorEntries, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::Vendor, entries, false);
            });
    }
    else if (travelName == "reagent vendor")
    {
        std::set<int32> reagentVendorEntrySet;
        std::vector<uint32> missingReagents = NeedsProfessionReagentsValue::GetMissingReagents(ai);
        for (uint32 reagentId : missingReagents)
        {
            std::list<int32> vendorEntries = GAI_VALUE2(std::list<int32>, "item vendor list", reagentId);
            for (int32 entry : vendorEntries)
                reagentVendorEntrySet.insert(entry);
        }

        std::vector<int32> reagentVendorEntries(reagentVendorEntrySet.begin(), reagentVendorEntrySet.end());

        if (reagentVendorEntries.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No reagent vendor entries found", "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [entries = reagentVendorEntries, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::Vendor, entries, false);
            });
    }
    else
    {
        uint32 useFlags;

        if (travelName == "city")
            useFlags = NPCFlags::UNIT_NPC_FLAG_BANKER | NPCFlags::UNIT_NPC_FLAG_AUCTIONEER;
        else if (travelName == "tabard")
            useFlags = NPCFlags::UNIT_NPC_FLAG_TABARDDESIGNER;
        else if (travelName == "petition")
            useFlags = NPCFlags::UNIT_NPC_FLAG_PETITIONER;


        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async, [cityFlags = useFlags, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                PartitionedTravelList list = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GenericRpg);

                for (auto& [partition, travelPoints] : list)
                {
                    travelPoints.erase(std::remove_if(travelPoints.begin(), travelPoints.end(), [cityFlags](TravelPoint point)
                        {
                            EntryTravelDestination* dest = (EntryTravelDestination*)std::get<TravelDestination*>(point);
                            if (!dest->GetCreatureInfo())
                                return true;

                            if (dest->GetCreatureInfo()->NpcFlags & cityFlags)
                                return false;

                            return true;
                        }), travelPoints.end());
                }
                return list;
            });
    }

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", getQualifier());
    SET_AI_VALUE2(std::string, "manual string", "future travel condition",
        travelName == "guild meeting" ? "should travel named::guild meeting" :
        travelName == "guild order" ? "should travel named::guild order" :
        event.getSource());
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);

    return true;
}

bool RequestNamedTravelTargetAction::isAllowed() const
{
    std::string name = getQualifier();
    if (name == "city")
    {
        if (urand(1, 100) > 10)
            return false;
        return true;
    }
    else if (name == "pvp")
    {
        if (urand(0, 4))
            return false;
        return true;
    }
    else if (name == "guild meeting")
        return true;
    else if (name == "reagent vendor")
        return true;
    else if (name == "guild order")
        return true;
    else if (name == "mount")
    {
        if (urand(1, 100) > 100)
            return false;
        return true;
    }
    else if (name.find("trainer") == 0)
    {
        if (urand(1, 100) > 100)
            return false;
        return true;
    }
    else if (name == "tabard")
        return true;
    else if (name == "petition")
        return true;

    return false;
}

bool RequestQuestTravelTargetAction::Execute(Event& event)
{
    const uint32 requestStartMs = WorldTimer::getMSTime();
    WorldPosition center = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
    const bool autonomousRandomBot = !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster();
    const time_t recentTurnInUntil = AI_VALUE2(time_t, "manual time", "recent quest turnin until");
    const int32 recentTurnInEntry = AI_VALUE2(int32, "manual int", "recent quest turnin entry");
    const int32 recentTurnInQuest = AI_VALUE2(int32, "manual int", "recent quest turnin quest");
    const bool hasRecentTurnInAnchor = recentTurnInUntil > time(nullptr) && recentTurnInEntry > 0;

    ai->TellDebug(ai->GetMaster(), "Getting new destination ranges for travel quest", "debug travel");

    auto starterQuestForRace = [](uint8 race) -> uint32
    {
        switch (race)
        {
            case RACE_HUMAN: return 783;      // A Threat Within
            case RACE_ORC:
            case RACE_TROLL: return 788;     // Cutting Teeth
            case RACE_DWARF:
            case RACE_GNOME: return 179;     // Dwarven Outfitters
            case RACE_NIGHTELF: return 458;  // The Woodland Protector
            case RACE_UNDEAD: return 364;    // The Mindless Ones
            case RACE_TAUREN: return 747;    // The Hunt Begins
            default: return 0;
        }
    };

    std::vector<std::tuple<uint32, int32, float>> turnInFetches;
    std::vector<std::tuple<uint32, int32, float>> objectiveFetches;
    std::vector<std::tuple<uint32, int32, float>> destinationFetches;
    std::vector<std::tuple<uint32, int32, float>> rescueFetches;
    QuestGuideHubIntent directorHubIntent;
    uint32 turnInQuestCount = 0;
    uint32 objectiveQuestCount = 0;
    bool hasAnyQuestHistory = false;
    bool hasActiveQuestWork = false;
    auto queuedQuestCount = [&turnInFetches, &objectiveFetches]() -> size_t
    {
        return turnInFetches.size() + objectiveFetches.size();
    };

    for (ObjectGuid guid : AI_VALUE(std::list<ObjectGuid>, "group members"))
    {
        Player* player = sObjectMgr.GetPlayer(guid);

        if (!player)
            continue;

        if (player->GetMapId() != bot->GetMapId())
            continue;

        if (!player->GetPlayerbotAI())
            continue;

        QuestStatusMap& questMap = player->getQuestStatusMap();
        if (!questMap.empty())
            hasAnyQuestHistory = true;

        bool onlyClassQuest = bot == player && !urand(0, 10);

        //Find destinations related to the active quests.
        for (auto& [questId, questStatus] : questMap)
        {
            uint32 flag = 0;
            if (questStatus.m_rewarded)
                continue;

            hasActiveQuestWork = true;

            Quest const* questTemplate = sObjectMgr.GetQuestTemplate(questId);

            if (!questTemplate)
                continue;

            // Outleveled-zone escape: don't route travel toward OBJECTIVES of trivial quests
            // (4+ levels below the bot). Any queued quest work suppresses the hub director below,
            // so bots holding leftover green quests farmed low zones forever (measured: Durotar
            // L10-19 population grew to 33, XP pinned ~500). Turn-ins are still fetched — they
            // clear the log and pay quest XP; class quests are exempt.
            const bool trivialQuest = !questTemplate->GetRequiredClasses() &&
                player->GetLevel() > player->GetQuestLevelForPlayer(questTemplate) + 4;
            if (trivialQuest && !player->CanRewardQuest(questTemplate, false))
                continue;

            if (player->CanRewardQuest(questTemplate, false))
            {
                flag = (uint32)TravelDestinationPurpose::QuestTaker;
                ++turnInQuestCount;
            }
            else
            {
                for (uint32 objective = 0; objective < 4; objective++)
                {
                    TravelDestinationPurpose purposeFlag = (TravelDestinationPurpose)(1 << (objective + 1));

                    std::vector<std::string> qualifier = { std::to_string(questId), std::to_string(objective) };

                    if (AI_VALUE2(bool, "group or", "following party,need quest objective::" + Qualified::MultiQualify(qualifier, ","))) //Noone needs the quest objective.
                        flag = flag | (uint32)purposeFlag;
                }

                if (flag)
                    ++objectiveQuestCount;
            }

            if (!flag)
                continue;

            float questWorkRange = 1000.0f + (bot->GetLevel() * bot->GetLevel()) * 75.0f;
            if (autonomousRandomBot && bot->GetLevel() <= 12)
                questWorkRange = std::max(questWorkRange, 4200.0f + bot->GetLevel() * 320.0f);
            else if (autonomousRandomBot && bot->GetLevel() <= 18)
                questWorkRange = std::max(questWorkRange, 3000.0f + bot->GetLevel() * 220.0f);

            if (flag == (uint32)TravelDestinationPurpose::QuestTaker)
                turnInFetches.push_back({ flag, questId, questWorkRange });
            else
                objectiveFetches.push_back({ flag, questId, questWorkRange });

            if (onlyClassQuest && queuedQuestCount() > 1) //Only do class quests if we have any.
            {
                Quest const* firstQuest = nullptr;
                if (!turnInFetches.empty())
                    firstQuest = sObjectMgr.GetQuestTemplate(std::get<1>(turnInFetches.front()));
                else if (!objectiveFetches.empty())
                    firstQuest = sObjectMgr.GetQuestTemplate(std::get<1>(objectiveFetches.front()));

                if (!firstQuest)
                    continue;

                if (firstQuest->GetRequiredClasses() && !questTemplate->GetRequiredClasses())
                    continue;

                if (!firstQuest->GetRequiredClasses() && questTemplate->GetRequiredClasses())
                {
                    turnInFetches.clear();
                    objectiveFetches.clear();
                }
            }
        }
    }

    uint32 starterQuest = starterQuestForRace(bot->getRace());
    bool starterQuestSeeded = false;
    const bool starterQuestAvailable = starterQuest &&
        bot->GetQuestStatus(starterQuest) == QUEST_STATUS_NONE &&
        !bot->GetQuestRewardStatus(starterQuest);

    if (bot->GetLevel() <= 8 && starterQuestAvailable && !hasActiveQuestWork)
    {
        destinationFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, (int32)starterQuest, 1200.0f });
        starterQuestSeeded = true;

        std::ostringstream out;
        out << "race=" << (uint32)bot->getRace()
            << " level=" << (uint32)bot->GetLevel()
            << " questId=" << starterQuest
            << " reason=" << (hasAnyQuestHistory ? "stalled-low-level-chain" : "empty-quest-log");
        sPlayerbotAIConfig.logEvent(ai, "StarterQuestTravelSeed", "QuestFetches", out.str());
    }

    float genericQuestGiverRange = 400.0f + bot->GetLevel() * 10.0f;
    if (autonomousRandomBot && bot->GetLevel() <= 12 && !hasActiveQuestWork)
    {
        // Low-level random bots need a much larger quest-giver search radius or
        // they never "see" the next hub beyond the starter cluster.
        genericQuestGiverRange = std::max(genericQuestGiverRange, 2200.0f + bot->GetLevel() * 120.0f);
    }
    else if (autonomousRandomBot && bot->GetLevel() <= 18 && !turnInQuestCount && !objectiveQuestCount)
    {
        genericQuestGiverRange = std::max(genericQuestGiverRange, 1600.0f + bot->GetLevel() * 80.0f);
    }

    if (hasRecentTurnInAnchor)
        genericQuestGiverRange = std::max(genericQuestGiverRange, 900.0f);

    const bool hasQueuedQuestWork = turnInQuestCount || objectiveQuestCount;

    const size_t objectiveFetchesBeforeBudget = objectiveFetches.size();
    const size_t objectiveFetchBudget =
        autonomousRandomBot && bot->GetLevel() <= 12 ? 8 :
        autonomousRandomBot && bot->GetLevel() <= 30 ? 10 : 12;

    if (objectiveFetches.size() > objectiveFetchBudget)
    {
        const uint32 botLevel = bot->GetLevel();
        const uint32 botZone = bot->GetZoneId();
        const uint32 botArea = bot->GetAreaId();
        Player* botPlayer = bot;

        std::stable_sort(objectiveFetches.begin(), objectiveFetches.end(),
            [botPlayer, botLevel, botZone, botArea](std::tuple<uint32, int32, float> const& left,
                                                    std::tuple<uint32, int32, float> const& right)
            {
                auto scoreQuest = [botPlayer, botLevel, botZone, botArea](std::tuple<uint32, int32, float> const& fetch) -> int32
                {
                    Quest const* quest = sObjectMgr.GetQuestTemplate(std::get<1>(fetch));
                    if (!quest)
                        return -100000;

                    int32 score = 0;
                    const int32 questLevel = botPlayer->GetQuestLevelForPlayer(quest);
                    const int32 levelGap = std::abs((int32)botLevel - questLevel);
                    score -= levelGap * 8;

                    if (quest->GetRequiredClasses())
                        score += 80;

                    const int32 zoneOrSort = quest->GetZoneOrSort();
                    if (zoneOrSort > 0)
                    {
                        if ((uint32)zoneOrSort == botZone)
                            score += 120;
                        else if ((uint32)zoneOrSort == botArea)
                            score += 60;
                    }

                    if (quest->GetMinLevel() && botLevel >= quest->GetMinLevel())
                        score += 25;

                    // A small stable wobble keeps same-level bots from converging on
                    // identical quest choices while preserving deterministic behavior.
                    score += (int32)((botPlayer->GetGUIDLow() ^ quest->GetQuestId()) % 17);

                    return score;
                };

                const int32 leftScore = scoreQuest(left);
                const int32 rightScore = scoreQuest(right);
                if (leftScore != rightScore)
                    return leftScore > rightScore;

                return std::get<1>(left) < std::get<1>(right);
            });

        objectiveFetches.resize(objectiveFetchBudget);
    }

    if (objectiveFetchesBeforeBudget > objectiveFetches.size() && sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "before=" << objectiveFetchesBeforeBudget
            << " after=" << objectiveFetches.size()
            << " budget=" << objectiveFetchBudget
            << " level=" << (uint32)bot->GetLevel()
            << " zone=" << bot->GetZoneId()
            << " area=" << bot->GetAreaId();
        sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveFetchBudget", "QuestFetches", out.str());
    }

    if (turnInQuestCount)
        destinationFetches.insert(destinationFetches.end(), turnInFetches.begin(), turnInFetches.end());

    if (!objectiveFetches.empty())
        destinationFetches.insert(destinationFetches.end(), objectiveFetches.begin(), objectiveFetches.end());

    // Only broaden into generic quest-giver discovery when the bot has no concrete
    // turn-in or objective work queued. This prevents starter-zone bots from
    // re-centering on nearby questgivers while they should be finishing and
    // advancing their current chain toward the next hub.
    if (!starterQuestSeeded && !hasQueuedQuestWork && hasRecentTurnInAnchor)
    {
        destinationFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, recentTurnInEntry, std::max(genericQuestGiverRange, 450.0f) });

        std::ostringstream out;
        out << "entry=" << recentTurnInEntry
            << " questId=" << recentTurnInQuest
            << " range=" << std::fixed << std::setprecision(1) << std::max(genericQuestGiverRange, 450.0f);
        sPlayerbotAIConfig.logEvent(ai, "RecentQuestTurnInAnchor", "QuestFetches", out.str());
    }

    bool hasGuideQuestCandidates = false;
    if (!starterQuestSeeded && !hasQueuedQuestWork)
    {
        std::string directorTrace;
        directorHubIntent = sQuestGuideMgr.SelectNextHub(bot, &directorTrace);
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "valid=" << (directorHubIntent.valid ? 1 : 0)
                << " hubId=" << directorHubIntent.hubId
                << " hub=" << directorHubIntent.hubName
                << " score=" << directorHubIntent.score
                << " reason=" << directorHubIntent.reason
                << " pickups=" << directorHubIntent.pickupCandidates
                << " active=" << directorHubIntent.activeQuests
                << " rewarded=" << directorHubIntent.rewardedQuests
                << " required=" << directorHubIntent.requiredQuests
                << " rewardedRequired=" << directorHubIntent.rewardedRequiredQuests
                << " crossRace=" << (directorHubIntent.crossRace ? 1 : 0)
                << " nativeMask=" << directorHubIntent.nativeRouteMask
                << " preferredMask=" << directorHubIntent.preferredRouteMask
                << " area=" << directorHubIntent.areaId
                << " next=" << directorHubIntent.nextHubIds
                << " " << directorTrace;
            sPlayerbotAIConfig.logEvent(ai, "QuestDirectorHubSelected", "QuestDirector", out.str());
        }

        std::string guideTrace;
        std::vector<uint32> guideQuestIds;
        if (directorHubIntent.valid)
            guideQuestIds = sQuestGuideMgr.GetQuestGiverQuestIdsForHub(bot, directorHubIntent.hubId, 8, &guideTrace);

        bool usedDirectorHub = !guideQuestIds.empty();
        if (guideQuestIds.empty())
            guideQuestIds = sQuestGuideMgr.GetQuestGiverQuestIds(bot, 8, &guideTrace);

        for (uint32 questId : guideQuestIds)
        {
            destinationFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, (int32)questId, std::max(genericQuestGiverRange, 1200.0f) });
            hasGuideQuestCandidates = true;
        }

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "count=" << guideQuestIds.size()
                << " range=" << std::fixed << std::setprecision(1) << std::max(genericQuestGiverRange, 1200.0f)
                << " fallback=" << (hasGuideQuestCandidates ? 0 : 1)
                << " directorHub=" << (usedDirectorHub ? 1 : 0)
                << " hubId=" << directorHubIntent.hubId
                << " " << guideTrace;
            sPlayerbotAIConfig.logEvent(ai, "QuestGuideCandidates", "QuestFetches", out.str());
        }
    }

    const bool sparseActiveQuestWork =
        hasQueuedQuestWork &&
        !turnInQuestCount &&
        objectiveFetches.size() <= 2;
    const bool shouldPrepareQuestRescue =
        autonomousRandomBot &&
        bot->GetLevel() <= 12 &&
        (sparseActiveQuestWork || hasRecentTurnInAnchor);

    bool hasRescueQuestCandidates = false;
    if (shouldPrepareQuestRescue)
    {
        std::string guideTrace;
        std::vector<uint32> guideQuestIds = sQuestGuideMgr.GetQuestGiverQuestIds(bot, 4, &guideTrace);
        for (uint32 questId : guideQuestIds)
        {
            rescueFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, (int32)questId, std::max(genericQuestGiverRange, 1200.0f) });
            hasRescueQuestCandidates = true;
        }

        if (hasRecentTurnInAnchor)
        {
            rescueFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, recentTurnInEntry, std::max(genericQuestGiverRange, 450.0f) });
            hasRescueQuestCandidates = true;
        }

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "count=" << rescueFetches.size()
                << " activeFetches=" << destinationFetches.size()
                << " activeObjectives=" << objectiveQuestCount
                << " activeTurnIns=" << turnInQuestCount
                << " sparseActiveWork=" << (sparseActiveQuestWork ? 1 : 0)
                << " recentTurnInAnchor=" << (hasRecentTurnInAnchor ? 1 : 0)
                << " range=" << std::fixed << std::setprecision(1) << std::max(genericQuestGiverRange, 1200.0f)
                << " " << guideTrace;
            sPlayerbotAIConfig.logEvent(ai, "QuestRescueFetchPlan", "QuestFetches", out.str());
        }
    }

    if (!starterQuestSeeded && !hasQueuedQuestWork)
    {
        if (!hasGuideQuestCandidates)
            destinationFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, 0, genericQuestGiverRange });
    }
    else if (starterQuestSeeded)
        destinationFetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, 0, std::min(genericQuestGiverRange, 1200.0f) });

    {
        std::ostringstream out;
        out << "total=" << destinationFetches.size()
            << " turnIns=" << turnInQuestCount
            << " objectives=" << objectiveQuestCount
            << " baseQuestGiver=1"
            << " genericRange=" << std::fixed << std::setprecision(1) << genericQuestGiverRange
            << " recentTurnInAnchor=" << (hasRecentTurnInAnchor ? 1 : 0)
            << " hasHistory=" << (hasAnyQuestHistory ? 1 : 0)
            << " hasActiveQuestWork=" << (hasActiveQuestWork ? 1 : 0)
            << " guideCandidates=" << (hasGuideQuestCandidates ? 1 : 0)
            << " rescueCandidates=" << (hasRescueQuestCandidates ? 1 : 0)
            << " autonomous=" << (autonomousRandomBot ? 1 : 0);
        sPlayerbotAIConfig.logEvent(ai, "RequestQuestTravelTargetAction", "QuestFetches", out.str());
    }

    const bool suppressBroadQuestgiverFallback =
        hasQueuedQuestWork ||
        (autonomousRandomBot && bot->GetLevel() <= 12 && !hasQueuedQuestWork && starterQuest != 0 && !starterQuestSeeded);
    const bool hasDirectorHubFallback =
        directorHubIntent.valid &&
        !starterQuestSeeded &&
        !hasQueuedQuestWork &&
        (directorHubIntent.x != 0.0f || directorHubIntent.y != 0.0f || directorHubIntent.z != 0.0f);
    WorldPosition directorHubPosition(directorHubIntent.mapId, directorHubIntent.x, directorHubIntent.y, directorHubIntent.z);

    {
        uint32 strictFetches = destinationFetches.size();
        uint32 softFetches = destinationFetches.size();
        uint32 broadFallbackFetches = suppressBroadQuestgiverFallback ? 0 : 1;

        std::ostringstream out;
        out << "fetches=" << destinationFetches.size()
            << " strictFetches=" << strictFetches
            << " softFallbackFetches=" << softFetches
            << " broadFallbackFetches=" << broadFallbackFetches
            << " suppressBroadFallback=" << (suppressBroadQuestgiverFallback ? 1 : 0)
            << " level=" << (uint32)bot->GetLevel()
            << " activeQuestWork=" << (hasActiveQuestWork ? 1 : 0)
            << " queuedQuestWork=" << (hasQueuedQuestWork ? 1 : 0)
            << " starterSeeded=" << (starterQuestSeeded ? 1 : 0)
            << " rescueEligible=" << (shouldPrepareQuestRescue ? 1 : 0)
            << " rescueFetches=" << rescueFetches.size()
            << " directorHubFallback=" << (hasDirectorHubFallback ? 1 : 0)
            << " directorHubId=" << directorHubIntent.hubId;
        sPlayerbotAIConfig.logEvent(ai, "QuestFetchPlan", "QuestFetches", out.str());
    }

    PerfStats::RecordTravelQuestRequest(WorldTimer::getMSTimeDiffToNow(requestStartMs), destinationFetches.size());

    *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async(std::launch::async,
        [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, destinationFetches, rescueFetches, suppressBroadQuestgiverFallback, hasDirectorHubFallback, directorHubPosition]()
        {
            const uint32 asyncStartMs = WorldTimer::getMSTime();
            uint32 strictCalls = 0;
            uint32 softCalls = 0;
            uint32 broadCalls = 0;
            PartitionedTravelList list;
            for (auto [purpose, questId, range] : destinationFetches)
            {
                std::vector<int32> entries;
                if (questId)
                    entries.push_back(questId);

                ++strictCalls;
                PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo, purpose, entries, true, range);

                for (auto& [partition, points] : subList)
                    list[partition].insert(list[partition].end(), points.begin(), points.end());
            }

            if (list.empty())
            {
                // If the strict "possible only" pass finds nothing useful, fall back to a softer
                // quest search before parking the bot in long null-travel cooldown. This keeps
                // autonomous questers moving instead of idling for minutes at a time.
                for (auto [purpose, questId, range] : destinationFetches)
                {
                    std::vector<int32> entries;
                    if (questId)
                        entries.push_back(questId);

                    ++softCalls;
                    PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo, purpose, entries, false, range);

                    for (auto& [partition, points] : subList)
                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                }
            }

            if (list.empty())
            {
                for (auto [purpose, questId, range] : rescueFetches)
                {
                    std::vector<int32> entries;
                    if (questId)
                        entries.push_back(questId);

                    ++softCalls;
                    PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo, purpose, entries, false, range);

                    for (auto& [partition, points] : subList)
                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                }
            }

            if (list.empty() && hasDirectorHubFallback && directorHubPosition.getMapId() == travelInfo.GetPosition().getMapId())
            {
                TemporaryTravelDestination* destination = new TemporaryTravelDestination(directorHubPosition);
                list[0].push_back(TravelPoint(destination, destination->GetPosition(), destination->GetPosition()->distance(center)));
            }

            if (list.empty())
            {
                if (!suppressBroadQuestgiverFallback)
                {
                    ++broadCalls;
                    list = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::QuestGiver, {}, false);
                }
            }

            uint32 resultPoints = 0;
            for (auto const& [partition, points] : list)
                resultPoints += points.size();

            PerfStats::RecordTravelQuestPlanCalls(strictCalls, softCalls, broadCalls);
            PerfStats::RecordTravelQuestAsyncJob(WorldTimer::getMSTimeDiffToNow(asyncStartMs), resultPoints == 0, resultPoints);

            return list;
        }
    );

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", "quest");
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", event.getSource());
    {
        std::ostringstream out;
        out << "fetches=" << destinationFetches.size()
            << " rescueFetches=" << rescueFetches.size()
            << " activeQuestWork=" << (hasActiveQuestWork ? 1 : 0)
            << " queuedQuestWork=" << (hasQueuedQuestWork ? 1 : 0)
            << " guideCandidates=" << (hasGuideQuestCandidates ? 1 : 0)
            << " rescueCandidates=" << (hasRescueQuestCandidates ? 1 : 0)
            << " rescueEligible=" << (shouldPrepareQuestRescue ? 1 : 0)
            << " starterSeeded=" << (starterQuestSeeded ? 1 : 0)
            << " directorHubFallback=" << (hasDirectorHubFallback ? 1 : 0)
            << " directorHubId=" << directorHubIntent.hubId;
        SET_AI_VALUE2(std::string, "manual string", "future travel detail", out.str());
    }
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);

    return true;
}

bool RequestQuestTravelTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    if (travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    if (travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN &&
        dynamic_cast<NullTravelDestination*>(travelTarget->GetDestination()))
        return false;

    if (travelTarget->GetDestination() &&
        dynamic_cast<NullTravelDestination*>(travelTarget->GetDestination()) &&
        travelTarget->GetTimeLeft() > 0)
        return false;

    if (AI_VALUE(bool, "travel target active"))
        return false;

    if (AI_VALUE2(bool, "no active travel destinations", "quest"))
    {
        const bool autonomousRandomBot = !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster();
        if (!(autonomousRandomBot && bot->GetLevel() <= 12))
            return false;
    }

    if (!AI_VALUE(bool, "can move around"))
        return false;

    if (!ai->AllowExpensivePlanner(2500, 900))
        return false;

    if (!isAllowed())
    {
        ai->TellDebug(ai->GetMaster(), "Skipped quest because of skip chance", "debug travel");
        return false;
    }

    return true;
}

bool RequestQuestTravelTargetAction::isAllowed() const
{
    if (AI_VALUE(bool, "should get money"))
        return urand(1, 100) < 90;
    else
        return urand(1, 100) < 95;

    return false;
}

bool FocusTravelTargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();

    if (text == "?")
    {
        std::set<uint32> questIds = AI_VALUE(focusQuestTravelList, "focus travel target");
        std::ostringstream out;
        if (questIds.empty())
            out << "No quests selected.";
        else
        {
            out << "I will try to only do the following " << questIds.size() << " quests:";

            for (auto questId : questIds)
            {
                const Quest* quest = sObjectMgr.GetQuestTemplate(questId);

                if (quest)
                    out << ChatHelper::formatQuest(quest);
            }

        }
        ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        return true;
    }

    std::set<uint32> questIds = ChatHelper::ExtractAllQuestIds(text);

    if (questIds.empty() && !text.empty())
    {
        if (Qualified::isValidNumberString(text))
            questIds.insert(stoi(text));
        else
        {
            std::vector<std::string> qualifiers = Qualified::getMultiQualifiers(text, ",");

            for (auto& qualifier : qualifiers)
                if (Qualified::isValidNumberString(qualifier))
                    questIds.insert(stoi(text));
        }
    }

    SET_AI_VALUE(focusQuestTravelList, "focus travel target", questIds);

    if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT))
        ai->TellError(requester, "travel strategy disabled bot needs this to actually do the quest.");

    if (!ai->HasStrategy("rpg quest", BotState::BOT_STATE_NON_COMBAT))
        ai->TellError(requester, "rpg quest strategy disabled bot needs this to actually do the quest.");

    std::ostringstream out;
    if (questIds.empty())
        out << "I will now do all quests.";
    else
    {
        out << "I will now only try to do the following " << questIds.size() << " quests:";

        for (auto questId : questIds)
        {
            const Quest* quest = sObjectMgr.GetQuestTemplate(questId);

            if (quest)
                out << ChatHelper::formatQuest(quest);
        }

    }
    ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

    TravelTarget* oldTarget = AI_VALUE(TravelTarget*, "travel target");

    oldTarget->SetExpireIn(1000);
    
    return true;
}
