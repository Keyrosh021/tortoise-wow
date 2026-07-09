#pragma once
#include "MovementActions.h"
#include "playerbot/strategy/values/HazardsValue.h"

namespace ai
{
    class MoveAwayFromHazard : public MovementAction
    {
    public:
        MoveAwayFromHazard(PlayerbotAI* ai, std::string name = "move away from hazard") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "move away from hazard"; }
        virtual std::string GetHelpDescription()
        {
            return "This action makes the bot move away from hazardous areas in dungeons.\n"
                   "It identifies dangerous positions and navigates to a safer location.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {"hazards"}; }
#endif 

    private:
        bool IsHazardNearby(const WorldPosition& point, const std::list<HazardPosition>& hazards) const;
    };

    // Carrying a run-out debuff: move away from ALL group members to the stored distance so the
    // detonation hits nobody (DB mechanic 'run_out_debuff').
    class RunOutOfGroupAction : public MovementAction
    {
    public:
        RunOutOfGroupAction(PlayerbotAI* ai, std::string name = "run out of group") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
    };

    // Tank repositions to the far side of the boss (relative to group centroid) so the boss,
    // which faces its tank, turns its breath/cleave away from everyone else.
    class TankFaceAwayAction : public MovementAction
    {
    public:
        TankFaceAwayAction(PlayerbotAI* ai, std::string name = "tank face away") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
    };

    // Off-tank executes the taunt swap on the boss stored in "attack target" by the trigger.
    class TauntSwapAction : public MovementAction
    {
    public:
        TauntSwapAction(PlayerbotAI* ai, std::string name = "taunt swap") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
    };

    // Off-duty staged tank backs out to the hold distance from the boss (in "attack target").
    class StagedTankHoldAction : public MovementAction
    {
    public:
        StagedTankHoldAction(PlayerbotAI* ai, std::string name = "staged tank hold") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
    };

    class MoveAwayFromCreature : public MovementAction
    {
    public:
        MoveAwayFromCreature(PlayerbotAI* ai, std::string name, uint32 creatureID, float range) : MovementAction(ai, name), creatureID(creatureID), range(range) {}
        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "move away from creature"; }
        virtual std::string GetHelpDescription()
        {
            return "This action makes the bot move away from a specific creature in dungeons.\n"
                   "It maintains a safe distance from the specified creature ID within a defined range.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {"hazards"}; }
#endif 

    private:
        bool IsValidPoint(const WorldPosition& point, const std::list<Creature*>& creatures, const std::list<HazardPosition>& hazards);
        bool HasCreaturesNearby(const WorldPosition& point, const std::list<Creature*>& creatures) const;
        bool IsHazardNearby(const WorldPosition& point, const std::list<HazardPosition>& hazards) const;

    private:
        uint32 creatureID;
        float range;
    };
}
