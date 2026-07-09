
#include "playerbot/playerbot.h"
#include "MoveToTravelTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "Maps/MapManager.h"
#include "Maps/PathFinder.h"
#include "Movement/spline/MoveSpline.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/strategy/actions/UseItemAction.h"
#include <cmath>
#include <iomanip>
#include <limits>
#include <mutex>
#include <unordered_map>

using namespace ai;

namespace
{
    static constexpr float PLAYERBOT_QUEST_GO_WORK_RANGE = 2.25f;

    struct HumanLikeTravelState
    {
        time_t nextPause = 0;
        time_t nextJump = 0;
        time_t nextTrace = 0;
    };

    std::mutex s_humanLikeTravelMutex;
    std::unordered_map<uint32, HumanLikeTravelState> s_humanLikeTravelStates;

    class QuestObjectiveItemUseAction : public UseAction
    {
    public:
        QuestObjectiveItemUseAction(PlayerbotAI* ai) : UseAction(ai, "quest objective item", 0) {}

        bool UseQuestObjectiveItem(Player* requester, uint32 itemId, Unit* target)
        {
            return UseItem(requester, itemId, target);
        }

        bool UseQuestObjectiveItem(Player* requester, uint32 itemId, GameObject* target)
        {
            return UseItem(requester, itemId, target);
        }
    };

    std::string QuestObjectiveGuidSkipQualifier(ObjectGuid guid)
    {
        return "quest objective guid skip::" + std::to_string(guid.GetRawValue());
    }

    std::string QuestObjectiveGoOpenPendingQualifier(ObjectGuid guid)
    {
        return "quest objective go open pending::" + std::to_string(guid.GetRawValue());
    }

    void SkipQuestObjectiveGuid(PlayerbotAI* ai, ObjectGuid guid, uint32 seconds)
    {
        if (!ai || !guid)
            return;

        ai->GetAiObjectContext()->GetValue<time_t>("manual time", QuestObjectiveGuidSkipQualifier(guid))->Set(time(0) + seconds);
    }

    void MarkQuestObjectiveGoOpenPending(PlayerbotAI* ai, ObjectGuid guid, uint32 seconds)
    {
        if (!ai || !guid)
            return;

        ai->GetAiObjectContext()->GetValue<time_t>("manual time", QuestObjectiveGoOpenPendingQualifier(guid))->Set(time(0) + seconds);
    }

    bool IsQuestObjectiveGoOpenPending(PlayerbotAI* ai, ObjectGuid guid)
    {
        return ai && guid && ai->GetAiObjectContext()->GetValue<time_t>("manual time", QuestObjectiveGoOpenPendingQualifier(guid))->Get() > time(0);
    }

    bool HasQuestGoLootWindow(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid)
            return false;

