
#include "playerbot/playerbot.h"
#include "LootAction.h"

#include "playerbot/LootObjectStack.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/LootStrategyValue.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/SharedValueContext.h"

#include <mutex>
#include <unordered_map>


using namespace ai;

namespace
{
    // 4.5y, was 2.0y: the core accepts CMSG_LOOT within INTERACTION_DISTANCE (5y), but forcing bots
    // to converge to 2y made MoveNear ORBIT the corpse (each dispatch lands on a slightly different
    // offset point) until the loot target timed out -- watched Seranon kill Princess at melee range,
    // wobble around her corpse for 8s in "move to loot", and give up without ever opening it. At
    // 4.5y the first hop is already close enough to open.
    static constexpr float PLAYERBOT_STRICT_LOOT_RANGE = 4.5f;
    static constexpr time_t PLAYERBOT_EMPTY_LOOT_RETRY_DELAY = 30;
    static constexpr time_t PLAYERBOT_GO_OPEN_RETRY_DELAY = 10;

    std::string GetLootSkipQualifier(ObjectGuid guid)
    {
        return "loot skip::" + std::to_string(guid.GetRawValue());
    }

    void LogLootTrace(PlayerbotAI* ai, Player* bot, const char* eventName, ObjectGuid guid, const std::string& details)
    {
        if (!ai || !bot || !sPlayerbotAIConfig.hasLog("bot_events.csv"))
            return;

        std::ostringstream out;
        out << "lootGuid=" << guid.GetCounter()
            << " botLootGuid=" << bot->GetLootGuid().GetCounter()
            << " lootingFlag=" << (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING) ? 1 : 0)
            << " standState=" << uint32(bot->getStandState())
            << " " << details;
        sPlayerbotAIConfig.logEvent(ai, eventName, std::to_string(guid.GetCounter()), out.str());
    }

    void ClearActiveLootState(PlayerbotAI* ai, Player* bot, ObjectGuid contextGuid = ObjectGuid())
    {
        if (!bot)
            return;

        const ObjectGuid previousLootGuid = bot->GetLootGuid();
        const bool hadLootingFlag = bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);

        if (WorldSession* session = bot->GetSession())
        {
            if (ObjectGuid lootGuid = bot->GetLootGuid())
                session->DoLootRelease(lootGuid);
        }

        if (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING))
            bot->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);

        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        if (previousLootGuid || hadLootingFlag)
        {
            const ObjectGuid traceGuid = contextGuid ? contextGuid : previousLootGuid;
            LogLootTrace(ai, bot, "LootClearState", traceGuid,
                "releasedPrevious=" + std::to_string(previousLootGuid.GetCounter()) +
                " hadLootingFlag=" + std::to_string(hadLootingFlag ? 1 : 0));
        }
    }

    bool HasAttachedLiveTarget(PlayerbotAI* ai, Player* bot, Unit* currentTarget)
    {
        if (!ai || !bot || !currentTarget || !currentTarget->IsInWorld() || sServerFacade.UnitIsDead(currentTarget))
            return false;

        const ObjectGuid targetGuid = currentTarget->GetObjectGuid();
        return bot->GetVictim() == currentTarget || bot->GetSelectionGuid() == targetGuid;
    }

    bool HasLiveHostileTarget(Player* bot, Unit* currentTarget)
    {
        if (!bot || !currentTarget || !currentTarget->IsInWorld() || sServerFacade.UnitIsDead(currentTarget))
            return false;

        return bot->IsValidAttackTarget(currentTarget) && sServerFacade.IsHostileTo(bot, currentTarget);
    }

    bool HasActiveCombatPressure(PlayerbotAI* ai, Player* bot, Unit* currentTarget)
    {
        if (!ai || !bot)
            return false;

        AiObjectContext* aiContext = ai->GetAiObjectContext();
        if (!aiContext)
            return false;

        // Only ACTUAL combat pressure blocks looting -- the bot is flagged in-combat or is being
        // attacked. A merely QUEUED next grind target (HasAttachedLiveTarget / HasLiveHostileTarget)
        // must NOT block looting: grinding bots ALWAYS have the next mob queued as "current target",
        // so counting that as pressure deferred (and the caller ABANDONED) the loot on nearly every
        // kill -> the fleet earned 0 gold and never looted quest items. Real players loot their kills
        // with the next mob already targeted; so do we.
        if (aiContext->GetValue<bool>("combat", "self target")->Get())
            return true;

        if (aiContext->GetValue<bool>("has attackers")->Get())
            return true;

        return false;
    }

    bool ShouldUseQuestGameObjectDirectly(Player* bot, GameObject* go)
    {
        if (!bot || !go || !go->IsInWorld() || go->GetMapId() != bot->GetMapId())
            return false;

        if (!sObjectMgr.IsGameObjectForQuests(go->GetEntry()) || !go->ActivateToQuest(bot))
            return false;

        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_GOOBER:
            case GAMEOBJECT_TYPE_GENERIC:
            case GAMEOBJECT_TYPE_SPELL_FOCUS:
                return true;
            default:
                return false;
        }
    }

    bool IsDisposableForQuestLoot(Player* bot, Item* item)
    {
        if (!bot || !item)
            return false;

        ItemPrototype const* proto = item->GetProto();
        if (!proto)
            return false;

        if (proto->Quality > ITEM_QUALITY_POOR)
            return false;

        if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
            return false;

        if (ItemUsageValue::IsNeededForQuest(bot, item->GetEntry()))
            return false;

        return true;
    }

    bool FreeJunkSlotForQuestLoot(PlayerbotAI* ai, Player* bot, uint32 questItemId, ObjectGuid lootGuid)
    {
        if (!bot)
            return false;

        auto destroyIfDisposable = [&](uint8 bag, uint8 slot) -> bool
        {
            Item* item = bot->GetItemByPos(bag, slot);
            if (!IsDisposableForQuestLoot(bot, item))
                return false;

            ItemPrototype const* proto = item->GetProto();
            uint32 entry = item->GetEntry();
            uint32 count = item->GetCount();
            std::string name = proto ? proto->Name1 : std::to_string(entry);

            bot->DestroyItem(bag, slot, true);

            LogLootTrace(ai, bot, "QuestLootFreedJunkSlot", lootGuid,
                "questItem=" + std::to_string(questItemId) +
                " destroyedItem=" + std::to_string(entry) +
                " destroyedName=" + name +
                " count=" + std::to_string(count));
            return true;
        };

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (destroyIfDisposable(INVENTORY_SLOT_BAG_0, slot))
                return true;
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = dynamic_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!bag)
                continue;

            for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                if (destroyIfDisposable(bagSlot, slot))
                    return true;
            }
        }

        return false;
    }

    bool CanStoreLootItem(Player* bot, uint32 itemId, uint32 itemCount)
    {
        if (!bot)
            return false;

        ItemPosCountVec dest;
        return bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, itemCount) == EQUIP_ERR_OK;
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
}

