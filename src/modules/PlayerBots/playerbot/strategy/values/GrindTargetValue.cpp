
#include "playerbot/playerbot.h"
#include "GrindTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "AttackersValue.h"
#include "PossibleAttackTargetsValue.h"
#include "playerbot/strategy/actions/ChooseTargetActions.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "Formulas.h"
#include <iomanip>

using namespace ai;

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = NULL;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(int assistCount)
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() || !master->GetPlayerbotAI()))
        master = nullptr;

    // "next grind target": chain-pull planning scan run MID-COMBAT for the mob AFTER this one.
    // It must not return current attackers (that's the fight we're already in).
    const bool planningNext = (getName() == "next grind target");

    std::list<ObjectGuid> attackers;
    if (!planningNext)
    {
        attackers = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
        for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); i++)
        {
            Unit* unit = ai->GetUnit(*i);
            if (!unit || !sServerFacade.IsAlive(unit))
                continue;

            if (!bot->InBattleGround() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit)))
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + "(hostile) ignored (out of free range).");
                continue;
            }

            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) +"(hostile) selected.");
       
            return unit;
        }
    }

    // PRE-SELECTED CHAIN TARGET: decided while the previous victim was dying. Re-validate NOW
    // (alive / not blacklisted / level / valid / untapped / free-move), consume once either way --
    // a stale plan falls through to the normal scan, never oscillates.
    if (!planningNext)
    {
        ObjectGuid preGuid = AI_VALUE(ObjectGuid, "pre-selected next target");
        if (preGuid)
        {
            SET_AI_VALUE(ObjectGuid, "pre-selected next target", ObjectGuid());
            Unit* pre = ai->GetUnit(preGuid);
            if (pre && sServerFacade.IsAlive(pre) &&
                !IsTargetGuidSkippedForBot(bot, preGuid) &&
                (int)pre->GetLevel() - (int)bot->GetLevel() <= (bot->GetGroup() ? 4 : 2) &&
                AttackersValue::IsValid(pre, bot, nullptr, false, false) &&
                PossibleAttackTargetsValue::IsPossibleTarget(pre, bot, sPlayerbotAIConfig.sightDistance, false) &&
                (bot->InBattleGround() || CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(pre))))
                return pre;
        }
    }

    std::list<ObjectGuid> targets = *context->GetValue<std::list<ObjectGuid> >("possible targets");
    const uint32 possibleTargetCount = targets.size();

    float distance = 0;
    Unit* result = NULL;
    ObjectGuid resultGuid;
    float fallbackDistance = 0;
    Unit* fallbackResult = NULL;
    ObjectGuid fallbackGuid;

    bool travelTargetWorking = AI_VALUE(bool, "travel target working");
    bool travelTargetTraveling = AI_VALUE(bool, "travel target traveling");
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    TravelDestination* travelDestination = travelTarget ? travelTarget->GetDestination() : nullptr;
    bool isGrindTravelDest = travelDestination && dynamic_cast<GrindTravelDestination*>(travelDestination);
    bool isQuestObjectiveTravelDest = travelDestination && dynamic_cast<QuestObjectiveTravelDestination*>(travelDestination);
    bool hasNullTravelDest = travelDestination && dynamic_cast<NullTravelDestination*>(travelDestination);
    const bool autonomousRandomBot = !ai->HasActivePlayerMaster() && !ai->HasRealPlayerMaster();
    TravelStatus travelStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
    const bool hasActiveNonGrindTravelTarget =
        travelTarget &&
        travelStatus != TravelStatus::TRAVEL_STATUS_NONE &&
        travelStatus != TravelStatus::TRAVEL_STATUS_COOLDOWN &&
        travelStatus != TravelStatus::TRAVEL_STATUS_EXPIRED &&
        !isGrindTravelDest &&
        !hasNullTravelDest;
    const bool hasBlockingNonObjectiveTravelTarget =
        hasActiveNonGrindTravelTarget && !isQuestObjectiveTravelDest;
    const bool hasActiveQuestObjectiveTravelTarget =
        hasActiveNonGrindTravelTarget && isQuestObjectiveTravelDest;
    bool hasQuestObjectivesPending = false;
    bool hasQuestTurnInsPending = false;
    bool hasUnrewardedQuestStatus = false;
    for (const auto& questEntry : bot->getQuestStatusMap())
    {
        uint32 questId = questEntry.first;
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        bool rewarded = bot->GetQuestRewardStatus(questId);
        if (!rewarded)
            hasUnrewardedQuestStatus = true;

        if (questStatus == QUEST_STATUS_INCOMPLETE)
            hasQuestObjectivesPending = true;
        else if (questStatus == QUEST_STATUS_COMPLETE && !rewarded)
            hasQuestTurnInsPending = true;

        if (hasQuestObjectivesPending && hasQuestTurnInsPending)
            break;
    }

    // Quest pickup routing is owned by the travel/quest actions. Do not scan
    // quest giver maps from grind target selection; this runs in the combat hot
    // path and can observe stale world-position data during bot churn. Still,
    // low-level autonomous bots with no live quest work should not settle into
    // starter-zone grinding; keep them committed to finding a quest giver.
    bool hasQuestPickupsPending =
        autonomousRandomBot &&
        bot->GetLevel() <= 12 &&
        !hasUnrewardedQuestStatus &&
        !hasQuestObjectivesPending &&
        !hasQuestTurnInsPending;

    const bool questPickupOnlyPending =
        hasQuestPickupsPending && !hasQuestObjectivesPending && !hasQuestTurnInsPending;
    const bool hasQueuedQuestWork = hasQuestObjectivesPending || hasQuestTurnInsPending;
    const bool mustCommitToQuestTravel =
        autonomousRandomBot &&
        (hasQuestPickupsPending || hasQuestTurnInsPending || hasActiveNonGrindTravelTarget);
    const bool skipGrindTargetScan =
        autonomousRandomBot &&
        mustCommitToQuestTravel &&
        (questPickupOnlyPending || (hasQuestTurnInsPending && !hasActiveQuestObjectiveTravelTarget) ||
            hasBlockingNonObjectiveTravelTarget);

    struct RejectStats
    {
        uint32 z = 0;
        uint32 free = 0;
        uint32 farMaster = 0;
        uint32 highLevel = 0;
        uint32 elite = 0;
        uint32 invalid = 0;
        uint32 impossible = 0;
        uint32 critter = 0;
        uint32 notQuest = 0;
        uint32 noXp = 0;
        uint32 crowded = 0;
    } rejectStats;

    struct MemberInfo {
        Player* player;
        float x, y;
    };
    std::vector<MemberInfo> groupMembers;
    if (group)
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        groupMembers.reserve(groupSlot.size());
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = sObjectMgr.GetPlayer(itr->guid);
            if (member && sServerFacade.IsAlive(member))
                groupMembers.push_back({ member, member->GetPositionX(), member->GetPositionY() });
        }
    }

    std::unordered_map<uint32, bool> needForQuestCache;

    // Threat-aware target selection (autonomous bots): precompute the positions of
    // all visible hostiles once, so for each candidate we can cheaply estimate how
    // "packed" it is and avoid beelining into the middle of a mob cluster — a top
    // cause of bot deaths (they pick a target deep in a pack, pull everything, die).
    std::vector<std::pair<float, float>> hostilePositions;
    const bool useThreatAwareSelection = autonomousRandomBot && possibleTargetCount > 0 && possibleTargetCount <= 40;
    if (useThreatAwareSelection)
    {
        hostilePositions.reserve(possibleTargetCount);
        for (const ObjectGuid& hg : targets)
            if (Unit* hu = ai->GetUnit(hg))
                hostilePositions.emplace_back(hu->GetPositionX(), hu->GetPositionY());
    }

    // GRIND-WHILE-WAITING setup: is any CLAIMABLE quest mob available right now? (the candidates
    // here are already claimable possible-targets, so a needForQuest one == claimable quest mob.)
    // If a bot is parked at its quest KILL camp (working a QuestObjective) with NONE claimable,
    // we let it grind a nearby xp mob instead of idling -- it gains xp and cycles shared spawn
    // points toward its target. A quest mob, the instant one becomes claimable, is in this same
    // list and outranks the grind pick (quest mobs are preferred), so quest kills keep priority.
    bool anyClaimableQuestMob = false;
    for (const ObjectGuid& qg : targets)
    {
        Unit* qu = ai->GetUnit(qg);
        if (!qu) continue;
        uint32 qe = qu->GetEntry();
        if (!qe) continue;
        auto qit = needForQuestCache.find(qe);
        bool qnfq;
        if (qit != needForQuestCache.end()) qnfq = qit->second;
        else { qnfq = AI_VALUE2(bool, "need for quest", std::to_string(qe)); needForQuestCache[qe] = qnfq; }
        if (qnfq) { anyClaimableQuestMob = true; break; }
    }
    // Fires for any autonomous bot that HAS kill objectives, is NOT mid-travel (so it's at/near
    // a camp, not en route to a quest), and has NO claimable quest mob right now. This catches the
    // big COOLDOWN-thrashing waiting population (not just the ~30 in WORK status). Still strictly
    // subordinate: the instant a quest mob is claimable, anyClaimableQuestMob flips true and the
    // quest mob (preferred in selection) wins; travelling bots keep travelling.
    const bool grindWhileWaiting =
        autonomousRandomBot && hasQuestObjectivesPending && !travelTargetTraveling && !anyClaimableQuestMob;

    for (std::list<ObjectGuid>::iterator tIter = skipGrindTargetScan ? targets.end() : targets.begin(); tIter != targets.end(); tIter++)
    {
        Unit* unit = ai->GetUnit(*tIter);
        if (!unit)
            continue;

        if (planningNext && (unit == bot->GetVictim() || unit->GetVictim() == bot))
            continue; // never "plan" the mob we're killing or an add already on us

        // Re-acquire blacklist: a target recently deferred by the stall/abandon breakers must not
        // be instantly re-picked (rewired 2026-07-03; the reader was lost in the git-reset).
        if (IsTargetGuidSkippedForBot(bot, *tIter))
            continue;

#ifdef MANGOSBOT_TWO 
        if (bot->GetMapId() == 609)
        {
            switch (unit->GetEntry())
            {
            case 28605: //Havenshire Stallion
            case 28606: //Havenshire Mare
            case 28607: //Havenshire Cotl
            case 28891: //Scarlet Miner
            case 28819: //Scarlet Miner
            case 28769: //Shadowy tormentor
            case 28768: //Dark Rider of Archeus
                continue;
            case 29080: //Scarlet Champion
            case 29029: //Scarlet Inquisitor
            case 29000: //Scarlet Commander Rodrick
            case 28940: //Scarlet Crusader
            case 28939: //Scarlet Preacher
            case 28936: //Scarlet Commander
            case 28898: //Scarlet Captain
            case 28896: //Scarlet Infantryman
            case 28895: //Scarlet Medic
            case 28892: //Scarlet Peasant
            case 28610: //Scarlet Marksman
                if (AI_VALUE2(bool, "need quest objective", "12680,0"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12701"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12727"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12754"))
                    continue;                
                break;
            case 29102: //HearthglenCrusader
            case 29103: //Tirisfal Crusader
            case 29104: //Scarlet Ballista
                if (AI_VALUE2(bool, "need quest objective", "12779"))
                {
                    if (AI_VALUE2(bool, "trigger active", "in vehicle") && bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance))
                        return unit;
                    continue;
                }
                break;
            case 28834: //Scarlet Fleet Defender
            case 28850: //Scarlet Land Canon
            case 28856: //Scarlet Fleet Guardian
                if (AI_VALUE2(bool, "need quest objective", "12701,0"))
                {
                    if (AI_VALUE2(bool, "trigger active", "in vehicle::Scarlet Cannon") && !urand(0, 5))
                        return unit;
                    continue;
                }
            }
        }
#endif

        const float zDistance = std::abs(bot->GetPositionZ() - unit->GetPositionZ());
        float maxZDistance = sPlayerbotAIConfig.spellDistance;

        // Starter-zone terrain regularly puts valid nearby mobs a few extra yards above/below
        // autonomous random bots. Let them tolerate a bit more vertical variance so they do not
        // deadlock on a single visible target.
        if (autonomousRandomBot)
        {
            maxZDistance = std::max(maxZDistance, 12.0f);

            const float planarDistance = sServerFacade.GetDistance2d(bot, unit);
            if (planarDistance <= std::max(12.0f, sPlayerbotAIConfig.grindDistance * 0.5f))
                maxZDistance = std::max(maxZDistance, 18.0f);
        }

        if (zDistance > maxZDistance)
        {
            ++rejectStats.z;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (to far above/below).");
            continue;
        }

        if (!bot->InBattleGround() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit))) //Do not grind mobs far away from master.
        {
            ++rejectStats.free;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (out of free range).");
            continue;
        }

        if (!bot->InBattleGround() && master &&
            (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
             ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
            sServerFacade.GetDistance2d(master, unit) > sPlayerbotAIConfig.proximityDistance)
        {
            ++rejectStats.farMaster;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        // +2 cap for SOLO autonomous grinding (was +4): the death log shows a fat band of deaths to
        // +3/+4 mobs (48 of 300) -- a solo bot picking oranges loses often. Real players solo-grind
        // green/yellow (-2..+2) and only take orange with a group; grouped/mastered bots keep +4.
        const int levelCapAbove = (autonomousRandomBot && !bot->GetGroup()) ? 2 : 4;
        if (!bot->InBattleGround() && (int)unit->GetLevel() - (int)bot->GetLevel() > levelCapAbove && !unit->GetObjectGuid().IsPlayer())
        {
            ++rejectStats.highLevel;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (" + std::to_string((int)unit->GetLevel() - (int)bot->GetLevel()) + " levels above bot).");
            continue;
        }

        Creature* creature = dynamic_cast<Creature*>(unit);
        if (creature && creature->GetCreatureInfo() && creature->GetCreatureInfo()->Rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite") &&
            !AI_VALUE2(bool, "trigger active", "in vehicle"))
        {
            ++rejectStats.elite;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (can not fight elites currently).");
            continue;
        }

        // Never grind a target the bot cannot plausibly KILL: TWoW's custom Training Dummies are
        // rank-0 LEVEL-1 creatures with 1.1M-110M HP, so they pass every level/elite filter -- and
        // nearest-first selection then locks bots onto them FOREVER (watched Rogerolan melee an
        // Apprentice Training Dummy for 20+ minutes, zero XP). Grinding exists to gain XP; a mob
        // with >40x the bot's max health is not a grind target, it's a time sink.
        if (creature && bot->GetMaxHealth() > 0 && creature->GetMaxHealth() > 40u * bot->GetMaxHealth())
        {
            ++rejectStats.impossible;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (unkillable hp pool).");
            continue;
        }

        if (!AttackersValue::IsValid(unit, bot, nullptr, false, false))
        {
            ++rejectStats.invalid;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (is pet or evading/unkillable).");
            continue;
        }

        if (!PossibleAttackTargetsValue::IsPossibleTarget(unit, bot, sPlayerbotAIConfig.sightDistance, false))
        {
            ++rejectStats.impossible;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (tapped, cced or out of range).");
            continue;
        }

        // Never autonomously grind critters. They give no xp, are passive, and bots
        // were getting stuck perpetually casting at rabbits/hares that never die. The
        // old probabilistic gate (urand(0,10)) let ~9% through, which was enough to
        // trap a bot for minutes. NOTE: only catches creature-type CRITTER; passive
        // type-0 wildlife (Toad/Squirrel/Sheep/Rat) is handled by the no-xp gate below.
        if (creature && creature->IsCritter())
        {
            ++rejectStats.critter;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (ignore critters).");
            continue;
        }

        float newdistance = sServerFacade.GetDistance2d(bot, unit);

        uint32 entry = unit->GetEntry();
        bool needForQuest = false;
        if (entry)
        {
            auto cacheIt = needForQuestCache.find(entry);
            if (cacheIt != needForQuestCache.end())
            {
                needForQuest = cacheIt->second;
            }
            else
            {
                needForQuest = AI_VALUE2(bool, "need for quest", std::to_string(entry));
                needForQuestCache[entry] = needForQuest;
            }
        }

        if (entry && !needForQuest)
        {
            // OPPORTUNISTIC PATH KILL (restored — the git-reset recovery silently dropped this
            // mechanism, so travelers rejected every non-quest mob again): an XP-giving mob within
            // 65y is a sanctioned kill even while committed to travel. Camp-mode v0.5 width.
            // green-or-better only (mob within 5 levels): XP>0 alone let a L13 chain L7-8 trash
            // for trickle XP (user: 'why is Arad 13 fighting level 5s') — worthless fights.
            const bool opportunisticPathKill =
                autonomousRandomBot && creature &&
                MaNGOS::XP::Gain(bot, creature) > 0 &&
                (int32)creature->GetLevel() + 5 >= (int32)bot->GetLevel() &&
                sServerFacade.GetDistance2d(bot, unit) <= 65.0f;

            if (opportunisticPathKill)
            {
                // fall through to the remaining validity checks below (no-xp/critter etc. can't
                // trigger: XP gain already verified)
            }
            else if (autonomousRandomBot && mustCommitToQuestTravel)
            {
                ++rejectStats.notQuest;
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (must commit to quest travel before grind).");

                continue;
            }
            else if (autonomousRandomBot && hasQueuedQuestWork)
            {
                ++rejectStats.notQuest;
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (queued quest work takes priority).");

                continue;
            }
            else if (hasBlockingNonObjectiveTravelTarget)
            {
                ++rejectStats.notQuest;
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (not needed for active quest).");

                continue;
            }
            else if (urand(0, 100) < 99 && travelTargetWorking && !isGrindTravelDest)
            {
                ++rejectStats.notQuest;
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (not needed for active quest).");

                continue;
            }
            else if (creature && !MaNGOS::XP::Gain(bot, creature))
            {
                // No-xp, non-quest creature: either a passive critter (Toad/Squirrel/
                // Sheep/Rat/Rabbit) or a gray (too-low-level) mob. Grinding these is
                // pointless and bots were getting stuck PERPETUALLY casting at passive
                // wildlife that never dies (measured: ~10k stuck casts/run, bots rooted
                // for minutes nuking a single toad). A real player never farms rabbits;
                // reject so the bot moves on / travels to level-appropriate mobs.
                //
                // Mostly-reject rather than always: a bot genuinely boxed in by only
                // gray mobs keeps a heavily-penalized last resort instead of hard-idling.
                // Previously autonomous bots KEPT these as cheap fallback (the bug).
                const bool rejectNoXp = autonomousRandomBot ? (urand(0, 6) != 0) : (urand(0, 50) != 0);
                if (rejectNoXp)
                {
                    ++rejectStats.noXp;
                    if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                        if (travelTargetTraveling && !isGrindTravelDest)
                            ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (no xp / critter).");

                    continue;
                }

                // Heavy distance penalty so any real mob is always preferred.
                ++rejectStats.noXp;
                newdistance += autonomousRandomBot ? 30.0f : 12.0f;
            }
            else if (urand(0, 100) < 75)
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    if (travelTargetTraveling && !isGrindTravelDest)
                        ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " increased distance (not needed for quest).");

                newdistance += 20;
            }

            // Starter areas can produce a stable but unhealthy local-farm loop where autonomous
            // bots keep choosing the closest non-quest mob and never commit to their queued
            // travel target. Favor spreading out by penalizing short-hop fallback grinding.
            if (autonomousRandomBot && !travelTargetTraveling && newdistance < 12.0f)
                newdistance += (12.0f - newdistance) * 2.5f;
        }

        // Pack/adds penalty: prefer isolated mobs over ones surrounded by other
        // hostiles, so the bot peels mobs off the edge of a cluster one at a time
        // instead of pulling the whole pack and dying. Soft (distance) penalty, and
        // scaled down when the bot is hurt (be more cautious at low HP). A genuinely
        // needed quest mob in a pack is still chosen if nothing safer exists, since
        // fallbackResult guarantees the bot never idles for lack of a target.
        if (useThreatAwareSelection)
        {
            const float packRadius = 10.0f;
            const float ux = unit->GetPositionX(), uy = unit->GetPositionY();
            int nearbyHostiles = -1; // discount the candidate itself (it is in the list)
            for (const auto& hp : hostilePositions)
            {
                const float dx = hp.first - ux, dy = hp.second - uy;
                if (dx * dx + dy * dy <= packRadius * packRadius && ++nearbyHostiles >= 6)
                    break;
            }
            const uint32 healthPct = AI_VALUE2(uint8, "health", "self target");
            const int safeAdds = healthPct >= 80 ? 2 : (healthPct >= 50 ? 1 : 0);
            if (nearbyHostiles > safeAdds)
                newdistance += (nearbyHostiles - safeAdds) * 18.0f;
        }

        int targetingPlayerCount = GetTargetingPlayerCount(unit);
        if (!bot->InBattleGround() && targetingPlayerCount > assistCount)
        {
            ++rejectStats.crowded;
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " increased distance (" + std::to_string(targetingPlayerCount) + " bots already targeting).");

            newdistance += targetingPlayerCount * (autonomousRandomBot ? 12.0f : 5.0f);
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = sObjectMgr.GetPlayer(itr->guid);
                if (!member || !sServerFacade.IsAlive(member))
                    continue;

                newdistance = sServerFacade.GetDistance2d(member, unit);
                if (!result || newdistance < distance)
                {
                    distance = newdistance;
                    result = unit;
                    resultGuid = unit->GetObjectGuid();
                }
            }
        }
        else
        {
            if (!result || (newdistance < distance && urand(0, abs(distance - newdistance)) > sPlayerbotAIConfig.sightDistance * 0.1))
            {
                distance = newdistance;
                result = unit;
                resultGuid = unit->GetObjectGuid();
            }
        }

        if (!fallbackResult || newdistance < fallbackDistance)
        {
            fallbackDistance = newdistance;
            fallbackResult = unit;
            fallbackGuid = unit->GetObjectGuid();
        }
    }

    if (!result && fallbackResult && autonomousRandomBot)
    {
        result = ai->GetUnit(fallbackGuid);
        resultGuid = fallbackGuid;
        distance = fallbackDistance;
    }

    if (resultGuid)
        result = ai->GetUnit(resultGuid);

    if (result && (!result->IsInWorld() || result->GetMapId() != bot->GetMapId() || !sServerFacade.IsAlive(result)))
        result = nullptr;

    if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
    {
        if(result)
        {
            ai->TellPlayer(GetMaster(), chat->formatWorldobject(result) + " selected.");
        }
        else
        {
            ai->TellPlayer(GetMaster(), "No grind target found.");
        }
    }

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "attackers=" << attackers.size()
            << " possible=" << possibleTargetCount
            << " assistCount=" << assistCount;

        if (result)
        {
            out << " target=" << result->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, result)
                << " targetedBy=" << GetTargetingPlayerCount(result)
                << " questPickups=" << (hasQuestPickupsPending ? 1 : 0)
                << " questTurnIns=" << (hasQuestTurnInsPending ? 1 : 0)
                << " questObjectives=" << (hasQuestObjectivesPending ? 1 : 0)
                << " questPickupOnly=" << (questPickupOnlyPending ? 1 : 0)
                << " mustCommitQuestTravel=" << (mustCommitToQuestTravel ? 1 : 0)
                << " queuedQuestWork=" << (hasQueuedQuestWork ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "GrindTargetSelect", std::to_string(result->GetGUIDLow()), out.str());
        }
        else
        {
            out << " rejectZ=" << rejectStats.z
                << " rejectFree=" << rejectStats.free
                << " rejectFarMaster=" << rejectStats.farMaster
                << " rejectHighLevel=" << rejectStats.highLevel
                << " rejectElite=" << rejectStats.elite
                << " rejectInvalid=" << rejectStats.invalid
                << " rejectImpossible=" << rejectStats.impossible
                << " rejectCritter=" << rejectStats.critter
                << " rejectNotQuest=" << rejectStats.notQuest
                << " rejectNoXp=" << rejectStats.noXp
                << " penalizedCrowded=" << rejectStats.crowded
                << " travelStatus=" << static_cast<uint32>(travelStatus)
                << " travelWorking=" << (travelTargetWorking ? 1 : 0)
                << " travelTraveling=" << (travelTargetTraveling ? 1 : 0)
                << " activeNonGrindTravel=" << (hasActiveNonGrindTravelTarget ? 1 : 0)
                << " activeObjectiveTravel=" << (hasActiveQuestObjectiveTravelTarget ? 1 : 0)
                << " objectiveTravel=" << (isQuestObjectiveTravelDest ? 1 : 0)
                << " skippedScan=" << (skipGrindTargetScan ? 1 : 0)
                << " questPickups=" << (hasQuestPickupsPending ? 1 : 0)
                << " questTurnIns=" << (hasQuestTurnInsPending ? 1 : 0)
                << " questObjectives=" << (hasQuestObjectivesPending ? 1 : 0)
                << " questPickupOnly=" << (questPickupOnlyPending ? 1 : 0)
                << " mustCommitQuestTravel=" << (mustCommitToQuestTravel ? 1 : 0)
                << " queuedQuestWork=" << (hasQueuedQuestWork ? 1 : 0)
                << " nullTravel=" << (hasNullTravelDest ? 1 : 0)
                << " grindDest=" << (isGrindTravelDest ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "GrindTargetSelectFailed", "none", out.str());
        }
    }

    return result;
}

int GrindTargetValue::GetTargetingPlayerCount( Unit* unit )
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    int count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player *member = sObjectMgr.GetPlayer(itr->guid);
        if( !member || !sServerFacade.IsAlive(member) || member == bot)
            continue;

        PlayerbotAI* ai = member->GetPlayerbotAI();
        Unit* currentTarget = nullptr;
        if (ai && ai->GetAiObjectContext())
        {
            if (Value<Unit*>* value = dynamic_cast<Value<Unit*>*>(ai->GetAiObjectContext()->GetUntypedValue("current target")))
                currentTarget = value->Get();
        }

        if ((ai && currentTarget == unit) ||
            (!ai && member->GetSelectionGuid() == unit->GetObjectGuid()))
            count++;
    }

    return count;
}
