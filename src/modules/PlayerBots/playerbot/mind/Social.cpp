#include "playerbot/playerbot.h"
#include "Mind.h"
#include "MindLog.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/AiObjectContext.h"

namespace mind
{
    // SOCIAL: a grouped NON-leader sticks to its leader and focus-fires the
    // leader's target — real party behavior ("don't fall behind, be active +
    // helping"). The leader is a normal autonomous bot; followers converge
    // and assist. Group formation itself stays in RandomPlayerbotMgr (world
    // thread); invite acceptance stays on the packet-reactive engine pass.
    Verdict BotMind::StepSocial(uint32 now)
    {
        if (!bot->GetGroup() || ai->IsGroupLeader())
            return { false, false, 0 };

        Player* leader = ai->GetGroupMaster();
        if (!leader || leader == bot || !leader->IsInWorld() || !leader->IsAlive())
            return { false, false, 0 };   // leader gone -> solo behavior; group disbands naturally

        SetGoal(Goal::Social, now);
        Log().social.fetch_add(1, std::memory_order_relaxed);

        const bool sameMap = leader->GetMapId() == bot->GetMapId();
        const float ld = sameMap ? bot->GetDistance(leader) : 99999.f;

        // ASSIST: leader is fighting -> engage its target (focus fire).
        Unit* lv = leader->GetVictim();
        if (lv && sServerFacade.IsAlive(lv) && !bot->IsInCombat() && sameMap && ld < 60.0f)
        {
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(lv);
            targetGuid = lv->GetObjectGuid();
            const bool acted = ai->DoSpecificAction("attack", Event(), true);
            return { true, acted, 300 };
        }

        // CATCH-UP: far behind / other map, out of combat, unseen -> blink to
        // the leader (the "never fall behind in dungeons" guard).
        if ((!sameMap || ld > 80.0f) && !bot->IsInCombat() && !SeenByPlayer(120.0f))
        {
            sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), leader->GetMapId(),
                leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), false);
            Log().teleports.fetch_add(1, std::memory_order_relaxed);
            return { true, true, 1500 };
        }

        // Leader is doing something (moving/fighting) -> normal group flow.
        if (lv || sServerFacade.isMoving(leader))
            socialHoldSince = 0;

        // FOLLOW: run to the leader when spread out; hold when close.
        if (sameMap && ld > 12.0f && !bot->IsInCombat())
        {
            const bool acted = ai->DoSpecificAction("follow", Event(), true);
            return { true, acted, 400 };
        }

        if (!bot->IsInCombat())
        {
            // Close & leader idle. Waiting is human for a while — 26 bots
            // statued on one graveyard coordinate is not (observed live).
            // After 45s of dead leader, LIVE A LITTLE: fall through to local
            // activity (fight/loot/errand nearby) but stay leashed — the
            // arbiter won't journey a leashed bot away from its group.
            if (!socialHoldSince)
                socialHoldSince = now;
            if (now - socialHoldSince < 45000)
                return { true, false, 800 };

            socialLeashed = true;
            return { false, false, 0 };
        }

        return { false, false, 0 };        // in combat -> CombatStep/engine own the tick
    }
}
