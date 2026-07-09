#include "LootObjectStack.h"
#include "playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include <mutex>
#include <unordered_map>

using namespace ai;

#define MAX_LOOT_OBJECT_COUNT 10

namespace
{
    static constexpr time_t PLAYERBOT_EMPTY_LOOT_RETRY_DELAY = 30;
    static constexpr time_t PLAYERBOT_FOREIGN_LOOT_RETRY_DELAY = 30;
    static std::mutex s_suppressedLootGuidsMutex;
    static std::unordered_map<uint64, time_t> s_suppressedLootGuids;

    std::string GetLootSkipQualifier(ObjectGuid guid)
    {
        return "loot skip::" + std::to_string(guid.GetRawValue());
    }

    void PruneSuppressedLootGuidsLocked(time_t now)
    {
        for (auto itr = s_suppressedLootGuids.begin(); itr != s_suppressedLootGuids.end();)
        {
            if (itr->second <= now)
                itr = s_suppressedLootGuids.erase(itr);
            else
                ++itr;
        }
    }

    bool IsSurvivalWoodLock(uint32 lockId)
    {
        switch (lockId)
        {
            case 1660:
            case 1661:
            case 1662:
            case 1663:
            case 1664:
            case 1665:
                return true;
            default:
                return false;
        }
    }

    bool IsSurvivalGatheringNode(GameObject const* go)
    {
        return go && (IsSurvivalWoodLock(go->GetGOInfo()->GetLockId()) ||
            go->GetName() == "Simple Wood Tree" ||
            go->GetName() == "Bright Wood Tree" ||
            go->GetName() == "Shade Wood Tree" ||
            go->GetName() == "Tropical Wood Tree" ||
            go->GetName() == "Star Wood Tree" ||
            go->GetName() == "Dead Wood Tree");
    }

    void LogLootRejection(Player* bot, ObjectGuid guid, const std::string& reason)
    {
        if (!bot || !sPlayerbotAIConfig.hasLog("bot_events.csv"))
            return;

        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            return;

        sPlayerbotAIConfig.logEvent(ai, "LootRejected", std::to_string(guid.GetCounter()), reason);
    }

}

bool ai::IsLootGuidSuppressed(ObjectGuid guid)
{
    if (!guid)
        return false;

    const time_t now = time(0);
    std::lock_guard<std::mutex> guard(s_suppressedLootGuidsMutex);
    PruneSuppressedLootGuidsLocked(now);

    auto itr = s_suppressedLootGuids.find(guid.GetRawValue());
    return itr != s_suppressedLootGuids.end() && itr->second > now;
}

void ai::SuppressLootGuid(ObjectGuid guid, time_t until)
{
    if (!guid)
        return;

    std::lock_guard<std::mutex> guard(s_suppressedLootGuidsMutex);
    PruneSuppressedLootGuidsLocked(time(0));
    s_suppressedLootGuids[guid.GetRawValue()] = until;
}

namespace
{
    std::mutex s_targetDeferMx;
    std::unordered_map<uint32, std::unordered_map<uint64, time_t>> s_targetDefers;
}

void ai::DeferTargetGuidForBot(Player* bot, ObjectGuid guid, time_t delaySeconds)
{
    if (!bot || !guid)
        return;
    std::lock_guard<std::mutex> lk(s_targetDeferMx);
    auto& m = s_targetDefers[bot->GetGUIDLow()];
    m[guid.GetRawValue()] = time(nullptr) + delaySeconds;
    if (m.size() > 64)
    {
        const time_t now = time(nullptr);
        for (auto it = m.begin(); it != m.end();)
            it = (it->second < now) ? m.erase(it) : std::next(it);
    }
}

