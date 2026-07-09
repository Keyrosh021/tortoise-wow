#pragma once

#include "playerbot/strategy/Action.h"
#include "MovementActions.h"

namespace ai
{
	class FollowAction : public MovementAction {
	public:
		FollowAction(PlayerbotAI* ai, std::string name = "follow") : MovementAction(ai, name) {}
		virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
        virtual bool CanDeadFollow(Unit* target);
    protected:
        // humanized follow for real-player masters: world-anchored personal slot + reaction
        // latency + stragglers, instead of the facing-locked core MoveFollow "pet ring"
        bool LooseFollow(Unit* master);
	};

	class StopFollowAction : public MovementAction {
	public:
		StopFollowAction(PlayerbotAI* ai, std::string name = "stop follow") : MovementAction(ai, name) {}
		virtual bool Execute(Event& event) override { ai->StopMoving(); return true; }
        virtual bool isUseful() override;
	};

    class FleeToMasterAction : public FollowAction {
    public:
        FleeToMasterAction(PlayerbotAI* ai) : FollowAction(ai, "flee to master") {}

        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };

}
