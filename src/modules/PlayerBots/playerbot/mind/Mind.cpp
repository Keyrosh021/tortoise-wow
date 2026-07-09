#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/AiObjectContext.h"

namespace mind
{
    BotMind::BotMind(PlayerbotAI* ai, Player* bot) : ai(ai), bot(bot)
    {
    }

    // Proactive movement/pursuit strategies the mind replaces. Everything
    // else on the non-combat engine stays: packet features ("default"), food,
    // buffs, pets, rolls, lfg, bg queueing, guild, duels, emotes, mount —
    // those are features, not churn. The strategies below are the ones that
    // fought each other for movement control and produced the random,
    // pointless walking the mind exists to end.
    const char** BotMind::StrippedNonCombatStrategies()
    {
        static const char* names[] = {
            "wander", "follow", "quest", "loot", "gather", "travel", "tfish",
            "rpg", "grind", "attack tagged", "collision", "avoid mobs",
            "explore", nullptr
        };
        return names;
    }

    void BotMind::SetGoal(Goal g, uint32 now)
    {
        if (goal == g)
            return;
        goal = g;
        goalSince = now;
        ResetStuck(now);
    }

    uint32 BotMind::Commit(uint32 now, uint32 forMs)
    {
        commitUntil = now + forMs;
        return forMs;
    }

    bool BotMind::BusyHold()
    {
        if (!bot->IsInWorld() || !bot->IsAlive())
            return false;

        if (bot->IsTaxiFlying() || bot->IsBeingTeleported())
        {
            ai->SetAIInternalUpdateDelay(1000);
            return true;
        }
        if (bot->IsNonMeleeSpellCasted(true, false, true))
        {
            ai->SetAIInternalUpdateDelay(300);
            return true;
        }
        return false;
    }

    // The one non-combat decision per tick. Runs every non-busy tick, after
    // the slimmed engine's reactive pass. Every exit sets the update delay —
    // a bot that decided something SLEEPS until the decision can have visibly
    // progressed (this is the apm-explosion fix, kept from the FSM).
    bool BotMind::Step(bool minimal)
    {
        if (!bot->IsInWorld() || !bot->IsAlive())
            return false;

        const uint32 now = WorldTimer::getMSTime();
        FlushLogIfDue(now);
        Log().steps.fetch_add(1, std::memory_order_relaxed);

        // Plain overwrite: the mind is the tempo authority (see Mind.h).
        auto sleep = [&](uint32 ms) {
            ai->SetAIInternalUpdateDelay(std::max<uint32>(ms, sPlayerbotAIConfig.reactDelay));
        };

        if (!bot->IsStandState())
        {
            // Sitting = the engine's food/drink is doing its job. Hold still,
            // re-check on a human cadence; the engine pass keeps running so
            // packet features (invites, rolls) stay responsive while resting.
            sleep(600);
            return true;
        }

        // COMMIT WINDOW: an engage is in flight. Either we are closing on the
        // mob (fine, hold), we entered combat (state flips before next tick),
        // or we are visibly NOT moving — then the pull is unreachable: break,
        // blacklist, decide fresh.
        if (now < commitUntil)
        {
            if (goal == Goal::Combat && !bot->IsInCombat() && MovingBlocked(now))
            {
                commitUntil = 0;
                if (targetGuid)
                    AddBlacklist(targetGuid, now, 8000);
                targetGuid = ObjectGuid();
            }
            else
            {
                sleep(std::min<uint32>(commitUntil - now, 600));
                return true;
            }
        }

        Verdict v = Arbitrate(now, minimal);
        sleep(v.sleepMs);
        return v.executed;
    }

    Verdict BotMind::Arbitrate(uint32 now, bool minimal)
    {
        // Priorities are the bot's INTENT order, top-down. First executor
        // that takes the tick owns it; each holds its goal via its own
        // commitment state, so this order only decides ties.
        Verdict v = StepLoot(now);          // the kill isn't done until it's looted
        if (v.handled) return v;

        v = StepSocial(now);                // party members stick together
        if (v.handled) return v;

        v = StepErrand(now);                // committed hand-ins/pickups beat new pulls
        if (v.handled) return v;

        v = StepCombatChase(now);           // a usable mob nearby -> take it
        if (v.handled) return v;

        return StepJourney(now);            // otherwise: be somewhere better (always handles)
    }
}