bool ai::IsTargetGuidSkippedForBot(Player* bot, ObjectGuid guid)
{
    if (!bot || !guid)
        return false;
    std::lock_guard<std::mutex> lk(s_targetDeferMx);
    auto bi = s_targetDefers.find(bot->GetGUIDLow());
    if (bi == s_targetDefers.end())
        return false;
    auto ti = bi->second.find(guid.GetRawValue());
    if (ti == bi->second.end())
        return false;
    if (ti->second < time(nullptr))
    {
        bi->second.erase(ti);
        return false;
    }
    return true;
}

bool ai::IsLootGuidSkippedForBot(Player* bot, ObjectGuid guid)
{
    if (!bot || !guid)
        return false;

    if (IsLootGuidSuppressed(guid))
        return true;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;

    return ai->GetAiObjectContext()->GetValue<time_t>("manual time", GetLootSkipQualifier(guid))->Get() > time(0);
}

void ai::DeferLootGuidForBot(Player* bot, ObjectGuid guid, time_t delaySeconds)
{
    if (!bot || !guid || !delaySeconds)
        return;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return;

    ai->GetAiObjectContext()->GetValue<time_t>("manual time", GetLootSkipQualifier(guid))->Set(time(0) + delaySeconds);
}

void ai::DeferForeignLootGuidForBot(Player* bot, ObjectGuid guid)
{
    DeferLootGuidForBot(bot, guid, PLAYERBOT_FOREIGN_LOOT_RETRY_DELAY);
}

void ai::ClearLootGuidSkipForBot(Player* bot, ObjectGuid guid)
{
    if (!bot || !guid)
        return;
    {
        std::lock_guard<std::mutex> guard(s_suppressedLootGuidsMutex);
        s_suppressedLootGuids.erase(guid.GetRawValue());
    }
    if (PlayerbotAI* ai = bot->GetPlayerbotAI())
        ai->GetAiObjectContext()->GetValue<time_t>("manual time", GetLootSkipQualifier(guid))->Set(0);
}

bool ai::IsLootObjectCloseEnoughToOpen(Player* bot, LootObject& lootObject, float creatureRange)
{
    if (!bot || lootObject.IsEmpty())
        return false;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;

    if (lootObject.guid.IsGameObject())
    {
        GameObject* go = ai->GetGameObject(lootObject.guid);
        return go && sServerFacade.isSpawned(go) && go->IsAtInteractDistance(bot);
    }

    WorldObject* wo = lootObject.GetWorldObject(bot);
    return wo && sServerFacade.GetDistance2d(bot, wo) <= creatureRange;
}

LootTarget::LootTarget(ObjectGuid guid) : guid(guid), asOfTime(time(0))
{
}

LootTarget::LootTarget(LootTarget const& other)
{
    guid = other.guid;
    asOfTime = other.asOfTime;
}

LootTarget& LootTarget::operator=(LootTarget const& other)
{
    if((void*)this == (void*)&other)
        return *this;

    guid = other.guid;
    asOfTime = other.asOfTime;

    return *this;
}

bool LootTarget::operator< (const LootTarget& other) const
{
    return guid < other.guid;
}

void LootTargetList::shrink(time_t fromTime)
{
    for (std::set<LootTarget>::iterator i = begin(); i != end(); )
    {
        if (i->asOfTime <= fromTime)
            erase(i++);
		else
			++i;
    }
}

LootObject::LootObject(Player* bot, ObjectGuid guid)
	: guid(), skillId(SKILL_NONE), reqSkillValue(0), reqItem(0)
{
    Refresh(bot, guid);
}

