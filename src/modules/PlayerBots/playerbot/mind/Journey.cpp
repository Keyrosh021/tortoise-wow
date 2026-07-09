#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"

#include <cmath>

namespace mind
{
    static constexpr float ARRIVE_DISTANCE = 25.0f;    // mobs are now in kill-scan range

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

    // JOURNEY: nothing to fight, loot or hand in HERE -> be somewhere it can
    // happen. Walk to the nearest real mob camp (spawn cache); when the map
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
            // Randomize the "nearest" pick and scatter the arrival point:
            // with a fixed minDist every bot in an area resolves to the SAME
            // camp coordinate and piles up on it (observed: 20+ levelers
            // standing on one spot in Westfall). Varying minDist spreads
            // bots across different camps; the offset spreads them within one.
            const float pickDist = 60.0f + (float)urand(0, 120);
            if (sRandomPlayerbotMgr.GetNearestGrindSpot(bot, pickDist, nx, ny, nz))
            {
                const float ang = frand(0, 2 * M_PI_F);
                const float off = frand(8.0f, 30.0f);
                destX = nx + std::cos(ang) * off;
                destY = ny + std::sin(ang) * off;
                destZ = nz;
                destPickAt = now + 20000;
                ResetStuck(now);
                pickedNew = true;
            }
            else
            {
                destX = destY = destZ = 0.f;
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
            return { true, true, 1000 };
        }

        MoveTowards(destX, destY, destZ, now);
        return { true, true, 500 };
    }
}
