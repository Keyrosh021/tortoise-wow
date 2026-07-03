
#include "playerbot/playerbot.h"
#include "CurrentTargetValue.h"

#include "playerbot/ServerFacade.h"
using namespace ai;

Unit* CurrentTargetValue::Get()
{
    if (selection.IsEmpty())
        return NULL;

    Unit* unit = sObjectAccessor.GetUnit(*bot, selection);
    if (!unit)
        return NULL;

    // A DEAD target is not a valid current target. Without this the bot keeps "current target"
    // pointed at the corpse of the mob it just killed (still in range), believes it is still
    // engaged, and never re-acquires -- so it stands there taking hits from a SECOND attacker
    // (e.g. a bear) and dies. Returning NULL frees the combat engine to pick the live attacker.
    if (!sServerFacade.IsAlive(unit))
        return NULL;

    if (!bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance))
        return NULL;

    return unit;
}

void CurrentTargetValue::Set(Unit* target)
{
    selection = target ? target->GetObjectGuid() : ObjectGuid();
}