        ObjectGuid lootGuid = bot->GetLootGuid();
        return lootGuid == guid || (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING) && (!lootGuid || lootGuid == guid));
    }

    bool TryUseQuestItemOn(PlayerbotAI* ai, Player* bot, WorldObject* target, char const* source)
    {
        if (!ai || !bot || !target)
            return false;

        Unit* unitTarget = target->ToUnit();
        GameObject* gameObjectTarget = dynamic_cast<GameObject*>(target);
        if (!unitTarget && !gameObjectTarget)
            return false;

        std::list<Item*> questItems = ai->GetAiObjectContext()->GetValue<std::list<Item*>>("inventory items", "quest")->Get();
        if (questItems.empty())
            return false;

        QuestObjectiveItemUseAction useAction(ai);
        for (Item* item : questItems)
        {
            if (!item || !item->GetProto())
                continue;

            std::string qualifier = Qualified::MultiQualify(
                { std::to_string(item->GetProto()->ItemId), GuidPosition(target).to_string(), "force" }, ",");
            if (!ai->GetAiObjectContext()->GetValue<bool>("can use item on", qualifier)->Get())
                continue;

            ai->StopMoving();
            bot->SetSelectionGuid(target->GetObjectGuid());
            bool used = unitTarget ?
                useAction.UseQuestObjectiveItem(bot, item->GetEntry(), unitTarget) :
                useAction.UseQuestObjectiveItem(bot, item->GetEntry(), gameObjectTarget);

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "source=" << (source ? source : "unknown")
                    << " item=" << item->GetEntry()
                    << " target=" << target->GetName()
                    << " guid=" << target->GetObjectGuid().GetRawValue()
                    << " used=" << (used ? 1 : 0)
                    << " dist=" << std::fixed << std::setprecision(2) << bot->GetDistance(target);
                sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveItemUse", target->GetName(), out.str());
            }

            if (used)
                return true;
        }

        return false;
    }

    bool ShouldUseQuestNpcStandPoint(TravelTarget* target, WorldPosition const& location, Player* bot)
    {
        if (!target || !target->GetDestination() || !target->GetPosition() || !bot)
            return false;

        QuestRelationTravelDestination* questDest = dynamic_cast<QuestRelationTravelDestination*>(target->GetDestination());
        if (!questDest || questDest->GetEntry() <= 0)
            return false;

        if (location.getMapId() != bot->GetMapId())
            return false;

        // Once the bot is already close enough to interact, let CheckStatus switch
        // the travel target into WORK instead of introducing more movement.
        return !target->GetDestination()->IsIn(WorldPosition(bot));
    }

    WorldPosition GetQuestNpcStandPoint(TravelTarget* target, WorldPosition const& npcLocation, Player* bot)
    {
        uint64 seed = bot->GetGUIDLow();
        if (target->GetDestination())
            seed ^= (uint64(uint32(target->GetDestination()->GetEntry())) << 21);
        if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
            seed ^= guidPos->GetRawValue();

        seed ^= seed >> 33;
        seed *= 0xff51afd7ed558ccdULL;
        seed ^= seed >> 33;

        const float baseAngle = static_cast<float>(seed % 6283) / 1000.0f;
        const float radius = 3.0f + static_cast<float>((seed >> 12) % 140) / 100.0f;

        for (uint32 attempt = 0; attempt < 8; ++attempt)
        {
            const float angleOffset = (attempt % 2 ? -1.0f : 1.0f) *
                static_cast<float>((attempt + 1) / 2) * (M_PI_F / 4.0f);
            const float angle = baseAngle + angleOffset;
            WorldPosition standPoint(
                npcLocation.getMapId(),
                npcLocation.getX() + std::cos(angle) * radius,
                npcLocation.getY() + std::sin(angle) * radius,
                npcLocation.getZ(),
                npcLocation.getAngleTo(WorldPosition(bot)));

            if (!standPoint.getTerrain())
                continue;

            const float groundZ = standPoint.getHeight(false);
            if (std::fabs(groundZ - npcLocation.getZ()) > 6.0f)
                continue;

            standPoint.setZ(groundZ);
            return standPoint;
        }

        return npcLocation;
    }

    WorldPosition GetQuestObjectiveCampPoint(QuestObjectiveTravelDestination* objectiveDest, TravelTarget* target, WorldPosition const& spawnLocation, Player* bot, uint32 waitSeconds)
    {
        uint64 seed = bot->GetGUIDLow();
        seed ^= (uint64(uint32(objectiveDest->GetQuestId())) << 32);
        seed ^= (uint64(uint32(objectiveDest->GetEntry())) << 8);
        seed ^= uint64(uint8(objectiveDest->GetObjective()));

        seed ^= seed >> 33;
        seed *= 0xff51afd7ed558ccdULL;
        seed ^= seed >> 33;

        float radius = 6.0f;
        if (waitSeconds >= 90)
            radius = 2.2f;
        else if (waitSeconds >= 45)
            radius = 3.2f;
        else if (waitSeconds >= 20)
            radius = 4.5f;

        const float angle = static_cast<float>(seed % 6283) / 1000.0f;

        for (uint32 attempt = 0; attempt < 8; ++attempt)
        {
            const float attemptRadius = std::max<float>(1.6f, radius - 0.35f * attempt);
            const float angleOffset = (attempt % 2 ? -1.0f : 1.0f) *
                static_cast<float>((attempt + 1) / 2) * (M_PI_F / 5.0f);
            const float campAngle = angle + angleOffset;

            WorldPosition campPoint(
                spawnLocation.getMapId(),
                spawnLocation.getX() + std::cos(campAngle) * attemptRadius,
                spawnLocation.getY() + std::sin(campAngle) * attemptRadius,
                spawnLocation.getZ(),
                spawnLocation.getAngleTo(WorldPosition(bot)));

            if (!campPoint.getTerrain())
                continue;

            const float groundZ = campPoint.getHeight(false);
            if (std::fabs(groundZ - spawnLocation.getZ()) > 4.0f)
                continue;

            campPoint.setZ(groundZ);
            return campPoint;
        }

        return spawnLocation;
    }

    bool IsSameQuestGameObjectLoot(TravelTarget* travelTarget, LootObject& lootObject, Player* bot)
    {
        if (!travelTarget || lootObject.IsEmpty() || !bot)
            return false;

        QuestObjectiveTravelDestination* objectiveDest =
            dynamic_cast<QuestObjectiveTravelDestination*>(travelTarget->GetDestination());
        if (!objectiveDest || objectiveDest->GetEntry() >= 0)
            return false;

        GuidPosition* guidPos = dynamic_cast<GuidPosition*>(travelTarget->GetPosition());
        if (!guidPos || guidPos->GetRawValue() != lootObject.guid.GetRawValue())
            return false;

        WorldObject* lootWo = lootObject.GetWorldObject(bot);
        if (!lootWo || !lootWo->IsInWorld() || lootWo->GetMapId() != bot->GetMapId())
            return false;

        return dynamic_cast<GameObject*>(lootWo) != nullptr;
    }

    bool ClearStaleTravelAttackers(PlayerbotAI* ai, AiObjectContext* context, Player* bot, char const* source)
    {
        if (!ai || !context || !bot)
            return false;

        if (!context->GetValue<bool>("has attackers")->Get())
            return false;

        if (sServerFacade.IsInCombat(bot) || bot->GetVictim() || context->GetValue<Unit*>("dps target")->Get())
            return false;

        std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers", 1)->Get();
        for (ObjectGuid const& guid : attackers)
        {
            Unit* attacker = ai->GetUnit(guid);
            if (!attacker || !attacker->IsInWorld() || attacker->GetMapId() != bot->GetMapId() ||
                sServerFacade.UnitIsDead(attacker) || sServerFacade.IsFriendlyTo(bot, attacker))
                continue;

            if (attacker->GetVictim() == bot || attacker->GetGuidValue(UNIT_FIELD_TARGET) == bot->GetObjectGuid())
                return false;
        }

        context->GetValue<std::list<ObjectGuid>>("attackers", 1)->Reset();
        context->GetValue<std::list<ObjectGuid>>("attackers")->Reset();
        context->GetValue<bool>("has attackers")->Reset();

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "source=" << (source ? source : "travel")
                << " attackers=" << attackers.size()
                << " combat=0 victim=0 dpsTarget=0";
            sPlayerbotAIConfig.logEvent(ai, "TravelStaleAttackersCleared", "move to travel target", out.str());
        }

        return true;
    }

    void ClearBrokenTravelTarget(PlayerbotAI* ai, AiObjectContext* context, TravelTarget* travelTarget, char const* source)
    {
        if (travelTarget)
        {
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            travelTarget->SetForced(false);
        }

        if (context)
        {
            context->GetValue<bool>("travel target active")->Reset();
            context->GetValue<GuidPosition>("rpg target")->Reset();
            context->GetValue<ObjectGuid>("attack target")->Reset();
            context->ClearValues("no active travel destinations");
        }

        if (ai && sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "source=" << (source ? source : "travel")
                << " targetPtr=" << (travelTarget ? 1 : 0)
                << " destinationPtr=" << (travelTarget && travelTarget->GetDestination() ? 1 : 0)
                << " positionPtr=" << (travelTarget && travelTarget->GetPosition() ? 1 : 0)
                << " status=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0);
            sPlayerbotAIConfig.logEvent(ai, "BrokenTravelTargetCleared", "move to travel target", out.str());
        }
    }

    // TRAVEL FREEZE BREAKER. "move to travel target" can "succeed" (a MovePoint IS issued) toward a
    // destination the bot cannot actually path to (degenerate / off-mesh path) -> MoveTo returns true,
    // WaitForReach parks the bot, and it stands "moving" forever. Measured: bots frozen 35-69s at a
    // fixed spot while the travel router THRASHES its destination entry every tick (verified:
    // travelEntry churns 10556->3121->3120... every snapshot). So we must NOT key on the destination
    // (it resets the window every tick); key purely on the BOT's NET DISPLACEMENT over wall-clock: if
    // the bot has physically moved <8y in the last 8s, it is stuck regardless of which target it is
    // currently thrashing on. (Per-tick wiggle reads as progress under a per-tick check, hence the
    // wall-clock window.)
    bool TravelMoveStuck(Player* bot, bool legitimatelyStationary = false)
    {
        if (!bot)
            return false;
        struct St { float x; float y; uint32 sinceMs; uint32 lastCheckMs; };
        constexpr uint32 WINDOW_MS = 20000;     // must net-progress within 20s of SCHEDULED time (8s still tripped through 1s-move/nap duty cycles: 31/min fleet)...
        constexpr float MIN_DISP = 8.0f;        // ...by at least 8 yards
        constexpr uint32 SCHED_GAP_MS = 7000;   // a gap between checks means the action wasn't running
        static std::mutex mx;
        static std::unordered_map<uint32, St> track;
        const uint32 now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        // Legit stationary states (combat, eating/drinking, casting) RESET the window instead of
        // aging it. Measured 13k abandons/75min fleet-wide: the window kept aging through combat,
        // so the first post-fight drink read as "frozen 30s" and killed every long journey.
        // SCHEDULING-AWARE: the window must be covered by CONSECUTIVE Executes. The activity
        // throttle (botActiveAlone) parks bots in multi-second naps and rpg/loot actions interleave;
        // the wall-clock window aged through those, so the first travel tick after a nap insta-
        // abandoned the target (measured 64.8 abandons/min = the entire fleet churn, 74% with the
        // bot having moved 0 yards). A real off-mesh freeze keeps Execute running every tick, so
        // requiring gap-free coverage still catches it.
        const bool schedulingGap = s.lastCheckMs && (now - s.lastCheckMs) > SCHED_GAP_MS;
        s.lastCheckMs = now;
        if (!s.sinceMs || legitimatelyStationary || schedulingGap)
        {
            s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.sinceMs = now;
            return false;
        }
        const float dx = bot->GetPositionX() - s.x, dy = bot->GetPositionY() - s.y;
        if (dx * dx + dy * dy >= MIN_DISP * MIN_DISP)   // real progress -> slide the window forward
        {
            s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.sinceMs = now;
            return false;
        }
        return (now - s.sinceMs) >= WINDOW_MS;          // barely moved the whole window -> stuck
    }

    // STRANDED RESCUE. Blacklist-and-replan assumes the TARGET is the problem; a bot in an
    // unpathable POSITION (measured: two bots swimming the Veiled Sea, z=-2, every destination
    // unreachable) abandons every target it picks, forever. If a bot racks up many abandons in
    // one spot, re-planning cannot help -> teleport it to the closest graveyard (never in view
    // of a real player).
    void TravelStrandedRescue(Player* bot, PlayerbotAI* ai)
    {
        if (!bot || !ai)
            return;
        struct Rs { uint32 count; uint32 firstMs; float x; float y; };
        constexpr uint32 WINDOW_MS = 180000;  // abandons must accumulate within 3 min...
        constexpr uint32 TRIGGER_COUNT = 6;   // ...at least 6 of them (window-reset caps rate at ~1 per 8-20s)...
        constexpr float MAX_DRIFT = 150.0f;   // ...without real relocation (ocean swim drifts 100y+ while hopeless)
        static std::mutex mx;
        static std::unordered_map<uint32, Rs> track;
        const uint32 now = WorldTimer::getMSTime();
        {   // decide under the lock, act after releasing it (TeleportTo takes map/grid locks)
            std::lock_guard<std::mutex> lk(mx);
            Rs& r = track[bot->GetGUIDLow()];
            const float dx = bot->GetPositionX() - r.x, dy = bot->GetPositionY() - r.y;
            if (!r.firstMs || (now - r.firstMs) > WINDOW_MS || dx * dx + dy * dy > MAX_DRIFT * MAX_DRIFT)
            {
                r.count = 1; r.firstMs = now; r.x = bot->GetPositionX(); r.y = bot->GetPositionY();
                return;
            }
            if (++r.count < TRIGGER_COUNT)
                return;
            r.count = 0; r.firstMs = 0;
        }
        if (ai->HasPlayerNearby())
            return;
        if (WorldSafeLocsEntry const* gy = sObjectMgr.GetClosestGraveYard(
                bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(), bot->GetTeam()))
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                sPlayerbotAIConfig.logEvent(ai, "TravelStrandedRescue", "graveyard",
                    "reason=repeated-abandons-no-displacement");
            bot->TeleportTo(gy->map_id, gy->x, gy->y, gy->z, bot->GetOrientation());
        }
    }
}

