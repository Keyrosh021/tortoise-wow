#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TravelMgr.h"
#include "QuestDef.h"

#include <cmath>
#include <vector>

namespace mind
{
    static constexpr float ARRIVE_DISTANCE = 25.0f;    // mobs are now in kill-scan range

    // Objective areas beyond this are a different trip, not a walk: leave
    // them to the relocate/dispersal machinery (no cross-map hiking).
    static constexpr float QUEST_DEST_RANGE = 1500.0f;
    static constexpr uint32 QUEST_DEST_RESOLVE_MS = 45000;

    bool BotMind::SeenByPlayer(float range) const
    {
        auto rp = sRandomPlayerbotMgr.GetRealPlayerSnapshot();
        if (!rp)
            return false;
        const float bx = bot->GetPositionX(), by = bot->GetPositionY();
        for (auto const& p : rp->players)
        {
            if (p.guidLow == 0xFFFFFFFEu)      // virtual observer -> not a real player
                continue;
            if (p.mapId != bot->GetMapId())
                continue;
            const float dx = p.x - bx, dy = p.y - by;
            if (dx * dx + dy * dy <= range * range)
                return true;
        }
        return false;
    }

    // Where an unfinished objective of one of our quests can actually be
    // progressed: the nearest cached spawn point of a creature/GO that gives
    // credit (kill, drop or use) for an INCOMPLETE, non-grey quest in the
    // log. This is the piece that turns journeys into quests/hr: measured
    // fleet-wide only +24 rewarded quests in ~1.2h while journeys aimed at
    // generic grind camps — the -40 quest-credit scan bonus (Percept.cpp)
    // only fires within 60y, so the journey must AIM at the objective area
    // for it to ever matter.
    //
    // Thread-safety (this runs on map threads): GetDestinations with
    // onlyPossible=false is a pure const walk over the destination map built
    // once in TravelMgr::LoadQuestTravelTable — no IsPossible/IsActive (those
    // read AI values), no GetPartitions worker lock, and none of the
    // area-level/vmap lazy loads that made GetPartitions async-thread-only
    // (see the ShouldBypassTravelLevelGate note in TravelMgr.cpp). The quest
    // log test mirrors NeedQuestObjectiveValue but reads only our own status
    // map + immutable quest templates (the KillHelpsQuest pattern).
    bool BotMind::QuestObjectiveDest(uint32 now, float& outX, float& outY, float& outZ)
    {
        // A dispersal/stuck teleport moved us to another map: the cached
        // point is garbage by definition — drop it and allow one immediate
        // re-resolve on the new map instead of walking camps for up to 45s.
        if (questDestFound && questDestMapId != bot->GetMapId())
        {
            questDestFound = false;
            questDestResolveAt = 0;
        }

        if (now < questDestResolveAt)
        {
            if (!questDestFound)
                return false;
            const float dx = questDestX - bot->GetPositionX();
            const float dy = questDestY - bot->GetPositionY();
            if (dx * dx + dy * dy > QUEST_DEST_RANGE * QUEST_DEST_RANGE)
                return false;              // drifted out of walking range since the resolve
            outX = questDestX; outY = questDestY; outZ = questDestZ;
            return true;
        }
        questDestResolveAt = now + QUEST_DEST_RESOLVE_MS;
        questDestFound = false;

        // Which quests still have an unfinished objective worth walking to?
        std::vector<int32> questIds;
        std::vector<std::pair<uint32, uint8>> needed;   // questId -> unfinished objective bits
        uint32 purposeMask = 0;
        for (const auto& qs : bot->getQuestStatusMap())
        {
            if (qs.second.m_status != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* quest = sObjectMgr.GetQuestTemplate(qs.first);
            if (!quest)
                continue;
            // Grey quests aren't worth the trip (same cut as the Errands pickup filter).
            if ((int32)quest->GetQuestLevel() < (int32)bot->GetLevel() - 8)
                continue;
            // Objectives above our level end in orange/red-mob deaths.
            if ((int32)quest->GetQuestLevel() > (int32)bot->GetLevel() + 1)
                continue;
            // Solo bots can't clear elite/dungeon/raid objective camps.
            if (!bot->GetGroup() &&
                (quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON ||
                 quest->GetType() == QUEST_TYPE_RAID))
                continue;

            uint8 objBits = 0;
            for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
            {
                const bool needItems = quest->ReqItemCount[j] &&
                    qs.second.m_itemcount[j] < quest->ReqItemCount[j];
                const bool needKills = quest->ReqCreatureOrGOCount[j] &&
                    qs.second.m_creatureOrGOcount[j] < quest->ReqCreatureOrGOCount[j];
                if (needItems || needKills)
                {
                    objBits |= (1 << j);
                    purposeMask |= (uint32)TravelDestinationPurpose::QuestObjective1 << j;
                }
            }
            if (objBits)
            {
                questIds.push_back((int32)qs.first);
                needed.push_back({ qs.first, objBits });
            }
        }
        if (questIds.empty())
            return false;

        // Nearest same-map spawn point across every unfinished objective
        // (quest destinations are keyed by questId, so the entries filter
        // narrows the walk to OUR quests). onlyPossible MUST stay false —
        // see the thread-safety note above.
        const uint32 mapId = bot->GetMapId();
        const float bx = bot->GetPositionX(), by = bot->GetPositionY();
        float bestD2 = QUEST_DEST_RANGE * QUEST_DEST_RANGE;
        for (TravelDestination* dest :
             sTravelMgr.GetDestinations(PlayerTravelInfo(), purposeMask, questIds, /*onlyPossible*/ false, /*maxDistance*/ 0.0f))
        {
            // Everything stored under an objective purpose IS an objective destination.
            QuestObjectiveTravelDestination* qDest = static_cast<QuestObjectiveTravelDestination*>(dest);

            // The purpose mask is fleet-of-quests wide; re-check THIS quest
            // still needs THIS objective (obj 1 done / obj 2 pending case).
            uint8 objBits = 0;
            for (const auto& need : needed)
                if (need.first == qDest->GetQuestId()) { objBits = need.second; break; }
            if (!(objBits & (1 << qDest->GetObjective())))
                continue;

            for (WorldPosition* p : qDest->GetPoints())
            {
                if (p->getMapId() != mapId)
                    continue;
                const float dx = p->getX() - bx, dy = p->getY() - by;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= bestD2)
                    continue;
                bestD2 = d2;
                questDestX = p->getX(); questDestY = p->getY(); questDestZ = p->getZ();
                questDestFound = true;
            }
        }
        if (!questDestFound)
            return false;

        questDestMapId = mapId;
        outX = questDestX; outY = questDestY; outZ = questDestZ;
        return true;
    }

