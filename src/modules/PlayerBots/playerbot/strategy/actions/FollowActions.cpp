
#include "playerbot/playerbot.h"
#include "FollowActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/Formations.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/LootObjectStack.h"

using namespace ai;

bool FollowAction::Execute(Event& event)
{
    bool moved = false;
    Unit* followTarget = AI_VALUE(Unit*, "follow target");
    Formation* formation = AI_VALUE(Formation*, "formation");

    if (ai->IsSafe(followTarget))
    {
        // LOOSE FOLLOW (humanization): bots following a REAL player must not use the core
        // MoveFollow generator -- it re-anchors every bot to the master's FACING every tick,
        // so the whole raid snap-rotates around the player like a bodyguard detail. Loose
        // mode gives each bot a world-space slot, reaction latency and stragglers.
        if (sPlayerbotAIConfig.looseFollow && followTarget->IsPlayer()
            && !((Player*)followTarget)->GetPlayerbotAI() && ai->HasRealPlayerMaster())
            return LooseFollow(followTarget);

        if (formation)
        {
            WorldLocation loc = formation->GetLocation();
            if (!Formation::IsNullLocation(loc) && loc.mapid != -1)
            {
                moved = Follow(followTarget, formation->GetOffset(), formation->GetAngle());
            }
        }
    }
    else
        moved = Follow(followTarget, 0, 0);

    return moved;
}

bool FollowAction::LooseFollow(Unit* master)
{
    struct LooseState
    {
        float slotAngle = 0, slotDist = 0;   // personal WORLD-space slot around the master
        uint32 rollAtMs = 0;                 // when to re-roll the slot (drifting crowd)
        uint32 reactDelayMs = 0;             // personal reaction latency to master movement
        uint32 notedMoveMs = 0;              // when we noticed the master strayed
        float anchorX = 0, anchorY = 0;      // master position we last pathed against
        bool anchored = false;
    };
    static std::mutex s_mx;
    static std::unordered_map<uint32, LooseState> s_states;

    const uint32 now = WorldTimer::getMSTime();
    const float mx = master->GetPositionX(), my = master->GetPositionY();

    bool moveNow = false;
    float slotAngle, slotDist, ax, ay;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        LooseState& st = s_states[bot->GetGUIDLow()];
        if (!st.rollAtMs || now >= st.rollAtMs)
        {
            st.rollAtMs = now + urand(45000, 90000);
            st.slotAngle = frand(0, 2 * M_PI_F);
            st.slotDist = frand(3.0f, 14.0f);
            st.reactDelayMs = urand(300, 1800);
            if (!urand(0, 4))
                st.reactDelayMs += urand(1000, 2500);   // straggler personality
        }

        const float ddx = mx - st.anchorX, ddy = my - st.anchorY;
        const float strayed2 = ddx * ddx + ddy * ddy;
        if (!st.anchored)
        {
            st.anchored = true; st.anchorX = mx; st.anchorY = my;
            moveNow = true;
        }
        else if (strayed2 > 60.0f * 60.0f)
        {
            // master is sprinting/mounted away -- no lazy reaction, keep up NOW
            st.anchorX = mx; st.anchorY = my; st.notedMoveMs = 0;
            moveNow = true;
        }
        else if (strayed2 > 10.0f * 10.0f)
        {
            // master strayed from our anchor: react only after the personal latency
            if (!st.notedMoveMs)
                st.notedMoveMs = now;
            else if (now - st.notedMoveMs >= st.reactDelayMs)
            {
                st.anchorX = mx; st.anchorY = my; st.notedMoveMs = 0;
                moveNow = true;
            }
        }
        else
            st.notedMoveMs = 0;   // master is loitering near our anchor -- hold position

        slotAngle = st.slotAngle; slotDist = st.slotDist;
        ax = st.anchorX; ay = st.anchorY;
    }

    const float tx = ax + cos(slotAngle) * slotDist;
    const float ty = ay + sin(slotAngle) * slotDist;

    // catch-up guard: deliberately holding is fine, being LEFT BEHIND is not (rez, loot detour)
    if (!moveNow)
    {
        const float bdx = tx - bot->GetPositionX(), bdy = ty - bot->GetPositionY();
        if (bdx * bdx + bdy * bdy > 30.0f * 30.0f)
            moveNow = true;
    }

    if (!moveNow)
        return true;   // deliberate hold = success (don't let the engine fidget)

    float tz = master->GetPositionZ();
    bot->UpdateAllowedPositionZ(tx, ty, tz);
    return MoveTo(master->GetMapId(), tx, ty, tz, false, false);
}