void LootObject::Refresh(Player* bot, ObjectGuid guid, bool debug)
{
    skillId = SKILL_NONE;
    reqSkillValue = 0;
    reqItem = 0;
    this->guid = ObjectGuid();

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Creature* creature = ai->GetCreature(guid);

    if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
    {
        if (creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Creature flag lootable.", "debug loot");

            // LOOT WINS while the corpse is still lootable. This core can flag a corpse LOOTABLE and
            // SKINNABLE simultaneously (verified live on Princess: lootable=1 skinnable=1), and the
            // skinnable branch below then OVERWRITES the object into a skinning node (skillId set) --
            // every non-skinner rejected the corpse as "a node I can't harvest" and NEVER LOOTED it.
            // Starter zones are ~80% beasts, so this silently killed most looting (quest drops like
            // the Brass Collar included: 45 bots stuck, 0 completions ever). A player can't skin an
            // unlooted corpse anyway -- register plain loot and only consider skinning once the
            // lootable flag is gone.
            this->guid = guid;
            return;
        }

        if (creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
        {
            skillId = creature->GetCreatureInfo()->GetRequiredLootSkill();
            uint32 targetLevel = creature->GetLevel();
            reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
            if (ai->HasSkill((SkillType)skillId) && bot->GetSkillValue(skillId) >= reqSkillValue)
            {
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "Creature flag skinnable and has skill.", "debug loot");
                this->guid = guid;
            }

            if (debug)
                ai->TellDebug(ai->GetMaster(), "Creature flag skinnable not enough skill.", "debug loot");
            return;
        }

        if (debug)
            ai->TellDebug(ai->GetMaster(), "Creature without loot or skin flag.", "debug loot");

        return;
    }

    GameObject* go = ai->GetGameObject(guid);
    if (go && sServerFacade.isSpawned(go) && !go->IsInUse())
    {
        if (IsSurvivalGatheringNode(go))
        {
            skillId = SKILL_SURVIVAL;
            reqSkillValue = 1;
            this->guid = guid;
            return;
        }

        bool isQuestItemOnly = false;

#ifdef MANGOSBOT_TWO
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
        {
            int itemId = go->GetGOInfo()->questItems[i];

            if (ItemUsageValue::IsNeededForQuest(bot, itemId))
            {
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "GO has item needed for quest.", "debug loot");
                this->guid = guid;
                return;
            }
            isQuestItemOnly |= itemId > 0;
        }
#else
        /*if (!guid.IsEmpty())
        {
            for (auto& entry : GAI_VALUE2(std::list<int32>, "item drop list", -1*uint(go->GetEntry())))
            {
                if (IsNeededForQuest(bot, entry))
                {
                    this->guid = guid;
                    return;
                }
                isQuestItemOnly |= entry > 0;
            }
        }*/
#endif

        if (isQuestItemOnly)
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go has only quests items we don't need.", "debug loot");
            return;
        }

        uint32 goId = go->GetGOInfo()->id;
        std::set<uint32>& skipGoLootList = ai->GetAiObjectContext()->GetValue<std::set<uint32>&>("skip go loot list")->Get();
        if (skipGoLootList.find(goId) != skipGoLootList.end())
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go in skip go loot list.", "debug loot");
            return;
        }

        uint32 lockId = go->GetGOInfo()->GetLockId();
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go has no lockid.", "debug loot");
            return;
        }

        for (int i = 0; i < 8; ++i)
        {
            switch (lockInfo->Type[i])
            {
                case LOCK_KEY_ITEM:
                    if (lockInfo->Index[i] > 0)
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go has lock with key requirement.", "debug loot");
                        reqItem = lockInfo->Index[i];
                        this->guid = guid;
                    }
                    break;
                case LOCK_KEY_SKILL:
                    if (goId == 13891 || goId == 19535) // Serpentbloom
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go is serpentbloom.", "debug loot");
                        this->guid = guid;
                    }
                    else if (SkillByLockType(LockType(lockInfo->Index[i])) > 0)
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go requires skill.", "debug loot");
                        skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        reqSkillValue = std::max((uint32)1, lockInfo->Skill[i]);
                        this->guid = guid;
                    }
                    break;
                case LOCK_KEY_NONE:
                    if (debug)
                        ai->TellDebug(ai->GetMaster(), "Go has open lock.", "debug loot");
                    this->guid = guid;
                    break;
            }
        }
    }

    if (debug && guid && !this->guid)
        ai->TellDebug(ai->GetMaster(), "Go has bad lock.", "debug loot");
}

