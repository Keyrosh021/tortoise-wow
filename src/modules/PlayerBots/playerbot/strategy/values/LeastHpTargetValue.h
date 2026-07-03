#pragma once
#include "playerbot/strategy/Value.h"
#include "TargetValue.h"

namespace ai
{
    class FindLeastHpTargetStrategy : public FindNonCcTargetStrategy
    {
    public:
        FindLeastHpTargetStrategy(PlayerbotAI* ai) : FindNonCcTargetStrategy(ai)
        {
            minHealth = 0;
        }
    public:
        virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
        {
            // do not use this logic for pvp
            if (attacker->IsPlayer())
                return;

            if (IsCcTarget(attacker))
                return;

            Group* group = ai->GetBot()->GetGroup();
            if (group)
            {
                uint64 guid = group->GetTargetIcon(4);
                if (guid && attacker->GetObjectGuid() == ObjectGuid(guid))
                    return;
            }
            // Prefer NEAR targets, not just the globally lowest-HP one. A far low-HP mob is almost
            // always one other bots are already finishing: the bot runs all the way to it, it dies
            // en route, and it re-picks another far mob -- the user's "runs to a target, changes her
            // mind right before attacking" (watched Cariani chase a 6%-HP Princess 66yd away that
            // died before she arrived, over and over). Score = healthPct + distance*3: a nearby mob
            // beats a far dying one, but a NEARBY dying mob is still finished first (focus fire when
            // it's actually reachable). Distance dominates past ~35yd so bots stop chasing far kills.
            const float score = attacker->GetHealthPercent() + ai->GetBot()->GetDistance(attacker) * 3.0f;
            if (!result || bestScore > score)
            {
                result = attacker;
                bestScore = score;
            }
        }
    protected:
        float minHealth;
        float bestScore = 100000.0f;
    };

    class LeastHpTargetValue : public TargetValue
	{
	public:
        LeastHpTargetValue(PlayerbotAI* ai, std::string name = "least hp target") : TargetValue(ai, name) {}

    public:
        Unit* Calculate() override;
    };
}
