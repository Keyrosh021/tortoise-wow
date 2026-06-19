
#include "playerbot/playerbot.h"
#include "RepairAllAction.h"

#include "playerbot/ServerFacade.h"

using namespace ai;

namespace
{
    static constexpr time_t PLAYERBOT_REPAIR_NO_NPC_RETRY_DELAY = 30;
    const char* PLAYERBOT_REPAIR_NO_NPC_SKIP = "repair no npc skip";

    Creature* FindRepairNpc(Player* bot, PlayerbotAI* ai)
    {
        if (!bot || !ai)
            return nullptr;

        std::list<ObjectGuid> npcs = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs")->Get();
        for (ObjectGuid const& guid : npcs)
        {
            if (Creature* unit = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_REPAIR))
                return unit;
        }

        return nullptr;
    }
}

bool RepairAllAction::isPossible()
{
    if (FindRepairNpc(bot, ai))
        return true;

    return AI_VALUE2(time_t, "manual time", PLAYERBOT_REPAIR_NO_NPC_SKIP) <= time(0);
}

bool RepairAllAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
    for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Creature *unit = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_REPAIR);
        if (!unit)
            continue;

#ifdef MANGOS
        if(bot->hasUnitState(UNIT_STAT_DIED))
#endif
#ifdef CMANGOS
        if (bot->hasUnitState(UNIT_STAT_FEIGN_DEATH))
#endif
            bot->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

        sServerFacade.SetFacingTo(bot, unit);
        float discountMod = bot->GetReputationPriceDiscount(unit);

        float durability = AI_VALUE(uint8, "durability inventory");

        uint32 botMoney = bot->GetMoney();
        if (ai->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(10000000);
        }

        //Repair weapons first.
        uint32 totalCost = bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND, true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        totalCost += bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_RANGED, true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        totalCost += bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_OFFHAND, true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        totalCost += bot->DurabilityRepairAll(true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        if (ai->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        //Totalcost is bugged in core. For now we use this work-around.
        totalCost = botMoney - bot->GetMoney();

        if (totalCost > 0)
        {
            std::ostringstream out;
            out << "Repair: " << chat->formatMoney(totalCost) << " (" << unit->GetName() << ")";
            ai->TellPlayerNoFacing(requester, out.str(),PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            if (sPlayerbotAIConfig.globalSoundEffects)      
                bot->PlayDistanceSound(7994);

            sPlayerbotAIConfig.logEvent(ai, "RepairAllAction", std::to_string(durability), std::to_string(totalCost));

            ai->DoSpecificAction("equip upgrades", event, true);
        }

        SET_AI_VALUE(uint32, "death count", 0);
        RESET_AI_VALUE(uint8, "durability inventory");

        return durability < 100 && AI_VALUE(uint8, "durability inventory") > durability;
    }

    SET_AI_VALUE2(time_t, "manual time", PLAYERBOT_REPAIR_NO_NPC_SKIP, time(0) + PLAYERBOT_REPAIR_NO_NPC_RETRY_DELAY);

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "durability=" << uint32(AI_VALUE(uint8, "durability"))
            << " lowest=" << uint32(AI_VALUE(uint8, "lowest durability"))
            << " inventory=" << uint32(AI_VALUE(uint8, "durability inventory"))
            << " suppressFor=" << uint32(PLAYERBOT_REPAIR_NO_NPC_RETRY_DELAY);
        sPlayerbotAIConfig.logEvent(ai, "RepairDeferredNoNpc", "", out.str());
    }

    if (requester && ai->HasRealPlayerMaster())
        ai->TellPlayerNoFacing(requester, "Cannot find any npc to repair at");

    return false;
}
