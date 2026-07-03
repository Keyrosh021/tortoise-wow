
#include "playerbot/playerbot.h"
#include "SayStrategy.h"

using namespace ai;

void SayStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    // DISABLED 2026-06-30 (user request): these all pushed a "say::*" chat/emote action at
    // relevance 99 -- HIGHER than every combat ability (10-90) -- and the low-health/low-mana
    // triggers stay active for as long as the bot is hurt, so the bot would STOP attacking,
    // emote "call for help", and stand there spamming chat every tick instead of fighting or
    // healing. That is the "bot drops to 50% hp, calls for help, stands still" bug. We do not
    // want in-combat health/mana chatter at all. Left empty so bots just keep fighting.
    // (To restore flavor later: re-add at a LOW relevance, e.g. < ACTION_IDLE, plus a cooldown
    // so it can never preempt combat and can't spam.)
}
