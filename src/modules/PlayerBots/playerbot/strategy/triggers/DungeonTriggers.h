#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
    class EnterDungeonTrigger : public Trigger
    {
    public:
        // You can get the mapID from worlddb > instance_template > map column
        // or from here https://wow.tools/dbc/?dbc=map&build=1.12.1.5875
        EnterDungeonTrigger(PlayerbotAI* ai, std::string name, std::string dungeonStrategy, uint32 mapID)
        : Trigger(ai, name, 5)
        , dungeonStrategy(dungeonStrategy)
        , mapID(mapID) {}

        bool IsActive() override;

    private:
        std::string dungeonStrategy;
        uint32 mapID;
    };

    class LeaveDungeonTrigger : public Trigger
    {
    public:
        // You can get the mapID from worlddb > instance_template > map column
        // or from here https://wow.tools/dbc/?dbc=map&build=1.12.1.5875
        LeaveDungeonTrigger(PlayerbotAI* ai, std::string name, std::string dungeonStrategy, uint32 mapID)
        : Trigger(ai, name, 5)
        , dungeonStrategy(dungeonStrategy)
        , mapID(mapID) {}

        bool IsActive() override;

    private:
        std::string dungeonStrategy;
        uint32 mapID;
    };

    class StartBossFightTrigger : public Trigger
    {
    public:
        StartBossFightTrigger(PlayerbotAI* ai, std::string name, std::string bossStrategy, uint64 bossID)
        : Trigger(ai, name, 1)
        , bossStrategy(bossStrategy)
        , bossID(bossID) {}

        bool IsActive() override;

    private:
        std::string bossStrategy;
        uint64 bossID;
    };

    class EndBossFightTrigger : public Trigger
    {
    public:
        EndBossFightTrigger(PlayerbotAI* ai, std::string name, std::string bossStrategy, uint64 bossID)
        : Trigger(ai, name, 5)
        , bossStrategy(bossStrategy)
        , bossID(bossID) {}

        bool IsActive() override;

    private:
        std::string bossStrategy;
        uint64 bossID;
    };

    class CloseToHazardTrigger : public Trigger
    {
    public:
        CloseToHazardTrigger(PlayerbotAI* ai, std::string name, int checkInterval, float hazardRadius, time_t hazardDuration)
        : Trigger(ai, name, checkInterval)
        , hazardRadius(hazardRadius)
        , hazardDuration(hazardDuration) {}

        bool IsActive() override final;

    protected:
        virtual std::list<ObjectGuid> GetPossibleHazards() = 0;
        virtual bool IsHazardValid(const ObjectGuid& hazzardGuid);

    private:
        float GetDistanceToHazard(const ObjectGuid& hazzardGuid);

    protected:
        float hazardRadius;
        time_t hazardDuration;
    };

    class CloseToGameObjectHazardTrigger : public CloseToHazardTrigger
    {
    public:
        CloseToGameObjectHazardTrigger(PlayerbotAI* ai, std::string name, uint32 gameObjectID, float radius, time_t expirationTime)
        : CloseToHazardTrigger(ai, name, 1, radius, expirationTime)
        , gameObjectID(gameObjectID) {}

    private:
        std::list<ObjectGuid> GetPossibleHazards() override;

    private:
        uint32 gameObjectID;
    };

    class CloseToCreatureHazardTrigger : public CloseToHazardTrigger
    {
    public:
        CloseToCreatureHazardTrigger(PlayerbotAI* ai, std::string name, uint32 creatureID, float radius, time_t expirationTime)
        : CloseToHazardTrigger(ai, name, 1, radius, expirationTime)
        , creatureID(creatureID) {}

    private:
        std::list<ObjectGuid> GetPossibleHazards() override;
        bool IsHazardValid(const ObjectGuid& hazzardGuid) override;

    protected:
        uint32 creatureID;
    };

    class CloseToHostileCreatureHazardTrigger : public CloseToCreatureHazardTrigger
    {
    public:
        CloseToHostileCreatureHazardTrigger(PlayerbotAI* ai, std::string name, uint32 creatureID, float radius, time_t expirationTime)
        : CloseToCreatureHazardTrigger(ai, name, creatureID, radius, expirationTime) {}

    private:
        std::list<ObjectGuid> GetPossibleHazards() override;
    };

    // DB-DRIVEN HAZARDS: reads ai_playerbot_encounter_mechanic rows (hazard_creature /
    // hazard_object, entry in spell_id column) for any engaged known boss and feeds the shared
    // hazard cache -- per-boss avoidance without hardcoded strategy classes.
    class DbEncounterHazardTrigger : public Trigger
    {
    public:
        DbEncounterHazardTrigger(PlayerbotAI* ai, std::string name = "db encounter hazard")
        : Trigger(ai, name, 1) {}

        bool IsActive() override;
    };

    // Bot carries a run-out debuff (e.g. Geddon Living Bomb 20475) per encounter table -> spread.
    class DbRunOutDebuffTrigger : public Trigger
    {
    public:
        DbRunOutDebuffTrigger(PlayerbotAI* ai, std::string name = "db run out debuff")
        : Trigger(ai, name, 1) {}

        bool IsActive() override;
    };

    // Non-tank standing in the frontal cone of an engaged known boss whose encounter rows mark
    // frontal danger (breath/cleave) -> get behind it.
    class DbAvoidFrontalTrigger : public Trigger
    {
    public:
        DbAvoidFrontalTrigger(PlayerbotAI* ai, std::string name = "db avoid frontal")
        : Trigger(ai, name, 1) {}

        bool IsActive() override;
    };

    // TANK side of face-away: this bot tanks a known frontal-danger boss while group members
    // stand in its frontal arc -> reposition so the boss turns away from them.
    class DbTankFaceAwayTrigger : public Trigger
    {
    public:
        DbTankFaceAwayTrigger(PlayerbotAI* ai, std::string name = "db tank face away")
        : Trigger(ai, name, 2) {}

        bool IsActive() override;
    };

    // Off-tank taunt swap: the boss's current tank carries the stacking debuff at/over the
    // row's threshold (radius column) -> this off-tank taunts (BWL drakes, etc.).
    class DbTankSwapTrigger : public Trigger
    {
    public:
        DbTankSwapTrigger(PlayerbotAI* ai, std::string name = "db tank swap")
        : Trigger(ai, name, 2) {}

        bool IsActive() override;
    };

    // Staged tanks (Ragnaros): off-tanks hold OUTSIDE the knockback zone while one tank is
    // active, so a knockback never leaves the boss with zero tanks in range.
    class DbStagedTanksTrigger : public Trigger
    {
    public:
        DbStagedTanksTrigger(PlayerbotAI* ai, std::string name = "db staged tanks")
        : Trigger(ai, name, 2) {}

        bool IsActive() override;
    };

    // 4H choreography v2 slice: carrying one boss's Mark while near a DIFFERENT tank_swap boss
    // stacks both marks -> back away from the second boss (keeps corners separated).
    class DbMarkSeparationTrigger : public Trigger
    {
    public:
        DbMarkSeparationTrigger(PlayerbotAI* ai, std::string name = "db mark separation")
        : Trigger(ai, name, 2) {}

        bool IsActive() override;
    };

    class CloseToCreatureTrigger : public Trigger
    {
    public:
        CloseToCreatureTrigger(PlayerbotAI* ai, std::string name, uint32 creatureID, float range)
        : Trigger(ai, name, 1)
        , creatureID(creatureID)
        , range(range) {}

        bool IsActive() override;

    private:
        uint32 creatureID;
        float range;
    };

    class ItemReadyTrigger : public Trigger
    {
    public:
        ItemReadyTrigger(PlayerbotAI* ai, std::string name, uint32 itemID)
        : Trigger(ai, name, 1)
        , itemID(itemID) {}

        virtual bool IsActive() override;

    protected:
        uint32 itemID;
    };

    class ItemBuffReadyTrigger : public ItemReadyTrigger
    {
    public:
        ItemBuffReadyTrigger(PlayerbotAI* ai, std::string name, uint32 itemID, uint32 buffID)
        : ItemReadyTrigger(ai, name, itemID)
        , buffID(buffID) {}

        bool IsActive() override;

    private:
        uint32 buffID;
    };
}