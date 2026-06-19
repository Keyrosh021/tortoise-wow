
#include "playerbot/playerbot.h"
#include "SetAvoidAreaAction.h"

#include "playerbot/strategy/values/PositionValue.h"
using namespace ai;


bool SetAvoidAreaAction::Execute(Event& event)
{
    SET_AI_VALUE2(PositionEntry, "pos", "last avoid", PositionEntry(bot));
    return false;
}

bool SetAvoidAreaAction::isUseful()
{
    if (bot->GetInstanceId())
        return false;

    PositionEntry p = AI_VALUE2(PositionEntry, "pos", "last avoid");

    if (!p.isSet())
        return true;

    return p.Get().distance(bot) > 20.0f;
}