// v6 unroutable-destination suppression state: (bot guid, destination-title hash) -> holdUntilMs.
// Written by the degenerate-stub detector, honored at the top of Execute.
static std::mutex s_unroutableMx;
static std::unordered_map<uint64, uint32> s_unroutableUntil;
static uint64 UnroutableKey(Player* bot, TravelTarget* target)
{
    const uint32 h = (uint32)std::hash<std::string>{}(
        target && target->GetDestination() ? target->GetDestination()->GetTitle() : std::string());
    return ((uint64)bot->GetGUIDLow() << 32) | h;
}

bool MoveToTravelTargetAction::Execute(Event& event)
{
    MapManager::SetContinentUpdatePhase("travel-execute-entry", bot ? bot->GetGUIDLow() : 0);
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->GetDestination() || !target->GetPosition())
    {
        ClearBrokenTravelTarget(ai, context, target, "execute-entry");
        return false;
    }

    // Break travel FREEZES: physically not moving for 8s+ while still FAR from the destination
    // (>INTERACTION_DISTANCE, so it genuinely needs to travel and isn't just standing at an NPC doing
    // work) -> blacklist the objective + abandon the target so we re-plan to something reachable
    // instead of standing "moving" forever. Combat/eating/drinking/casting legitimately hold
    // position and must reset the window, not merely skip the check.
    const bool travelHoldLegit = sServerFacade.IsInCombat(bot) ||
        bot->IsNonMeleeSpellCasted(false) ||
        bot->getStandState() != UNIT_STAND_STATE_STAND ||
        bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    if (travelHoldLegit)
        TravelMoveStuck(bot, true); // reset the freeze window; do not age it through the hold
    else if (target->GetPosition()->distance(bot) > INTERACTION_DISTANCE &&
        TravelMoveStuck(bot))
    {
        // NullTravelDestination: nothing to blacklist, and clearing would erase the deliberate
        // COOLDOWN backoff the chooser put on it -> leave it alone (measured: half of one stranded
        // bot's abandons were the breaker fighting the null-target cooldown).
        if (!dynamic_cast<NullTravelDestination*>(target->GetDestination()))
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                sPlayerbotAIConfig.logEvent(ai, "TravelFrozenAbandoned",
                    target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
                    "reason=no-displacement window=8s");
            if (GuidPosition* gp = dynamic_cast<GuidPosition*>(target->GetPosition()))
                SkipQuestObjectiveGuid(ai, *gp, 120);
            ClearBrokenTravelTarget(ai, context, target, "travel-frozen");
            TravelMoveStuck(bot, true); // fresh 8s window for the NEXT target, not instant re-abandon
            TravelStrandedRescue(bot, ai); // repeated hopeless abandons in place -> unstick
        }
        return false;
    }

    // UNROUTABLE-DESTINATION SUPPRESSION (v6): SetStatus(COOLDOWN) alone did not hold -- the
    // chooser replaced the cooled target with a FRESH one for the same destination within
    // seconds (Betan: 353 stub-cycles/70min on one trainer). After the stub detector below
    // condemns a (bot, destination) pair, every re-pick of that destination short-circuits
    // here for 10 minutes: no pathfinding, no movement, action returns false so the engine
    // falls through to reachable work (grind/quest nearby).
    {
        std::lock_guard<std::mutex> lk(s_unroutableMx);
        auto it = s_unroutableUntil.find(UnroutableKey(bot, target));
        if (it != s_unroutableUntil.end())
        {
            if (WorldTimer::getMSTime() < it->second)
            {
                target->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
                target->SetExpireIn(60000);
                return false;
            }
            s_unroutableUntil.erase(it);
        }
    }

    auto dispatchQuestObjectiveCombat = [this, target](bool requireCombatRange, char const* source) -> bool
    {
        MapManager::SetContinentUpdatePhaseNamed("travel-combat-dispatch", source ? source : "unknown", bot ? bot->GetGUIDLow() : 0);
        QuestObjectiveTravelDestination* objectiveDest =
            target ? dynamic_cast<QuestObjectiveTravelDestination*>(target->GetDestination()) : nullptr;
        if (!objectiveDest || objectiveDest->GetEntry() <= 0)
            return false;

        ObjectGuid targetGuid;
        Unit* objectiveTarget = nullptr;
        if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
        {
            if (WorldObject* wo = guidPos->GetWorldObject(bot->GetInstanceId()))
            {
                targetGuid = wo->GetObjectGuid();
                objectiveTarget = dynamic_cast<Unit*>(wo);
            }
        }

        if (!objectiveTarget || !objectiveTarget->IsAlive() || !objectiveTarget->IsInWorld() || objectiveTarget->GetMapId() != bot->GetMapId())
        {
            float bestDistance = std::numeric_limits<float>::max();
            std::list<ObjectGuid> possibleTargets = AI_VALUE(std::list<ObjectGuid>, "possible targets");
            for (ObjectGuid const& possibleGuid : possibleTargets)
            {
                if (!possibleGuid.IsCreature() || possibleGuid.GetEntry() != objectiveDest->GetEntry())
                    continue;

                Creature* creature = ai->GetCreature(possibleGuid);
                if (!creature || !creature->IsAlive() || !creature->IsInWorld() || creature->GetMapId() != bot->GetMapId())
                    continue;

                float distance = sServerFacade.GetDistance2d(bot, creature);
                if (distance >= bestDistance)
                    continue;

                bestDistance = distance;
                targetGuid = possibleGuid;
                objectiveTarget = creature;
            }
        }

        const bool hostileCreatureObjective =
            targetGuid &&
            objectiveTarget &&
            objectiveTarget->IsAlive() &&
            objectiveTarget->IsInWorld() &&
            objectiveTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, objectiveTarget);

        if (!hostileCreatureObjective)
            return false;

        const float distance = sServerFacade.GetDistance2d(bot, objectiveTarget);
        const float distance3d = bot->GetDistance(objectiveTarget);
        const float verticalDelta = std::fabs(bot->GetPositionZ() - objectiveTarget->GetPositionZ());
        const bool inLos = bot->IsWithinLOSInMap(objectiveTarget, true);
        const bool ranged = ai->IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
        const float engageRange = ranged ? std::max<float>(ai->GetRange("spell"), 20.0f) + sPlayerbotAIConfig.contactDistance
                                         : std::max<float>(bot->GetCombinedCombatReach(objectiveTarget, true) + sPlayerbotAIConfig.contactDistance, 2.0f);
        if (requireCombatRange && distance > engageRange)
            return false;

        if (!inLos && verticalDelta > 6.0f)
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "entry=" << objectiveDest->GetEntry()
                    << " questId=" << objectiveDest->GetQuestId()
                    << " objective=" << static_cast<uint32>(objectiveDest->GetObjective())
                    << " guid=" << targetGuid.GetRawValue()
                    << " target=" << objectiveTarget->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << distance
                    << " dist3d=" << std::fixed << std::setprecision(2) << distance3d
                    << " dz=" << std::fixed << std::setprecision(2) << verticalDelta
                    << " los=0"
                    << " source=" << source
                    << " reason=vertical_los";
                sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveCombatDeferred", target->GetDestination()->GetTitle(), out.str());
            }

            return false;
        }

        bot->SetSelectionGuid(targetGuid);
        bot->SetTarget(objectiveTarget);
        SET_AI_VALUE(Unit*, "current target", objectiveTarget);
        SET_AI_VALUE(ObjectGuid, "attack target", targetGuid);

        if (!ranged && !bot->CanReachWithMeleeAutoAttack(objectiveTarget))
        {
            const bool moved = ChaseTo(objectiveTarget, 0.0f, 0.0f);
            if (moved)
                target->SetExpireIn(3000);

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "entry=" << objectiveDest->GetEntry()
                    << " questId=" << objectiveDest->GetQuestId()
                    << " objective=" << static_cast<uint32>(objectiveDest->GetObjective())
                    << " guid=" << targetGuid.GetRawValue()
                    << " target=" << objectiveTarget->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << distance
                    << " dist3d=" << std::fixed << std::setprecision(2) << distance3d
                    << " dz=" << std::fixed << std::setprecision(2) << verticalDelta
                    << " los=" << (inLos ? 1 : 0)
                    << " engageRange=" << std::fixed << std::setprecision(2) << engageRange
                    << " moved=" << (moved ? 1 : 0)
                    << " source=" << source;
                sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveCombatApproach", target->GetDestination()->GetTitle(), out.str());
            }

            if (!moved)
            {
                // see !primed branch: never leave a planted-but-disengaged current target behind
                RESET_AI_VALUE(Unit*, "current target");
                RESET_AI_VALUE(ObjectGuid, "attack target");
                bot->SetSelectionGuid(ObjectGuid());
            }
            return moved;
        }

        ai->StopMoving();

        bool pulled = false;
        if (ranged && !bot->IsInCombat() && !bot->GetVictim())
            pulled = ai->DoSpecificAction("pull my target", Event("attack anything", targetGuid, bot), true);

        const bool attacked = pulled || ai->DoSpecificAction("attack my target", Event("quest objective", targetGuid, bot), true);
        ai->OnCombatStarted();

        const bool primed =
            pulled ||
            attacked ||
            bot->GetVictim() == objectiveTarget ||
            (!ranged && bot->CanReachWithMeleeAutoAttack(objectiveTarget));

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "entry=" << objectiveDest->GetEntry()
                << " questId=" << objectiveDest->GetQuestId()
                << " objective=" << static_cast<uint32>(objectiveDest->GetObjective())
                << " guid=" << targetGuid.GetRawValue()
                << " target=" << objectiveTarget->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << distance
                << " dist3d=" << std::fixed << std::setprecision(2) << distance3d
                << " dz=" << std::fixed << std::setprecision(2) << verticalDelta
                << " los=" << (inLos ? 1 : 0)
                << " engageRange=" << std::fixed << std::setprecision(2) << engageRange
                << " ranged=" << (ranged ? 1 : 0)
                << " pulled=" << (pulled ? 1 : 0)
                << " attacked=" << (attacked ? 1 : 0)
                << " primed=" << (primed ? 1 : 0)
                << " combat=" << (bot->IsInCombat() ? 1 : 0)
                << " victim=" << (bot->GetVictim() == objectiveTarget ? 1 : 0)
                << " source=" << source;
            sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveCombatDispatch", target->GetDestination()->GetTitle(), out.str());
        }

        if (!primed)
        {
            // Failed engage must not leave a planted current/attack target behind: a live-but-
            // disengaged current target silences the "no target" trigger, so "attack anything"
            // is never even pushed (measured 164k bot-seconds stalled holding a target <=40y).
            RESET_AI_VALUE(Unit*, "current target");
            RESET_AI_VALUE(ObjectGuid, "attack target");
            bot->SetSelectionGuid(ObjectGuid());
            return false;
        }

        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        target->SetExpireIn(1000);
        context->ClearValues("no active travel destinations");
        RESET_AI_VALUE(bool, "travel target active");
        return true;
    };

    ClearStaleTravelAttackers(ai, context, bot, "execute");

    if (sServerFacade.IsInCombat(bot) || AI_VALUE(bool, "has attackers") || AI_VALUE(Unit*, "dps target"))
    {
        ai->StopMoving();
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0)
                << " attackers=" << (AI_VALUE(bool, "has attackers") ? 1 : 0)
                << " dpsTarget=" << (AI_VALUE(Unit*, "dps target") ? 1 : 0)
                << " status=" << static_cast<uint32>(target->GetStatus());
            sPlayerbotAIConfig.logEvent(ai, "TravelCombatInterrupt", target->GetDestination() ? target->GetDestination()->GetTitle() : "travel", out.str());
        }
        return false;
    }

    if (!ai->HasRealPlayerMaster() && !ai->HasActivePlayerMaster())
    {
        bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);
        bot->SetWalk(false, true);
    }

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
    {
        ai->TellDebug(ai->GetMaster(), "The target is ready to travel start now.", "debug travel");
        target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
    }

    MapManager::SetContinentUpdatePhase("travel-check-status", bot ? bot->GetGUIDLow() : 0);
    target->CheckStatus();

    MapManager::SetContinentUpdatePhase("travel-dispatch-range", bot ? bot->GetGUIDLow() : 0);
    if (dispatchQuestObjectiveCombat(true, "range"))
        return true;

    MapManager::SetContinentUpdatePhase("travel-status-after-check", bot ? bot->GetGUIDLow() : 0);
    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
    {
        MapManager::SetContinentUpdatePhase("travel-work-enter", bot ? bot->GetGUIDLow() : 0);
        MapManager::SetContinentUpdatePhase("travel-work-combat", bot ? bot->GetGUIDLow() : 0);
        if (dispatchQuestObjectiveCombat(false, "work"))
            return true;

        MapManager::SetContinentUpdatePhase("travel-work-go", bot ? bot->GetGUIDLow() : 0);
        if (QuestObjectiveTravelDestination* objectiveDest = dynamic_cast<QuestObjectiveTravelDestination*>(target->GetDestination()))
        {
            if (objectiveDest->GetEntry() < 0)
            {
                ObjectGuid targetGuid;
                GameObject* go = nullptr;
                if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
                {
                    MapManager::SetContinentUpdatePhase("travel-work-go-lookup", bot ? bot->GetGUIDLow() : 0);
                    if (WorldObject* wo = guidPos->GetWorldObject(bot->GetInstanceId()))
                    {
                        targetGuid = wo->GetObjectGuid();
                        go = dynamic_cast<GameObject*>(wo);
                    }
                }

                bool addedLoot = false;
                bool didQuestWork = false;
                bool pendingApproach = false;
                float goDistance = 0.0f;
                bool inInteractRange = false;
                bool skippedBusyObject = false;
                bool movedCloser = false;
                bool heldLootWindow = false;
                bool heldPendingOpen = false;
                if (targetGuid && go && go->IsInWorld() && go->GetMapId() == bot->GetMapId())
                {
                    goDistance = sServerFacade.GetDistance2d(bot, go);
                    inInteractRange = goDistance <= PLAYERBOT_QUEST_GO_WORK_RANGE && go->IsAtInteractDistance(bot);
                    bot->SetSelectionGuid(targetGuid);
                    if (HasQuestGoLootWindow(bot, targetGuid))
                    {
                        ai->StopMoving();
                        heldLootWindow = true;
                    }
                    else if (go->IsInUse() && IsQuestObjectiveGoOpenPending(ai, targetGuid))
                    {
                        ai->StopMoving();
                        heldPendingOpen = true;
                    }
                    else if (go->IsInUse())
                    {
                        SkipQuestObjectiveGuid(ai, targetGuid, 3);
                        skippedBusyObject = true;
                    }
                    else if (!inInteractRange)
                    {
                        MapManager::SetContinentUpdatePhase("travel-work-go-approach", bot ? bot->GetGUIDLow() : 0);
                        WorldPosition goPos(go);
                        WorldPosition approachPoint = GetQuestObjectiveCampPoint(objectiveDest, target, goPos, bot, 120);
                        movedCloser = MoveTo(approachPoint.getMapId(), approachPoint.getX(), approachPoint.getY(), approachPoint.getZ(), false, false);
                        if (movedCloser)
                            WaitForReach(std::max<float>(WorldPosition(bot).distance(approachPoint), 1.0f));
                    }
                    else
                    {
                        MapManager::SetContinentUpdatePhase("travel-work-go-use", bot ? bot->GetGUIDLow() : 0);
                        didQuestWork = TryUseQuestItemOn(ai, bot, go, "gameobject-work");
                        if (!didQuestWork)
                        {
                            MapManager::SetContinentUpdatePhase("travel-work-go-loot", bot ? bot->GetGUIDLow() : 0);
                            addedLoot = AI_VALUE(LootObjectStack*, "available loot")->Add(targetGuid);
                            SET_AI_VALUE(LootObject, "loot target", LootObject(bot, targetGuid));
                            didQuestWork = ai->DoSpecificAction("open loot", Event("quest objective", targetGuid, bot), true);
                            pendingApproach = didQuestWork && bot->GetLootGuid() != targetGuid &&
                                !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);
                            if (pendingApproach)
                                MarkQuestObjectiveGoOpenPending(ai, targetGuid, 3);
                        }
                    }
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "source=move-to-travel"
                        << " entry=" << objectiveDest->GetEntry()
                        << " questId=" << objectiveDest->GetQuestId()
                        << " objective=" << static_cast<uint32>(objectiveDest->GetObjective())
                        << " guid=" << (targetGuid ? targetGuid.GetRawValue() : 0)
                        << " go=" << (go ? go->GetName() : "none")
                        << " goType=" << (go ? uint32(go->GetGoType()) : 0)
                        << " goState=" << (go ? uint32(go->GetGoState()) : 0)
                        << " lootState=" << (go ? uint32(go->getLootState()) : 0)
                        << " inUse=" << (go && go->IsInUse() ? 1 : 0)
                        << " questActive=" << (go && sObjectMgr.IsGameObjectForQuests(go->GetEntry()) && go->ActivateToQuest(bot) ? 1 : 0)
                        << " dist=" << std::fixed << std::setprecision(2) << goDistance
                        << " interact=" << (inInteractRange ? 1 : 0)
                        << " pendingApproach=" << (pendingApproach ? 1 : 0)
                        << " heldLootWindow=" << (heldLootWindow ? 1 : 0)
                        << " heldPendingOpen=" << (heldPendingOpen ? 1 : 0)
                        << " addedLoot=" << (addedLoot ? 1 : 0)
                        << " didQuestWork=" << (didQuestWork ? 1 : 0)
                        << " skippedBusyObject=" << (skippedBusyObject ? 1 : 0)
                        << " movedCloser=" << (movedCloser ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveGoWorkAction", target->GetDestination()->GetTitle(), out.str());
                }

                if (heldLootWindow || heldPendingOpen)
                {
                    SetDuration(sPlayerbotAIConfig.lootDelay);
                    target->SetExpireIn(3000);
                    return true;
                }

                if (pendingApproach || movedCloser)
                {
                    target->SetExpireIn(10000);
                    return true;
                }

                target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                target->SetExpireIn(didQuestWork || skippedBusyObject ? 1000 : 5000);
                context->ClearValues("no active travel destinations");
                RESET_AI_VALUE(bool, "travel target active");
                return didQuestWork || skippedBusyObject;
            }
        }

        MapManager::SetContinentUpdatePhase("travel-work-camp", bot ? bot->GetGUIDLow() : 0);
        if (QuestObjectiveTravelDestination* objectiveDest = dynamic_cast<QuestObjectiveTravelDestination*>(target->GetDestination()))
        {
            if (objectiveDest->GetEntry() > 0 && objectiveDest->GetSize() == 1)
            {
                ObjectGuid targetGuid;
                Unit* objectiveUnit = nullptr;
                if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
                {
                    MapManager::SetContinentUpdatePhase("travel-work-camp-lookup", bot ? bot->GetGUIDLow() : 0);
                    if (WorldObject* wo = guidPos->GetWorldObject(bot->GetInstanceId()))
                    {
                        targetGuid = wo->GetObjectGuid();
                        objectiveUnit = dynamic_cast<Unit*>(wo);
                    }
                }

                if (targetGuid && objectiveUnit && objectiveUnit->IsAlive() && objectiveUnit->IsInWorld() && objectiveUnit->GetMapId() == bot->GetMapId())
                {
                    const float unitDistance = sServerFacade.GetDistance2d(bot, objectiveUnit);
                    if (unitDistance > INTERACTION_DISTANCE)
                    {
                        MapManager::SetContinentUpdatePhase("travel-work-camp-approach", bot ? bot->GetGUIDLow() : 0);
                        bool moved = MoveTo(objectiveUnit->GetMapId(), objectiveUnit->GetPositionX(), objectiveUnit->GetPositionY(), objectiveUnit->GetPositionZ(), false, false);
                        if (moved)
                            WaitForReach(std::max<float>(unitDistance, 1.0f));

                        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                        {
                            std::ostringstream out;
                            out << "entry=" << objectiveDest->GetEntry()
                                << " questId=" << objectiveDest->GetQuestId()
                                << " objective=" << static_cast<uint32>(objectiveDest->GetObjective())
                                << " target=" << objectiveUnit->GetName()
                                << " guid=" << targetGuid.GetRawValue()
                                << " dist=" << std::fixed << std::setprecision(2) << unitDistance
                                << " moved=" << (moved ? 1 : 0);
                            sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveCreatureApproach", target->GetDestination()->GetTitle(), out.str());
                        }

                        if (moved)
                            return true;
                    }
                    else
                    {
                        MapManager::SetContinentUpdatePhase("travel-work-camp-use", bot ? bot->GetGUIDLow() : 0);
                        if (TryUseQuestItemOn(ai, bot, objectiveUnit, "creature-work"))
                        {
                            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                            target->SetExpireIn(1000);
                            context->ClearValues("no active travel destinations");
                            RESET_AI_VALUE(bool, "travel target active");
                            return true;
                        }
                    }
                }

                std::ostringstream qualifier;
                qualifier << "quest objective camp start::"
                          << objectiveDest->GetQuestId() << ":"
                          << static_cast<uint32>(objectiveDest->GetObjective()) << ":"
                          << objectiveDest->GetEntry();

                time_t campStart = AI_VALUE2(time_t, "manual time", qualifier.str());
                const time_t now = time(0);
                if (!campStart)
                {
                    campStart = now;
                    SET_AI_VALUE2(time_t, "manual time", qualifier.str(), campStart);
                }

                WorldPosition spawnLocation = *target->GetPosition();
                WorldPosition campPoint = GetQuestObjectiveCampPoint(objectiveDest, target, spawnLocation, bot, uint32(now - campStart));
                WorldPosition botLocation(bot);
                const float desiredRadius = uint32(now - campStart) >= 90 ? 2.8f :
                    (uint32(now - campStart) >= 45 ? 3.8f :
                    (uint32(now - campStart) >= 20 ? 5.0f : 6.5f));

                if (botLocation.getMapId() == campPoint.getMapId() && botLocation.sqDistance2d(campPoint) > desiredRadius * desiredRadius)
                {
                    MapManager::SetContinentUpdatePhase("travel-work-camp-move", bot ? bot->GetGUIDLow() : 0);
                    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                    {
                        std::ostringstream out;
                        out << "entry=" << objectiveDest->GetEntry()
                            << " questId=" << objectiveDest->GetQuestId()
                            << " objective=" << static_cast<uint32>(objectiveDest->GetObjective())
                            << " waitSec=" << uint32(now - campStart)
                            << " dist=" << std::fixed << std::setprecision(2) << std::sqrt(botLocation.sqDistance2d(campPoint))
                            << " desired=" << std::fixed << std::setprecision(2) << desiredRadius
                            << " x=" << campPoint.getX()
                            << " y=" << campPoint.getY()
                            << " z=" << campPoint.getZ();
                        sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveCampMove", target->GetDestination()->GetTitle(), out.str());
                    }

                    bool moved = MoveTo(campPoint.getMapId(), campPoint.getX(), campPoint.getY(), campPoint.getZ(), false, false);
                    if (moved)
                        WaitForReach(std::max<float>(botLocation.distance(campPoint), 1.0f));
                    return moved;
                }
            }
        }

        MapManager::SetContinentUpdatePhase("travel-work-quest", bot ? bot->GetGUIDLow() : 0);
        if (QuestRelationTravelDestination* questDest = dynamic_cast<QuestRelationTravelDestination*>(target->GetDestination()))
        {
            ObjectGuid targetGuid;
            WorldObject* questWo = nullptr;
            if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
            {
                MapManager::SetContinentUpdatePhase("travel-work-quest-lookup", bot ? bot->GetGUIDLow() : 0);
                if (WorldObject* wo = guidPos->GetWorldObject(bot->GetInstanceId()))
                {
                    targetGuid = wo->GetObjectGuid();
                    questWo = wo;
                }
            }

            if (targetGuid)
                bot->SetSelectionGuid(targetGuid);

            if (questWo && questWo->IsInWorld() && questWo->GetMapId() == bot->GetMapId() &&
                (!sServerFacade.IsWithinLOSInMap(bot, questWo) ||
                 sServerFacade.GetDistance2d(bot, questWo) > INTERACTION_DISTANCE))
            {
                MapManager::SetContinentUpdatePhase("travel-work-quest-approach", bot ? bot->GetGUIDLow() : 0);
                bool moved = MoveTo(questWo->GetMapId(), questWo->GetPositionX(), questWo->GetPositionY(), questWo->GetPositionZ(), false, false);
                if (moved)
                    WaitForReach(std::max<float>(bot->GetDistance(questWo), 1.0f));

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "source=move-to-travel"
                        << " entry=" << questDest->GetEntry()
                        << " questId=" << questDest->GetQuestId()
                        << " relation=" << (uint32)questDest->GetRelation()
                        << " guid=" << targetGuid.GetRawValue()
                        << " npc=" << questWo->GetName()
                        << " dist=" << std::fixed << std::setprecision(2) << bot->GetDistance(questWo)
                        << " dist2d=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, questWo)
                        << " los=" << (sServerFacade.IsWithinLOSInMap(bot, questWo) ? 1 : 0)
                        << " moved=" << (moved ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(ai, "QuestTravelNpcApproach", target->GetDestination()->GetTitle(), out.str());
                }

                return moved;
            }

            bool didQuestWork = false;
            bool usedFallback = false;
            const std::string questIdParam = std::to_string(questDest->GetQuestId());
            MapManager::SetContinentUpdatePhase("travel-work-quest-action", bot ? bot->GetGUIDLow() : 0);
            if (questDest->GetRelation() == 0)
            {
                if (targetGuid)
                    didQuestWork = ai->DoSpecificAction("talk to quest giver", Event("quest travel", targetGuid, bot), true);

                if (!didQuestWork)
                {
                    usedFallback = true;
                    didQuestWork = ai->DoSpecificAction("accept quest", Event("quest travel", questIdParam, bot), true);
                }
            }
            else
            {
                didQuestWork = targetGuid &&
                    ai->DoSpecificAction("talk to quest giver", Event("quest travel", targetGuid, bot), true);
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "source=move-to-travel"
                    << " entry=" << questDest->GetEntry()
                    << " questId=" << questDest->GetQuestId()
                    << " relation=" << (uint32)questDest->GetRelation()
                    << " requestedQuestId=" << questIdParam
                    << " guid=" << (targetGuid ? targetGuid.GetRawValue() : 0)
                    << " didQuestWork=" << (didQuestWork ? 1 : 0)
                    << " fallback=" << (usedFallback ? 1 : 0);
                sPlayerbotAIConfig.logEvent(ai, "QuestTravelWorkAction", target->GetDestination()->GetTitle(), out.str());
            }

            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            if (!didQuestWork)
            {
                const time_t suppressUntil = time(0) + 30;
                std::ostringstream skipQualifier;
                skipQualifier << "quest relation skip::" << questDest->GetEntry() << ":" << questDest->GetQuestId() << ":" << (uint32)questDest->GetRelation();
                SET_AI_VALUE2(time_t, "manual time", skipQualifier.str(), suppressUntil);
                std::ostringstream entrySkipQualifier;
                entrySkipQualifier << "quest relation entry skip::" << questDest->GetEntry() << ":" << (uint32)questDest->GetRelation();
                SET_AI_VALUE2(time_t, "manual time", entrySkipQualifier.str(), suppressUntil);
                target->SetExpireIn(15000);
                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    sPlayerbotAIConfig.logEvent(ai, "QuestTravelReplan",
                        target->GetDestination()->GetTitle(),
                        "reason=no-work refreshMs=15000 suppressSec=30 source=move-to-travel");
                }
            }
            context->ClearValues("no active travel destinations");
            RESET_AI_VALUE(bool, "travel target active");
            return didQuestWork;
        }

        MapManager::SetContinentUpdatePhase("travel-work-done", bot ? bot->GetGUIDLow() : 0);
    }

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL)
        return true;

    MapManager::SetContinentUpdatePhase("travel-travel-enter", bot ? bot->GetGUIDLow() : 0);
    WorldPosition botLocation(bot);
    WorldPosition location = *target->GetPosition();
    bool usingQuestNpcStandPoint = false;
    WorldPosition exactTravelLocation = location;

    if (ShouldUseQuestNpcStandPoint(target, location, bot))
    {
        MapManager::SetContinentUpdatePhase("travel-standpoint", bot ? bot->GetGUIDLow() : 0);
        WorldPosition standPoint = GetQuestNpcStandPoint(target, location, bot);
        if (standPoint != location)
        {
            location = standPoint;
            usingQuestNpcStandPoint = true;
        }
    }
    
    Group* group = bot->GetGroup();
    const bool allowGroupWaitPause = false;
    if (allowGroupWaitPause && group && ai->HasActivePlayerMaster() && ai->IsGroupLeader() && !urand(0, 1) && !bot->IsInCombat())
    {        
        MapManager::SetContinentUpdatePhase("travel-group-wait-check", bot ? bot->GetGUIDLow() : 0);
        uint32 scanned = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            if (++scanned > 40)
            {
                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                    sPlayerbotAIConfig.logEvent(ai, "GroupIterationGuard", "move to travel target", "reason=group-reference-scan-cap");
                break;
            }

            Player* member = ref->getSource();
            if (!member || member == bot)
                continue;

            if (!member->IsAlive())
                continue;

            if (!member->IsMoving())
                continue;

            if (member->GetPlayerbotAI() &&
                !(member->GetPlayerbotAI()->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || member->GetPlayerbotAI()->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
                continue;

            WorldPosition memberPos(member);
            WorldPosition targetPos = *target->GetPosition();

            float memberDistance = std::min(botLocation.distance(memberPos), location.distance(memberPos));

            if (memberDistance < 50.0f)
                continue;
            if (memberDistance > sPlayerbotAIConfig.reactDistance * 20)
                continue;

           // float memberAngle = botLocation.getAngleBetween(targetPos, memberPos);

           // if (botLocation.getMapId() == targetPos.getMapId() && botLocation.getMapId() == memberPos.getMapId() && memberAngle < M_PI_F / 2) //We are heading that direction anyway.
           //     continue;

            if (!urand(0, 5))
            {
                std::ostringstream out;
                if ((ai->GetMaster() && !bot->GetGroup()->IsMember(ai->GetMaster()->GetObjectGuid())) || !ai->HasActivePlayerMaster())
                    out << "Waiting a bit for ";
                else
                    out << "Please hurry up ";

                out << member->GetName();

                if (bot->GetPlayerbotAI() && !ai->HasActivePlayerMaster())
                {
                    out << " who is " << round(memberDistance) << "y away";
                    if (!memberPos.getAreaName().empty())
                        out << " in " << memberPos.getAreaName();
                }

                ai->TellPlayerNoFacing(GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            }

            // Introduce a random delay between 80% and 120% of maxWaitForMove to make waiting more natural
            uint32 randomDelay = sPlayerbotAIConfig.maxWaitForMove * (urand(80, 120) / 100.0f);
            target->SetExpireIn(target->GetTimeLeft() + randomDelay);

            SetDuration(randomDelay);

            sPlayerbotAIConfig.logEvent(ai, "HumanLikePause", member->GetName(), std::to_string(randomDelay));

            // Occasionally face the member and perform an emote
            if (urand(0, 3) == 0) { // 25% chance to emote
                bot->SetFacingToObject(member);
                uint32 emoteChoice = urand(0, 2);
                switch (emoteChoice) {
                    case 0:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_POINT);
                        break;
                    case 1:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                        break;
                    case 2:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_EXCLAMATION);
                        break;
                }
            }

            return true;
        }
    }
    MapManager::SetContinentUpdatePhase("travel-group-wait-done", bot ? bot->GetGUIDLow() : 0);

    float x = location.getX();
    float y = location.getY();
    float z = location.getZ();
    float mapId = location.getMapId();

    const bool humanLikeTravel = false;
    time_t now = time(0);
    HumanLikeTravelState humanState;
    if (humanLikeTravel)
    {
        std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
        humanState = s_humanLikeTravelStates[bot->GetGUIDLow()];
    }

    if (humanLikeTravel)
    {
        if (humanState.nextTrace <= now)
        {
            std::ostringstream trace;
            trace << "combat=" << (bot->IsInCombat() ? 1 : 0)
                  << " mounted=" << (bot->IsMounted() ? 1 : 0)
                  << " sameMap=" << (botLocation.getMapId() == location.getMapId() ? 1 : 0)
                  << " sq2d=" << std::fixed << std::setprecision(2) << botLocation.sqDistance2d(location);

            sPlayerbotAIConfig.logEvent(
                ai,
                "HumanLikeTravelTrace",
                target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
                trace.str());

            std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
            s_humanLikeTravelStates[bot->GetGUIDLow()].nextTrace = now + 15;
        }
    }

    if (botLocation.getMapId() == location.getMapId() && botLocation.sqDistance2d(location) < 10000.0f)
    {
        if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Moving to ";
            out << target->GetDestination()->GetTitle();
            if (!(*target->GetPosition() == WorldPosition()))
            {
                out << " at " << uint32(target->GetPosition()->distance(bot)) << "y";
            }
            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " for " << (target->GetTimeLeft() / 1000) << "s";
            if (target->GetRetryCount(true))
                out << " (move retry: " << target->GetRetryCount(true) << ")";
            else if (target->GetRetryCount(false))
                out << " (retry: " << target->GetRetryCount(false) << ")";
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    if (humanLikeTravel && !bot->IsInCombat() && !bot->IsMounted())
    {
        sPlayerbotAIConfig.logEvent(
            ai,
            "HumanLikeTravelEligible",
            target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
            "eligible");

        if (humanState.nextPause <= now && frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikePauseChance)
        {
            uint32 pauseMs = urand(sPlayerbotAIConfig.humanLikePauseMinMs, sPlayerbotAIConfig.humanLikePauseMaxMs);
            SetDuration(pauseMs);
            sPlayerbotAIConfig.logEvent(ai, "HumanLikePause", target->GetDestination() ? target->GetDestination()->GetTitle() : "travel", std::to_string(pauseMs));

            {
                std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
                HumanLikeTravelState& state = s_humanLikeTravelStates[bot->GetGUIDLow()];
                state.nextPause = now + (sPlayerbotAIConfig.humanLikePauseCooldownMs / 1000);
            }

            return true;
        }

        if (frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikePathJitterChance)
        {
            float dx = location.getX() - botLocation.getX();
            float dy = location.getY() - botLocation.getY();
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.01f)
            {
                dx /= len;
                dy /= len;
                float lateralX = -dy;
                float lateralY = dx;

                float forwardJitter = frand(-sPlayerbotAIConfig.humanLikePathForwardJitterRadius,
                                            sPlayerbotAIConfig.humanLikePathForwardJitterRadius);
                float lateralJitter = frand(-sPlayerbotAIConfig.humanLikePathJitterRadius,
                                            sPlayerbotAIConfig.humanLikePathJitterRadius);

                x += dx * forwardJitter + lateralX * lateralJitter;
                y += dy * forwardJitter + lateralY * lateralJitter;

                sPlayerbotAIConfig.logEvent(
                    ai,
                    "HumanLikePathJitter",
                    std::to_string(forwardJitter),
                    std::to_string(lateralJitter));
            }
        }

        if (humanState.nextJump <= now)
        {
            if (frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikeSpinChance)
            {
                float spinOffset = frand(0.9f * static_cast<float>(M_PI), 1.8f * static_cast<float>(M_PI));
                float wobble = frand(-0.45f, 0.45f);
                bot->SetFacingTo(bot->GetOrientation() + spinOffset + wobble);
                sPlayerbotAIConfig.logEvent(ai, "HumanLikeSpin", std::to_string(spinOffset), std::to_string(wobble));
            }

            if (frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikeJumpChance)
            {
                ai->DoSpecificAction("jump::random", Event(), true);
            }

            {
                std::lock_guard<std::mutex> guard(s_humanLikeTravelMutex);
                HumanLikeTravelState& state = s_humanLikeTravelStates[bot->GetGUIDLow()];
                state.nextJump = now + (sPlayerbotAIConfig.humanLikeJumpCooldownMs / 1000);
            }
        }
    }

    MapManager::SetContinentUpdatePhase("travel-move-dispatch", bot ? bot->GetGUIDLow() : 0);
    bool canMove = MoveTo(mapId, x, y, z, false, false);

    // DEGENERATE-STUB DETECTOR (root cause of the 16% move-idle doubling): toward an
    // UNROUTABLE far destination (e.g. Elwynn -> Ironforge, tram-only) the pathfinder returns
    // short INCOMPLETE stubs; MoveTo "succeeds", the bot hops in place, and the hops evade the
    // 8y/20s no-displacement breaker forever. Thresholds from live traces: shufflers produce
    // SUSTAINED 7-31y stubs (Therineri avg 15y, Betan avg 24y); legitimate chunked long-hauls
    // reach 360y+. Three trips in a row -> destination is unroutable -> blacklist + re-plan
    // DIRECTLY (the earlier canMove=false/IncRetry version never fired: intermittent >15y
    // stubs kept resetting the retry count).
    if (canMove && mapId == bot->GetMapId())
    {
        const float sdx = x - bot->GetPositionX(), sdy = y - bot->GetPositionY();
        if (sdx * sdx + sdy * sdy > 150.0f * 150.0f)
        {
            // NOTE: cannot read bot->movespline here -- MovePoint defers path computation to
            // the next unit update, so the spline still holds the PREVIOUS (finalized) move.
            // Recompute the path directly (same probe the trace validated; only for >150y
            // requests, ~0.2ms) and measure how far it actually reaches.
            PathFinder dpf(bot);
            dpf.calculate(x, y, z, false);
            auto const& dpts = dpf.getPath();
            float freach2 = 1e18f;
            if (!dpts.empty())
            {
                const float fdx = dpts.back().x - bot->GetPositionX();
                const float fdy = dpts.back().y - bot->GetPositionY();
                freach2 = fdx * fdx + fdy * fdy;
            }
            if (freach2 < 35.0f * 35.0f)
            {
                static std::mutex dgMx;
                static std::unordered_map<uint32, std::pair<uint32, uint32>> dgTrips; // guid -> {count, lastMs}
                const uint32 dgNow = WorldTimer::getMSTime();
                bool abandon = false;
                {
                    std::lock_guard<std::mutex> lk(dgMx);
                    auto& t = dgTrips[bot->GetGUIDLow()];
                    if (dgNow - t.second > 60000)
                        t.first = 0;               // stale window -> fresh count
                    t.second = dgNow;
                    if (++t.first >= 3)
                    {
                        t.first = 0;
                        abandon = true;
                    }
                }
                // (DGTRIP temp diagnostic removed after v6 verification)

                // NO null-destination exemption here (unlike the freeze breaker): this branch
                // only runs for >150y requests, and a null "camp" anchor 150y+ away means the
                // bot was teleported/rescued after it was set -- broken garbage that must be
                // cleared, not a deliberate cooldown to protect.
                if (abandon)
                {
                    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                        sPlayerbotAIConfig.logEvent(ai, "TravelUnroutableAbandoned",
                            target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
                            "reason=degenerate-path-stubs");
                    // Register the 10-minute (bot, destination) suppression -- the top-of-Execute
                    // gate then blocks every re-pick of this destination without pathfinding
                    // (SetStatus(COOLDOWN) alone did not hold: the chooser recreated the target).
                    {
                        std::lock_guard<std::mutex> lk(s_unroutableMx);
                        s_unroutableUntil[UnroutableKey(bot, target)] = dgNow + 600000;
                    }
                    target->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
                    target->SetForced(false);
                    if (GuidPosition* gp = dynamic_cast<GuidPosition*>(target->GetPosition()))
                        SkipQuestObjectiveGuid(ai, *gp, 300);   // extra help when it IS a quest objective
                    TravelMoveStuck(bot, true);    // fresh breaker window for the NEXT target
                    return false;
                }
            }
        }
    }



    if (!canMove && usingQuestNpcStandPoint)
    {
        MapManager::SetContinentUpdatePhase("travel-move-fallback", bot ? bot->GetGUIDLow() : 0);
        x = exactTravelLocation.getX();
        y = exactTravelLocation.getY();
        z = exactTravelLocation.getZ();
        mapId = exactTravelLocation.getMapId();
        canMove = MoveTo(mapId, x, y, z, false, false);
    }

    if (!canMove)
    {
        MapManager::SetContinentUpdatePhase("travel-move-failed", bot ? bot->GetGUIDLow() : 0);
        target->IncRetry(true);

        if (target->IsMaxRetry(true))
        {
            ai->TellDebug(ai->GetMaster(), "The target is cooling down because we failed to move to it a few times in a row.", "debug travel");
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=move-failed"
                    << " retries=max"
                    << " status=" << static_cast<uint32>(target->GetStatus())
                    << " destination=" << (target->GetDestination() ? target->GetDestination()->GetTitle() : "none")
                    << " botMap=" << bot->GetMapId()
                    << " targetMap=" << mapId
                    << " targetX=" << x
                    << " targetY=" << y
                    << " targetZ=" << z;
                sPlayerbotAIConfig.logEvent(ai, "TravelCooldownTrace", target->GetDestination() ? target->GetDestination()->GetTitle() : "travel", out.str());
            }
            target->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
            target->SetForced(false);

            // P2 anti-churn: if we repeatedly fail to PATH to a quest objective
            // (NPC on another floor, inside a cave, across unreachable terrain),
            // blacklist that specific objective for a while so the next re-plan
            // picks a DIFFERENT, reachable objective instead of looping forever on
            // the same unreachable destination (the dominant cause of the ~100k
            // RequestQuestTravelTargetAction re-plans/run). The selection path in
            // ChooseTravelTargetAction already honors this skip key.
            if (GuidPosition* guidPos = dynamic_cast<GuidPosition*>(target->GetPosition()))
            {
                SkipQuestObjectiveGuid(ai, *guidPos, 120);
                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                    sPlayerbotAIConfig.logEvent(ai, "TravelObjectiveUnreachableBlacklisted",
                        target->GetDestination() ? target->GetDestination()->GetTitle() : "travel",
                        "skip=120s reason=path-fail");
            }
        }
    }
    else
    {
        target->DecRetry(true);
        MapManager::SetContinentUpdatePhase("travel-move-wait", bot ? bot->GetGUIDLow() : 0);
        WaitForReach(std::max<float>(botLocation.distance(WorldPosition(mapId, x, y, z)), 1.0f));
    }

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        WorldPosition* pos = target->GetPosition();
        GuidPosition* guidP = dynamic_cast<GuidPosition*>(pos);

        std::string name = (guidP && guidP->GetWorldObject(bot->GetInstanceId())) ? chat->formatWorldobject(guidP->GetWorldObject(bot->GetInstanceId())) : "travel target";

        if (mapId == bot->GetMapId())
        {
            ai->Poi(x, y, name);
        }
        else
        {
            LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
            if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(location) < 20.0f)
            {
                for (auto& p : lastMove.lastPath.getPointPath())
                {
                    if (p.getMapId() == bot->GetMapId())
                        ai->Poi(p.getX(), p.getY(), name);
                }
            }
        }
    }
     
    return canMove;
}

