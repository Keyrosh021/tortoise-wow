
#include "playerbot/playerbot.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/values/AttackersValue.h"
#include "PullActions.h"
#include "playerbot/strategy/values/PositionValue.h"

using namespace ai;

bool PullRequestAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (!strategy)
    {
        return false;
    }

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    Unit* target = GetTarget(event);
    if (!target)
    {
        ai->TellPlayerNoFacing(requester, "You have no target");
        return false;
    }

    // Solo autonomous bots pull what's NEARBY. reactDistance*3 = 450y let a grinder pull-chase a mob
    // 70y+ through populated terrain, aggro 2 adds en route, and die to an L8 boar at L14 (watched
    // Willane). A real solo player pulls from ~spell range; long walk-pulls are a group tactic.
    const bool soloAutonomous = !ai->HasRealPlayerMaster() && !bot->GetGroup();
    const float maxPullDistance = soloAutonomous ? 30.0f : sPlayerbotAIConfig.reactDistance * 3;
    const float distanceToPullTarget = target->GetDistance(ai->GetBot());
    if (distanceToPullTarget > maxPullDistance)
    {
        ai->TellPlayerNoFacing(requester, "The target is too far away");
        return false;
    }

    if (!AttackersValue::IsValid(target, bot, nullptr, false))
    {
        ai->TellPlayerNoFacing(requester, "The target can't be pulled");
        return false;
    }

    if (!strategy->CanDoPullAction(target))
    {
        std::ostringstream out; out << "Can't perform pull action '" << strategy->GetPullActionName() << "'";
        ai->TellPlayerNoFacing(requester, out.str());
        return false;
    }

    //Set position to return to after pulling.
    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionEntry pullPosition = posMap["pull"];

    pullPosition.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    posMap["pull"] = pullPosition;

    strategy->RequestPull(target);

    // Force change combat state to have a faster reaction time
    ai->OnCombatStarted();

    return true;
}

Unit* PullMyTargetAction::GetTarget(Event& event)
{
    Unit* target = nullptr;

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (event.getSource() == "attack anything")
    {
        ObjectGuid guid = event.getObject();
        target = ai->GetCreature(guid);
    }
    else if (requester)
    {
        target = ai->GetUnit(requester->GetSelectionGuid());
    }

    return target;
}

Unit* PullRTITargetAction::GetTarget(Event& event)
{
    return AI_VALUE(Unit*, "rti target");
}

bool PullStartAction::Execute(Event& event)
{
    bool result = false;
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        // ABORT the pull choreography the moment something is already BEATING on us mid-pull (adds
        // aggroed during the approach). Watched Willane stand in "pull start" for 20+ seconds at
        // melee range with two mobs on her, not fighting back, and die. When attacked, the pull is
        // moot -- end it and let the normal combat engine fight.
        if (!ai->HasRealPlayerMaster() && !bot->getAttackers().empty())
        {
            strategy->OnPullEnded();
            return false;
        }

        Unit* target = strategy->GetTarget();
        if (target)
        {
            if (strategy->GetPreActionName().empty())
                result = true;
            else
            {
                result = ai->DoSpecificAction(strategy->GetPreActionName(), event, true);
                if(result)
                    SetDuration(ai->GetAIInternalUpdateDelay());
            }

            // Set the pet on passive mode during the pull
            Pet* pet = bot->GetPet();
            if (pet)
            {
                UnitAI* creatureAI = ((Creature*)pet)->AI();
                if (creatureAI)
                {
                    strategy->SetPetReactState(creatureAI->GetReactState());
                    creatureAI->SetReactState(REACT_PASSIVE);
                }
            }

            strategy->OnPullStarted();
        }
    }

    return result;
}


PullAction::PullAction(PlayerbotAI* ai, std::string name) : CastSpellAction(ai, name)
{
    InitPullAction();
}

bool PullAction::Execute(Event& event)
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        Unit* target = strategy->GetTarget();
        if (target)
        {
            // Check if we are on pull range
            const float distanceToTarget = target->GetDistance(bot);
            if (distanceToTarget <= strategy->GetRange())
            {
                if (sServerFacade.isMoving(bot))
                {
                    // Force stop
                    ai->StopMoving();
                    strategy->RequestPull(target, false);
                    return false;
                }

                std::string actionName = strategy->GetPullActionName();

                // Execute the pull action
                SET_AI_VALUE(Unit*, "current target", GetTarget());
                if (ai->DoSpecificAction(actionName, event, true))
                {
                    strategy->RequestPull(target); //extend pull timer to walk back.
                    return true;
                }
                else
                    return false;
            }
            else
            {
                // Retry the reach pull action
                strategy->RequestPull(target, false);
            }
        }
    }

    return false;
}

bool PullAction::isPossible()
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        std::string spellName = strategy->GetSpellName();
        Unit* target = strategy->GetTarget();
        if (!spellName.empty() && target)
        {
            if (!ai->CanCastSpell(spellName, target, true, nullptr, true))
            {
                return false;
            }
        }
    }

    return true;
}

void PullAction::InitPullAction()
{
    // Get the pull action spell name from the strategy
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        std::string spellName = strategy->GetSpellName();
        if (!spellName.empty())
        {
            SetSpellName(spellName);

            float spellRange;
            if (ai->GetSpellRange(spellName, &spellRange))
            {
                range = spellRange;
            }
        }
    }
}

bool PullEndAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        // Restore the pet react state
        Pet* pet = bot->GetPet();
        if (pet)
        {
            UnitAI* creatureAI = ((Creature*)pet)->AI();
            if (creatureAI)
            {
                creatureAI->SetReactState(strategy->GetPetReactState());
            }
        }

        // Remove the saved pull position
        AiObjectContext* context = ai->GetAiObjectContext();
        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
        PositionEntry stayPosition = posMap["pull"];
        if (stayPosition.isSet())
        {
            posMap.erase("pull");
        }

        strategy->OnPullEnded();
        return true;
    }

    return false;
}