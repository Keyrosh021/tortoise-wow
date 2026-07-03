
#include "playerbot/playerbot.h"
#include "RangedCombatStrategy.h"

using namespace ai;

void RangedCombatStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    // BOT-CAM EVIDENCE (2026-06-30): pushing flee at ACTION_MOVE (30) here made ranged bots
    // (esp. hunters) flee FOREVER whenever a mob was within minimum-range. flee(30) outranks
    // raptor strike(11), auto shot(1), and every nuke, and "enemy too close" stays true as long
    // as the mob is on top of you -> a full-HP hunter runs in circles for 30s while its pet solos
    // the mob and the bot deals ZERO damage. Watched Shobassi do exactly this at 100% HP.
    // Fix: this kite is a LAST RESORT, not a priority. Below the attack tier so the bot prefers
    // to FIGHT (raptor strike / point-blank nuke) when something is in its face, and only backs
    // off to regain range when no attack is available (then auto shot re-enables and it shoots).
    // The real "I'm dying" flee is a SEPARATE push at ACTION_EMERGENCY+ (untouched).
    triggers.push_back(new TriggerNode(
        "enemy too close for spell",
        NextAction::array(0, new NextAction("flee", ACTION_NORMAL - 5), NULL)));
}
