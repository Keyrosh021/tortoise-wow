
#include "playerbot/playerbot.h"
#include "InvalidTargetValue.h"
#include "PossibleAttackTargetsValue.h"
#include "EnemyPlayerValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

bool InvalidTargetValue::Calculate()
{
    Unit* target = AI_VALUE(Unit*, qualifier);
    if (!target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId())
    {
        return true;
    }

    Unit* duelTarget = AI_VALUE(Unit*, "duel target");
    if (duelTarget && duelTarget == target)
    {
        return false;
    }

    // Do NOT invalidate the current target merely because the bot's CLIENT
    // selection guid differs from it. "current target" is the bot's authoritative
    // combat target; the client selection is only set once the bot actually starts
    // attacking. Requiring them to match here was a DEADLOCK: SelectNewTargetAction
    // clears the selection (SetSelectionGuid(empty)) + AttackStop() every run, so the
    // current target was perpetually "mismatched" -> the "invalid target" trigger
    // (relevance 90, beats every attack ability) fired every tick -> select new target
    // re-acquired the SAME target -> still mismatched -> repeat. Bots spun on target
    // selection and never swung. Validity below is judged on the target itself.

    const bool validTarget = PossibleAttackTargetsValue::IsValid(target, bot);
    if (qualifier == "current target" && !validTarget)
    {
        // COMMIT to a current target during APPROACH / PULL. The strict IsValid
        // above requires the mob to already be engaged (in combat / targeting
        // something / our attack target), so a freshly chosen grind target was
        // flagged invalid every tick while the bot was still WALKING UP to it -
        // firing "select new target" (relevance 90), which preempted the pull and
        // re-picked the same mob forever (bots churned select-new-target during the
        // approach instead of pulling). A mob that is still a legitimate, in-range,
        // attackable target is NOT invalid just because it isn't fighting yet; only
        // drop it once it is no longer a possible target at all (despawned / out of
        // range / unattackable), in which case re-selection is correct.
        if (PossibleAttackTargetsValue::IsPossibleTarget(target, bot, sPlayerbotAIConfig.sightDistance, false))
            return false;

        // LOS-TOLERANT APPROACH: IsPossibleTarget fails a target that is momentarily OUT OF LOS
        // (forest trees, ridges) even when it is on the same ground. During an APPROACH that made the
        // current target flicker valid/invalid every second as the mob wandered behind scenery --
        // watched Eliney lock a boar at 45-75y, drop it, re-pick it, and never actually walk over
        // (52% of her time idle). A real player keeps walking toward a mob that stepped behind a
        // tree. Keep the commitment while out of combat if the target is alive, attackable-range,
        // and roughly on our ground plane (vertical delta small = reachable; the big-delta case is
        // the genuine unreachable-above/below one and still drops).
        // ...but NEVER stay committed to a mob TAPPED by someone else (grey name): with many bots
        // sharing one named quest spawn (watched: a ring of bots locked on "Princess", fire-blasting
        // air at 58y while the tapping bot killed her), a tapped target is unhittable and unlootable
        // -- keeping it means standing and staring. Drop it so the bot re-picks something claimable.
        if (!bot->IsInCombat() && target->IsAlive() && target->GetMapId() == bot->GetMapId() &&
            bot->IsWithinDistInMap(target, sPlayerbotAIConfig.sightDistance) &&
            std::fabs(bot->GetPositionZ() - target->GetPositionZ()) <= 8.0f &&
            !sServerFacade.IsFriendlyTo(bot, target) &&
            PossibleAttackTargetsValue::IsTapped(target, bot))
            return false;

        return true;
    }

    if (!validTarget)
    {
        std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
        if (std::find(attackers.begin(), attackers.end(), target->GetObjectGuid()) != attackers.end())
        {
            return false;
        }
    }

    return !validTarget;
}