bool LootAction::Execute(Event& event)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (HasActiveCombatPressure(ai, bot, currentTarget))
    {
        ClearActiveLootState(ai, bot);
        context->GetValue<LootObject>("loot target")->Set(LootObject());

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "combat=" << (AI_VALUE2(bool, "combat", "self target") ? 1 : 0)
                << " attackers=" << (AI_VALUE(bool, "has attackers") ? 1 : 0)
                << " currentTargetAlive=" << (currentTarget && !sServerFacade.UnitIsDead(currentTarget) ? 1 : 0)
                << " currentTarget=" << (currentTarget ? currentTarget->GetName() : "none");
            sPlayerbotAIConfig.logEvent(ai, "LootDeferredCombat", currentTarget ? std::to_string(currentTarget->GetGUIDLow()) : "none", out.str());
        }
        return false;
    }

    if (!AI_VALUE(bool, "has available loot"))
        return false;

    LootObject prevLoot = AI_VALUE(LootObject, "loot target");
    LootObject const& lootObject = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);

    if (!prevLoot.IsEmpty() && prevLoot.guid != lootObject.guid)
    {
        // Switching loot target: release the previous one, but ONLY if the bot actually
        // has it open (GetLootGuid). Releasing a GO the bot merely targeted but never
        // opened would run DoLootRelease's "not fully looted" branch and LOCK it. This
        // matches the old HandleLootReleaseOpcode, which only ever released GetLootGuid.
        if (bot->GetLootGuid() == prevLoot.guid)
        {
            if (WorldSession* session = bot->GetSession())
                session->DoLootRelease(prevLoot.guid);
        }
    }

    context->GetValue<LootObject>("loot target")->Set(lootObject);
    return true;
}

enum ProfessionSpells
{
    ALCHEMY                      = 2259,
    BLACKSMITHING                = 2018,
    COOKING                      = 2550,
    ENCHANTING                   = 7411,
    ENGINEERING                  = 49383,
    FIRST_AID                    = 3273,
    FISHING                      = 7620,
    HERB_GATHERING               = 2366,
    INSCRIPTION                  = 45357,
    JEWELCRAFTING                = 25229,
    MINING                       = 2575,
    SKINNING                     = 8613,
    TAILORING                    = 3908
};

