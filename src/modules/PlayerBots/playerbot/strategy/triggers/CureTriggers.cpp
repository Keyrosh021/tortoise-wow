
#include "playerbot/playerbot.h"
#include "GenericTriggers.h"
#include "CureTriggers.h"
#include "playerbot/strategy/actions/WorldBuffAction.h"

using namespace ai;

bool NeedCureTrigger::IsActive() 
{
    // Mass random-bot leveling should not let dispel scans stall continent map
    // workers. Keep active dispel behavior for commanded bots; later we can
    // reintroduce autonomous dispels through a sampled/budgeted combat model.
    if (!ai->HasRealPlayerMaster())
        return false;

	Unit* target = GetTarget();
	return target && ai->HasAuraToDispel(target, dispelType);
}

Value<Unit*>* PartyMemberNeedCureTrigger::GetTargetValue()
{
	return context->GetValue<Unit*>("party member to dispel", dispelType);
}

bool NeedWorldBuffTrigger::IsActive()
{
    return !WorldBuffAction::NeedWorldBuffs(bot).empty();   
}
