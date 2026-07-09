
#include "playerbot/playerbot.h"
#include "LootNonCombatStrategy.h"

using namespace ai;

void LootNonCombatStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    // GOAL 4 "first thing bots do after killing is looting": the whole loot chain must OUTRANK
    // attack-anything (7.05) or bots chain-pull forever and corpses rot (measured: looting state in
    // 1/3066 snapshots). Old relevances: add-all-loot 1.0 (the queue-filler starved, so "loot
    // available" never even fired), loot 6.0 and move-to-loot 7.0 (both lost to re-attacking).
    // New chain: fill(7.2) -> start(7.15) -> walk(7.25) -> open(8.0), all above 7.05, so the
    // sequence is kill -> loot -> THEN next target. Combat reactions untouched (non-combat engine).
    triggers.push_back(new TriggerNode(
        "loot available",
        NextAction::array(0, new NextAction("loot", 7.15f), NULL)));

    triggers.push_back(new TriggerNode(
        "far from loot target",
        NextAction::array(0, new NextAction("move to loot", 7.25f), NULL)));

    triggers.push_back(new TriggerNode(
        "can loot",
        NextAction::array(0, new NextAction("open loot", 8.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("add all loot", 7.2f), NULL)));
}

void GatherStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "timer",
        NextAction::array(0, new NextAction("add gathering loot", 2.0f), NULL)));
}

void RevealStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("reveal gathering item", 50.0f), NULL)));
}

void RollStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("auto loot roll", 100.0f), NULL)));
}

void RollStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    RollStrategy::InitNonCombatTriggers(triggers);
}

void DelayedRollStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "loot roll",
        NextAction::array(0, new NextAction("loot roll", 100.0f), NULL)));
}

void DelayedRollStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    DelayedRollStrategy::InitNonCombatTriggers(triggers);
}