    // JOURNEY: nothing to fight, loot or hand in HERE -> be somewhere it can
    // happen. Walk to the nearest unfinished quest-objective area first
    // (quests/hr is the goal, and camps only complete quests by accident);
    // otherwise the nearest real mob camp (spawn cache); when the map
    // genuinely has nothing for our level and nobody can see us, teleport to
    // level-appropriate content (dispersal — Goal 0 keeps bots in zones that
    // match their bracket). A bot on this path is ALWAYS visibly walking with
    // a purpose; there is no stand-and-wait branch.
    Verdict BotMind::StepJourney(uint32 now)
    {
        SetGoal(Goal::Journey, now);
        Log().journey.fetch_add(1, std::memory_order_relaxed);

        // UNPRODUCTIVE-TERRAIN RELOCATE: measured live (run 7): 59% of scan
        // candidates around journeying bots are other bot players, 30% grey
        // mobs — walking on this map earns nothing. If nothing productive
        // (engage/loot/errand/combat) happened for 150s, jump to
        // level-matched content instead of hiking through dead country.
        if (!lastProductiveAt)
            lastProductiveAt = now;
        if (now - lastProductiveAt > 150000 && !SeenByPlayer(180.0f) && !bot->IsInCombat())
        {
            lastProductiveAt = now;
            destX = destY = destZ = 0.f;
            destPickAt = 0;
            destIsObjective = false;
            sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0.f, 0.f, 0.f, /*forLevel*/ true);
            Log().teleports.fetch_add(1, std::memory_order_relaxed);
            return { true, true, 1500 };
        }