bool MoveToTravelTargetAction::isUseful()
{
    MapManager::SetContinentUpdatePhase("travel-useful-entry", bot ? bot->GetGUIDLow() : 0);
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    MapManager::SetContinentUpdatePhase("travel-useful-combat", bot ? bot->GetGUIDLow() : 0);
    ClearStaleTravelAttackers(ai, context, bot, "isUseful");

    if (sServerFacade.IsInCombat(bot) || AI_VALUE(bool, "has attackers") || AI_VALUE(Unit*, "dps target"))
        return false;

    // POST-FLEE HOLD: after fleeing, resuming travel IMMEDIATELY routed the bot straight back through
    // the mob it just escaped -> re-aggro -> flee -> travel -> ... (watched Shugevy ping-pong across a
    // boar's aggro boundary at 27 direction-reversals/min). A real player waits out the leash and
    // recovers first. Hold travel for 10s after a flee while below 90% HP (eat/regen fills the gap).
    {
        const time_t lastFlee = AI_VALUE(LastMovement&, "last movement").lastFlee;
        if (lastFlee && time(0) - lastFlee < 10 && bot->GetHealthPercent() < 90.0f)
            return false;
    }

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    if (!travelTarget)
        return false;

    if (!travelTarget->GetDestination() || !travelTarget->GetPosition())
    {
        ClearBrokenTravelTarget(ai, context, travelTarget, "isUseful-entry");
        return false;
    }

    if (travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
    {
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
        travelTarget->SetForced(false);
        RESET_AI_VALUE(bool, "travel target active");
        RESET_AI_VALUE(GuidPosition, "rpg target");
        RESET_AI_VALUE(ObjectGuid, "attack target");
        ai->GetAiObjectContext()->ClearValues("no active travel destinations");

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "reason=expired"
                << " destination=" << (travelTarget->GetDestination() ? travelTarget->GetDestination()->GetTitle() : "none");
            sPlayerbotAIConfig.logEvent(ai, "TravelExpiredReset", "move to travel target", out.str());
        }

        return false;
    }

    MapManager::SetContinentUpdatePhase("travel-useful-target", bot ? bot->GetGUIDLow() : 0);
    if (!AI_VALUE(bool, "travel target traveling") && travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_READY)
        return false;

    MapManager::SetContinentUpdatePhase("travel-useful-taxi", bot ? bot->GetGUIDLow() : 0);
    if (bot->IsTaxiFlying())
        return false;

    MapManager::SetContinentUpdatePhase("travel-useful-moving", bot ? bot->GetGUIDLow() : 0);
    if (MEM_AI_VALUE(WorldPosition, "current position")->LastChangeDelay() < 10)
#ifndef MANGOSBOT_ZERO
        if (bot->IsMovingIgnoreFlying())
            return false;
#else
        if (bot->IsMoving())
            return false;
#endif

    MapManager::SetContinentUpdatePhase("travel-useful-canmove", bot ? bot->GetGUIDLow() : 0);
    if (!ai->CanMove() || bot->GetTradeData() || bot->IsNonMeleeSpellCasted(true, false, true))
        return false;

    MapManager::SetContinentUpdatePhase("travel-useful-follow", bot ? bot->GetGUIDLow() : 0);
    if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT))
    {
        auto conditions = travelTarget->GetConditions();
        for (auto& cond : conditions)
        {
            if (cond == "should travel named::guild order")
                return false;
        }
    }

    MapManager::SetContinentUpdatePhase("travel-useful-group", bot ? bot->GetGUIDLow() : 0);
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetObjectGuid()))
        if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT))
            if (!travelTarget->IsForced())
                return false;

    MapManager::SetContinentUpdatePhase("travel-useful-pos", bot ? bot->GetGUIDLow() : 0);
    WorldPosition travelPos(*travelTarget->GetPosition());

    MapManager::SetContinentUpdatePhase("travel-useful-dungeon", bot ? bot->GetGUIDLow() : 0);
    if (travelPos.isDungeon() && bot->GetGroup() && bot->GetGroup()->IsLeader(bot->GetObjectGuid()) && sTravelMgr.MapTransDistance(bot, travelPos, true) < sPlayerbotAIConfig.sightDistance && !AI_VALUE2(bool, "group and", "near leader"))
        return false;
     
    MapManager::SetContinentUpdatePhase("travel-useful-loot", bot ? bot->GetGUIDLow() : 0);
    if (AI_VALUE(bool, "has available loot"))
    {
        LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
        LootObject lootObject = availableLoot ? availableLoot->GetLoot(sPlayerbotAIConfig.lootDistance) : LootObject();
        if (!lootObject.IsEmpty() && lootObject.IsLootPossible(bot))
        {
            const bool sameQuestGo = IsSameQuestGameObjectLoot(travelTarget, lootObject, bot);
            if (sameQuestGo && sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=same-quest-go-loot"
                    << " lootGuid=" << lootObject.guid.GetRawValue()
                    << " entry=" << travelTarget->GetEntry()
                    << " status=" << static_cast<uint32>(travelTarget->GetStatus());
                sPlayerbotAIConfig.logEvent(ai, "TravelAvailableLootBypass",
                    travelTarget->GetDestination() ? travelTarget->GetDestination()->GetTitle() : "travel", out.str());
            }
            if (!sameQuestGo)
                return false;
        }
    }

    MapManager::SetContinentUpdatePhase("travel-useful-freemove", bot ? bot->GetGUIDLow() : 0);
    if (!travelTarget->IsForced())
        if (!CanFreeMoveValue::CanFreeMoveTo(ai, *travelTarget->GetPosition()))
            return false;

    MapManager::SetContinentUpdatePhase("travel-useful-done", bot ? bot->GetGUIDLow() : 0);
    return true;
}
