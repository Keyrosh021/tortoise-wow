#include "playerbot/strategy/Action.h"
#include "ChooseTargetActions.h"
#include <atomic>

// Target-churn counters (defined at global scope in PlayerbotAI.cpp). Declared here at GLOBAL
// file scope so the out-of-line ai::SelectNewTargetAction::Execute body (which lives in namespace
// ai) references the GLOBAL symbols via :: rather than declaring ai::-scoped ones (link error).
extern std::atomic<uint64_t> g_botSelectNewTarget;
extern std::atomic<uint64_t> g_botSelectNewTargetAlive;
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

        // Threat-aware quest-objective selection (autonomous bots): prefer the
        // LEAST-packed instance of the objective mob so the bot peels a kill off
        // the edge of a camp instead of beelining into the middle of a pack and
        // dying (the "Hogger / Kobold Tunneler deep in a gnoll pack" case).
        const bool packAwareObjective =
            !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster() &&
            possibleTargets.size() <= 40;
        std::vector<std::pair<float, float>> objHostilePositions;
        if (packAwareObjective)
        {
            objHostilePositions.reserve(possibleTargets.size());
            for (ObjectGuid const& hg : possibleTargets)
                if (Unit* hu = ai->GetUnit(hg))
                    objHostilePositions.emplace_back(hu->GetPositionX(), hu->GetPositionY());
        }

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

    // Don't START a new fight badly hurt. Bots revived at ~50% HP were instantly engaging fresh
    // 100% mobs ("attack anything" rel 5.0 beats "food" rel 3.0) and losing the race -- watched
    // Thuskey die 16x in 13 min re-engaging at 52% HP every revive. A real player eats first.
    // Only gates STARTING fights; being attacked / already in combat is unaffected.
    if (!ai->HasRealPlayerMaster() && !bot->IsInCombat() && bot->getAttackers().empty() &&
        bot->GetHealthPercent() < 55.0f)
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

    // Distance-aware traveling veto: the blanket veto discarded the grind value's sanctioned
    // en-route mob (45y opportunisticPathKill) one layer up, so TRAVEL-status bots never fought
    // anything (measured 164k bot-seconds stalled with a live target <=40y). A path-local kill is
    // firm: GrindTargetValue already rejects far non-quest mobs for travelers -- kill, resume.
    if (!questObjectiveTarget && AI_VALUE(bool, "travel target traveling") &&
        CanFreeMoveValue::CanFreeMoveTo(ai, *AI_VALUE(TravelTarget*,"travel target")->GetPosition()) &&
        (!target || sServerFacade.GetDistance2d(bot, target) > 45.0f)) //Bot is traveling with no path-local kill
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

    // HEAL-CAST GUARD: this action calls ai->InterruptSpell() + AttackStop() below, which CANCELS
    // an in-flight heal -> the heal-recast-death loop (a bot at low HP would tear down its target,
    // cancel its life-saving heal, recast, repeat, die). If the bot is mid-cast on a HEAL, do NOT
    // run: keep the target, let the heal land. (Belt-and-suspenders vs. the combat-preempt delay
    // hold -- covers the reaction-engine path too.) See [[bot-heal-recast-thrash]].
    if (bot->IsNonMeleeSpellCasted(true, false, true))
    {
        if (Spell* gcast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (gcast->m_spellInfo && PlayerbotAI::IsHealSpell(gcast->m_spellInfo))
                return false;
    }

    // SURVIVAL FALLBACK -- the single biggest killer. bot-cam death analysis: 46% of bots that die
    // do so in the "idle select-new-target loop" -- a bot with LIVE attackers but an invalid current
    // target spins this action doing ZERO damage and bleeds out (lvl-12 Ilikluc: 47 ticks of it with
    // 2 mobs on it -> dead). The assist/teardown logic below does NOT guarantee re-engagement: group
    // assist can return true without yielding a fightable target, and the solo path picks nothing.
    // So BEFORE any of that: if we are in combat and our current target is invalid but something
    // alive is attacking us, COMMIT to the nearest such attacker, START the attack, and return FALSE
    // so the engine runs the attack rotation on the now-valid target THIS SAME tick. (Returning true
    // here would consume the tick and block the attack -- that was an earlier regression.) Applies to
    // ALL bots (grouped included) -- fighting back when attacked is never wrong. A valid current
    // target is left untouched, so this never fights the normal assist/commit behaviour.
    if (bot->IsInCombat() &&
        (!target || !sServerFacade.IsAlive(target) ||
         !bot->IsWithinDistInMap(target, sPlayerbotAIConfig.sightDistance)))
    {
        Unit* engage = bot->GetVictim();
        if (!engage || !sServerFacade.IsAlive(engage) || sServerFacade.IsFriendlyTo(bot, engage) ||
            !bot->IsWithinDistInMap(engage, sPlayerbotAIConfig.sightDistance))
            engage = nullptr;
        if (!engage)
        {
            float best = 100000.0f;
            for (const ObjectGuid& g : AI_VALUE(std::list<ObjectGuid>, "attackers"))
            {
                Unit* atk = ai->GetUnit(g);
                if (!atk || !sServerFacade.IsAlive(atk) || sServerFacade.IsFriendlyTo(bot, atk))
                    continue;
                if (!bot->IsWithinDistInMap(atk, sPlayerbotAIConfig.sightDistance))
                    continue;
                float d = sServerFacade.GetDistance2d(bot, atk);
                if (d < best) { best = d; engage = atk; }
            }
        }
        if (engage)
        {
            SET_AI_VALUE(Unit*, "current target", engage);
            bot->SetSelectionGuid(engage->GetObjectGuid());
            bot->Attack(engage, !ai->IsRanged(bot));   // start engaging now (melee swing for melee classes)
            return false;                              // let the rotation act on it THIS tick
        }
    }

    // TARGET-CHURN instrumentation: count target teardown+re-pick, and whether the old target was
    // still alive (= switching off a live mob = potential oscillation).
    {
        ::g_botSelectNewTarget.fetch_add(1, std::memory_order_relaxed);
        if (target && !sServerFacade.UnitIsDead(target))
            ::g_botSelectNewTargetAlive.fetch_add(1, std::memory_order_relaxed);
    }

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
