
#include "playerbot/playerbot.h"
#include "LootTriggers.h"
#include "playerbot/LootObjectStack.h"

#include "playerbot/ServerFacade.h"
using namespace ai;

namespace
{
    static constexpr float PLAYERBOT_STRICT_LOOT_RANGE = 2.0f;
}

bool LootAvailableTrigger::IsActive()
{
    // Always allow selecting a loot target when loot exists outside of combat.
    // The follow-up "far from loot target" trigger is responsible for walking to it first.
    return AI_VALUE(bool, "has available loot") &&
            !AI_VALUE2(bool, "combat", "self target") &&
            !AI_VALUE2(bool, "mounted", "self target");
}

bool FarFromCurrentLootTrigger::IsActive()
{
    LootObject loot = AI_VALUE(LootObject, "loot target");

    if (!loot.IsLootPossible(bot))
        return false;

    return AI_VALUE2(float, "distance", "loot target") > PLAYERBOT_STRICT_LOOT_RANGE;
}

bool CanLootTrigger::IsActive()
{
    return AI_VALUE(bool, "can loot");
}
