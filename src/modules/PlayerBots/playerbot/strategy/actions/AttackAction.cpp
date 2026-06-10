
#include "playerbot/playerbot.h"
#include "AttackAction.h"
#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "AI/CreatureAI.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/BotDiagnostics.h" // SC_LOG for attack-command diagnostic trace
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/generic/CombatStrategy.h"
#include <iomanip>

using namespace ai;

bool AttackAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    Unit* target = GetTarget();
    if (target && target->IsInWorld() && target->GetMapId() == bot->GetMapId())
    {
        return Attack(requester, target);
    }

    return false;
}

bool AttackAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    const bool isRangedBot = ai->IsRanged(bot);
    const bool meleeStalled = !isRangedBot &&
        bot->GetVictim() == target &&
        (!bot->CanReachWithMeleeAutoAttack(target) ||
         bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE);
    const bool alreadyEngaged = bot->GetVictim() == target && bot->IsInCombat() &&
        (isRangedBot || bot->HasUnitState(UNIT_STAT_MELEE_ATTACKING));

    return !alreadyEngaged || meleeStalled;
}

bool AttackMyTargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    SC_LOG("attack-cmd entry bot=%s requester=%s eventOwner=%s",
           bot ? bot->GetName() : "(null)",
           requester ? requester->GetName() : "(null)",
           event.getOwner() ? event.getOwner()->GetName() : "(null)");

    if(requester)
    {
        const ObjectGuid guid = requester->GetSelectionGuid();
        SC_LOG("attack-cmd selection bot=%s requester=%s selGuid=0x%016llx",
               bot ? bot->GetName() : "(null)",
               requester->GetName(),
               (unsigned long long)guid.GetRawValue());

        if (guid)
        {
            Unit* tgt = ai->GetUnit(guid);
            SC_LOG("attack-cmd target bot=%s tgt=%s tgtMap=%d botMap=%u",
                   bot ? bot->GetName() : "(null)",
                   tgt ? tgt->GetName() : "(null-unit)",
                   tgt ? (int)tgt->GetMapId() : -1,
                   bot ? bot->GetMapId() : 0);

            if (Attack(requester, tgt))
            {
                SET_AI_VALUE(ObjectGuid, "attack target", guid);
                SC_LOG("attack-cmd OK bot=%s tgt=%s",
                       bot ? bot->GetName() : "(null)",
                       tgt ? tgt->GetName() : "(null)");
                return true;
            }
            SC_LOG("attack-cmd FAIL bot=%s — Attack() returned false", bot ? bot->GetName() : "(null)");
        }
        else if (verbose)
        {
            SC_LOG("attack-cmd FAIL bot=%s — requester has no selection", bot ? bot->GetName() : "(null)");
            ai->TellError(requester, "You have no target");
        }
    }
    else
    {
        SC_LOG("attack-cmd FAIL bot=%s — no requester (event has no owner, no master)", bot ? bot->GetName() : "(null)");
    }

    return false;
}

bool AttackRTITargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        if (Attack(requester, rtiTarget))
        {
            SET_AI_VALUE(ObjectGuid, "attack target", rtiTarget->GetObjectGuid());
            return true;
        }
    }
    else
    {
        ai->TellError(requester, "I dont see my rti attack target");
    }

    return false;
}

bool AttackMyTargetAction::isUseful()
{
    return !ai->ContainsStrategy(STRATEGY_TYPE_HEAL) || ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT);
}

bool AttackRTITargetAction::isUseful()
{
    return !ai->ContainsStrategy(STRATEGY_TYPE_HEAL) || ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT);
}