bool OpenLootAction::Execute(Event& event)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (HasActiveCombatPressure(ai, bot, currentTarget))
    {
        ClearActiveLootState(ai, bot);
        context->GetValue<LootObject>("loot target")->Set(LootObject());

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "combat=" << (AI_VALUE2(bool, "combat", "self target") ? 1 : 0)
                << " attackers=" << (AI_VALUE(bool, "has attackers") ? 1 : 0)
                << " currentTargetAlive=" << (currentTarget && !sServerFacade.UnitIsDead(currentTarget) ? 1 : 0)
                << " currentTarget=" << (currentTarget ? currentTarget->GetName() : "none");
            sPlayerbotAIConfig.logEvent(ai, "LootOpenDeferredCombat", currentTarget ? std::to_string(currentTarget->GetGUIDLow()) : "none", out.str());
        }
        return false;
    }

    LootObject lootObject = AI_VALUE(LootObject, "loot target");
    if (lootObject.IsEmpty())
    {
        ClearActiveLootState(ai, bot);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
        return false;
    }

    bool result = DoLoot(lootObject);
    if (result)
    {
        // If opening a GO synchronously put the bot into a loot session, keep the
        // target around until StoreLootAction drains and releases it. Forgetting it
        // here can leave chests/quest objects stuck in GO_ACTIVATED/"in use" state
        // if the packet action is delayed or interrupted.
        if (bot->GetLootGuid() == lootObject.guid || bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING))
            SetDuration(sPlayerbotAIConfig.lootDelay);
        else
        {
            AI_VALUE(LootObjectStack*, "available loot")->Remove(lootObject.guid);
            context->GetValue<LootObject>("loot target")->Set(LootObject());
        }
    }
    else if (!lootObject.IsLootPossible(bot))
    {
        ClearActiveLootState(ai, bot, lootObject.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
    }
    else
    {
        // DoLoot failed but ILP still passes: without a bound this retries every tick forever
        // (the SKINNABLE-gate mismatch spun 1.43M times on one corpse). After a few consecutive
        // failures on the same guid, defer it briefly so the bot moves on and retries later.
        struct FailSt { ObjectGuid guid; uint32 count; };
        static std::mutex failMx;
        static std::unordered_map<uint32, FailSt> failTrack;
        std::lock_guard<std::mutex> lk(failMx);
        FailSt& fs = failTrack[bot->GetGUIDLow()];
        if (fs.guid != lootObject.guid) { fs.guid = lootObject.guid; fs.count = 0; }
        if (++fs.count >= 5)
        {
            fs.count = 0;
            AI_VALUE(LootObjectStack*, "available loot")->Remove(lootObject.guid);
            context->GetValue<LootObject>("loot target")->Set(LootObject());
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                sPlayerbotAIConfig.logEvent(ai, "LootOpenGivenUp", std::to_string(lootObject.guid.GetCounter()), "reason=5-consecutive-doloot-failures");
        }
    }
    return result;
}