WorldObject* LootObject::GetWorldObject(Player* bot)
{
    Refresh(bot, guid);

    PlayerbotAI* ai = bot->GetPlayerbotAI();

    Creature *creature = ai->GetCreature(guid);
    if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        return creature;

    GameObject* go = ai->GetGameObject(guid);
    if (go && sServerFacade.isSpawned(go))
        return go;

    return NULL;
}

LootObject::LootObject(const LootObject& other)
{
    guid = other.guid;
    skillId = other.skillId;
    reqSkillValue = other.reqSkillValue;
    reqItem = other.reqItem;
}

bool LootObject::IsLootPossible(Player* bot)
{
    if (IsEmpty() || !GetWorldObject(bot))
        return false;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if (IsLootGuidSkippedForBot(bot, guid))
    {
        // Respawned creatures REUSE their guid, so a skip recorded against a PREVIOUS corpse (foreign
        // kill defer / empty-loot suppression -- and the suppression layer is GLOBAL across bots!)
        // poisons the bot's OWN later kill of the same guid. Verified live: Helinaria repeatedly
        // killed Princess (kill xp granted) but never opened the corpse, so quest-drop items were
        // never obtainable fleet-wide (45 bots stuck on 'Princess Must Die!', 0 completions ever).
        // If the bot has loot RIGHTS on the current corpse, the skip is stale: clear it and proceed.
        Creature* skipCreature = guid.IsCreature() ? ai->GetCreature(guid) : nullptr;
        if (skipCreature && sServerFacade.GetDeathState(skipCreature) == CORPSE &&
            bot->IsAllowedToLoot(skipCreature))
        {
            ClearLootGuidSkipForBot(bot, guid);
        }
        else
            return false;
    }

    if (reqItem && !bot->HasItemCount(reqItem, 1))
        return false;

    if (guid.IsCreature())
    {
        Creature* creature = ai->GetCreature(guid);
        if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        {
            if (skillId != SKILL_SKINNING && !bot->IsAllowedToLoot(creature))
            {
                DeferForeignLootGuidForBot(bot, guid);

                std::ostringstream out;
                out << "type=creature reason=not_allowed_looter"
                    << " target=" << creature->GetName()
                    << " tapped=" << (creature->HasLootRecipient() ? 1 : 0)
                    << " recipient=" << creature->GetLootRecipientGuid().GetCounter()
                    << " retry=" << uint32(PLAYERBOT_FOREIGN_LOOT_RETRY_DELAY)
                    << " groupRecipient=" << creature->GetLootGroupRecipientId();
                LogLootRejection(bot, guid, out.str());
                return false;
            }

            if (creature->m_loot && skillId != SKILL_SKINNING)
            {
                if (!creature->m_loot->IsAllowedLooter(bot->GetObjectGuid()))
                {
                    DeferForeignLootGuidForBot(bot, guid);

                    std::ostringstream out;
                    out << "type=creature reason=loot_not_reserved_for_bot"
                        << " target=" << creature->GetName()
                        << " recipient=" << creature->GetLootRecipientGuid().GetCounter()
                        << " retry=" << uint32(PLAYERBOT_FOREIGN_LOOT_RETRY_DELAY)
                        << " groupRecipient=" << creature->GetLootGroupRecipientId();
                    LogLootRejection(bot, guid, out.str());
                    return false;
                }
            }
        }
    }

    if (!AI_VALUE2_LAZY(bool, "should loot object", std::to_string(guid.GetRawValue())))
        return false;

    // Check if the game object has quest loot and bot has the quest for it
    if (guid.IsGameObject())
    {
        GameObject* go = ai->GetGameObject(guid);
        if (go)
        {
            if (IsSurvivalGatheringNode(go) && !ai->HasSkill(SKILL_SURVIVAL))
                return false;

            // Ignore for mining nodes and herbs
            if (skillId != SKILL_MINING && skillId != SKILL_HERBALISM && skillId != SKILL_SURVIVAL)
            {
                if (sObjectMgr.IsGameObjectForQuests(guid.GetEntry()))
                {
                    if (!go->ActivateToQuest(bot))
                    {
                        return false;
                    }
                }
            }
            
            // herb-like quest objects
            if (skillId == SKILL_HERBALISM && reqSkillValue == 1)
            {
                if (sObjectMgr.IsGameObjectForQuests(guid.GetEntry()))
                {
                    if (go->ActivateToQuest(bot))
                    {
                        bool hasQuestItems = false;
                        for (auto& entry : GAI_VALUE2(std::list<uint32>, "entry loot list", -1*int(go->GetEntry())))
                        {
                            if (ItemUsageValue::IsNeededForQuest(bot, entry))
                            {
                                hasQuestItems = true;
                            }
                        }
                        return hasQuestItems || go->GetLootState() != GO_READY;
                    }
                }
            }

            //Ignore objects that are currently in use.
            if (go->IsInUse() || go->GetGoState() == GO_STATE_ACTIVE)
                return false;
        }
    }

    if (skillId == SKILL_NONE)
        return true;

    if (skillId == SKILL_FISHING)
        return false;

    if (!ai->HasSkill((SkillType)skillId))
        return false;

    if (!reqSkillValue)
        return true;

    uint32 skillValue = uint32(bot->GetSkillValue(skillId));
    if (reqSkillValue > skillValue)
        return false;

    if (skillId == SKILL_MINING && !bot->HasItemCount(2901, 1))
        return false;

    if (skillId == SKILL_SKINNING && !bot->HasItemCount(7005, 1))
        return false;

    return true;
}

