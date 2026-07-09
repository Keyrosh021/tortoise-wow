#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "QuestDef.h"

namespace mind
{
    static constexpr float ERRAND_SCAN_RANGE = 80.0f;
    static constexpr float INTERACT_DISTANCE_SNUG = 3.5f;

    // Does this NPC end a quest the bot has COMPLETE in its log?
    static bool HasTurnInFor(Player* bot, Creature* npc)
    {
        QuestRelationsMapBounds bounds =
            sObjectMgr.GetCreatureQuestInvolvedRelationsMapBounds(npc->GetEntry());
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            const uint32 questId = it->second;
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE &&
                !bot->GetQuestRewardStatus(questId))
                return true;
        }
        return false;
    }

    // Does this NPC offer a quest the bot can and should take?
    static bool HasPickupFor(Player* bot, Creature* npc)
    {
        QuestRelationsMapBounds bounds =
            sObjectMgr.GetCreatureQuestRelationsMapBounds(npc->GetEntry());
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            const uint32 questId = it->second;
            Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
            if (!quest)
                continue;
            if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE || bot->GetQuestRewardStatus(questId))
                continue;
            if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
                continue;
            // Don't hoard grey quests: reward must still be worth the bot's time.
            if ((int32)quest->GetQuestLevel() < (int32)bot->GetLevel() - 8)
                continue;
            return true;
        }
        return false;
    }

    // ERRAND: a quest hand-in or pickup within walking distance. This is what
    // turns kills into quests/hr — the mind walks the bot TO the questgiver
    // with commitment, then runs the existing dialog actions (which handle
    // reward choice, shareable quests, log-full, etc.).
    Verdict BotMind::StepErrand(uint32 now)
    {
        // Committed errand in progress.
        if (errandNpc)
        {
            Creature* npc = ai->GetCreature(errandNpc);
            if (!npc || !npc->IsInWorld() || !npc->IsAlive() || IsBlacklisted(errandNpc, now))
            {
                errandNpc = ObjectGuid();
            }
            else
            {
                SetGoal(Goal::Errand, now);
                const float dist = sServerFacade.GetDistance2d(bot, npc);
                if (dist > INTERACT_DISTANCE_SNUG)
                {
                    if (MovingBlocked(now))
                    {
                        AddBlacklist(errandNpc, now, 120000);
                        errandNpc = ObjectGuid();
                        return { true, false, 500 };
                    }
                    MoveTowards(npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(), now);
                    return { true, true, 600 };
                }

                // At the NPC. Pace the dialog like a person, one step per beat.
                if (now < errandStageAt)
                    return { true, false, 400 };
                errandStageAt = now + 1500;

                if (HasTurnInFor(bot, npc))
                {
                    WorldPacket p(CMSG_QUESTGIVER_COMPLETE_QUEST);
                    p << npc->GetObjectGuid();
                    p.rpos(0);
                    Event e("rpg action", p);
                    const bool acted = ai->DoSpecificAction("talk to quest giver", e, true);
                    if (acted)
                    {
                        lastProductiveAt = now;
                        Log().errands.fetch_add(1, std::memory_order_relaxed);
                    }
                    return { true, acted, 1200 };
                }

                if (HasPickupFor(bot, npc))
                {
                    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
                    p << npc->GetObjectGuid();
                    p.rpos(0);
                    Event e("rpg action", p);
                    const bool acted = ai->DoSpecificAction("accept all quests", e, true);
                    if (acted)
                        Log().errands.fetch_add(1, std::memory_order_relaxed);
                    return { true, acted, 1200 };
                }

                // Nothing (left) to do here — errand complete.
                errandNpc = ObjectGuid();
                nextErrandScanAt = now + 20000;
                return { true, false, 400 };
            }
        }

        // Look for a new errand only on a slow cadence — questgiver scans are
        // not free and a bot mid-grind shouldn't sprint to a giver every kill.
        if (now < nextErrandScanAt)
            return { false, false, 0 };
        nextErrandScanAt = now + 15000;

        std::list<Creature*> npcs;
        MaNGOS::AllCreaturesInRange check(bot, ERRAND_SCAN_RANGE);
        MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesInRange> searcher(npcs, check);
        Cell::VisitGridObjects(bot, searcher, ERRAND_SCAN_RANGE);

        Creature* best = nullptr;
        int bestScore = 0;
        for (Creature* npc : npcs)
        {
            if (!npc || !npc->IsAlive() || IsBlacklisted(npc->GetObjectGuid(), now))
                continue;
            if (!npc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
                continue;
            if (!sServerFacade.IsFriendlyTo(npc, bot))
                continue;

            int score = 0;
            if (HasTurnInFor(bot, npc))
                score = 3;                         // hand-ins first: banked xp/gold
            else if (HasPickupFor(bot, npc))
                score = 2;
            if (score > bestScore)
            {
                bestScore = score;
                best = npc;
            }
        }

        if (!best)
            return { false, false, 0 };

        SetGoal(Goal::Errand, now);
        lastProductiveAt = now;
        errandNpc = best->GetObjectGuid();
        errandStageAt = 0;
        ResetStuck(now);
        return { true, true, 400 };
    }
}