bool OpenLootAction::DoLoot(LootObject& lootObject)
{
    if (lootObject.IsEmpty())
    {
        ClearActiveLootState(ai, bot);
        return false;
    }

    Creature* creature = ai->GetCreature(lootObject.guid);
    // Mirror the CORE's loot-range test exactly (Player::SendLoot uses 3D IsWithinDistInMap with
    // GetMaxLootDistance and no size credit). The old 2D 4.5y gate passed positions the core then
    // rejected with LOOT_ERROR_TOO_FAR -- the bot stood "in range" by its own math, got the error
    // response, never re-approached, and spun on the corpse forever.
    if (creature && !bot->IsWithinDistInMap(creature, bot->GetMaxLootDistance(creature), true, SizeFactor::None))
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << creature->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, creature)
                << " desired=" << PLAYERBOT_STRICT_LOOT_RANGE;
            sPlayerbotAIConfig.logEvent(ai, "LootMoveBeforeOpen", std::to_string(creature->GetGUIDLow()), out.str());
        }

        bot->SetWalk(false, false);
        // HYSTERESIS: approach well INSIDE the open gate (1.5y vs the 4.5y check above). Approaching
        // to exactly the gate distance made MoveNear land ON the ring edge, float-jittering in/out --
        // the bot orbited its own kill until the loot target timed out, never opening.
        return MoveNear(creature, 1.5f);
    }

    // LOOT WINS at the dispatch gate too: this core sets SKINNABLE at death, so fresh corpses are
    // lootable+skinnable simultaneously. Requiring !SKINNABLE here sent every such corpse into the
    // skinning branch, which returns false for bots without the skill -- the rights holder retried
    // forever and CMSG_LOOT was NEVER sent (measured: 1.43M refresh spins, zero opens, quest 88
    // never completed by any bot). Mirrors the loot-wins rule in LootObject::Refresh.
    if (creature && creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE) && lootObject.skillId != SKILL_SKINNING)
    {
        if (!lootObject.IsLootPossible(bot)) //Clear loot if bot can't loot it.
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "target=" << creature->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, creature)
                    << " canLoot=0";
                sPlayerbotAIConfig.logEvent(ai, "LootRejected", std::to_string(creature->GetGUIDLow()), out.str());
            }

            return true;
        }

        WorldPacket packet(CMSG_LOOT, 8);
        packet << lootObject.guid;
        bot->GetSession()->HandleLootOpcode(packet);
        SetDuration(sPlayerbotAIConfig.lootDelay);

        LogLootTrace(ai, bot, "LootOpenDispatch", lootObject.guid,
            "type=creature dist=" + std::to_string(sServerFacade.GetDistance2d(bot, creature)));

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << creature->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, creature)
                << " desired=" << PLAYERBOT_STRICT_LOOT_RANGE;
            sPlayerbotAIConfig.logEvent(ai, "LootOpenTrace", std::to_string(creature->GetGUIDLow()), out.str());
        }

        if (bot->isRealPlayer())
        {
            WorldPacket data(SMSG_EMOTE, 4 + 8);
            data << uint32(EMOTE_ONESHOT_LOOT);
            data << bot->GetObjectGuid();
            bot->GetSession()->SendPacket(data);
        }

        return true;
    }

    if (creature)
    {
        SkillType skill = (SkillType)creature->GetCreatureInfo()->GetRequiredLootSkill();
        if (!CanOpenLock(skill, lootObject.reqSkillValue))
            return false;

        switch (skill)
        {
        case SKILL_ENGINEERING:
            return ai->HasSkill(SKILL_ENGINEERING) ? ai->CastSpell(ENGINEERING, creature) : false;
        case SKILL_HERBALISM:
            return ai->HasSkill(SKILL_HERBALISM) ? ai->CastSpell(32605, creature) : false;
        case SKILL_MINING:
            return ai->HasSkill(SKILL_MINING) ? ai->CastSpell(32606, creature) : false;
        default:
            return ai->HasSkill(SKILL_SKINNING) ? ai->CastSpell(SKINNING, creature) : false;
        }
    }

    GameObject* go = ai->GetGameObject(lootObject.guid);
    if (go && sServerFacade.GetDistance2d(bot, go) > PLAYERBOT_STRICT_LOOT_RANGE)
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << go->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, go)
                << " desired=" << PLAYERBOT_STRICT_LOOT_RANGE;
            sPlayerbotAIConfig.logEvent(ai, "LootMoveBeforeOpen", std::to_string(go->GetGUIDLow()), out.str());
        }

        bot->SetWalk(false, false);
        return MoveNear(go, PLAYERBOT_STRICT_LOOT_RANGE);
    }

    if (go && (go->IsInUse() || go->GetGoState() == GO_STATE_ACTIVE))
        return false;

    if (ShouldUseQuestGameObjectDirectly(bot, go))
    {
        if (!go->IsAtInteractDistance(bot) || !go->PlayerCanUse(bot))
        {
            LogLootTrace(ai, bot, "QuestGoUseRejected", lootObject.guid,
                std::string("target=") + go->GetName() +
                " type=" + std::to_string(uint32(go->GetGoType())) +
                " state=" + std::to_string(uint32(go->GetGoState())) +
                " lootState=" + std::to_string(uint32(go->getLootState())) +
                " dist=" + std::to_string(sServerFacade.GetDistance2d(bot, go)) +
                " interact=" + std::to_string(go->IsAtInteractDistance(bot) ? 1 : 0) +
                " playerCanUse=" + std::to_string(go->PlayerCanUse(bot) ? 1 : 0));
            return false;
        }

        WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
        packet << lootObject.guid;
        bot->GetSession()->HandleGameObjectUseOpcode(packet);
        SetDuration(sPlayerbotAIConfig.lootDelay);

        LogLootTrace(ai, bot, "QuestGoUseDispatch", lootObject.guid,
            std::string("target=") + go->GetName() +
            " type=" + std::to_string(uint32(go->GetGoType())) +
            " state=" + std::to_string(uint32(go->GetGoState())) +
            " lootState=" + std::to_string(uint32(go->getLootState())) +
            " dist=" + std::to_string(sServerFacade.GetDistance2d(bot, go)));

        return true;
    }

    if (lootObject.skillId == SKILL_MINING)
        return ai->HasSkill(SKILL_MINING) ? ai->CastSpell(MINING, bot) : false;

    if (lootObject.skillId == SKILL_HERBALISM)
    {
        // herb-like quest objects
        bool isForQuest = false;
        if (go && sObjectMgr.IsGameObjectForQuests(lootObject.guid.GetEntry()))
        {
            if (go->ActivateToQuest(bot))
            {
                std::list<uint32> lootItems = GAI_VALUE2(std::list<uint32>, "entry loot list", -1*int32(go->GetEntry()));
                isForQuest = !lootItems.empty() || go->GetLootState() != GO_READY;
            }
        }

        if (!isForQuest)
        {
            return ai->HasSkill(SKILL_HERBALISM) ? ai->CastSpell(HERB_GATHERING, bot) : false;
        }
    }

    uint32 spellId = GetOpeningSpell(lootObject);
    if (!spellId)
        return false;

    if (!lootObject.IsLootPossible(bot)) //Clear loot if bot can't loot it.
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "target=" << (go ? go->GetName() : "gameobject")
                << " dist=" << std::fixed << std::setprecision(2) << (go ? sServerFacade.GetDistance2d(bot, go) : 0.0f)
                << " canLoot=0";
            sPlayerbotAIConfig.logEvent(ai, "LootRejected", std::to_string(lootObject.guid.GetCounter()), out.str());
        }

        return true;
    }

    //Keys need to use the key 
    if (spellId == sPlayerbotAIConfig.openGoSpell && go && lootObject.reqItem && bot->HasItemCount(lootObject.reqItem,1,false))
    {
        return ai->DoSpecificAction("use", Event("do loot", chat->formatQItem(lootObject.reqItem) + " " + chat->formatGameobject(go)));
    }

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "target=" << (go ? go->GetName() : "gameobject")
            << " dist=" << std::fixed << std::setprecision(2) << (go ? sServerFacade.GetDistance2d(bot, go) : 0.0f)
            << " desired=" << PLAYERBOT_STRICT_LOOT_RANGE
            << " spellId=" << spellId;
        sPlayerbotAIConfig.logEvent(ai, "LootOpenTrace", std::to_string(lootObject.guid.GetCounter()), out.str());
    }

    LogLootTrace(ai, bot, "LootOpenDispatch", lootObject.guid,
        "type=gameobject spellId=" + std::to_string(spellId));

    const bool castStarted = ai->CastSpell(spellId, go);
    if (!castStarted)
        return false;

    if (!bot->GetLootGuid() && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING))
    {
        DeferLootGuidForBot(bot, lootObject.guid, PLAYERBOT_GO_OPEN_RETRY_DELAY);
        AI_VALUE(LootObjectStack*, "available loot")->Remove(lootObject.guid);
        RESET_AI_VALUE2(bool, "should loot object", std::to_string(lootObject.guid.GetRawValue()));

        LogLootTrace(ai, bot, "LootOpenNoImmediateResponse", lootObject.guid,
            "type=gameobject spellId=" + std::to_string(spellId) +
            " suppressFor=" + std::to_string(uint32(PLAYERBOT_GO_OPEN_RETRY_DELAY)));
    }

    return true;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject)
{
    GameObject* go = ai->GetGameObject(lootObject.guid);
    if (go && sServerFacade.isSpawned(go))
        return GetOpeningSpell(lootObject, go);

    ClearActiveLootState(ai, bot, lootObject.guid);
    return 0;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject, GameObject* go)
{
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;

		if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled || IsPassiveSpell(spellId))
			continue;

		if (spellId == MINING || spellId == HERB_GATHERING)
			continue;

		const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
		if (!pSpellInfo)
			continue;

        if (CanOpenLock(lootObject, pSpellInfo, go))
            return spellId;
    }

    for (uint32 spellId = 0; spellId < sServerFacade.GetSpellInfoRows(); spellId++)
    {
        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

		const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
		if (!pSpellInfo)
            continue;

        if (CanOpenLock(lootObject, pSpellInfo, go))
            return spellId;
    }

    return sPlayerbotAIConfig.openGoSpell;
}

