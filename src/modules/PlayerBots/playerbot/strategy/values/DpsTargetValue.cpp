
#include "playerbot/playerbot.h"
#include "DpsTargetValue.h"
#include "LeastHpTargetValue.h"
#include "playerbot/EncounterKnowledgeMgr.h"

using namespace ai;


Unit* DpsTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    // ADDS FIRST (encounter table): fighting a known boss whose mechanics list add entries ->
    // dps burn the adds before the boss (lowest-HP add first). Raid icons above still override.
    {
        std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
        std::vector<EncounterMechanic> const* mechanics = nullptr;
        for (const ObjectGuid& guid : attackers)
            if (Unit* unit = ai->GetUnit(guid))
                if ((mechanics = sEncounterKnowledgeMgr.GetMechanics(unit->GetEntry())))
                    break;
        if (mechanics)
        {
            Unit* bestAdd = nullptr;
            for (EncounterMechanic const& mech : *mechanics)
            {
                if (mech.mechanicType != "adds_first" || !mech.spellId)
                    continue;
                for (const ObjectGuid& guid : attackers)
                {
                    Unit* unit = ai->GetUnit(guid);
                    if (!unit || unit->GetEntry() != mech.spellId || !unit->IsAlive())
                        continue;
                    if (!bestAdd || unit->GetHealth() < bestAdd->GetHealth())
                        bestAdd = unit;
                }
            }
            if (bestAdd)
                return bestAdd;
        }
    }

    FindLeastHpTargetStrategy strategy(ai);
    return TargetValue::FindTarget(&strategy);
}

class FindMaxHpTargetStrategy : public FindTargetStrategy
{
public:
    FindMaxHpTargetStrategy(PlayerbotAI* ai) : FindTargetStrategy(ai)
    {
        maxHealth = 0;
    }

public:
    virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
    {
        Group* group = ai->GetBot()->GetGroup();
        if (group)
        {
            uint64 guid = group->GetTargetIcon(4);
            if (guid && attacker->GetObjectGuid() == ObjectGuid(guid))
                return;
        }
        if (!result || result->GetHealth() < attacker->GetHealth())
            result = attacker;
    }

protected:
    float maxHealth;
};

Unit* DpsAoeTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    FindMaxHpTargetStrategy strategy(ai);
    return TargetValue::FindTarget(&strategy);
}
