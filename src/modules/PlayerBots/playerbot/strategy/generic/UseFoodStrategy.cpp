
#include "playerbot/playerbot.h"
#include "UseFoodStrategy.h"

using namespace ai;

void UseFoodStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    // Eat below MEDIUM health (<70%), not just low (<50%). Bots were STARTING fights at 55-99% HP
    // without topping up -- a needless handicap vs full-HP mobs and a steady feed into the death
    // rate (deaths stuck ~10/bot-hr vs the 0.05-0.15/hr real-player target). A real player eats to
    // near-full between pulls; the report's downtime budget (4-8 min/hr eating) covers this. Paired
    // with the engage-gate at 70% in AttackAnythingAction so eat-then-engage has no dead band.
    triggers.push_back(new TriggerNode(
        "medium health",
        NextAction::array(0, new NextAction("food", 3.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "high mana",
        NextAction::array(0, new NextAction("drink", 3.0f), NULL)));
}
