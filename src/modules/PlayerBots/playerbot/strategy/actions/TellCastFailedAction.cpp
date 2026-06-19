
#include "playerbot/playerbot.h"
#include "TellCastFailedAction.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

bool TellCastFailedAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    WorldPacket p(event.getPacket());
    p.rpos(0);
    uint8 castCount, result;
    uint32 spellId;
    p >> castCount >> spellId >> result;
    ai->SpellInterrupted(spellId);

    if (result == SPELL_CAST_OK)
        return false;

    const SpellEntry *const pSpellInfo =  sServerFacade.LookupSpellInfo(spellId);
    if (pSpellInfo && bot && bot->getClass() == CLASS_PALADIN && sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::string spellName = pSpellInfo->SpellName[0];
        if (spellName == "Holy Light" || spellName == "Flash of Light")
        {
            std::ostringstream trace;
            trace << "spell=" << spellName
                  << " result=" << static_cast<uint32>(result)
                  << " health=" << static_cast<uint32>(bot->GetHealthPercent())
                  << " combat=" << (bot->IsInCombat() ? 1 : 0)
                  << " moving=" << (sServerFacade.isMoving(bot) ? 1 : 0)
                  << " attackers=" << AI_VALUE(uint8, "attacker count");
            sPlayerbotAIConfig.logEvent(ai, "PaladinHealCastFailed", std::to_string(spellId), trace.str());
        }
    }
    std::ostringstream out; out << chat->formatSpell(pSpellInfo) << ": ";
    switch (result)
    {
    case SPELL_FAILED_NOT_READY:
        out << "not ready";
        break;
    case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
        out << "requires spell focus";
        break;
    case SPELL_FAILED_REQUIRES_AREA:
        out << "cannot cast here";
        break;
    case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
        out << "requires item";
        break;
    case SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND:
    case SPELL_FAILED_EQUIPPED_ITEM_CLASS_OFFHAND:
        out << "requires weapon";
        break;
    case SPELL_FAILED_PREVENTED_BY_MECHANIC:
        out << "interrupted";
        break;
    default:
        out << "cannot cast";
    }
    int32 castTime = GetSpellCastTime(pSpellInfo
#ifdef CMANGOS
            , bot
#endif
    );
    if (castTime >= 2000)
        ai->TellError(requester, out.str());
    return true;
}

bool TellSpellAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string spell = event.getParam();
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId)
        return false;

    SpellEntry const *spellInfo = sServerFacade.LookupSpellInfo(spellId );
    if (!spellInfo)
        return false;

    std::ostringstream out; out << chat->formatSpell(spellInfo);
    ai->TellError(requester, out.str());
    return true;
}
