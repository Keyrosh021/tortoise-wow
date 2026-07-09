#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/ServerFacade.h"
#include "Movement/MotionMaster.h"

#include <cmath>

namespace mind
{
    // Far destinations are walked in ~120y pathfindable hops: MovePoint straight
    // at a cross-zone point fails to path and the bot just STANDS (the
    // L60-in-Stormwind bug). The hop keeps the bot visibly, continuously
    // walking toward content, and the next hop re-aims.
    static constexpr float HOP_DISTANCE = 120.0f;

    bool BotMind::MoveTowards(float x, float y, float z, uint32 now)
    {
        const float bx = bot->GetPositionX(), by = bot->GetPositionY();
        const float dx = x - bx, dy = y - by;
        const float dist = std::sqrt(dx * dx + dy * dy);

        float wx = x, wy = y, wz = z;
        if (dist > HOP_DISTANCE * 1.25f)
        {
            const float f = HOP_DISTANCE / dist;
            wx = bx + dx * f;
            wy = by + dy * f;
            wz = bot->GetPositionZ();   // pathfinder snaps to ground
        }

        // Only (re)issue the move when not already moving: MovePoint spam
        // restarts the spline every tick and reads as stutter-stepping.
        if (!sServerFacade.isMoving(bot))
            bot->GetMotionMaster()->MovePoint(bot->GetMapId(), wx, wy, wz,
                                              MOVE_PATHFINDING | MOVE_RUN_MODE, 0.0f);
        return true;
    }

    // The ONE stuck detector. An executor that committed to movement calls
    // this each tick; two consecutive ~1.2s windows without net displacement
    // means the path is not working — break the commitment and let the
    // arbiter pick something reachable (usually after blacklisting the dest).
    bool BotMind::MovingBlocked(uint32 now)
    {
        if (now - lastMoveCheck < 1200)
            return false;

        const float x = bot->GetPositionX(), y = bot->GetPositionY();
        const float dx = x - lastX, dy = y - lastY;
        const float moved = std::sqrt(dx * dx + dy * dy);
        lastX = x; lastY = y; lastMoveCheck = now;

        if (moved >= 3.0f)
        {
            stuckStrikes = 0;
            return false;
        }

        if (++stuckStrikes < 2)
            return false;

        stuckStrikes = 0;
        Log().stucks.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void BotMind::ResetStuck(uint32 now)
    {
        lastX = bot->GetPositionX();
        lastY = bot->GetPositionY();
        lastMoveCheck = now;
        stuckStrikes = 0;
    }
}