bool LootObjectStack::Add(ObjectGuid guid)
{
    if (IsLootGuidSkippedForBot(bot, guid))
        return false;

    if (!availableLoot.insert(guid).second)
        return false;

    if (availableLoot.size() < MAX_LOOT_OBJECT_COUNT)
        return true;

    std::vector<LootObject> ordered = OrderByDistance();
    for (size_t i = MAX_LOOT_OBJECT_COUNT; i < ordered.size(); i++)
        Remove(ordered[i].guid);

    return true;
}

void LootObjectStack::Remove(ObjectGuid guid)
{
    LootTargetList::iterator i = availableLoot.find(guid);
    if (i != availableLoot.end())
        availableLoot.erase(i);
}

void LootObjectStack::Clear()
{
    availableLoot.clear();
}

bool LootObjectStack::CanLoot(float maxDistance)
{
    std::vector<LootObject> ordered = OrderByDistance(maxDistance);
    return !ordered.empty();
}

LootObject LootObjectStack::GetLoot(float maxDistance)
{
    std::vector<LootObject> ordered = OrderByDistance(maxDistance);
    return ordered.empty() ? LootObject() : *ordered.begin();
}

std::vector<LootObject> LootObjectStack::OrderByDistance(float maxDistance)
{
    availableLoot.shrink(time(0) - 30);

    std::map<float, LootObject> sortedMap;
    LootTargetList safeCopy(availableLoot);
    for (LootTargetList::iterator i = safeCopy.begin(); i != safeCopy.end(); i++)
    {
        ObjectGuid guid = i->guid;
        LootObject lootObject(bot, guid);
        if (!lootObject.IsLootPossible(bot))
            continue;

        float distance = bot->GetDistance(lootObject.GetWorldObject(bot));
        if (!maxDistance || distance <= maxDistance)
            sortedMap[distance] = lootObject;
    }

    std::vector<LootObject> result;
    for (std::map<float, LootObject>::iterator i = sortedMap.begin(); i != sortedMap.end(); i++)
        result.push_back(i->second);
    return result;
}
