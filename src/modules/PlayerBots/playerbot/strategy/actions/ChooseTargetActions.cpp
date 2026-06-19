#include "playerbot/strategy/Action.h"
#include "ChooseTargetActions.h"
#include "Movement/MovementGenerator.h"
#include "AI/CreatureAI.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "PerfStats.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace
{
    int32 GetActiveQuestObjectiveEntry(PlayerbotAI* ai)
    {
        if (!ai || !ai->GetAiObjectContext())
            return 0;

        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!target || !target->GetDestination())
            return 0;

        if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_READY &&
            target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL &&
            target->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK)
            return 0;

        QuestObjectiveTravelDestination* objective = dynamic_cast<QuestObjectiveTravelDestination*>(target->GetDestination());
        if (!objective || objective->GetEntry() <= 0)
            return 0;

        return objective->GetEntry();
    }

    bool ItemNameSuggestsCreature(ItemPrototype const* proto, CreatureInfo const* creatureInfo)
    {
        if (!proto || !creatureInfo)
            return false;

        std::string itemName = proto->Name1;
        std::string creatureName = creatureInfo->name;
        std::transform(itemName.begin(), itemName.end(), itemName.begin(), ::tolower);
        std::transform(creatureName.begin(), creatureName.end(), creatureName.begin(), ::tolower);

        return !itemName.empty() && !creatureName.empty() && itemName.find(creatureName) != std::string::npos;
    }

    bool HasSuggestedCreatureDropSource(ItemPrototype const* proto, std::list<int32> const& dropEntries)
    {
        for (int32 entry : dropEntries)
            if (entry > 0 && ItemNameSuggestsCreature(proto, sObjectMgr.GetCreatureTemplate(uint32(entry))))
                return true;

        return false;
    }

    std::set<uint32> GetNeededQuestCreatureEntries(Player* bot)
    {
        std::set<uint32> entries;
        if (!bot)
            return entries;

        QuestStatusMap& questStatusMap = bot->getQuestStatusMap();
        for (auto const& [questId, questStatus] : questStatusMap)
        {
            Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
            if (!quest || !quest->IsActive() || questStatus.m_status != QUEST_STATUS_INCOMPLETE)
                continue;

            for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
            {
                if (quest->ReqCreatureOrGOCount[objective] &&
                    questStatus.m_creatureOrGOcount[objective] < quest->ReqCreatureOrGOCount[objective] &&
                    quest->ReqCreatureOrGOId[objective] > 0)
                {
                    entries.insert(uint32(quest->ReqCreatureOrGOId[objective]));
                }

                if (!quest->ReqItemCount[objective] || questStatus.m_itemcount[objective] >= quest->ReqItemCount[objective])
                    continue;

                uint32 itemId = quest->ReqItemId[objective];
                while (itemId)
                {
                    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
                    std::list<int32> dropEntries = GAI_VALUE2(std::list<int32>, "item drop list", itemId);
                    const bool hasSuggestedSource = HasSuggestedCreatureDropSource(proto, dropEntries);

                    for (int32 entry : dropEntries)
                    {
                        if (entry <= 0)
                            continue;

                        if (hasSuggestedSource && !ItemNameSuggestsCreature(proto, sObjectMgr.GetCreatureTemplate(uint32(entry))))
                            continue;

                        entries.insert(uint32(entry));
                    }

                    itemId = ItemUsageValue::ItemCreatedFrom(itemId);
                }
            }
        }

        return entries;
    }

    Unit* FindVisibleQuestObjectiveTarget(PlayerbotAI* ai, Player* bot)
    {
        if (!ai || !bot || !bot->IsInWorld() || !ai->GetAiObjectContext())
            return nullptr;

        std::set<uint32> objectiveEntries = GetNeededQuestCreatureEntries(bot);
        if (objectiveEntries.empty())
            return nullptr;

        Unit* bestActiveTarget = nullptr;
        Unit* bestFallbackTarget = nullptr;
        float bestActiveDistance = std::numeric_limits<float>::max();
        float bestFallbackDistance = std::numeric_limits<float>::max();
        const int32 activeObjectiveEntry = GetActiveQuestObjectiveEntry(ai);
        Value<std::list<ObjectGuid>>* possibleTargetsValue = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets");
        if (!possibleTargetsValue)
            return nullptr;

        std::list<ObjectGuid> possibleTargets = possibleTargetsValue->Get();
        for (ObjectGuid const& guid : possibleTargets)
        {
            if (!guid.IsCreature() || !objectiveEntries.count(guid.GetEntry()))
                continue;

            Unit* target = ai->GetUnit(guid);
            if (!target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId() || sServerFacade.UnitIsDead(target))
                continue;

            if (sServerFacade.IsFriendlyTo(bot, target))
                continue;

            if (!bot->IsWithinLOSInMap(target, true))
                continue;

            float distance = sServerFacade.GetDistance2d(bot, target);
            if (activeObjectiveEntry && int32(guid.GetEntry()) == activeObjectiveEntry)
            {
                if (distance < bestActiveDistance)
                {
                    bestActiveDistance = distance;
                    bestActiveTarget = target;
                }

                continue;
            }

            if (distance < bestFallbackDistance)
            {
                bestFallbackDistance = distance;
                bestFallbackTarget = target;
            }
        }

        if (activeObjectiveEntry)
            return bestActiveTarget;

        return bestFallbackTarget;
    }
}

