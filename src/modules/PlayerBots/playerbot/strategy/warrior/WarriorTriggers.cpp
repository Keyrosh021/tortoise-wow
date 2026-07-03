
#include "playerbot/playerbot.h"
#include "WarriorTriggers.h"
#include "WarriorActions.h"

using namespace ai;

bool BloodrageBuffTrigger::IsActive()
{
    return AI_VALUE2(uint8, "health", "self target") >= sPlayerbotAIConfig.mediumHealth &&
        AI_VALUE2(uint8, "rage", "self target") < 20;
}

bool SunderArmorDebuffTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // Sunder costs 15 rage; without this gate rage-starved warriors queued it every tick and the
    // cast failed NO_POWER 120k+ times per day (pure decision churn, no visible action).
    if (AI_VALUE2(uint8, "rage", "self target") < 15)
        return false;

    if (ai->IsTank(bot) && !target->IsPlayer())
        return true;

    return !ai->HasAura("sunder armor", target, true) && !HasMaxDebuffs();
}

bool CommandingShoutTrigger::IsActive()
{
    uint32 comShout = AI_VALUE2(uint32, "spell id", "commanding shout");
    uint32 batShout = AI_VALUE2(uint32, "spell id", "battle shout");
    if (!(comShout || batShout))
        return false;

    if (bot->HasSpell(comShout))
        return !ai->HasAura("commanding shout", bot);

    if (bot->HasSpell(batShout))
        return !ai->HasAura("battle shout", bot);

    return false;
}