bool FollowAction::isUseful()
{
    if (!ai->CanMove())
        return false;

    float distance = 0;
    Unit* followTarget = AI_VALUE(Unit*, "follow target");
    Formation* formation = AI_VALUE(Formation*, "formation");

    if (followTarget && followTarget->IsPlayer())
    {
        if (AI_VALUE(GuidPosition, "rpg target") && CanFreeMoveValue::CanFreeMoveTo(ai, AI_VALUE(GuidPosition, "rpg target")))
        {
            return false;
        }
    }

    if (followTarget)
    {
        if (followTarget->IsTaxiFlying() || !CanDeadFollow(followTarget) || followTarget->GetGUIDLow() == bot->GetGUIDLow())
        {
            return false;
        }
    }

    if (followTarget)
    {
        distance = sServerFacade.GetDistance2d(bot, followTarget);
    }
    else
    {
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || bot->GetMapId() != loc.mapid)
        {
            return false;
        }

        distance = sServerFacade.GetDistance2d(bot, loc.coord_x, loc.coord_y);
    }

    if (sServerFacade.IsDistanceGreaterThan(distance, sPlayerbotAIConfig.sightDistance))
    {
        return true;
    }

    // maybe jump is faster?
    if (ai->HasStrategy("follow jump", BotState::BOT_STATE_NON_COMBAT) && ai->AllowActivity())
    {
        return true;
    }

    if (followTarget && sServerFacade.GetChaseTarget(bot) && sServerFacade.GetChaseTarget(bot)->GetObjectGuid() == followTarget->GetObjectGuid() && formation->GetAngle() == sServerFacade.GetChaseAngle(bot) && formation->GetOffset() == sServerFacade.GetChaseOffset(bot))
    {
        return false;
    }

    return true;
}

bool FollowAction::CanDeadFollow(Unit* target)
{
    //Move to corpse when dead and player is alive or not a ghost.
    if (!sServerFacade.IsAlive(bot) && (sServerFacade.IsAlive(target) || !target->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)))
    {
        return false;
    }
    
    return true;
}

bool StopFollowAction::isUseful()
{
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        return false;

    if (sServerFacade.GetChaseTarget(bot) && !sServerFacade.GetChaseTarget(bot)->IsPlayer() && sServerFacade.GetChaseTarget(bot)->IsMoving())
        return false;

    return true;
}

bool FleeToMasterAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* fTarget = AI_VALUE(Unit*, "master target");
    bool canFollow = Follow(fTarget);
    if (!canFollow)
    {
        //SetDuration(5000);
        return false;
    }

    WorldPosition targetPos(fTarget);
    WorldPosition bosPos(bot);
    float distance = bosPos.fDist(targetPos);


    uint32 scale = 5;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 5)
        scale = bot->GetGroup()->GetMembersCount();
        

    if (distance > sPlayerbotAIConfig.reactDistance && bot->IsInCombat())
    {
        if (!urand(0, scale))
            ai->TellPlayerNoFacing(requester, "I'm heading to your location but I'm in combat", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            //ai->TellPlayer(BOT_TEXT("wait_travel_combat"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
    else if (distance < sPlayerbotAIConfig.reactDistance * 3)
    {
        if (!urand(0, scale))
            ai->TellPlayerNoFacing(requester, BOT_TEXT("wait_travel_close"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
    else if (distance < 1000)
    {
        if (!urand(0, scale*4))
            ai->TellPlayerNoFacing(requester, BOT_TEXT("wait_travel_medium"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
    else
        if (!urand(0, scale*6))
            ai->TellPlayerNoFacing(requester, BOT_TEXT("wait_travel_medium"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
           
    SetDuration(3000U);
    return true;
}

bool FleeToMasterAction::isUseful()
{
    if (!ai->CanMove())
        return false;

    if (!ai->GetGroupMaster())
        return false;

    if (ai->GetGroupMaster() == bot)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");

    if (target && ai->GetGroupMaster()->HasTarget(target->GetObjectGuid()))
        return false;

    if (!(ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
      ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        return false;

    Unit* fTarget = AI_VALUE(Unit*, "master target");
    
    if (!CanDeadFollow(fTarget))
        return false;

    if (fTarget && fTarget->IsPlayer())
    {
        if (AI_VALUE(GuidPosition, "rpg target") && CanFreeMoveValue::CanFreeMoveTo(ai, AI_VALUE(GuidPosition, "rpg target")))
            return false;
    }

    LootObject loot = AI_VALUE(LootObject, "loot target");
    if (loot.IsLootPossible(bot))
        return false;

    return true;
}

