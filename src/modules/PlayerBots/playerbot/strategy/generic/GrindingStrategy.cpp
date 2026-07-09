
#include "playerbot/playerbot.h"
#include "GrindingStrategy.h"

using namespace ai;

void GrindingStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "no target",
        NextAction::array(0,
        // 7.05 (was 6.6, was 5.0): travel bookkeeping runs at 6.0-6.99 -- at 6.6 the travel
        // CHOOSE/REFRESH/RESET triggers (refresh 6.7, choose 6.98, reset 6.99) still OUT-RANKED a
        // sanctioned kill, so a bot with no travel target churned "choose travel target" instead of
        // killing the mob in front of it (measured: 24% of bots have an engageable mob <=30y while
        // combat share sits at ~2%). 7.05 puts a real nearby kill above ALL travel bookkeeping.
        // SAFE: AttackAnythingAction::isUseful only returns true when a valid, reachable, level-
        // appropriate target actually exists (and still yields via the distance-aware travel veto,
        // quest objectives, HP<55% and healer gates) -- so travel still runs whenever there is
        // nothing to kill; this only wins the tick when there genuinely is.
        new NextAction("attack anything", 7.05f),
        // Patrol/wander when genuinely idle: if there's nothing to attack AND no
        // travel destination (the legacy idle-rescue cascades that used to do this
        // are compile-disabled), wander to a new spot to find mobs instead of just
        // standing. MoveRandomAction::isUseful gates this to truly-idle autonomous
        // bots and won't re-fire while already moving (anti-thrash). The moment a
        // mob comes within sight, "attack anything" (rel 5) preempts and engages.
        new NextAction("move random", 0.4f), NULL)));
}