#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "Formulas.h"
#include "QuestDef.h"

namespace mind
{
    // How far the mind scans for kill targets / owed corpses. Kept inside
    // visibility so everything the bot reacts to is something a nearby player
    // could also see it react to.
    static constexpr float KILL_SCAN_RANGE = 60.0f;
    static constexpr float LOOT_SCAN_RANGE = 45.0f;

    bool BotMind::KillHelpsQuest(uint32 creatureEntry) const
    {
        for (const auto& qs : bot->getQuestStatusMap())
        {
            if (qs.second.m_status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr.GetQuestTemplate(qs.first);
            if (!quest)
                continue;

            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (quest->ReqCreatureOrGOId[i] == (int32)creatureEntry &&
                    qs.second.m_creatureOrGOcount[i] < quest->ReqCreatureOrGOCount[i])
                    return true;
            }
        }
        return false;
    }

    bool BotMind::IsUsableKillTarget(Unit* u) const
    {
        if (!u || !u->IsInWorld() || u->GetMapId() != bot->GetMapId() || !u->IsAlive())
            return false;

        if (!bot->IsValidAttackTarget(u))
            return false;

        Creature* c = u->ToCreature();
        if (!c)
            return false;   // the mind grinds creatures; pvp stays a legacy-engine feature

        if (c->IsCritter() || c->IsPet())
            return false;

        // Solo bots don't pull elites; a dead bot generates nothing but a death stat.
        if (!bot->GetGroup() && c->GetCreatureInfo() && c->GetCreatureInfo()->Rank > CREATURE_ELITE_NORMAL)
            return false;

        // Don't chase mobs meaningfully above us (red-mob deaths) — cap at +3.
        if ((int32)c->GetLevel() > (int32)bot->GetLevel() + 3)
            return false;

        // Grey/no-xp mobs are pointless UNLESS a quest needs them (Goal 2).
        if (!MaNGOS::XP::Gain(bot, c) && !KillHelpsQuest(c->GetEntry()))
            return false;

        // Tapped by someone who isn't us: their kill, their loot. Move on.
        if (c->GetLootRecipientGuid() && !c->IsTappedBy(bot))
            return false;

        // Already fighting someone else's fight (don't pile onto another
        // player's mob unless it is fighting us or our group).
        if (Unit* victim = c->GetVictim())
        {
            Player* vp = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (vp && vp != bot && (!bot->GetGroup() || !vp->GetGroup() || vp->GetGroup() != bot->GetGroup()))
                return false;
        }

        return true;
    }

    Unit* BotMind::PinnedOrBestTarget(uint32 now)
    {
        // STICKY: the pinned target stays the target while it is usable.
        // Re-scanning "just in case" is exactly the flip-flop we removed.
        if (targetGuid)
        {
            Unit* u = ai->GetUnit(targetGuid);
            if (IsUsableKillTarget(u) && !IsBlacklisted(targetGuid, now) &&
                sServerFacade.GetDistance2d(bot, u) <= KILL_SCAN_RANGE * 1.5f)
                return u;
            targetGuid = ObjectGuid();
        }

        // Promote the target queued during the last fight (Goal 4: the bot
        // already knows what it does after this mob dies).
        if (nextTargetGuid)
        {
            Unit* u = ai->GetUnit(nextTargetGuid);
            nextTargetGuid = ObjectGuid();
            if (IsUsableKillTarget(u) && !IsBlacklisted(u->GetObjectGuid(), now))
            {
                targetGuid = u->GetObjectGuid();
                return u;
            }
        }

        if (now < targetScanAt)
            return nullptr;
        targetScanAt = now + 1200;

        std::list<Unit*> candidates;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(bot, KILL_SCAN_RANGE);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(candidates, check);
        Cell::VisitAllObjects(bot, searcher, KILL_SCAN_RANGE);

        Unit* best = nullptr;
        float bestScore = 1e9f;
        for (Unit* u : candidates)
        {
            if (!IsUsableKillTarget(u) || IsBlacklisted(u->GetObjectGuid(), now))
                continue;

            float score = sServerFacade.GetDistance2d(bot, u);
            if (KillHelpsQuest(u->GetEntry()))
                score -= 40.0f;   // quest credit beats proximity (Goal 1: quests/hr)
            if (score < bestScore)
            {
                bestScore = score;
                best = u;
            }
        }

        if (best)
            targetGuid = best->GetObjectGuid();
        return best;
    }

    Creature* BotMind::LootOwed(uint32 now)
    {
        // Sticky committed corpse first.
        if (lootGuid)
        {
            Creature* c = ai->GetCreature(lootGuid);
            if (c && c->IsInWorld() && !c->IsAlive() &&
                c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE) &&
                bot->IsAllowedToLoot(c) && !IsBlacklisted(lootGuid, now))
                return c;
            lootGuid = ObjectGuid();
        }

        if (now < lootScanAt)
            return nullptr;
        lootScanAt = now + 1000;

        // Scan dead creatures we hold loot rights on. This is what makes
        // group round-robin loot work: whoever the loot was AWARDED to walks
        // over and takes it, so no group kill is wasted (Goal 4).
        std::list<Creature*> corpses;
        MaNGOS::AllCreaturesInRange check(bot, LOOT_SCAN_RANGE);
        MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesInRange> searcher(corpses, check);
        Cell::VisitGridObjects(bot, searcher, LOOT_SCAN_RANGE);

        Creature* best = nullptr;
        float bestDist = 1e9f;
        for (Creature* c : corpses)
        {
            if (!c || c->IsAlive() || !c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
                continue;
            if (!bot->IsAllowedToLoot(c) || IsBlacklisted(c->GetObjectGuid(), now))
                continue;

            float d = sServerFacade.GetDistance2d(bot, c);
            if (d < bestDist)
            {
                bestDist = d;
                best = c;
            }
        }

        if (best)
            lootGuid = best->GetObjectGuid();
        return best;
    }

    bool BotMind::IsBlacklisted(const ObjectGuid& g, uint32 now) const
    {
        for (const BlEntry& e : blacklist)
            if (e.g == g && now < e.until)
                return true;
        return false;
    }

    void BotMind::AddBlacklist(const ObjectGuid& g, uint32 now, uint32 forMs)
    {
        blacklist[blCursor] = { g, now + forMs };
        blCursor = (blCursor + 1) % 8;
        Log().blacklists.fetch_add(1, std::memory_order_relaxed);
    }
}