bool OpenLootAction::CanOpenLock(LootObject& lootObject, const SpellEntry* pSpellInfo, GameObject* go)
{
    if (go && IsSurvivalWoodLock(go->GetGOInfo()->GetLockId()) && !bot->GetPlayerbotAI()->HasSkill(SKILL_SURVIVAL))
        return false;

    for (int effIndex = 0; effIndex <= EFFECT_INDEX_2; effIndex++)
    {
        if (pSpellInfo->Effect[effIndex] != SPELL_EFFECT_OPEN_LOCK && pSpellInfo->Effect[effIndex] != SPELL_EFFECT_SKINNING)
            return false;

        uint32 lockId = go->GetGOInfo()->GetLockId();
        if (!lockId)
            return false;

        LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
            return false;

        bool reqKey = false;                                    // some locks not have reqs

        for(int j = 0; j < 8; ++j)
        {
            switch(lockInfo->Type[j])
            {
            /*
            case LOCK_KEY_ITEM:
                return true;
            */
            case LOCK_KEY_SKILL:
                {
                    if(uint32(pSpellInfo->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                        continue;

                    uint32 skillId = SkillByLockType(LockType(lockInfo->Index[j]));
                    if (skillId == SKILL_NONE)
                        return true;

                    if (CanOpenLock(skillId, lockInfo->Skill[j]))
                        return true;
                }
            }
        }
    }

    return false;
}

bool OpenLootAction::CanOpenLock(uint32 skillId, uint32 reqSkillValue)
{
    uint32 skillValue = bot->GetSkillValue(skillId);
    return skillValue >= reqSkillValue || !reqSkillValue;
}

bool StoreLootAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    WorldPacket p(event.getPacket()); // (8+1+4+1+1+4+4+4+4+4+1)
    ObjectGuid guid;
    uint8 loot_type;
    uint32 gold = 0;
    uint8 items = 0;

    p.rpos(0);
    p >> guid;      // 8 corpse guid
    p >> loot_type; // 1 loot type

    if (p.size() > 10)
    {
        p >> gold;      // 4 money on corpse
        p >> items;     // 1 number of items on corpse
    }

    bot->SetLootGuid(guid);

    // TEMP DIAG (Princess/quest-drop hunt): record every OPEN of creature 330's corpse with the
    // packet contents summary, so "never opens corpse" vs "opens but collar missing from view" is
    // distinguishable. Remove after the quest-loot root cause is fixed.
    if (guid.IsCreature() && guid.GetEntry() == 330)
    {
        FILE* qf = fopen("logs/questloot_diag.csv", "a");
        if (!qf) qf = fopen("../logs/questloot_diag.csv", "a");
        if (qf)
        {
            fprintf(qf, "%s,%s,item=0,slotType=0,idx=0,OPEN330:gold=%u items=%u lootType=%u\n",
                sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), gold, (uint32)items, (uint32)loot_type);
            fclose(qf);
        }
    }

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "lootGuid=" << guid.GetCounter()
            << " lootType=" << (uint32)loot_type
            << " gold=" << gold
            << " items=" << (uint32)items
            << " combat=" << (AI_VALUE2(bool, "combat", "self target") ? 1 : 0)
            << " attackers=" << (AI_VALUE(bool, "has attackers") ? 1 : 0)
            << " currentTargetAlive=" << (currentTarget && !sServerFacade.UnitIsDead(currentTarget) ? 1 : 0)
            << " currentTarget=" << (currentTarget ? currentTarget->GetName() : "none");
        sPlayerbotAIConfig.logEvent(ai, "StoreLootCombatTrace", std::to_string(guid.GetCounter()), out.str());
    }

    // Short SMSG_LOOT_RESPONSE (guid + type=0 + error byte) = Player::SendLootError refusal
    // (TOO_FAR, DIDNT_KILL, ...). Drop the loot target so the bot re-plans instead of keeping a
    // corpse the core just refused (measured: refused bots spun REFRESH on the corpse forever).
    if (loot_type == 0 && p.size() <= 10)
    {
        uint8 lootError = 0;
        if (p.size() == 10)
            p >> lootError;
        AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
        bot->SetLootGuid(ObjectGuid());
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            sPlayerbotAIConfig.logEvent(ai, "LootOpenError", std::to_string(guid.GetCounter()),
                "err=" + std::to_string((uint32)lootError));
        return false;
    }

    if (HasActiveCombatPressure(ai, bot, currentTarget))
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "lootGuid=" << guid.GetCounter()
                << " combat=" << (AI_VALUE2(bool, "combat", "self target") ? 1 : 0)
                << " attackers=" << (AI_VALUE(bool, "has attackers") ? 1 : 0)
                << " currentTargetAlive=" << (currentTarget && !sServerFacade.UnitIsDead(currentTarget) ? 1 : 0)
                << " currentTarget=" << (currentTarget ? currentTarget->GetName() : "none")
                << " reason=store-loot-during-combat";
            sPlayerbotAIConfig.logEvent(ai, "StoreLootDeferredCombat", std::to_string(guid.GetCounter()), out.str());
        }

        // DEFER, do NOT abandon: keep the corpse in "available loot" so the bot comes back and loots
        // it the moment combat clears, instead of permanently forgetting it (the old Remove(guid) here
        // is what made bots earn 0 gold). Just close the open loot window so we're not stuck kneeling
        // in the loot animation mid-fight, and clear the CURRENT loot target so combat actions run.
        RESET_AI_VALUE(LootObject, "loot target");
        // Release by EXPLICIT guid. HandleLootReleaseOpcode ignores the packet's guid and
        // releases bot->GetLootGuid() instead, so clearing the loot guid first (as the old
        // code did here) made the release a NO-OP.
        if (WorldSession* session = bot->GetSession())
            session->DoLootRelease(guid);
        return false;
    }

    Loot* loot = sLootMgr.GetLoot(bot);

    if (!loot)
    {
        const time_t suppressUntil = time(0) + PLAYERBOT_EMPTY_LOOT_RETRY_DELAY;
        SET_AI_VALUE2(time_t, "manual time", GetLootSkipQualifier(guid), suppressUntil);
        SuppressLootGuid(guid, suppressUntil);

        LogLootTrace(ai, bot, "StoreLootMissingLoot", guid,
            "lootType=" + std::to_string(uint32(loot_type)) +
            " gold=" + std::to_string(gold) +
            " items=" + std::to_string(uint32(items)) +
            " suppressFor=" + std::to_string(uint32(PLAYERBOT_EMPTY_LOOT_RETRY_DELAY)));

        AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);
        RESET_AI_VALUE(LootObject, "loot target");
        RESET_AI_VALUE2(bool, "should loot object", std::to_string(guid.GetRawValue()));
        if (WorldSession* session = bot->GetSession())
            session->DoLootRelease(guid);   // explicit guid; see note above
        ClearActiveLootState(ai, bot, guid);

        LogLootTrace(ai, bot, "StoreLootReleased", guid,
            "accelerateRespawn=0 reason=missing-loot suppressUntil=" + std::to_string(uint32(suppressUntil)));
        return false;
    }

    uint32 lootedItems = 0;
    uint32 skippedSlotType = 0;
    uint32 skippedStrategy = 0;
    uint32 skippedSpace = 0;
    uint32 skippedProto = 0;
    uint32 skippedMissingSlot = 0;
    uint32 skippedBlocked = 0;

    if (gold > 0)
    {
        WorldPacket packet(CMSG_LOOT_MONEY, 0);
        bot->GetSession()->HandleLootMoneyOpcode(packet);
    }

    for (uint8 i = 0; i < items; ++i)
    {
        uint32 itemid;
        uint32 randomPropertyId;
        uint32 itemcount;
        uint8 lootslot_type;
        uint8 itemindex;
        bool grab = false;

        p >> itemindex;
        p >> itemid;
        p >> itemcount;
        p.read_skip<uint32>();  // display id
        p.read_skip<uint32>();  // randomSuffix
        p >> randomPropertyId;  // randomPropertyId
        p >> lootslot_type;     // 0 = can get, 1 = look only, 2 = master get

        ItemQualifier itemQualifier(itemid, ((int32)randomPropertyId));

        // QUEST-LOOT DIAG (always-on): quest-needed items were NEVER obtained fleet-wide (45 bots
        // stuck on 'Princess Must Die!', 0 completions ever) -- log every decision this loop makes
        // about an item the bot needs for a quest, so the failing step is provable.
        const bool qdiag = ItemUsageValue::IsNeededForQuest(bot, itemid);
        auto qlog = [&](char const* verdict)
        {
            if (!qdiag) return;
            FILE* f = fopen("logs/questloot_diag.csv", "a");
            if (!f) f = fopen("../logs/questloot_diag.csv", "a");
            if (f)
            {
                fprintf(f, "%s,%s,item=%u,slotType=%u,idx=%u,%s\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
                    bot->GetName(), itemid, (uint32)lootslot_type, (uint32)itemindex, verdict);
                fclose(f);
            }
        };

        if (lootslot_type != LOOT_SLOT_NORMAL
#ifndef MANGOSBOT_ZERO
		        && lootslot_type != LOOT_SLOT_OWNER
#endif
            )
        {
            qlog("SKIP:slotType");
            ++skippedSlotType;
			continue;
        }

        bool isGameObjectLoot = guid.IsGameObject();
        bool isQuestRequired = ItemUsageValue::IsNeededForQuest(bot, itemid);

        // Bots must drain gameobject loot they open. Leaving a skipped item in a
        // chest/quest object makes core keep the GO activated and future users see
        // "That is already being used." Corpse loot can still follow bot strategy.
        if (!isGameObjectLoot && loot_type != LOOT_SKINNING && !IsLootAllowed(itemQualifier, ai))
        {
            qlog("SKIP:strategy");
            ++skippedStrategy;
            continue;
        }

        ItemPrototype const *proto = sItemStorage.LookupEntry<ItemPrototype>(itemid);
        if (!proto)
        {
            qlog("SKIP:proto");
            ++skippedProto;
            continue;
        }

        if (!CanStoreLootItem(bot, itemid, itemcount))
        {
            bool freedSlot = (isQuestRequired || isGameObjectLoot) && FreeJunkSlotForQuestLoot(ai, bot, itemid, guid);

            if (!CanStoreLootItem(bot, itemid, itemcount))
            {
                qlog("SKIP:space");
                ++skippedSpace;
                LogLootTrace(ai, bot, "StoreLootSkippedSpace", guid,
                    "item=" + std::to_string(itemid) +
                    " name=" + proto->Name1 +
                    " count=" + std::to_string(itemcount) +
                    " questRequired=" + std::to_string(isQuestRequired ? 1 : 0) +
                    " gameObjectLoot=" + std::to_string(isGameObjectLoot ? 1 : 0) +
                    " freedSlot=" + std::to_string(freedSlot ? 1 : 0));
                continue;
            }
        }

        QuestItem* qitem = nullptr;
        QuestItem* ffaitem = nullptr;
        QuestItem* conditem = nullptr;
        LootItem* lootItem = loot->LootItemInSlot(itemindex, bot->GetGUIDLow(), &qitem, &ffaitem, &conditem);

        if (!lootItem)
        {
            qlog("SKIP:missingSlot");
            ++skippedMissingSlot;
            continue;
        }

        // Match WorldSession::HandleAutostoreLootItemOpcode so virtual quest/FFA
        // slots are not mistaken for missing raw item slots.
        if (!lootItem->AllowedForPlayer(bot, loot->GetLootTarget()) || (!qitem && lootItem->is_blocked))
        {
            qlog("SKIP:blocked");
            ++skippedBlocked;
            continue;
        }
        qlog("STORE");

        Player* master = ai->GetMaster();
        if (sRandomPlayerbotMgr.IsRandomBot(bot) && master)
        {
            uint32 price = itemcount * ItemUsageValue::GetBotBuyPrice(proto, bot) + gold;
            if (price)
                sRandomPlayerbotMgr.AddTradeDiscount(bot, master, price);
        }

        WorldPacket packet(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        packet << itemindex;
        bot->GetSession()->HandleAutostoreLootItemOpcode(packet);

        if (proto->Quality > ITEM_QUALITY_NORMAL && !urand(0, 50) && ai->HasStrategy("emote", BotState::BOT_STATE_NON_COMBAT)) ai->PlayEmote(TEXTEMOTE_CHEER);
        if (proto->Quality >= ITEM_QUALITY_RARE && !urand(0, 1) && ai->HasStrategy("emote", BotState::BOT_STATE_NON_COMBAT)) ai->PlayEmote(TEXTEMOTE_CHEER);

        if (requester && (ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT) || (requester->GetMapId() != bot->GetMapId() || WorldPosition(requester).sqDistance2d(bot) > (sPlayerbotAIConfig.sightDistance * sPlayerbotAIConfig.sightDistance))))
        {
            std::map<std::string, std::string> args;
            args["%item"] = chat->formatItem(itemQualifier);
            ai->TellPlayerNoFacing(requester, BOT_TEXT2("loot_command", args), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        }

        sPlayerbotAIConfig.logEvent(ai, "StoreLootAction", proto->Name1, std::to_string(proto->ItemId));

        BroadcastHelper::BroadcastLootingItem(ai, bot, proto, itemQualifier);
        ++lootedItems;
    }

    LogLootTrace(ai, bot, "StoreLootSummary", guid,
        "lootType=" + std::to_string(uint32(loot_type)) +
        " gold=" + std::to_string(gold) +
        " items=" + std::to_string(uint32(items)) +
        " looted=" + std::to_string(lootedItems) +
        " skippedSlotType=" + std::to_string(skippedSlotType) +
        " skippedStrategy=" + std::to_string(skippedStrategy) +
        " skippedSpace=" + std::to_string(skippedSpace) +
        " skippedProto=" + std::to_string(skippedProto) +
        " skippedMissingSlot=" + std::to_string(skippedMissingSlot) +
        " skippedBlocked=" + std::to_string(skippedBlocked));

    AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);
    RESET_AI_VALUE(LootObject, "loot target");
    RESET_AI_VALUE2(bool, "should loot object", std::to_string(guid.GetRawValue()));
    // Release by EXPLICIT guid (see note in the combat-defer path above). The old
    // SetLootGuid(empty)-then-HandleLootReleaseOpcode order made the release a no-op,
    // so a chest the bot only PARTIALLY looted (it skips gray/unneeded items) was never
    // unlocked -- stuck "Still being used" for everyone.
    if (WorldSession* session = bot->GetSession())
        session->DoLootRelease(guid);
    ClearActiveLootState(ai, bot, guid);

    LogLootTrace(ai, bot, "StoreLootReleased", guid, "accelerateRespawn=1");

    ai->AccelerateRespawn(guid);

    return true;
}