        const float bx = bot->GetPositionX(), by = bot->GetPositionY();
        const bool haveDest = (destX != 0.f || destY != 0.f);
        const float dxd = destX - bx, dyd = destY - by;
        const float destDist = haveDest ? std::sqrt(dxd * dxd + dyd * dyd) : 1e9f;

        // (Re)pick a destination when we have none, arrived, or it went stale.
        bool pickedNew = false;
        if (!haveDest || destDist < ARRIVE_DISTANCE || now >= destPickAt)
        {
            if (haveDest && destDist < ARRIVE_DISTANCE)
                Log().arrivals.fetch_add(1, std::memory_order_relaxed);

            float nx, ny, nz;
            // QUEST OBJECTIVE FIRST: an unfinished objective of a logged
            // quest within walking range beats any generic camp — that is
            // what moves rewarded-quests/hr (cached lookup, ~45s cadence).
            const bool objective = QuestObjectiveDest(now, nx, ny, nz);
            bool haveSpot = objective;
            if (!haveSpot)
            {
                // Randomize the "nearest" pick and scatter the arrival point:
                // with a fixed minDist every bot in an area resolves to the SAME
                // camp coordinate and piles up on it (observed: 20+ levelers
                // standing on one spot in Westfall). Varying minDist spreads
                // bots across different camps; the offset spreads them within one.
                const float pickDist = 60.0f + (float)urand(0, 120);
                haveSpot = sRandomPlayerbotMgr.GetNearestGrindSpot(bot, pickDist, nx, ny, nz);
            }
            if (haveSpot)
            {
                const float ang = frand(0, 2 * M_PI_F);
                const float off = frand(8.0f, 30.0f);
                destX = nx + std::cos(ang) * off;
                destY = ny + std::sin(ang) * off;
                destZ = nz;
                destPickAt = now + 20000;
                destIsObjective = objective;
                ResetStuck(now);
                pickedNew = true;
            }
            else
            {
                destX = destY = destZ = 0.f;
                destIsObjective = false;
                if (!SeenByPlayer(180.0f) && !bot->IsInCombat())
                {
                    // Off-screen and this map has no content for us: jump to
                    // level-appropriate content (the dispersal mechanism).
                    sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0.f, 0.f, 0.f, /*forLevel*/ true);
                    Log().teleports.fetch_add(1, std::memory_order_relaxed);
                    return { true, true, 1500 };
                }
                // Watched: stay human — roam a short leg and rescan.
                const bool moved = ai->DoSpecificAction("move random", Event(), true);
                Log().idleFallback.fetch_add(1, std::memory_order_relaxed);
                return { true, moved, 1200 };
            }
        }

        // Blocked walking toward the camp: far + unseen -> stuck-escape
        // teleport to it; otherwise drop this camp and re-pick next tick.
        if (!pickedNew && MovingBlocked(now))
        {
            if (destDist > 200.0f && !SeenByPlayer(200.0f) && !bot->IsInCombat())
            {
                sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), bot->GetMapId(), destX, destY, destZ, false);
                Log().teleports.fetch_add(1, std::memory_order_relaxed);
            }
            destX = destY = destZ = 0.f;
            destPickAt = 0;
            destIsObjective = false;
            return { true, true, 1000 };
        }

        if (destIsObjective)
            Log().questJourney.fetch_add(1, std::memory_order_relaxed);
        MoveTowards(destX, destY, destZ, now);
        return { true, true, 500 };
    }
}
