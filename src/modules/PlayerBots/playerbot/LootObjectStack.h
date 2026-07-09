#pragma once
#include "playerbot.h"

namespace ai
{
    bool IsLootGuidSuppressed(ObjectGuid guid);
    void SuppressLootGuid(ObjectGuid guid, time_t until);
    bool IsLootGuidSkippedForBot(Player* bot, ObjectGuid guid);
    // Per-bot short-lived ATTACK-target blacklist (anti re-acquire thrash after stall/abandon).
    void DeferTargetGuidForBot(Player* bot, ObjectGuid guid, time_t delaySeconds);
    bool IsTargetGuidSkippedForBot(Player* bot, ObjectGuid guid);
    void DeferLootGuidForBot(Player* bot, ObjectGuid guid, time_t delaySeconds);
    void DeferForeignLootGuidForBot(Player* bot, ObjectGuid guid);
    // Clears BOTH skip layers (per-bot defer + global suppression) for a guid. Needed because
    // respawned creatures REUSE guids: a skip recorded against a previous corpse (foreign kill /
    // empty loot) poisons the bot's OWN later kill of the same guid.
    void ClearLootGuidSkipForBot(Player* bot, ObjectGuid guid);

    class ItemQualifier;

    class LootObject
    {
    public:
        LootObject() : skillId(0), reqSkillValue(0), reqItem(0) {}
        LootObject(Player* bot, ObjectGuid guid);
        LootObject(const LootObject& other);

    public:
        bool IsEmpty() { return !guid; }
        bool IsLootPossible(Player* bot);
        void Refresh(Player* bot, ObjectGuid guid, bool debug = false);
        WorldObject* GetWorldObject(Player* bot);
        ObjectGuid guid;

        uint32 skillId;
        uint32 reqSkillValue;
        uint32 reqItem;
    };

    // 4.5y (was 2.0): the core accepts CMSG_LOOT within INTERACTION_DISTANCE (5y); at 2y the "open
    // loot" trigger never fired while MoveNear orbited the corpse at 3-5y, so killers paced around
    // their own kills until the loot target timed out (the never-loots-quest-drops chain, final gate).
    bool IsLootObjectCloseEnoughToOpen(Player* bot, LootObject& lootObject, float creatureRange = 4.5f);

    class LootTarget
    {
    public:
        LootTarget(ObjectGuid guid);
        LootTarget(LootTarget const& other);

    public:
        LootTarget& operator=(LootTarget const& other);
        bool operator< (const LootTarget& other) const;

    public:
        ObjectGuid guid;
        time_t asOfTime;
    };

    class LootTargetList : public std::set<LootTarget>
    {
    public:
        void shrink(time_t fromTime);
    };

    class LootObjectStack
    {
    public:
        LootObjectStack(Player* bot) : bot(bot) {}

    public:
        bool Add(ObjectGuid guid);
        void Remove(ObjectGuid guid);
        void Clear();
        bool CanLoot(float maxDistance);
        LootObject GetLoot(float maxDistance = 0);

    private:
        std::vector<LootObject> OrderByDistance(float maxDistance = 0);

    private:
        Player* bot;
        LootTargetList availableLoot;
    };

};
