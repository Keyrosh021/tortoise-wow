#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/strategy/AiObjectContext.h"
#include "MotionMaster.h"

namespace mind
{
    // ============================ IN-COMBAT ==================================
    // The mind's combat job is INTENT, not rotation: stick to ONE target,
    // close the distance fast, and already know what happens when it dies
    // (loot -> next target). The per-class rotation, heals, kites, flees and
    // procs are class competence — the combat engine runs them unchanged the
    // moment we are in range.
    bool BotMind::CombatStep(bool minimal, bool* executed)
    {
        *executed = false;
        if (!bot->IsInWorld() || !bot->IsAlive())
            return false;
        if (bot->GetHealthPercent() < 30.0f)
            return false;                       // survival -> engine decides flee/heal
        if (!ai->CanMove())
            return false;                       // rooted/feared/stunned -> engine
        if (bot->IsNonMeleeSpellCasted(true, false, true))
            return false;                       // mid-cast -> never interrupt

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return false;

        const uint32 now = WorldTimer::getMSTime();

        Unit* target = bot->GetVictim();
        if (!target || !sServerFacade.IsAlive(target))
            target = context->GetValue<Unit*>("current target")->Get();
        if (!target || !sServerFacade.IsAlive(target))
            return false;                       // no live target -> engine acquires one

        // Pin: the rotation, the chase and the mind all agree on ONE target.
        targetGuid = target->GetObjectGuid();
        context->GetValue<Unit*>("current target")->Set(target);

        // PROACTIVE NEXT TASK (Goal 4): while this mob dies, pre-pick the one
        // we take after it — no post-kill rethink dead-air.
        if (!nextTargetGuid && now >= nextTaskScanAt)
        {
            nextTaskScanAt = now + 2500;
            ObjectGuid pinned = targetGuid;      // protect the pin during the scan
            targetGuid = ObjectGuid();
            targetScanAt = 0;
            if (Unit* next = PinnedOrBestTarget(now))
                if (next->GetObjectGuid() != pinned)
                    nextTargetGuid = next->GetObjectGuid();
            targetGuid = pinned;
        }

        // Melee vs ranged (paladins count as melee even when IsRanged()'s
        // holy heuristic says otherwise).
        const bool ranged = ai->IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
        bool inRange;
        if (ranged)
            inRange = bot->IsWithinLOSInMap(target) && bot->GetDistance(target) <= ai->GetRange("spell");
        else
            inRange = bot->CanReachWithMeleeAutoAttack(target) && bot->IsWithinLOSInMap(target);

        if (inRange)
            return false;                       // engine runs the rotation

        // STUCK-CHASE guard: no distance progress for >4s means unreachable
        // (fleeing target, bad path, LoS wall) -> drop it, take the next one.
        const float dist = bot->GetDistance(target);
        if (chaseTgt != target->GetObjectGuid())
        {
            chaseTgt = target->GetObjectGuid();
            chaseLastDist = dist;
            chaseProgressAt = now;
        }
        else if (dist < chaseLastDist - 1.0f)
        {
            chaseLastDist = dist;
            chaseProgressAt = now;
        }
        else if (now - chaseProgressAt > 4000)
        {
            chaseTgt = ObjectGuid();
            AddBlacklist(target->GetObjectGuid(), now, 30000);
            bot->AttackStop();
            targetGuid = ObjectGuid();
            context->GetValue<Unit*>("current target")->Set(nullptr);
            ai->SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);
            *executed = true;
            return true;                        // re-acquire next tick
        }

        // STICK and CHASE with the tested reach actions (persistent MoveChase).
        Log().chase.fetch_add(1, std::memory_order_relaxed);
        const bool alreadyChasing =
            bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;
        bool acted = true;
        if (!alreadyChasing)
            acted = ai->DoSpecificAction(ranged ? "reach spell" : "reach melee", Event(), true);
        ai->SetAIInternalUpdateDelay(alreadyChasing ? 300 : (sPlayerbotAIConfig.reactDelay + 100));
        if (acted)
        {
            *executed = true;
            return true;
        }
        return false;                           // reach failed -> engine tries something else
    }

    // ============================ POST-KILL LOOT ==============================
    // First thing after a kill is the corpse (Goal 4) — including corpses the
    // GROUP awarded to us on a kill someone else landed. The mind owns the
    // decision + approach commitment; the proven loot dance (open, store,
    // release, junk-slot freeing, skinning, error handling) runs through the
    // existing loot actions and the packet-reactive "store loot".
    Verdict BotMind::StepLoot(uint32 now)
    {
        Creature* corpse = LootOwed(now);
        if (!corpse)
        {
            // Mid-loot-session (window open, items streaming in): hold still.
            if (bot->GetLootGuid())
                return { true, false, 300 };
            return { false, false, 0 };
        }

        SetGoal(Goal::Loot, now);
        Log().lootRuns.fetch_add(1, std::memory_order_relaxed);

        // Give up on corpses we cannot path to instead of orbiting them.
        if (MovingBlocked(now))
        {
            AddBlacklist(corpse->GetObjectGuid(), now, 60000);
            lootGuid = ObjectGuid();
            return { true, false, 400 };
        }

        // Point the loot chain at OUR corpse and run one step of the proven
        // dance. OpenLootAction approaches when far (with the anti-orbit
        // hysteresis), sends CMSG_LOOT in range; "store loot" fires off the
        // response packet and drains money + items.
        AiObjectContext* context = ai->GetAiObjectContext();
        LootObject current = context->GetValue<LootObject>("loot target")->Get();
        if (current.guid != corpse->GetObjectGuid())
            context->GetValue<LootObject>("loot target")->Set(LootObject(bot, corpse->GetObjectGuid()));

        const bool acted = ai->DoSpecificAction("open loot", Event(), true);
        if (acted)
            Log().lootOpens.fetch_add(1, std::memory_order_relaxed);
        else
        {
            // The dance refused (rights changed, empty, locked): drop it so
            // we don't spin on a corpse the core won't give us.
            AddBlacklist(corpse->GetObjectGuid(), now, 30000);
            lootGuid = ObjectGuid();
            context->GetValue<LootObject>("loot target")->Set(LootObject());
        }
        return { true, acted, 500 };
    }

    // ============================ ENGAGE =====================================
    // A usable kill target in range of the scan -> commit and attack it. The
    // proven "attack" action starts the swing + chase; entering combat hands
    // the tick to CombatStep + the combat engine.
    Verdict BotMind::StepCombatChase(uint32 now)
    {
        Unit* target = PinnedOrBestTarget(now);
        if (!target)
            return { false, false, 0 };

        SetGoal(Goal::Combat, now);
        AiObjectContext* context = ai->GetAiObjectContext();
        context->GetValue<Unit*>("current target")->Set(target);

        const bool acted = ai->DoSpecificAction("attack", Event(), true);
        if (!acted)
        {
            // The engage refused (immune, evade, tap raced away). Blacklist
            // briefly and walk on rather than standing on an unattackable mob.
            AddBlacklist(target->GetObjectGuid(), now, 10000);
            targetGuid = ObjectGuid();
            context->GetValue<Unit*>("current target")->Set(nullptr);
            return { true, false, 300 };
        }

        Log().engage.fetch_add(1, std::memory_order_relaxed);
        ResetStuck(now);
        Commit(now, 2500);
        return { true, true, 500 };
    }
}