bool DpsAssistAction::isUseful()
{
    if (!AttackAction::isUseful())
        return false;

    // if carry flag, do not start fight
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return false;

    return true;
}

bool AttackAnythingAction::isUseful() 
{
    if (!bot->IsInCombat() && AI_VALUE(bool, "has available loot"))
        return false;

    Unit* questObjectiveTarget = FindVisibleQuestObjectiveTarget(ai, bot);
    if (!questObjectiveTarget && !AttackAction::isUseful())
        return false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (currentTarget && currentTarget->IsInWorld() && !sServerFacade.UnitIsDead(currentTarget) &&
        !AI_VALUE2(bool, "invalid target", "current target"))
    {
        const bool engagedWithCurrent =
            bot->IsInCombat() ||
            bot->GetVictim() == currentTarget ||
            currentTarget->GetVictim() == bot;
        const ObjectGuid attackTarget = AI_VALUE(ObjectGuid, "attack target");
        const bool intendedCurrent =
            attackTarget == currentTarget->GetObjectGuid() ||
            GetTarget() == currentTarget;

        if (!engagedWithCurrent && intendedCurrent)
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "target=" << currentTarget->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, currentTarget)
                    << " victim=0 combat=0";
                sPlayerbotAIConfig.logEvent(ai, "AttackCurrentTargetRetry", std::to_string(currentTarget->GetGUIDLow()), out.str());
            }
        }
        else
        {
            return false;
        }
    }

    if (Unit* victim = bot->GetVictim())
    {
        if (victim->IsInWorld() && !sServerFacade.UnitIsDead(victim))
        {
            return false;
        }
    }

    if (!ai->AllowActivity(GRIND_ACTIVITY)) //Bot not allowed to be active
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    Unit* target = questObjectiveTarget ? questObjectiveTarget : GetTarget();

    if (!target || !ai->IsSafe(target))
        return false;

    if (ai->ContainsStrategy(STRATEGY_TYPE_HEAL) && !ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT))
        return false;

    if(!target->IsPlayer() && bot->isInFront(target,target->GetAttackDistance(bot)*1.5f, M_PI_F*0.5f) && target->CanAttackOnSight(bot) && target->GetLevel() < bot->GetLevel() + 3.0) //Attack before being attacked.
        return true;

    if (!questObjectiveTarget && AI_VALUE(bool, "travel target traveling") && CanFreeMoveValue::CanFreeMoveTo(ai, *AI_VALUE(TravelTarget*,"travel target")->GetPosition())) //Bot is traveling
        return false;

    return true;
}

bool ai::AttackAnythingAction::isPossible()
{
    return AttackAction::isPossible() && (FindVisibleQuestObjectiveTarget(ai, bot) || GetTarget());
}

bool ai::AttackAnythingAction::Execute(Event& event)
{
    if (Unit* questObjectiveTarget = FindVisibleQuestObjectiveTarget(ai, bot))
    {
        if (!ai->AllowPressureWork(PerfStats::BOT_PRESSURE_WORK_VISIBLE_OBJECTIVE_ATTACK, 1500, 5000))
            return false;

        SET_AI_VALUE(Unit*, "current target", questObjectiveTarget);
        SET_AI_VALUE(ObjectGuid, "attack target", questObjectiveTarget->GetObjectGuid());
        bot->SetSelectionGuid(questObjectiveTarget->GetObjectGuid());

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << questObjectiveTarget->GetName()
                << " entry=" << questObjectiveTarget->GetEntry()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, questObjectiveTarget)
                << " travel=" << (AI_VALUE(TravelTarget*, "travel target")->GetDestination() ? AI_VALUE(TravelTarget*, "travel target")->GetDestination()->GetTitle() : "none");
            sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveVisibleAttack", std::to_string(questObjectiveTarget->GetGUIDLow()), out.str());
        }

        return Attack(event.getOwner() ? event.getOwner() : GetMaster(), questObjectiveTarget);
    }

    if (Unit* victim = bot->GetVictim())
    {
        if (victim->IsInWorld() && !sServerFacade.UnitIsDead(victim))
        {
            context->GetValue<Unit*>("current target")->Set(victim);
            context->GetValue<ObjectGuid>("attack target")->Set(victim->GetObjectGuid());
            return true;
        }
    }

    bool result = AttackAction::Execute(event);
    Unit* grindTarget = GetTarget();

    auto tryPullDispatch = [this, &event, grindTarget]() -> bool
    {
        if (!grindTarget || !grindTarget->IsInWorld() || sServerFacade.UnitIsDead(grindTarget))
            return false;

        if (!ai->HasStrategy("pull", BotState::BOT_STATE_COMBAT))
            return false;

        PullStrategy* strategy = PullStrategy::Get(ai);
        if (!strategy || !strategy->CanDoPullAction(grindTarget))
            return false;

        Event pullEvent("attack anything", grindTarget->GetObjectGuid());
        bool didPull = ai->DoSpecificAction("pull my target", pullEvent, true);

        if (didPull && sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << grindTarget->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, grindTarget)
                << " ranged=" << (ai->IsRanged(bot) ? 1 : 0)
                << " inCombat=" << (bot->IsInCombat() ? 1 : 0)
                << " victim=" << (bot->GetVictim() ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "AttackAnythingPullDispatch", std::to_string(grindTarget->GetGUIDLow()), out.str());
        }

        return didPull;
    };

    if (result)
    {
        if (grindTarget)
        {
            std::string grindName = grindTarget->GetName();
            if (!grindName.empty())
            {
                sPlayerbotAIConfig.logEvent(ai, "AttackAnythingAction", grindName, std::to_string(grindTarget->GetEntry()));

                if (ai->HasStrategy("pull", BotState::BOT_STATE_COMBAT))
                {
                    if (PullStrategy* strategy = PullStrategy::Get(ai))
                    {
                        if (strategy->CanDoPullAction(grindTarget))
                        {
                            if (tryPullDispatch())
                            {
                                return true;
                            }
                        }
                    }
                }

                context->GetValue<ObjectGuid>("attack target")->Set(grindTarget->GetObjectGuid());
            }
        }
    }
    else if (grindTarget && ai->IsRanged(bot) && !bot->GetVictim() && !bot->IsInCombat())
    {
        if (tryPullDispatch())
            return true;
    }

    return result;
}

