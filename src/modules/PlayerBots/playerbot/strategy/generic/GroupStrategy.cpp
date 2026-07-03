
#include "playerbot/playerbot.h"
#include "GroupStrategy.h"

using namespace ai;

void GroupStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("invite nearby", 4.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "random",
        NextAction::array(0, new NextAction("invite guild", 4.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "seldom",
        NextAction::array(0, new NextAction("leave far away", 4.0f), NULL)));

    // Responsive seat-efficiency check (~10s): LeaveFarAwayAction::isUseful drops a
    // random member the instant it leaves the leader's shared-credit radius or its kill
    // goal diverges, so the seat goes to a co-located same-objective bot. The leave
    // conditions are stable (distance + quest-log goals), so frequent checks don't churn.
    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("leave far away", 4.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leader is afk",
        NextAction::array(0, new NextAction("leave far away", 4.0f), NULL)));

    /*triggers.push_back(new TriggerNode(
        "seldom",
        NextAction::array(0, new NextAction("reset instances", 1.0f), NULL)));*/
}

void GroupStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "seldom",
        NextAction::array(0, new NextAction("leave far away", 4.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leader is afk",
        NextAction::array(0, new NextAction("leave far away", 4.0f), NULL)));
}