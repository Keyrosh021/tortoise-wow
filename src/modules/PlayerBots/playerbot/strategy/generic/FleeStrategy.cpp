
#include "playerbot/playerbot.h"
#include "FleeStrategy.h"

using namespace ai;

void FleeStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    // Masterless bots CAN flee, but only when it actually makes sense:
    //  - "outnumbered": OutNumberedTrigger weighs foe power (count + level + health)
    //    vs the bot's power and only fires when genuinely overpowered.
    //  - "panic": now gated so it fires only when the bot is LOSING the fight
    //    (target healthier than the bot), not on low HP alone — so a bot one hit
    //    from killing a mob finishes it instead of running circles around it.
    // Smart disengage: CC/blink/bubble with a class escape tool FIRST (relevance +10,
    // above the +9 flee run), THEN run. "use escape ability" returns false when nothing
    // is off-cooldown, so the engine falls straight through to "flee" the same tick.
    triggers.push_back(new TriggerNode(
        "panic",
        NextAction::array(0, new NextAction("use escape ability", ACTION_EMERGENCY + 10),
                             new NextAction("flee", ACTION_EMERGENCY + 9), NULL)));

    triggers.push_back(new TriggerNode(
        "outnumbered",
        NextAction::array(0, new NextAction("use escape ability", ACTION_EMERGENCY + 10),
                             new NextAction("flee", ACTION_EMERGENCY + 9), NULL)));
}

void FleeFromAddsStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "has nearest adds",
        NextAction::array(0, new NextAction("runaway", 50.0f), NULL)));
}