bool AttackEnemyPlayerAction::isUseful()
{
    return !sPlayerbotAIConfig.IsInPvpProhibitedZone(sServerFacade.GetAreaId(bot));
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    Unit* target = context->GetValue<Unit*>("enemy flag carrier")->Get();
    return target && sServerFacade.IsDistanceLessOrEqualThan(sServerFacade.GetDistance2d(bot, target), 75.0f) && (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976));
}

bool SelectNewTargetAction::Execute(Event& event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && sServerFacade.UnitIsDead(target))
    {
        // Save the dead target for later looting
        ObjectGuid guid = target->GetObjectGuid();
        if (guid)
        {
            AI_VALUE(LootObjectStack*, "available loot")->Add(guid);
        }
    }

    // Clear the target variables
    ObjectGuid attackTarget = AI_VALUE(ObjectGuid, "attack target");
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>, "possible targets no los");
    if (attackTarget && find(possible.begin(), possible.end(), attackTarget) == possible.end())
    {
        SET_AI_VALUE(ObjectGuid, "attack target", ObjectGuid());
    }

    // Save the old target and clear the current target
    if(target)
    {
        SET_AI_VALUE(Unit*, "old target", target);
        SET_AI_VALUE(Unit*, "current target", nullptr);
    }
    
    // Stop attacking
    bot->SetSelectionGuid(ObjectGuid());
    ai->InterruptSpell();
    bot->AttackStop();


    bool moreAttackers = false;
    // Check if there is any enemy targets available to attack
    if (AI_VALUE(bool, "has attackers"))
    {
        if (ai->HasStrategy("pvp", BotState::BOT_STATE_COMBAT) ||
            ai->HasStrategy("duel", BotState::BOT_STATE_COMBAT))
        {
            // Check if there is an enemy player nearby
            if (AI_VALUE(bool, "has enemy player targets"))
            {
                moreAttackers = true;
                return ai->DoSpecificAction("attack enemy player", event, true);
            }
        }

        // Let the dps/tank assist pick a target to attack
        if (ai->HasStrategy("dps assist", BotState::BOT_STATE_NON_COMBAT))
        {
            moreAttackers = true;
            return ai->DoSpecificAction("dps assist", event, true);
        }
        else if (ai->HasStrategy("tank assist", BotState::BOT_STATE_NON_COMBAT))
        {
            moreAttackers = true;
            return ai->DoSpecificAction("tank assist", event, true);
        }
    }
    
    if (!moreAttackers)
    {
        // Stop pet attacking
        Pet* pet = bot->GetPet();
        if (pet)
        {
            UnitAI* creatureAI = ((Creature*)pet)->AI();
            if (creatureAI)
            {
                // Send pet action packet
                const ObjectGuid& petGuid = pet->GetObjectGuid();
                const ObjectGuid& targetGuid = ObjectGuid();
                const uint8 flag = ACT_COMMAND;
                const uint32 spellId = COMMAND_FOLLOW;
                const uint32 command = (flag << 24) | spellId;

                WorldPacket data(CMSG_PET_ACTION);
                data << petGuid;
                data << command;
                data << targetGuid;
                bot->GetSession()->HandlePetAction(data);
            }
        }
    }

    return false;
}
