
#include "playerbot/playerbot.h"
#include "GrindingStrategy.h"

using namespace ai;

void GrindingStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "no target",
        NextAction::array(0,
        // 6.6 (was 5.0): travel bookkeeping runs at 6.0-6.99, so at 5.0 the engine burned every
        // tick on request/choose/reset loops while a sanctioned kill stood 20y away (combat share
        // 6.4% vs 30-47% goal). isUseful still yields to real travel (distance-aware veto), quest
        // objectives, HP/healer gates -- this only wins the tick when a nearby kill exists.
        new NextAction("attack anything", 6.6f),
        // Patrol/wander when genuinely idle: if there's nothing to attack AND no
        // travel destination (the legacy idle-rescue cascades that used to do this
        // are compile-disabled), wander to a new spot to find mobs instead of just
        // standing. MoveRandomAction::isUseful gates this to truly-idle autonomous
        // bots and won't re-fire while already moving (anti-thrash). The moment a
        // mob comes within sight, "attack anything" (rel 5) preempts and engages.
        new NextAction("move random", 0.4f), NULL)));
}