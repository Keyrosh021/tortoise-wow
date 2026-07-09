
#include "playerbot/playerbot.h"
#include "DungeonStrategy.h"

using namespace ai;

void DungeonStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // DB-driven per-boss hazard avoidance (ai_playerbot_encounter_mechanic) — generic for every
    // seeded encounter, no per-raid strategy class needed.
    triggers.push_back(new TriggerNode(
        "db encounter hazard",
        NextAction::array(0, new NextAction("move away from hazard", 95.0f), NULL)));

    // Carrying a lethal debuff (e.g. Living Bomb): leave the group NOW — outranks everything
    // except hard emergencies.
    triggers.push_back(new TriggerNode(
        "db run out debuff",
        NextAction::array(0, new NextAction("run out of group", 96.0f), NULL)));

    // Standing in a known boss's breath/cleave cone: step behind it.
    triggers.push_back(new TriggerNode(
        "db avoid frontal",
        NextAction::array(0, new NextAction("set behind", 94.0f), NULL)));

    // Tank turns a frontal-danger boss away from group members caught in its arc.
    triggers.push_back(new TriggerNode(
        "db tank face away",
        NextAction::array(0, new NextAction("tank face away", 93.0f), NULL)));

    // Off-tank taunts when the main tank's swap-debuff stacks hit the row threshold.
    triggers.push_back(new TriggerNode(
        "db tank swap",
        NextAction::array(0, new NextAction("taunt swap", 97.0f), NULL)));

    // Off-duty staged tanks hold outside the knockback zone (Ragnaros pattern).
    triggers.push_back(new TriggerNode(
        "db staged tanks",
        NextAction::array(0, new NextAction("staged tank hold", 92.0f), NULL)));

    // 4H: don't collect a second Horseman's Mark — back off the other boss (reuses staged hold).
    triggers.push_back(new TriggerNode(
        "db mark separation",
        NextAction::array(0, new NextAction("staged tank hold", 91.0f), NULL)));

    // Add this combat triggers in case the bot gets summoned into the dungeon and goes straight into combat
    triggers.push_back(new TriggerNode(
        "enter onyxia's lair",
        NextAction::array(0, new NextAction("enable onyxia's lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter molten core",
        NextAction::array(0, new NextAction("enable molten core strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter blackwing lair",
        NextAction::array(0, new NextAction("enable blackwing lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter karazhan",
        NextAction::array(0, new NextAction("enable karazhan strategy", 100.0f), NULL)));
}

void DungeonStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "enter onyxia's lair",
        NextAction::array(0, new NextAction("enable onyxia's lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave onyxia's lair",
        NextAction::array(0, new NextAction("disable onyxia's lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter molten core",
        NextAction::array(0, new NextAction("enable molten core strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave molten core",
        NextAction::array(0, new NextAction("disable molten core strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter blackwing lair",
        NextAction::array(0, new NextAction("enable blackwing lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave blackwing lair",
        NextAction::array(0, new NextAction("disable blackwing lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter karazhan",
        NextAction::array(0, new NextAction("enable karazhan strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave karazhan",
        NextAction::array(0, new NextAction("disable karazhan strategy", 100.0f), NULL)));
}