bool AttackAction::Attack(Player* requester, Unit* target)
{
    auto logAttackYield = [this, target](char const* eventName, char const* reason, float distance)
    {
        if (!target || !sPlayerbotAIConfig.hasLog("bot_events.csv"))
            return;

        std::ostringstream out;
        out << "target=" << target->GetName()
            << " dist=" << std::fixed << std::setprecision(2) << distance
            << " reason=" << reason
            << " ranged=" << (ai->IsRanged(bot) ? 1 : 0)
            << " victim=" << ((bot->GetVictim() == target) ? 1 : 0);
        sPlayerbotAIConfig.logEvent(ai, eventName, std::to_string(target->GetGUIDLow()), out.str());
    };

    auto runReachAction = [this, target](char const* actionName, char const* logName, float distance)
    {
        bool moved = ai->DoSpecificAction(actionName, Event(), true);

        if (target && sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << target->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << distance
                << " action=" << actionName
                << " result=" << (moved ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, logName, std::to_string(target->GetGUIDLow()), out.str());
        }

        return moved;
    };

    auto ensureMeleeChase = [this, target]()
    {
        if (!target || ai->IsRanged(bot))
            return false;

        if (bot->CanReachWithMeleeAutoAttack(target))
            return false;

        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm)
            return false;

        Unit* chaseTarget = sServerFacade.GetChaseTarget(bot);
        if (mm->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
            chaseTarget && chaseTarget->GetObjectGuid() == target->GetObjectGuid())
        {
            return false;
        }

        mm->Clear(false, true);
        mm->MoveChase(target, ATTACK_DISTANCE, bot->GetAngle(target));

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << target->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target)
                << " movingGen=" << mm->GetCurrentMovementGeneratorType();
            sPlayerbotAIConfig.logEvent(ai, "AttackForceChase", std::to_string(target->GetGUIDLow()), out.str());
        }

        return true;
    };

    auto suspendTravelForCombat = [this]()
    {
        if (TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target"))
        {
            if (travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE &&
                travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_COOLDOWN)
            {
                travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
            }
        }

        if (AI_VALUE(GuidPosition, "rpg target"))
            RESET_AI_VALUE(GuidPosition, "rpg target");
    };

    MotionMaster &mm = *bot->GetMotionMaster();
	if (mm.GetCurrentMovementGeneratorType() == TAXI_MOTION_TYPE || (bot->IsFlying() && WorldPosition(bot).currentHeight() > 10.0f))
    {
        SC_LOG("attack-cmd FAIL bot=%s — taxi/flying", bot ? bot->GetName() : "(null)");
        if (verbose)
        {
            ai->TellPlayerNoFacing(requester, "I cannot attack in flight");
        }

        return false;
    }

    if (IsTargetValid(requester, target))
    {
        SC_LOG("attack-cmd valid-tgt bot=%s tgt=%s mounted=%d range=%.1f",
               bot ? bot->GetName() : "(null)",
               target ? target->GetName() : "(null)",
               bot ? (int)bot->IsMounted() : -1,
               target ? sServerFacade.GetDistance2d(bot, target) : -1.0f);
        if (bot->IsMounted() && (sServerFacade.GetDistance2d(bot, target) < 40.0f || bot->IsFlying()))
        {
            ai->Unmount();
            
            if (bot->IsFlying())
            {
                return true;
            }
        }

        const bool isWaitingForAttack = WaitForAttackStrategy::ShouldWait(ai);
        const bool isRangedBot = ai->IsRanged(bot);
        const bool isCasterRanged = isRangedBot && bot->getClass() != CLASS_HUNTER;
        const bool hasManaBar = AI_VALUE2(bool, "has mana", "self target");
        const uint8 manaPct = hasManaBar ? AI_VALUE2(uint8, "mana", "self target") : 0;
        const bool canFallbackToMelee = !isCasterRanged || !hasManaBar || manaPct <= sPlayerbotAIConfig.lowMana;
        const bool alreadyEngaged = bot->GetVictim() == target && bot->IsInCombat() &&
            (isRangedBot || bot->HasUnitState(UNIT_STAT_MELEE_ATTACKING));

        if (alreadyEngaged)
        {
            if (TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target"))
            {
                if (travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE &&
                    travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_COOLDOWN)
                {
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
                }
            }

            if (AI_VALUE(GuidPosition, "rpg target"))
            {
                RESET_AI_VALUE(GuidPosition, "rpg target");
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "target=" << target->GetName()
                    << " ranged=" << (isRangedBot ? 1 : 0)
                    << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target);
                sPlayerbotAIConfig.logEvent(ai, "AttackAlreadyEngaged", std::to_string(target->GetGUIDLow()), out.str());
            }

            ai->OnCombatStarted();
            if (!isRangedBot)
            {
                const float engagedDistance = sServerFacade.GetDistance2d(bot, target);
                if (!bot->CanReachWithMeleeAutoAttack(target))
                {
                    logAttackYield("AttackYieldReachMelee", "already_engaged_out_of_melee", engagedDistance);
                    return runReachAction("reach melee", "AttackReachMeleeDispatch", engagedDistance);
                }

                if (ai->CanMove() && !sServerFacade.IsInFront(bot, target, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
                    sServerFacade.SetFacingTo(bot, target);
                return false;
            }

            // Ranged bots should yield here so combat spell actions can execute on the same target.
            return false;
        }

        ObjectGuid guid = target->GetObjectGuid();
        Unit* previousCurrentTarget = AI_VALUE(Unit*, "current target");
        const bool alreadySelectedThisTarget = previousCurrentTarget == target && bot->GetSelectionGuid() == guid;
        bot->SetSelectionGuid(target->GetObjectGuid());

        Unit* oldTarget = AI_VALUE(Unit*, "current target");
        if(oldTarget)
        {
            SET_AI_VALUE(Unit*, "old target", oldTarget);
        }

        SET_AI_VALUE(Unit*, "current target", target);

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << target->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target)
                << " ranged=" << (isRangedBot ? 1 : 0)
                << " waiting=" << (isWaitingForAttack ? 1 : 0)
                << " stealthed=" << (ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT) ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "AttackCommandTrace", std::to_string(target->GetGUIDLow()), out.str());
        }

        Pet* pet = bot->GetPet();
        if (pet)
        {
            UnitAI* creatureAI = ((Creature*)pet)->AI();
            if (creatureAI)
            {
                // Don't send the pet to attack if the bot is waiting for attack
                if (!isWaitingForAttack && (!ai->HasStrategy("stay", BotState::BOT_STATE_COMBAT) || AI_VALUE2(float, "distance", "current target") < ai->GetRange("spell")))
                {
                    // Reset the pet state if no master
                    if (creatureAI->GetReactState() == REACT_PASSIVE && !ai->GetMaster())
                    {
                        creatureAI->SetReactState(REACT_DEFENSIVE);
                    }

                    // Don't send the pet to attack if set to passive
                    if (creatureAI->GetReactState() != REACT_PASSIVE)
                    {
                        creatureAI->AttackStart(target);
                    }
                }
            }
        }

        if (ai->CanMove() && !sServerFacade.IsInFront(bot, target, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
        {
            sServerFacade.SetFacingTo(bot, target);
        }

        bool result = true;
        const float targetDistance = sServerFacade.GetDistance2d(bot, target);
        const float spellRange = ai->GetRange("spell");
        const bool hasLineOfSight = bot->IsWithinLOSInMap(target, true);
        const bool yieldToCasterCombat = isCasterRanged && hasManaBar && manaPct > sPlayerbotAIConfig.lowMana &&
            hasLineOfSight && targetDistance <= spellRange;

        if (!isRangedBot && !bot->CanReachWithMeleeAutoAttack(target))
        {
            logAttackYield("AttackYieldReachMelee", "target_out_of_melee", targetDistance);
            suspendTravelForCombat();
            ai->OnCombatStarted();
            return runReachAction("reach melee", "AttackReachMeleeDispatch", targetDistance);
        }

        if (isRangedBot && (!hasLineOfSight || targetDistance > (spellRange + sPlayerbotAIConfig.contactDistance)))
        {
            logAttackYield("AttackYieldReachSpell",
                hasLineOfSight ? "target_out_of_spell_range" : "target_los_blocked",
                targetDistance);
            suspendTravelForCombat();
            ai->OnCombatStarted();
            return runReachAction("reach spell", "AttackReachSpellDispatch", targetDistance);
        }

        if (yieldToCasterCombat)
        {
            if (!isWaitingForAttack && !ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT))
            {
                suspendTravelForCombat();
                bot->SetSelectionGuid(target->GetObjectGuid());
                bot->SetTarget(target);
                bot->AttackStop(true);
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "target=" << target->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << targetDistance
                    << " mana=" << static_cast<uint32>(manaPct)
                    << " spellRange=" << std::fixed << std::setprecision(2) << spellRange
                    << " reselection=" << (alreadySelectedThisTarget ? 1 : 0)
                    << " precombatSuppressed=1"
                    << " fallbackAttack=0"
                    << " fallbackResult=0"
                    << " forcedCombat=0";
                sPlayerbotAIConfig.logEvent(ai, "RangedAttackYield", std::to_string(target->GetGUIDLow()), out.str());
            }

            ai->OnCombatStarted();
            return false;
        }

        // Don't attack target if it is waiting for attack or in stealth
        if (!ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT) && !isWaitingForAttack)
        {
            suspendTravelForCombat();
            ai->PlayAttackEmote(1);
            result = bot->Attack(target, !isRangedBot || (canFallbackToMelee && targetDistance < 5.0f));
            const bool chaseForced = ensureMeleeChase();
            SC_LOG("attack-cmd bot->Attack bot=%s tgt=%s result=%d",
                   bot ? bot->GetName() : "(null)",
                   target ? target->GetName() : "(null)",
                   (int)result);

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                    out << "target=" << target->GetName()
                    << " ranged=" << (isRangedBot ? 1 : 0)
                    << " casterRanged=" << (isCasterRanged ? 1 : 0)
                    << " hasMana=" << (hasManaBar ? 1 : 0)
                    << " mana=" << static_cast<uint32>(manaPct)
                    << " meleeFallback=" << (canFallbackToMelee ? 1 : 0)
                    << " chase=" << (chaseForced ? 1 : 0);
                sPlayerbotAIConfig.logEvent(ai, "RangedMeleeDecision", std::to_string(target->GetGUIDLow()), out.str());
            }

            result = result || chaseForced;
        }
        else
        {
            SC_LOG("attack-cmd skip bot->Attack bot=%s — stealthed=%d isWaitingForAttack=%d",
                   bot ? bot->GetName() : "(null)",
                   (int)ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT),
                   (int)isWaitingForAttack);
        }

        if (result)
        {
            // Force change combat state to have a faster reaction time
            ai->OnCombatStarted();
        }

        return result;
    }

    SC_LOG("attack-cmd FAIL bot=%s — IsTargetValid rejected", bot ? bot->GetName() : "(null)");
    return false;
}

bool AttackAction::IsTargetValid(Player* requester, Unit* target)
{
    if (!target)
    {
        if (verbose) 
        {
            ai->TellPlayerNoFacing(requester, "I have no target");
        }

        return false;
    }
    else if (sServerFacade.IsFriendlyTo(bot, target))
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is friendly to me";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }
    else if (sServerFacade.UnitIsDead(target))
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is dead";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }
    else if (sServerFacade.GetDistance2d(bot, target) > sPlayerbotAIConfig.sightDistance)
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is too far away";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }

    return true;
}

bool AttackDuelOpponentAction::isUseful()
{
    return AI_VALUE(Unit*, "duel target");
}

bool AttackDuelOpponentAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    return Attack(requester, AI_VALUE(Unit*, "duel target"));
}