bool StoreLootAction::IsLootAllowed(ItemQualifier& itemQualifier, PlayerbotAI *ai)
{
    AiObjectContext *context = ai->GetAiObjectContext();
    
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemQualifier.GetId());
    if (!proto)
        return false;

    std::set<uint32>& lootItems = AI_VALUE(std::set<uint32>&, "always loot list");
    if (lootItems.find(itemQualifier.GetId()) != lootItems.end())
        return true;

    std::set<uint32>& skipItems = AI_VALUE(std::set<uint32>&, "skip loot list");
    if (skipItems.find(itemQualifier.GetId()) != skipItems.end())
        return false;

    uint32 max = proto->MaxCount;
    if (max > 0 && ai->GetBot()->HasItemCount(itemQualifier.GetId(), max, true))
        return false;

    if (proto->StartQuest)
    {
        if (sPlayerbotAIConfig.syncQuestWithPlayer)
            return false; //Quest is autocomplete for the bot so no item needed.
        else
            return true;
    }

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 entry = ai->GetBot()->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr.GetQuestTemplate(entry);
        if (!quest)
            continue;

        for (int i = 0; i < 4; i++)
        {
            if (quest->ReqItemId[i] == itemQualifier.GetId())
            {
                if (ai->GetMaster() && sPlayerbotAIConfig.syncQuestWithPlayer)
                    return false; //Quest is autocomplete for the bot so no item needed.

                if (AI_VALUE2(uint32, "item count", proto->Name1) >= quest->ReqItemCount[i])
                    return false;
            }
        }
    }

    //if (proto->Bonding == BIND_QUEST_ITEM ||  //Still testing if it works ok without these lines.
    //    proto->Bonding == BIND_QUEST_ITEM1 || //Eventually this has to be removed.
    //    proto->Class == ITEM_CLASS_QUEST)
    //{

    bool canLoot = LootStrategyValue::CanLoot(itemQualifier, ai);

    //if (canLoot && proto->Bonding == BIND_WHEN_PICKED_UP && ai->HasActivePlayerMaster())
    //    canLoot = sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(ai->GetBot()->GetObjectGuid()));

    return canLoot;
}

bool ReleaseLootAction::Execute(Event& event)
{
    ObjectGuid previousLootGuid = bot->GetLootGuid();

    // Release ONLY the loot the bot itself currently has open. The old code swept over all
    // nearby gameobjects/corpses sending CMSG_LOOT_RELEASE, but HandleLootReleaseOpcode
    // ignores the packet guid and only releases bot->GetLootGuid() -- so the sweep was a
    // no-op past the first object anyway. Releasing arbitrary nearby objects by guid would
    // be actively harmful: DoLootRelease on a chest the bot is NOT looting runs its
    // "not fully looted" branch -> SetLootState(GO_ACTIVATED), LOCKING it for everyone.
    if (previousLootGuid)
    {
        if (WorldSession* session = bot->GetSession())
            session->DoLootRelease(previousLootGuid);
    }

    bot->SetLootGuid(ObjectGuid());
    LogLootTrace(ai, bot, "ReleaseLootSweep", previousLootGuid,
        "released=" + std::to_string(previousLootGuid.GetCounter()));
    return true;
}
