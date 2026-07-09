#include "PlayerbotMgr.h"
#include "playerbot/playerbot.h"
#include "playerbot/mind/Mind.h"
#include "playerbot/PerformanceMonitor.h"
#include "PerfStats.h"
#include "MapManager.h"
#include <algorithm>
#include <stdarg.h>
#include <cmath>
#include <iomanip>
#include <map>
#include <mutex>
#include <unordered_map>

#include "playerbot/AiFactory.h"

#include "Movement/MovementGenerator.h"
#include "Movement/MotionMaster.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "strategy/values/LastMovementValue.h"
#include "strategy/actions/LogLevelAction.h"
#include "strategy/actions/SayAction.h"
#include "strategy/actions/EmoteAction.h"
#include "strategy/actions/MoveToTravelTargetAction.h"
#include "strategy/actions/ReviveFromCorpseAction.h"
#include "strategy/values/LastSpellCastValue.h"
#include "LootObjectStack.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include "BotLearningMgr.h"
#include "BotDiagnostics.h"
#include "BotActionLog.h"
#include "playerbot/PlayerbotFactory.h"
#include "PlayerbotSecurity.h"
#include "Group/Group.h"
#include "Objects/Pet.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "PlayerbotDbStore.h"
#include "strategy/values/PositionValue.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TravelMgr.h"
#include "Movement/spline/MoveSplineInitArgs.h"
#include "Maps/InstanceData.h"
#include "ChatHelper.h"
#include "strategy/values/BudgetValues.h"
#include "SocialMgr.h"
#include "PlayerbotTextMgr.h"
#include "RandomItemMgr.h"
#include "strategy/ItemVisitors.h"
#include "strategy/values/LootValues.h"
#include "strategy/values/AttackersValue.h"
#include "strategy/values/PossibleTargetsValue.h"
#include "Transports/Transport.h"
#include "Guild/GuildMgr.h"
#include "Chat/ChannelMgr.h"
#include "PlayerbotLLMInterface.h"

#include <boost/algorithm/string.hpp>
#include <cmath>

#ifdef MANGOSBOT_TWO
#include "Entities/Vehicle.h"
#endif

#ifdef BUILD_ELUNA
#include "LuaEngine/LuaEngine.h"
#endif
#include "AI/ScriptDevAI/ScriptDevAIMgr.h"
#include "strategy/values/GuildValues.h"

using namespace ai;

bool Playerbot_WsgNavigate(Player* bot);  // WSG graph waypoint navigator (BattleGroundTactics.cpp)



// DECISION-THRASH INSTRUMENTATION (phase 1, baseline-before-fix). Global, lock-free counters
// incremented from PlayerbotAI::CastSpell (runs on bot map-update worker threads). The fleet
// diagnostic (RandomPlayerbotMgr bot_fleet.csv) externs these and emits per-minute rates +
// percentages on a THRASH line so we can PROVE the commitment layer reduces self-interruption.
//   g_botCasts            - total spell-cast attempts that reached the cast point
//   g_botCastInterrupts   - casts started while a DIFFERENT generic cast was still in progress
//                           (the bot abandoning its own cast = "constantly interrupting")
//   g_botHealCasts        - heal-spell casts (denominator for recast rate)
//   g_botHealRecasts      - same heal re-cast within 4s (the "recasts its own heal" symptom)
std::atomic<uint64_t> g_botCasts{0};
std::atomic<uint64_t> g_botCastInterrupts{0};
std::atomic<uint64_t> g_botHealCasts{0};
std::atomic<uint64_t> g_botHealRecasts{0};
// Disambiguator: a heal re-cast FASTER than a heal could possibly complete (< ~1.8s, less than
// a typical heal cast time) means the previous heal was CANCELED, not completed -> the recast is
// interruption. A high tight_pct => cancellation (chase/movement mid-cast); a low tight_pct with
// high recast_pct => sustained healing / weak heals / losing fights (heal lands, HP still low).
std::atomic<uint64_t> g_botHealRecastTight{0};
// TARGET-CHURN instrumentation: how often the combat target is torn down + re-picked, and how
// often that happens while the OLD target was still ALIVE (= switching off a live mob = potential
// oscillation/thrash, vs. healthy retarget-after-kill). Incremented in SelectNewTargetAction.
std::atomic<uint64_t> g_botSelectNewTarget{0};
std::atomic<uint64_t> g_botSelectNewTargetAlive{0};

// ===== BOT-CAM: my "eyes". A COMPLETE per-second snapshot of one bot so I can see exactly what it
// is doing every moment, like watching the screen: position, hp/mana, in-combat, moving/casting,
// what it's targeting (+ that target's hp / distance / whether it's hitting the bot back),
// how many things are attacking it, what it's auto-attacking, how many corpses it has left
// unlooted, and the actual action it just executed. One row per bot per second -> logs/botcam.csv.
// Gated by EnableActionLog so it's only on for the small watched fleet. This is the ground truth
// the learning loop reads (and that I read to ask: what is it doing, why, what SHOULD it do).
static void WriteBotCam(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld())
        return;
    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx)
        return;

    static std::mutex camMx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 nowMs = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(camMx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && nowMs - t < 1000)
            return;               // throttle: 1 row / bot / second
        t = nowMs;
    }

    Unit* tgt = ctx->GetValue<Unit*>("current target")->Get();
    Unit* victim = bot->GetVictim();
    uint8 atkN = ctx->GetValue<uint8>("attackers count")->Get();

    std::string castName = "-";
    if (Spell* s = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (s->m_spellInfo)
            castName = s->m_spellInfo->SpellName[0];

    int lootN = 0;
    if (LootObjectStack* ls = ctx->GetValue<LootObjectStack*>("available loot")->Get())
        lootN = ls->CanLoot(sPlayerbotAIConfig.lootDistance) ? 1 : 0;   // 1 = has a corpse to loot in range

    const Action* la = ai->GetLastExecutedAction(ai->GetState());
    std::string doing = la ? const_cast<Action*>(la)->getName() : "-";

    const uint32 maxMana = bot->GetMaxPower(POWER_MANA);
    const int manaPct = maxMana ? (int)(bot->GetPower(POWER_MANA) * 100 / maxMana) : -1;

    FILE* f = fopen("logs/botcam.csv", "a");
    if (!f) f = fopen("../logs/botcam.csv", "a");
    if (!f)
        return;
    fprintf(f,
        "%s,%s,L%u,m%u,%.0f|%.0f,hp%u,mana%d,combat%d,move%d,cast=%s,tgt=%s,tgtHp%d,tgtDist%.0f,onMe%d,atk%u,auto=%s,loot%d,do=%s\n",
        sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), bot->GetLevel(), bot->GetMapId(),
        bot->GetPositionX(), bot->GetPositionY(),
        (uint32)bot->GetHealthPercent(), manaPct,
        bot->IsInCombat() ? 1 : 0,
        (sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0,
        castName.c_str(),
        tgt ? tgt->GetName() : "-",
        tgt ? (int)tgt->GetHealthPercent() : -1,
        tgt ? (double)bot->GetDistance(tgt) : -1.0,
        (tgt && tgt->GetVictim() == bot) ? 1 : 0,
        (uint32)atkN,
        victim ? victim->GetName() : "-",
        lootN,
        doing.c_str());
    fclose(f);
}

// UNREACHABLE-LOOT GIVE-UP. With the metric stall-rescues disabled (ENABLE_METRIC_RESCUES=false),
// a bot that targets a loot corpse it can't path to (e.g. behind the Brackwell pumpkin-patch fence)
// runs IN PLACE against the obstacle indefinitely -- watched Cariani do "move to loot" for 55s with
// her position frozen. The old !movementProgress stall checks miss this because she wiggles
// >0.03yd/tick, so per-tick movement reads as progress. Track NET displacement over a window: if a
// bot has barely moved while chasing the SAME corpse, it's unreachable -> caller defers it so the bot
// abandons it and gets back to productive work. File-static per-bot state (watched-fleet scale).
static bool LootPursuitStuck(Player* bot, ObjectGuid lootGuid)
{
    struct St { float x; float y; uint32 sinceMs; uint64 g; };
    constexpr uint32 WINDOW_MS = 7000;   // must net-progress within 7s...
    constexpr float MIN_DISP = 6.0f;     // ...by at least 6 yards
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const uint64 g = lootGuid.GetRawValue();
    std::lock_guard<std::mutex> lk(mx);
    St& s = track[bot->GetGUIDLow()];
    if (s.g != g || !s.sinceMs)
    {
        s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.sinceMs = now; s.g = g;
        return false;
    }
    const float dx = bot->GetPositionX() - s.x, dy = bot->GetPositionY() - s.y;
    if (dx * dx + dy * dy >= MIN_DISP * MIN_DISP)   // made real progress -> slide window forward
    {
        s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.sinceMs = now;
        return false;
    }
    return (now - s.sinceMs) >= WINDOW_MS;          // barely moved the whole window -> stuck
}

// Z-SANITY CORRECTION (band-aid, NOT a physics engine). Bots sometimes end up UNDER the terrain --
// e.g. resurrected onto a tree/WMO model below ground -- and, because they don't simulate gravity and
// the navmesh has gaps, the bad Z just persists. WoW collision is VMAP/MMAP data, not runtime physics;
// the proper fix is regenerating/repairing that data, but this safety net removes the visible bug.
// SAFETY: only act when the bot is BOTH underground (existing purpose-built WorldPosition::isUnderground)
// AND frozen there for several seconds. A bot legitimately in a cave/mine is moving/questing; a bot
// bugged under the terrain is stuck -- so the freeze gate avoids ripping bots out of legit indoor areas.
// Snaps up to the terrain heightmap (no models, so it clears the tree it's stuck under) and LOGS every
// correction to logs/botz_fix.csv so we can confirm it only catches real bugs. Gated to the watched
// fleet for now (perf + validation) via the caller.
static void CorrectBotZ(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap())
        return;
    if (bot->IsTaxiFlying() || bot->IsFlying() || bot->IsBeingTeleported() || bot->IsFalling() ||
        bot->IsInWater() || bot->HasMovementFlag(MOVEFLAG_SWIMMING) || bot->GetTransport())
        return;

    struct ZTrack { float x; float y; uint32 sinceMs; };
    static std::mutex mx;
    static std::unordered_map<uint32, ZTrack> track;
    static std::unordered_map<uint32, uint32> lastCheck;

    const uint32 now = WorldTimer::getMSTime();
    const float x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();
    const uint32 key = bot->GetGUIDLow();

    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& lc = lastCheck[key];
        if (lc && now - lc < 1000) return;   // ~1 check / sec / bot
        lc = now;
    }

    // NOTE: the FLOATING case ("bots hover slightly above ground when idle") is now fixed at its ROOT
    // in Unit::UpdateSplineMovement -- a bot is settled onto the walkable surface the moment its
    // movement spline ends (no client => no gravity, so the navmesh endpoint's +0.5f pad used to
    // persist forever). This function only handles UNDERGROUND, a genuinely different bug: navmesh /
    // collision DATA holes let a bot path BELOW the terrain, where no arrival-settle can help.
    if (!WorldPosition(bot).isUnderground())
    {
        std::lock_guard<std::mutex> lk(mx);
        track[key] = { x, y, now };           // reset the freeze window whenever we're above ground
        return;
    }

    bool frozenUnderground = false;
    {
        std::lock_guard<std::mutex> lk(mx);
        ZTrack& t = track[key];
        if (!t.sinceMs) t = { x, y, now };
        const float dx = x - t.x, dy = y - t.y;
        if (dx * dx + dy * dy >= 3.0f * 3.0f) t = { x, y, now };           // moved -> reset window
        else if (now - t.sinceMs >= 4000) frozenUnderground = true;       // stuck underground >=4s
    }
    if (!frozenUnderground)
        return;

    // terrain heightmap surface above us (vmap=false so models/the tree are ignored)
    const float terrainZ = bot->GetMap()->GetHeight(x, y, z + 60.0f, false, 120.0f);
    if (terrainZ <= INVALID_HEIGHT || terrainZ - z < 1.0f)
        return;

    bot->NearTeleportTo(x, y, terrainZ + 0.5f, bot->GetOrientation());
    { std::lock_guard<std::mutex> lk(mx); track[key] = { x, y, now }; }

    FILE* f = fopen("logs/botz_fix.csv", "a");
    if (!f) f = fopen("../logs/botz_fix.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,from_z=%.1f,to_z=%.1f,map=%u,%.0f|%.0f\n",
            sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), z, terrainZ + 0.5f,
            bot->GetMapId(), x, y);
        fclose(f);
    }
}

// PHYSICAL UNSTUCK (runtime; navmesh/mmap regen is off-limits -- it crashes the host). Bots regularly
// end up in spots where pathfinding fails for EVERYTHING: they can't travel (freeze), flee (then die),
// reach a combat target (idle in combat), or run to their corpse (long DEAD). All the same root. When
// a bot is genuinely FROZEN -- net displacement <8y for >12s -- while ALIVE, out of combat, and not
// doing a legit stationary action (cast / loot / sit-to-eat), dislodge it: expire its thrashing
// travel/rpg targets and NearTeleportTo a nearby valid ground point (the same crash-safe call
// CorrectBotZ uses). Rotates escape direction across nudges to get out of corners. Every nudge is
// logged to logs/freeze_fix.csv (ALWAYS-ON, unlike the hasLog-gated events) so we can PROVE it fires.
static void FreezeNudge(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->GetMap() || !bot->IsAlive())
        return;
    if (bot->IsInCombat() || !bot->getAttackers().empty())
        return;                                               // fighting legitimately holds position
    if (bot->IsTaxiFlying() || bot->IsBeingTeleported() || bot->IsFalling() || bot->GetTransport())
        return;
    if (bot->IsNonMeleeSpellCasted(true, false, true) || !bot->IsStandState())
        return;                                               // mid-cast or sitting (eat/drink) = ok
    if (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING) || !ai->CanMove())
        return;
    // IMMERSION: NEVER teleport where a real player can see it -- a blinking bot is unmistakably a bot,
    // which defeats the whole goal. Near a player a stuck bot instead relies on the natural walking
    // paths (anti-idle roam / flee) or just stands (reads as a player pausing). Teleport-unstick is
    // only for the far fleet nobody is watching.
    if (ai->HasPlayerNearby())
        return;

    struct St { float x; float y; uint32 sinceMs; uint32 lastNudgeMs; uint8 dir; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const float x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();

    uint8 dir;
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        if (!s.sinceMs) { s.x = x; s.y = y; s.sinceMs = now; return; }
        const float dx = x - s.x, dy = y - s.y;
        if (dx * dx + dy * dy >= 8.0f * 8.0f) { s.x = x; s.y = y; s.sinceMs = now; return; }  // moving fine
        if (now - s.sinceMs < 12000) return;                       // not frozen long enough yet
        if (s.lastNudgeMs && now - s.lastNudgeMs < 5000) return;   // don't spam nudges
        s.dir = (s.dir + 3) & 7;                                   // rotate escape direction each nudge
        dir = s.dir;
        s.lastNudgeMs = now; s.sinceMs = now; s.x = x; s.y = y;    // tentatively reset (avoid re-entry)
    }

    // find a nearby VALID walkable point (vmap surface, not a cliff/hole) to hop onto
    Map* map = bot->GetMap();
    float bestX = 0, bestY = 0, bestZ = 0; bool found = false;
    for (uint8 k = 0; k < 8 && !found; ++k)
    {
        const float ang = (((dir + k) & 7)) * (M_PI_F / 4.0f);
        const float nx = x + cosf(ang) * 12.0f, ny = y + sinf(ang) * 12.0f;
        const float nz = map->GetHeight(nx, ny, z + 5.0f, true);
        // 4y (was 8): an 8y climb at 12y range accepted building eaves/roofs as nudge landings
        // (bot observed stranded on the Goldshire inn roof -- unhuman + no navmesh off it).
        if (nz > INVALID_HEIGHT && fabs(nz - z) < 4.0f)
        { bestX = nx; bestY = ny; bestZ = nz; found = true; }
    }
    if (!found)
        return;

    if (AiObjectContext* ctx = ai->GetAiObjectContext())
    {
        if (TravelTarget* tt = ctx->GetValue<TravelTarget*>("travel target")->Get())
            tt->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        ctx->GetValue<bool>("travel target active")->Reset();
        ctx->GetValue<GuidPosition>("rpg target")->Reset();
    }

    bot->NearTeleportTo(bestX, bestY, bestZ + 0.5f, bot->GetOrientation());
    { std::lock_guard<std::mutex> lk(mx); St& s = track[bot->GetGUIDLow()]; s.x = bestX; s.y = bestY; s.sinceMs = now; }

    FILE* f = fopen("logs/freeze_fix.csv", "a");
    if (!f) f = fopen("../logs/freeze_fix.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,from=%.0f|%.0f,to=%.0f|%.0f\n",
            sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), bot->GetMapId(), x, y, bestX, bestY);
        fclose(f);
    }
}

// Is a REAL (human) player within `range` of this bot? Ignores the virtual-observer sentinel.
// Map-thread safe (reads the published snapshot). Used to NEVER teleport/relocate a bot a human
// can see (Goal 3: no visible teleporting).
static bool RealPlayerWithin(Player* bot, float range)
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

// Is this spot free of lava/slime/deep water? Used to stop loiterers pacing into hazards.
static bool SpotDry(uint32 mapId, float x, float y, float z)
{
    const TerrainInfo* t = sTerrainMgr.LoadTerrain(mapId);
    if (!t)
        return true;
    GridMapLiquidData d;
    GridMapLiquidStatus s = t->getLiquidStatus(x, y, z, MAP_ALL_LIQUIDS, &d);
    if (s == LIQUID_MAP_NO_WATER)
        return true;
    if (d.type_flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME))
        return false;                       // lava / slime
    return !(s & LIQUID_MAP_UNDER_WATER);   // deep water = would swim
}

// CITY LIFE (Goal 0: "if a player is in the city the bots need to be pretending they are doing
// activities appropriate for the city"). PopulateCapitalCities sits ~1800 high-level bots in the
// capitals to make the world feel populated -- but they stand frozen. When one goes idle, give it
// visible city life: people-watch (turn to face a new direction), one-shot social emotes, and SHORT
// reachable strolls. NO long-range roam -- the dense city navmesh rejects far random points (that's
// why "attack anything"/"move random" no-op in cities); short pathfinding hops fail safe. Throttled
// ~9s/bot. Logged to antiidle.csv (what=city*) so its effect is measurable.
static void CityResidentActivity(PlayerbotAI* ai, Player* bot)
{
    static std::mutex mx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && now - t < 9000)
            return;
        t = now;
    }

    uint32 h = bot->GetGUIDLow() * 2654435761u + (now / 9000);
    h ^= h >> 13;
    const uint32 roll = h % 100;
    const float ang = (float)(h % 628) / 100.0f;   // 0..2pi

    const char* what;
    if (roll < 35)
    {
        bot->SetFacingTo(ang);                       // people-watch
        what = "citylook";
    }
    else if (roll < 70)
    {
        static const uint32 emotes[] = {
            EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_CHEER,
            EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_APPLAUD, EMOTE_ONESHOT_POINT };
        bot->HandleEmoteCommand(emotes[h % 6]);      // social emote
        what = "cityemote";
    }
    else
    {
        const float dist = 8.0f + (float)(h % 14);   // short reachable stroll (8-22y)
        float x = bot->GetPositionX() + cos(ang) * dist;
        float y = bot->GetPositionY() + sin(ang) * dist;
        float z = bot->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        if (MaNGOS::IsValidMapCoord(x, y, z) && ai->CanMove() && bot->IsStandState())
            bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE, 0.0f);
        what = "citystroll";
    }

    FILE* f = fopen("logs/antiidle.csv", "a");
    if (!f) f = fopen("../logs/antiidle.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,%.0f|%.0f,%s,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), what);
        fclose(f);
    }
}

// DUNGEON/RAID ENTRANCE LOITER behavior (paired with RandomPlayerbotMgr::PopulateDungeonEntrances).
// A bot assigned the loiterer role holds its entrance and acts like it's forming a group: paces,
// social-emotes, looks around. ~1 in 6 are "zone-in" actors -- they walk onto the portal and then
// vanish (recycled to a grind spot => reads as "entered the dungeon"), freeing the slot so a DIFFERENT
// bot rotates in. Runs INSTEAD of grind/travel AI (loiterers hold their post). Dormant far-away
// loiterers just stand frozen at the portal until a player approaches -- the illusion only needs to
// work when seen, so this stays cheap.
static void DungeonLoiterController(PlayerbotAI* ai, Player* bot)
{
    uint32 mapId; float ex, ey, ez; bool zoneIn;
    if (!sRandomPlayerbotMgr.GetDungeonLoiterSpot(bot->GetGUIDLow(), mapId, ex, ey, ez, zoneIn))
        return;
    if (!bot->IsAlive())
        return;
    // Already zoned into the instance -> dwell (afk/dormant) a couple minutes, then leave so
    // instances don't accumulate occupants over time.
    if (bot->GetMapId() != mapId)
    {
        if (sRandomPlayerbotMgr.LoitererDwellExpired(bot->GetGUIDLow()))
        {
            sRandomPlayerbotMgr.EndDungeonRole(bot->GetGUIDLow());
            // leave the instance -- via the world-thread queue (never teleport from a map tick)
            sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0, 0, 0, true);
        }
        return;
    }
    // FIGHT hostiles around the entrance: proactively pull anything hostile nearby so the crowd
    // defends its spot / clears the area instead of standing while mobs are around. (A loiterer
    // ALREADY in combat never reaches here -- the outer hook falls it through to the full combat AI.)
    if (ai->DoSpecificAction("attack anything", Event(), true))
        return;

    static std::mutex mx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && now - t < 4000) return;
        t = now;
    }
    if (bot->IsNonMeleeSpellCasted(true, false, true) || sServerFacade.isMoving(bot))
        return;                                  // let an in-flight move/cast finish
    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    if (!ai->CanMove())
    {
        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
        return;
    }

    const float dx = bot->GetPositionX() - ex, dy = bot->GetPositionY() - ey;
    const float dist2 = dx * dx + dy * dy;
    uint32 h = bot->GetGUIDLow() * 2654435761u + now / 4000; h ^= h >> 13;
    const float ang = (float)(h % 628) / 100.0f;

    if (dist2 > 45.0f * 45.0f)                   // wandered off -> return to the entrance crowd
    {
        bot->GetMotionMaster()->MovePoint(mapId, ex + cos(ang) * (h % 20), ey + sin(ang) * (h % 20), ez,
            MOVE_PATHFINDING | MOVE_RUN_MODE, 0.0f);
        return;
    }

    if (zoneIn && dist2 < 10.0f * 10.0f && (h % 3 == 0))   // reached the portal -> actually zone IN
    {
        uint32 dm; float dxp, dyp, dzp;
        if (sRandomPlayerbotMgr.GetDungeonZoneInDest(bot->GetGUIDLow(), dm, dxp, dyp, dzp))
        {
            sRandomPlayerbotMgr.MarkLoitererZonedIn(bot->GetGUIDLow());
            // into the instance -> dwell; via world-thread queue (never teleport from a map tick)
            sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), dm, dxp, dyp, dzp, false);
        }
        return;
    }

    const uint32 roll = h % 100;
    if (zoneIn && roll < 55)                      // zone-in actor edges toward the portal
    {
        bot->GetMotionMaster()->MovePoint(mapId, ex, ey, ez, MOVE_PATHFINDING | MOVE_RUN_MODE, 0.0f);
    }
    else if (roll < 45)                           // pace around the entrance (waiting for the group)
    {
        const float rr = 8.0f + (float)(h % 12);
        float x = ex + cos(ang) * rr, y = ey + sin(ang) * rr, z = ez;
        bot->UpdateAllowedPositionZ(x, y, z);
        if (MaNGOS::IsValidMapCoord(x, y, z) && SpotDry(mapId, x, y, z))   // never pace into lava/water
            bot->GetMotionMaster()->MovePoint(mapId, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE, 0.0f);
    }
    else if (roll < 80)                           // "LFG! anyone?" social emotes
    {
        static const uint32 em[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_WAVE,
                                     EMOTE_ONESHOT_ROAR, EMOTE_ONESHOT_APPLAUD, EMOTE_ONESHOT_CHEER };
        bot->HandleEmoteCommand(em[h % 6]);
    }
    else
    {
        bot->SetFacingTo(ang);                    // look around for members
    }
}

// ANTI-IDLE: the dominant remaining waste is bots STANDING with no active travel goal (travelStatus
// COOLDOWN/EXPIRED, 95 of ~142 idle bots) -- they loop "choose travel target"/"request quest travel"
// and never actually move or fight. When a bot has been genuinely idle (net displacement <3y for >5s)
// while alive, out of combat, and NOT doing a legit stationary thing (cast/loot/eat), force it to be
// productive: engage the nearest grind mob ("attack anything"), or if none is claimable, roam ("move
// random") so it travels toward content instead of standing. This converts IDLE seconds into
// MOVE/MELEE/CAST -- the machine-gun-activity goal. Logged to logs/antiidle.csv (always-on) with what
// it did, so its effect is measurable and it can't silently inflate anything.
static void AntiIdleAction(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive())
        return;
    if (bot->IsInCombat() || !bot->getAttackers().empty())
        return;
    if (bot->IsTaxiFlying() || bot->IsBeingTeleported() || bot->IsFalling() || bot->GetTransport())
        return;
    if (bot->IsNonMeleeSpellCasted(true, false, true) || !bot->IsStandState())
        return;                                    // casting or sitting (eat/drink) is fine
    if (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING) || !ai->CanMove())
        return;
    // Never poke a bot that is ACTIVELY MOVING. The net-displacement gate below (<3y over 5s) is
    // satisfied by a ZIGZAGGING bot, so anti-idle kept injecting "move random" into an already
    // contested movement decision -- one of the owners behind the watched forward-back-forward
    // ping-pong (Dreligs: 12 direction reversals/min alternating rpg/random/travel moves).
    if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
        return;
    // Don't fight the engine when it HAS a live travel goal it's actively working. (Forcing grind/roam
    // on bots stuck FAR from an objective was tried and reverted -- it pulled unwinnable fights and
    // raised DEAD. Bots stuck en route to an unreachable objective are handled by the travel-freeze
    // breaker / FreezeNudge instead.)
    if (AiObjectContext* ctx = ai->GetAiObjectContext())
    {
        if (TravelTarget* tt = ctx->GetValue<TravelTarget*>("travel target")->Get())
        {
            const TravelStatus st = tt->GetStatus();
            if (st == TravelStatus::TRAVEL_STATUS_TRAVEL || st == TravelStatus::TRAVEL_STATUS_READY ||
                st == TravelStatus::TRAVEL_STATUS_WORK)
                return;
        }
    }

    struct St { float x; float y; uint32 sinceMs; uint32 lastActMs; uint8 dir; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const float x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();

    uint8 dir;
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        if (!s.sinceMs) { s.x = x; s.y = y; s.sinceMs = now; return; }
        const float dx = x - s.x, dy = y - s.y;
        if (dx * dx + dy * dy >= 3.0f * 3.0f) { s.x = x; s.y = y; s.sinceMs = now; return; }   // moving
        if (now - s.sinceMs < 5000) return;                        // not idle long enough
        if (s.lastActMs && now - s.lastActMs < 3000) return;       // throttle forced actions
        s.dir = (s.dir + 3) & 7; dir = s.dir;
        s.lastActMs = now; s.sinceMs = now; s.x = x; s.y = y;
    }

    // grind the nearest claimable mob; if none, roam toward content via the normal (pathfinding)
    // wander. NOTE: a straight-line WALK fallback for stuck bots was tried and REVERTED -- ignoring
    // collision walked them into mobs/terrain and raised DEAD 15%->18% for no idle/move gain. Stuck
    // bots the pathfinder can't move are left to FreezeNudge (far fleet only) or to stand near players.
    (void)dir;
    // City residents that have gone idle do city LIFE (people-watch / emote / short stroll) instead
    // of the grind/roam fallback -- there are no hostile mobs to attack in a capital and long roams
    // fail on the dense city navmesh, so attack/roam just no-op (acted0). Keeps the capitals visibly
    // alive when a player walks through (Goal 0). Bots actively traveling/questing never reach here
    // (they return at the travel-goal / isMoving gates above).
    if (sRandomPlayerbotMgr.IsCityResident(bot->GetGUIDLow()))
    {
        CityResidentActivity(ai, bot);
        return;
    }

    // ===== NEVER-IDLE EXECUTOR (the "actor" layer) ==========================================
    // An actor never has a "nothing" state. A genuinely-idle bot here means the decision engine
    // resolved to no executable action (chosen travel dest unroutable / no mob in reach / cooling
    // down). Guarantee a visible activity, in priority order, so idle -> productive every time:
    //   1) engage the nearest reachable mob (mobs sit on walkable ground -> attack anything works);
    //   2) if NO human can see it -> teleport-RESCUE to a fresh level-appropriate mob camp (the
    //      never-fail guarantee: it will be standing in content next tick). Throttled to a real
    //      "stranded bot" rescue, and NEVER when a human is in range (Goal 3: no visible teleport);
    //   3) on-screen with nothing to fight -> commit to walking to its travel goal / stroll, and as
    //      an absolute last resort face around (never a frozen statue).
    const char* what = "attack";
    bool acted = ai->DoSpecificAction("attack anything", Event(), true);   // (1)

    if (!acted)
    {
        const float rr = sPlayerbotAIConfig.activeRenderRange > 0.0f ? sPlayerbotAIConfig.activeRenderRange : 170.0f;
        const bool humanCanSee = RealPlayerWithin(bot, rr + 80.0f);        // margin past render range
        if (!humanCanSee && sRandomPlayerbotMgr.IsRandomBot(bot) && !ai->HasRealPlayerMaster()
            && bot->IsAlive() && !bot->IsInCombat() && !bot->IsTaxiFlying()
            && !bot->IsBeingTeleported() && !bot->InBattleGround() && !bot->InBattleGroundQueue())
        {
            static std::mutex tmx;
            static std::unordered_map<uint32, uint32> lastTele;
            const uint32 nowT = WorldTimer::getMSTime();
            bool doTele = false;
            {
                std::lock_guard<std::mutex> lk(tmx);
                uint32& t = lastTele[bot->GetGUIDLow()];
                if (!t || nowT - t > 45000) { t = nowT; doTele = true; }   // >=45s between rescues
            }
            if (doTele)
            {
                // (2) -> mob-dense camp, via the world-thread queue (never teleport from a map tick)
                sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0, 0, 0, true);
                acted = true; what = "rescue";
            }
        }
    }

    if (!acted) { acted = ai->DoSpecificAction("move to travel target", Event(), true); if (acted) what = "gotarget"; }  // (3)
    if (!acted) { acted = ai->DoSpecificAction("move random", Event(), true); if (acted) what = "roam"; }
    if (!acted) { bot->SetFacingTo((float)((bot->GetGUIDLow() + WorldTimer::getMSTime() / 1000) % 628) / 100.0f); acted = true; what = "face"; }

    FILE* f = fopen("logs/antiidle.csv", "a");
    if (!f) f = fopen("../logs/antiidle.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,%.0f|%.0f,%s,acted%d\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), x, y, what, acted ? 1 : 0);
        fclose(f);
    }
}

// HOPELESS-FIGHT BREAKER. A bot that fights the SAME victim for 60s+ WITHOUT denting its health is in
// an unwinnable engagement -- watched Rogerolan melee an Apprentice Training Dummy (1.1M HP, level 1,
// passes every filter) for 20+ minutes with zero XP; also catches evade-mode / unhittable mobs. A real
// player gives up in seconds. Track (victim, firstHP) per bot; if after 60s the victim is still at
// >=97% of the HP it had when we started watching, drop the target and walk away. The grind filter
// (GrindTargetValue: >40x HP pool reject) prevents instant re-pick of dummies.
static void HopelessFightBreaker(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat())
        return;
    Unit* victim = bot->GetVictim();
    if (!victim || !victim->IsAlive())
        return;
    if (!bot->getAttackers().empty() && bot->getAttackers().count(victim))
        return;                                   // it's actually fighting BACK -> not a dummy-type sink

    struct St { uint64 g; uint32 sinceMs; uint32 firstHp; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const uint64 g = victim->GetObjectGuid().GetRawValue();

    bool hopeless = false;
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        if (s.g != g || !s.sinceMs)
        {
            s.g = g; s.sinceMs = now; s.firstHp = victim->GetHealth();
            return;
        }
        if (victim->GetHealth() < s.firstHp * 0.97f)
        {
            s.sinceMs = now; s.firstHp = victim->GetHealth();   // making progress -> slide window
            return;
        }
        if (now - s.sinceMs >= 60000)
        {
            hopeless = true;
            s.g = 0; s.sinceMs = 0;                             // reset so we don't re-fire instantly
        }
    }
    if (!hopeless)
        return;

    bot->AttackStop();
    bot->InterruptNonMeleeSpells(false);
    if (AiObjectContext* ctx = ai->GetAiObjectContext())
    {
        ctx->GetValue<Unit*>("current target")->Set(nullptr);
        ctx->GetValue<ObjectGuid>("attack target")->Set(ObjectGuid());
    }
    ai->StopMoving();

    FILE* f = fopen("logs/antiidle.csv", "a");
    if (!f) f = fopen("../logs/antiidle.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,%.0f|%.0f,giveup:%s,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), victim->GetName());
        fclose(f);
    }
}

// AUTO-AMMO. Same silent-degradation family as durability: the factory stocks ammo at creation and
// NOTHING refills it, so hunters/throwers/gunners shoot dry and are stuck as weak melee forever
// (measured: 6 hunters failing "concussive shot", Hurtian failing "throw", a gunner failing "shoot
// gun" -- all SPELL_FAILED_EQUIPPED_ITEM_CLASS = no ammo). Ammo is invisible to other players;
// PlayerbotFactory::InitAmmo is idempotent (tops up only when low), so just call it periodically.
static void AutoAmmo(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return;
    static std::mutex mx;
    static std::unordered_map<uint32, uint32> last;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = last[bot->GetGUIDLow()];
        if (t && now - t < 180000) return;      // every 3 min
        t = now;
    }
    if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        return;
    const uint32 beforeEntry = bot->GetUInt32Value(PLAYER_AMMO_ID);
    const uint32 beforeCount = beforeEntry ? bot->GetItemCount(beforeEntry) : 0;
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitAmmo();
    const uint32 afterEntry = bot->GetUInt32Value(PLAYER_AMMO_ID);
    const uint32 afterCount = afterEntry ? bot->GetItemCount(afterEntry) : 0;
    if (afterCount > beforeCount || afterEntry != beforeEntry)
    {
        FILE* f = fopen("logs/antiidle.csv", "a");
        if (!f) f = fopen("../logs/antiidle.csv", "a");
        if (f)
        {
            fprintf(f, "%s,%s,map%u,%.0f|%.0f,ammo:%u->%u,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
                bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), beforeCount, afterCount);
            fclose(f);
        }
    }
}

// QUEST-LOOP BREAKER (kill-the-same-mob-forever). At contested camps (Princess), the quest DROP is
// often unobtainable in practice (crowd taps, group combat never clears) -- the router then re-sends
// the bot at the same mob forever: watched bots kill Princess 20+ times each with zero collars ever
// obtained. A real player gives up on a camped objective and does other quests. If a bot kills the
// SAME creature entry 6+ times within 20 minutes while its travel objective points at that entry,
// blacklist the objective for 2 hours and expire the travel target so it re-plans elsewhere.
static uint32 QuestProgressChecksum(Player* bot)
{
    // Sum of all kill-credit and item-collection progress across the quest log. If this number moved,
    // the bot IS progressing and must not be loop-broken.
    uint32 sum = 0;
    for (auto const& qs : bot->getQuestStatusMap())
    {
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            sum += qs.second.m_creatureOrGOcount[i];
            sum += qs.second.m_itemcount[i];
        }
    }
    return sum;
}

static void QuestLoopBreaker(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive())
        return;
    struct St { uint32 entry; uint32 count; uint32 sinceMs; uint32 lastVictimEntry; uint8 prevCombat; uint32 progressSum; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const bool inCombat = bot->IsInCombat();

    uint32 loopedEntry = 0;
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        if (inCombat)
        {
            if (Unit* v = bot->GetVictim())
                if (v->GetObjectGuid().IsCreature())
                    s.lastVictimEntry = v->GetObjectGuid().GetEntry();
        }
        else if (s.prevCombat == 1 && s.lastVictimEntry)
        {
            // combat just ended; count a kill-ish cycle of that entry
            if (s.entry != s.lastVictimEntry || now - s.sinceMs > 1200000)
            { s.entry = s.lastVictimEntry; s.count = 0; s.sinceMs = now; s.progressSum = QuestProgressChecksum(bot); }
            if (++s.count >= 6)
            {
                // Only a LOOP if the quest log made NO progress across those kills. v1 fired on ANY
                // 6 kills of the router's entry -- booting bots off healthy kill-credit camps
                // (Stonetusk Boar!) and cratering XP. Kill quests advance m_creatureOrGOcount and
                // collect quests advance m_itemcount; if either moved, this camp is WORKING.
                const uint32 nowSum = QuestProgressChecksum(bot);
                if (nowSum == s.progressSum)
                    loopedEntry = s.entry;
                s.count = 0; s.sinceMs = now; s.progressSum = nowSum;
            }
            s.lastVictimEntry = 0;
        }
        s.prevCombat = inCombat ? 1 : 0;
    }
    if (!loopedEntry)
        return;

    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx) return;
    TravelTarget* tt = ctx->GetValue<TravelTarget*>("travel target")->Get();
    if (!tt || !tt->GetDestination() || tt->GetEntry() != (int32)loopedEntry)
        return;                                   // only break loops the ROUTER is driving
    // ...and ONLY for QUEST-objective destinations. v2 still misfired on GRIND destinations: a bot
    // grinding boars for XP has zero quest-log progress by definition, so the checksum gate passed
    // and healthy grind camps got blacklisted. Grinding is self-justifying (XP); only a quest
    // objective that yields neither progress nor its drop across 6 kills is a genuine dead loop.
    if (!dynamic_cast<QuestTravelDestination*>(tt->GetDestination()))
        return;

    // OBSERVE-ONLY (v4): three iterations of in-session blacklisting (v1 any-kills, v2 zero-progress,
    // v3 quest-only) all misfired enough to depress fleet XP -- a single bot's 6-kill window is too
    // noisy a signal to act on live. The breaker now only RECORDS the verdict; the cross-run quest
    // manager (failure_count>=20 across many bots/runs -> boot-loaded travel penalty) makes the
    // actual skip decision with real statistics. In-session, the existing per-guid mechanisms
    // (travel-freeze breaker, foreign-loot defer) still bound the worst waste.

    // QUEST MANAGER MEMORY: persist the verdict so skips accumulate ACROSS runs. Each loop-break adds
    // 6 failures to ai_playerbot_objective_stats; at failure_count>=20 with learned_penalty>0 the
    // next boot loads it as a standing travel penalty (>=2.5 = suppressed destination) -- chronically
    // broken objectives (contested camps, unobtainable drops) get fleet-wide skipped without code.
    WorldDatabase.PExecute(
        "INSERT INTO ai_playerbot_objective_stats "
        "(objective_type, quest_id, entry, name, attempt_count, failure_count, learned_penalty, last_failure_reason, first_seen_at, last_seen_at) "
        "SELECT 'interaction', 0, %u, 'questloop', 0, 0, 0, '', NOW(), NOW() FROM DUAL "
        "WHERE NOT EXISTS (SELECT 1 FROM ai_playerbot_objective_stats WHERE objective_type='interaction' AND entry=%u AND name='questloop')",
        loopedEntry, loopedEntry);
    WorldDatabase.PExecute(
        "UPDATE ai_playerbot_objective_stats SET failure_count=failure_count+6, attempt_count=attempt_count+6, "
        "learned_penalty=GREATEST(learned_penalty, 3.0), pain_score=pain_score+1, "
        "last_failure_reason='quest-drop-unobtainable-loop', last_seen_at=NOW() "
        "WHERE objective_type='interaction' AND entry=%u AND name='questloop'",
        loopedEntry);

    FILE* f = fopen("logs/antiidle.csv", "a");
    if (!f) f = fopen("../logs/antiidle.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,%.0f|%.0f,questloop:entry=%u,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), loopedEntry);
        fclose(f);
    }
}

// LOOT UNDER (GROUP) FIRE. At contested camps, grouped bots share the group's combat state -- the
// combat engine has NO loot actions, so a follower who kills its quest mob stands next to the corpse
// "in combat" (because the group keeps fighting) until the loot target times out; quest drops are
// never collected (watched Seranon at the Princess camp: loot1 flag up for a minute, never opened).
// A real player grabs the loot between swings when nobody is hitting HIM. If the bot is in combat but
// has NO personal attackers, and a lootable corpse is within reach, poke the loot chain directly.
static void LootUnderFire(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive())
        return;
    if (!bot->IsInCombat() || !bot->getAttackers().empty())
        return;                                    // only the group-combat-but-personally-safe case
    if (bot->IsNonMeleeSpellCasted(true, false, true) || bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING))
        return;
    static std::mutex mx;
    static std::unordered_map<uint32, uint32> last;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = last[bot->GetGUIDLow()];
        if (t && now - t < 3000) return;
        t = now;
    }
    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx || !ctx->GetValue<bool>("has available loot")->Get())
        return;
    if (ai->DoSpecificAction("loot", Event(), true))
    {
        FILE* f = fopen("logs/antiidle.csv", "a");
        if (!f) f = fopen("../logs/antiidle.csv", "a");
        if (f)
        {
            fprintf(f, "%s,%s,map%u,%.0f|%.0f,lootfire,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
                bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
            fclose(f);
        }
    }
}

// GRAVITY SETTLE. Bots have no client, hence no gravity. Navmesh path points sit ABOVE true ground
// (Recast poly height + 0.5y pad), and while WALKING the bot's Z rides the spline between them --
// observers' clients smooth moving units so that looks fine. The float appears on STOP: natural
// spline completion settles (fixed earlier in Unit::UpdateSplineMovement), but bots overwhelmingly
// stop by INTERRUPT (StopMoving / motion-clear on "close enough" / re-decisions -- 56 callsites),
// which freezes the server Z at the last mid-spline interpolated height (up to ~1y high) and skips
// the arrival settle. This helper is the missing gravity: drop a stationary bot to the walkable
// surface under it (vmap height => bridges/WMO floors respected; water/flying/falling exempt).
// Called from StopMoving (instant, the dominant stop path) and the 1/sec supervisor sweep (catch-all).
static void SettleToGround(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || !bot->IsAlive())
        return;
    if (bot->IsTaxiFlying() || bot->IsFlying() || bot->IsBeingTeleported() || bot->IsFalling() ||
        bot->IsInWater() || bot->HasMovementFlag(MOVEFLAG_SWIMMING) || bot->GetTransport())
        return;
    const float x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();
    const float ground = bot->GetMap()->GetHeight(x, y, z + 0.5f, true);
    if (ground > INVALID_HEIGHT && z - ground > 0.05f && z - ground < 5.0f)
    {
        // NearTeleportTo, NOT raw Relocate: Relocate skips map/grid relocation bookkeeping and is a
        // suspect for the "Removing object which is already removed" SIGSEGV. NearTeleportTo does the
        // full safe path and a sub-yard Z drop renders as the bot simply standing grounded.
        bot->NearTeleportTo(x, y, ground, bot->GetOrientation());
    }
}

// NO-PROGRESS GRIND FALLBACK. The fleet MOVES 54% of the time (more than real players) but is in
// combat only ~13% (report target 30-47%): bots wander between distant quest objectives that mostly
// never complete (quest progression ~4%), earning ~1k XP/hr vs the 10-15k target. A real player who
// notices he's made no progress stops wandering and grinds where he stands. If a bot has gained ZERO
// xp for 10+ minutes while alive (and isn't currently fighting/eating), dump its travel target and
// force a grind engagement in the local area -- kills, loot, and chain-pulls all work now, so local
// farming is productive. XP gain resets the clock naturally.
static void NoProgressGrindFallback(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return;
    if (bot->IsTaxiFlying() || bot->IsBeingTeleported() || !bot->IsStandState())
        return;
    if (ai->HasRealPlayerMaster())
        return;

    struct St { uint32 lvl; uint32 xp; uint32 sinceMs; uint32 lastActMs; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        const uint32 lvl = bot->GetLevel(), xp = bot->GetUInt32Value(PLAYER_XP);
        if (!s.sinceMs || s.lvl != lvl || s.xp != xp)
        {
            s.lvl = lvl; s.xp = xp; s.sinceMs = now;
            return;
        }
        if (now - s.sinceMs < 600000) return;                    // <10 min without xp -> fine
        if (s.lastActMs && now - s.lastActMs < 30000) return;    // retry at most every 30s
        s.lastActMs = now;
    }

    if (AiObjectContext* ctx = ai->GetAiObjectContext())
    {
        if (TravelTarget* tt = ctx->GetValue<TravelTarget*>("travel target")->Get())
        {
            tt->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            tt->SetForced(false);
        }
        ctx->GetValue<bool>("travel target active")->Reset();
    }
    const bool acted = ai->DoSpecificAction("attack anything", Event(), true);

    FILE* f = fopen("logs/antiidle.csv", "a");
    if (!f) f = fopen("../logs/antiidle.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,%.0f|%.0f,noprogress-grind,acted%d\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), acted ? 1 : 0);
        fclose(f);
    }
}

// CHAIN-PULL. A real efficient player already KNOWS the next mob before the current one dies and
// transitions kill -> loot -> next pull in seconds; our bots instead re-ran the whole strategy churn
// (travel/rpg/choose-target) after every kill, leaving multi-second dead-air. On the combat->done
// transition, immediately poke "attack anything": its own isUseful() correctly declines when loot is
// pending (loot first) or HP is low (eat first, engage-gate), so this only fires when the right next
// action IS another pull -- making the decision instant instead of re-thought.
static void ChainPullNext(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive())
        return;
    struct St { uint8 prevCombat; uint32 endedMs; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const bool inCombat = bot->IsInCombat();
    const uint32 now = WorldTimer::getMSTime();
    bool readyToChain = false;
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        if (s.prevCombat == 1 && !inCombat)
            s.endedMs = now;                       // combat just ended -> start the loot grace window
        s.prevCombat = inCombat ? 1 : 0;
        if (inCombat)
            s.endedMs = 0;
        // 300ms confirmation covers the XpGain->available-loot packet race (packet handlers run
        // inside UpdateAIInternal AFTER this supervisor hook); beyond that, chain the INSTANT the
        // loot pipeline is idle instead of a fixed 2s grace (measured: the 2s floor + post-loot
        // re-scan was 3-4s of dead-air on every one of ~110 kills/bot/2h). Loot stays first: the
        // window holds open while anything is lootable, and bots without loot rights (group
        // round-robin losers, foreign taps) chain immediately.
        if (s.endedMs && now - s.endedMs >= 300)
        {
            if (now - s.endedMs > 15000)
                s.endedMs = 0;                     // safety cap: stop polling after 15s
            else
                readyToChain = true;               // provisional; loot check below may hold it
        }
    }
    if (!readyToChain || bot->GetHealthPercent() < 55.0f)
        return;

    AiObjectContext* chainCtx = ai->GetAiObjectContext();
    const bool lootPending = chainCtx &&
        (chainCtx->GetValue<bool>("has available loot")->Get() ||
         chainCtx->GetValue<bool>("can loot")->Get() ||
         !chainCtx->GetValue<LootObject>("loot target")->Get().IsEmpty() ||
         bot->GetLootGuid());
    if (lootPending)
        return;                                    // keep endedMs armed; re-check next tick
    {
        std::lock_guard<std::mutex> lk(mx);
        track[bot->GetGUIDLow()].endedMs = 0;      // consuming the chain now
    }

    if (ai->DoSpecificAction("attack anything", Event(), true))
    {
        FILE* f = fopen("logs/antiidle.csv", "a");
        if (!f) f = fopen("../logs/antiidle.csv", "a");
        if (f)
        {
            fprintf(f, "%s,%s,map%u,%.0f|%.0f,chain,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
                bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
            fclose(f);
        }
    }
}

// CHAIN-PULL PRE-SELECTION (goal #4): while the current victim is dying (<=25% HP), decide the
// NEXT grind target so the post-kill sequence is loot -> engage with zero re-think. Pure internal
// state: no selection packets, so pre-selection can never aggro anything early. The plan is
// consumed (and re-validated) by GrindTargetValue when "attack anything" next runs.
static void PreSelectNextTarget(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat())
        return;
    if (ai->HasRealPlayerMaster())
        return;
    Unit* victim = bot->GetVictim();
    if (!victim || victim->GetHealthPercent() > 25.0f)
        return;

    static std::mutex mx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && now - t < 1000)
            return;
        t = now;
    }

    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx)
        return;
    ObjectGuid existing = ctx->GetValue<ObjectGuid>("pre-selected next target")->Get();
    if (existing)
    {
        Unit* ex = ai->GetUnit(existing);
        if (ex && sServerFacade.IsAlive(ex))
            return;                                // stay firm: keep the existing plan
    }
    Unit* next = ctx->GetValue<Unit*>("next grind target")->Get();
    if (next && next != victim)
        ctx->GetValue<ObjectGuid>("pre-selected next target")->Set(next->GetObjectGuid());
}

// LFT MASTER CLEANUP. LFT-filled bots get a real-player master set directly (Playerbot_PrepareLfgBot).
// If that player later leaves the group / kicks the bot while staying online, the bot would keep the
// master link and follow them across the world forever. A random bot whose real-player master is no
// longer a group mate releases the link and returns to the fleet.
static void LftMasterCleanup(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld())
        return;
    static std::mutex mx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && now - t < 30000)
            return;
        t = now;
    }

    Player* master = ai->GetMaster();
    if (!master || master == bot || !master->isRealPlayer())
        return;
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    const bool sameGroup = bot->GetGroup() && master->GetGroup() == bot->GetGroup();
    if (sameGroup)
        return;

    if (bot->GetGroup())
        bot->GetGroup()->RemoveMember(bot->GetObjectGuid(), 0);
    ai->SetMaster(nullptr);
    ai->ResetStrategies();
    ai->Reset(true);

    // Send the fill HOME: back to its pre-dungeon spot (falls back to homebind if the origin is
    // unknown or the bot somehow originated inside an instance). Without this, released bots
    // idle in the emptied dungeon forever instead of resuming their open-world lives.
    extern bool Playerbot_GetLfgOrigin(uint32 guidLow, WorldLocation& out);
    if (bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid()))
    {
        WorldLocation origin;
        bool haveOrigin = Playerbot_GetLfgOrigin(bot->GetGUIDLow(), origin);
        if (haveOrigin && !sMapStore.LookupEntry(origin.mapid)->Instanceable())
            sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), origin.mapid, origin.coord_x, origin.coord_y, origin.coord_z, false, false);
        else
            sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0, 0, 0, true, false);
    }
    sLog.outDetail("LFT: released bot %s back to the fleet (master left group)", bot->GetName());
}

// INSTANCE STRAND RESCUE. During raid/dungeon runs with a real player, fill bots that end up
// OUTSIDE the instance (walked out the portal, wrong instance id, died at the entrance — user
// watched fills exit MC's portal and die in the lava) get revived and teleported to the master:
// with the master inside, the group bind resolves them into the CORRECT instance.
static void InstanceStrandRescue(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld())
        return;
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;
    Player* master = ai->GetMaster();
    if (!master || master == bot || !master->isRealPlayer() || !master->IsInWorld())
        return;
    if (!bot->GetGroup() || master->GetGroup() != bot->GetGroup())
        return;
    Map* mmap = master->GetMap();
    if (!mmap || (!mmap->IsDungeon() && !mmap->IsRaid()))
        return;
    // bot inside the same instance (map + instance id): fine
    if (bot->GetMapId() == master->GetMapId() && bot->GetInstanceId() == master->GetInstanceId())
        return;

    static std::mutex mx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && now - t < 15000)
            return;
        t = now;
    }

    sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), master->GetMapId(),
        master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(), false, !bot->IsAlive());
    sLog.outBasic("LFT rescue: %s queued into %s's instance (map %u)", bot->GetName(), master->GetName(), master->GetMapId());
}

// TRAVEL SCHEDULE RESCUE (the Erielah class): some bots hold an ACTIVE travel target while their
// travel Execute is never scheduled at all — no movement, no breaker (it lives inside Execute),
// zero TravelFrozenAbandoned events, forever. Detect it from the outside: an alive, out-of-combat
// bot with a live travel target that hasn't displaced 5y in 45s gets its target expired, which
// re-enters the (verifiably scheduled) choose/request planning path.
static void TravelScheduleRescue(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsTaxiFlying())
        return;
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx)
        return;
    TravelTarget* target = ctx->GetValue<TravelTarget*>("travel target")->Get();
    if (!target)
        return;
    TravelStatus status = target->GetStatus();
    if (status != TravelStatus::TRAVEL_STATUS_TRAVEL && status != TravelStatus::TRAVEL_STATUS_READY)
        return;

    struct St { float x = 0, y = 0; uint32 sinceMs = 0; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const float x = bot->GetPositionX(), y = bot->GetPositionY();

    std::lock_guard<std::mutex> lk(mx);
    St& st = track[bot->GetGUIDLow()];
    const float dx = x - st.x, dy = y - st.y;
    if (!st.sinceMs || dx * dx + dy * dy > 25.0f)
    {
        st.x = x; st.y = y; st.sinceMs = now;
        return;
    }
    if (now - st.sinceMs < 45000)
        return;

    target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    st.sinceMs = now;
    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        sPlayerbotAIConfig.logEvent(ai, "TravelScheduleRescue", "unscheduled travel expired", "");
}

// ABANDONED-RUN + HOSTILE-CORPSE RESCUE. When the real player logs out mid-dungeon, LFT fills were
// left masterless inside the instance; they wandered out (or graveyard-ported) into the enemy capital
// and corpse-looped under the guards (user report: alliance fills dying to Orgrimmar guards after an
// RFC run). Two rules, both random-bot-only:
//  1. A random bot inside an instance whose group has NO online real player (or no group at all)
//     is an abandoned fill: leave group, drop master, teleport home.
//  2. A dead random bot whose corpse lies in a zone where guards will farm it forever (any capital
//     or the bot ghost-looping repeatedly) gets a spirit-rez and a trip home.
static void AbandonedRunRescue(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld())
        return;
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    static std::mutex mx;
    static std::unordered_map<uint32, uint32> lastMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = lastMs[bot->GetGUIDLow()];
        if (t && now - t < 20000)
            return;
        t = now;
    }

    // Rule 2: corpse stranded in an enemy capital -> rez + go home
    if (!bot->IsAlive())
    {
        if (Corpse* corpse = bot->GetCorpse())
        {
            uint32 zone = sTerrainMgr.GetZoneId(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
            const bool allianceBot = bot->GetTeam() == ALLIANCE;
            const bool inEnemyCapital = allianceBot
                ? (zone == 1637 || zone == 1497 || zone == 1638)   // Org, UC, TB
                : (zone == 1519 || zone == 1537 || zone == 1657);  // SW, IF, Darn
            if (inEnemyCapital)
            {
                if (bot->GetGroup())
                    bot->GetGroup()->RemoveMember(bot->GetObjectGuid(), 0);
                ai->SetMaster(nullptr);
                ai->ResetStrategies();
                ai->Reset(true);
                // teleport+rez QUEUED to the world thread — direct TeleportTo from a map-update
                // thread deadlocks the MapManager (the 6h world freeze).
                sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0, 0, 0, true, true);
                sLog.outBasic("LFT rescue: %s queued for rez+home (enemy capital zone %u)", bot->GetName(), zone);
                return;
            }
        }
    }

    // Rule 1: abandoned inside an instance
    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return;
    Group* group = bot->GetGroup();
    bool hasLiveRealPlayer = false;
    if (group)
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
                if (member->isRealPlayer() && member->IsInWorld())
                    { hasLiveRealPlayer = true; break; }
    if (hasLiveRealPlayer)
        return;

    if (group)
        group->RemoveMember(bot->GetObjectGuid(), 0);
    ai->SetMaster(nullptr);
    ai->ResetStrategies();
    ai->Reset(true);
    extern bool Playerbot_GetLfgOrigin(uint32 guidLow, WorldLocation& out);
    WorldLocation origin;
    if (Playerbot_GetLfgOrigin(bot->GetGUIDLow(), origin) && !sMapStore.LookupEntry(origin.mapid)->Instanceable())
        sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), origin.mapid, origin.coord_x, origin.coord_y, origin.coord_z, false, false);
    else
        sRandomPlayerbotMgr.QueueBotTeleport(bot->GetGUIDLow(), 0, 0, 0, 0, true, false);
    sLog.outBasic("LFT rescue: %s abandoned in instance -> queued home", bot->GetName());
}

// AUTO-REPAIR. Bots almost never visit repair vendors (the only wiring is an RPG-idle flavor action
// at relevance 1.095), so gear degrades to ZERO over deaths -- verified: Thuskey, 16 deaths in 13 min,
// EVERY equipped item at durability 0, fighting a level-appropriate mob with fists and losing -- the
// self-perpetuating death spiral (death -> broken gear -> weaker -> death). Durability has NO visual
// to other players, so repairing in place is immersion-safe; charge the bot's gold when it can afford
// it (the vendor-trip gold sink real players pay), free only when broke (a broken bot can't earn).
static void AutoRepair(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return;
    struct St { uint32 lastMs; };
    static std::mutex mx;
    static std::unordered_map<uint32, uint32> last;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(mx);
        uint32& t = last[bot->GetGUIDLow()];
        if (t && now - t < 120000) return;      // check every 2 min
        t = now;
    }

    // worst equipped durability pct
    float worst = 1.0f;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item) continue;
        const uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (!maxDur) continue;
        const float pct = (float)item->GetUInt32Value(ITEM_FIELD_DURABILITY) / (float)maxDur;
        if (pct < worst) worst = pct;
    }
    if (worst >= 0.35f)
        return;

    const uint32 moneyBefore = bot->GetMoney();
    bot->DurabilityRepairAll(true, 1.0f);       // charges gold like a vendor
    if (bot->GetMoney() == moneyBefore)
        bot->DurabilityRepairAll(false, 1.0f);  // too broke -> free repair (keep the bot functional)

    FILE* f = fopen("logs/antiidle.csv", "a");
    if (!f) f = fopen("../logs/antiidle.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,%.0f|%.0f,repair:worst=%.0f%%,acted1\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), worst * 100.0f);
        fclose(f);
    }
}

// STUCK-GHOST RESCUE. Dead bots release and run to their corpse; when the ghost's path fails (navmesh
// hole) it stands at the graveyard FOREVER (watched Tarinari: 100% DEAD for 20+ minutes, position
// frozen). A ghost is INVISIBLE to living players, so teleporting it to its corpse is immersion-safe
// even in supervisor mode near players -- observers only ever see the bot pop back up at its corpse,
// which is exactly what a normal res looks like. Frozen (<8y net) for 30s+ as a ghost -> teleport to
// corpse so ReviveFromCorpseAction can reclaim.
static void GhostRescue(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || bot->IsAlive() || !bot->GetMap())
        return;
    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;                                   // not released yet (corpse still held) -> engine handles
    Corpse* corpse = bot->GetCorpse();
    if (!corpse || corpse->GetMapId() != bot->GetMapId())
        return;

    struct St { float x; float y; uint32 sinceMs; };
    static std::mutex mx;
    static std::unordered_map<uint32, St> track;
    const uint32 now = WorldTimer::getMSTime();
    const float x = bot->GetPositionX(), y = bot->GetPositionY();
    {
        std::lock_guard<std::mutex> lk(mx);
        St& s = track[bot->GetGUIDLow()];
        if (!s.sinceMs) { s.x = x; s.y = y; s.sinceMs = now; return; }
        const float dx = x - s.x, dy = y - s.y;
        if (dx * dx + dy * dy >= 8.0f * 8.0f) { s.x = x; s.y = y; s.sinceMs = now; return; }
        if (now - s.sinceMs < 30000) return;
        s.x = corpse->GetPositionX(); s.y = corpse->GetPositionY(); s.sinceMs = now;
    }

    bot->NearTeleportTo(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ() + 0.5f,
        bot->GetOrientation());

    FILE* f = fopen("logs/freeze_fix.csv", "a");
    if (!f) f = fopen("../logs/freeze_fix.csv", "a");
    if (f)
    {
        fprintf(f, "%s,%s,map%u,from=%.0f|%.0f,to=%.0f|%.0f,GHOST\n", sPlayerbotAIConfig.GetTimestampStr().c_str(),
            bot->GetName(), bot->GetMapId(), x, y, corpse->GetPositionX(), corpse->GetPositionY());
        fclose(f);
    }
}

// DEBUG: an idle bot near a real player SAYs what it is thinking/trying (~1/sec) so you can read its
// "thoughts" in local /say right next to it and see WHY it's standing: its last action, current
// target, travel destination, and the ms until its NEXT decision (that number counts DOWN during a
// freeze, so you literally watch the pause). Only fires when the bot is standing still + out of combat
// (the "sitting idle" state you want to diagnose) and only for bots near a real player (so it's just
// the ones you're looking at). Gated by the action-log flag -> only the small watched fleet.
static void DebugSayIdle(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld() || bot->IsInCombat())
        return;
    if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
        return;                                  // only when standing still
    if (!ai->HasPlayerNearby())
        return;                                  // only bots near a real player (what you're watching)

    static std::mutex sayMx;
    static std::unordered_map<uint32, uint32> lastSayMs;
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> lk(sayMx);
        uint32& t = lastSayMs[bot->GetGUIDLow()];
        if (t && now - t < 1000)
            return;                              // ~1 line / sec / bot (readable, not spam)
        t = now;
    }

    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx)
        return;

    const float bx = bot->GetPositionX(), by = bot->GetPositionY(), bz = bot->GetPositionZ();
    const Action* la = ai->GetLastExecutedAction(ai->GetState());
    const std::string actName = la ? const_cast<Action*>(la)->getName() : "idle";
    Unit* tgt = ctx->GetValue<Unit*>("current target")->Get();
    TravelTarget* tt = ctx->GetValue<TravelTarget*>("travel target")->Get();

    // Concise line for in-game /say (kept short + now carries the bot's own coordinate).
    std::ostringstream say;
    say << "[" << actName << "]";
    if (tgt)
        say << " tgt=" << tgt->GetName() << "(" << (int)bot->GetDistance(tgt) << "y)";
    if (tt && tt->GetDestination())
        say << " ->" << tt->GetDestination()->GetTitle();
    say << " next=" << ai->GetAIInternalUpdateDelay() << "ms @" << (int)bx << "," << (int)by;
    bot->Say(say.str(), LANG_UNIVERSAL);

    // Rich WHY-line to the log: action + coordinate for the bot AND the target/destination, plus the
    // gates that decide whether it CAN move/act, so a standing-still-but-ticking bot reveals its
    // blocker (target on another map, no real destination position, movement disallowed, LOS, etc).
    std::ostringstream log;
    log << actName
        << " bot=(" << (int)bx << "," << (int)by << "," << (int)bz << ") map=" << bot->GetMapId()
        << " mv=" << (sServerFacade.isMoving(bot) ? 1 : 0)
        << " canMove=" << (ai->CanMove() ? 1 : 0)
        << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0);
    if (tgt)
        log << " tgt=" << tgt->GetName()
            << " tgtPos=(" << (int)tgt->GetPositionX() << "," << (int)tgt->GetPositionY() << "," << (int)tgt->GetPositionZ() << ")"
            << " tgtMap=" << tgt->GetMapId()
            << " dist=" << (int)bot->GetDistance(tgt)
            << " los=" << (bot->IsWithinLOSInMap(tgt, true) ? 1 : 0);
    else
        log << " tgt=none";
    if (tt)
    {
        log << " travelStatus=" << (int)tt->GetStatus()
            << " dest=" << (tt->GetDestination() ? tt->GetDestination()->GetTitle() : "null");
        if (WorldPosition* p = tt->GetPosition())
            log << " destPos=(" << (int)p->getX() << "," << (int)p->getY() << "," << (int)p->getZ() << ")"
                << " destMap=" << (int)p->getMapId()
                << " destDist=" << (int)p->distance(WorldPosition(bot));
    }
    else
        log << " travel=none";
    log << " next=" << ai->GetAIInternalUpdateDelay() << "ms";

    FILE* f = fopen("logs/bot_thoughts.log", "a");
    if (!f) f = fopen("../logs/bot_thoughts.log", "a");
    if (f)
    {
        fprintf(f, "%s %s %s\n", sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), log.str().c_str());
        fclose(f);
    }
}

// SUPERVISOR VISIBLE-ACTIVITY TRACKER — the HONEST metric. Classifies each SECOND of a bot's life by
// what is EXTERNALLY VISIBLE to another player, so we measure ACTION not INTENTION:
//   DEAD | MOVE (real position change >1.5y) | REGEN (sitting: eat/drink) | CAST (a spell actually in
//   progress) | MELEE (in combat, victim in reach) | LOOT (loot flag) | else IDLE.
// Intentions that "execute" but do nothing visible (choose rpg target, rpg work, a filler drink that
// returns true, target selection) produce NONE of the above -> they fall to IDLE. That is the whole
// point: the existing apm metric counts any action that returns true (PlayerbotAI.cpp filler bug), so
// it looks busy while bots stand around; THIS counts only what a bystander would actually see.
// Writes a per-second row (type S) and a rolling 60s summary (type M, with %s + a true-APM estimate
// and netMove so "moving flag but not displacing" can't fake movement) to logs/supervisor.csv.
static void SupervisorTrack(PlayerbotAI* ai, Player* bot)
{
    if (!ai || !bot || !bot->IsInWorld())
        return;

    struct SupState
    {
        uint32 lastSampleMs = 0;
        float lastX = 0, lastY = 0;
        uint32 winStartSec = 0;
        float winStartX = 0, winStartY = 0;
        uint32 secs = 0, moveSec = 0, castSec = 0, meleeSec = 0, lootSec = 0, regenSec = 0, idleSec = 0, combatSec = 0, deadSec = 0;
        uint32 castStarts = 0, loots = 0;
        double swingAccum = 0.0;          // fractional auto-attack swings accumulated over melee seconds
        uint32 lastCastSpellId = 0;
        bool prevLooting = false;
    };
    static std::mutex smx;
    static std::unordered_map<uint32, SupState> smap;

    const uint32 nowMs = WorldTimer::getMSTime();
    const uint32 nowSec = (uint32)time(0);
    const uint32 key = bot->GetGUIDLow();

    std::lock_guard<std::mutex> lk(smx);
    SupState& s = smap[key];
    if (s.lastSampleMs && nowMs - s.lastSampleMs < 1000)
        return;                            // one sample / bot / second
    const bool firstSample = (s.lastSampleMs == 0);
    s.lastSampleMs = nowMs;

    const float x = bot->GetPositionX(), y = bot->GetPositionY();
    if (firstSample) { s.lastX = x; s.lastY = y; s.winStartSec = nowSec; s.winStartX = x; s.winStartY = y; }

    // ---- classify THIS second by externally-visible state ----
    const bool dead = !bot->IsAlive();
    const float dx = x - s.lastX, dy = y - s.lastY;
    const bool moved = (dx * dx + dy * dy) > (1.5f * 1.5f);
    const bool casting = bot->IsNonMeleeSpellCasted(true, false, true);
    const bool sitting = !bot->IsStandState();
    Unit* victim = bot->GetVictim();
    const bool melee = bot->IsInCombat() && victim && bot->CanReachWithMeleeAutoAttack(victim);
    const bool looting = bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);

    uint32 curSpellId = 0;
    if (Spell* sp = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)) { if (sp->m_spellInfo) curSpellId = sp->m_spellInfo->Id; }
    if (!curSpellId) { if (Spell* sp = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL)) { if (sp->m_spellInfo) curSpellId = sp->m_spellInfo->Id; } }

    // discrete visible "press": a spell now in progress that wasn't a second ago
    if (curSpellId && curSpellId != s.lastCastSpellId)
        ++s.castStarts;
    s.lastCastSpellId = curSpellId;

    // discrete loot: rising edge of the looting flag
    if (looting && !s.prevLooting)
        ++s.loots;
    s.prevLooting = looting;

    // bucket the second (movement first: travel is the dominant visible activity; sitting = downtime)
    const char* vis;
    if (dead)         { ++s.deadSec;  vis = "DEAD"; }
    else if (moved)   { ++s.moveSec;  vis = "MOVE"; }
    else if (sitting) { ++s.regenSec; vis = "REGEN"; }
    else if (casting) { ++s.castSec;  vis = "CAST"; }
    else if (melee)   { ++s.meleeSec; vis = "MELEE"; }
    else if (looting) { ++s.lootSec;  vis = "LOOT"; }
    else              { ++s.idleSec;  vis = "IDLE"; }
    if (bot->IsInCombat()) ++s.combatSec;
    if (melee) { const uint32 at = bot->GetAttackTime(BASE_ATTACK); if (at) s.swingAccum += 1000.0 / at; }
    ++s.secs;
    s.lastX = x; s.lastY = y;

    const Action* la = ai->GetLastExecutedAction(ai->GetState());
    const std::string intend = la ? const_cast<Action*>(la)->getName() : std::string("-");
    const uint32 maxMana = bot->GetMaxPower(POWER_MANA);
    const int manaPct = maxMana ? (int)(bot->GetPower(POWER_MANA) * 100 / maxMana) : -1;

    FILE* f = fopen("logs/supervisor.csv", "a");
    if (!f) f = fopen("../logs/supervisor.csv", "a");
    if (!f)
        return;

    fprintf(f, "%s,S,%s,L%u,m%u,%.0f|%.0f,vis=%s,intend=%s,combat%d,mana%d\n",
        sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), bot->GetLevel(), bot->GetMapId(),
        x, y, vis, intend.c_str(), bot->IsInCombat() ? 1 : 0, manaPct);

    if (nowSec - s.winStartSec >= 60 && s.secs > 0)
    {
        const float netMove = std::sqrt((x - s.winStartX) * (x - s.winStartX) + (y - s.winStartY) * (y - s.winStartY));
        const uint32 win = s.secs;
        auto pct = [win](uint32 v) -> uint32 { return (uint32)(100.0 * v / win); };
        const uint32 castsPM = (uint32)(s.castStarts * 60.0 / win);
        const uint32 swingsPM = (uint32)(s.swingAccum * 60.0 / win);
        const uint32 lootsPM = (uint32)(s.loots * 60.0 / win);
        const uint32 apm = castsPM + swingsPM + lootsPM;
        const uint32 activePct = pct(s.moveSec + s.castSec + s.meleeSec + s.lootSec);
        fprintf(f, "%s,M,%s,L%u,m%u,%.0f|%.0f,secs=%u,apm~%u,castsPM=%u,swingsPM~%u,lootsPM=%u,"
                   "move%%=%u,cast%%=%u,melee%%=%u,loot%%=%u,regen%%=%u,idle%%=%u,active%%=%u,combat%%=%u,netMove=%.0f\n",
            sPlayerbotAIConfig.GetTimestampStr().c_str(), bot->GetName(), bot->GetLevel(), bot->GetMapId(),
            x, y, win, apm, castsPM, swingsPM, lootsPM,
            pct(s.moveSec), pct(s.castSec), pct(s.meleeSec), pct(s.lootSec), pct(s.regenSec), pct(s.idleSec),
            activePct, pct(s.combatSec), netMove);
        s.winStartSec = nowSec; s.winStartX = x; s.winStartY = y;
        s.secs = s.moveSec = s.castSec = s.meleeSec = s.lootSec = s.regenSec = s.idleSec = s.combatSec = s.deadSec = 0;
        s.castStarts = s.loots = 0; s.swingAccum = 0.0;
    }
    fclose(f);
}

namespace
{
    constexpr uint32 VISIBLE_IDLE_WARN_SECONDS = 5;
    constexpr uint32 VISIBLE_IDLE_RESCUE_SECONDS = 10;

    // CALCULATED MODE: when false, the metric/snapshot "rescue" cascades that
    // re-pick the bot's work (clear targets, tear down travel, force moves) are
    // disabled, leaving TrackActionMetrics / LogVisibleActivitySnapshot to MEASURE
    // ONLY. The bot is driven solely by its strategy engine. See the matching
    // kEnableLegacyFastLanes gate in DoNextAction.
    constexpr bool ENABLE_METRIC_RESCUES = false;

    bool IsForegroundPriority(ActivePiorityType type)
    {
        switch (type)
        {
        case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
        case ActivePiorityType::IS_REAL_PLAYER:
        case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
        case ActivePiorityType::VISIBLE_FOR_PLAYER:
        case ActivePiorityType::NEARBY_PLAYER:
        case ActivePiorityType::IN_COMBAT:
        case ActivePiorityType::IN_INSTANCE:
        case ActivePiorityType::IN_BATTLEGROUND:
        case ActivePiorityType::IS_RUNNING_TEST:
            return true;
        default:
            return false;
        }
    }

    // LOD COLD tier: bots with no real player in their zone/map (or an empty server). No
    // one can see them, so their heavy brain (DoNextAction: travel + decision engine) can
    // run on a long interval instead of the ~1s background cadence. This is the dominant
    // CPU saver AND crash-rate reducer (crashes scale with bot-AI execution volume). HOT
    // (foreground) and WARM (IN_ACTIVE_AREA = a player's own zone) are intentionally NOT
    // cold, so anything a player could actually see stays responsive.
    bool IsColdLODPriority(ActivePiorityType type)
    {
        switch (type)
        {
        case ActivePiorityType::IN_ACTIVE_MAP:
        case ActivePiorityType::IN_INACTIVE_MAP:
        case ActivePiorityType::IN_EMPTY_SERVER:
            return true;
        default:
            return false;
        }
    }

    uint32 GetSpellInProgressRecheckDelay(uint32 remainingMs)
    {
        if (remainingMs > 0)
            return std::min<uint32>(remainingMs + sPlayerbotAIConfig.reactDelay + sWorld.GetAverageDiff(), 5000);

        // A finished-but-still-current spell should be rechecked quickly so
        // casters can chain the next spell instead of burning a full reaction delay.
        return std::max<uint32>(1, std::min<uint32>(sWorld.GetAverageDiff(), 50));
    }

    bool IsActiveTravelWork(TravelTarget* travelTarget)
    {
        if (!travelTarget || !travelTarget->GetDestination() || dynamic_cast<NullTravelDestination*>(travelTarget->GetDestination()))
            return false;

        switch (travelTarget->GetStatus())
        {
            case TravelStatus::TRAVEL_STATUS_READY:
            case TravelStatus::TRAVEL_STATUS_TRAVEL:
            case TravelStatus::TRAVEL_STATUS_WORK:
                return true;
            default:
                return false;
        }
    }

    bool IsPreparedTravelWork(TravelTarget* travelTarget)
    {
        return travelTarget && travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE;
    }

    bool IsVisibleTargetWork(Player* bot, Unit* target, bool targetMovementProgress)
    {
        if (!bot || !target)
            return false;

        if (bot->IsNonMeleeSpellCasted(true, false, true))
            return true;

        if (targetMovementProgress)
            return true;

        return bot->GetVictim() == target &&
               bot->CanReachWithMeleeAutoAttack(target) &&
               bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
    }

    bool IsChasingTarget(Player* bot, Unit* target)
    {
        if (!bot || !target)
            return false;

        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm || mm->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
            return false;

        Unit* chaseTarget = sServerFacade.GetChaseTarget(bot);
        return chaseTarget &&
               chaseTarget->GetObjectGuid() == target->GetObjectGuid() &&
               (sServerFacade.isMoving(bot) || !bot->IsStopped() || bot->isMovingOrTurning());
    }

    bool HasUsefulAssignedWork(Player* bot, TravelTarget* travelTarget, Unit* target, LootObject lootTarget,
        bool movingAfter, bool directMovementIssued = false)
    {
        // A queued target/loot/travel object is not useful by itself. It only
        // counts when the bot is visibly acting on it, otherwise stale plans can
        // reset the idle clock forever while the bot stands still.
        const bool movingOnDirectedWork =
            movingAfter &&
            (directMovementIssued || IsActiveTravelWork(travelTarget) || IsPreparedTravelWork(travelTarget) ||
                !lootTarget.IsEmpty());

        return (bot && bot->IsNonMeleeSpellCasted(true, false, true)) ||
               IsVisibleTargetWork(bot, target, target && IsChasingTarget(bot, target)) ||
               movingOnDirectedWork;
    }

    bool HasConcreteRecoveryWork(Player* bot, TravelTarget* travelTarget, Unit* target, LootObject lootTarget,
        bool movingAfter, bool directTravelMovementIssued = false, bool allowConnectedMelee = true)
    {
        if (!bot)
            return false;

        if (bot->IsNonMeleeSpellCasted(true, false, true))
            return true;

        if (allowConnectedMelee &&
            target &&
            bot->GetVictim() == target &&
            bot->CanReachWithMeleeAutoAttack(target) &&
            bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING))
            return true;

        // Movement requests are only concrete recovery when they are attached to
        // real travel/loot work. Target/RPG nudges must prove themselves via
        // actual movement progress on the next metric tick.
        return movingAfter &&
               directTravelMovementIssued &&
               (IsActiveTravelWork(travelTarget) || IsPreparedTravelWork(travelTarget) || !lootTarget.IsEmpty());
    }

    bool HasDirectedMovementWork(TravelTarget* travelTarget, GuidPosition rpgTarget,
        bool travelMoved, bool rpgMoved, bool movingAfter)
    {
        if (!movingAfter)
            return false;

        return (travelMoved && (IsActiveTravelWork(travelTarget) || IsPreparedTravelWork(travelTarget))) ||
               (rpgMoved && rpgTarget);
    }

    bool HasFallbackMovementWork(bool randomMoved, bool idleNudged, bool movingAfter)
    {
        return movingAfter && (randomMoved || idleNudged);
    }

    bool RequestGrindTravelFallback(PlayerbotAI* ai, AiObjectContext* context, Player* bot, char const* reason);
    bool ForceIdleRescueNudge(PlayerbotAI* ai, Player* bot, char const* reason);
    bool ForceConcreteTargetWork(PlayerbotAI* ai, Player* bot, Unit* target, char const* reason);
    bool ForceCombatConnect(PlayerbotAI* ai, Player* bot, Unit* target, char const* reason);

    struct IdleRescueFailureState
    {
        uint32 failures = 0;
        time_t firstFailure = 0;
        time_t lastFailure = 0;
    };

    std::mutex s_idleRescueFailureMutex;
    std::unordered_map<uint32, IdleRescueFailureState> s_idleRescueFailures;

    void ResetTargetScanCaches(AiObjectContext* context)
    {
        if (!context)
            return;

        context->GetValue<std::list<ObjectGuid>>("possible targets")->Reset();
        context->GetValue<std::list<ObjectGuid>>("possible targets no los")->Reset();
        context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Reset();
        context->GetValue<std::list<ObjectGuid>>("attackers")->Reset();
        context->GetValue<Unit*>("grind target")->Reset();
    }

    void ClearCurrentCombatTarget(AiObjectContext* context, Player* bot, Unit* target, bool stopCombat)
    {
        if (!context || !bot)
            return;

        if (target && bot->GetVictim() == target)
            bot->AttackStop(true);

        context->GetValue<Unit*>("old target")->Reset();
        context->GetValue<Unit*>("current target")->Reset();
        context->GetValue<Unit*>("pull target")->Reset();
        context->GetValue<Unit*>("dps target")->Reset();
        context->GetValue<ObjectGuid>("attack target")->Reset();
        bot->SetSelectionGuid(ObjectGuid());
        bot->SetTargetGuid(ObjectGuid());

        if (stopCombat)
            bot->CombatStop(false);

        ResetTargetScanCaches(context);
    }

    uint32 RegisterIdleRescueNudgeFailure(Player* bot)
    {
        if (!bot)
            return 0;

        std::lock_guard<std::mutex> lock(s_idleRescueFailureMutex);
        const time_t now = time(nullptr);
        IdleRescueFailureState& state = s_idleRescueFailures[bot->GetGUIDLow()];

        if (!state.lastFailure || now > state.lastFailure + 90)
        {
            state.failures = 0;
            state.firstFailure = now;
        }

        state.lastFailure = now;
        return ++state.failures;
    }

    void ResetIdleRescueNudgeFailures(Player* bot)
    {
        if (!bot)
            return;

        std::lock_guard<std::mutex> lock(s_idleRescueFailureMutex);
        s_idleRescueFailures.erase(bot->GetGUIDLow());
    }

    void ClearAutonomousIdleWorkState(PlayerbotAI* ai, Player* bot)
    {
        if (!ai || !bot)
            return;

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return;

        TravelTarget* travelTarget = context->GetValue<TravelTarget*>("travel target")->Get();
        if (travelTarget)
        {
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            travelTarget->SetForced(false);
        }

        ClearCurrentCombatTarget(context, bot, context->GetValue<Unit*>("current target")->Get(), false);
        context->GetValue<LootObject>("loot target")->Reset();
        context->GetValue<GuidPosition>("rpg target")->Reset();
        context->GetValue<bool>("travel target active")->Reset();
        context->GetValue<ObjectGuid>("attack target")->Reset();
        context->ClearValues("no active travel destinations");
    }

    bool HardRescueIdleBotToHomebind(PlayerbotAI* ai, Player* bot, char const* reason, char const* source, uint32 failures)
    {
        if (!ai || !bot || !bot->IsInWorld() || bot->isRealPlayer() || bot->IsBeingTeleported() ||
            bot->InBattleGround() || sServerFacade.UnitIsDead(bot) || sServerFacade.IsInCombat(bot) ||
            ai->HasActivePlayerMaster() || ai->HasRealPlayerMaster())
            return false;

        const uint32 oldMap = bot->GetMapId();
        const float oldX = bot->GetPositionX();
        const float oldY = bot->GetPositionY();
        const float oldZ = bot->GetPositionZ();

        ClearAutonomousIdleWorkState(ai, bot);
        bot->InterruptMoving(true);

        const bool teleported = bot->TeleportToHomebind(0, false);
        if (teleported)
            ResetIdleRescueNudgeFailures(bot);

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "reason=" << (reason ? reason : "idle-rescue")
                << " source=" << (source ? source : "unknown")
                << " failures=" << failures
                << " teleported=" << (teleported ? 1 : 0)
                << " oldMap=" << oldMap
                << " oldX=" << std::fixed << std::setprecision(2) << oldX
                << " oldY=" << oldY
                << " oldZ=" << oldZ;
            sPlayerbotAIConfig.logEvent(ai, "IdleRescueHomebind", "", out.str());
        }

        return teleported;
    }

    bool MoveAssignedTravelWork(PlayerbotAI* ai, AiObjectContext* context, Player* bot, char const* reason)
    {
        if (!ai || !context || !bot)
            return false;

        TravelTarget* travelTarget = context->GetValue<TravelTarget*>("travel target")->Get();
        if (!travelTarget)
            return false;

        if (travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        {
            ai->DoSpecificAction("choose travel target",
                Event(reason ? reason : "assigned travel", "resolve assigned travel work", bot), true);
            travelTarget = context->GetValue<TravelTarget*>("travel target")->Get();
        }

        if (!IsActiveTravelWork(travelTarget))
            return false;

        Event moveEvent(reason ? reason : "assigned travel", "move assigned travel work", bot);
        bool moved = ai->DoSpecificAction("move to travel target", moveEvent, true);
        if (!moved && ai->CanMove())
        {
            // Hard-idle rescue is allowed to bypass isUseful(): a READY/TRAVEL/WORK
            // destination is concrete work, and stale "already moving" state must not
            // leave the bot standing still with a valid travel target.
            MoveToTravelTargetAction forcedMove(ai);
            moved = forcedMove.Execute(moveEvent);

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << (reason ? reason : "assigned-travel")
                    << " status=" << static_cast<uint32>(travelTarget->GetStatus())
                    << " destinationPtr=" << (travelTarget->GetDestination() ? 1 : 0)
                    << " moved=" << (moved ? 1 : 0)
                    << " movingAfter=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0);
                sPlayerbotAIConfig.logEvent(ai, "ForcedTravelMoveDispatch", "", out.str());
            }
        }

        const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        if (!moved && !movingAfter && !bot->IsNonMeleeSpellCasted(true, false, true) &&
            !sServerFacade.IsInCombat(bot))
        {
            if (travelTarget)
            {
                travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                travelTarget->SetExpireIn(1000);
                travelTarget->SetForced(false);
            }

            context->GetValue<bool>("travel target active")->Reset();
            context->GetValue<GuidPosition>("rpg target")->Reset();
            context->GetValue<ObjectGuid>("attack target")->Reset();
            context->ClearValues("no active travel destinations");

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << (reason ? reason : "assigned-travel")
                    << " status=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
                    << " destinationPtr=" << (travelTarget && travelTarget->GetDestination() ? 1 : 0)
                    << " movingAfter=0";
                sPlayerbotAIConfig.logEvent(ai, "AssignedTravelNoMoveExpired", "", out.str());
            }
        }

        return moved;
    }

    bool TryRecoveryWork(PlayerbotAI* ai, AiObjectContext* context, Player* bot, char const* reason)
    {
        if (!ai || !context || !bot || sServerFacade.IsInCombat(bot) || sServerFacade.UnitIsDead(bot))
            return false;

        const bool shouldEat = context->GetValue<bool>("should eat")->Get();
        const bool shouldDrink = context->GetValue<bool>("should drink")->Get();
        if (!shouldEat && !shouldDrink)
            return false;

        context->GetValue<Unit*>("current target")->Reset();
        context->GetValue<Unit*>("pull target")->Reset();
        context->GetValue<ObjectGuid>("attack target")->Reset();
        bot->AttackStop(true);
        bot->SetSelectionGuid(ObjectGuid());
        bot->SetTargetGuid(ObjectGuid());

        const bool wasMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        if (wasMoving)
        {
            ai->StopMoving();
            bot->InterruptMoving(true);
        }

        bool ate = false;
        bool drank = false;
        if (!wasMoving)
        {
            if (shouldEat)
                ate = ai->DoSpecificAction("food", Event(reason ? reason : "recovery", "idle recovery", bot), true);
            if (shouldDrink)
                drank = ai->DoSpecificAction("drink", Event(reason ? reason : "recovery", "idle recovery", bot), true);
        }

        const bool recovering = wasMoving || ate || drank || bot->IsNonMeleeSpellCasted(true, false, true);
        if (recovering && sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 8) == 1)
        {
            std::ostringstream out;
            out << "reason=" << (reason ? reason : "recovery")
                << " hp=" << static_cast<uint32>(bot->GetHealthPercent())
                << " mana=" << static_cast<uint32>(bot->GetPowerPercent())
                << " shouldEat=" << (shouldEat ? 1 : 0)
                << " shouldDrink=" << (shouldDrink ? 1 : 0)
                << " stoppedMove=" << (wasMoving ? 1 : 0)
                << " ate=" << (ate ? 1 : 0)
                << " drank=" << (drank ? 1 : 0)
                << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "IdleRecoveryWork", "", out.str());
        }

        return recovering;
    }

    bool PushVisibleWork(PlayerbotAI* ai, AiObjectContext* context, Player* bot, char const* reason,
        bool& questRequested, bool& travelChosen, bool& targetSelected, bool& attackAnything,
        bool& grindRequested, bool& travelMoved, bool& rpgChosen, bool& rpgMoved,
        bool& randomMoved, bool& idleNudged, bool allowFallbackMovement = true,
        bool allowLocalTargets = true)
    {
        if (!ai || !context || !bot)
            return false;

        if (TryRecoveryWork(ai, context, bot, reason))
            return true;

        auto hasImmediateWork = [&]() -> bool
        {
            Unit* workTarget = context->GetValue<Unit*>("current target")->Get();
            LootObject workLoot = context->GetValue<LootObject>("loot target")->Get();
            GuidPosition workRpgTarget = context->GetValue<GuidPosition>("rpg target")->Get();
            TravelTarget* workTravel = context->GetValue<TravelTarget*>("travel target")->Get();
            const bool workMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovement =
                HasDirectedMovementWork(workTravel, workRpgTarget, travelMoved, rpgMoved, workMoving);
            return HasUsefulAssignedWork(bot, workTravel, workTarget, workLoot, workMoving, directedMovement);
        };

        auto tryAssignedTravelMove = [&]() -> bool
        {
            const bool moved = MoveAssignedTravelWork(ai, context, bot, reason);
            travelMoved = travelMoved || moved;
            return moved;
        };

        questRequested = ai->DoSpecificAction("request quest travel target", Event(), true);
        if (questRequested)
            tryAssignedTravelMove();

        if (!hasImmediateWork())
        {
            travelChosen = ai->DoSpecificAction("choose travel target", Event(), true);
            if (travelChosen)
                tryAssignedTravelMove();
        }

        if (allowLocalTargets && !hasImmediateWork())
        {
            ResetTargetScanCaches(context);
            targetSelected = ai->DoSpecificAction("select new target", Event(), true);
        }

        if (allowLocalTargets && !hasImmediateWork())
            attackAnything = ai->DoSpecificAction("attack anything", Event(), true);

        Unit* selectedTarget = context->GetValue<Unit*>("current target")->Get();
        if (allowLocalTargets && !hasImmediateWork() && selectedTarget)
        {
            attackAnything = ForceConcreteTargetWork(ai, bot, selectedTarget, reason) || attackAnything;
        }

        if (!hasImmediateWork())
        {
            grindRequested = RequestGrindTravelFallback(ai, context, bot, reason);
            if (grindRequested)
                tryAssignedTravelMove();
        }

        if (allowFallbackMovement && !hasImmediateWork())
        {
            rpgChosen = ai->DoSpecificAction("choose rpg target", Event(), true);
            rpgMoved = ai->DoSpecificAction("move to rpg target", Event(), true);
        }

        if (allowFallbackMovement && !hasImmediateWork() && !rpgMoved)
            randomMoved = ai->DoSpecificAction("move random", Event(reason ? reason : "idle work", "visible work rescue", bot), true);

        if (allowFallbackMovement && !hasImmediateWork() && !randomMoved && !sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
            idleNudged = ForceIdleRescueNudge(ai, bot, reason);

        return hasImmediateWork();
    }

    bool RequestGrindTravelFallback(PlayerbotAI* ai, AiObjectContext* context, Player* bot, char const* reason)
    {
        if (!ai || !context || !bot || bot->InBattleGround() ||
            ai->HasActivePlayerMaster() || ai->HasRealPlayerMaster())
            return false;

        const std::string grindPurpose = std::to_string(static_cast<uint32>(TravelDestinationPurpose::Grind));
        const std::string reasonText = reason ? reason : "";
        const bool forceNoWorkRetry =
            reasonText.find("idle") != std::string::npos ||
            reasonText.find("no-work") != std::string::npos ||
            reasonText.find("hard-idle") != std::string::npos ||
            reasonText.find("metric") != std::string::npos ||
            reasonText.find("movement-no-progress") != std::string::npos;

        static std::mutex s_grindFallbackCooldownMutex;
        static std::unordered_map<uint32, time_t> s_grindFallbackNextAttempt;
        const time_t now = time(nullptr);
        const uint32 botGuid = bot->GetGUIDLow();
        {
            std::lock_guard<std::mutex> guard(s_grindFallbackCooldownMutex);
            auto itr = s_grindFallbackNextAttempt.find(botGuid);
            if (itr != s_grindFallbackNextAttempt.end() && itr->second > now)
            {
                if (forceNoWorkRetry && sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 25) == 1)
                {
                    std::ostringstream out;
                    out << "reason=" << (reason ? reason : "no-work")
                        << " level=" << (uint32)bot->GetLevel()
                        << " cooldownLeft=" << uint32(itr->second - now);
                    sPlayerbotAIConfig.logEvent(ai, "NoWorkGrindTravelCooldown", grindPurpose, out.str());
                }
                return false;
            }

            s_grindFallbackNextAttempt[botGuid] = now + (forceNoWorkRetry ? 1 : 8);
        }

        TravelTarget* currentTravel = context->GetValue<TravelTarget*>("travel target")->Get();
        const bool hasActiveTravel =
            IsActiveTravelWork(currentTravel) ||
            IsPreparedTravelWork(currentTravel);
        if (forceNoWorkRetry && !hasActiveTravel)
        {
            if (currentTravel)
            {
                currentTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                currentTravel->SetForced(false);
            }

            context->GetValue<bool>("travel target active")->Reset();
            context->ClearValues("no active travel destinations");
        }

        const bool grindKnownEmpty =
            context->GetValue<bool>("no active travel destinations", grindPurpose)->Get() ||
            context->GetValue<bool>("no active travel destinations", "Grind")->Get();
        if (grindKnownEmpty && forceNoWorkRetry)
        {
            context->GetValue<bool>("no active travel destinations", grindPurpose)->Reset();
            context->GetValue<bool>("no active travel destinations", "Grind")->Reset();
            context->ClearValues("no active travel destinations");

            if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 5) == 1)
            {
                std::ostringstream out;
                out << "reason=" << (reason ? reason : "no-work")
                    << " level=" << (uint32)bot->GetLevel()
                    << " emptyGrindKnown=1"
                    << " forcedRetry=1";
                sPlayerbotAIConfig.logEvent(ai, "NoWorkGrindTravelRetry", grindPurpose, out.str());
            }
        }
        else if (grindKnownEmpty)
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 25) == 1)
            {
                std::ostringstream out;
                out << "reason=" << (reason ? reason : "no-work")
                    << " level=" << (uint32)bot->GetLevel()
                    << " emptyGrindKnown=1";
                sPlayerbotAIConfig.logEvent(ai, "NoWorkGrindTravelSuppressed", grindPurpose, out.str());
            }

            return false;
        }

        const bool requested = ai->DoSpecificAction("request travel target::" + grindPurpose,
            Event(reason ? reason : "no work", "grind fallback", bot), true);

        if (requested && sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 5) == 1)
        {
            std::ostringstream out;
            out << "reason=" << (reason ? reason : "no-work")
                << " level=" << (uint32)bot->GetLevel()
                << " map=" << bot->GetMapId()
                << " x=" << std::fixed << std::setprecision(2) << bot->GetPositionX()
                << " y=" << bot->GetPositionY();
            sPlayerbotAIConfig.logEvent(ai, "NoWorkGrindTravelRequest", grindPurpose, out.str());
        }

        return requested;
    }

    bool ForceIdleRescueNudge(PlayerbotAI* ai, Player* bot, char const* reason)
    {
        if (!ai || !bot || !bot->IsInWorld() || sServerFacade.UnitIsDead(bot) ||
            sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
            return false;

        // Pure anti-AFK "look active" nudge — NEVER allowed during a fight or
        // while the bot has a live target/cast. No fake movement interrupts combat.
        if (sServerFacade.IsInCombat(bot) ||
            bot->GetVictim() ||
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() ||
            bot->IsNonMeleeSpellCasted(true, false, true))
            return false;

        const float botX = bot->GetPositionX();
        const float botY = bot->GetPositionY();
        const float botZ = bot->GetPositionZ();
        if (!bot->GetMap() || !MaNGOS::IsValidMapCoord(botX, botY, botZ))
        {
            const uint32 failures = RegisterIdleRescueNudgeFailure(bot);
            return HardRescueIdleBotToHomebind(ai, bot, reason, "current-position-invalid", failures);
        }

        const float angle = static_cast<float>((bot->GetGUIDLow() + time(nullptr)) % 6283) / 1000.0f;
        const float distance = static_cast<float>(urand(8, 22));
        float x = botX + std::cos(angle) * distance;
        float y = botY + std::sin(angle) * distance;
        float z = botZ;
        if (!bot->GetMap() || !MaNGOS::IsValidMapCoord(x, y, z))
        {
            const uint32 failures = RegisterIdleRescueNudgeFailure(bot);
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << (reason ? reason : "idle-rescue")
                    << " failures=" << failures
                    << " x=" << std::fixed << std::setprecision(2) << x
                    << " y=" << y
                    << " z=" << z;
                sPlayerbotAIConfig.logEvent(ai, "IdleRescueNudgeRejected", "", out.str());
            }

            if (failures >= 3)
                return HardRescueIdleBotToHomebind(ai, bot, reason, "nudge-position-invalid", failures);

            return false;
        }

        bot->UpdateAllowedPositionZ(x, y, z);
        if (!MaNGOS::IsValidMapCoord(x, y, z))
        {
            const uint32 failures = RegisterIdleRescueNudgeFailure(bot);
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << (reason ? reason : "idle-rescue")
                    << " failures=" << failures
                    << " x=" << std::fixed << std::setprecision(2) << x
                    << " y=" << y
                    << " z=" << z;
                sPlayerbotAIConfig.logEvent(ai, "IdleRescueNudgeRejected", "", out.str());
            }

            if (failures >= 3)
                return HardRescueIdleBotToHomebind(ai, bot, reason, "nudge-height-invalid", failures);

            return false;
        }

        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm)
            return false;

#ifdef MANGOSBOT_ZERO
        mm->MovePoint(bot->GetMapId(), x, y, z, MOVE_RUN_MODE, 0.0f, -10.0f);
#else
        mm->MovePoint(bot->GetMapId(), Position(x, y, z, 0.0f), MOVE_RUN_MODE, 0.0f, -10.0f);
#endif

        if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 8) == 1)
        {
            std::ostringstream out;
            out << "reason=" << (reason ? reason : "idle-rescue")
                << " x=" << std::fixed << std::setprecision(2) << x
                << " y=" << y
                << " z=" << z;
            sPlayerbotAIConfig.logEvent(ai, "IdleRescueNudgeMove", "", out.str());
        }

        ResetIdleRescueNudgeFailures(bot);
        return true;
    }

    bool ForceConcreteTargetWork(PlayerbotAI* ai, Player* bot, Unit* target, char const* reason)
    {
        if (!ai || !bot || !target || !target->IsInWorld() || sServerFacade.UnitIsDead(target) ||
            sServerFacade.IsFriendlyTo(bot, target) || bot->IsNonMeleeSpellCasted(true, false, true))
            return false;

        bot->SetSelectionGuid(target->GetObjectGuid());
        bot->SetTarget(target);

        bool acted = ai->DoSpecificAction("attack anything",
            Event(reason ? reason : "force target work", "concrete target work", bot), true);

        const bool alreadyConcrete =
            bot->IsNonMeleeSpellCasted(true, false, true) ||
            IsChasingTarget(bot, target) ||
            (bot->GetVictim() == target &&
             bot->CanReachWithMeleeAutoAttack(target) &&
             bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING));

        if (!alreadyConcrete)
            acted = ForceCombatConnect(ai, bot, target, reason) || acted;

        if (!bot->IsNonMeleeSpellCasted(true, false, true) &&
            !bot->CanReachWithMeleeAutoAttack(target) &&
            !ai->IsRanged(bot))
        {
            if (MotionMaster* mm = bot->GetMotionMaster())
            {
                bot->InterruptMoving(true);
                mm->MoveChase(target);
                acted = true;
            }

            acted = ai->DoSpecificAction("reach melee",
                Event(reason ? reason : "force target work", "concrete target reach", bot), true) || acted;
        }

        if (bot->CanReachWithMeleeAutoAttack(target))
        {
            bot->Attack(target, true);
            bot->MeleeAttackStart(target);
            acted = true;
        }

        const bool concrete =
            bot->IsNonMeleeSpellCasted(true, false, true) ||
            IsChasingTarget(bot, target) ||
            (bot->GetVictim() == target && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING));

        if (sPlayerbotAIConfig.hasLog("bot_events.csv") && (!concrete || urand(1, 12) == 1))
        {
            std::ostringstream out;
            out << "reason=" << (reason ? reason : "force-target")
                << " target=" << target->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target)
                << " ranged=" << (ai->IsRanged(bot) ? 1 : 0)
                << " meleeReach=" << (bot->CanReachWithMeleeAutoAttack(target) ? 1 : 0)
                << " acted=" << (acted ? 1 : 0)
                << " moving=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
                << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0)
                << " victim=" << ((bot->GetVictim() == target) ? 1 : 0)
                << " concrete=" << (concrete ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, "ConcreteTargetWork", std::to_string(target->GetGUIDLow()), out.str());
        }

        return acted || concrete;
    }

    bool IsHostileCombatSpellForTarget(Player* bot, Unit* target, SpellEntry const* spellInfo)
    {
        if (!bot || !target || !spellInfo)
            return false;

        if (target == bot)
            return false;

        if (IsPositiveSpell(spellInfo))
            return false;

        if (sServerFacade.IsFriendlyTo(bot, target))
            return false;

        for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; ++i)
        {
            SpellEffectIndex effectIndex = static_cast<SpellEffectIndex>(i);
            switch (spellInfo->Effect[effectIndex])
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
                    return true;
                case SPELL_EFFECT_APPLY_AURA:
                    if (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
                        spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_LEECH)
                    {
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }

        return false;
    }

    bool ForceRangedPrimeChaseFallback(PlayerbotAI* ai, Player* bot, Unit* target, char const* reason)
    {
        if (!ai || !bot || !target || !target->IsInWorld() || sServerFacade.UnitIsDead(target))
            return false;

        if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning() ||
            bot->IsNonMeleeSpellCasted(true, false, true))
            return true;

        bot->SetSelectionGuid(target->GetObjectGuid());
        bot->SetTarget(target);
        const bool attackStarted = bot->Attack(target, true);
        const bool meleeReach = bot->CanReachWithMeleeAutoAttack(target);
        bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        bool meleeSwinging = false;

        if (meleeReach)
        {
            bot->MeleeAttackStart(target);
            meleeSwinging = bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
        }
        else
        {
            if (MotionMaster* mm = bot->GetMotionMaster())
            {
                mm->MoveChase(target);
                moving = true;
            }
        }

        const bool productive =
            moving ||
            meleeSwinging ||
            bot->IsNonMeleeSpellCasted(true, false, true) ||
            (attackStarted && bot->GetVictim() == target && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING));

        if (sPlayerbotAIConfig.hasLog("bot_events.csv") && (!productive || urand(1, 25) == 1))
        {
            std::ostringstream out;
            out << "target=" << target->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target)
                << " meleeReach=" << (meleeReach ? 1 : 0)
                << " attackStarted=" << (attackStarted ? 1 : 0)
                << " moving=" << (moving ? 1 : 0)
                << " meleeSwing=" << (meleeSwinging ? 1 : 0)
                << " victim=" << ((bot->GetVictim() == target) ? 1 : 0)
                << " productive=" << (productive ? 1 : 0)
                << " reason=" << (reason ? reason : "spell-prime-failed");
            sPlayerbotAIConfig.logEvent(ai, "RangedCombatPrimeFallbackChase", std::to_string(target->GetGUIDLow()), out.str());
        }

        return productive;
    }

    bool TryImmediateRangedCombatAction(PlayerbotAI* ai, Player* bot, Unit* target)
    {
        if (!ai || !bot || !target || !target->IsInWorld() || sServerFacade.UnitIsDead(target))
            return false;

        if (!bot->IsWithinLOSInMap(target, true))
            return false;

        const float combatReach = bot->GetCombinedCombatReach(target, false);
        const float distance = sServerFacade.GetDistance2d(bot, target);
        if (bot->getClass() == CLASS_HUNTER)
        {
            if (bot->CanReachWithMeleeAutoAttack(target))
            {
                if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
                    ai->StopMoving();

                bot->SetSelectionGuid(target->GetObjectGuid());
                bot->SetTarget(target);
                bot->Attack(target, true);
                bot->MeleeAttackStart(target);
                ai->DoSpecificAction("raptor strike", Event("ranged combat prime", "hunter melee connect", bot), true);

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 10) == 1)
                {
                    std::ostringstream out;
                    out << "target=" << target->GetName()
                        << " dist=" << std::fixed << std::setprecision(2) << distance
                        << " meleeSwing=" << (bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING) ? 1 : 0)
                        << " victim=" << ((bot->GetVictim() == target) ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(ai, "HunterMeleePrime", std::to_string(target->GetGUIDLow()), out.str());
                }

                return bot->GetVictim() == target && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
            }

            // Classic hunter dead zone: too far to melee but too close to shoot.
            // The configured "shoot" range is a preferred/max range, not the
            // minimum range, so keep this anchored near the real 8yd minimum.
            const float minShootDistance = 8.0f + combatReach;
            const bool meleeReach = false;
            const bool deadZone = !meleeReach && distance < minShootDistance;
            if (deadZone)
            {
                if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
                    return true;

                if (MotionMaster* mm = bot->GetMotionMaster())
                {
                    if (target->GetVictim() == bot && !target->IsRooted())
                    {
                        bot->Attack(target, true);
                        mm->MoveChase(target);
                    }
                    else
                    {
                        float dx = bot->GetPositionX() - target->GetPositionX();
                        float dy = bot->GetPositionY() - target->GetPositionY();
                        float len = std::sqrt(dx * dx + dy * dy);
                        if (len < 0.1f)
                        {
                            dx = std::cos(bot->GetOrientation());
                            dy = std::sin(bot->GetOrientation());
                            len = 1.0f;
                        }

                        const float step = std::min<float>(12.0f, std::max<float>(4.0f, minShootDistance + 2.0f - distance));
                        float x = bot->GetPositionX() + (dx / len) * step;
                        float y = bot->GetPositionY() + (dy / len) * step;
                        float z = bot->GetPositionZ();
                        if (!bot->GetMap() || !MaNGOS::IsValidMapCoord(x, y, z))
                            return false;

                        bot->UpdateAllowedPositionZ(x, y, z);
                        if (!MaNGOS::IsValidMapCoord(x, y, z))
                            return false;

#ifdef MANGOSBOT_ZERO
                        mm->MovePoint(bot->GetMapId(), x, y, z, MOVE_RUN_MODE, 0.0f, -10.0f);
#else
                        mm->MovePoint(bot->GetMapId(), Position(x, y, z, 0.0f), MOVE_RUN_MODE, 0.0f, -10.0f);
#endif
                    }

                    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                    {
                        std::ostringstream out;
                        out << "target=" << target->GetName()
                            << " dist=" << std::fixed << std::setprecision(2) << distance
                            << " minShoot=" << minShootDistance
                            << " meleeReach=" << (meleeReach ? 1 : 0)
                            << " victim=" << (target->GetVictim() == bot ? 1 : 0);
                        sPlayerbotAIConfig.logEvent(ai, "HunterDeadZoneReposition", std::to_string(target->GetGUIDLow()), out.str());
                    }

                    return true;
                }
            }
        }

        const float spellRange = ai->GetRange("spell") + sPlayerbotAIConfig.contactDistance;
        if (distance > spellRange)
            return false;

        const bool casterPrime =
            bot->getClass() == CLASS_MAGE ||
            bot->getClass() == CLASS_WARLOCK ||
            bot->getClass() == CLASS_PRIEST ||
            bot->getClass() == CLASS_DRUID ||
            bot->getClass() == CLASS_SHAMAN;
        if (casterPrime)
        {
            if (sServerFacade.isMoving(bot) && !bot->IsFalling())
            {
                ai->StopMoving();
                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "target=" << target->GetName()
                        << " dist=" << std::fixed << std::setprecision(2) << distance
                        << " reason=stop-to-cast";
                    sPlayerbotAIConfig.logEvent(ai, "RangedCombatPrimeSetup", std::to_string(target->GetGUIDLow()), out.str());
                }
            }

            if (!sServerFacade.IsInFront(bot, target, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
            {
                sServerFacade.SetFacingTo(bot, target, true);
                bot->SetInFront(target);
                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "target=" << target->GetName()
                        << " dist=" << std::fixed << std::setprecision(2) << distance
                        << " reason=face-target";
                    sPlayerbotAIConfig.logEvent(ai, "RangedCombatPrimeSetup", std::to_string(target->GetGUIDLow()), out.str());
                }
            }
        }

        Event event("ranged combat prime", "spell opener", bot);
        auto tryAction = [&](char const* action) -> bool
        {
            return action && *action && ai->DoSpecificAction(action, event, true);
        };

        bool openerStarted = false;
        switch (bot->getClass())
        {
            case CLASS_MAGE:
                openerStarted = tryAction("frostbolt") || tryAction("fireball") || tryAction("shoot");
                break;
            case CLASS_WARLOCK:
                openerStarted = tryAction("shadow bolt") || tryAction("shoot");
                break;
            case CLASS_PRIEST:
                openerStarted = tryAction("smite") || tryAction("shoot");
                break;
            case CLASS_DRUID:
                openerStarted = tryAction("wrath") || tryAction("shoot");
                break;
            case CLASS_SHAMAN:
                openerStarted = tryAction("lightning bolt") || tryAction("shoot");
                break;
            case CLASS_HUNTER:
                openerStarted = tryAction("auto shot") || tryAction("arcane shot") || tryAction("shoot");
                break;
            default:
                openerStarted = tryAction("shoot");
                break;
        }

        if (openerStarted)
        {
            const bool visibleCombatAction =
                bot->IsNonMeleeSpellCasted(true, false, true) ||
                sServerFacade.isMoving(bot) ||
                bot->isMovingOrTurning() ||
                (bot->GetVictim() == target &&
                 bot->CanReachWithMeleeAutoAttack(target) &&
                 bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING));

            if (visibleCombatAction)
                return true;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "target=" << target->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << distance
                    << " class=" << static_cast<uint32>(bot->getClass())
                    << " moving=0 casting=0 meleeSwing=0 combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0)
                    << " reason=opener-returned-without-visible-action";
                sPlayerbotAIConfig.logEvent(ai, "RangedCombatPrimeNoVisibleAction", std::to_string(target->GetGUIDLow()), out.str());
            }
        }

        return ForceRangedPrimeChaseFallback(ai, bot, target, "opener-failed");
    }

    bool ForceCombatConnect(PlayerbotAI* ai, Player* bot, Unit* target, char const* reason)
    {
        if (!ai || !bot || !target || !target->IsInWorld() || sServerFacade.UnitIsDead(target))
            return false;

        const bool rangedBot = ai->IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
        const bool inLos = bot->IsWithinLOSInMap(target, true);
        const float distance = sServerFacade.GetDistance2d(bot, target);
        const float spellRange = ai->GetRange("spell") + sPlayerbotAIConfig.contactDistance;
        const bool meleeReach = bot->CanReachWithMeleeAutoAttack(target);
        const bool spellReach = rangedBot && inLos && distance <= spellRange;

        bot->SetSelectionGuid(target->GetObjectGuid());
        bot->SetTarget(target);

        bool issued = false;
        if (ai->CanMove() && inLos)
        {
            sServerFacade.SetFacingTo(bot, target, true);
            bot->SetInFront(target);
        }

        if (meleeReach && (!rangedBot || bot->getClass() == CLASS_HUNTER))
        {
            if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
                ai->StopMoving();

            bot->Attack(target, true);
            bot->MeleeAttackStart(target);
            if (bot->getClass() == CLASS_HUNTER)
                ai->DoSpecificAction("raptor strike", Event(reason ? reason : "combat connect", "hunter melee connect", bot), true);
            issued = bot->GetVictim() == target && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
        }
        else if (rangedBot && spellReach)
        {
            if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
                ai->StopMoving();

            bot->Attack(target, false);
            issued = TryImmediateRangedCombatAction(ai, bot, target) ||
                bot->IsNonMeleeSpellCasted(true, false, true);
        }
        else if (meleeReach)
        {
            if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning())
                ai->StopMoving();

            bot->Attack(target, true);
            bot->MeleeAttackStart(target);
            issued = bot->GetVictim() == target && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
        }
        else
        {
            bot->Attack(target, true);
            if (MotionMaster* mm = bot->GetMotionMaster())
            {
                // Ranged bots close only to keep-away (flee) distance, not melee.
                if (rangedBot)
                    mm->MoveChase(target, ai->GetRange("flee"), bot->GetAngle(target));
                else
                    mm->MoveChase(target);
                issued = true;
            }
        }

        if (sPlayerbotAIConfig.hasLog("bot_events.csv") && (!issued || urand(1, 10) == 1))
        {
            std::ostringstream out;
            out << "target=" << target->GetName()
                << " dist=" << std::fixed << std::setprecision(2) << distance
                << " ranged=" << (rangedBot ? 1 : 0)
                << " los=" << (inLos ? 1 : 0)
                << " meleeReach=" << (meleeReach ? 1 : 0)
                << " spellReach=" << (spellReach ? 1 : 0)
                << " moving=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
                << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0)
                << " meleeSwing=" << (bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING) ? 1 : 0)
                << " victim=" << ((bot->GetVictim() == target) ? 1 : 0)
                << " issued=" << (issued ? 1 : 0)
                << " reason=" << (reason ? reason : "combat-stall");
            sPlayerbotAIConfig.logEvent(ai, "CombatConnectKick", std::to_string(target->GetGUIDLow()), out.str());
        }

        return issued;
    }

    bool TeleportGhostUnstuck(PlayerbotAI* ai, Player* bot, const WorldPosition& destination, const char* eventName, float radius = 4.0f)
    {
        if (!ai || !bot || !destination)
            return false;

        WorldPosition movePosition = destination;
        if (radius > 0.0f)
            movePosition.GetReachableRandomPointOnGround(bot, radius, urand(0, 1));

        if (!movePosition)
            movePosition = destination;

        bot->GetMotionMaster()->Clear();
        const bool teleported = bot->TeleportTo(movePosition.getMapId(), movePosition.getX(), movePosition.getY(), movePosition.getZ(), movePosition.getO(), 0);
        if (!teleported)
            return false;

        if (bot->isRealPlayer())
            bot->SendHeartBeat();

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "map=" << movePosition.getMapId()
                << " x=" << movePosition.getX()
                << " y=" << movePosition.getY()
                << " z=" << movePosition.getZ();
            sPlayerbotAIConfig.logEvent(ai, eventName, "", out.str());
        }

        return true;
    }

    bool ForceHostileSpellCombatPrime(Player* bot, Unit* target, SpellEntry const* spellInfo)
    {
        if (!bot || !target || !spellInfo)
            return false;

        if (!IsHostileCombatSpellForTarget(bot, target, spellInfo))
            return false;

        Creature* creatureTarget = target->ToCreature();
        if (!creatureTarget || !creatureTarget->IsAlive())
            return false;

        if (creatureTarget->GetVictim() == bot)
            return false;

        if (creatureTarget->IsInCombat())
            return false;

        creatureTarget->EnterCombatWithTarget(bot);
        bot->SetInCombatWith(creatureTarget);
        return true;
    }

    void LogSpellCastFailure(PlayerbotAI* ai, Player* bot, Unit* target, SpellEntry const* spellInfo,
        SpellCastResult result, char const* stage, bool castTimeSpell, bool selfPositiveCast)
    {
        if (!ai || !bot || !spellInfo || !sPlayerbotAIConfig.hasLog("bot_events.csv"))
            return;

        static std::mutex s_spellFailureTraceMutex;
        static std::map<std::string, time_t> s_spellFailureTraceNextLog;

        std::ostringstream key;
        key << bot->GetGUIDLow() << ':' << spellInfo->Id << ':' << static_cast<uint32>(result) << ':'
            << (stage ? stage : "unknown");

        time_t now = time(nullptr);
        {
            std::lock_guard<std::mutex> guard(s_spellFailureTraceMutex);
            time_t& nextLog = s_spellFailureTraceNextLog[key.str()];
            if (nextLog > now)
                return;

            nextLog = now + 5;
        }

        std::ostringstream out;
        out << "spell=" << spellInfo->SpellName[0]
            << " stage=" << (stage ? stage : "unknown")
            << " result=" << static_cast<uint32>(result)
            << " target=" << (target ? target->GetName() : "self")
            << " castTime=" << (castTimeSpell ? 1 : 0)
            << " selfPositive=" << (selfPositiveCast ? 1 : 0)
            << " moving=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
            << " falling=" << (bot->IsFalling() ? 1 : 0)
            << " combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0);

        if (target)
        {
            out << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target)
                << " los=" << (bot->IsWithinLOSInMap(target, true) ? 1 : 0);
        }

        sPlayerbotAIConfig.logEvent(ai, "SpellCastFailedTrace", std::to_string(spellInfo->Id), out.str());
    }

    bool IsQuestTravelDestination(TravelDestination* destination)
    {
        if (!destination)
            return false;

        switch (destination->GetPurpose())
        {
            case TravelDestinationPurpose::QuestGiver:
            case TravelDestinationPurpose::QuestTaker:
            case TravelDestinationPurpose::QuestObjective1:
            case TravelDestinationPurpose::QuestObjective2:
            case TravelDestinationPurpose::QuestObjective3:
            case TravelDestinationPurpose::QuestObjective4:
                return true;
            default:
                return false;
        }
    }

    uint32 GetRespawnCorpseSourceGuid(Creature* creature)
    {
        if (!creature)
            return 0;

        if (creature->IsLootCorpseClone() && creature->GetLootCorpseSourceGuidLow())
            return creature->GetLootCorpseSourceGuidLow();

        if (creature->GetDbGuid())
            return creature->GetDbGuid();

        return creature->GetGUIDLow();
    }

    uint32 CountRespawnCorpsesForSource(Creature* creature)
    {
        if (!creature || !creature->IsInWorld())
            return 0;

        uint32 sourceGuid = GetRespawnCorpseSourceGuid(creature);
        if (!sourceGuid)
            return 0;

        std::list<Creature*> nearbyCreatures;
        MaNGOS::AnyUnitInObjectRangeCheck check(creature, 3.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearbyCreatures, check);
        Cell::VisitAllObjects(creature, searcher, 3.0f);

        uint32 count = creature->IsCorpse() ? 1u : 0u;
        for (Creature* nearby : nearbyCreatures)
        {
            if (!nearby || nearby == creature || !nearby->IsCorpse())
                continue;

            if (GetRespawnCorpseSourceGuid(nearby) != sourceGuid)
                continue;

            ++count;
        }

        return count;
    }

    void CopyLootForRespawnCorpse(Creature* source, Creature* clone)
    {
        clone->loot.clear();
        clone->loot.m_personal = source->loot.m_personal;
        clone->loot.items = source->loot.items;
        clone->loot.gold = source->loot.gold;
        clone->loot.unlootedCount = source->loot.unlootedCount;
        clone->loot.groupLeaderGuid = source->loot.groupLeaderGuid;
        clone->loot.roundRobinPlayer = source->loot.roundRobinPlayer;
        clone->loot.loot_type = source->loot.loot_type;
        clone->loot.m_questItems = source->loot.m_questItems;
        clone->loot.SetTeam(static_cast<Team>(source->loot.GetTeam()));

        clone->lootForBody = source->lootForBody;
        clone->lootForSkin = source->lootForSkin;
        clone->lootForPickPocketed = source->lootForPickPocketed;
        clone->skinningForOthersTimer = source->skinningForOthersTimer;
        clone->lootForCreator = source->lootForCreator;
        clone->CopyLootAccessFrom(*source);
        clone->SetHealth(0);
        clone->SetUInt32Value(UNIT_FIELD_FLAGS, source->GetUInt32Value(UNIT_FIELD_FLAGS));
        clone->SetUInt32Value(UNIT_DYNAMIC_FLAGS, source->GetUInt32Value(UNIT_DYNAMIC_FLAGS));
        // Keep preserved corpses interactable even when the loot payload is empty so
        // body visibility and next-spawn timing stay independent.
        clone->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
    }

    bool CreateRespawnCorpseClone(PlayerbotAI* ai, Creature* creature, float x, float y, float z, float o,
        uint32 preservedCorpses, uint32 maxCorpses, char const* eventName, bool consumeOriginalCorpse)
    {
        CreatureInfo const* cinfo = creature ? creature->GetCreatureInfo() : nullptr;
        if (!ai || !creature || !cinfo || !creature->GetMap())
            return false;

        CreatureCreatePos corpsePos(creature->GetMap(), x, y, z, o);
        Creature* corpseClone = new Creature();
        if (!corpseClone->Create(creature->GetMap()->GenerateLocalLowGuid(cinfo->GetHighGuid()), corpsePos, cinfo, creature->GetEntry()))
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << creature->GetEntry()
                    << ",reason=create_failed"
                    << ",source=" << GetRespawnCorpseSourceGuid(creature)
                    << ",event=" << eventName;
                sPlayerbotAIConfig.logEvent(ai, "RespawnCorpseCloneSkip", creature->GetName(), out.str());
            }

            delete corpseClone;
            return false;
        }

        corpseClone->SetLootCorpseClone(true);
        corpseClone->SetLootCorpseSourceGuidLow(GetRespawnCorpseSourceGuid(creature));
        corpseClone->SetDeathState(CORPSE);
        corpseClone->SetRespawnDelay(0, true);
        corpseClone->SetRespawnTime(0);
        corpseClone->SetCorpseDecayTimer(std::max<uint32>(creature->GetCorpseDecayTimer(), 20 * IN_MILLISECONDS));
        CopyLootForRespawnCorpse(creature, corpseClone);
        creature->GetMap()->Add(corpseClone);

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << creature->GetEntry() << ",source=" << GetRespawnCorpseSourceGuid(creature)
                << ",preserved=" << (preservedCorpses + 1) << ",cap=" << maxCorpses
                << ",consume_original=" << (consumeOriginalCorpse ? 1 : 0);
            sPlayerbotAIConfig.logEvent(ai, eventName, creature->GetName(), out.str());
        }

        if (consumeOriginalCorpse)
        {
            creature->loot.clear();
            creature->lootForBody = false;
            creature->lootForSkin = false;
            creature->lootForPickPocketed = false;
            creature->SetLootRecipient(nullptr);
            creature->RemoveCorpse();
        }

        return true;
    }

    bool TryPreserveRespawnCorpse(PlayerbotAI* ai, Creature* creature)
    {
        if (!ai || !creature)
            return false;

        if (CountRespawnCorpsesForSource(creature) > 0 && (!creature->IsCorpse() || creature->IsAlive()))
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << creature->GetEntry()
                    << ",reason=already_preserved"
                    << ",death_state=" << creature->GetDeathState();
                sPlayerbotAIConfig.logEvent(ai, "RespawnCorpseCloneSkip", creature->GetName(), out.str());
            }
            return false;
        }

        // Accelerated respawn can hit while the creature is still in a just-died
        // transition state. Normalize that early so preservation does not depend
        // on which exact update tick we landed on.
        if (!creature->IsCorpse() && !creature->IsAlive())
            creature->SetDeathState(CORPSE);

        if (!creature->GetDbGuid())
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << creature->GetEntry()
                    << ",reason=no_dbguid"
                    << ",death_state=" << creature->GetDeathState();
                sPlayerbotAIConfig.logEvent(ai, "RespawnCorpseCloneSkip", creature->GetName(), out.str());
            }
            return false;
        }

        uint32 maxCorpses = sWorld.getConfig(CONFIG_UINT32_DYN_RESPAWN_CORPSE_CLONE_MAX);
        if (!maxCorpses)
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << creature->GetEntry()
                    << ",reason=cap_disabled";
                sPlayerbotAIConfig.logEvent(ai, "RespawnCorpseCloneSkip", creature->GetName(), out.str());
            }
            return false;
        }

        uint32 preservedCorpses = CountRespawnCorpsesForSource(creature);
        if (preservedCorpses >= maxCorpses)
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << creature->GetEntry() << ",source=" << GetRespawnCorpseSourceGuid(creature)
                    << ",preserved=" << preservedCorpses << ",cap=" << maxCorpses;
                sPlayerbotAIConfig.logEvent(ai, "RespawnCorpseCloneCap", creature->GetName(), out.str());
            }
            return false;
        }

        if (!creature->IsCorpse())
        {
            if (creature->IsAlive() && creature->GetLastDeathSnapshotAt())
            {
                time_t now = time(nullptr);
                if ((now - creature->GetLastDeathSnapshotAt()) <= 10)
                {
                    if (CreateRespawnCorpseClone(ai, creature,
                        creature->GetLastDeathPositionX(),
                        creature->GetLastDeathPositionY(),
                        creature->GetLastDeathPositionZ(),
                        creature->GetLastDeathOrientation(),
                        preservedCorpses, maxCorpses, "RespawnCorpseCloneRecovered", false))
                    {
                        return true;
                    }
                }
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << creature->GetEntry()
                    << ",reason=not_corpse"
                    << ",death_state=" << creature->GetDeathState()
                    << ",alive=" << (creature->IsAlive() ? 1 : 0)
                    << ",last_death_age_secs="
                    << (creature->GetLastDeathSnapshotAt() ? static_cast<long long>(time(nullptr) - creature->GetLastDeathSnapshotAt()) : -1);
                sPlayerbotAIConfig.logEvent(ai, "RespawnCorpseCloneSkip", creature->GetName(), out.str());
            }
            return false;
        }

        return CreateRespawnCorpseClone(ai, creature,
            creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), creature->GetOrientation(),
            preservedCorpses, maxCorpses, "RespawnCorpseClone", true);
    }
}

std::vector<std::string>& split(const std::string &s, char delim, std::vector<std::string> &elems);
std::vector<std::string> split(const std::string &s, char delim);
char * strstri (std::string str1, std::string str2);
uint64 extractGuid(WorldPacket& packet);
std::string &trim(std::string &s);

std::set<std::string> PlayerbotAI::unsecuredCommands;

uint32 PlayerbotChatHandler::extractQuestId(std::string str)
{
    char* source = (char*)str.c_str();
    char* cId = ExtractKeyFromLink(&source,"Hquest");
    return cId ? atol(cId) : 0;
}

void PacketHandlingHelper::AddHandler(uint16 opcode, std::string handler, bool shouldDelay)
{
    handlers[opcode] = handler;
    delay[opcode] = shouldDelay;
}

void PacketHandlingHelper::Handle(ExternalEventHelper &helper)
{
    if (!m_botPacketMutex.try_lock()) //Packets do not have to be handled now. Handle them later.
        return;

    // queue holds unique_ptr<WorldPacket> due to Penqle's move-only WorldPacket.
    std::stack<std::unique_ptr<WorldPacket>> delayed;

    while (!queue.empty())
    {
        if (!helper.HandlePacket(handlers, *queue.top()))
            if (delay[queue.top()->GetOpcode()])
                delayed.push(std::move(queue.top()));
        queue.pop();
    }

    queue = std::move(delayed);

    m_botPacketMutex.unlock();
}

void PacketHandlingHelper::AddPacket(const WorldPacket& packet)
{
    if (packet.empty() && packet.GetOpcode() != MSG_RAID_READY_CHECK)
        return;

    m_botPacketMutex.lock(); //We are going to add packets. Stop any new handling and add them.

	if (handlers.find(packet.GetOpcode()) != handlers.end())
        queue.push(std::make_unique<WorldPacket>(packet));

    m_botPacketMutex.unlock();
}

PlayerbotAI::PlayerbotAI() : PlayerbotAIBase(), bot(NULL), aiObjectContext(NULL),
    currentEngine(NULL), chatHelper(this), chatFilter(this), accountId(0), security(NULL), master(NULL), currentState(BotState::BOT_STATE_NON_COMBAT), faceTargetUpdateDelay(0), jumpTime(0), fallAfterJump(false)
{
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
        engines[i] = NULL;

    for (int i = 0; i < MAX_ACTIVITY_TYPE; i++)
    {
        allowActiveCheckTimer[i] = time(nullptr);
        allowActive[i] = false;
    }
}

PlayerbotAI::PlayerbotAI(Player* bot) :
    PlayerbotAIBase(), chatHelper(this), chatFilter(this), security(bot), master(NULL), faceTargetUpdateDelay(0), jumpTime(0), fallAfterJump(false)
{
    this->bot = bot;
    if (!bot->isTaxiCheater() && HasCheat(BotCheatMask::taxi))
        bot->SetTaxiCheater(true);

    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        engines[i] = NULL;

    for (int i = 0; i < MAX_ACTIVITY_TYPE; i++)
    {
        allowActiveCheckTimer[i] = time(nullptr);
        allowActive[i] = false;
    }

	accountId = sObjectMgr.GetPlayerAccountIdByGUID(bot->GetObjectGuid());

    aiObjectContext = AiFactory::createAiObjectContext(bot, this);

    UpdateTalentSpec();

    engines[(uint8)BotState::BOT_STATE_COMBAT] = AiFactory::createCombatEngine(bot, this, aiObjectContext);
    engines[(uint8)BotState::BOT_STATE_NON_COMBAT] = AiFactory::createNonCombatEngine(bot, this, aiObjectContext);
    engines[(uint8)BotState::BOT_STATE_DEAD] = AiFactory::createDeadEngine(bot, this, aiObjectContext);
    engines[(uint8)BotState::BOT_STATE_REACTION] = reactionEngine = AiFactory::createReactionEngine(bot, this, aiObjectContext);

    for (uint8 e = 0; e < (uint8)BotState::BOT_STATE_ALL; e++)
    {
        engines[e]->initMode = false;
        engines[e]->Init();
    }

    currentEngine = engines[(uint8)BotState::BOT_STATE_NON_COMBAT];
    currentState = BotState::BOT_STATE_NON_COMBAT;

    masterIncomingPacketHandlers.AddHandler(CMSG_GAMEOBJ_USE, "use game object");
    masterIncomingPacketHandlers.AddHandler(CMSG_AREATRIGGER, "area trigger");
    masterIncomingPacketHandlers.AddHandler(CMSG_LOOT_ROLL, "loot roll", true);
    masterIncomingPacketHandlers.AddHandler(CMSG_GOSSIP_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_COMPLETE_QUEST, "complete quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_ACCEPT_QUEST, "accept quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUEST_CONFIRM_ACCEPT, "confirm quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATETAXI, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATETAXIEXPRESS, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_TAXICLEARALLNODES, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_TAXICLEARNODE, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_GROUP_UNINVITE, "uninvite");
    masterIncomingPacketHandlers.AddHandler(CMSG_GROUP_UNINVITE_GUID, "uninvite guid");
    masterIncomingPacketHandlers.AddHandler(CMSG_PUSHQUESTTOPARTY, "quest share");
    masterIncomingPacketHandlers.AddHandler(CMSG_CAST_SPELL, "see spell");
    masterIncomingPacketHandlers.AddHandler(CMSG_REPOP_REQUEST, "release spirit");
    masterIncomingPacketHandlers.AddHandler(CMSG_RECLAIM_CORPSE, "revive from corpse");

#ifdef MANGOSBOT_TWO
    masterIncomingPacketHandlers.AddHandler(CMSG_LFG_TELEPORT, "lfg teleport");
#endif

    botOutgoingPacketHandlers.AddHandler(SMSG_PETITION_SHOW_SIGNATURES, "petition offer");
    botOutgoingPacketHandlers.AddHandler(SMSG_BATTLEFIELD_STATUS, "bg status");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_INVITE, "group invite");
    botOutgoingPacketHandlers.AddHandler(SMSG_GUILD_INVITE, "guild accept");
    botOutgoingPacketHandlers.AddHandler(BUY_ERR_NOT_ENOUGHT_MONEY, "not enough money");
    botOutgoingPacketHandlers.AddHandler(BUY_ERR_REPUTATION_REQUIRE, "not enough reputation");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_SET_LEADER, "group set leader");
    botOutgoingPacketHandlers.AddHandler(SMSG_FORCE_RUN_SPEED_CHANGE, "check mount state");
    botOutgoingPacketHandlers.AddHandler(SMSG_RESURRECT_REQUEST, "resurrect request");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "cannot equip");
    botOutgoingPacketHandlers.AddHandler(SMSG_TRADE_STATUS, "trade status");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_RESPONSE, "loot response", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_KILL, "quest update add kill", true);
#ifndef MANGOSBOT_TWO
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_ITEM, "quest update add item", true);
#else
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_ITEM_OBSOLETE, "quest update add item", true);
#endif
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_FAILED, "quest update failed", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_FAILEDTIMER, "quest update failed timer", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_COMPLETE, "quest update complete", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_ITEM_PUSH_RESULT, "item push result", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
    botOutgoingPacketHandlers.AddHandler(SMSG_LEVELUP_INFO, "levelup", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_LOG_XPGAIN, "xpgain", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_PVP_CREDIT, "honorgain", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_TEXT_EMOTE, "receive text emote");
    botOutgoingPacketHandlers.AddHandler(SMSG_EMOTE, "receive emote");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_START_ROLL, "loot start roll", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_SUMMON_REQUEST, "summon request");
    botOutgoingPacketHandlers.AddHandler(MSG_RAID_READY_CHECK, "ready check");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUEST_CONFIRM_ACCEPT, "confirm quest");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTGIVER_QUEST_DETAILS, "quest details");   

#ifndef MANGOSBOT_ZERO
    botOutgoingPacketHandlers.AddHandler(SMSG_ARENA_TEAM_INVITE, "arena team invite");
#endif
#ifdef MANGOSBOT_TWO
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_ROLE_CHECK_UPDATE, "lfg role check");
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_PROPOSAL_UPDATE, "lfg proposal");
#endif

    botOutgoingPacketHandlers.AddHandler(SMSG_CAST_RESULT, "cast failed");
    botOutgoingPacketHandlers.AddHandler(SMSG_DUEL_REQUESTED, "duel requested");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "inventory change failure");

    masterOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
#ifndef MANGOSBOT_ZERO
    masterOutgoingPacketHandlers.AddHandler(MSG_RAID_READY_CHECK_FINISHED, "ready check finished");
#endif

    if (!HasRealPlayerMaster() && bot->GetFreeTalentPoints() > 0)
    {
        DoSpecificAction("auto talents");
    }
}

PlayerbotAI::~PlayerbotAI()
{
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        if (engines[i])
            delete engines[i];
    }

    if (aiObjectContext)
        delete aiObjectContext;
}

// ===== 10K SCALABILITY (Phase A) ============================================================
// The full supervisor sweep, extracted so both the fast path (1 Hz) and the full path (per-tick
// while fighting, 1 Hz otherwise) share it. Every helper keeps its own internal per-bot
// throttle; this gate only removes the 16 call+mutex+map-lookup overheads from the ~6/sec
// not-due brain calls that dominate at 1000+ bots.
static void RunSupervisorHelpers(PlayerbotAI* ai, Player* bot)
{
    // Dormant-tier bots are parked by design: no rescues, no anti-idle prodding (forcing an
    // invisible bot to act recreates the invisible-aggro bug), and no supervisor.csv rows
    // (a 9k dormant tier would otherwise swamp the active fleet's idle stats).
    if (ai->IsColdDormant())
        return;
    SupervisorTrack(ai, bot);
    FreezeNudge(ai, bot);              // physically dislodge bots pathfinding can't move (travel/flee/corpse)
    AntiIdleAction(ai, bot);           // force standing bots to grind/roam instead of looping no-op decisions
    HopelessFightBreaker(ai, bot);     // give up on unkillable targets (training dummies, evade mobs)
    GhostRescue(ai, bot);              // stuck ghost -> teleport to corpse (invisible to living players)
    AutoRepair(ai, bot);               // broken gear = death spiral; repair (paid when affordable)
    AutoAmmo(ai, bot);                 // hunters shoot dry and never restock; top up (invisible)
    LftMasterCleanup(ai, bot);         // released LFT fills return to the fleet
    AbandonedRunRescue(ai, bot);       // logout-abandoned fills + hostile-capital corpses go home
    InstanceStrandRescue(ai, bot);     // fills outside the master's instance get pulled back in
    TravelScheduleRescue(ai, bot);     // Erielah-class: live travel target, Execute never scheduled
    PreSelectNextTarget(ai, bot);      // decide the NEXT pull while the current victim dies
    ChainPullNext(ai, bot);            // kill -> loot -> next pull with no re-think dead-air
    NoProgressGrindFallback(ai, bot);  // 10 min w/o xp -> stop wandering, grind the local area
    LootUnderFire(ai, bot);            // group-combat but personally safe -> grab the corpse now
    QuestLoopBreaker(ai, bot);         // 6+ kills of the same quest mob w/o the drop -> do other quests
    // 1/sec gravity sweep for STATIONARY bots: catches every stop path that isn't StopMoving
    // (motion-clears, chase-holds) so no bot stands floating above the ground.
    if (!sServerFacade.isMoving(bot))
        SettleToGround(bot);
}

// Fleet-wide brain service-interval histogram: delta between consecutive UpdateAIInternal
// executions per bot. This is the acceptance metric for the 10k-bot mandate ("a decision every
// 20 seconds is bad"): the goal is p95 <= ~1s for active bots at any fleet size. Dumped to
// logs/brain_interval.csv once a minute by whichever brain crosses the boundary.
static std::atomic<uint32> s_brainIntervalBuckets[7] = {}; // <250ms <500 <1s <2s <5s <10s >=10s
static std::atomic<uint64> s_brainExecs{0};
static std::atomic<uint64> s_fastPathSkips{0};
static std::atomic<uint32> s_brainDumpDueMs{0};
static std::mutex s_brainDumpMx;

static void RecordBrainServiceInterval(uint32 nowMs, uint32& lastExecMs)
{
    if (lastExecMs)
    {
        const uint32 d = nowMs - lastExecMs;
        const int b = d < 250 ? 0 : d < 500 ? 1 : d < 1000 ? 2 : d < 2000 ? 3 : d < 5000 ? 4 : d < 10000 ? 5 : 6;
        s_brainIntervalBuckets[b].fetch_add(1, std::memory_order_relaxed);
    }
    lastExecMs = nowMs;
    s_brainExecs.fetch_add(1, std::memory_order_relaxed);

    if (nowMs >= s_brainDumpDueMs.load(std::memory_order_relaxed) && s_brainDumpMx.try_lock())
    {
        if (nowMs >= s_brainDumpDueMs.load(std::memory_order_relaxed))
        {
            s_brainDumpDueMs.store(nowMs + 60000, std::memory_order_relaxed);
            FILE* f = fopen("logs/brain_interval.csv", "a");
            if (!f) f = fopen("../logs/brain_interval.csv", "a");
            if (f)
            {
                time_t tt = time(0); struct tm tmv; localtime_r(&tt, &tmv);
                char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
                fprintf(f, "%s,execs=%llu,fastskips=%llu,lt250=%u,lt500=%u,lt1s=%u,lt2s=%u,lt5s=%u,lt10s=%u,ge10s=%u\n",
                    ts,
                    (unsigned long long)s_brainExecs.exchange(0),
                    (unsigned long long)s_fastPathSkips.exchange(0),
                    s_brainIntervalBuckets[0].exchange(0), s_brainIntervalBuckets[1].exchange(0),
                    s_brainIntervalBuckets[2].exchange(0), s_brainIntervalBuckets[3].exchange(0),
                    s_brainIntervalBuckets[4].exchange(0), s_brainIntervalBuckets[5].exchange(0),
                    s_brainIntervalBuckets[6].exchange(0));
                fclose(f);
            }
        }
        s_brainDumpMx.unlock();
    }
}

void PlayerbotAI::UpdateAI(uint32 elapsed, bool minimal)
{
    // ===== 10K FAST PATH (APM scalability, Phase A) =====
    // At 1000 bots ~6 of every 7 UpdateAI calls arrive while the bot's think-delay is still
    // counting down, yet each paid the full entry cost (WorldPosition + string alloc, perf
    // monitor, telemetry, 16 supervisor scans) before reaching the delay check -- measured
    // ~0.4 ms/call, saturating the two continent threads and collapsing per-bot APM as the
    // fleet grows. A not-due bot with nothing needing an instant reaction pays only the
    // countdown (plus the 1 Hz supervisor cadence) here. Every state that needs the full
    // body -- combat entry/exit wake+preempt, a master (real-player fills), jump landing,
    // transport, teleport, open loot, logout, cheats, dormancy, action-log debug -- falls
    // through to the full path below.
    // DORMANT FAST LANE: a cold-dormant bot (10k two-tier fleet: parked, invisible, synthetic
    // progression) needs only its countdown and a 1 Hz wake probe -- nothing else. This is what
    // makes the dormant tier ~free per map tick: no supervisor scans, no telemetry, no perf
    // monitor, no facing/reaction machinery.
    if (coldDormant && !master && bot->IsInWorld())
    {
        MapManager::SetContinentUpdatePhase("bot-ai-dormant", bot->GetGUIDLow());
        if (aiInternalUpdateDelay > elapsed) aiInternalUpdateDelay -= elapsed;
        else { aiInternalUpdateDelay = 0; isWaiting = false; }
        const uint32 dNow = WorldTimer::getMSTime();
        if (dNow >= supervisorHelperNextMs)
        {
            supervisorHelperNextMs = dNow + 1000;
            const bool wake = bot->IsInCombat() || !bot->getAttackers().empty()
                || sRandomPlayerbotMgr.IsActiveCohort(bot->GetGUIDLow())
                || IsInstancedContent();
            if (wake)
            {
                SetColdDormant(false);
                ResetAIInternalUpdateDelay();   // think NOW -- wake is next map tick
            }
        }
        return;
    }

    // Cheats that only matter on a per-tick basis (continuous stat pinning). The fleet default
    // mask "taxi,item,breath" must NOT veto the fast path: taxi never runs in UpdateAI, and
    // item (ammo restock) + breath are idempotent and fire plenty often on brain-due ticks.
    const uint32 perTickCheats = (uint32)BotCheatMask::health | (uint32)BotCheatMask::mana
        | (uint32)BotCheatMask::power | (uint32)BotCheatMask::cooldown | (uint32)BotCheatMask::movespeed;

    if (sPlayerbotAIConfig.fastPathEnabled
        && !bot->IsInCombat() && !inCombat && bot->getAttackers().empty()
        && aiInternalUpdateDelay >= elapsed + 100U   // stays not-due even after this decrement
        && !master && !jumpTime
        && !bot->IsBeingTeleported() && !bot->GetTransport() && !isMovingToTransport
        && !bot->GetLootGuid()
        && !bot->IsStunnedByLogout() && !bot->GetSession()->isLogingOut()
        && (((uint32)GetCheat() | (uint32)sPlayerbotAIConfig.botCheatMask) & perTickCheats) == 0
        && !ai::botdiag::IsActionLogEnabled())
    {
        MapManager::SetContinentUpdatePhase("bot-ai-fast", bot->GetGUIDLow());
        aiInternalUpdateDelay -= elapsed;
        s_fastPathSkips.fetch_add(1, std::memory_order_relaxed);
        if (sPlayerbotAIConfig.supervisorMode)
        {
            const uint32 fpNow = WorldTimer::getMSTime();
            if (fpNow >= supervisorHelperNextMs)
            {
                supervisorHelperNextMs = fpNow + 1000;
                RunSupervisorHelpers(this, bot);
            }
        }
        return;
    }

    AiObjectContext* context = aiObjectContext;
    std::string mapString = WorldPosition(bot).isInstance() ? "I" : std::to_string(bot->GetMapId());
    auto pmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAI " + mapString, nullptr, bot->GetMapId(), bot->GetInstanceId());

    SC_PHASE("UpdateAI.entry", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-ai-entry", bot ? bot->GetGUIDLow() : 0);
    sBotLearningMgr.RecordBotTelemetry(this, elapsed);

    // revalidate the
    // cached master pointer against ObjectAccessor BEFORE any code
    // path can deref it. If the master Player was destroyed since
    // SetMaster() was called (typical when master logs out /
    // disconnects), FindPlayer returns nullptr or a different live
    // pointer. We null `master` defensively. The masterGuid shadow
    // (set in SetMaster, see PlayerbotAI.h) is the safe lookup key —
    // we never deref the cached `master` pointer in this check.
    RevalidateMasterPointer();

    // DEBUG: idle near-player bots narrate their "thoughts" in /say so you can diagnose pauses in-game.
    if (ai::botdiag::IsActionLogEnabled())
        DebugSayIdle(this, bot);

    // SUPERVISOR MODE: full sweep per-tick while fighting (chain-pull, next-target and
    // loot-under-fire need combat cadence), 1 Hz otherwise (every helper self-throttles to
    // >=1s per bot internally anyway, so nothing loses cadence -- only call overhead).
    if (sPlayerbotAIConfig.supervisorMode)
    {
        const uint32 supNow = WorldTimer::getMSTime();
        if (bot->IsInCombat() || !bot->getAttackers().empty() || supNow >= supervisorHelperNextMs)
        {
            supervisorHelperNextMs = supNow + 1000;
            RunSupervisorHelpers(this, bot);
        }
    }

    // ===== LOD COLD DORMANCY (the scale lever for 10-15k bots) =====
    // A random-fleet bot with NO real player within lodColdRange goes DORMANT: SetColdDormant
    // drops its combat + makes it VISIBILITY_OFF, and its ENTIRE AI is SKIPPED here (truly idle).
    // Critically it does NOT run its grind/combat engine while invisible -- an invisible bot's
    // pull-cast can't make a mob aggro (Spell IsVisibleForOrDetect), which is the exact bug that
    // got this feature hard-disabled before; skipping all AI when dormant avoids it. The bot wakes
    // the instant a real player comes within lodColdRange. HasPlayerNearby only iterates the handful
    // of online real players (cheap even at 15k bots) -- the expensive DoNextAction is what we skip,
    // so we only pay full AI cost for bots a player can actually see. Instant rollback:
    // AiPlayerbot.LODColdUpdateMs = 0 in bin/aiplayerbot.conf + restart.
    bool parkEligible = false;
    if (sPlayerbotAIConfig.lodColdUpdateMs && bot->IsInWorld() && !HasRealPlayerMaster()
        && sRandomPlayerbotMgr.IsRandomBot(bot)
        && !IsInstancedContent()                                    // instanced bots never park
        && !bot->InBattleGroundQueue())                              // queued bots stay awake to process the BG invite
    {
        const uint32 nowPark = WorldTimer::getMSTime();
        if (sRandomPlayerbotMgr.IsActiveCohort(bot->GetGUIDLow()))
            lastActiveCohortMs = nowPark;                           // active cohort never parks
        // GROUP COHERENCE: a party member whose LEADER is in the active cohort stays awake too, so it
        // can FOLLOW instead of parking and drifting away (measured: only 33% of grouped members stayed
        // near their leader because followers parked while the leader ran on). Keeps the whole visible
        // party together. Bounded: only near-a-player parties (their leader is active) stay awake.
        else if (bot->GetGroup() && !IsGroupLeader()
                 && sRandomPlayerbotMgr.IsActiveCohort(bot->GetGroup()->GetLeaderGuid().GetCounter()))
            lastActiveCohortMs = nowPark;
        // TRANSITION HYSTERESIS: a bot that just dropped out of the active set (player walked
        // away / turned around) keeps its full brain for a 10s grace window before parking --
        // no flicker at the render-range boundary, and a returning player finds it mid-task
        // instead of mid-wakeup.
        else if (!lastActiveCohortMs || nowPark - lastActiveCohortMs > 10000)
            parkEligible = true;
    }
    if (parkEligible)
    {
        // NEVER go dormant while in a fight: SetColdDormant does CombatStop, so dormant-ing a
        // bot mid-combat makes it "stop attacking and stand there eating hits" -- exactly the
        // symptom seen as a player moves and bots at the trailing edge of the lodColdRange bubble
        // get put to sleep mid-fight. A bot that is fighting or being attacked STAYS AWAKE until
        // the fight ends, then goes dormant. (A dormant bot is invisible so nothing attacks it ->
        // it never enters this branch unless it was already awake + engaged.)
        const bool fighting = bot->IsInCombat() || !bot->getAttackers().empty();
        if (!fighting)   // out of the brain budget -> park (allocator owns proximity now)
        {
            if (!coldDormant)
                SetColdDormant(true);
            return;                 // dormant -> skip ALL AI this tick
        }
        else if (coldDormant)
        {
            SetColdDormant(false);  // player near OR pulled into a fight -> wake / stay awake
        }
    }

    // DUNGEON/RAID ENTRANCE LOITERER: an awake bot assigned this "actor role" holds its post at the
    // entrance (paces / social-emotes / some walk in and zone in) INSTEAD of running grind/travel AI,
    // so a passing player sees a lively group-forming crowd. (Parked far-away loiterers never reach
    // here -- they stand frozen at the portal, which is exactly what we want until a player shows up.)
    if (sRandomPlayerbotMgr.IsDungeonLoiterer(bot->GetGUIDLow())
        && !bot->IsInCombat() && bot->getAttackers().empty())
    {
        DungeonLoiterController(this, bot);
        return;
    }
    // A loiterer that is fighting / being attacked does NOT get held at its post -- it falls through
    // to the full combat AI below and kills what's on it, then resumes loitering when combat ends.

    // COMBAT PREEMPT: a bot that is in combat or is being attacked must NOT stay parked on a long
    // non-combat wait. Travel/camp/move actions set multi-second WaitForReach delays, and the
    // early-return below blocks the bot's whole brain -- including combat reactions -- until that
    // delay elapses. That is the visible "stands there with a mob on it, doesn't attack for 20-30s
    // / freezes when a 2nd mob joins after the first dies" bug: the bot is mid non-combat action and
    // can't react. CAP the wait to one global cooldown so it re-evaluates and fights within ~0.5s
    // (not zero -> avoids every-frame churn / extra bot-AI execution that drives the tick + crashes).
    // CAST COMMITMENT: do NOT shrink the delay while the bot is mid-cast on a cast-time spell.
    // A successful cast set the delay to cover its full cast duration (CastSpell ->
    // GetSpellCastDuration); shrinking it back to GCD here re-runs the strategy engine MID-CAST,
    // and the engine then picks a chase/move/target action that CANCELS the in-flight heal/cast.
    // The heal never lands -> HP stays low -> the heal trigger re-fires -> the bot recasts the
    // SAME heal in a tight loop (measured: recast_pct ~80%, the user's "constantly recasts its
    // own heal" symptom). Let the cast finish. EXCEPTION: at critical HP allow the preempt so a
    // dying bot can still react/flee instead of finishing a doomed cast.
    // Use the AUTHORITATIVE "am I casting a cast-time/channeled non-melee spell" check (the same
    // one the movement guards use), NOT a single SPELL_STATE: a cast-time heal sits in PREPARING
    // during its cast bar, so the earlier narrow SPELL_STATE_CASTING test missed it -> the preempt
    // kept shrinking the delay -> the engine re-ran mid-cast and canceled the heal.
    const bool castCommitting = bot->IsNonMeleeSpellCasted(true, false, true);
    // Is the in-progress cast a HEAL? A heal at low HP is the bot's SURVIVAL action -- it MUST
    // finish, never bail. The heal_cancel.csv trace proved the old unconditional critical-HP bail
    // was canceling the very heal keeping the bot alive: paladins at 2-16% HP looped Holy Light,
    // the bail re-enabled the preempt -> the engine re-ran mid-cast -> 'select new target' (rel 90)
    // canceled the heal every ~0.5s until the bot died. So only allow a critical-HP bail when the
    // in-flight cast is NOT a heal (e.g. a doomed offensive cast worth abandoning to flee).
    bool castingHeal = false;
    if (Spell* gcast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        castingHeal = gcast->m_spellInfo && IsHealSpell(gcast->m_spellInfo);
    const bool criticalHpBail = (bot->GetHealthPercent() < 25.0f) && !castingHeal;
    if (bot && bot->IsInWorld() && (bot->IsInCombat() || !bot->getAttackers().empty()) &&
        aiInternalUpdateDelay > sPlayerbotAIConfig.globalCoolDown &&
        (!castCommitting || criticalHpBail))
    {
        aiInternalUpdateDelay = sPlayerbotAIConfig.globalCoolDown;
    }

    if(aiInternalUpdateDelay > elapsed)
    {
        aiInternalUpdateDelay -= elapsed;
    }
    else
    {
        aiInternalUpdateDelay = 0;
        isWaiting = false;
    }

    // LOD fast wake: a dormant (COLD) bot must become visible/active the INSTANT a real
    // player comes within range, not after its slow ~15s brain tick. Cheap: short-circuits
    // on an empty server and only scans the handful of real players.
    if (coldDormant && bot && bot->IsInWorld() && !sRandomPlayerbotMgr.GetRealPlayerSnapshot()->players.empty())
    {
        const bool nearPlayer = HasPlayerNearby(sPlayerbotAIConfig.lodColdRange);
        // TEMP COLDDBG: sample whether dormant bots see the online player + distance.
        static std::atomic<uint32> dbgCtr{0};
        if ((dbgCtr++ % 150) == 0)
            sLog.outError("COLDDBG dormant bot=%s map=%u players=%u near=%d range=%.0f",
                bot->GetName(), bot->GetMapId(), (uint32)sRandomPlayerbotMgr.GetRealPlayerSnapshot()->players.size(),
                nearPlayer ? 1 : 0, sPlayerbotAIConfig.lodColdRange);
        if (nearPlayer)
        {
            sLog.outError("COLDWAKE bot=%s woke (player within %.0f)", bot->GetName(), sPlayerbotAIConfig.lodColdRange);
            SetColdDormant(false);
            ResetAIInternalUpdateDelay();   // think now, don't wait out the cold delay
        }
    }

    // ===== BG WAYPOINT NAVIGATION (WSG) =====
    // Replaces straight-line forced movement (which clipped through walls/ground). Bots now
    // follow the hand-traced waypoint graph -- out the tunnels, around the flag-room buildings,
    // across mid-field, down the one-way graveyard cliff-jumps -- via Playerbot_WsgNavigate().
    // This block only handles TRAVEL when the bot is idle+safe; combat (target chase via
    // MoveChase) and flag pickup/cap are the engine's job when an enemy/flag is close.
    if (bot->InBattleGround() && bot->IsAlive() && sRandomPlayerbotMgr.IsRandomBot(bot)
        && !HasRealPlayerMaster())
    {
        BattleGround* wsg = bot->GetBattleGround();
        if (wsg && wsg->GetTypeId() == BATTLEGROUND_WS && wsg->GetStatus() == STATUS_IN_PROGRESS
            && !bot->IsNonMeleeSpellCasted(true, false, true) && !bot->IsBeingTeleported()
            && !bot->IsInCombat() && bot->getAttackers().empty() && !bot->IsMoving())
        {
            // enemy within engage range -> DON'T travel; hand to combat (engine + MoveChase fight)
            bool enemyNear = false;
            for (auto const& pr : wsg->GetPlayers())
            {
                Player* op = sObjectMgr.GetPlayer(pr.first);
                if (!op || !op->IsInWorld() || !op->IsAlive() || op->GetTeam() == bot->GetTeam())
                    continue;
                if (bot->GetDistance(op) <= 15.0f) { enemyNear = true; break; }
            }
            if (!enemyNear)
                Playerbot_WsgNavigate(bot);   // one traced-route leg toward the objective
        }
    }

        // HUMAN-LIKE IDLE JUMPS: real players hop around while standing at the AH or waiting on a
    // boat, not only mid-travel (the MoveTo path covers running jumps). Rolls every 4-12s per
    // bot, scaled by HumanLikeJumpChance * per-bot jumpiness. Never during a cast/channel,
    // combat, sitting (eating/drinking), looting, taxi or teleport -- nothing that would
    // interrupt what the bot is doing. Only bothers when a real player can actually see it.
    if (sPlayerbotAIConfig.humanLikeJumpChance > 0.0f && !coldDormant && bot->IsInWorld())
    {
        const uint32 nowJump = WorldTimer::getMSTime();
        if (nowJump >= nextIdleJumpMs)
        {
            nextIdleJumpMs = nowJump + urand(4000, 12000);
            if (bot->IsAlive() && !bot->IsInCombat() && bot->getAttackers().empty()
                && !bot->IsNonMeleeSpellCasted(true, false, true)
                && bot->GetStandState() == UNIT_STAND_STATE_STAND
                && !bot->IsTaxiFlying() && !bot->IsBeingTeleported()
                && !bot->GetLootGuid()
                && !IsJumping()
                && HasPlayerNearby()
                && frand(0.0f, 1.0f) < sPlayerbotAIConfig.humanLikeJumpChance * GetJumpiness() * 0.5f)
            {
                DoSpecificAction("jump::random", Event(), true);
            }
        }
    }

    // cancel logout in combat
    if (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut())
    {
        if (sServerFacade.IsInCombat(bot) || (master && sServerFacade.IsInCombat(master) && sServerFacade.GetDistance2d(bot, master) < 30.0f))
        {
            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            TellPlayer(GetMaster(), BOT_TEXT("logout_cancel"));
        }
    }

    // Leontiesh - fix movement desync
    bool botMoving = false;
    if (!bot->IsStopped() || bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE) 
        botMoving = true;
    if (!bot->GetMotionMaster()->empty())
        if (MovementGenerator* movgen = bot->GetMotionMaster()->top())
            botMoving = true;


    if (botMoving && !bot->IsBeingTeleported() && bot->IsInWorld())
    {
        isMoving = true;

        GuidPosition lootObject(bot->GetLootGuid(), bot);

        bool shouldRelease = true;

        if (lootObject)
        {
            if (lootObject.IsGameObject() && lootObject.GetGameObjectInfo()->type == GAMEOBJECT_TYPE_FISHINGNODE)
                shouldRelease = false;
            else if (lootObject.GetWorldObject(bot->GetInstanceId()) && bot->GetDistance(lootObject.GetWorldObject(bot->GetInstanceId())) < INTERACTION_DISTANCE)
                shouldRelease = false;

            
            if (Loot* loot = sLootMgr.GetLoot(bot, bot->GetLootGuid()))
            {
                if (lootObject.IsUnit())
                {
                    Unit* unitTarget = (Unit*)loot->GetLootTarget();
                    // Unit doesn't have m_loot; only Creature does.
                    Loot* utLoot = (unitTarget && unitTarget->GetTypeId() == TYPEID_UNIT) ? ((Creature*)unitTarget)->m_loot : nullptr;
                    // LootAccess wraps Loot* now (no more reinterpret_cast layout-cheat).
                    LootAccess lootAccess(utLoot);
                    if (utLoot && lootAccess.playersLooting().count(bot->GetObjectGuid()) == 0)
                    {
                        // You shouldn't release if you aren't looting already
                        shouldRelease = false;
                    }
                }
                else if (lootObject.IsGameObject())
                {
                    GameObject* gameobjectTarget = (GameObject*)loot->GetLootTarget();
                    // LootAccess wraps Loot* now.
                    LootAccess lootAccess(gameobjectTarget ? gameobjectTarget->m_loot : nullptr);
                    if (!lootAccess.playersLooting().count(bot->GetObjectGuid()))
                    {
                        // You shouldn't release if you aren't looting already
                        shouldRelease = false;
                    }
                }
                else
                {
                    // If it's not a unit or GO, what looting would this be?
                    // safe to assume you probably shouldn't release
                    shouldRelease = false;
                }
                // release loot if moving and far from object.
                if (shouldRelease)
                {
                    loot->Release(bot);
                }
            }
        }
    }
    else if (isMoving)
    {
        if (!bot->IsTaxiFlying())
            StopMoving();

        isMoving = false;
    }

#ifdef MANGOSBOT_TWO
    if (bot->IsPendingDismount())
        bot->ResolvePendingUnmount();
    else
        bot->ResolvePendingMount();
#endif

    // wake up if in combat
    bool isCasting = bot->IsNonMeleeSpellCasted(true);
    if (sServerFacade.IsInCombat(bot))
    {
        if (!inCombat && !isCasting && !isWaiting)
        {
            ResetAIInternalUpdateDelay();
        }
        else if (!AllowActivity())
        {
            if (AllowActivity(ALL_ACTIVITY, true))
                ResetAIInternalUpdateDelay();
        }

        inCombat = true;
    }
    else
    {
        if (inCombat && !isCasting && !isWaiting)
            ResetAIInternalUpdateDelay();

        inCombat = false;
    }

    // force stop if moving but should not
#ifndef MANGOSBOT_TWO
    if (!bot->IsStopped() && !IsJumping() && !CanMove() && !bot->IsTaxiFlying() && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING) && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED))
#else
    if (!bot->IsStopped() && !IsJumping() && !CanMove() && !bot->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING) && !bot->IsTaxiFlying() && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING) && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED))
#endif
    {
        StopMoving();
    }

    // land after knockback/jump
    uint32 curTime = sWorld.GetCurrentMSTime();
    if (jumpTime && (jumpTime < curTime || (jumpTime + 10000 < curTime)))
    {
        // might be not needed
        if (GetJumpDestination())
        {
            bot->Relocate(jumpDestination.getX(), jumpDestination.getY(), jumpDestination.getZ());
        }

        // normal landing
        if (!fallAfterJump)
        {
            bot->m_movementInfo.AddMovementFlag(MOVEFLAG_FALLINGFAR);

            WorldPacket stop(MSG_MOVE_STOP);
            stop << bot->GetObjectGuid().WriteAsPacked();   // packed GUID first (all versions)
            stop << bot->m_movementInfo;
            bot->SendMessageToSetExcept(stop, bot);

            bot->m_movementInfo.SetMovementFlags(MOVEFLAG_NONE);
            bot->m_movementInfo.jump = MovementInfo::JumpInfo();

            WorldPacket land(MSG_MOVE_FALL_LAND);
            land << bot->GetObjectGuid().WriteAsPacked();   // packed GUID first (all versions)
            land << bot->m_movementInfo;
            bot->SendMessageToSetExcept(land, bot);
            sLog.outDetail("%s: Jump: Landed, landTime: %u", bot->GetName(), curTime);

            jumpTime = 0;
            fallAfterJump = false;
            ResetJumpDestination();

            bot->InterruptMoving();
        }
        // falling after hitting something
        else
        {
            //bot->SetFallInformation(0, bot->m_movementInfo.pos.z);
#ifdef MANGOSBOT_ZERO
            bot->m_movementInfo.AddMovementFlag(MOVEFLAG_JUMPING);
#else
            bot->m_movementInfo.AddMovementFlag(MOVEFLAG_FALLING);
#endif
            // use motion master (disabled for now, makes bot move to ceiling it just hit)
            static bool useMoveFall = false;
            if (useMoveFall && bot->GetMotionMaster()->MoveFall())
            {
                jumpTime = 0;
                fallAfterJump = false;
                ResetJumpDestination();
                sLog.outDetail("%s: Jump: MoveFall activated", bot->GetName());
            }
            // simulate falling
            else
            {
                float landingHeight = bot->m_movementInfo.pos.z;
                bot->UpdateAllowedPositionZ(bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, landingHeight);

                // calculate fall time
                float gravity = 19.2911f;
                float terminalVelocity = 60.148f;
                float time;

                const float terminal_length = float(terminalVelocity* terminalVelocity) / (2.f* gravity);
                const float terminalFallTime = float(terminalVelocity / gravity);

                float path_length = fabs(bot->m_movementInfo.pos.z - landingHeight);
                if (path_length >= terminal_length)
                    time = (path_length - terminal_length) / terminalVelocity + terminalFallTime;
                else
                    time = sqrtf(2.f * path_length / gravity);

                SetJumpTime(curTime + uint32(time * static_cast<uint32>(IN_MILLISECONDS)) + 1000);
                fallAfterJump = false;
                jumpDestination = WorldPosition(bot->GetMapId(), bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, landingHeight);
                sLog.outDetail("%s: Jump: Falling simulated, height: %f, timeToLand %u", bot->GetName(), landingHeight, jumpTime);
            }
        }
    }

    // cheat options
    if (bot->IsAlive() && ((uint32)GetCheat() > 0 || (uint32)sPlayerbotAIConfig.botCheatMask > 0))
    {
        if (HasCheat(BotCheatMask::health))
            bot->SetHealthPercent(100);
        if (HasCheat(BotCheatMask::mana) && bot->GetPowerType() == POWER_MANA)
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
        if (HasCheat(BotCheatMask::power) && bot->GetPowerType() != POWER_MANA)
            bot->SetPower(bot->GetPowerType(), bot->GetMaxPower(bot->GetPowerType()));
        if (HasCheat(BotCheatMask::cooldown))
            bot->RemoveAllCooldowns();
        if (HasCheat(BotCheatMask::movespeed))
        {
            bot->UpdateSpeed(MOVE_WALK, true, 10);
            bot->UpdateSpeed(MOVE_RUN, true, 10);
            bot->UpdateSpeed(MOVE_SWIM, true, 10);
        }
        if (HasCheat(BotCheatMask::breath))
        {
            bot->SetWaterBreathingIntervalMultiplier(0);
        }
        if (HasCheat(BotCheatMask::item) && (bot->getClass() == CLASS_HUNTER || bot->getClass() == CLASS_ROGUE || bot->getClass() == CLASS_WARRIOR))
        {
            uint32 itemId = bot->GetUInt32Value(PLAYER_AMMO_ID);
            if (itemId && bot->GetItemCount(itemId))
            {                
                std::list<Item*> items = AI_VALUE2(std::list<Item*>, "inventory items", this->chatHelper.formatQItem(itemId));

                for (auto item : items)
                {
                    if (bot->getClass() == CLASS_HUNTER && item->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_THROWN) //Do not replenish thrown weapons for hunters.
                        break;

                    item->SetCount(item->GetProto()->GetMaxStackSize());
                    break;
                }
            }
            else
            {
#ifndef MANGOSBOT_ZERO
                if (!bot->HasAura(46699)) // Thori'dal
                {
                    PlayerbotFactory(bot, bot->GetLevel(), 0).InitAmmo();
                }
#else
                PlayerbotFactory(bot, bot->GetLevel(), 0).InitAmmo();
#endif
            }
        }
    }

    // Alt level sync - level up non-random bots to match master level
    if (sPlayerbotAIConfig.syncAltLevelToMaster && bot->IsAlive() && master && !sRandomPlayerbotMgr.IsRandomBot(bot)
        && bot->GetGroup() && master->GetGroup() && IsSafe(master) && bot->GetGroup()->GetLeaderGuid() == master->GetObjectGuid())
    {
        uint32 botLevel = bot->GetLevel();
        uint32 masterLevel = master->GetLevel();
        uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

        if (botLevel < masterLevel && botLevel < maxLevel)
        {
            bot->GiveLevel(botLevel + 1);
         
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                sPlayerbotAIConfig.logEvent(this, "AltLevelSync", ObjectGuid(),
                    "Leveled from " + std::to_string(botLevel) + " to " + std::to_string(bot->GetLevel()) +
                    " (master: " + std::to_string(masterLevel) + ")");
            }
        }
    }

    if (master && IsSafe(master) && bot->GetDistance(master) < INTERACTION_DISTANCE * 2.5 && master->GetTransport() != bot->GetTransport() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        bot->StopMoving();
        if (master->GetTransport() && WorldPosition(bot).isOnTransport(master->GetTransport()))
            master->GetTransport()->AddPassenger(bot);
        else if (bot->GetTransport())
        {
            WorldPosition botPos(bot);
            bot->GetTransport()->RemovePassenger(bot);
            bot->NearTeleportTo(bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, bot->m_movementInfo.pos.z, bot->m_movementInfo.pos.o);
            MANGOS_ASSERT(botPos.fDist(bot) < 500.0f);
        }
    }
    else if (!HasRealPlayerMaster() && !bot->IsBeingTeleported() && bot->GetTransport() && bot->GetMapId() == bot->GetTransport()->GetMapId() && !WorldPosition(bot).isOnTransport(bot->GetTransport()) && !isMovingToTransport)
    {
        if (HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            TellPlayer(GetMaster(), "Jumping off " + std::string(bot->GetTransport()->GetName()));
        }

        WorldPosition botPos(bot);
        bot->GetTransport()->RemovePassenger(bot);
        bot->NearTeleportTo(bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, bot->m_movementInfo.pos.z, bot->m_movementInfo.pos.o);
        MANGOS_ASSERT(botPos.fDist(bot) < 500.0f);
        bot->StopMoving();
    }

    // Update facing
    SC_PHASE("UpdateAI.UpdateFaceTarget", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-face", bot ? bot->GetGUIDLow() : 0);
    UpdateFaceTarget(elapsed, minimal);

    bool doMinimalReaction = minimal || !AllowActivity(REACT_ACTIVITY);

#ifdef PLAYERBOT_ELUNA
    // used by eluna
    if (Eluna* e = bot->GetEluna())
        e->OnUpdateAI(this, bot->GetName());
#endif

    // Only update the internal ai when no reaction is running and the internal ai can be updated
    SC_PHASE("UpdateAI.UpdateAIReaction", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-reaction", bot ? bot->GetGUIDLow() : 0);
    if(!UpdateAIReaction(elapsed, doMinimalReaction, bot->IsTaxiFlying()) && CanUpdateAIInternal())
    {
        // Update the delay with the spell cast time
        Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (currentSpell && (currentSpell->getState() == SPELL_STATE_CASTING) && (currentSpell->GetCastedTime() > 0U))
        {
            SetAIInternalUpdateDelay(currentSpell->GetCastedTime() + sPlayerbotAIConfig.reactDelay + sWorld.GetAverageDiff());

            // Cancel the update if the new delay increased
            if (!CanUpdateAIInternal())
            {
                return;
            }
        }

        SC_PHASE("UpdateAI.UpdateAIInternal", bot ? bot->GetName() : "(null)");
        MapManager::SetContinentUpdatePhase("bot-internal", bot ? bot->GetGUIDLow() : 0);
        RecordBrainServiceInterval(WorldTimer::getMSTime(), lastBrainExecMs);
        UpdateAIInternal(elapsed, minimal);

        // LOD COLD tier: a bot with NO real player within lodColdRange (different
        // zone/continent, or an empty server) goes DORMANT -- invisible (Greater
        // Invisibility) + combat dropped + brain on the slow lodColdUpdateMs interval --
        // REGARDLESS of whether it is currently fighting, because going dormant drops the
        // fight. This is deliberately distance-based, NOT GetPriorityType(), which would
        // classify any fighting bot as IN_COMBAT/foreground and so could never go cold.
        // This is what lets the throttle apply to the constantly-fighting far fleet, and
        // it is the primary CPU + crash-rate reduction (crashes scale with bot-AI
        // execution volume). SetAIInternalUpdateDelay here is not overridden by the
        // YieldAIInternalThread below (it only acts when the delay is near zero).
        if (sPlayerbotAIConfig.lodColdUpdateMs && bot && bot->IsInWorld()
            && !HasRealPlayerMaster() && sRandomPlayerbotMgr.IsRandomBot(bot)
            && !sRandomPlayerbotMgr.IsActiveCohort(bot->GetGUIDLow())     // brain budget decides
            && !IsInstancedContent()                                     // instanced bots never park
            && !bot->IsInCombat() && bot->getAttackers().empty())         // finish the fight first
        {
            SetColdDormant(true);
            SetAIInternalUpdateDelay(sPlayerbotAIConfig.lodColdUpdateMs);
        }
        else if (coldDormant)
        {
            SetColdDormant(false);   // a real player is now near -> promote / wake instantly
        }

        bool min = minimal;
        // Keep distant noncombat bots on the cheap yield, but let bots in a
        // player's visual/nearby lane react at normal cadence so they do not
        // look frozen while the rest of the world stays throttled.
        if (!inCombat)
            min = !IsForegroundPriority(GetPriorityType());

        // SUPERVISOR MODE: keep the whole (small) fleet at full foreground cadence even with NO
        // player online, so it quests/grinds at real speed for autonomous observation of true APM.
        if (sPlayerbotAIConfig.supervisorMode)
            min = false;

        // FULL CADENCE for the active brain set (the 200 bots nearest a real player) + instanced
        // bots. Without this they dropped to the 1s "minimal" yield and looked frozen/laggy near the
        // player (the "bots standing still around me" report) even when the tick was fast.
        if (IsInstancedContent() || sRandomPlayerbotMgr.IsActiveCohort(bot->GetGUIDLow()))
            min = false;

        SC_PHASE("UpdateAI.YieldAIInternalThread", bot ? bot->GetName() : "(null)");
        MapManager::SetContinentUpdatePhase("bot-yield", bot ? bot->GetGUIDLow() : 0);
        YieldAIInternalThread(min);

        // TIGHTER react floor for instanced bots so they press buttons + update movement/range
        // faster than the open-world 100ms (max 40-80 bots per instance -> cheap). Applied after
        // Yield (which set the 100ms floor); shrink it to instanceReactDelay for responsiveness.
        if (IsInstancedContent() && aiInternalUpdateDelay > sPlayerbotAIConfig.instanceReactDelay)
            aiInternalUpdateDelay = sPlayerbotAIConfig.instanceReactDelay;
	}
	SC_PHASE("UpdateAI.exit", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-ai-exit", bot ? bot->GetGUIDLow() : 0);
}

bool PlayerbotAI::AllowExpensivePlanner(uint32 intervalMs, uint32 activeIntervalMs)
{
    if (!bot)
        return false;

    const bool autonomousRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot) && !HasActivePlayerMaster() && !HasRealPlayerMaster();
    const bool fastLane =
        !autonomousRandomBot ||
        currentState != BotState::BOT_STATE_NON_COMBAT ||
        inCombat ||
        sServerFacade.IsInCombat(bot) ||
        bot->InBattleGround() ||
        bot->IsTaxiFlying() ||
        bot->IsBeingTeleported() ||
        bot->IsNonMeleeSpellCasted(true) ||
        bot->GetLootGuid();

    if (fastLane)
    {
        PerfStats::RecordPlayerbotPlanner(true, true);
        return true;
    }

    if (!intervalMs)
        intervalMs = 1;
    if (!activeIntervalMs)
        activeIntervalMs = 1;

    const uint32 pressure = PerfStats::GetBotPressureLevel();
    if (pressure >= PerfStats::BOT_PRESSURE_CRITICAL)
    {
        intervalMs *= 4;
        activeIntervalMs *= 4;
    }
    else if (pressure >= PerfStats::BOT_PRESSURE_PRESSURE)
    {
        intervalMs *= 2;
        activeIntervalMs *= 2;
    }

    const uint32 now = WorldTimer::getMSTime();
    if (!expensivePlannerNextAllowedMs)
    {
        // Spread the first planner pass so login/startup clusters do not synchronize.
        expensivePlannerNextAllowedMs = now + (bot->GetGUIDLow() % intervalMs);
        if (pressure != PerfStats::BOT_PRESSURE_NORMAL)
            PerfStats::RecordBotPressureDeferred(PerfStats::BOT_PRESSURE_WORK_QUEST_PLANNER);
        PerfStats::RecordPlayerbotPlanner(false, false);
        return false;
    }

    if (expensivePlannerNextAllowedMs > now &&
        WorldTimer::getMSTimeDiff(now, expensivePlannerNextAllowedMs) < intervalMs * 4)
    {
        if (pressure != PerfStats::BOT_PRESSURE_NORMAL)
            PerfStats::RecordBotPressureDeferred(PerfStats::BOT_PRESSURE_WORK_QUEST_PLANNER);
        PerfStats::RecordPlayerbotPlanner(false, false);
        return false;
    }

    expensivePlannerNextAllowedMs = now + activeIntervalMs + (bot->GetGUIDLow() % intervalMs);
    PerfStats::RecordPlayerbotPlanner(true, false);
    return true;
}

bool PlayerbotAI::AllowPressureWork(uint32 workType, uint32 pressureIntervalMs, uint32 criticalIntervalMs)
{
    // Observation stays on, but the active pressure governor is disabled after
    // it caused short-run stalls. Keep call sites harmless while we isolate.
    (void)workType;
    (void)pressureIntervalMs;
    (void)criticalIntervalMs;
    return bot != nullptr;

    if (!bot || workType >= 5)
        return false;

    const uint32 pressure = PerfStats::GetBotPressureLevel();
    if (pressure == PerfStats::BOT_PRESSURE_NORMAL)
        return true;

    const bool autonomousRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot) && !HasActivePlayerMaster() && !HasRealPlayerMaster();
    if (!autonomousRandomBot)
        return true;

    if (bot->InBattleGround() || bot->IsTaxiFlying() || bot->IsBeingTeleported() || bot->IsNonMeleeSpellCasted(true) || bot->GetLootGuid())
        return true;

    uint32 intervalMs = pressure >= PerfStats::BOT_PRESSURE_CRITICAL ? criticalIntervalMs : pressureIntervalMs;
    if (!intervalMs)
        intervalMs = 1;

    const uint32 now = WorldTimer::getMSTime();
    uint32& nextAllowed = pressureWorkNextAllowedMs[workType];
    if (!nextAllowed)
        nextAllowed = now + ((bot->GetGUIDLow() + workType * 997u) % intervalMs);

    if (nextAllowed > now && WorldTimer::getMSTimeDiff(now, nextAllowed) < intervalMs * 4)
    {
        PerfStats::RecordBotPressureDeferred(static_cast<PerfStats::BotPressureWorkType>(workType));
        return false;
    }

    nextAllowed = now + intervalMs + ((bot->GetGUIDLow() + workType * 997u) % intervalMs);
    return true;
}

bool PlayerbotAI::UpdateAIReaction(uint32 elapsed, bool minimal, bool isStunned)
{
    bool reactionFound;
    std::string mapString = WorldPosition(bot).isInstance() ? "I" : std::to_string(bot->GetMapId());
    auto pmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAIReaction " + mapString, nullptr, bot->GetMapId(), bot->GetInstanceId());
    const bool reactionInProgress = reactionEngine->Update(elapsed, minimal, isStunned, reactionFound);
    pmo.reset();

    if(reactionFound)
    {
        // If new reaction found force stop current actions (if required)
        const Reaction* reaction = reactionEngine->GetReaction();
        if(reaction)
        {
            if(reaction->ShouldInterruptCast())
            {
                // Only break a cast for a panic reaction (flee/avoid-aoe) when the bot is actually
                // in danger. At healthy HP a player finishes the 1-2s cast first -- interrupting
                // offensive casts at full health read as scatter-brained self-cancels.
                if (bot->GetHealthPercent() < 50.0f || !bot->IsNonMeleeSpellCasted(true, false, true))
                    InterruptSpell();
            }

            if (reaction->ShouldInterruptMovement())
            {
                StopMoving();
            }
        }
    }

    return reactionInProgress;
}

void PlayerbotAI::UpdateFaceTarget(uint32 elapsed, bool minimal)
{
    faceTargetUpdateDelay = faceTargetUpdateDelay > elapsed ? faceTargetUpdateDelay - elapsed : 0U;
    if (faceTargetUpdateDelay <= 0U)
    {
        // Only update the target facing when in combat
        if (IsStateActive(BotState::BOT_STATE_COMBAT))
        {
            // Don't update facing if bot is moving
            if (!sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
            {
                AiObjectContext* context = GetAiObjectContext();
                Unit* target = AI_VALUE(Unit*, "current target");
                if(target)
                {
                    // Do not update the facing while pulling
                    Unit* pullTarget = AI_VALUE(Unit*, "pull target");
                    if (pullTarget == nullptr)
                    {
                        if (!AI_VALUE2(bool, "facing", "current target"))
                        {
                            if (!sServerFacade.UnitIsDead(bot) &&
                                !sServerFacade.IsFrozen(bot) &&
                                !sServerFacade.IsCharmed(bot) &&
                                !bot->IsPolymorphed() &&
                                !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) &&
                                !bot->IsBeingTeleported() &&
                                !bot->HasAuraType(SPELL_AURA_MOD_CONFUSE) &&
                                !bot->HasAuraType(SPELL_AURA_MOD_STUN) &&
                                !bot->IsTaxiFlying() &&
                                !bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
                            {
                                sServerFacade.SetFacingTo(bot, target);
                                bot->SetInFront(target);
                                WorldPacket data(MSG_MOVE_SET_FACING);
                                data << bot->GetPackGUID();
                                data << bot->m_movementInfo;
                                bot->GetMover()->SendMessageToSetExcept(data, bot);
                            }
                        }
                    }
                }
            }
        }

        faceTargetUpdateDelay = minimal ? sPlayerbotAIConfig.reactDelay * 10 : sPlayerbotAIConfig.reactDelay * 5;
    }
}

void PlayerbotAI::SetActionDuration(const Action* action)
{
    if (action)
    {
        if(action->IsReaction())
        {
            reactionEngine->SetReactionDuration(action);
        }
        else
        {
            SetAIInternalUpdateDelay(action->GetDuration());
        }
    }
}

void PlayerbotAI::SetActionDuration(uint32 duration)
{
    SetAIInternalUpdateDelay(duration);
}

const Action* PlayerbotAI::GetLastExecutedAction(BotState state) const
{
    const Engine* engine = engines[(uint8)state];
    if(engine)
    {
        return engine->GetLastExecutedAction();
    }

    return nullptr;
}

bool PlayerbotAI::IsImmuneToSpell(uint32 spellId) const
{
    for (std::list<uint32>::iterator i = sPlayerbotAIConfig.immuneSpellIds.begin(); i != sPlayerbotAIConfig.immuneSpellIds.end(); ++i)
    {
        if (spellId == (*i))
        {
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::IsInPve()
{
    return !IsInPvp() && !IsInRaid();
}

bool PlayerbotAI::IsInPvp()
{
    if (IsSafe(bot))
    {
        const bool inDuel = bot->m_duel && bot->m_duel->opponent;
        if (!inDuel)
        {
            const bool inBattleground = bot->InBattleGround();
            bool inArena = false;
#ifndef MANGOSBOT_ZERO
            inArena = bot->InArena();
#endif
            if (!inBattleground && !inArena)
            {
                AiObjectContext* context = aiObjectContext;
                const bool isPlayerNear = AI_VALUE(bool, "has enemy player targets");
                if (!isPlayerNear)
                {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

bool PlayerbotAI::IsInRaid()
{
    bool inRaidFight = false;
    if (IsSafe(bot))
    {
        const Map* map = bot->GetMap();
        if (map && (map->IsDungeon() || map->IsRaid()))
        {
            inRaidFight = true;
        }
        else if (!IsInPvp() && GetState() == BotState::BOT_STATE_COMBAT)
        {
            AiObjectContext* context = GetAiObjectContext();
            const std::list<ObjectGuid>& attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
            for (const ObjectGuid& attackerGuid : attackers)
            {
                Creature* creature = GetCreature(attackerGuid);
                if (creature)
                {
                    const CreatureInfo* creatureInfo = creature->GetCreatureInfo();
                    if (creatureInfo)
                    {
                        if (creatureInfo->Rank == CREATURE_ELITE_WORLDBOSS)
                        {
                            inRaidFight = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    return inRaidFight;
}

PlayerTalentSpec PlayerbotAI::GetTalentSpec()
{
    return aiObjectContext->GetValue<PlayerTalentSpec>("talent spec")->Get();
}

void PlayerbotAI::UpdateTalentSpec(PlayerTalentSpec spec)
{
    if(spec == PlayerTalentSpec::TALENT_SPEC_INVALID)
    {
        int talentsTab = 0;
        if(bot->GetLevel() < 10)
        {
            switch (bot->getClass())
            {
                case CLASS_MAGE:
                {
                    talentsTab = 1;
                    break;
                }

                case CLASS_PALADIN:
                {
                    talentsTab = 0;
                    break;
                }

                case CLASS_PRIEST:
                {
                    talentsTab = 1;
                    break;
                }
            }
        }
        else
        {
            talentsTab = AiFactory::GetPlayerSpecTab(bot);
        }

        spec = PlayerTalentSpec(((bot->getClass() * 3) - 2) + talentsTab);
    }

    aiObjectContext->GetValue<PlayerTalentSpec>("talent spec")->Set(spec);
}

bool PlayerbotAI::CanEnterArea(const AreaTrigger* area)
{
    if (sRandomPlayerbotMgr.IsRandomBot(GetBot()))
    {
        DungeonPersistentState* state = bot->GetBoundInstanceSaveForSelfOrGroup(area->target_mapId);
        Map* map = sMapMgr.FindMap(area->target_mapId, state ? state->GetInstanceId() : 0);
        const MapEntry* mapEntry = sMapStore.LookupEntry(area->target_mapId);

        // check if this account try to abuse reseting instance
#ifdef MANGOSBOT_ZERO
        if (mapEntry->IsNonRaidDungeon())
#elif MANGOSBOT_ONE
        if (mapEntry->IsNonRaidDungeon() && ((map && map->GetDifficulty() == DUNGEON_DIFFICULTY_NORMAL) || (bot->GetDifficulty() == DUNGEON_DIFFICULTY_NORMAL)))
#else
        if (mapEntry->IsNonRaidDungeon() && ((map && map->GetDifficulty() == DUNGEON_DIFFICULTY_NORMAL) || (bot->GetDifficulty(false) == DUNGEON_DIFFICULTY_NORMAL)))
#endif
        {
            if (!bot->CanEnterNewInstance(state ? state->GetInstanceId() : 0))
            {
                return false;
            }
        }

        // Map's state check
        if (map && map->IsDungeon())
        {
            // cannot enter if the instance is full (player cap), GMs don't count, must not check when teleporting around the same map
            if (bot->GetMapId() != area->target_mapId)
            {
                if (((DungeonMap*)map)->GetPlayersCountExceptGMs() >= ((DungeonMap*)map)->GetMaxPlayers())
                {
                    return false;
                }

                // In Combat check
                if (map && map->GetInstanceData() && map->GetInstanceData()->IsEncounterInProgress())
                {
                    return false;
                }

                // Bind Checks
#ifdef MANGOSBOT_ZERO
                InstancePlayerBind* pBind = bot->GetBoundInstance(area->target_mapId);
#elif MANGOSBOT_ONE
                InstancePlayerBind* pBind = bot->GetBoundInstance(area->target_mapId, bot->GetDifficulty());
#else
                InstancePlayerBind* pBind = bot->GetBoundInstance(area->target_mapId, bot->GetDifficulty(mapEntry->IsRaid()));
#endif
                if (pBind && pBind->perm && pBind->state != state)
                {
                    return false;
                }

                if (pBind && pBind->perm && pBind->state != map->GetPersistentState())
                {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

void PlayerbotAI::Unmount()
{
    if ((bot->IsMounted() || bot->GetMountID()) && !bot->IsTaxiFlying())
    {
        bot->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        bot->Unmount();

        bot->UpdateSpeed(MOVE_RUN, true);
        bot->UpdateSpeed(MOVE_RUN, false);

        if (bot->IsFlying())
        {
            bot->GetMotionMaster()->MoveFall();
        }
    }
}

bool PlayerbotAI::IsStateActive(BotState state) const
{
    return currentEngine == engines[(uint8)state];
}

time_t PlayerbotAI::GetCombatStartTime() const
{
    return aiObjectContext->GetValue<time_t>("combat start time")->Get();
}

void PlayerbotAI::OnCombatStarted()
{
    if (aiObjectContext->GetValue<GuidPosition>("rpg target")->Get())
    {
        aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
    }

    bool interruptedTravelMovement = false;
    bool interruptedQuestTravel = false;
    TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
    if (travelTarget && travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE &&
        travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_COOLDOWN)
    {
        interruptedTravelMovement =
            travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_READY ||
            travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL ||
            travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK;
        interruptedQuestTravel = IsQuestTravelDestination(travelTarget->GetDestination());
        travelTarget->SetStatus(interruptedQuestTravel ? TravelStatus::TRAVEL_STATUS_EXPIRED : TravelStatus::TRAVEL_STATUS_COOLDOWN);
        if (interruptedQuestTravel)
        {
            travelTarget->SetExpireIn(1000);
            aiObjectContext->ClearValues("no active travel destinations");
            aiObjectContext->GetValue<bool>("travel target active")->Reset();
        }
    }

    if(!IsStateActive(BotState::BOT_STATE_COMBAT))
    {
        // Reset the combat start timestamp
        aiObjectContext->GetValue<time_t>("combat start time")->Set(time(0));

        // Update stay position on location when combat starts
        if (HasStrategy("stay", BotState::BOT_STATE_COMBAT) && !HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT))
        {
            aiObjectContext->GetValue<PositionEntry>("pos", "stay")->Set(PositionEntry(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId()));
        }

        // Stop follow movement on combat start
        if (!(HasStrategy("follow", BotState::BOT_STATE_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_COMBAT)) &&
            (HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        {
            StopMoving();
        }

        if (interruptedTravelMovement)
        {
            StopMoving();
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "status=" << (interruptedQuestTravel ? "expired" : "cooldown")
                    << " destinationPtr=" << (travelTarget && travelTarget->GetDestination() ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "TravelCombatInterrupt", bot->GetName(), out.str());
            }
        }

        aiObjectContext->GetValue<std::list<ObjectGuid>>("attackers", 1)->Reset();
        aiObjectContext->GetValue<bool>("has attackers")->Reset();

        ChangeEngine(BotState::BOT_STATE_COMBAT);
        if (bot->IsSitState())
        {
            ResetAIInternalUpdateDelay();
        }
    }
}

void PlayerbotAI::OnCombatEnded()
{
    if (!IsStateActive(BotState::BOT_STATE_NON_COMBAT))
    {
        // Reset the combat start timestamp
        aiObjectContext->GetValue<time_t>("combat start time")->Set(0);

        // Stop following on combat end
        if ((HasStrategy("follow", BotState::BOT_STATE_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_COMBAT)) &&
            !(HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        {
            StopMoving();
        }

        ChangeEngine(BotState::BOT_STATE_NON_COMBAT);
    }
}

void PlayerbotAI::OnDeath()
{
    if (!IsStateActive(BotState::BOT_STATE_DEAD) && !sServerFacade.IsAlive(bot))
    {
        StopMoving();

        Player* master = GetMaster();
        AiObjectContext* context = aiObjectContext;
        if (!HasActivePlayerMaster() && !bot->InBattleGround())
        {

            SET_AI_VALUE(uint32, "death count", AI_VALUE(uint32, "death count") + 1);

            if (sPlayerbotAIConfig.hasLog("deaths.csv"))
            {
                WorldPosition botPos(bot);

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);

                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                float subLevel = ((float)bot->GetLevel() + ((float)bot->GetUInt32Value(PLAYER_XP) / (float)bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)));

                out << subLevel << ",";

                botPos.printWKT(out);

                AiObjectContext* context = GetAiObjectContext();

                Unit* ctarget = AI_VALUE(Unit*, "current target");

                if (ctarget)
                {
                    out << "\"" << ctarget->GetName() << "\"," << ctarget->GetLevel() << "," << ctarget->GetHealthPercent() << ",";
                }
                else
                    out << "\"none\",0,100,";

                std::list<ObjectGuid> targets = AI_VALUE_LAZY(std::list<ObjectGuid>, "all targets");

                out << "\"";

                uint32 adds = 0;

                for (auto target : targets)
                {
                    if (!target.IsCreature())
                        continue;

                    Unit* unit = GetUnit(target);
                    if (!unit)
                        continue;

                    if (unit->GetVictim() != bot)
                        continue;

                    if (unit == ctarget)
                        continue;

                    out << unit->GetName() << "(" << unit->GetLevel() << ")";

                    adds++;
                }

                out << "\"," << std::to_string(adds);

                sPlayerbotAIConfig.log("deaths.csv", out.str().c_str());
            }
        }

        SET_AI_VALUE(Unit*, "current target", nullptr);
        SET_AI_VALUE(Unit*, "enemy player target", nullptr);
        SET_AI_VALUE(Unit*, "pull target", nullptr);
        SET_AI_VALUE(ObjectGuid, "attack target", ObjectGuid());
        SET_AI_VALUE(LootObject, "loot target", LootObject());
        SET_AI_VALUE(time_t, "combat start time", 0);
        SET_AI_VALUE2(bool, "manual bool", "enemies near corpse", false);
        SET_AI_VALUE2(bool, "manual bool", "enemies near graveyard", false);
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
        sTravelMgr.SetNullTravelTarget(travelTarget);
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        travelTarget->SetExpireIn(1000);
        *AI_VALUE(FutureDestinations*, "future travel destinations") = FutureDestinations();
        RESET_AI_VALUE2(std::string, "manual string", "future travel purpose");
        RESET_AI_VALUE2(std::string, "manual string", "future travel condition");
        RESET_AI_VALUE2(int, "manual int", "future travel relevance");
        context->ClearValues("no active travel destinations");
        ChangeEngine(BotState::BOT_STATE_DEAD);
    }
}

void PlayerbotAI::OnResurrected()
{
    if (IsStateActive(BotState::BOT_STATE_DEAD) && sServerFacade.IsAlive(bot))
    {
        // Stop following on resurrected
        if ((HasStrategy("follow", BotState::BOT_STATE_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_COMBAT)) &&
            !(HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        {
            StopMoving();
        }

        ChangeEngine(BotState::BOT_STATE_NON_COMBAT);
    }
}

void PlayerbotAI::HandleCommands()
{
    ExternalEventHelper helper(aiObjectContext);

    // NL COMMAND BRIDGE, execution side: run LLM-translated commands. Parse failures here are
    // dropped silently (the LLM only speaks the known vocabulary; anything else is noise).
    {
        std::queue<std::pair<std::string, uint32>> ready;
        {
            std::scoped_lock lock(translatedCommandsMutex);
            ready.swap(translatedCommands);
        }
        while (!ready.empty())
        {
            auto& pr = ready.front();
            Player* owner = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, pr.second));
            if (owner)
                helper.ParseChatCommand(pr.first, owner);
            ready.pop();
        }
    }

    std::list<ChatCommandHolder> delayed;
    while (!chatCommands.empty())
    {
        ChatCommandHolder holder = chatCommands.front();
        time_t checkTime = holder.GetTime();
        if (checkTime && time(0) < checkTime)
        {
            delayed.push_back(holder);
            chatCommands.pop();
            continue;
        }

        std::string command = holder.GetCommand();
        Player* owner = holder.GetOwner();
        if (!helper.ParseChatCommand(command, owner) && holder.GetType() == CHAT_MSG_WHISPER)
        {
            // NL COMMAND BRIDGE, translation side: a whisper from the real-player master that no
            // string command matched. Ask the LLM to translate it into the known vocabulary
            // (CMD: a | b) or classify it as conversation (SAY: ...). Async — never blocks the tick.
            if (sPlayerbotAIConfig.llmCommandBridgeEnabled && sPlayerbotAIConfig.llmEnabled &&
                owner && owner->isRealPlayer() && GetMaster() == owner)
            {
                std::string text = command;
                uint32 ownerGuid = owner->GetGUIDLow();
                uint32 msgType = holder.GetType();
                PlayerbotAI* self = this;
                std::string senderName = owner->GetName();

                std::map<std::string, std::string> jsonFill;
                jsonFill["<pre prompt>"] = sPlayerbotAIConfig.llmCommandPrompt;
                jsonFill["<context>"] = "";
                jsonFill["<prompt>"] = "Player instruction: " + PlayerbotLLMInterface::SanitizeForJson(text);
                jsonFill["<post prompt>"] = "";
                std::string json = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmApiJson, jsonFill);

                (void)std::async(std::launch::async, [self, json, ownerGuid, msgType, text, senderName]()
                {
                    std::vector<std::string> debug;
                    std::string response = PlayerbotLLMInterface::Generate(json,
                        sPlayerbotAIConfig.llmGenerationTimeout, sPlayerbotAIConfig.llmMaxSimultaniousGenerations, debug);
                    std::vector<std::string> parts = PlayerbotLLMInterface::ParseResponse(response,
                        sPlayerbotAIConfig.llmResponseStartPattern, sPlayerbotAIConfig.llmResponseEndPattern,
                        sPlayerbotAIConfig.llmResponseDeletePattern, sPlayerbotAIConfig.llmResponseSplitPattern, debug);
                    std::string parsed;
                    for (std::string const& part : parts)
                        parsed += part + " ";

                    size_t cmdPos = parsed.find("CMD:");
                    if (cmdPos != std::string::npos)
                    {
                        std::string cmds = parsed.substr(cmdPos + 4);
                        std::scoped_lock lock(self->translatedCommandsMutex);
                        std::stringstream ss(cmds);
                        std::string one;
                        while (std::getline(ss, one, '|'))
                        {
                            // trim
                            size_t b = one.find_first_not_of(" \t\r\n");
                            size_t e = one.find_last_not_of(" \t\r\n.");
                            if (b == std::string::npos)
                                continue;
                            self->translatedCommands.push({ one.substr(b, e - b + 1), ownerGuid });
                        }
                    }
                    else
                    {
                        // conversation: hand to the normal AI-chat reply path
                        size_t sayPos = parsed.find("SAY:");
                        (void)sayPos;
                        self->QueueChatResponse(msgType, ObjectGuid(HIGHGUID_PLAYER, ownerGuid), ObjectGuid(),
                            text, "", senderName, true);
                    }
                });
            }
        }

        chatCommands.pop();
    }

    for (std::list<ChatCommandHolder>::iterator i = delayed.begin(); i != delayed.end(); ++i)
    {
        chatCommands.push(*i);
    }
}

void PlayerbotAI::UpdateAIInternal(uint32 elapsed, bool minimal)
{
    SC_PHASE("UpdateAIInternal.entry", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-internal-entry", bot ? bot->GetGUIDLow() : 0);
    if (bot->IsBeingTeleported() || !bot->IsInWorld())
        return;

    std::string mapString = WorldPosition(bot).isInstance() ? "I" : std::to_string(bot->GetMapId());
    auto pmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAIInternal " + mapString, nullptr, bot->GetMapId(), bot->GetInstanceId());

    ExternalEventHelper helper(aiObjectContext);

    // chat replies
    std::list<ChatQueuedReply> delayedResponses;
    {
        std::scoped_lock lock(chatRepliesMutex);
        while (!chatReplies.empty())
        {
            ChatQueuedReply holder = chatReplies.front();
            time_t checkTime = holder.m_time;
            if (checkTime && time(0) < checkTime)
            {
                delayedResponses.push_back(holder);
                chatReplies.pop();
                continue;
            }
            ChatReplyAction::ChatReplyDo(bot, holder.m_type, holder.m_guid1, holder.m_guid2, holder.m_msg, holder.m_chanName, holder.m_name);
            chatReplies.pop();
        }

        for (std::list<ChatQueuedReply>::iterator i = delayedResponses.begin(); i != delayedResponses.end(); ++i)
        {
            chatReplies.push(*i);
        }
    }
    // logout if logout timer is ready or if instant logout is possible
    if (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut())
    {
        WorldSession* botWorldSessionPtr = bot->GetSession();
        bool logout = botWorldSessionPtr->ShouldLogOut(time(nullptr));
        if (!master || master->GetSession()->GetState() != WORLD_SESSION_STATE_READY)
            logout = true;

        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || bot->IsTaxiFlying() ||
            botWorldSessionPtr->GetSecurity() >= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_INSTANT_LOGOUT))
        {
            logout = true;
        }

        if (master && (master->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || master->IsTaxiFlying() ||
            (master->GetSession() && master->GetSession()->GetSecurity() >= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_INSTANT_LOGOUT))))
        {
            logout = true;
        }

        if (logout && !bot->GetSession()->ShouldLogOut(time(nullptr)))
        {
            // This runs on a map-worker thread. LogoutPlayerBot tears the Player down and raced
            // the world thread's unlocked playerBots iteration (13 SIGSEGVs in AllowActivity via
            // LogPlayerLocation) -> queue the logout; the holder drains it on the world thread.
            if (master && master->GetPlayerbotMgr())
            {
                master->GetPlayerbotMgr()->QueueBotLogout(bot->GetObjectGuid().GetRawValue());
            }
            else
            {
                sRandomPlayerbotMgr.QueueBotLogout(bot->GetObjectGuid().GetRawValue());
            }
            return;
        }

        SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);
        return;
    }

    SC_PHASE("UpdateAIInternal.botOutgoingPackets", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-packets-out", bot ? bot->GetGUIDLow() : 0);
    botOutgoingPacketHandlers.Handle(helper);
    SC_PHASE("UpdateAIInternal.masterIncomingPackets", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-master-in", bot ? bot->GetGUIDLow() : 0);
    masterIncomingPacketHandlers.Handle(helper);
    SC_PHASE("UpdateAIInternal.masterOutgoingPackets", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-master-out", bot ? bot->GetGUIDLow() : 0);
    masterOutgoingPacketHandlers.Handle(helper);

    SC_PHASE("UpdateAIInternal.DoNextAction", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-next-action", bot ? bot->GetGUIDLow() : 0);
	DoNextAction(minimal);
    SC_PHASE("UpdateAIInternal.exit", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-internal-exit", bot ? bot->GetGUIDLow() : 0);
}

void PlayerbotAI::HandleTeleportAck()
{
    if (IsRealPlayer() && bot->IsBeingTeleportedFar())
        return;

    StopMoving();

	if (bot->IsBeingTeleportedNear())
	{
        if (!bot->GetMotionMaster()->empty())
            if (MovementGenerator* movgen = bot->GetMotionMaster()->top())
                movgen->Interrupt(*bot);

       /* WorldLocation dest = bot->GetTeleportDest();
        bot->Relocate(dest.coord_x, dest.coord_y, dest.coord_z, dest.orientation);*/

		WorldPacket p = WorldPacket(MSG_MOVE_TELEPORT_ACK, 8 + 4 + 4);
#ifdef MANGOSBOT_TWO
        p << bot->GetObjectGuid().WriteAsPacked();
#else
        p << bot->GetObjectGuid();
#endif
        uint32 teleportCounter = GetPendingTeleportAckCounter();
        if (!teleportCounter && bot->GetMovementCounter() > 0)
            teleportCounter = bot->GetMovementCounter() - 1;

		p << teleportCounter;
		p << (uint32) time(0); // time - not currently used
        bot->GetSession()->HandleMoveTeleportAckOpcode(p);
        pendingTeleportAckCounter = 0;

        // add delay to simulate teleport delay
        SetAIInternalUpdateDelay(urand(1000, 2000));
	}
	else if (bot->IsBeingTeleportedFar())
	{
        bot->GetSession()->HandleMoveWorldportAckOpcode();

        // add delay to simulate teleport delay
        SetAIInternalUpdateDelay(urand(2000, 5000));
	}

    if (IsRealPlayer())
        bot->SendHeartBeat();

    Reset();
}

uint32 PlayerbotAI::GetPendingTeleportAckCounter() const
{
    return pendingTeleportAckCounter;
}

std::string PlayerbotAI::BuildCurrentTaskSummary()
{
    AiObjectContext* context = aiObjectContext;
    std::ostringstream out;
    out << "state=" << static_cast<uint32>(currentState);

    const Action* lastAction = GetLastExecutedAction(currentState);
    out << " action=" << (lastAction ? const_cast<Action*>(lastAction)->getName() : "none");

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    out << " target=" << (currentTarget ? currentTarget->GetName() : "none");

    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    out << " loot=" << (!lootTarget.IsEmpty() ? 1 : 0);

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    TravelDestination* destination = travelTarget ? travelTarget->GetDestination() : nullptr;
    out << " travelStatus=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
        << " travelPtr=" << (destination ? 1 : 0);

    out << " moving=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
        << " dead=" << (sServerFacade.UnitIsDead(bot) ? 1 : 0)
        << " ghost=" << (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0)
        << " corpse=" << (bot->GetCorpse() ? 1 : 0)
        << " combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0)
        << " afk=" << (bot->isAFK() ? 1 : 0)
        << " waiting=" << (isWaiting ? 1 : 0)
        << " delayMs=" << aiInternalUpdateDelay;

    Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    Spell* channeledSpell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    SpellEntry const* activeSpellInfo = currentSpell ? currentSpell->m_spellInfo :
        (channeledSpell ? channeledSpell->m_spellInfo : nullptr);
    out << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0)
        << " currentSpell=" << (activeSpellInfo ? activeSpellInfo->SpellName[0] : "none")
        << " currentSpellId=" << (activeSpellInfo ? activeSpellInfo->Id : 0);

    return out.str();
}

void PlayerbotAI::LogActionMetricsSnapshot(time_t now)
{
    if (!sPlayerbotAIConfig.hasLog("bot_events.csv") || !actionMetricWindowStart || actionMetricTicks == 0)
        return;

    const double elapsedSeconds = std::max(1.0, difftime(now, actionMetricWindowStart));
    const double perMinute = 60.0 / elapsedSeconds;
    const uint32 apm = static_cast<uint32>(std::round(actionMetricExecutedTicks * perMinute));
    const uint32 decisionTpm = static_cast<uint32>(std::round(actionMetricTicks * perMinute));

    std::ostringstream out;
    out << "apm=" << apm
        << " decisionTpm=" << decisionTpm
        << " windowSec=" << static_cast<uint32>(elapsedSeconds)
        << " executedTicks=" << actionMetricExecutedTicks
        << " minimalTicks=" << actionMetricMinimalTicks
        << " afkTicks=" << actionMetricAfkTicks
        << " movingTicks=" << actionMetricMovingTicks
        << " combatTicks=" << actionMetricCombatTicks
        << " targetTicks=" << actionMetricTargetTicks
        << " travelTicks=" << actionMetricTravelTicks
        << " questTravelTicks=" << actionMetricQuestTravelTicks
        << " moveProgressTicks=" << actionMetricMoveProgressTicks
        << " staleResets=" << actionMetricStaleResets
        << " noProgressTicks=" << actionMetricNoProgressTicks
        << " sameActionStreak=" << actionMetricSameActionStreak
        << " sinceProgressSec=" << (actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0);

    sPlayerbotAIConfig.logEvent(this, "ActionRateTrace", actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str());
    sPlayerbotAIConfig.logEvent(this, "ActionTaskTrace", "task", BuildCurrentTaskSummary());
    LogBotStatsSnapshot(now, apm, decisionTpm, actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0);

    actionMetricWindowStart = now;
    actionMetricTicks = 0;
    actionMetricExecutedTicks = 0;
    actionMetricMinimalTicks = 0;
    actionMetricAfkTicks = 0;
    actionMetricMovingTicks = 0;
    actionMetricCombatTicks = 0;
    actionMetricTargetTicks = 0;
    actionMetricTravelTicks = 0;
    actionMetricQuestTravelTicks = 0;
    actionMetricMoveProgressTicks = 0;
    actionMetricStaleResets = 0;
    actionMetricNoProgressTicks = 0;
}

void PlayerbotAI::LogBotStatsSnapshot(time_t now, uint32 apm, uint32 decisionTpm, uint32 sinceProgressSec)
{
    if (!sPlayerbotAIConfig.hasLog("bot_events.csv"))
        return;

    uint32 questLogCount = 0;
    uint32 questCompleteCount = 0;
    uint32 questIncompleteCount = 0;
    uint32 questRewardReadyCount = 0;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        ++questLogCount;
        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_COMPLETE)
        {
            ++questCompleteCount;
            if (!bot->GetQuestRewardStatus(questId))
                ++questRewardReadyCount;
        }
        else if (status == QUEST_STATUS_INCOMPLETE)
        {
            ++questIncompleteCount;
        }
    }

    TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
    TravelDestination* destination = travelTarget ? travelTarget->GetDestination() : nullptr;
    Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
    LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();

    std::ostringstream out;
    out << "level=" << uint32(bot->GetLevel())
        << " xp=" << bot->GetUInt32Value(PLAYER_XP)
        << " nextXp=" << bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
        << " money=" << bot->GetMoney()
        << " map=" << bot->GetMapId()
        << " x=" << std::fixed << std::setprecision(2) << bot->GetPositionX()
        << " y=" << bot->GetPositionY()
        << " z=" << bot->GetPositionZ()
        << " dead=" << (sServerFacade.UnitIsDead(bot) ? 1 : 0)
        << " ghost=" << (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0)
        << " moving=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
        << " combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0)
        << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0)
        << " apm=" << apm
        << " decisionTpm=" << decisionTpm
        << " sinceProgressSec=" << sinceProgressSec
        << " quests=" << questLogCount
        << " questComplete=" << questCompleteCount
        << " questIncomplete=" << questIncompleteCount
        << " questRewardReady=" << questRewardReadyCount
        << " target=" << (currentTarget ? currentTarget->GetName() : "none")
        << " loot=" << (!lootTarget.IsEmpty() ? 1 : 0)
        << " travelStatus=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
        << " travelEntry=" << (travelTarget ? travelTarget->GetEntry() : 0)
        << " travelPtr=" << (destination ? 1 : 0);

    sPlayerbotAIConfig.logEvent(this, "BotStatsSnapshot", "", out.str());
}

void PlayerbotAI::LogVisibleActivitySnapshot(time_t now)
{
    if (!sPlayerbotAIConfig.hasLog("bot_events.csv") || !actionMetricWindowStart || actionMetricTicks == 0)
        return;

    ActivePiorityType priorityType = GetPriorityType();
    if (!IsForegroundPriority(priorityType))
        return;

    AiObjectContext* context = aiObjectContext;
    const double elapsedSeconds = std::max(1.0, difftime(now, actionMetricWindowStart));
    const double perMinute = 60.0 / elapsedSeconds;
    const uint32 apm = static_cast<uint32>(std::round(actionMetricExecutedTicks * perMinute));
    const uint32 decisionTpm = static_cast<uint32>(std::round(actionMetricTicks * perMinute));
    const uint32 sinceProgressSec = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
    const bool unitDead = sServerFacade.UnitIsDead(bot);
    const bool ghost = bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
    Corpse* corpse = bot->GetCorpse();
    const bool hasCorpse = corpse;
    const bool corpseRecoveryAction =
        (unitDead || ghost || currentState == BotState::BOT_STATE_DEAD) &&
        (currentState == BotState::BOT_STATE_DEAD || hasCorpse || ghost);

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
    const bool combat = sServerFacade.IsInCombat(bot);
    const bool casting = bot->IsNonMeleeSpellCasted(true, false, true);
    GuidPosition rpgTarget = AI_VALUE(GuidPosition, "rpg target");
    const float corpseDist = corpse ? WorldPosition(bot).distance(WorldPosition(corpse)) : 0.0f;
    const bool reclaimReady = corpse ?
        corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) <= now :
        false;
    const bool validCorpseReclaimWait =
        corpseRecoveryAction &&
        corpse &&
        !reclaimReady &&
        corpseDist < CORPSE_RECLAIM_RADIUS - 5.0f;
    const bool passiveAction =
        actionMetricLastActionName.empty() ||
        actionMetricLastActionName == "check values" ||
        actionMetricLastActionName == "update pve strats" ||
        actionMetricLastActionName == "emote" ||
        actionMetricLastActionName == "cannibalize" ||
        actionMetricLastActionName == "reset raids" ||
        actionMetricLastActionName == "clean quest log" ||
        actionMetricLastActionName == "reset travel target";

    TravelDestination* visibleTravelDestination = travelTarget ? travelTarget->GetDestination() : nullptr;
    const bool hasRealTravelDestination =
        visibleTravelDestination && !dynamic_cast<NullTravelDestination*>(visibleTravelDestination);

    std::string visibleState = "active";
    if (validCorpseReclaimWait)
        visibleState = "corpse_wait";
    else if (corpseRecoveryAction)
        visibleState = "corpse_recovery";
    else if (currentTarget)
        visibleState = "target_stall";
    else if (!lootTarget.IsEmpty())
        visibleState = "loot_stall";
    else if (travelTarget && hasRealTravelDestination)
    {
        switch (travelTarget->GetStatus())
        {
            case TravelStatus::TRAVEL_STATUS_PREPARE:
                visibleState = "travel_prepare";
                break;
            case TravelStatus::TRAVEL_STATUS_TRAVEL:
                visibleState = "travel_move";
                break;
            case TravelStatus::TRAVEL_STATUS_WORK:
                visibleState = "travel_work";
                break;
            case TravelStatus::TRAVEL_STATUS_COOLDOWN:
                visibleState = "travel_cooldown";
                break;
            default:
                break;
        }
    }

    if (!corpseRecoveryAction && passiveAction && !moving && !combat && !currentTarget && lootTarget.IsEmpty())
        visibleState = "passive_loop";

    const bool hasTravelWork = IsActiveTravelWork(travelTarget) || IsPreparedTravelWork(travelTarget);
    const bool usefulTargetMovement =
        moving &&
        currentTarget &&
        (sinceProgressSec < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
    const bool usefulMoving =
        moving &&
        (combat || casting || usefulTargetMovement || !lootTarget.IsEmpty() || hasTravelWork || rpgTarget ||
            corpseRecoveryAction);

    if (!corpseRecoveryAction && visibleState == "active" && !usefulMoving && !combat && sinceProgressSec >= VISIBLE_IDLE_WARN_SECONDS)
        visibleState = "idle_loop";

    int32 healthScore = 100;
    std::string healthIssue = "ok";
    auto penalize = [&](int32 penalty, const char* issue)
    {
        healthScore = std::max<int32>(0, healthScore - penalty);
        if (healthIssue == "ok")
            healthIssue = issue;
    };

    const bool dead = unitDead || ghost || currentState == BotState::BOT_STATE_DEAD;
    const bool visibleTargetWork = IsVisibleTargetWork(bot, currentTarget, usefulTargetMovement);
    const bool visibleCombatWork = combat && (casting || visibleTargetWork || usefulMoving);
    const bool visibleTravelWork = hasTravelWork && usefulMoving;
    const bool visibleLootWork = !lootTarget.IsEmpty() && (usefulMoving || casting);
    const bool hasAssignedWork = currentTarget || !lootTarget.IsEmpty() || hasTravelWork || rpgTarget || combat;
    const bool hasVisibleWork =
        usefulMoving ||
        validCorpseReclaimWait ||
        casting ||
        visibleCombatWork ||
        visibleTargetWork ||
        visibleLootWork ||
        visibleTravelWork ||
        corpseRecoveryAction;
    const bool transientMetricWindow =
        elapsedSeconds < 5.0 &&
        sinceProgressSec < 5 &&
        actionMetricNoProgressTicks <= 3 &&
        actionMetricSameActionStreak < 20;
    const bool assignedButNoVisibleWork =
        hasAssignedWork &&
        !hasVisibleWork &&
        !validCorpseReclaimWait &&
        !transientMetricWindow &&
        sinceProgressSec >= VISIBLE_IDLE_WARN_SECONDS;

    if (!hasVisibleWork && !transientMetricWindow)
        penalize(55, "no_work");
    if (assignedButNoVisibleWork)
        penalize(45, "assigned_no_motion");
    if (dead && !usefulMoving && !corpseRecoveryAction)
        penalize(45, "dead_idle");
    else if (dead && !usefulMoving && !validCorpseReclaimWait)
        penalize(25, "dead_not_moving");
    if (visibleState == "passive_loop" && !transientMetricWindow)
        penalize(35, "passive_loop");
    else if (visibleState == "idle_loop")
        penalize(30, "idle_loop");
    else if ((visibleState == "travel_work" || visibleState == "travel_move") && !usefulMoving && !combat && !casting)
        penalize(30, "travel_no_motion");
    if (apm == 0 && !transientMetricWindow && !validCorpseReclaimWait)
        penalize(30, "zero_apm");
    else if (apm < 20 && !transientMetricWindow && !validCorpseReclaimWait)
        penalize(15, "low_apm");
    if (sinceProgressSec >= 20 && !validCorpseReclaimWait)
        penalize(35, "no_progress_20s");
    else if (sinceProgressSec >= VISIBLE_IDLE_RESCUE_SECONDS && !validCorpseReclaimWait)
        penalize(25, "no_progress_10s");
    else if (sinceProgressSec >= VISIBLE_IDLE_WARN_SECONDS && !validCorpseReclaimWait)
        penalize(15, "no_progress_5s");
    if (!usefulMoving && !validCorpseReclaimWait && !combat && !casting && decisionTpm >= 120 && apm < 30 && !transientMetricWindow)
        penalize(15, "busy_idle");
    if (actionMetricSameActionStreak >= 80)
        penalize(20, "same_action_80");
    else if (actionMetricSameActionStreak >= 30)
        penalize(10, "same_action_30");

    std::ostringstream out;
    out << "priority=" << static_cast<uint32>(priorityType)
        << " visibleState=" << visibleState
        << " healthScore=" << healthScore
        << " healthIssue=" << healthIssue
        << " apm=" << apm
        << " decisionTpm=" << decisionTpm
        << " windowSec=" << static_cast<uint32>(elapsedSeconds)
        << " executedTicks=" << actionMetricExecutedTicks
        << " noProgressTicks=" << actionMetricNoProgressTicks
        << " sameActionStreak=" << actionMetricSameActionStreak
        << " sinceProgressSec=" << sinceProgressSec
        << " moveProgressTicks=" << actionMetricMoveProgressTicks
        << " minimalTicks=" << actionMetricMinimalTicks
        << " transient=" << (transientMetricWindow ? 1 : 0)
        << " corpseWait=" << (validCorpseReclaimWait ? 1 : 0)
        << " corpseDist=" << std::fixed << std::setprecision(1) << corpseDist
        << " reclaimReady=" << (reclaimReady ? 1 : 0)
        << " " << BuildCurrentTaskSummary();

    if (!corpseRecoveryAction)
    {
        sPlayerbotAIConfig.logEvent(this, "VisibleBotActivityTrace",
            actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str());
    }

    if (healthScore < 60 && (!corpseRecoveryAction || sinceProgressSec >= VISIBLE_IDLE_RESCUE_SECONDS))
    {
        sPlayerbotAIConfig.logEvent(this, "BotHealthLow",
            actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str());
    }

    if (!corpseRecoveryAction && decisionTpm >= 20 && apm < 20 && sinceProgressSec >= VISIBLE_IDLE_WARN_SECONDS)
    {
        sPlayerbotAIConfig.logEvent(this, "VisibleBotLowApm",
            actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str());
    }

    if (!corpseRecoveryAction &&
        sinceProgressSec >= VISIBLE_IDLE_WARN_SECONDS &&
        (visibleState == "idle_loop" ||
         visibleState == "target_stall" ||
         visibleState == "loot_stall" ||
         visibleState == "passive_loop" ||
         visibleState == "travel_cooldown" ||
         visibleState == "travel_move" ||
         visibleState == "travel_work"))
    {
        sPlayerbotAIConfig.logEvent(this, "VisibleBotStuckState",
            actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str());
    }

    if (!corpseRecoveryAction && assignedButNoVisibleWork)
    {
        sPlayerbotAIConfig.logEvent(this, "VisibleAssignedNoMotion",
            actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str());
    }

    const bool hardAssignedStall =
        ENABLE_METRIC_RESCUES &&
        !corpseRecoveryAction &&
        assignedButNoVisibleWork &&
        sinceProgressSec >= VISIBLE_IDLE_WARN_SECONDS &&
        !dead &&
        !casting &&
        !bot->IsTaxiFlying() &&
        !bot->IsBeingTeleported() &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster();

    if (hardAssignedStall)
    {
        if (moving && !usefulMoving)
            StopMoving();

        ObjectGuid clearedTargetGuid;
        uint64 clearedLootGuid = 0;
        std::string clearedTargetName = "none";
        TravelStatus oldStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
        TravelDestination* oldDestination = travelTarget ? travelTarget->GetDestination() : nullptr;
        bool clearedTarget = false;
        bool clearedLoot = false;
        bool clearedTravel = false;
        bool clearedRpg = false;
        bool exitedCombat = false;

        if (currentTarget &&
            currentTarget->IsInWorld() &&
            currentTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, currentTarget))
        {
            clearedTargetGuid = currentTarget->GetObjectGuid();
            clearedTargetName = currentTarget->GetName();
            if (!sServerFacade.UnitIsDead(currentTarget))
                DeferTargetGuidForBot(bot, clearedTargetGuid, 12);

            RESET_AI_VALUE(bool, "has attackers");
            ClearCurrentCombatTarget(aiObjectContext, bot, currentTarget, true);
            OnCombatEnded();
            ResetStaleTargetState();
            clearedTarget = true;
            exitedCombat = true;
        }
        else if (combat && !bot->GetVictim())
        {
            bot->CombatStop(false);
            OnCombatEnded();
            ResetStaleTargetState();
            exitedCombat = true;
        }

        if (!lootTarget.IsEmpty())
        {
            clearedLootGuid = lootTarget.guid.GetCounter();
            DeferLootGuidForBot(bot, lootTarget.guid, 10);
            if (LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot"))
                availableLoot->Remove(lootTarget.guid);
            RESET_AI_VALUE(LootObject, "loot target");
            clearedLoot = true;
        }

        if (travelTarget)
        {
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            travelTarget->SetForced(false);
            RESET_AI_VALUE(bool, "travel target active");
            aiObjectContext->ClearValues("no active travel destinations");
            clearedTravel = true;
        }

        if (rpgTarget)
        {
            RESET_AI_VALUE(GuidPosition, "rpg target");
            clearedRpg = true;
        }

        RESET_AI_VALUE(ObjectGuid, "attack target");

        bool questRequested = false;
        bool travelChosen = false;
        bool targetSelected = false;
        bool attackAnything = false;
        bool grindRequested = false;
        bool travelMoved = false;
        bool rpgChosen = false;
        bool rpgMoved = false;
        bool randomMoved = false;
        bool idleNudged = false;

        PushVisibleWork(this, aiObjectContext, bot, "visible-assigned-stall",
            questRequested, travelChosen, targetSelected, attackAnything, grindRequested, travelMoved,
            rpgChosen, rpgMoved, randomMoved, idleNudged, true);

        Unit* rescuedTarget = AI_VALUE(Unit*, "current target");
        LootObject rescuedLoot = AI_VALUE(LootObject, "loot target");
        GuidPosition rescuedRpgTarget = AI_VALUE(GuidPosition, "rpg target");
        TravelTarget* rescuedTravel = AI_VALUE(TravelTarget*, "travel target");
        TravelDestination* rescuedDestination = rescuedTravel ? rescuedTravel->GetDestination() : nullptr;
        const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        const bool directedMovementWork =
            HasDirectedMovementWork(rescuedTravel, rescuedRpgTarget, travelMoved, rpgMoved, movingAfter);
        const bool fallbackMovementWork =
            HasFallbackMovementWork(randomMoved, idleNudged, movingAfter);
        const bool workAssigned =
            HasConcreteRecoveryWork(bot, rescuedTravel, rescuedTarget, rescuedLoot, movingAfter,
                travelMoved, false) ||
            directedMovementWork ||
            fallbackMovementWork;

        std::ostringstream rescueOut;
        rescueOut << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
            << " oldState=" << visibleState
            << " oldStatus=" << static_cast<uint32>(oldStatus)
            << " newStatus=" << (rescuedTravel ? static_cast<uint32>(rescuedTravel->GetStatus()) : 0)
            << " sinceProgressSec=" << sinceProgressSec
            << " noProgressTicks=" << actionMetricNoProgressTicks
            << " sameActionStreak=" << actionMetricSameActionStreak
            << " clearedTarget=" << (clearedTarget ? 1 : 0)
            << " clearedTargetGuid=" << clearedTargetGuid.GetCounter()
            << " clearedTargetName=" << clearedTargetName
            << " clearedLoot=" << (clearedLoot ? 1 : 0)
            << " clearedLootGuid=" << clearedLootGuid
            << " clearedTravel=" << (clearedTravel ? 1 : 0)
            << " clearedRpg=" << (clearedRpg ? 1 : 0)
            << " exitedCombat=" << (exitedCombat ? 1 : 0)
            << " quest=" << (questRequested ? 1 : 0)
            << " choose=" << (travelChosen ? 1 : 0)
            << " selectTarget=" << (targetSelected ? 1 : 0)
            << " attackAnything=" << (attackAnything ? 1 : 0)
            << " grindRequest=" << (grindRequested ? 1 : 0)
            << " rpgChoose=" << (rpgChosen ? 1 : 0)
            << " travelMove=" << (travelMoved ? 1 : 0)
            << " rpgMove=" << (rpgMoved ? 1 : 0)
            << " randomMove=" << (randomMoved ? 1 : 0)
            << " idleNudge=" << (idleNudged ? 1 : 0)
            << " directedMove=" << (directedMovementWork ? 1 : 0)
            << " fallbackMove=" << (fallbackMovementWork ? 1 : 0)
            << " movingAfter=" << (movingAfter ? 1 : 0)
            << " target=" << (rescuedTarget ? rescuedTarget->GetName() : "none")
            << " loot=" << (!rescuedLoot.IsEmpty() ? 1 : 0)
            << " rpgTarget=" << (rescuedRpgTarget ? 1 : 0)
            << " workAssigned=" << (workAssigned ? 1 : 0)
            << " destinationPtr=" << (rescuedDestination ? 1 : (oldDestination ? 1 : 0));
        sPlayerbotAIConfig.logEvent(this, "VisibleAssignedStallRescue", "", rescueOut.str());

        if (workAssigned)
            actionMetricLastProgress = now;
    }

    const bool hardVisibleIdle =
        ENABLE_METRIC_RESCUES &&
        !corpseRecoveryAction &&
        !hasVisibleWork &&
        sinceProgressSec >= VISIBLE_IDLE_RESCUE_SECONDS &&
        !validCorpseReclaimWait;

    if (((healthScore == 0 && healthIssue == "no_work") || hardVisibleIdle) &&
        !corpseRecoveryAction &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        currentState != BotState::BOT_STATE_DEAD &&
        !dead &&
        !combat &&
        !casting &&
        !currentTarget &&
        lootTarget.IsEmpty())
    {
        if (moving && !usefulMoving)
            StopMoving();

        TravelStatus oldStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
        TravelDestination* oldDestination = travelTarget ? travelTarget->GetDestination() : nullptr;
        if (travelTarget)
        {
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            travelTarget->SetForced(false);
        }

        RESET_AI_VALUE(bool, "travel target active");
        RESET_AI_VALUE(GuidPosition, "rpg target");
        RESET_AI_VALUE(ObjectGuid, "attack target");
        aiObjectContext->GetValue<bool>("no active travel destinations", "quest")->Reset();

        bool questRequested = false;
        bool travelChosen = false;
        bool targetSelected = false;
        bool attackAnything = false;
        bool grindRequested = false;
        bool travelMoved = false;
        bool rpgChosen = false;
        bool rpgMoved = false;
        bool randomMoved = false;
        bool idleNudged = false;

        auto visibleHasImmediateWork = [&]() -> bool
        {
            Unit* workTarget = AI_VALUE(Unit*, "current target");
            LootObject workLoot = AI_VALUE(LootObject, "loot target");
            GuidPosition workRpgTarget = AI_VALUE(GuidPosition, "rpg target");
            TravelTarget* workTravel = AI_VALUE(TravelTarget*, "travel target");
            const bool workMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovement =
                HasDirectedMovementWork(workTravel, workRpgTarget, travelMoved, rpgMoved, workMoving);
            return HasUsefulAssignedWork(bot, workTravel, workTarget, workLoot, workMoving, directedMovement);
        };

        auto visibleTryAssignedTravelMove = [&]() -> bool
        {
            const bool moved = MoveAssignedTravelWork(this, aiObjectContext, bot, "visible-no-work");
            travelMoved = travelMoved || moved;
            return moved;
        };

        questRequested = DoSpecificAction("request quest travel target", Event(), true);
        if (questRequested)
            visibleTryAssignedTravelMove();

        if (!visibleHasImmediateWork())
        {
            travelChosen = DoSpecificAction("choose travel target", Event(), true);
            if (travelChosen)
                visibleTryAssignedTravelMove();
        }

        if (!visibleHasImmediateWork())
        {
            ResetTargetScanCaches(aiObjectContext);
            targetSelected = DoSpecificAction("select new target", Event(), true);
        }

        if (!visibleHasImmediateWork())
            attackAnything = DoSpecificAction("attack anything", Event(), true);

        if (!visibleHasImmediateWork())
        {
            Unit* selectedTarget = AI_VALUE(Unit*, "current target");
            if (selectedTarget)
                attackAnything = ForceConcreteTargetWork(this, bot, selectedTarget, "visible-no-work-target-handoff") ||
                    attackAnything;
        }

        if (!visibleHasImmediateWork())
        {
            grindRequested = RequestGrindTravelFallback(this, aiObjectContext, bot, "visible-no-work");
            if (grindRequested)
                visibleTryAssignedTravelMove();
        }

        const bool allowVisibleFallbackMovement = false;
        if (allowVisibleFallbackMovement && !visibleHasImmediateWork())
        {
            rpgChosen = DoSpecificAction("choose rpg target", Event(), true);
            rpgMoved = DoSpecificAction("move to rpg target", Event(), true);
            if (!visibleHasImmediateWork() && !rpgMoved)
                randomMoved = DoSpecificAction("move random", Event("visible no work", "emergency rescue", bot), true);
            if (!visibleHasImmediateWork() && !randomMoved && !sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
                idleNudged = ForceIdleRescueNudge(this, bot, "visible-no-work");
        }

        Unit* rescuedTarget = AI_VALUE(Unit*, "current target");
        LootObject rescuedLoot = AI_VALUE(LootObject, "loot target");
        GuidPosition rescuedRpgTarget = AI_VALUE(GuidPosition, "rpg target");
        TravelTarget* rescuedTravel = AI_VALUE(TravelTarget*, "travel target");
        TravelDestination* rescuedDestination = rescuedTravel ? rescuedTravel->GetDestination() : nullptr;
        const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        const bool directedMovementWork =
            HasDirectedMovementWork(rescuedTravel, rescuedRpgTarget, travelMoved, rpgMoved, movingAfter);
        const bool workAssigned =
            HasConcreteRecoveryWork(bot, rescuedTravel, rescuedTarget, rescuedLoot, movingAfter,
                travelMoved, false);

        std::ostringstream rescueOut;
        rescueOut << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
            << " oldStatus=" << static_cast<uint32>(oldStatus)
            << " newStatus=" << (rescuedTravel ? static_cast<uint32>(rescuedTravel->GetStatus()) : 0)
            << " sinceProgressSec=" << sinceProgressSec
            << " sameActionStreak=" << actionMetricSameActionStreak
            << " hardIdle=" << (hardVisibleIdle ? 1 : 0)
            << " quest=" << (questRequested ? 1 : 0)
            << " choose=" << (travelChosen ? 1 : 0)
            << " selectTarget=" << (targetSelected ? 1 : 0)
            << " attackAnything=" << (attackAnything ? 1 : 0)
            << " grindRequest=" << (grindRequested ? 1 : 0)
            << " rpgChoose=" << (rpgChosen ? 1 : 0)
            << " travelMove=" << (travelMoved ? 1 : 0)
            << " rpgMove=" << (rpgMoved ? 1 : 0)
            << " randomMove=" << (randomMoved ? 1 : 0)
            << " idleNudge=" << (idleNudged ? 1 : 0)
            << " nudgeOnly=" << ((randomMoved || idleNudged) && !workAssigned ? 1 : 0)
            << " directedMove=" << (directedMovementWork ? 1 : 0)
            << " movingAfter=" << (movingAfter ? 1 : 0)
            << " target=" << (rescuedTarget ? rescuedTarget->GetName() : "none")
            << " loot=" << (!rescuedLoot.IsEmpty() ? 1 : 0)
            << " rpgTarget=" << (rescuedRpgTarget ? 1 : 0)
            << " workAssigned=" << (workAssigned ? 1 : 0)
            << " destinationPtr=" << (rescuedDestination ? 1 : (oldDestination ? 1 : 0));
        sPlayerbotAIConfig.logEvent(this, "VisibleNoWorkEmergencyRescue", "", rescueOut.str());

        if (workAssigned)
            actionMetricLastProgress = now;
    }
}

void PlayerbotAI::LogDecisionStallSnapshot(time_t now, const std::string& loopReason)
{
    if (!sPlayerbotAIConfig.hasLog("bot_events.csv"))
        return;

    std::ostringstream out;
    out << "reason=" << loopReason
        << " sinceProgressSec=" << (actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0)
        << " sameActionStreak=" << actionMetricSameActionStreak;

    sPlayerbotAIConfig.logEvent(this, "DecisionLoopTrace", actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName, out.str() + " " + BuildCurrentTaskSummary());
}

void PlayerbotAI::TrackActionMetrics(bool minimal, bool actionExecuted, bool staleReset)
{
    AiObjectContext* context = aiObjectContext;
    const time_t now = time(0);

    if (!actionMetricWindowStart)
    {
        actionMetricWindowStart = now;
        actionMetricLastProgress = now;
        actionMetricLastSnapshot = now;
        actionMetricHasLastMovePosition = true;
        actionMetricLastMoveMap = bot->GetMapId();
        actionMetricLastMoveX = bot->GetPositionX();
        actionMetricLastMoveY = bot->GetPositionY();
        actionMetricLastMoveZ = bot->GetPositionZ();
    }

    ++actionMetricTicks;

    if (minimal)
        ++actionMetricMinimalTicks;
    if (bot->isAFK())
        ++actionMetricAfkTicks;
    const bool movementIntent = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
    bool movementProgress = false;
    const uint32 currentMoveMap = bot->GetMapId();
    const float currentMoveX = bot->GetPositionX();
    const float currentMoveY = bot->GetPositionY();
    const float currentMoveZ = bot->GetPositionZ();
    if (movementIntent)
    {
        if (actionMetricHasLastMovePosition && actionMetricLastMoveMap == currentMoveMap)
        {
            const float dx = currentMoveX - actionMetricLastMoveX;
            const float dy = currentMoveY - actionMetricLastMoveY;
            const float dz = currentMoveZ - actionMetricLastMoveZ;
            // This runs at server tick cadence. With many bots the per-update
            // movement delta can be far below a quarter yard even while the bot
            // is visibly walking, so use a small threshold and let the stall
            // timer catch true no-progress cases.
            movementProgress = (dx * dx + dy * dy + dz * dz) >= 0.03f * 0.03f;
        }
        else if (actionMetricHasLastMovePosition)
        {
            movementProgress = true;
        }
    }

    actionMetricHasLastMovePosition = true;
    actionMetricLastMoveMap = currentMoveMap;
    actionMetricLastMoveX = currentMoveX;
    actionMetricLastMoveY = currentMoveY;
    actionMetricLastMoveZ = currentMoveZ;

    if (movementIntent)
        ++actionMetricMovingTicks;
    if (movementProgress)
        ++actionMetricMoveProgressTicks;
    if (sServerFacade.IsInCombat(bot))
        ++actionMetricCombatTicks;
    if (AI_VALUE(Unit*, "current target"))
        ++actionMetricTargetTicks;

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    TravelDestination* destination = travelTarget ? travelTarget->GetDestination() : nullptr;
    const bool travelActive = travelTarget && travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE;
    const bool travelWorkActive = IsActiveTravelWork(travelTarget);
    if (travelActive)
        ++actionMetricTravelTicks;
    if (destination)
    {
        TravelDestinationPurpose purpose = destination->GetPurpose();
        if (purpose == TravelDestinationPurpose::QuestGiver ||
            purpose == TravelDestinationPurpose::QuestTaker ||
            purpose == TravelDestinationPurpose::QuestObjective1 ||
            purpose == TravelDestinationPurpose::QuestObjective2 ||
            purpose == TravelDestinationPurpose::QuestObjective3 ||
            purpose == TravelDestinationPurpose::QuestObjective4)
            ++actionMetricQuestTravelTicks;
    }

    const Action* lastAction = GetLastExecutedAction(currentState);
    const std::string lastActionName = lastAction ? const_cast<Action*>(lastAction)->getName() : "";
    if (!lastActionName.empty())
    {
        if (lastActionName == actionMetricLastActionName)
            ++actionMetricSameActionStreak;
        else
            actionMetricSameActionStreak = 1;
        actionMetricLastActionName = lastActionName;
    }
    else
    {
        actionMetricSameActionStreak = 0;
        actionMetricLastActionName.clear();
    }

    if (staleReset)
        ++actionMetricStaleResets;

    const bool currentCombat = sServerFacade.IsInCombat(bot);
    const bool currentCasting = bot->IsNonMeleeSpellCasted(true, false, true);
    Unit* metricCurrentTarget = AI_VALUE(Unit*, "current target");
    const bool hasCurrentTarget = metricCurrentTarget != nullptr;
    const bool hasLootTarget = !AI_VALUE(LootObject, "loot target").IsEmpty();
    GuidPosition activeRpgTarget = AI_VALUE(GuidPosition, "rpg target");
    const bool usefulMovementProgress =
        movementProgress &&
        (currentCombat || currentCasting || hasCurrentTarget || hasLootTarget || travelWorkActive || activeRpgTarget);
    const bool visibleTargetWork = IsVisibleTargetWork(bot, metricCurrentTarget, movementProgress);
    // A bot standing still while actively meleeing its target is doing REAL work,
    // not stalling. The old streak-based "melee swing loop" detector flipped on at
    // sameActionStreak >= 60 and made hasLiveWork=false, so the no-progress timer
    // climbed during every melee fight and tripped the rescue system below, which
    // yanked the bot off its target into a fake "look active" move. Never treat an
    // active melee swing as "no progress".
    const bool meleeSwingLoop = false;
    const bool hasLiveWork =
        usefulMovementProgress ||
        currentCasting ||
        (visibleTargetWork && !meleeSwingLoop);
    const bool fillerNoWorkAction =
        lastActionName.empty() ||
        lastActionName == "none" ||
        lastActionName == "check values" ||
        lastActionName == "update pve strats" ||
        lastActionName == "emote" ||
        lastActionName == "jump" ||
        lastActionName == "cannibalize" ||
        lastActionName == "drink" ||
        lastActionName == "food" ||
        lastActionName == "clean quest log" ||
        lastActionName == "reset raids" ||
        lastActionName == "reset travel target" ||
        // NOTE: the bot's own PLANNING/navigation actions (choose travel target,
        // request quest travel target, request named travel target, choose rpg
        // target, move to rpg target) are NOT "filler / no work" - they are the bot
        // actively deciding where to go and pursuing it. Counting them as no-work is
        // what made a normally-questing bot trip the idle/stall rescue cascades every
        // ~6-12 ticks, which then cleared the travel target it had just picked
        // (self-perpetuating churn). They are intentionally NOT listed here.
        lastActionName == "move random" ||
        lastActionName == "add all loot" ||
        lastActionName == "loot" ||
        lastActionName == "store loot" ||
        lastActionName == "attack anything" ||
        lastActionName == "attack my target" ||
        lastActionName == "accept trade" ||
        lastActionName == "accept invitation" ||
        lastActionName == "invite nearby" ||
        lastActionName == "rpg duel" ||
        lastActionName == "rpg gossip talk" ||
        lastActionName == "rpg emote" ||
        lastActionName == "rpg stay" ||
        lastActionName == "rpg use" ||
        lastActionName == "rpg sell" ||
        lastActionName == "rpg cancel";
    const bool usefulActionProgress =
        actionExecuted &&
        (hasLiveWork || (!fillerNoWorkAction && !currentCombat && !hasCurrentTarget));

    if (usefulActionProgress || staleReset || usefulMovementProgress)
    {
        ++actionMetricExecutedTicks;
        actionMetricLastProgress = now;
        actionMetricNoProgressTicks = 0;
    }
    else
    {
        if (actionExecuted)
            ++actionMetricExecutedTicks;
        ++actionMetricNoProgressTicks;
    }

    if ((now - actionMetricLastSnapshot) >= 60)
    {
        LogActionMetricsSnapshot(now);
        actionMetricLastSnapshot = now;
    }

    const time_t noProgressSecs = actionMetricLastProgress ? (now - actionMetricLastProgress) : 0;

    if (!minimal &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        currentState != BotState::BOT_STATE_DEAD &&
        !sServerFacade.UnitIsDead(bot) &&
        !bot->IsTaxiFlying() &&
        !bot->IsBeingTeleported() &&
        !currentCasting &&
        !currentCombat &&
        ENABLE_METRIC_RESCUES &&
        noProgressSecs >= 6)
    {
        // NOTE: this whole "hard idle / stall rescue" block is now gated on
        // !currentCombat. A bot in real combat must NEVER be declared stalled,
        // have its target deferred/cleared, or be force-moved — that was the
        // "fake move every melee fight" behavior. Reachability while fighting is
        // handled in AttackAction (relocate-to-unreachable), not by this rescue.
        Unit* hardTarget = AI_VALUE(Unit*, "current target");
        LootObject hardLoot = AI_VALUE(LootObject, "loot target");
        TravelTarget* hardTravel = AI_VALUE(TravelTarget*, "travel target");
        TravelDestination* hardDestination = hardTravel ? hardTravel->GetDestination() : nullptr;
        const bool hardDestinationPresent = hardDestination != nullptr;
        const TravelStatus hardOldStatus = hardTravel ? hardTravel->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
        GuidPosition hardRpgTarget = AI_VALUE(GuidPosition, "rpg target");
        const bool hardNullDestination = !hardDestinationPresent;
        const bool hardTravelActive = IsActiveTravelWork(hardTravel);
        const bool hardTravelPreparing = hardOldStatus == TravelStatus::TRAVEL_STATUS_PREPARE;
        const bool hardTargetVisible =
            hardTarget &&
            hardTarget->IsInWorld() &&
            hardTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, hardTarget);
        const bool hardMeleeConnected =
            hardTargetVisible &&
            bot->GetVictim() == hardTarget &&
            bot->CanReachWithMeleeAutoAttack(hardTarget) &&
            bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
        const bool hardFakeMovement =
            movementIntent && !movementProgress &&
            (actionMetricNoProgressTicks >= 12 || actionMetricSameActionStreak >= 12 || noProgressSecs >= 8);
        const bool hardCombatStall =
            currentCombat &&
            hardTargetVisible &&
            !hardMeleeConnected &&
            !movementProgress &&
            (actionMetricNoProgressTicks >= 12 || actionMetricSameActionStreak >= 12 || noProgressSecs >= 8);
        // A bot connected in melee with its target in real combat is NEVER stalled.
        // The old streak-based "melee loop stall" declared an actively-fighting bot
        // stuck after 45 identical swings and deferred its target for 20 ticks, making
        // it run off mid-fight and return — the "fake move every melee fight" cheat.
        // Disabled: real melee combat is real work.
        const bool hardMeleeLoopStall = false;
        const bool hardNoVisibleWork =
            !currentCombat &&
            !movementProgress &&
            !hardMeleeConnected &&
            (fillerNoWorkAction || actionMetricNoProgressTicks >= 12 || actionMetricSameActionStreak >= 12);
        const bool hardTravelStall =
            hardTravel &&
            !movementProgress &&
            ((!hardTravelPreparing && hardNullDestination) ||
             hardOldStatus == TravelStatus::TRAVEL_STATUS_NONE ||
             hardOldStatus == TravelStatus::TRAVEL_STATUS_COOLDOWN ||
             hardOldStatus == TravelStatus::TRAVEL_STATUS_EXPIRED ||
             (hardTravelPreparing && noProgressSecs >= 20) ||
             (hardTravelActive && (actionMetricNoProgressTicks >= 12 || noProgressSecs >= 8)));
        const bool hardLootStall =
            !hardLoot.IsEmpty() &&
            !movementProgress &&
            (actionMetricNoProgressTicks >= 12 || actionMetricSameActionStreak >= 12 || noProgressSecs >= 8);
        const bool hardTargetStall =
            hardTargetVisible &&
            !hardMeleeConnected &&
            !movementProgress &&
            !currentCasting &&
            (actionMetricNoProgressTicks >= 12 || actionMetricSameActionStreak >= 12 || noProgressSecs >= 8);

        if (hardFakeMovement || hardCombatStall || hardMeleeLoopStall || hardNoVisibleWork ||
            hardTravelStall || hardLootStall || hardTargetStall)
        {
            bool clearedTravel = false;
            bool clearedTarget = false;
            bool clearedLoot = false;
            bool clearedRpg = false;
            ObjectGuid targetGuid;
            uint64 lootGuid = 0;
            std::string targetName = "none";

            if (movementIntent && !movementProgress)
                StopMoving();

            if (!hardLoot.IsEmpty())
            {
                lootGuid = hardLoot.guid.GetCounter();
                DeferLootGuidForBot(bot, hardLoot.guid, 10);
                if (LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot"))
                    availableLoot->Remove(hardLoot.guid);
                RESET_AI_VALUE(LootObject, "loot target");
                clearedLoot = true;
            }

            if (hardTargetVisible && (!currentCombat || hardCombatStall || hardMeleeLoopStall || hardTargetStall))
            {
                targetGuid = hardTarget->GetObjectGuid();
                targetName = hardTarget->GetName();
                const bool hardTargetDead = sServerFacade.UnitIsDead(hardTarget);
                if (!hardTargetDead && (hardCombatStall || hardMeleeLoopStall || hardTargetStall))
                {
                    DeferTargetGuidForBot(bot, targetGuid, hardMeleeLoopStall ? 20 : 12);
                    ResetTargetScanCaches(context);
                }
                RESET_AI_VALUE(bool, "has attackers");
                ClearCurrentCombatTarget(context, bot, hardTarget, !hardTargetDead);
                if (!hardTargetDead)
                    bot->CombatStop(false);
                OnCombatEnded();
                ResetStaleTargetState();
                clearedTarget = true;
            }

            const bool hardTravelInvalid =
                hardTravel &&
                (!hardTravelPreparing && hardNullDestination ||
                 hardOldStatus == TravelStatus::TRAVEL_STATUS_NONE ||
                 hardOldStatus == TravelStatus::TRAVEL_STATUS_COOLDOWN ||
                 hardOldStatus == TravelStatus::TRAVEL_STATUS_EXPIRED);
            if (hardTravelInvalid)
            {
                hardTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                hardTravel->SetForced(false);
                RESET_AI_VALUE(bool, "travel target active");
                context->ClearValues("no active travel destinations");
                clearedTravel = true;
            }

            if (hardRpgTarget)
            {
                RESET_AI_VALUE(GuidPosition, "rpg target");
                clearedRpg = true;
            }

            bool questRequested = false;
            bool travelChosen = false;
            bool targetSelected = false;
            bool attackAnything = false;
            bool grindRequested = false;
            bool travelMoved = false;
            bool rpgChosen = false;
            bool rpgMoved = false;
            bool randomMoved = false;
            bool idleNudged = false;
            const bool suppressLocalTargetRetry =
                hardMeleeLoopStall &&
                !movementProgress &&
                noProgressSecs >= 10;

            auto hasImmediateWork = [&]() -> bool
            {
                Unit* workTarget = AI_VALUE(Unit*, "current target");
                LootObject workLoot = AI_VALUE(LootObject, "loot target");
                GuidPosition workRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                TravelTarget* workTravel = AI_VALUE(TravelTarget*, "travel target");
                const bool workMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool directedMovement =
                    HasDirectedMovementWork(workTravel, workRpgTarget, travelMoved, rpgMoved, workMoving);
                return HasUsefulAssignedWork(bot, workTravel, workTarget, workLoot, workMoving, directedMovement);
            };

            auto tryAssignedTravelMove = [&]() -> bool
            {
                const bool moved = MoveAssignedTravelWork(this, context, bot, "hard-idle");
                travelMoved = travelMoved || moved;
                return moved;
            };

            if (IsActiveTravelWork(hardTravel))
                tryAssignedTravelMove();

            questRequested = DoSpecificAction("request quest travel target", Event(), true);
            if (questRequested)
                tryAssignedTravelMove();

            if (!hasImmediateWork())
            {
                travelChosen = DoSpecificAction("choose travel target", Event(), true);
                if (travelChosen)
                    tryAssignedTravelMove();
            }

            if (!suppressLocalTargetRetry && !hasImmediateWork())
            {
                ResetTargetScanCaches(context);
                targetSelected = DoSpecificAction("select new target", Event(), true);
            }

            // Selecting a target is not visible work. Immediately push the target
            // through attack/reach so melee bots do not stand staring at mobs.
            if (!suppressLocalTargetRetry && !hasImmediateWork())
                attackAnything = DoSpecificAction("attack anything", Event(), true);

            if (!suppressLocalTargetRetry && !hasImmediateWork())
            {
                Unit* selectedTarget = AI_VALUE(Unit*, "current target");
                if (selectedTarget)
                    attackAnything = ForceConcreteTargetWork(this, bot, selectedTarget, "hard-idle-target-handoff") ||
                        attackAnything;
            }

            if (!hasImmediateWork())
            {
                grindRequested = RequestGrindTravelFallback(this, context, bot, "hard-idle");
                if (grindRequested)
                    tryAssignedTravelMove();
            }

            // If planning cannot produce quest/travel/target work, do not leave
            // the bot visually dead. A small RPG/random/nudge move keeps it
            // participating while the next planner tick finds stronger work.
            const bool allowHardIdleFallbackMovement = true;
            if (allowHardIdleFallbackMovement && !hasImmediateWork())
            {
                rpgChosen = DoSpecificAction("choose rpg target", Event(), true);
                rpgMoved = DoSpecificAction("move to rpg target", Event(), true);
                if (!hasImmediateWork() && !rpgMoved)
                    randomMoved = DoSpecificAction("move random", Event("hard idle", "visible hard idle rescue", bot), true);
                if (!hasImmediateWork() && !randomMoved && !sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
                    idleNudged = ForceIdleRescueNudge(this, bot, "hard-idle");
            }

            Unit* newTarget = AI_VALUE(Unit*, "current target");
            const std::string newTargetName =
                (newTarget && newTarget->IsInWorld()) ? newTarget->GetName() : "none";
            LootObject newLoot = AI_VALUE(LootObject, "loot target");
            GuidPosition newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
            TravelTarget* newTravel = AI_VALUE(TravelTarget*, "travel target");
            const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovementWork =
                HasDirectedMovementWork(newTravel, newRpgTarget, travelMoved, rpgMoved, movingAfter);
            bool reacquiredClearedTarget =
                targetGuid &&
                newTarget &&
                newTarget->GetObjectGuid() == targetGuid &&
                (hardCombatStall || hardMeleeLoopStall || hardTargetStall) &&
                !bot->IsNonMeleeSpellCasted(true, false, true);
            if (reacquiredClearedTarget)
            {
                DeferTargetGuidForBot(bot, targetGuid, hardMeleeLoopStall ? 30 : 18);
                ClearCurrentCombatTarget(context, bot, newTarget, true);
                OnCombatEnded();
                ResetStaleTargetState();

                PushVisibleWork(this, context, bot, "hard-idle-reacquire-clear",
                    questRequested, travelChosen, targetSelected, attackAnything, grindRequested, travelMoved,
                    rpgChosen, rpgMoved, randomMoved, idleNudged, true, !suppressLocalTargetRetry);

                newTarget = AI_VALUE(Unit*, "current target");
                newLoot = AI_VALUE(LootObject, "loot target");
                newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                newTravel = AI_VALUE(TravelTarget*, "travel target");
            }

            if (newTarget &&
                !suppressLocalTargetRetry &&
                !bot->IsNonMeleeSpellCasted(true, false, true) &&
                !(bot->GetVictim() == newTarget && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING)))
            {
                attackAnything = ForceConcreteTargetWork(this, bot, newTarget, "hard-idle-target-handoff") ||
                    attackAnything;
                newTarget = AI_VALUE(Unit*, "current target");
                newLoot = AI_VALUE(LootObject, "loot target");
                newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                newTravel = AI_VALUE(TravelTarget*, "travel target");
            }

            const bool staleSuppressedTarget =
                suppressLocalTargetRetry &&
                newTarget &&
                !bot->IsNonMeleeSpellCasted(true, false, true) &&
                !(bot->GetVictim() == newTarget &&
                  bot->CanReachWithMeleeAutoAttack(newTarget) &&
                  bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING));
            if (staleSuppressedTarget)
            {
                DeferTargetGuidForBot(bot, newTarget->GetObjectGuid(), hardMeleeLoopStall ? 30 : 18);
                ClearCurrentCombatTarget(context, bot, newTarget, true);
                OnCombatEnded();
                ResetStaleTargetState();
                ResetTargetScanCaches(context);

                newTarget = nullptr;
                newLoot = AI_VALUE(LootObject, "loot target");
                newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                newTravel = AI_VALUE(TravelTarget*, "travel target");
            }

            const bool movingAfterReacquireClear = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovementAfterReacquireClear =
                HasDirectedMovementWork(newTravel, newRpgTarget, travelMoved, rpgMoved, movingAfterReacquireClear);
            const bool concreteRecoveryWork =
                HasConcreteRecoveryWork(bot, newTravel, newTarget, newLoot, movingAfterReacquireClear,
                    travelMoved, false) &&
                (!reacquiredClearedTarget || !newTarget || newTarget->GetObjectGuid() != targetGuid);
            const bool fallbackRecoveryProbe = HasFallbackMovementWork(randomMoved, idleNudged, movingAfterReacquireClear);
            const bool workAssigned =
                concreteRecoveryWork ||
                directedMovementAfterReacquireClear ||
                fallbackRecoveryProbe;
            const bool rescueAttempted =
                questRequested || travelChosen || targetSelected || attackAnything || grindRequested ||
                travelMoved || rpgChosen || rpgMoved || randomMoved || idleNudged || clearedTravel ||
                clearedTarget || clearedLoot || clearedRpg;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason="
                    << (hardCombatStall ? "combat-stall" :
                        hardMeleeLoopStall ? "melee-loop-stall" :
                        hardTargetStall ? "target-stall" :
                        hardLootStall ? "loot-stall" :
                        hardTravelStall ? "travel-stall" :
                        hardFakeMovement ? "fake-movement" : "no-visible-work")
                    << " action=" << (lastActionName.empty() ? "none" : lastActionName)
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " moveProgressTicks=" << actionMetricMoveProgressTicks
                    << " movingIntent=" << (movementIntent ? 1 : 0)
                    << " movementProgress=" << (movementProgress ? 1 : 0)
                    << " oldStatus=" << static_cast<uint32>(hardOldStatus)
                    << " nullDest=" << (hardNullDestination ? 1 : 0)
                    << " clearedTravel=" << (clearedTravel ? 1 : 0)
                    << " clearedTarget=" << (clearedTarget ? 1 : 0)
                    << " clearedLoot=" << (clearedLoot ? 1 : 0)
                    << " clearedRpg=" << (clearedRpg ? 1 : 0)
                    << " target=" << targetName
                    << " targetGuid=" << targetGuid.GetCounter()
                    << " lootGuid=" << lootGuid
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " selectTarget=" << (targetSelected ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " rpgChoose=" << (rpgChosen ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " rpgMove=" << (rpgMoved ? 1 : 0)
                    << " randomMove=" << (randomMoved ? 1 : 0)
                    << " idleNudge=" << (idleNudged ? 1 : 0)
                    << " movingAfter=" << (movingAfterReacquireClear ? 1 : 0)
                    << " directedMove=" << (directedMovementAfterReacquireClear ? 1 : 0)
                    << " suppressLocalTargetRetry=" << (suppressLocalTargetRetry ? 1 : 0)
                    << " reacquiredClearedTarget=" << (reacquiredClearedTarget ? 1 : 0)
                    << " staleSuppressedTarget=" << (staleSuppressedTarget ? 1 : 0)
                    << " newTarget=" << newTargetName
                    << " newLoot=" << (!newLoot.IsEmpty() ? 1 : 0)
                    << " newRpgTarget=" << (newRpgTarget ? 1 : 0)
                    << " concreteWork=" << (concreteRecoveryWork ? 1 : 0)
                    << " fallbackProbe=" << (fallbackRecoveryProbe ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0)
                    << " destinationPtr=" << (hardDestinationPresent ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "HardIdleBotTrace", "", out.str());
            }

            if (workAssigned)
            {
                actionMetricLastProgress = now;
                return;
            }

            if (rescueAttempted && (movingAfterReacquireClear || newRpgTarget || newTravel))
            {
                StopMoving();
                bot->InterruptMoving(true);
                RESET_AI_VALUE(GuidPosition, "rpg target");

                if (newTravel && !IsActiveTravelWork(newTravel) && !IsPreparedTravelWork(newTravel))
                {
                    newTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    newTravel->SetForced(false);
                    RESET_AI_VALUE(bool, "travel target active");
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 5) == 1)
                {
                    std::ostringstream out;
                    out << "reason=no-concrete-work"
                        << " oldReason="
                        << (hardCombatStall ? "combat-stall" :
                            hardMeleeLoopStall ? "melee-loop-stall" :
                            hardTargetStall ? "target-stall" :
                            hardLootStall ? "loot-stall" :
                            hardTravelStall ? "travel-stall" :
                            hardFakeMovement ? "fake-movement" : "no-visible-work")
                        << " movingAfter=" << (movingAfterReacquireClear ? 1 : 0)
                        << " travelStatus=" << (newTravel ? static_cast<uint32>(newTravel->GetStatus()) : 0)
                        << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                        << " target=" << (newTarget ? newTarget->GetName() : "none")
                        << " loot=" << (!newLoot.IsEmpty() ? 1 : 0)
                        << " randomMove=" << (randomMoved ? 1 : 0)
                        << " idleNudge=" << (idleNudged ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "NoConcreteWorkClear", "", out.str());
                }
            }
        }
    }

    if (!minimal &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        currentState != BotState::BOT_STATE_DEAD &&
        !sServerFacade.UnitIsDead(bot) &&
        !bot->IsTaxiFlying() &&
        !bot->IsBeingTeleported() &&
        !currentCombat &&
        !currentCasting &&
        !hasCurrentTarget &&
        !hasLootTarget &&
        fillerNoWorkAction &&
        ENABLE_METRIC_RESCUES &&
        noProgressSecs >= VISIBLE_IDLE_WARN_SECONDS)
    {
        TravelTarget* noWorkTravel = AI_VALUE(TravelTarget*, "travel target");
        TravelDestination* noWorkDestination = noWorkTravel ? noWorkTravel->GetDestination() : nullptr;
        TravelStatus noWorkStatus = noWorkTravel ? noWorkTravel->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
        GuidPosition noWorkRpgTarget = AI_VALUE(GuidPosition, "rpg target");
        const bool noWorkNullDestination = !noWorkDestination || dynamic_cast<NullTravelDestination*>(noWorkDestination);
        const bool noWorkMovingWithoutAssignment =
            movementIntent &&
            !IsActiveTravelWork(noWorkTravel) &&
            !IsPreparedTravelWork(noWorkTravel) &&
            !noWorkRpgTarget;
        const bool noConcreteWork =
            !IsActiveTravelWork(noWorkTravel) &&
            !IsPreparedTravelWork(noWorkTravel) &&
            !noWorkRpgTarget;

        if ((noWorkMovingWithoutAssignment || (noConcreteWork && noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS)) &&
            (noWorkNullDestination ||
             noWorkStatus == TravelStatus::TRAVEL_STATUS_NONE ||
             noWorkStatus == TravelStatus::TRAVEL_STATUS_COOLDOWN ||
             noWorkStatus == TravelStatus::TRAVEL_STATUS_EXPIRED ||
             actionMetricSameActionStreak >= 20))
        {
            if (noWorkMovingWithoutAssignment)
                StopMoving();

            if (noWorkTravel)
            {
                noWorkTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                noWorkTravel->SetForced(false);
            }

            RESET_AI_VALUE(bool, "travel target active");
            RESET_AI_VALUE(GuidPosition, "rpg target");
            RESET_AI_VALUE(ObjectGuid, "attack target");
            context->GetValue<bool>("no active travel destinations", "quest")->Reset();

            bool questRequested = false;
            bool travelChosen = false;
            bool targetSelected = false;
            bool attackAnything = false;
            bool grindRequested = false;
            bool travelMoved = false;
            bool rpgChosen = false;
            bool rpgMoved = false;
            bool randomMoved = false;
            bool idleNudged = false;

            auto noWorkHasImmediateWork = [&]() -> bool
            {
                Unit* workTarget = AI_VALUE(Unit*, "current target");
                LootObject workLoot = AI_VALUE(LootObject, "loot target");
                GuidPosition workRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                TravelTarget* workTravel = AI_VALUE(TravelTarget*, "travel target");
                const bool workMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool directedMovement =
                    HasDirectedMovementWork(workTravel, workRpgTarget, travelMoved, rpgMoved, workMoving);
                return HasUsefulAssignedWork(bot, workTravel, workTarget, workLoot, workMoving, directedMovement);
            };

            auto noWorkTryAssignedTravelMove = [&]() -> bool
            {
                const bool moved = MoveAssignedTravelWork(this, context, bot, "metric-no-work");
                travelMoved = travelMoved || moved;
                return moved;
            };

            questRequested = DoSpecificAction("request quest travel target", Event(), true);
            if (questRequested)
                noWorkTryAssignedTravelMove();

            if (!noWorkHasImmediateWork())
            {
                travelChosen = DoSpecificAction("choose travel target", Event(), true);
                if (travelChosen)
                    noWorkTryAssignedTravelMove();
            }

            if (!noWorkHasImmediateWork())
            {
                ResetTargetScanCaches(context);
                targetSelected = DoSpecificAction("select new target", Event(), true);
            }

            if (!noWorkHasImmediateWork())
                attackAnything = DoSpecificAction("attack anything", Event(), true);

            if (!noWorkHasImmediateWork())
            {
                Unit* selectedTarget = AI_VALUE(Unit*, "current target");
                if (selectedTarget)
                    attackAnything = ForceConcreteTargetWork(this, bot, selectedTarget, "metric-no-work-target-handoff") ||
                        attackAnything;
            }

            if (!noWorkHasImmediateWork())
            {
                grindRequested = RequestGrindTravelFallback(this, context, bot, "metric-no-work");
                if (grindRequested)
                    noWorkTryAssignedTravelMove();
            }

            const bool allowNoWorkFallbackMovement = true;
            if (allowNoWorkFallbackMovement && !noWorkHasImmediateWork())
            {
                rpgChosen = DoSpecificAction("choose rpg target", Event(), true);
                rpgMoved = DoSpecificAction("move to rpg target", Event(), true);
                if (!noWorkHasImmediateWork() && !rpgMoved)
                    randomMoved = DoSpecificAction("move random", Event("metric no work", "active filler rescue", bot), true);
                if (!noWorkHasImmediateWork() && !randomMoved && !sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
                    idleNudged = ForceIdleRescueNudge(this, bot, "metric-no-work");
            }

            Unit* rescuedTarget = AI_VALUE(Unit*, "current target");
            LootObject rescuedLoot = AI_VALUE(LootObject, "loot target");
            GuidPosition rescuedRpgTarget = AI_VALUE(GuidPosition, "rpg target");
            TravelTarget* rescuedTravel = AI_VALUE(TravelTarget*, "travel target");
            TravelDestination* rescuedDestination = rescuedTravel ? rescuedTravel->GetDestination() : nullptr;
            TravelStatus rescuedStatus = rescuedTravel ? rescuedTravel->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
            if (rescuedTarget &&
                !bot->IsNonMeleeSpellCasted(true, false, true) &&
                !(bot->GetVictim() == rescuedTarget && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING)))
            {
                attackAnything = ForceConcreteTargetWork(this, bot, rescuedTarget, "metric-no-work-target-handoff") ||
                    attackAnything;
                rescuedTarget = AI_VALUE(Unit*, "current target");
                rescuedLoot = AI_VALUE(LootObject, "loot target");
                rescuedRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                rescuedTravel = AI_VALUE(TravelTarget*, "travel target");
                rescuedDestination = rescuedTravel ? rescuedTravel->GetDestination() : nullptr;
                rescuedStatus = rescuedTravel ? rescuedTravel->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
            }
            const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovementWork =
                HasDirectedMovementWork(rescuedTravel, rescuedRpgTarget, travelMoved, rpgMoved, movingAfter);
            const bool fallbackMovementWork =
                HasFallbackMovementWork(randomMoved, idleNudged, movingAfter);
            const bool workAssigned =
                HasConcreteRecoveryWork(bot, rescuedTravel, rescuedTarget, rescuedLoot, movingAfter,
                    travelMoved, false) ||
                directedMovementWork ||
                fallbackMovementWork;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv") && (workAssigned || urand(1, 5) == 1))
            {
                std::ostringstream out;
                out << "action=" << (lastActionName.empty() ? "none" : lastActionName)
                    << " oldStatus=" << static_cast<uint32>(noWorkStatus)
                    << " newStatus=" << static_cast<uint32>(rescuedStatus)
                    << " nullDest=" << (noWorkNullDestination ? 1 : 0)
                    << " movingWithoutWork=" << (noWorkMovingWithoutAssignment ? 1 : 0)
                    << " noProgressSec=" << noProgressSecs
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " selectTarget=" << (targetSelected ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " rpgChoose=" << (rpgChosen ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " rpgMove=" << (rpgMoved ? 1 : 0)
                    << " randomMove=" << (randomMoved ? 1 : 0)
                    << " idleNudge=" << (idleNudged ? 1 : 0)
                    << " nudgeOnly=" << ((randomMoved || idleNudged) && !workAssigned ? 1 : 0)
                    << " directedMove=" << (directedMovementWork ? 1 : 0)
                    << " fallbackMove=" << (fallbackMovementWork ? 1 : 0)
                    << " movingAfter=" << (movingAfter ? 1 : 0)
                    << " target=" << (rescuedTarget ? rescuedTarget->GetName() : "none")
                    << " loot=" << (!rescuedLoot.IsEmpty() ? 1 : 0)
                    << " rpgTarget=" << (rescuedRpgTarget ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0)
                    << " destinationPtr=" << (rescuedDestination ? 1 : (noWorkDestination ? 1 : 0));
                sPlayerbotAIConfig.logEvent(this, "NoWorkActiveRescue", "", out.str());
            }

            if (workAssigned)
            {
                actionMetricLastProgress = now;
                return;
            }

            if (movingAfter || rescuedRpgTarget ||
                (rescuedTravel && !IsActiveTravelWork(rescuedTravel) && !IsPreparedTravelWork(rescuedTravel)))
            {
                StopMoving();
                bot->InterruptMoving(true);
                RESET_AI_VALUE(GuidPosition, "rpg target");

                if (rescuedTravel && !IsActiveTravelWork(rescuedTravel) && !IsPreparedTravelWork(rescuedTravel))
                {
                    rescuedTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    rescuedTravel->SetForced(false);
                    RESET_AI_VALUE(bool, "travel target active");
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 5) == 1)
                {
                    std::ostringstream out;
                    out << "reason=no-work-active"
                        << " movingAfter=" << (movingAfter ? 1 : 0)
                        << " travelStatus=" << static_cast<uint32>(rescuedStatus)
                        << " rpgTarget=" << (rescuedRpgTarget ? 1 : 0)
                        << " target=" << (rescuedTarget ? rescuedTarget->GetName() : "none")
                        << " loot=" << (!rescuedLoot.IsEmpty() ? 1 : 0)
                        << " randomMove=" << (randomMoved ? 1 : 0)
                        << " idleNudge=" << (idleNudged ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "NoConcreteWorkClear", "", out.str());
                }
            }
        }
    }

    if (ENABLE_METRIC_RESCUES && !minimal && !actionExecuted && actionMetricLastProgress && noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS)
    {
        const bool autonomousRandomBot = !HasActivePlayerMaster() && !HasRealPlayerMaster();
        std::string loopReason = "idle";
        if (travelTarget)
        {
            switch (travelTarget->GetStatus())
            {
                case TravelStatus::TRAVEL_STATUS_PREPARE:
                    loopReason = "travel_prepare";
                    break;
                case TravelStatus::TRAVEL_STATUS_TRAVEL:
                    loopReason = "travel_move";
                    break;
                case TravelStatus::TRAVEL_STATUS_WORK:
                    loopReason = "travel_work";
                    break;
                case TravelStatus::TRAVEL_STATUS_COOLDOWN:
                    loopReason = "travel_cooldown";
                    break;
                default:
                    break;
            }
        }

        if (AI_VALUE(Unit*, "current target"))
            loopReason = "target_stall";
        else if (!AI_VALUE(LootObject, "loot target").IsEmpty())
            loopReason = "loot_stall";

        const bool corpseRecoveryStall =
            sServerFacade.UnitIsDead(bot) &&
            (currentState == BotState::BOT_STATE_DEAD || bot->GetCorpse() || bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)) &&
            (actionMetricLastActionName.empty() ||
             actionMetricLastActionName == "none" ||
             actionMetricLastActionName == "auto release" ||
             actionMetricLastActionName == "find corpse" ||
             actionMetricLastActionName == "revive from corpse" ||
             actionMetricLastActionName == "spirit healer");

        if (autonomousRandomBot &&
            currentState != BotState::BOT_STATE_DEAD &&
            !sServerFacade.UnitIsDead(bot) &&
            !bot->IsTaxiFlying() &&
            !bot->IsBeingTeleported() &&
            !bot->IsNonMeleeSpellCasted(true, false, true) &&
            actionMetricExecutedTicks == 0 &&
            noProgressSecs >= 8)
        {
            Unit* zeroApmTarget = AI_VALUE(Unit*, "current target");
            LootObject zeroApmLoot = AI_VALUE(LootObject, "loot target");
            TravelTarget* zeroApmTravel = AI_VALUE(TravelTarget*, "travel target");
            TravelDestination* zeroApmDestination = zeroApmTravel ? zeroApmTravel->GetDestination() : nullptr;
            const bool zeroApmNullDestination =
                !zeroApmDestination || dynamic_cast<NullTravelDestination*>(zeroApmDestination);
            const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool combat = sServerFacade.IsInCombat(bot) || bot->GetVictim();
            const bool staleLoot = !zeroApmLoot.IsEmpty();
            const bool staleTarget =
                zeroApmTarget &&
                zeroApmTarget->IsInWorld() &&
                zeroApmTarget->GetMapId() == bot->GetMapId() &&
                !sServerFacade.IsFriendlyTo(bot, zeroApmTarget);
            const bool staleTravelWork =
                zeroApmTravel &&
                zeroApmTarget == nullptr &&
                zeroApmLoot.IsEmpty() &&
                actionMetricMoveProgressTicks == 0 &&
                noProgressSecs >= 12 &&
                (zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL ||
                 zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK);
            const bool staleNullTravel =
                zeroApmTravel &&
                zeroApmNullDestination &&
                noProgressSecs >= 8 &&
                (zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE ||
                 zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL ||
                 zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK ||
                 zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED);

            if ((!moving && (staleLoot || (!combat && staleTarget))) || staleTravelWork || staleNullTravel)
            {
                uint64 lootGuid = 0;
                uint64 targetGuid = 0;
                std::string targetName = "none";

                if (staleLoot)
                {
                    lootGuid = zeroApmLoot.guid.GetCounter();
                    DeferLootGuidForBot(bot, zeroApmLoot.guid, 15);
                    if (LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot"))
                        availableLoot->Remove(zeroApmLoot.guid);
                    RESET_AI_VALUE(LootObject, "loot target");
                }

                if (!combat && staleTarget)
                {
                    targetGuid = zeroApmTarget->GetObjectGuid().GetCounter();
                    targetName = zeroApmTarget->GetName();
                    RESET_AI_VALUE(bool, "has attackers");
                    RESET_AI_VALUE(Unit*, "old target");
                    RESET_AI_VALUE(Unit*, "current target");
                    RESET_AI_VALUE(Unit*, "pull target");
                    RESET_AI_VALUE(Unit*, "dps target");
                    RESET_AI_VALUE(ObjectGuid, "attack target");
                    bot->SetSelectionGuid(ObjectGuid());
                    bot->CombatStop(false);
                    OnCombatEnded();
                    ResetStaleTargetState();
                }

                if (zeroApmTravel &&
                    (staleTravelWork ||
                     staleNullTravel ||
                     zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK ||
                     zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN ||
                     zeroApmTravel->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED))
                {
                    if (staleTravelWork || staleNullTravel)
                        StopMoving();
                    zeroApmTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    zeroApmTravel->SetForced(false);
                    RESET_AI_VALUE(bool, "travel target active");
                    context->ClearValues("no active travel destinations");
                }

                RESET_AI_VALUE(GuidPosition, "rpg target");

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "loop=" << loopReason
                        << " action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                        << " noProgressSec=" << noProgressSecs
                        << " noProgressTicks=" << actionMetricNoProgressTicks
                        << " sameActionStreak=" << actionMetricSameActionStreak
                        << " nullDest=" << (zeroApmNullDestination ? 1 : 0)
                        << " lootGuid=" << lootGuid
                        << " targetGuid=" << targetGuid
                        << " target=" << targetName
                        << " travelStatus=" << (zeroApmTravel ? static_cast<uint32>(zeroApmTravel->GetStatus()) : 0)
                        << " travelPtr=" << (zeroApmDestination ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "ZeroApmStaleWorkClear", "", out.str());
                }

                actionMetricLastProgress = now;
                return;
            }
        }

        if (autonomousRandomBot &&
            currentState != BotState::BOT_STATE_DEAD &&
            !sServerFacade.UnitIsDead(bot) &&
            !bot->IsTaxiFlying() &&
            !bot->IsBeingTeleported() &&
            !bot->IsNonMeleeSpellCasted(true, false, true) &&
            !movementIntent &&
            !sServerFacade.IsInCombat(bot) &&
            !bot->GetVictim())
        {
            Unit* stalledTarget = AI_VALUE(Unit*, "current target");
            if (stalledTarget &&
                stalledTarget->IsInWorld() &&
                stalledTarget->GetMapId() == bot->GetMapId() &&
                !sServerFacade.IsFriendlyTo(bot, stalledTarget) &&
                !sServerFacade.UnitIsDead(stalledTarget) &&
                noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS &&
                (actionMetricNoProgressTicks >= 20 || actionMetricSameActionStreak >= 15 || noProgressSecs >= 20))
            {
                ObjectGuid targetGuid = stalledTarget->GetObjectGuid();
                std::string targetName = stalledTarget->GetName();

                RESET_AI_VALUE(bool, "has attackers");
                RESET_AI_VALUE(Unit*, "old target");
                RESET_AI_VALUE(Unit*, "current target");
                RESET_AI_VALUE(Unit*, "pull target");
                RESET_AI_VALUE(Unit*, "dps target");
                RESET_AI_VALUE(ObjectGuid, "attack target");
                bot->SetSelectionGuid(ObjectGuid());
                bot->CombatStop(false);
                OnCombatEnded();
                ResetStaleTargetState();

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                        << " target=" << targetName
                        << " guid=" << targetGuid.GetCounter()
                        << " noProgressSec=" << noProgressSecs
                        << " noProgressTicks=" << actionMetricNoProgressTicks
                        << " sameActionStreak=" << actionMetricSameActionStreak
                        << " reason=pre-stuck";
                    sPlayerbotAIConfig.logEvent(this, "CombatTargetMetricRefresh", std::to_string(targetGuid.GetCounter()), out.str());
                }

                actionMetricLastProgress = now;
                return;
            }
        }

        if (autonomousRandomBot &&
            currentState != BotState::BOT_STATE_DEAD &&
            !sServerFacade.UnitIsDead(bot) &&
            !bot->IsTaxiFlying() &&
            !bot->IsBeingTeleported() &&
            !bot->IsNonMeleeSpellCasted(true, false, true) &&
            !movementIntent &&
            !sServerFacade.IsInCombat(bot) &&
            !AI_VALUE(Unit*, "current target") &&
            AI_VALUE(LootObject, "loot target").IsEmpty() &&
            noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS)
        {
            TravelTarget* staleTravel = AI_VALUE(TravelTarget*, "travel target");
            TravelDestination* staleDestination = staleTravel ? staleTravel->GetDestination() : nullptr;
            TravelStatus oldStatus = staleTravel ? staleTravel->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
            const bool staleNullDestination =
                !staleDestination || dynamic_cast<NullTravelDestination*>(staleDestination);

            RESET_AI_VALUE(bool, "has attackers");
            RESET_AI_VALUE(Unit*, "old target");
            RESET_AI_VALUE(Unit*, "pull target");
            RESET_AI_VALUE(Unit*, "dps target");
            RESET_AI_VALUE(ObjectGuid, "attack target");
            RESET_AI_VALUE(GuidPosition, "rpg target");
            bot->SetSelectionGuid(ObjectGuid());
            ResetStaleTargetState();

            if (staleTravel &&
                (oldStatus == TravelStatus::TRAVEL_STATUS_NONE ||
                 (staleNullDestination &&
                  (oldStatus == TravelStatus::TRAVEL_STATUS_PREPARE ||
                   oldStatus == TravelStatus::TRAVEL_STATUS_TRAVEL ||
                   oldStatus == TravelStatus::TRAVEL_STATUS_WORK)) ||
                 oldStatus == TravelStatus::TRAVEL_STATUS_COOLDOWN ||
                 oldStatus == TravelStatus::TRAVEL_STATUS_EXPIRED))
            {
                staleTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                staleTravel->SetForced(false);
                RESET_AI_VALUE(bool, "travel target active");
                context->ClearValues("no active travel destinations");
            }

            bool questRequested = false;
            bool travelChosen = false;
            bool targetSelected = false;
            bool attackAnything = false;
            bool grindRequested = false;
            bool travelMoved = false;
            bool rpgChosen = false;
            bool rpgMoved = false;
            bool randomMoved = false;
            bool idleNudged = false;

            auto metricResetHasImmediateWork = [&]() -> bool
            {
                Unit* workTarget = AI_VALUE(Unit*, "current target");
                LootObject workLoot = AI_VALUE(LootObject, "loot target");
                GuidPosition workRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                TravelTarget* workTravel = AI_VALUE(TravelTarget*, "travel target");
                const bool workMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool directedMovement =
                    HasDirectedMovementWork(workTravel, workRpgTarget, travelMoved, rpgMoved, workMoving);
                return HasUsefulAssignedWork(bot, workTravel, workTarget, workLoot, workMoving, directedMovement);
            };

            auto metricResetTryAssignedTravelMove = [&]() -> bool
            {
                const bool moved = MoveAssignedTravelWork(this, context, bot, "metric-reset-no-work");
                travelMoved = travelMoved || moved;
                return moved;
            };

            questRequested = DoSpecificAction("request quest travel target", Event(), true);
            if (questRequested)
                metricResetTryAssignedTravelMove();

            if (!metricResetHasImmediateWork())
            {
                travelChosen = DoSpecificAction("choose travel target", Event(), true);
                if (travelChosen)
                    metricResetTryAssignedTravelMove();
            }

            if (!metricResetHasImmediateWork())
            {
                ResetTargetScanCaches(context);
                targetSelected = DoSpecificAction("select new target", Event(), true);
            }

            if (!metricResetHasImmediateWork())
                attackAnything = DoSpecificAction("attack anything", Event(), true);

            if (!metricResetHasImmediateWork())
            {
                Unit* selectedTarget = AI_VALUE(Unit*, "current target");
                if (selectedTarget)
                    attackAnything = ForceConcreteTargetWork(this, bot, selectedTarget,
                        "metric-reset-no-work-target-handoff") || attackAnything;
            }

            if (!metricResetHasImmediateWork())
            {
                grindRequested = RequestGrindTravelFallback(this, context, bot, "metric-reset-no-work");
                if (grindRequested)
                    metricResetTryAssignedTravelMove();
            }

            const bool allowMetricResetFallbackMovement = true;
            if (allowMetricResetFallbackMovement && !metricResetHasImmediateWork())
            {
                rpgChosen = DoSpecificAction("choose rpg target", Event(), true);
                rpgMoved = DoSpecificAction("move to rpg target", Event(), true);
                if (!metricResetHasImmediateWork() && !rpgMoved)
                    randomMoved = DoSpecificAction("move random", Event("metric no work", "idle rescue", bot), true);
                if (!metricResetHasImmediateWork() && !randomMoved && !sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
                    idleNudged = ForceIdleRescueNudge(this, bot, "metric-reset-no-work");
            }

            bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            Unit* newTarget = AI_VALUE(Unit*, "current target");
            LootObject newLootTarget = AI_VALUE(LootObject, "loot target");
            GuidPosition newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
            TravelTarget* newTravel = AI_VALUE(TravelTarget*, "travel target");
            TravelDestination* newDestination = newTravel ? newTravel->GetDestination() : nullptr;
            const bool directedMovementWork =
                HasDirectedMovementWork(newTravel, newRpgTarget, travelMoved, rpgMoved, movingAfter);
            const bool fallbackMovementWork =
                HasFallbackMovementWork(randomMoved, idleNudged, movingAfter);
            bool workAssigned =
                HasConcreteRecoveryWork(bot, newTravel, newTarget, newLootTarget, movingAfter, travelMoved, false) ||
                directedMovementWork ||
                fallbackMovementWork;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " oldStatus=" << static_cast<uint32>(oldStatus)
                    << " newStatus=" << (newTravel ? static_cast<uint32>(newTravel->GetStatus()) : 0)
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " selectTarget=" << (targetSelected ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " rpgChoose=" << (rpgChosen ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " rpgMove=" << (rpgMoved ? 1 : 0)
                    << " randomMove=" << (randomMoved ? 1 : 0)
                    << " idleNudge=" << (idleNudged ? 1 : 0)
                    << " nudgeOnly=" << ((randomMoved || idleNudged) && !workAssigned ? 1 : 0)
                    << " directedMove=" << (directedMovementWork ? 1 : 0)
                    << " fallbackMove=" << (fallbackMovementWork ? 1 : 0)
                    << " movingAfter=" << (movingAfter ? 1 : 0)
                    << " target=" << (newTarget ? newTarget->GetName() : "none")
                    << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                    << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0)
                    << " destinationPtr=" << (newDestination ? 1 : (staleDestination ? 1 : 0));
                sPlayerbotAIConfig.logEvent(this, "NoWorkMetricReset", "", out.str());
            }

            if (workAssigned)
            {
                actionMetricLastProgress = now;
                return;
            }
        }

        if (autonomousRandomBot &&
            travelTarget &&
            travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED &&
            !AI_VALUE(Unit*, "current target") &&
            AI_VALUE(LootObject, "loot target").IsEmpty() &&
            !movementIntent &&
            !sServerFacade.IsInCombat(bot))
        {
            TravelDestination* expiredDestination = travelTarget->GetDestination();
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << loopReason
                    << " stallSec=" << noProgressSecs
                    << " action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                    << " travelPtr=" << (expiredDestination ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "ExpiredTravelMetricRefresh", "", out.str());
            }

            RESET_AI_VALUE(bool, "travel target active");
            context->ClearValues("no active travel destinations");
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            travelTarget->SetForced(false);
            actionMetricLastProgress = now;
            return;
        }

        if (!corpseRecoveryStall && (now - actionMetricLastStallSnapshot) >= 15)
        {
            LogDecisionStallSnapshot(now, loopReason);
            actionMetricLastStallSnapshot = now;
        }

        if (autonomousRandomBot &&
            travelTarget &&
            travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN &&
            dynamic_cast<NullTravelDestination*>(travelTarget->GetDestination()))
        {
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << loopReason
                    << " stallSec=" << noProgressSecs
                    << " action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName);
                sPlayerbotAIConfig.logEvent(this, "NullTravelForcedRefresh", "", out.str());
            }

            RESET_AI_VALUE(bool, "travel target active");
            aiObjectContext->ClearValues("no active travel destinations");
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            actionMetricLastProgress = now;
            return;
        }

        if (autonomousRandomBot && noProgressSecs >= 60)
        {
            if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                if (actionMetricLastActionName == "find corpse")
                {
                    FindCorpseAction findCorpse(this);
                    Event event;
                    bool nudged = findCorpse.Execute(event);
                    bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                    if (Corpse* corpse = bot->GetCorpse())
                    {
                        WorldPosition corpsePos(corpse);
                        if (corpsePos &&
                            !HasPlayerNearby(corpsePos) &&
                            sPlayerbotAIConfig.hasLog("bot_events.csv") &&
                            (now - actionMetricLastStallSnapshot) >= 15)
                        {
                            std::ostringstream out;
                            out << "stallSec=" << noProgressSecs
                                << " nudged=" << (nudged ? 1 : 0)
                                << " moving=" << (moving ? 1 : 0)
                                << " corpseMap=" << corpsePos.getMapId()
                                << " corpseX=" << std::fixed << std::setprecision(2) << corpsePos.getX()
                                << " corpseY=" << corpsePos.getY()
                                << " corpseZ=" << corpsePos.getZ();
                            sPlayerbotAIConfig.logEvent(this, "DeadCorpseMetricStall", "", out.str());
                            actionMetricLastStallSnapshot = now;
                        }
                    }
                    return;
                }
            }

            if (travelTarget && travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
            {
                TravelDestination* cooldownDestination = travelTarget->GetDestination();
                const bool questCooldown = cooldownDestination != nullptr;

                if (questCooldown || !AI_VALUE(LootObject, "loot target").IsEmpty())
                {
                    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                    {
                        std::ostringstream out;
                        out << "reason=" << loopReason
                            << " stallSec=" << noProgressSecs
                            << " action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                            << " travelPtr=" << (cooldownDestination ? 1 : 0);
                        sPlayerbotAIConfig.logEvent(this, "TravelCooldownForcedRefresh", "", out.str());
                    }

                    RESET_AI_VALUE(LootObject, "loot target");
                    RESET_AI_VALUE(bool, "travel target active");
                    context->ClearValues("no active travel destinations");
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    actionMetricLastProgress = now;
                    return;
                }
            }
        }
    }

    if ((now - actionMetricLastVisibleSnapshot) >= 15)
    {
        LogVisibleActivitySnapshot(now);
        actionMetricLastVisibleSnapshot = now;
    }
}

void PlayerbotAI::ResetStaleTargetState()
{
    staleTargetGuid = ObjectGuid();
    staleTargetSince = 0;
    staleTargetLastProgress = 0;
    staleTargetLastX = 0.0f;
    staleTargetLastY = 0.0f;
    staleTargetLastZ = 0.0f;
    staleTargetLastDistance = 0.0f;
    staleTargetLastTargetHealth = 0;
    staleTargetRecoveries = 0;
}

bool PlayerbotAI::ClearExpiredTravelAfterStaleTargetReset()
{
    TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
    if (!travelTarget || travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
        return false;

    TravelDestination* destination = travelTarget->GetDestination();
    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
    travelTarget->SetForced(false);
    aiObjectContext->GetValue<bool>("travel target active")->Reset();
    aiObjectContext->ClearValues("no active travel destinations");
    aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
    aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
    aiObjectContext->GetValue<LootObject>("loot target")->Reset();

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "destinationPtr=" << (destination ? 1 : 0)
            << " state=" << static_cast<uint32>(currentState);
        sPlayerbotAIConfig.logEvent(this, "StaleTargetExpiredTravelReset", "", out.str());
    }

    return true;
}

void PlayerbotAI::ResetTravelNoMotionState()
{
    travelNoMotionRetries = 0;
    travelNoMotionNextRetry = 0;
    travelNoMotionEntry = 0;
    travelNoMotionMap = 0;
    travelNoMotionX = 0.0f;
    travelNoMotionY = 0.0f;
    travelNoMotionZ = 0.0f;
}

bool PlayerbotAI::RecoverTravelNoMotion(time_t now)
{
    if (currentState == BotState::BOT_STATE_DEAD ||
        HasActivePlayerMaster() ||
        HasRealPlayerMaster() ||
        bot->IsTaxiFlying() ||
        bot->IsBeingTeleported() ||
        bot->IsNonMeleeSpellCasted(true, false, true) ||
        sServerFacade.isMoving(bot) ||
        bot->isMovingOrTurning() ||
        sServerFacade.IsInCombat(bot) ||
        aiObjectContext->GetValue<Unit*>("current target")->Get() ||
        !aiObjectContext->GetValue<LootObject>("loot target")->Get().IsEmpty())
    {
        ResetTravelNoMotionState();
        return false;
    }

    TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
    if (!travelTarget || !travelTarget->GetPosition())
    {
        ResetTravelNoMotionState();
        return false;
    }

    TravelStatus status = travelTarget->GetStatus();
    if (status != TravelStatus::TRAVEL_STATUS_READY &&
        status != TravelStatus::TRAVEL_STATUS_TRAVEL)
    {
        ResetTravelNoMotionState();
        return false;
    }

    uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
    if (noProgressSecs < 6)
        return false;

    WorldPosition* travelPos = travelTarget->GetPosition();
    const bool sameEndpoint =
        travelNoMotionEntry == travelTarget->GetEntry() &&
        travelNoMotionMap == travelPos->getMapId() &&
        std::fabs(travelNoMotionX - travelPos->getX()) < 1.0f &&
        std::fabs(travelNoMotionY - travelPos->getY()) < 1.0f &&
        std::fabs(travelNoMotionZ - travelPos->getZ()) < 3.0f;

    if (!sameEndpoint)
    {
        travelNoMotionRetries = 0;
        travelNoMotionNextRetry = 0;
        travelNoMotionEntry = travelTarget->GetEntry();
        travelNoMotionMap = travelPos->getMapId();
        travelNoMotionX = travelPos->getX();
        travelNoMotionY = travelPos->getY();
        travelNoMotionZ = travelPos->getZ();
    }

    if (travelNoMotionNextRetry && now < travelNoMotionNextRetry)
        return false;

    bool dispatched = DoSpecificAction("move to travel target", Event(), true);
    const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
    Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
    LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
    TravelStatus statusAfter = travelTarget->GetStatus();
    const bool usefulAfter =
        movingAfter ||
        sServerFacade.IsInCombat(bot) ||
        currentTarget ||
        !lootTarget.IsEmpty();

    if (usefulAfter)
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 8) == 1)
        {
            std::ostringstream out;
            out << "reason=recovered"
                << " dispatched=" << (dispatched ? 1 : 0)
                << " retries=" << travelNoMotionRetries
                << " noProgressSec=" << noProgressSecs
                << " status=" << static_cast<uint32>(status)
                << " statusAfter=" << static_cast<uint32>(statusAfter)
                << " moving=" << (movingAfter ? 1 : 0)
                << " entry=" << travelTarget->GetEntry()
                << " travelPtr=" << (travelTarget->GetDestination() ? 1 : 0);
            sPlayerbotAIConfig.logEvent(this, "TravelNoMotionRecovery", "", out.str());
        }

        ResetTravelNoMotionState();
        return true;
    }

    ++travelNoMotionRetries;
    travelNoMotionNextRetry = now + 3;
    const bool expireTravel = (travelNoMotionRetries >= 2 && noProgressSecs >= 12) || noProgressSecs >= 18;

    if (expireTravel)
    {
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        travelTarget->SetExpireIn(1000);
        aiObjectContext->GetValue<bool>("travel target active")->Reset();
        aiObjectContext->GetValue<LootObject>("loot target")->Reset();
        aiObjectContext->ClearValues("no active travel destinations");

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "reason=expired"
                << " dispatched=" << (dispatched ? 1 : 0)
                << " retries=" << travelNoMotionRetries
                << " noProgressSec=" << noProgressSecs
                << " status=" << static_cast<uint32>(status)
                << " statusAfter=" << static_cast<uint32>(statusAfter)
                << " moving=0"
                << " map=" << travelPos->getMapId()
                << " x=" << std::fixed << std::setprecision(2) << travelPos->getX()
                << " y=" << travelPos->getY()
                << " z=" << travelPos->getZ()
                << " entry=" << travelTarget->GetEntry()
                << " travelPtr=" << (travelTarget->GetDestination() ? 1 : 0);
            sPlayerbotAIConfig.logEvent(this, "TravelNoMotionRecovery", "", out.str());
        }

        ResetTravelNoMotionState();
        return true;
    }

    if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 4) == 1)
    {
        std::ostringstream out;
        out << "reason=retry"
            << " dispatched=" << (dispatched ? 1 : 0)
            << " retries=" << travelNoMotionRetries
            << " noProgressSec=" << noProgressSecs
            << " status=" << static_cast<uint32>(status)
            << " statusAfter=" << static_cast<uint32>(statusAfter)
            << " moving=0"
            << " entry=" << travelTarget->GetEntry()
            << " travelPtr=" << (travelTarget->GetDestination() ? 1 : 0);
        sPlayerbotAIConfig.logEvent(this, "TravelNoMotionRecovery", "", out.str());
    }

    return false;
}

bool PlayerbotAI::DetectAndClearStaleTarget()
{
    AiObjectContext* context = aiObjectContext;
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (!currentTarget || !currentTarget->IsInWorld() || !currentTarget->IsAlive() || currentTarget->GetMapId() != bot->GetMapId())
    {
        ResetStaleTargetState();
        return false;
    }

    ObjectGuid const targetGuid = currentTarget->GetObjectGuid();
    time_t const now = time(0);
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float const distance = bot->GetDistance(currentTarget);
    float const distance2d = sServerFacade.GetDistance2d(bot, currentTarget);
    float const verticalDelta = std::fabs(bot->GetPositionZ() - currentTarget->GetPositionZ());
    bool const inLos = bot->IsWithinLOSInMap(currentTarget, true);
    bool const isRangedBot = IsRanged(bot);
    bool const inMeleeRange = bot->CanReachWithMeleeAutoAttack(currentTarget);
    bool const inUsefulCombatRange = isRangedBot ?
        (inLos && distance2d <= GetRange("spell") + sPlayerbotAIConfig.contactDistance) :
        inMeleeRange;
    bool const steepCloseGeometry = !isRangedBot && inLos && !inMeleeRange && distance2d <= 8.0f && verticalDelta > 2.5f;

    if (staleTargetGuid != targetGuid || !staleTargetSince)
    {
        staleTargetGuid = targetGuid;
        staleTargetSince = now;
        staleTargetLastProgress = now;
        staleTargetLastX = botX;
        staleTargetLastY = botY;
        staleTargetLastZ = botZ;
        staleTargetLastDistance = distance2d;
        staleTargetLastTargetHealth = currentTarget->GetHealth();
        staleTargetRecoveries = 0;
        return false;
    }

    LastSpellCast const& lastSpell = AI_VALUE(LastSpellCast&, "last spell cast");
    float const dx = botX - staleTargetLastX;
    float const dy = botY - staleTargetLastY;
    float const dz = botZ - staleTargetLastZ;
    float const movedSq = dx * dx + dy * dy + dz * dz;
    bool const moved = movedSq >= 0.75f * 0.75f;
    bool const moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
    bool const hasVictim = bot->GetVictim() == currentTarget;
    bool const hasSelection = bot->GetSelectionGuid() == targetGuid;
    bool const attachedToTarget = hasVictim || hasSelection;
    bool const hasAttackers = AI_VALUE(bool, "has attackers");
    bool const inCombat = sServerFacade.IsInCombat(bot) || AI_VALUE2(bool, "combat", "self target");
    bool const recentSpell = lastSpell.time && lastSpell.target == targetGuid && (now - lastSpell.time) <= 3;
    bool const targetHealthChanged = staleTargetLastTargetHealth != currentTarget->GetHealth();
    bool const targetHealthDropped = currentTarget->GetHealth() < staleTargetLastTargetHealth;
    bool const closingDistance = moving && staleTargetLastDistance > 0.0f && (distance2d + 1.0f) < staleTargetLastDistance;
    // Progress means the fight is actually advancing: the target is LOSING health, or
    // we are closing the gap to it. Merely casting at it (recentSpell) or standing in
    // spell range (!inCombat && inUsefulCombatRange) is NOT progress -- those let a bot
    // rooted in place, perpetually casting at a target whose HP never drops (mob is
    // unreachable / leashed / evade-resetting), escape stale detection forever. Measured:
    // ~8.5k such casts/run, bots rooted for minutes on a single un-killable mob. Using
    // "health dropped" rather than "health changed" also catches the evade-reset loop
    // where the mob takes a little damage then heals back to full between casts.
    bool const usefulProgress = targetHealthDropped || closingDistance;
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    bool const expiredTravelTarget = travelTarget && travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED;

    if (usefulProgress)
    {
        staleTargetLastProgress = now;
        staleTargetLastX = botX;
        staleTargetLastY = botY;
        staleTargetLastZ = botZ;
        staleTargetLastDistance = distance2d;
        staleTargetLastTargetHealth = currentTarget->GetHealth();
        staleTargetRecoveries = 0;
        return false;
    }

    staleTargetLastX = botX;
    staleTargetLastY = botY;
    staleTargetLastZ = botZ;
    if (!moving || moved)
        staleTargetLastDistance = std::min(staleTargetLastDistance > 0.0f ? staleTargetLastDistance : distance2d, distance2d);
    staleTargetLastTargetHealth = currentTarget->GetHealth();

    if (bot->IsNonMeleeSpellCasted(true) || bot->HasUnitState(UNIT_STAT_CAN_NOT_MOVE | UNIT_STAT_STUNNED | UNIT_STAT_CONFUSED | UNIT_STAT_FLEEING))
        return false;

    uint32 const staleThreshold = inCombat ? 6u : 8u;
    uint32 const recoveryThreshold = inCombat ? 4u : 6u;
    uint32 const detachedThreshold = (!attachedToTarget && !hasAttackers && !inCombat) ? 2u : staleThreshold;
    uint32 const maxRecoveryAttempts = inCombat ? 3u : 4u;
    bool recoveryExhausted = false;

    if (hasVictim && staleTargetLastProgress && (now - staleTargetLastProgress) >= recoveryThreshold &&
        !(steepCloseGeometry && staleTargetSince && (now - staleTargetSince) >= staleThreshold + 2u))
    {
        recoveryExhausted =
            staleTargetRecoveries >= maxRecoveryAttempts &&
            !recentSpell &&
            !targetHealthChanged &&
            !closingDistance;

        if (recoveryExhausted)
        {
            std::string const targetName = currentTarget->GetName();
            if (bot->GetVictim() == currentTarget)
                bot->AttackStop(true);

            DeferTargetGuidForBot(bot, targetGuid, 20);
            RESET_AI_VALUE(Unit*, "old target");
            RESET_AI_VALUE(Unit*, "current target");
            RESET_AI_VALUE(Unit*, "pull target");
            RESET_AI_VALUE(ObjectGuid, "attack target");
            bot->SetSelectionGuid(ObjectGuid());
            bool const expiredTravelReset = ClearExpiredTravelAfterStaleTargetReset();

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "target=" << targetName
                    << " dist=" << std::fixed << std::setprecision(2) << distance
                    << " dist2d=" << std::fixed << std::setprecision(2) << distance2d
                    << " dz=" << std::fixed << std::setprecision(2) << verticalDelta
                    << " recoveries=" << staleTargetRecoveries
                    << " staleFor=" << (now - staleTargetLastProgress)
                    << " targetAge=" << (now - staleTargetSince)
                    << " moving=" << (moving ? 1 : 0)
                    << " meleeRange=" << (inMeleeRange ? 1 : 0)
                    << " los=" << (inLos ? 1 : 0)
                    << " expiredTravelReset=" << (expiredTravelReset ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "StaleTargetRecoveryExhausted", std::to_string(targetGuid.GetCounter()), out.str());
                sPlayerbotAIConfig.logEvent(this, "StaleTargetHardReset", std::to_string(targetGuid.GetCounter()), out.str());
                sPlayerbotAIConfig.logEvent(this, "StaleTargetReset", std::to_string(targetGuid.GetCounter()), out.str());
            }

            ResetStaleTargetState();
            return true;
        }
        else
        {
            bool recovered = false;
            MotionMaster* mm = bot->GetMotionMaster();
            MovementGeneratorType moveType = mm ? mm->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;

            const bool closeOrCommittedTarget =
                distance2d <= 12.0f ||
                hasVictim ||
                inCombat;
            if (!closeOrCommittedTarget &&
                !AllowPressureWork(PerfStats::BOT_PRESSURE_WORK_STALE_TARGET_RECOVER, 1500, 4500))
                return false;

            bot->SetSelectionGuid(targetGuid);
            bot->SetTarget(currentTarget);

            if (!isRangedBot)
            {
                if (mm && !inMeleeRange)
                {
                    if (moveType == CHASE_MOTION_TYPE && !closingDistance)
                        bot->InterruptMoving(true);

                    mm->MoveChase(currentTarget);
                    recovered = true;
                }
                else if (mm && moveType != CHASE_MOTION_TYPE)
                {
                    mm->MoveChase(currentTarget);
                    recovered = true;
                }

                if (inMeleeRange)
                {
                    bot->AttackStop(true);
                    recovered = bot->Attack(currentTarget, true) || recovered;
                }
                else if (!hasVictim)
                {
                    recovered = bot->Attack(currentTarget, true) || recovered;
                }
            }
            else if (inLos)
            {
                recovered = bot->Attack(currentTarget, false);
            }

            if (recovered)
            {
                const bool recoveryMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool recoveryCasting = bot->IsNonMeleeSpellCasted(true, false, true);
                const bool recoveryMadeProgress =
                    recoveryCasting ||
                    targetHealthChanged ||
                    closingDistance ||
                    (!inMeleeRange && !hasVictim && recoveryMoving);

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "target=" << currentTarget->GetName()
                        << " dist=" << std::fixed << std::setprecision(2) << distance
                        << " dist2d=" << std::fixed << std::setprecision(2) << distance2d
                        << " dz=" << std::fixed << std::setprecision(2) << verticalDelta
                        << " ranged=" << (isRangedBot ? 1 : 0)
                        << " meleeRange=" << (inMeleeRange ? 1 : 0)
                        << " motion=" << moveType
                        << " los=" << (inLos ? 1 : 0)
                        << " healthChanged=" << (targetHealthChanged ? 1 : 0)
                        << " closing=" << (closingDistance ? 1 : 0)
                        << " movingAfter=" << (recoveryMoving ? 1 : 0)
                        << " castingAfter=" << (recoveryCasting ? 1 : 0)
                        << " progress=" << (recoveryMadeProgress ? 1 : 0)
                        << " closeBypass=" << (closeOrCommittedTarget ? 1 : 0)
                        << " staleFor=" << (now - staleTargetLastProgress);
                    sPlayerbotAIConfig.logEvent(this, "StaleTargetRecover", std::to_string(targetGuid.GetCounter()), out.str());
                }

                staleTargetLastX = botX;
                staleTargetLastY = botY;
                staleTargetLastZ = botZ;
                if (recoveryMadeProgress)
                    staleTargetLastProgress = now;
                staleTargetLastDistance = distance2d;
                staleTargetLastTargetHealth = currentTarget->GetHealth();
                ++staleTargetRecoveries;
                return false;
            }
        }
    }

    if (expiredTravelTarget &&
        staleTargetLastProgress &&
        (now - staleTargetLastProgress) >= 10 &&
        !hasAttackers &&
        !recentSpell &&
        !targetHealthChanged)
    {
        recoveryExhausted = true;
    }

    if (!recoveryExhausted && staleTargetLastProgress && (now - staleTargetLastProgress) < detachedThreshold)
        return false;

    std::list<ObjectGuid> possibleTargets = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
    bool const targetStillPreferred = std::find(possibleTargets.begin(), possibleTargets.end(), targetGuid) != possibleTargets.end();
    bool const geometryBlocked = (!inLos && verticalDelta > 6.0f) ||
        (steepCloseGeometry && staleTargetSince && (now - staleTargetSince) >= staleThreshold + 2u);
    bool const notClosing = moving && staleTargetLastDistance > 0.0f && distance2d >= staleTargetLastDistance - 0.5f;
    if (!recoveryExhausted && targetStillPreferred && !geometryBlocked && !notClosing && (now - staleTargetLastProgress) < staleThreshold + 2u)
        return false;

    std::string const targetName = currentTarget->GetName();

    if (bot->GetVictim() == currentTarget)
        bot->AttackStop(true);

    DeferTargetGuidForBot(bot, targetGuid, recoveryExhausted ? 20 : 12);
    RESET_AI_VALUE(Unit*, "old target");
    RESET_AI_VALUE(Unit*, "current target");
    RESET_AI_VALUE(Unit*, "pull target");
    RESET_AI_VALUE(ObjectGuid, "attack target");
    bot->SetSelectionGuid(ObjectGuid());
    bool const expiredTravelReset = ClearExpiredTravelAfterStaleTargetReset();

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "target=" << targetName
            << " dist=" << std::fixed << std::setprecision(2) << distance
            << " staleFor=" << (now - staleTargetLastProgress)
            << " inCombat=" << (inCombat ? 1 : 0)
            << " moving=" << (moving ? 1 : 0)
            << " victim=" << (hasVictim ? 1 : 0)
            << " selection=" << (hasSelection ? 1 : 0)
            << " recentSpell=" << (recentSpell ? 1 : 0)
            << " healthChanged=" << (targetHealthChanged ? 1 : 0)
            << " preferred=" << (targetStillPreferred ? 1 : 0)
            << " attackers=" << (hasAttackers ? 1 : 0)
            << " dist2d=" << std::fixed << std::setprecision(2) << distance2d
            << " lastDist=" << std::fixed << std::setprecision(2) << staleTargetLastDistance
            << " closing=" << (closingDistance ? 1 : 0)
            << " usefulRange=" << (inUsefulCombatRange ? 1 : 0)
            << " los=" << (inLos ? 1 : 0)
            << " dz=" << std::fixed << std::setprecision(2) << verticalDelta
            << " geometryBlocked=" << (geometryBlocked ? 1 : 0)
            << " notClosing=" << (notClosing ? 1 : 0)
            << " expiredTravelReset=" << (expiredTravelReset ? 1 : 0);
        sPlayerbotAIConfig.logEvent(this, "StaleTargetReset", std::to_string(targetGuid.GetCounter()), out.str());
    }

    ResetStaleTargetState();
    return true;
}

void PlayerbotAI::Reset(bool full)
{
    AiObjectContext* context = aiObjectContext;

    if (bot->IsTaxiFlying())
        return;

    if (!HasActivePlayerMaster() && currentEngine == engines[(uint8)BotState::BOT_STATE_COMBAT] && sServerFacade.IsInCombat(bot) && time(0) - AI_VALUE(time_t,"combat start time") > 5 * MINUTE)
    {
        bot->CombatStop();
    }

    currentEngine = engines[(uint8)BotState::BOT_STATE_NON_COMBAT];
    currentState = BotState::BOT_STATE_NON_COMBAT;
    ResetAIInternalUpdateDelay();
    reactionEngine->Reset();
    whispers.clear();

    PullStrategy* strategy = PullStrategy::Get(this);
    if (strategy)
        strategy->OnPullEnded();

    RESET_AI_VALUE(Unit*,"old target");
    RESET_AI_VALUE(Unit*,"current target");
    RESET_AI_VALUE(Unit*,"pull target");
    RESET_AI_VALUE(ObjectGuid,"attack target");
    RESET_AI_VALUE(GuidPosition,"rpg target");
    RESET_AI_VALUE(LootObject,"loot target");
    RESET_AI_VALUE(uint32,"lfg proposal");
    RESET_AI_VALUE(time_t,"combat start time");
    bot->SetSelectionGuid(ObjectGuid());
    ResetStaleTargetState();

    LastSpellCast & lastSpell = AI_VALUE(LastSpellCast&,"last spell cast");
    lastSpell.Reset();

    if (bot->GetTradeData())
        bot->TradeCancel(true);

    if (full)
    {
        RESET_AI_VALUE(LastMovement&,"last movement");
        RESET_AI_VALUE(LastMovement&,"last area trigger");
        RESET_AI_VALUE(LastMovement&,"last taxi");

        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
        sTravelMgr.SetNullTravelTarget(target);
        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        target->SetExpireIn(1000);

        *AI_VALUE(FutureDestinations*, "future travel destinations") = FutureDestinations();
        RESET_AI_VALUE2(std::string, "manual string", "future travel purpose");
        RESET_AI_VALUE2(int, "manual int", "future travel relevance");

        InterruptSpell();

        StopMoving();

        jumpTime = 0;
        fallAfterJump = false;
        ResetJumpDestination();

        WorldSession* botWorldSessionPtr = bot->GetSession();
        bool logout = botWorldSessionPtr->ShouldLogOut(time(nullptr));

        // cancel logout
        if (!logout && (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut()))
        {
            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            TellPlayer(GetMaster(), BOT_TEXT("logout_cancel"));
        }
    }

    AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target").clear();

    if (bot->IsTaxiFlying())
    {
#ifdef MANGOS
        bot->m_taxi.ClearTaxiDestinations();
#endif
        bot->OnTaxiFlightEject(true);
    }

    if (full)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            engines[i]->Init();
        }
    }
}

std::map<std::string,ChatMsg> chatMap;

bool PlayerbotAI::IsAllowedCommand(std::string text)
{
    if (unsecuredCommands.empty())
    {
        unsecuredCommands.insert("who");
        unsecuredCommands.insert("where");
        unsecuredCommands.insert("wts");
        unsecuredCommands.insert("sendmail");
        unsecuredCommands.insert("invite");
        unsecuredCommands.insert("leave");
        unsecuredCommands.insert("join");
        unsecuredCommands.insert("lfg");
        unsecuredCommands.insert("guild invite");
        unsecuredCommands.insert("guild leave");
    }

    for (std::set<std::string>::iterator i = unsecuredCommands.begin(); i != unsecuredCommands.end(); ++i)
    {
        if (text.find(*i) == 0)
        {
            return true;
        }
    }

    return false;
}

void PlayerbotAI::HandleCommand(uint32 type, const std::string& text, Player& fromPlayer, const uint32 lang)
{
    std::string filtered = text;

    if (!IsAllowedCommand(filtered) && !GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_INVITE, type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_ADDON)
        return;

    if (filtered.find("BOT\t") == 0) //Mangosbot has BOT prefix so we remove that.
        filtered = filtered.substr(4);
    else if (lang == LANG_ADDON) //Other addon messages should not command bots.
        return;

    if (type == CHAT_MSG_SYSTEM)
        return;

    if (filtered.find(sPlayerbotAIConfig.commandSeparator) != std::string::npos)
    {
        std::vector<std::string> commands;
        split(commands, filtered, sPlayerbotAIConfig.commandSeparator.c_str());
        for (std::vector<std::string>::iterator i = commands.begin(); i != commands.end(); ++i)
        {
            HandleCommand(type, *i, fromPlayer);
        }
        return;
    }

    if (!sPlayerbotAIConfig.commandPrefix.empty())
    {
        if (filtered.find(sPlayerbotAIConfig.commandPrefix) != 0)
            return;

        filtered = filtered.substr(sPlayerbotAIConfig.commandPrefix.size());
    }

    if (chatMap.empty())
    {
        chatMap["#w "] = CHAT_MSG_WHISPER;
        chatMap["#p "] = CHAT_MSG_PARTY;
        chatMap["#r "] = CHAT_MSG_RAID;
        chatMap["#a "] = CHAT_MSG_ADDON;
        chatMap["#g "] = CHAT_MSG_GUILD;
    }
    currentChat = std::pair<ChatMsg, time_t>(CHAT_MSG_WHISPER, 0);
    for (std::map<std::string,ChatMsg>::iterator i = chatMap.begin(); i != chatMap.end(); ++i)
    {
        if (filtered.find(i->first) == 0)
        {
            filtered = filtered.substr(3);
            currentChat = std::pair<ChatMsg, time_t>(i->second, time(0) + 3);
            break;
        }
    }

    filtered = chatFilter.Filter(trim((std::string&)filtered));
    if (filtered.empty())
        return;

    if (filtered.substr(0, 6) == "debug ")
    {
        std::string response = HandleRemoteCommand(filtered.substr(6));
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_ADDON, response.c_str(), LANG_ADDON,
                CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());
        sServerFacade.SendPacket(&fromPlayer, data);
        return;
    }

    if (!IsAllowedCommand(filtered) && !GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_RAID_WARNING && filtered.find(bot->GetName()) != std::string::npos && filtered.find("award") == std::string::npos)
    {
        ChatCommandHolder cmd("warning", &fromPlayer, type);
        chatCommands.push(cmd);
        return;
    }

    if ((filtered.size() > 2 && filtered.substr(0, 2) == "d ") || (filtered.size() > 3 && filtered.substr(0, 3) == "do "))
    {
        Event event("do", "", &fromPlayer);
        std::string action = filtered.substr(filtered.find(" ") + 1);
        DoSpecificAction(action, event);
    }
    if (ChatHelper::parseValue("command", filtered).substr(0, 3) == "do ")
    {
        Event event("do", "", &fromPlayer);
        std::string action = ChatHelper::parseValue("command", filtered);
        action = action.substr(3);
        DoSpecificAction(action, event);
    }
    else if (type != CHAT_MSG_WHISPER && filtered.size() > 6 && filtered.substr(0, 6) == "queue ")
    {
        std::string remaining = filtered.substr(filtered.find(" ") + 1);
        int index = 1;
        Group* group = bot->GetGroup();
        if (group)
        {
            for (GroupReference *ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (ref->getSource() == master)
                    continue;

                if (ref->getSource() == bot)
                    break;

                index++;
            }
        }
        ChatCommandHolder cmd(remaining, &fromPlayer, type, time(0) + index);
        chatCommands.push(cmd);
    }
    else if (filtered == "reset")
    {
        Reset(true);
    }
    else if (filtered == "logout")
    {
        if (!(bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut()))
        {
            if (type == CHAT_MSG_WHISPER)
                TellPlayer(&fromPlayer, BOT_TEXT("logout_start"));

            if (master && master->GetPlayerbotMgr())
                SetShouldLogOut(true);
        }
    }
    else if (filtered == "logout cancel")
    {
        if (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut())
        {
            if (type == CHAT_MSG_WHISPER)
                TellPlayer(&fromPlayer, BOT_TEXT("logout_cancel"));

            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            SetShouldLogOut(false);
        }
    }
    else if ((filtered.size() > 5) && (filtered.substr(0, 5) == "wait ") && (filtered.find("wait for attack") == std::string::npos))
    {
        std::string remaining = filtered.substr(filtered.find(" ") + 1);
        uint32 delay = atof(remaining.c_str()) * static_cast<uint32>(IN_MILLISECONDS);
        if (delay > 20000)
        {
            TellPlayer(&fromPlayer, "Max wait time is 20 seconds!");
            return;
        }

        IncreaseAIInternalUpdateDelay(delay);
        isWaiting = true;
        TellPlayer(&fromPlayer, "Waiting for " + remaining + " seconds!");
        return;
    }
    else
    {
        SC_LOG("chat-cmd queued bot=%s from=%s type=%u text='%s'",
               bot ? bot->GetName() : "(null)",
               fromPlayer.GetName(),
               (unsigned)type,
               filtered.c_str());
        ChatCommandHolder cmd(filtered, &fromPlayer, type);
        chatCommands.push(cmd);
    }
}

void PlayerbotAI::HandleBotOutgoingPacket(const WorldPacket& packet)
{
    //if (packet.empty())
    //    return;

	switch (packet.GetOpcode())
	{
    case MSG_MOVE_TELEPORT_ACK:
    {
        WorldPacket p(packet);
        p.rpos(0);

#ifdef MANGOSBOT_TWO
        ObjectGuid moverGuid;
        p >> moverGuid.ReadAsPacked();
        if (moverGuid != bot->GetObjectGuid())
            return;
#else
        if (extractGuid(p) != bot->GetObjectGuid().GetRawValue())
            return;
#endif

        uint32 movementCounter = 0;
        p >> movementCounter;
        pendingTeleportAckCounter = movementCounter;
        return;
    }
	case SMSG_SPELL_FAILURE:
	{
		WorldPacket p(packet);
		p.rpos(0);
		ObjectGuid casterGuid;
        p >> casterGuid.ReadAsPacked();
		if (casterGuid != bot->GetObjectGuid())
			return;

		uint32 spellId;
		p >> spellId;
		SpellInterrupted(spellId);
		return;
	}
	case SMSG_SPELL_DELAYED:
	{
		WorldPacket p(packet);
		p.rpos(0);
		ObjectGuid casterGuid;
        p >> casterGuid.ReadAsPacked();

		if (casterGuid != bot->GetObjectGuid())
			return;

		uint32 delaytime;
		p >> delaytime;
		if (delaytime <= 1000)
            IncreaseAIInternalUpdateDelay(delaytime);
		return;
	}
    case SMSG_EMOTE: // do not react to NPC emotes
    {
        WorldPacket p(packet);
        ObjectGuid source;
        uint32 emoteId;
        p.rpos(0);
        p >> emoteId >> source;
        if (source.IsPlayer())
            botOutgoingPacketHandlers.AddPacket(packet);

        return;
    }
    case SMSG_MESSAGECHAT: // do not react to self or if not ready to reply
    {
        if (!AllowActivity())
            return;

        WorldPacket p(packet);
#ifndef MANGOSBOT_ZERO
        if (!p.empty() && (p.GetOpcode() == SMSG_MESSAGECHAT || p.GetOpcode() == SMSG_GM_MESSAGECHAT))
#else
        if (!p.empty() && p.GetOpcode() == SMSG_MESSAGECHAT)
#endif
        {
            p.rpos(0);
            uint8 msgtype, chatTag;
            uint32 lang, textLen, unused;
            ObjectGuid guid1, guid2;
            std::string name, chanName, message;
            p >> msgtype >> lang;

            // filter msg type
            switch (msgtype)
            {
            case CHAT_MSG_CHANNEL:
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
#ifdef MANGOSBOT_TWO
            case CHAT_MSG_PARTY_LEADER:
#endif
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_GUILD:
                break;
            default:
                return;
                break;
            }

#ifdef MANGOSBOT_ZERO
            switch (msgtype)
            {
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_YELL:
                p >> guid1 >> guid2;
                break;
            case CHAT_MSG_CHANNEL:
                p >> chanName >> unused >> guid1;
                break;
            default:
                p >> guid1;
                break;
            }

            if (guid1.IsEmpty() || p.size() > 0x1000)
                return;

            p >> textLen >> message >> chatTag;
#endif
#ifdef MANGOSBOT_ONE
            p >> guid1 >> unused;
            if (guid1.IsEmpty() || p.size() > 0x1000)
                return;

            switch (msgtype)
            {
            case CHAT_MSG_CHANNEL:
                p >> chanName;
                [[fallthrough]];
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_GUILD:
                p >> guid2;
                p >> textLen >> message >> chatTag;
                break;
            default:
                break;
            }
#endif
#ifdef MANGOSBOT_TWO
            p >> guid1 >> unused;
            if (guid1.IsEmpty() || p.size() > 0x1000)
                return;

            if (p.GetOpcode() == SMSG_GM_MESSAGECHAT)
            {
                p >> textLen;
                p >> name;
            }

            switch (msgtype)
            {
            case CHAT_MSG_CHANNEL:
                p >> chanName;
                [[fallthrough]];
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_GUILD:
                p >> guid2;
                p >> textLen >> message >> chatTag;
                break;
            default:
                break;
            }
#endif

            bool isAiChat = sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3);

            if (m_recordIncommingMessages)
            {
                std::string recievedMessage = message;

                std::string senderName;
                if(!sObjectMgr.GetPlayerNameByGUID(guid1, senderName))
                    senderName = "unknown";

                std::string recievedChatType;

                //Add chat type prefix before recieved message. Ie. for Guilg "guild:".
                switch (msgtype)
                {
                    case CHAT_MSG_SAY:
                        recievedChatType = "says";
                        break;
                    case CHAT_MSG_YELL:
                        recievedChatType = "yells";
                        break;
                    case CHAT_MSG_PARTY:
                        recievedChatType = "party";
                        break;
#ifdef MANGOSBOT_TWO
                    case CHAT_MSG_PARTY_LEADER:
                        recievedChatType = "party leader";
                        break;
#endif
                    case CHAT_MSG_GUILD:
                        recievedChatType = "guild";
                        break;
                    case CHAT_MSG_OFFICER:
                        recievedChatType = "officer";
                        break;
                    case CHAT_MSG_WHISPER:
                        //Get player name for whisper and add "<whispe:" before message.
                        recievedChatType = "whispers";
                        break;
                    case CHAT_MSG_CHANNEL: //Channel needs channel name to be added as prefix, so we add "c:" before channel name.
                        recievedChatType = chanName;
                        break;
                    default:
                        break;
                }

                recievedMessage = "[" + senderName + "] " + recievedChatType + ": " + recievedMessage;

                m_recordedMessages.push_back(recievedMessage);
            }

            if (isAiChat && (lang == LANG_ADDON || message.find("d:") == 0))
                return;

            if (guid1 != bot->GetObjectGuid()) // do not reply to self
            {
                // try to always reply to real player
                time_t lastChat = GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Get();
                bool isPaused = time(0) < lastChat;
                bool shouldReply = false;
                bool isFromFreeBot = false;
                sObjectMgr.GetPlayerNameByGUID(guid1, name);
                uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guid1);
                isFromFreeBot = sPlayerbotAIConfig.IsInRandomAccountList(accountId);
                if (!isFromFreeBot)
                {

                    isFromFreeBot = sPlayerbotAIConfig.IsFreeAltBot(guid1);

                    if (isFromFreeBot)
                    {
                        Player* player = sObjectMgr.GetPlayer(guid1);
                        if (player && player->isRealPlayer())
                            isFromFreeBot = false;
                    }
                }

                bool isMentioned = message.find(bot->GetName()) != std::string::npos;
                

                ChatChannelSource chatChannelSource = GetChatChannelSource(bot, msgtype, chanName);

                if (!isAiChat || isFromFreeBot)
                {
                    // random bot speaks, chat CD
                    if ((isFromFreeBot || isAiChat) && isPaused)
                        return;

                    // BG: react only if mentioned or if not channel and real player spoke
                    if (bot->InBattleGround() && !(isMentioned || (msgtype != CHAT_MSG_CHANNEL && !isFromFreeBot)))
                        return;

                    if (HasRealPlayerMaster() && guid1 != GetMaster()->GetObjectGuid())
                        return;

                    if (lang == LANG_ADDON)
                        return;

                    if (boost::algorithm::istarts_with(message, sPlayerbotAIConfig.toxicLinksPrefix)
                        && (GetChatHelper()->ExtractAllItemIds(message).size() > 0 || GetChatHelper()->ExtractAllQuestIds(message).size() > 0)
                        && sPlayerbotAIConfig.toxicLinksRepliesChance)
                    {
                        if (urand(0, 50) > 0 || urand(1, 100) > sPlayerbotAIConfig.toxicLinksRepliesChance)
                        {
                            return;
                        }
                    }
                    else if ((GetChatHelper()->ExtractAllItemIds(message).count(19019) && sPlayerbotAIConfig.thunderfuryRepliesChance))
                    {
                        if (urand(0, 60) > 0 || urand(1, 100) > sPlayerbotAIConfig.thunderfuryRepliesChance)
                        {
                            return;
                        }
                    }
                    else
                    {
                        if (isFromFreeBot && urand(0, 20))
                            return;

                        if (msgtype == CHAT_MSG_GUILD && (!sPlayerbotAIConfig.guildRepliesRate || urand(1, 100) >= sPlayerbotAIConfig.guildRepliesRate))
                            return;

                        if (!isFromFreeBot)
                        {
                            // WHO IS BEING ADDRESSED (user: "bots whisper me from all over"): for
                            // say/yell from a real player, a bot only treats it as directed at them
                            // when it plausibly IS: the player targets this bot, said its name, or
                            // stands close AND faces it. Everyone else stays quiet.
                            if (isAiChat && (msgtype == CHAT_MSG_SAY || msgtype == CHAT_MSG_YELL))
                            {
                                Player* speaker = sObjectAccessor.FindPlayer(guid1);
                                bool targeted = speaker && speaker->GetSelectionGuid() == bot->GetObjectGuid();
                                bool addressed = targeted || isMentioned;
                                if (!addressed && speaker && speaker->IsInWorld() &&
                                    speaker->GetMapId() == bot->GetMapId() &&
                                    speaker->GetDistance(bot) <= 8.0f &&
                                    speaker->HasInArc(bot, M_PI_F / 3.0f))
                                    addressed = true;
                                if (!addressed)
                                    return;
                            }
                            else if (!isMentioned && urand(0, 4))
                                return;
                        }
                        else
                        {
                            if (urand(0, 20 + 10 * isMentioned))
                                return;
                        }
                    }
                }

                MANGOS_ASSERT(!message.empty());     
                QueueChatResponse(msgtype, guid1, ObjectGuid(), message, chanName, name, isAiChat);
                GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));

                return;
            }

            if (isAiChat)
            {
                ChatChannelSource chatChannelSource = bot->GetPlayerbotAI()->GetChatChannelSource(bot, msgtype, chanName);

                std::string llmChannel;

                if (!sPlayerbotAIConfig.llmGlobalContext)
                    llmChannel = ((chatChannelSource == ChatChannelSource::SRC_WHISPER) ? name : std::to_string(chatChannelSource));

                AiObjectContext* context = aiObjectContext;
                std::string llmContext = AI_VALUE(std::string, "manual string::llmcontext" + llmChannel);
                llmContext = llmContext + " " + bot->GetName() + ":" + message;
                PlayerbotLLMInterface::LimitContext(llmContext, llmContext.size());
                SET_AI_VALUE(std::string, "manual string::llmcontext" + llmChannel, llmContext);
            }
        }

        return;
    }
    case SMSG_MOVE_KNOCK_BACK: // handle knockbacks
    {
        if (!sPlayerbotAIConfig.useKnockback || !HasPlayerNearby())
            return;

        WorldPacket p(packet);
        p.rpos(0);

        ObjectGuid guid;
        uint32 counter;
        float vcos, vsin, horizontalSpeed, verticalSpeed = 0.f;

        p >> guid.ReadAsPacked() >> counter >> vcos >> vsin >> horizontalSpeed >> verticalSpeed;
        verticalSpeed = -verticalSpeed;

        // stop casting
        InterruptSpell(false);

        // stop movement
        StopMoving();

        float timeToLand, distToLand, maxHeight;
        bool goodLanding = true;
        float angleRadian = (vsin > 0) ? acos(vcos) : -acos(vcos);
        float angle = angleRadian;
        std::vector<WorldPosition> path;
        WorldPosition dest_calculated = JumpAction::CalculateJumpParameters(WorldPosition(bot), bot, angle, verticalSpeed, horizontalSpeed, timeToLand, distToLand, maxHeight, goodLanding, path, 200.f);
        bool jumpInPlace = horizontalSpeed == 0.f;
        if (!dest_calculated)
        {
            sLog.outDetail("Knockback fail to calculate");
            return;
        }

        // set fall height for fall damage calculations
        bot->SetFallInformation(0, maxHeight);

        // fix height
        if (goodLanding)
        {
            float ox = dest_calculated.getX();
            float oy = dest_calculated.getY();
            float oz = dest_calculated.getZ();
            bot->UpdateAllowedPositionZ(ox, oy, oz);
            // set to fall after land if not at the ground
            if (fabs(oz - dest_calculated.getZ()) > 5.0f)
            {
                SetFallAfterJump();
            }
            else
            {
                dest_calculated = WorldPosition(dest_calculated.getMapId(), ox, oy, oz);
            }

        }
        else
            SetFallAfterJump();


        // add moveflags
#ifdef MANGOSBOT_ZERO
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_JUMPING);
        bot->m_movementInfo.AddMovementFlag(MOVEFLAG_BACKWARD);
#else
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_FALLING);
        bot->m_movementInfo.AddMovementFlag(MOVEFLAG_BACKWARD);
#endif

        // send ack
        WorldPacket ack(CMSG_MOVE_KNOCK_BACK_ACK);
        bot->m_movementInfo.jump.cosAngle = vcos;
        bot->m_movementInfo.jump.sinAngle = vsin;
        bot->m_movementInfo.jump.zspeed = -verticalSpeed;
        bot->m_movementInfo.jump.xyspeed = horizontalSpeed;
#ifdef MANGOSBOT_TWO
        ack << bot->GetObjectGuid().WriteAsPacked();
#else
        ack << bot->GetObjectGuid();
#endif
        ack << uint32(0);
        ack << bot->m_movementInfo;
        bot->GetSession()->HandleMoveKnockBackAck(ack);

        // write jump time
        uint32 curTime = sWorld.GetCurrentMSTime();
        jumpTime = curTime + sWorld.GetAverageDiff() + (uint32)(timeToLand * static_cast<uint32>(IN_MILLISECONDS)) + 1000;
        SetJumpDestination(dest_calculated);

        // set highest jump point to relocate
        WorldPosition highestPoint = dest_calculated;
        for (auto& point : path)
        {
            if (point.getZ() > highestPoint.getZ())
                highestPoint = point;
        }

        bot->Relocate(highestPoint.getX(), highestPoint.getY(), highestPoint.getZ());
        //bot->m_movementInfo.ChangePosition(dest_calculated.getX(), dest_calculated.getY(), dest_calculated.getZ(), bot->GetOrientation());
        sLog.outDetail("%s: KNOCKBACK x: %f, y: %f, z: %f, time: %f, dist: %f, maxHeight: %f inPlace: %u, landTime: %u", bot->GetName(), dest_calculated.getX(), dest_calculated.getY(), dest_calculated.getZ(), timeToLand, distToLand, maxHeight, jumpInPlace, jumpTime);
        return;
    }
	default:
		// log group invite arrivals so we
		// can verify the auto-accept handler fires. If we see this line in
		// the server log but no subsequent "AcceptInvitationAction" event,
		// the strategy/trigger is registered but the action isn't being
		// scheduled — points us at engine priority or trigger.Check() bugs.
		if (packet.GetOpcode() == SMSG_GROUP_INVITE) {
			SC_LOG("bot=%s received SMSG_GROUP_INVITE — queued for 'group invite' trigger",
			       bot ? bot->GetName() : "(null)");
		}
		botOutgoingPacketHandlers.AddPacket(packet);
	}
}

void PlayerbotAI::SpellInterrupted(uint32 spellid)
{
    LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();
    if (!spellid || lastSpell.id != spellid)
        return;

    time_t now = time(0);
    if (now <= lastSpell.time)
        return;

    uint32 castTimeSpent = 1000 * (now - lastSpell.time);
    uint32 globalCooldown = CalculateGlobalCooldown(lastSpell.id);
    if (castTimeSpent < globalCooldown)
        SetAIInternalUpdateDelay(globalCooldown - castTimeSpent);
    else
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);

    lastSpell.id = 0;
}

int32 PlayerbotAI::CalculateGlobalCooldown(uint32 spellid)
{
    if (!spellid)
        return 0;

    int32 globalCooldown = 0;
    SpellEntry const* spellEntry = sSpellTemplate.LookupEntry<SpellEntry>(spellid);
    if (spellEntry)
    {
        globalCooldown = spellEntry->StartRecoveryTime;
    }

    return globalCooldown > 0 ? globalCooldown : sPlayerbotAIConfig.reactDelay;
}

void PlayerbotAI::HandleMasterIncomingPacket(const WorldPacket& packet)
{
    masterIncomingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::HandleMasterOutgoingPacket(const WorldPacket& packet)
{
    masterOutgoingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::ChangeEngine(BotState type)
{
    Engine* engine = engines[(uint8)type];

    if (currentEngine != engine)
    {
        currentEngine = engine;
        currentState = type;
        ReInitCurrentEngine();

        switch (type)
        {
        case BotState::BOT_STATE_COMBAT:
            sLog.outDebug( "=== %s COMBAT ===", bot->GetName());
            break;
        case BotState::BOT_STATE_NON_COMBAT:
            sLog.outDebug( "=== %s NON-COMBAT ===", bot->GetName());
            break;
        case BotState::BOT_STATE_DEAD:
            sLog.outDebug( "=== %s DEAD ===", bot->GetName());
            break;
        case BotState::BOT_STATE_REACTION:
            sLog.outDebug("=== %s REACTION ===", bot->GetName());
            break;
        default: break;
        }
    }
}


// ===================== AUTONOMOUS FSM (decision-engine replacement) =========================
// The measured pathology (headless observer + bot_events, 2026-07-08, active near-player cohort):
// moving 15% / combat 6% / target=none 37%; apm median 14, p90 162, MAX 966 -- i.e. a quarter of the
// active cohort makes 20-966 decisions/min while moving=0 AND combat=0. The relevance engine can pick
// an action that never executes and then re-pick it forever (no execute-or-fail damper). This FSM
// replaces the top-level DECISION for autonomous (no real master), out-of-combat, random bots: it
// picks ONE concrete action and then COMMITS -- it will NOT re-decide until that action's observable
// effect resolves (moved toward target / arrived / entered combat) or a break event fires. Combat
// rotation, combat reactions, quests/rpg/rest/buffs are left to the existing engine (delegated).
// Rollback: AiPlayerbot.AutonomousFSM = 0.
enum FsmVerdict { FSM_DELEGATE = 0, FSM_HANDLED = 1 };

struct FsmTrack
{
    uint32 commitUntil = 0;      // suppress re-decision until this ms (the anti-thrash core)
    uint32 idleSince = 0;        // first ms we had nothing concrete to do
    uint32 idleDelegateAt = 0;   // next ms we permit an engine delegation (throttles idle-thrash)
    uint32 relocateAt = 0;       // next ms a relocate is allowed
    float  lastX = 0.f, lastY = 0.f;
    uint32 lastMoveCheck = 0;
    uint8  state = 0;            // 0 idle 1 grind 2 travel 3 loot 4 pursue-content
    uint8  stuckStrikes = 0;     // consecutive no-displacement checks while committed to a move
    uint32 grindSkipUntil = 0;   // skip GRIND after a stuck pull (the mob is unreachable)
    uint32 lookAliveAt = 0;      // next ms a watched-idle bot performs a look-alive action
    float  destX = 0.f, destY = 0.f, destZ = 0.f;  // committed mob-camp destination (pursue-content)
    uint32 destPickAt = 0;       // next ms we may re-pick the destination
};

// ---- FSM branch telemetry (which branch each autonomous non-combat bot takes; per-min -> fsm_branch.csv)
static std::atomic<uint32> s_fsmGrind{0}, s_fsmPursue{0}, s_fsmPursueMove{0}, s_fsmFollow{0},
                           s_fsmLoot{0}, s_fsmHold{0}, s_fsmNoSpot{0};
static std::atomic<uint32> s_fsmCalls{0}, s_fsmSit{0}, s_fsmInvite{0};   // diagnostic: where do bots go?
static std::atomic<uint32> s_fsmDumpDueMs{0};
static std::mutex s_fsmDumpMx;
static void FsmBranchTelemetry(uint32 nowMs)
{
    if (nowMs < s_fsmDumpDueMs.load(std::memory_order_relaxed) || !s_fsmDumpMx.try_lock())
        return;
    if (nowMs >= s_fsmDumpDueMs.load(std::memory_order_relaxed))
    {
        s_fsmDumpDueMs.store(nowMs + 60000, std::memory_order_relaxed);
        FILE* f = fopen("logs/fsm_branch.csv", "a");
        if (!f) f = fopen("../logs/fsm_branch.csv", "a");
        if (f)
        {
            time_t tt = time(0); struct tm tmv; localtime_r(&tt, &tmv); char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
            fprintf(f, "%s,calls=%u,grind=%u,pursue=%u,pursueMove=%u,follow=%u,loot=%u,hold=%u,sit=%u,invite=%u,nospot=%u\n", ts,
                s_fsmCalls.exchange(0), s_fsmGrind.exchange(0), s_fsmPursue.exchange(0), s_fsmPursueMove.exchange(0),
                s_fsmFollow.exchange(0), s_fsmLoot.exchange(0), s_fsmHold.exchange(0),
                s_fsmSit.exchange(0), s_fsmInvite.exchange(0), s_fsmNoSpot.exchange(0));
            fclose(f);
        }
    }
    s_fsmDumpMx.unlock();
}

static FsmVerdict AutonomousFsmTick(PlayerbotAI* ai, Player* bot, bool minimal, bool* executed)
{
    *executed = false;
    s_fsmCalls.fetch_add(1, std::memory_order_relaxed);   // every FSM entry (diagnostic)
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive())
        return FSM_DELEGATE;
    // PENDING GROUP INVITE must be handled by the engine IMMEDIATELY -- the accept-invitation action
    // lives there, and the FSM's idle throttle otherwise delays acceptance by seconds (measured: a
    // real player inviting fleet bots got only 2/8 to accept, slowly). Never let the FSM sit on a
    // real player's invite; delegate every tick until the invite resolves.
    if (bot->GetGroupInvite())
        { s_fsmInvite.fetch_add(1, std::memory_order_relaxed); return FSM_DELEGATE; }
    if (bot->IsTaxiFlying() || bot->IsBeingTeleported() || !bot->IsStandState())
        { s_fsmSit.fetch_add(1, std::memory_order_relaxed); ai->SetAIInternalUpdateDelay(500); *executed = true; return FSM_HANDLED; }  // busy/sitting

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return FSM_DELEGATE;

    const uint32 now = WorldTimer::getMSTime();
    const uint32 guid = bot->GetGUIDLow();
    static std::mutex fsmMx;
    static std::unordered_map<uint32, FsmTrack> fsmTracks;

    FsmTrack s;
    { std::lock_guard<std::mutex> lk(fsmMx); s = fsmTracks[guid]; }
    // Every FSM-handled path MUST set an update delay, else the bot re-ticks at the floor rate and
    // spins (apm explosion) even though the commit window blocks re-DECISION. The delay makes the
    // bot actually SLEEP until its next meaningful check. reactDelay(~100ms) is the floor.
    auto handled = [&](bool exec, uint32 sleepMs) -> FsmVerdict {
        { std::lock_guard<std::mutex> lk(fsmMx); fsmTracks[guid] = s; }
        ai->SetAIInternalUpdateDelay(std::max<uint32>(sleepMs, sPlayerbotAIConfig.reactDelay));
        *executed = exec;
        return FSM_HANDLED;
    };
    auto save = [&]() { std::lock_guard<std::mutex> lk(fsmMx); fsmTracks[guid] = s; };

    const float x = bot->GetPositionX(), y = bot->GetPositionY();
    FsmBranchTelemetry(now);

    // ---- FOLLOW/ASSIST: a grouped NON-leader bot sticks to its leader and focus-fires the leader's
    // target -- real party behavior ("don't fall behind, be active + helping"). FormGrindingParties
    // (world thread) creates the parties; this map-thread branch drives the followers. The leader is a
    // normal autonomous bot (grinds/travels via the rest of the FSM); followers converge + assist.
    if (sPlayerbotAIConfig.autonomousParties && bot->GetGroup() && !ai->IsGroupLeader())
    {
        Player* leader = ai->GetGroupMaster();
        if (leader && leader != bot && leader->IsInWorld() && leader->IsAlive())
        {
            s_fsmFollow.fetch_add(1, std::memory_order_relaxed);
            const bool sameMap = leader->GetMapId() == bot->GetMapId();
            const float ld = sameMap ? bot->GetDistance(leader) : 99999.f;

            // ASSIST: leader is fighting -> engage its target (focus fire). Entering combat hands off
            // to the combat director, which STICKS to the target.
            Unit* lv = leader->GetVictim();
            if (lv && sServerFacade.IsAlive(lv) && !bot->IsInCombat() && sameMap && ld < 60.0f)
            {
                context->GetValue<Unit*>("current target")->Set(lv);
                const bool a = ai->DoSpecificAction("reach melee", Event(), true) ||
                               ai->DoSpecificAction("attack anything", Event(), true);
                return handled(a, 300);
            }

            // CATCH-UP: too far behind / on another map, unseen, out of combat -> teleport to the leader
            // (the "never fall behind in dungeons/raids" guard). Only when a human can't see the blink.
            if ((!sameMap || ld > 80.0f) && !bot->IsInCombat() && !RealPlayerWithin(bot, 120.0f))
            {
                sRandomPlayerbotMgr.QueueBotTeleport(guid, leader->GetMapId(),
                    leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), false);
                return handled(true, 1500);
            }

            // FOLLOW: run to the leader when spread out; hold when close.
            if (sameMap && ld > 12.0f && !bot->IsInCombat())
            {
                const bool a = ai->DoSpecificAction("follow", Event(), true);
                return handled(a, 400);
            }
            if (!bot->IsInCombat())
                return handled(false, 800);   // close & leader idle -> hold near leader
            // in combat -> fall through (combat director / engine handles the fight)
        }
        // leader invalid -> fall through to normal solo FSM; group disbands naturally.
    }

    // ---- COMMIT WINDOW: while a concrete action is in flight, verify progress; do NOT re-decide.
    if (now < s.commitUntil)
    {
        if (s.state == 2 /*travel*/ && now - s.lastMoveCheck > 1200)
        {
            const float moved = sqrtf((x - s.lastX) * (x - s.lastX) + (y - s.lastY) * (y - s.lastY));
            s.lastX = x; s.lastY = y; s.lastMoveCheck = now;
            if (moved < 3.0f) { if (++s.stuckStrikes >= 2) s.commitUntil = 0; }  // unroutable -> break
            else s.stuckStrikes = 0;
        }
        // GRIND verify: a bot committed to a pull must be moving toward the mob OR in combat. If it is
        // neither for 2 checks, the mob is unreachable -> break the commit and blacklist GRIND briefly
        // so the bot doesn't re-pick the same unreachable mob and spin (the residual apm>=100 mv0 case).
        if (s.state == 1 /*grind*/ && !bot->IsInCombat() && now - s.lastMoveCheck > 800)
        {
            const float moved = sqrtf((x - s.lastX) * (x - s.lastX) + (y - s.lastY) * (y - s.lastY));
            s.lastX = x; s.lastY = y; s.lastMoveCheck = now;
            if (moved < 2.0f) { if (++s.stuckStrikes >= 2) { s.commitUntil = 0; s.grindSkipUntil = now + 6000; } }
            else s.stuckStrikes = 0;
        }
        if (now < s.commitUntil)
        {
            s_fsmHold.fetch_add(1, std::memory_order_relaxed);
            // sleep until the commit expires (cap so travel re-checks displacement ~every 600ms)
            return handled(true, std::min<uint32>(s.commitUntil - now, 600));
        }
    }

    // ---- DECIDE: one action, one commit. -------------------------------------------------------

    // (A) LOOT owed -> run the loot chain first (Goal 4).
    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    if (AI_VALUE(bool, "has available loot") || bot->GetLootGuid() || !lootTarget.IsEmpty())
    {
        const bool acted = ai->DoSpecificAction("move to loot", Event(), true) ||
                           ai->DoSpecificAction("open loot", Event(), true) ||
                           ai->DoSpecificAction("loot", Event(), true);
        s.state = 3; s.commitUntil = now + 800; s.stuckStrikes = 0; s.idleSince = 0;
        s_fsmLoot.fetch_add(1, std::memory_order_relaxed);
        return handled(acted, 400);
    }

    // (B) GRIND: a live, level-appropriate mob within grind range -> engage. "attack anything" sets
    //     up a persistent MoveChase, so one call + a commit window makes the bot close and fight
    //     WITHOUT re-deciding every tick (the apm-966 fix).
    Unit* gt = (now >= s.grindSkipUntil) ? AI_VALUE(Unit*, "grind target") : nullptr;
    if (gt && sServerFacade.IsAlive(gt) && sServerFacade.GetDistance2d(bot, gt) <= sPlayerbotAIConfig.grindDistance)
    {
        const bool acted = ai->DoSpecificAction("attack anything", Event(), true);
        if (acted)
        {
            s.state = 1;
            s.commitUntil = now + std::min<uint32>(sPlayerbotAIConfig.maxWaitForMove, 2500);
            s.lastX = x; s.lastY = y; s.lastMoveCheck = now; s.stuckStrikes = 0; s.idleSince = 0;
            s_fsmGrind.fetch_add(1, std::memory_order_relaxed);
            return handled(true, 500);   // engaged; re-check in 0.5s (combat reconcile takes over on aggro)
        }
        // "attack anything" DECLINED this mob (grey / tapped / swarmed / unreachable). Do NOT commit to
        // a hold here (that was the trap: bots standing on a mob they won't attack). Skip grind briefly
        // and WALK to a fresh, level-appropriate camp via PURSUE below.
        s.grindSkipUntil = now + 5000;
    }

    // (C) TRAVEL branch REMOVED. It relied on the engine's "travel target", which is the DEAD travel
    //     pipeline: 71% of idle bots had a travel target stuck in COOLDOWN, "travel target active"
    //     stayed true, so "move to travel target" fired every tick, no-op'd (unroutable), and the bot
    //     HELD in a commit window -- never walking, never reaching PURSUE. That trap is why 80% of
    //     open-world bots stood idle. The brain now owns ALL movement via PURSUE below (its own
    //     content search), so the engine's travel target is never consulted again.

    // (D) PURSUE CONTENT -- NEVER STAND IDLE. No loot, no mob in grind range, no active travel. Rather
    //     than hand the bot back to the dead travel pipeline (measured live: 85% of near-player bots
    //     standing still, 67% with NO travel destination -> they just stood there), the FSM OWNS the
    //     content search: pick the nearest real mob camp (from the spawn cache) and WALK to it, every
    //     tick, until a mob is in grind range (then GRIND above engages). This is the "always doing
    //     something purposeful" rewrite -- no hold, no stroll-in-place, no delegation-to-idle.
    s.state = 4;
    if (!s.idleSince) s.idleSince = now;

    const bool haveDest = (s.destX != 0.f || s.destY != 0.f);
    const float dxd = s.destX - x, dyd = s.destY - y;
    const float destDist = haveDest ? sqrtf(dxd * dxd + dyd * dyd) : 1e9f;

    // (Re)pick a destination when we have none, arrived (mobs should now be in grind range), or it aged.
    bool pickedNew = false;
    if (!haveDest || destDist < 25.0f || now >= s.destPickAt)
    {
        float nx, ny, nz;
        if (sRandomPlayerbotMgr.GetNearestGrindSpot(bot, 60.0f, nx, ny, nz))
        {
            s.destX = nx; s.destY = ny; s.destZ = nz; s.destPickAt = now + 20000;
            s.lastX = x; s.lastY = y; s.lastMoveCheck = now; s.stuckStrikes = 0;
            pickedNew = true;
        }
        else
        {
            s_fsmNoSpot.fetch_add(1, std::memory_order_relaxed);
            // No cached spot on this map (rare). Unseen -> level teleport to content; seen -> short roam.
            if (!RealPlayerWithin(bot, 180.f) && !bot->IsInCombat())
            {
                sRandomPlayerbotMgr.QueueBotTeleport(guid, 0, 0.f, 0.f, 0.f, /*forLevel*/ true);
                s.destX = s.destY = 0.f;
                return handled(true, 1500);
            }
            s.destX = s.destY = 0.f;
            return handled(ai->DoSpecificAction("move random", Event(), true), 1200);
        }
    }

    // ALWAYS WALK toward content -- we do NOT teleport-then-sleep here (that queued teleport made the
    // bot sit mv0 for 1.5s each tick = the "standing around" the player sees). Verify displacement; if
    // walking is genuinely BLOCKED for 2 checks, THEN teleport as a stuck-escape (unseen only) or
    // re-pick another camp. Otherwise the stepped walk below keeps the bot continuously moving.
    if (!pickedNew && now - s.lastMoveCheck > 1200)
    {
        const float moved = sqrtf((x - s.lastX) * (x - s.lastX) + (y - s.lastY) * (y - s.lastY));
        s.lastX = x; s.lastY = y; s.lastMoveCheck = now;
        if (moved < 3.0f)
        {
            if (++s.stuckStrikes >= 2)
            {
                s.stuckStrikes = 0;
                if (destDist > 200.0f && !RealPlayerWithin(bot, 200.f) && !bot->IsInCombat())
                    sRandomPlayerbotMgr.QueueBotTeleport(guid, bot->GetMapId(), s.destX, s.destY, s.destZ, false);
                s.destX = s.destY = 0.f; s.destPickAt = 0;   // re-pick / re-plan next tick
                return handled(true, 1000);
            }
        }
        else s.stuckStrikes = 0;
    }
    // WALK toward the camp. For a FAR (often cross-zone) camp, MovePoint to the exact far point fails
    // to pathfind and the bot just STANDS (the L60-in-Stormwind bug). So step toward it in a reachable
    // ~120y hop -- the bot is ALWAYS walking toward content, on foot when a human is watching (visible,
    // purposeful) and eventually reaching mobs where GRIND engages.
    float wx = s.destX, wy = s.destY, wz = s.destZ;
    if (destDist > 150.0f)
    {
        const float f = 120.0f / destDist;
        wx = x + (s.destX - x) * f;
        wy = y + (s.destY - y) * f;
        wz = bot->GetPositionZ();   // pathfinder snaps to ground
    }
    s_fsmPursue.fetch_add(1, std::memory_order_relaxed);
    if (pickedNew || !sServerFacade.isMoving(bot))
    {
        bot->GetMotionMaster()->MovePoint(bot->GetMapId(), wx, wy, wz,
                                          MOVE_PATHFINDING | MOVE_RUN_MODE, 0.0f);
        s_fsmPursueMove.fetch_add(1, std::memory_order_relaxed);
    }
    return handled(true, 500);   // walking toward content; re-check in 0.5s (GRIND takes over when close)
}

// ---- Combat-director telemetry (O(1)/tick atomic counters, dumped once/min to combat_director.csv).
static std::atomic<uint32> s_cdInRange{0};   // delegated: in range, engine rotates
static std::atomic<uint32> s_cdChase{0};     // director forced/held a chase
static std::atomic<uint32> s_cdReached{0};   // chase converted to in-range this window
static std::atomic<uint32> s_cdGaveUp{0};    // stuck-chase guard dropped an unreachable target
static std::atomic<uint32> s_cdDumpDueMs{0};
static std::mutex s_cdDumpMx;
static void CombatDirectorTelemetry(uint32 nowMs)
{
    if (nowMs < s_cdDumpDueMs.load(std::memory_order_relaxed) || !s_cdDumpMx.try_lock())
        return;
    if (nowMs >= s_cdDumpDueMs.load(std::memory_order_relaxed))
    {
        s_cdDumpDueMs.store(nowMs + 60000, std::memory_order_relaxed);
        FILE* f = fopen("logs/combat_director.csv", "a");
        if (!f) f = fopen("../logs/combat_director.csv", "a");
        if (f)
        {
            time_t tt = time(0); struct tm tmv; localtime_r(&tt, &tmv); char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
            fprintf(f, "%s,inrange=%u,chase=%u,reached=%u,gaveup=%u\n", ts,
                s_cdInRange.exchange(0), s_cdChase.exchange(0),
                s_cdReached.exchange(0), s_cdGaveUp.exchange(0));
            fclose(f);
        }
    }
    s_cdDumpMx.unlock();
}

// COMBAT DIRECTOR: "less thinking, more acting" for autonomous random bots in combat. Measured
// pathology on live: 68% of in-combat bots are NOT moving (chase-stalled) while the relevance engine
// re-decides ~5x/sec (apm median 283, max 916) -- it keeps clearing/re-picking instead of COMMITTING
// to chase the target it already has. This director enforces the simple, snappy rule: if I have a live
// target and I'm out of range and healthy and can move -> CHASE it (persistent MoveChase, tested
// reach action) with a SHORT re-check delay, so the bot STICKS and closes fast. Melee -> reach melee
// (melee range); ranged/caster -> reach spell (spell range). A STUCK-CHASE guard drops a target the
// bot cannot close on for several seconds (the "chase a fleeing/unreachable bot for 20 min" lock).
// In range, low HP (flee), rooted/feared, mid-cast, or no target -> delegated to the engine unchanged.
// The reaction engine (flee/AoE/interrupt) runs earlier in UpdateAI and is untouched. Rollback:
// AiPlayerbot.CombatDirector=0.
struct CdTrack { uint32 tgtLow=0; float lastDist=0.f; uint32 lastProgressMs=0; };

static FsmVerdict CombatFsmTick(PlayerbotAI* ai, Player* bot, bool* executed)
{
    *executed = false;
    if (!ai || !bot || !bot->IsInWorld() || !bot->IsAlive())
        return FSM_DELEGATE;
    if (bot->GetHealthPercent() < 30.0f)
        return FSM_DELEGATE;                                   // survival -> engine decides flee/heal
    if (!ai->CanMove())
        return FSM_DELEGATE;                                   // rooted/feared/stunned -> engine
    if (bot->IsNonMeleeSpellCasted(true, false, true))
        return FSM_DELEGATE;                                   // mid-cast -> never interrupt

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return FSM_DELEGATE;

    Unit* target = bot->GetVictim();
    if (!target || !sServerFacade.IsAlive(target))
        target = AI_VALUE(Unit*, "current target");
    if (!target || !sServerFacade.IsAlive(target))
        return FSM_DELEGATE;                                   // no live target -> engine acquires one

    const uint32 now = WorldTimer::getMSTime();
    CombatDirectorTelemetry(now);

    // Melee vs ranged: paladins count as melee even when IsRanged()'s holy heuristic says otherwise.
    const bool ranged = ai->IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
    bool inRange;
    if (ranged)
    {
        const float spellRange = ai->GetRange("spell");
        inRange = bot->IsWithinLOSInMap(target) &&
                  bot->GetDistance(target) <= spellRange;
    }
    else
        inRange = bot->CanReachWithMeleeAutoAttack(target) && bot->IsWithinLOSInMap(target);

    // In range + LoS -> the engine runs the class rotation (that part works well).
    if (inRange)
    {
        s_cdInRange.fetch_add(1, std::memory_order_relaxed);
        s_cdReached.fetch_add(1, std::memory_order_relaxed);
        return FSM_DELEGATE;
    }

    // OUT OF RANGE, healthy, can move -> STICK and CHASE. Keep "current target" consistent so the reach
    // action chases exactly this target.
    context->GetValue<Unit*>("current target")->Set(target);

    // STUCK-CHASE guard: if we've been chasing this same target but not getting closer for >4s, it's
    // effectively unreachable (fleeing bot, bad path, LoS wall) -> drop it so a new target is picked
    // next tick instead of chasing forever (the observed 15-20 min cross-faction chase-lock).
    static std::mutex cdMx;
    static std::unordered_map<uint32, CdTrack> cdTracks;
    const uint32 guid = bot->GetGUIDLow();
    const uint32 tgtLow = target->GetGUIDLow();
    const float dist = bot->GetDistance(target);
    bool giveUp = false;
    {
        std::lock_guard<std::mutex> lk(cdMx);
        CdTrack& c = cdTracks[guid];
        if (c.tgtLow != tgtLow) { c.tgtLow = tgtLow; c.lastDist = dist; c.lastProgressMs = now; }
        else if (dist < c.lastDist - 1.0f) { c.lastDist = dist; c.lastProgressMs = now; }  // progress
        else if (now - c.lastProgressMs > 4000) { giveUp = true; c.tgtLow = 0; }           // stuck
    }
    if (giveUp)
    {
        s_cdGaveUp.fetch_add(1, std::memory_order_relaxed);
        bot->AttackStop();
        context->GetValue<Unit*>("current target")->Set(nullptr);
        ai->SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);
        *executed = true; return FSM_HANDLED;   // re-acquire next tick (engine or director)
    }

    s_cdChase.fetch_add(1, std::memory_order_relaxed);
    const bool alreadyChasing =
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;
    bool acted = true;
    if (!alreadyChasing)
        acted = ai->DoSpecificAction(ranged ? "reach spell" : "reach melee", Event(), true);
    ai->SetAIInternalUpdateDelay(alreadyChasing ? 300 : (sPlayerbotAIConfig.reactDelay + 100));
    if (acted)
        { *executed = true; return FSM_HANDLED; }
    return FSM_DELEGATE;   // reach failed (LoS/path) -> let the engine try something else
}


void PlayerbotAI::DoNextAction(bool min)
{
    SC_PHASE("DoNextAction.entry", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-next-entry", bot ? bot->GetGUIDLow() : 0);
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || (GetMaster() && GetMaster()->IsBeingTeleported()))
    {
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        return;
    }

    // if in combat but stuck with old data - clear targets
    SC_PHASE("DoNextAction.staleTargetCheck", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-stale-check", bot ? bot->GetGUIDLow() : 0);
    if (currentEngine == engines[(uint8)BotState::BOT_STATE_NON_COMBAT] && sServerFacade.IsInCombat(bot))
    {
        if (aiObjectContext->GetValue<Unit*>("current target")->Get() != NULL ||
            aiObjectContext->GetValue<ObjectGuid>("attack target")->Get() != ObjectGuid() ||
            aiObjectContext->GetValue<Unit*>("dps target")->Get() != NULL)
        {
            Reset();
        }
    }

    bool minimal = !AllowActivity();

    SC_PHASE("DoNextAction.staleTargetRecover", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-stale-recover", bot ? bot->GetGUIDLow() : 0);
    bool staleReset = DetectAndClearStaleTarget();

    const bool botDead = sServerFacade.UnitIsDead(bot);
    if (!botDead)
    {
        deadFastLaneNextRetry = 0;
        deadFastLaneRetries = 0;
    }

    if (botDead && !bot->IsBeingTeleported())
    {
        const time_t now = time(0);
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
        const bool ghost = bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
        const bool wasGhost = ghost;
        bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        bool recovered = false;
        bool attemptedRecovery = false;

        // Dead bots should not carry live quest/combat/loot bookkeeping. That
        // stale work makes the health scorer report idle/no-work while the dead
        // fast lane is actually trying to release or corpse-run.
        aiObjectContext->GetValue<Unit*>("old target")->Reset();
        aiObjectContext->GetValue<Unit*>("current target")->Reset();
        aiObjectContext->GetValue<ObjectGuid>("pre-selected next target")->Reset();
        aiObjectContext->GetValue<Unit*>("pull target")->Reset();
        aiObjectContext->GetValue<Unit*>("dps target")->Reset();
        aiObjectContext->GetValue<Unit*>("enemy player target")->Reset();
        aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
        aiObjectContext->GetValue<LootObject>("loot target")->Reset();
        aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
        aiObjectContext->GetValue<bool>("travel target active")->Reset();
        if (TravelTarget* deadTravelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get())
        {
            deadTravelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            deadTravelTarget->SetForced(false);
        }
        bot->SetSelectionGuid(ObjectGuid());

        const bool retryDue = !deadFastLaneNextRetry || now >= deadFastLaneNextRetry || noProgressSecs >= 120;

        if (retryDue)
        {
            attemptedRecovery = true;
            if (!ghost)
                recovered = DoSpecificAction("auto release", Event(), true);
            else if (bot->GetCorpse())
            {
                FindCorpseAction findCorpse(this);
                Event event;
                recovered = findCorpse.Execute(event);
            }
            else
                recovered = DoSpecificAction("repop", Event(), true);

            moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool apparentRecoveryProgress =
                moving ||
                (!wasGhost && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)) ||
                !sServerFacade.UnitIsDead(bot) ||
                bot->IsBeingTeleported();

            if (apparentRecoveryProgress)
                deadFastLaneRetries = 0;
            else if (deadFastLaneRetries < 10)
                ++deadFastLaneRetries;

            const uint32 retryDelay = 1;
            deadFastLaneNextRetry = now + retryDelay;
        }

        if (!moving &&
            ghost &&
            bot->GetCorpse() &&
            noProgressSecs >= 120)
        {
            WorldPosition corpsePos(bot->GetCorpse());
            if (corpsePos && !HasPlayerNearby(corpsePos))
                recovered = TeleportGhostUnstuck(this, bot, corpsePos, "DeadFastLaneCorpseTeleport", CORPSE_RECLAIM_RADIUS > 5.0f ? CORPSE_RECLAIM_RADIUS - 2.0f : 3.0f);
        }

        moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        const bool deadAfterRecovery = sServerFacade.UnitIsDead(bot);
        const bool ghostAfterRecovery = bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
        const bool releasedThisTick = !wasGhost && ghostAfterRecovery;
        const bool revivedThisTick = !deadAfterRecovery;
        const bool teleportedThisTick = bot->IsBeingTeleported();
        const bool madeRecoveryProgress = moving || releasedThisTick || revivedThisTick || teleportedThisTick;

        if (madeRecoveryProgress)
        {
            actionMetricLastProgress = now;
            actionMetricNoProgressTicks = 0;
            actionMetricSameActionStreak = 0;
        }

        if (sPlayerbotAIConfig.hasLog("bot_events.csv") &&
            attemptedRecovery &&
            ((recovered && urand(1, 20) == 1) ||
             (recovered && !madeRecoveryProgress && noProgressSecs >= 10 && (now - actionMetricLastStallSnapshot) >= 10) ||
             (!recovered && !moving && noProgressSecs >= 60 && (now - actionMetricLastStallSnapshot) >= 15)))
        {
            std::ostringstream out;
            out << "ghost=" << (ghost ? 1 : 0)
                << " corpse=" << (bot->GetCorpse() ? 1 : 0)
                << " recovered=" << (recovered ? 1 : 0)
                << " progress=" << (madeRecoveryProgress ? 1 : 0)
                << " released=" << (releasedThisTick ? 1 : 0)
                << " revived=" << (revivedThisTick ? 1 : 0)
                << " teleported=" << (teleportedThisTick ? 1 : 0)
                << " moving=" << (moving ? 1 : 0)
                << " noProgressSec=" << noProgressSecs
                << " retryDelaySec=" << (deadFastLaneNextRetry > now ? static_cast<uint32>(deadFastLaneNextRetry - now) : 0)
                << " retries=" << deadFastLaneRetries
                << " action=" << (ghost ? (bot->GetCorpse() ? "find corpse" : "repop") : "auto release");
            sPlayerbotAIConfig.logEvent(this, "DeadFastLaneRecovery", "", out.str());
            if ((!recovered && !moving) || (recovered && !madeRecoveryProgress))
                actionMetricLastStallSnapshot = now;
        }

        if (madeRecoveryProgress)
        {
            TrackActionMetrics(false, true, staleReset);
            return;
        }

        // Dead bots must not fall through into normal live engines. If corpse
        // recovery could not make progress this tick, wait for the next dead
        // fast-lane retry instead of creating quest/combat/travel work as a ghost.
        TrackActionMetrics(false, false, staleReset);
        return;
    }

    // ============================ CALCULATED MODE ============================
    // The legacy fast-lane "rescue" cascades below (~12 blocks) each ran a private
    // copy of a "re-pick the bot's work" sequence and then return;ed BEFORE the
    // normal strategy engine ran. That meant any of them could REPLACE the bot's
    // own decision and interrupt it mid-task on any tick its stall counters tripped
    // - the "hidden drivers" behind jumpy / out-of-order behavior. They are gated
    // OFF here so living bots ALWAYS fall through to currentEngine->DoNextAction()
    // and behave by their strategy relevances alone. Flip to true to restore.
    const bool kEnableLegacyFastLanes = false;
    if (kEnableLegacyFastLanes)
    {
    if (!minimal &&
        actionMetricLastProgress &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        currentState != BotState::BOT_STATE_DEAD &&
        !bot->IsTaxiFlying() &&
        !bot->IsBeingTeleported() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        !sServerFacade.isMoving(bot) &&
        !bot->isMovingOrTurning() &&
        !sServerFacade.IsInCombat(bot) &&
        !bot->GetVictim())
    {
        AiObjectContext* context = aiObjectContext;
        const time_t now = time(0);
        const uint32 noProgressSecs = static_cast<uint32>(now - actionMetricLastProgress);
        Unit* staleTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        LootObject staleLoot = aiObjectContext->GetValue<LootObject>("loot target")->Get();
        const bool hasStaleLoot = !staleLoot.IsEmpty();
        const bool hasStaleTarget =
            staleTarget &&
            staleTarget->IsInWorld() &&
            staleTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, staleTarget);
        const bool targetLootConflict =
            hasStaleLoot &&
            hasStaleTarget &&
            staleLoot.guid == staleTarget->GetObjectGuid();
        const bool staleTargetNoMotion =
            hasStaleTarget &&
            noProgressSecs >= 8 &&
            (actionMetricNoProgressTicks >= 10 || actionMetricSameActionStreak >= 10);
        const bool staleLootNoMotion =
            hasStaleLoot &&
            noProgressSecs >= 8 &&
            (actionMetricNoProgressTicks >= 10 || actionMetricSameActionStreak >= 10);

        if ((targetLootConflict && noProgressSecs >= 5) || staleTargetNoMotion || staleLootNoMotion)
        {
            ObjectGuid targetGuid;
            uint64 lootGuid = 0;
            std::string targetName = "none";

            if (hasStaleLoot)
            {
                lootGuid = staleLoot.guid.GetCounter();
                DeferLootGuidForBot(bot, staleLoot.guid, 10);
                if (LootObjectStack* availableLoot = aiObjectContext->GetValue<LootObjectStack*>("available loot")->Get())
                    availableLoot->Remove(staleLoot.guid);
                aiObjectContext->GetValue<LootObject>("loot target")->Reset();
            }

            if (hasStaleTarget)
            {
                targetGuid = staleTarget->GetObjectGuid();
                targetName = staleTarget->GetName();
                RESET_AI_VALUE(bool, "has attackers");
                RESET_AI_VALUE(Unit*, "old target");
                RESET_AI_VALUE(Unit*, "current target");
                RESET_AI_VALUE(Unit*, "pull target");
                RESET_AI_VALUE(Unit*, "dps target");
                RESET_AI_VALUE(ObjectGuid, "attack target");
                bot->SetSelectionGuid(ObjectGuid());
                bot->CombatStop(false);
                OnCombatEnded();
                ResetStaleTargetState();
            }

            RESET_AI_VALUE(GuidPosition, "rpg target");

            TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
            bool questRequested = DoSpecificAction("request quest travel target", Event(), true);
            bool travelChosen = questRequested ? false : DoSpecificAction("choose travel target", Event(), true);
            bool targetSelected = (questRequested || travelChosen) ? false : DoSpecificAction("select new target", Event(), true);
            bool attackAnything = (questRequested || travelChosen || targetSelected) ? false : DoSpecificAction("attack anything", Event(), true);
            bool grindRequested = (questRequested || travelChosen || targetSelected || attackAnything) ? false :
                RequestGrindTravelFallback(this, aiObjectContext, bot, "stale-loot-target-conflict");
            bool travelMoved = (questRequested || travelChosen || grindRequested) ?
                MoveAssignedTravelWork(this, aiObjectContext, bot, "stale-loot-target-conflict") : false;

            Unit* newTarget = AI_VALUE(Unit*, "current target");
            LootObject newLoot = AI_VALUE(LootObject, "loot target");
            GuidPosition newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
            bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            bool directedMovementWork =
                HasDirectedMovementWork(travelTarget, newRpgTarget, travelMoved, false, movingAfter);
            bool workAssigned =
                HasUsefulAssignedWork(bot, travelTarget, newTarget, newLoot, movingAfter, directedMovementWork);
            bool rescuePushed = false;

            if (!workAssigned)
            {
                bool rescueQuestRequested = false;
                bool rescueTravelChosen = false;
                bool rescueTargetSelected = false;
                bool rescueAttackAnything = false;
                bool rescueGrindRequested = false;
                bool rescueTravelMoved = false;
                bool rescueRpgChosen = false;
                bool rescueRpgMoved = false;
                bool rescueRandomMoved = false;
                bool rescueIdleNudged = false;

                rescuePushed = PushVisibleWork(this, aiObjectContext, bot, "stale-loot-target-fast-clear",
                    rescueQuestRequested, rescueTravelChosen, rescueTargetSelected, rescueAttackAnything,
                    rescueGrindRequested, rescueTravelMoved, rescueRpgChosen, rescueRpgMoved,
                    rescueRandomMoved, rescueIdleNudged, true);

                questRequested = questRequested || rescueQuestRequested;
                travelChosen = travelChosen || rescueTravelChosen;
                targetSelected = targetSelected || rescueTargetSelected;
                attackAnything = attackAnything || rescueAttackAnything;
                grindRequested = grindRequested || rescueGrindRequested;
                travelMoved = travelMoved || rescueTravelMoved;

                newTarget = AI_VALUE(Unit*, "current target");
                newLoot = AI_VALUE(LootObject, "loot target");
                newRpgTarget = AI_VALUE(GuidPosition, "rpg target");
                travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                directedMovementWork =
                    HasDirectedMovementWork(travelTarget, newRpgTarget, travelMoved || rescueTravelMoved,
                        rescueRpgMoved, movingAfter);
                workAssigned = rescuePushed ||
                    HasConcreteRecoveryWork(bot, travelTarget, newTarget, newLoot, movingAfter,
                        travelMoved || rescueTravelMoved, false);
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "reason=" << (targetLootConflict ? "target-loot-conflict" : (hasStaleTarget ? "target-no-motion" : "loot-no-motion"))
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " target=" << targetName
                    << " targetGuid=" << targetGuid.GetCounter()
                    << " lootGuid=" << lootGuid
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " selectTarget=" << (targetSelected ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " rescuePushed=" << (rescuePushed ? 1 : 0)
                    << " movingAfter=" << (movingAfter ? 1 : 0)
                    << " newTarget=" << (newTarget ? newTarget->GetName() : "none")
                    << " newLoot=" << (!newLoot.IsEmpty() ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "StaleLootTargetFastClear", "", out.str());
            }

            if (workAssigned)
                actionMetricLastProgress = now;
            TrackActionMetrics(false, workAssigned, true);
            return;
        }
    }

    if (currentState == BotState::BOT_STATE_COMBAT &&
        !botDead &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        !aiObjectContext->GetValue<Unit*>("current target")->Get() &&
        !aiObjectContext->GetValue<Unit*>("dps target")->Get() &&
        aiObjectContext->GetValue<LootObject>("loot target")->Get().IsEmpty())
    {
        const time_t now = time(0);
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
        const bool passiveAction =
            actionMetricLastActionName.empty() ||
            actionMetricLastActionName == "none" ||
            actionMetricLastActionName == "check values" ||
            actionMetricLastActionName == "update pve strats" ||
            actionMetricLastActionName == "emote" ||
            actionMetricLastActionName == "cannibalize" ||
            actionMetricLastActionName == "reset raids" ||
            actionMetricLastActionName == "move random" ||
            actionMetricLastActionName == "move to rpg target" ||
            actionMetricLastActionName == "choose rpg target" ||
            actionMetricLastActionName == "accept invitation" ||
            actionMetricLastActionName == "select new target" ||
            actionMetricLastActionName == "move to travel target" ||
            actionMetricLastActionName == "request quest travel target";

        if (passiveAction && (noProgressSecs >= 3 || actionMetricNoProgressTicks >= 3 || actionMetricSameActionStreak >= 20))
        {
            ObjectGuid attackTarget = aiObjectContext->GetValue<ObjectGuid>("attack target")->Get();
            TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();

            aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
            aiObjectContext->GetValue<Unit*>("old target")->Reset();
            aiObjectContext->GetValue<Unit*>("pull target")->Reset();
            bot->CombatStop(false);
            OnCombatEnded();

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " moving=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
                    << " attackTarget=" << (attackTarget ? 1 : 0)
                    << " travelStatus=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
                    << " travelPtr=" << (travelTarget && travelTarget->GetDestination() ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "CombatStateNoWorkExit", "", out.str());
            }
        }
    }

    if (!minimal &&
        currentState == BotState::BOT_STATE_COMBAT &&
        !botDead &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        sServerFacade.IsInCombat(bot) &&
        !aiObjectContext->GetValue<Unit*>("current target")->Get() &&
        !aiObjectContext->GetValue<Unit*>("dps target")->Get() &&
        !bot->GetVictim() &&
        aiObjectContext->GetValue<LootObject>("loot target")->Get().IsEmpty())
    {
        const time_t now = time(0);
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
        const bool staleNoTargetCombat =
            noProgressSecs >= 8 ||
            actionMetricNoProgressTicks >= 80 ||
            actionMetricSameActionStreak >= 80;

        if (staleNoTargetCombat)
        {
            const bool hadAttackers = aiObjectContext->GetValue<bool>("has attackers")->Get();
            Unit* newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
            bool exitedCombat = true;
            bool workAssigned = false;
            bool questRequested = false;
            bool travelChosen = false;
            bool targetSelected = false;
            bool attackAnything = false;
            bool grindRequested = false;
            bool travelMoved = false;
            bool rpgChosen = false;
            bool rpgMoved = false;
            bool randomMoved = false;
            bool idleNudged = false;

            aiObjectContext->GetValue<bool>("has attackers")->Reset();
            aiObjectContext->GetValue<Unit*>("old target")->Reset();
            aiObjectContext->GetValue<Unit*>("current target")->Reset();
            aiObjectContext->GetValue<ObjectGuid>("pre-selected next target")->Reset();
            aiObjectContext->GetValue<Unit*>("pull target")->Reset();
            aiObjectContext->GetValue<Unit*>("dps target")->Reset();
            aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
            aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
            ResetTargetScanCaches(aiObjectContext);
            bot->AttackStop(true);
            bot->SetSelectionGuid(ObjectGuid());
            bot->SetTargetGuid(ObjectGuid());
            bot->CombatStop(true);
            OnCombatEnded();
            ResetStaleTargetState();

            // This path means combat has already been stale with no usable target.
            // Do not immediately scan/reacquire local mobs on the same tick; make
            // the bot resume quest/travel/grind work first and let normal combat
            // reacquire attackers on the next active tick if something is truly hitting it.
            PushVisibleWork(this, aiObjectContext, bot, "combat-no-target-hard-recover",
                questRequested, travelChosen, targetSelected, attackAnything, grindRequested, travelMoved,
                rpgChosen, rpgMoved, randomMoved, idleNudged, true, false);
            newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();

            LootObject newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
            GuidPosition newRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
            TravelTarget* newTravel = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
            const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovementWork =
                HasDirectedMovementWork(newTravel, newRpgTarget, travelMoved, rpgMoved, movingAfter);
            const bool usefulWorkAssigned =
                HasConcreteRecoveryWork(bot, newTravel, newTarget, newLootTarget, movingAfter,
                    travelMoved, false);
            const bool fallbackWorkAssigned = HasFallbackMovementWork(randomMoved, idleNudged, movingAfter);
            const bool stillCombatNoTarget =
                sServerFacade.IsInCombat(bot) &&
                !newTarget &&
                !bot->GetVictim() &&
                !bot->IsNonMeleeSpellCasted(true, false, true);
            workAssigned = usefulWorkAssigned || directedMovementWork || fallbackWorkAssigned;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " attackers=" << (hadAttackers ? 1 : 0)
                    << " selected=" << (targetSelected ? 1 : 0)
                    << " newTarget=" << (newTarget ? newTarget->GetName() : "none")
                    << " exited=" << (exitedCombat ? 1 : 0)
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " rpgMove=" << (rpgMoved ? 1 : 0)
                    << " randomMove=" << (randomMoved ? 1 : 0)
                    << " idleNudge=" << (idleNudged ? 1 : 0)
                    << " movingAfter=" << (movingAfter ? 1 : 0)
                    << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                    << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                    << " directedWork=" << (directedMovementWork ? 1 : 0)
                    << " usefulWork=" << (usefulWorkAssigned ? 1 : 0)
                    << " fallbackWork=" << (fallbackWorkAssigned ? 1 : 0)
                    << " stillCombatNoTarget=" << (stillCombatNoTarget ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "CombatNoTargetHardRecover", "", out.str());
            }

            if (workAssigned)
            {
                TrackActionMetrics(false, true, staleReset || exitedCombat);
                return;
            }

            if ((movingAfter || newRpgTarget) && !workAssigned)
            {
                StopMoving();
                bot->InterruptMoving(true);
                aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
                if (newTravel && !IsActiveTravelWork(newTravel) && !IsPreparedTravelWork(newTravel))
                {
                    newTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    newTravel->SetForced(false);
                    aiObjectContext->GetValue<bool>("travel target active")->Reset();
                }
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !botDead &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        !sServerFacade.IsInCombat(bot) &&
        !bot->GetVictim() &&
        !sServerFacade.isMoving(bot) &&
        !bot->isMovingOrTurning())
    {
        Unit* stalledTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        if (stalledTarget &&
            stalledTarget->IsInWorld() &&
            stalledTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, stalledTarget) &&
            !sServerFacade.UnitIsDead(stalledTarget))
        {
            const time_t now = time(0);
            const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
            const bool staleTargetState =
                noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS &&
                (actionMetricNoProgressTicks >= 20 || actionMetricSameActionStreak >= 15 || noProgressSecs >= 20);

            if (staleTargetState)
            {
                const ObjectGuid targetGuid = stalledTarget->GetObjectGuid();
                const std::string targetName = stalledTarget->GetName();
                TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();

                aiObjectContext->GetValue<bool>("has attackers")->Reset();
                aiObjectContext->GetValue<Unit*>("old target")->Reset();
                aiObjectContext->GetValue<Unit*>("current target")->Reset();
                aiObjectContext->GetValue<ObjectGuid>("pre-selected next target")->Reset();
                aiObjectContext->GetValue<Unit*>("pull target")->Reset();
                aiObjectContext->GetValue<Unit*>("dps target")->Reset();
                aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
                bot->SetSelectionGuid(ObjectGuid());
                bot->CombatStop(false);
                OnCombatEnded();
                ResetStaleTargetState();

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                        << " target=" << targetName
                        << " guid=" << targetGuid.GetCounter()
                        << " dist=" << std::fixed << std::setprecision(2) << bot->GetDistance(stalledTarget)
                        << " noProgressSec=" << noProgressSecs
                        << " noProgressTicks=" << actionMetricNoProgressTicks
                        << " sameActionStreak=" << actionMetricSameActionStreak
                        << " travelStatus=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
                        << " travelPtr=" << (travelTarget && travelTarget->GetDestination() ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "CombatTargetNoProgressReset", std::to_string(targetGuid.GetCounter()), out.str());
                }

                TrackActionMetrics(false, true, true);
                return;
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !botDead &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        !bot->IsTaxiFlying() &&
        !bot->IsBeingTeleported() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        !sServerFacade.IsInCombat(bot) &&
        !sServerFacade.isMoving(bot) &&
        !bot->isMovingOrTurning() &&
        !aiObjectContext->GetValue<Unit*>("current target")->Get() &&
        !aiObjectContext->GetValue<Unit*>("dps target")->Get() &&
        !bot->GetVictim() &&
        aiObjectContext->GetValue<LootObject>("loot target")->Get().IsEmpty())
    {
        const time_t now = time(0);
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
        const bool staleActionWithoutWork =
            (actionMetricLastActionName.empty() || actionMetricLastActionName == "none") ?
                (noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS && actionMetricNoProgressTicks >= 20) :
                (noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS &&
                 (actionMetricSameActionStreak >= 15 || actionMetricNoProgressTicks >= 6 || noProgressSecs >= 20));

        if (staleActionWithoutWork)
        {
            TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
            TravelDestination* destination = travelTarget ? travelTarget->GetDestination() : nullptr;
            const TravelStatus oldStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;

            aiObjectContext->GetValue<bool>("has attackers")->Reset();
            aiObjectContext->GetValue<Unit*>("old target")->Reset();
            aiObjectContext->GetValue<Unit*>("pull target")->Reset();
            aiObjectContext->GetValue<Unit*>("dps target")->Reset();
            aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
            bot->SetSelectionGuid(ObjectGuid());
            ResetStaleTargetState();

            if (travelTarget)
            {
                travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                travelTarget->SetForced(false);
                aiObjectContext->GetValue<bool>("travel target active")->Reset();
                aiObjectContext->ClearValues("no active travel destinations");
            }

            aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();

            bool questRequested = false;
            bool travelChosen = false;
            bool targetSelected = false;
            bool attackAnything = false;
            bool grindRequested = false;
            bool travelMoved = false;
            bool rpgChosen = false;
            bool rpgMoved = false;
            bool randomMoved = false;
            bool idleNudged = false;

            PushVisibleWork(this, aiObjectContext, bot, "stale-action-no-work",
                questRequested, travelChosen, targetSelected, attackAnything, grindRequested, travelMoved,
                rpgChosen, rpgMoved, randomMoved, idleNudged, true);
            Unit* newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
            const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            LootObject newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
            GuidPosition newRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
            const bool directedMovementWork =
                HasDirectedMovementWork(travelTarget, newRpgTarget, travelMoved, rpgMoved, movingAfter);
            const bool fallbackMovementWork =
                HasFallbackMovementWork(randomMoved, idleNudged, movingAfter);
            const bool workAssigned =
                HasConcreteRecoveryWork(bot, travelTarget, newTarget, newLootTarget, movingAfter, travelMoved, false) ||
                directedMovementWork ||
                fallbackMovementWork;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "action=" << actionMetricLastActionName
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " oldStatus=" << static_cast<uint32>(oldStatus)
                    << " newStatus=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " selectTarget=" << (targetSelected ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " rpgChoose=" << (rpgChosen ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " rpgMove=" << (rpgMoved ? 1 : 0)
                    << " randomMove=" << (randomMoved ? 1 : 0)
                    << " idleNudge=" << (idleNudged ? 1 : 0)
                    << " nudgeOnly=" << ((randomMoved || idleNudged) && !workAssigned ? 1 : 0)
                    << " directedMove=" << (directedMovementWork ? 1 : 0)
                    << " fallbackMove=" << (fallbackMovementWork ? 1 : 0)
                    << " movingAfter=" << (movingAfter ? 1 : 0)
                    << " target=" << (newTarget ? newTarget->GetName() : "none")
                    << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                    << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0)
                    << " destinationPtr=" << (destination ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "NoWorkActionReset", "", out.str());
            }

            if (workAssigned)
            {
                TrackActionMetrics(false, true, true);
                return;
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !botDead &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
        if (travelTarget && travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
        {
            const time_t now = time(0);
            Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
            LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
            TravelDestination* destination = travelTarget->GetDestination();
            const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool combat = sServerFacade.IsInCombat(bot);
            const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
            const bool shouldClear =
                !moving &&
                ((!lootTarget.IsEmpty() && (combat || currentTarget || noProgressSecs >= 3 || actionMetricSameActionStreak >= 20)) ||
                 noProgressSecs >= 3 ||
                 actionMetricNoProgressTicks >= 30 ||
                 actionMetricSameActionStreak >= 20);

            if (shouldClear)
            {
                const uint64 lootGuid = lootTarget.IsEmpty() ? 0 : lootTarget.guid.GetCounter();
                if (!lootTarget.IsEmpty())
                {
                    DeferLootGuidForBot(bot, lootTarget.guid, 15);
                    if (LootObjectStack* availableLoot = aiObjectContext->GetValue<LootObjectStack*>("available loot")->Get())
                        availableLoot->Remove(lootTarget.guid);
                    aiObjectContext->GetValue<LootObject>("loot target")->Reset();
                }

                travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                travelTarget->SetForced(false);
                aiObjectContext->GetValue<bool>("travel target active")->Reset();
                aiObjectContext->ClearValues("no active travel destinations");
                aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();

                if (!currentTarget)
                    aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "destinationPtr=" << (destination ? 1 : 0)
                        << " noProgressSec=" << noProgressSecs
                        << " sameActionStreak=" << actionMetricSameActionStreak
                        << " combat=" << (combat ? 1 : 0)
                        << " target=" << (currentTarget ? currentTarget->GetName() : "none")
                        << " lootGuid=" << lootGuid
                        << " action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName);
                    sPlayerbotAIConfig.logEvent(this, "ExpiredTravelStaleClear", "", out.str());
                }

                staleReset = true;
            }
        }
    }

    if (currentState != BotState::BOT_STATE_DEAD &&
        !botDead &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        Unit* loopTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        if (loopTarget &&
            loopTarget->IsInWorld() &&
            loopTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, loopTarget) &&
            !sServerFacade.UnitIsDead(loopTarget))
        {
            const time_t now = time(0);
            const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
            const bool loopAction =
                actionMetricLastActionName == "reach melee" ||
                actionMetricLastActionName == "reach spell" ||
                actionMetricLastActionName == "-ranged,+close" ||
                actionMetricLastActionName == "melee" ||
                actionMetricLastActionName == "battle shout" ||
                actionMetricLastActionName == "renew on party" ||
                actionMetricLastActionName == "clean quest log" ||
                actionMetricLastActionName == "add all loot" ||
                actionMetricLastActionName == "store loot" ||
                actionMetricLastActionName == "loot" ||
                actionMetricLastActionName == "move to loot" ||
                actionMetricLastActionName == "move random" ||
                actionMetricLastActionName == "move out of enemy contact" ||
                actionMetricLastActionName == "move to rpg target" ||
                actionMetricLastActionName == "choose rpg target" ||
                actionMetricLastActionName == "choose travel target" ||
                actionMetricLastActionName == "choose group travel target" ||
                actionMetricLastActionName == "invite nearby" ||
                actionMetricLastActionName == "emote" ||
                actionMetricLastActionName == "hunter's mark" ||
                actionMetricLastActionName == "select new target" ||
                actionMetricLastActionName == "request quest travel target" ||
                actionMetricLastActionName == "update pve strats";
            const bool passiveCombatLoopAction =
                actionMetricLastActionName == "check values" ||
                actionMetricLastActionName == "update pve strats" ||
                actionMetricLastActionName == "move to rpg target" ||
                actionMetricLastActionName == "choose rpg target" ||
                actionMetricLastActionName == "choose travel target" ||
                actionMetricLastActionName == "choose group travel target" ||
                actionMetricLastActionName == "invite nearby" ||
                actionMetricLastActionName == "select new target";
            const bool targetFillerLoopAction =
                passiveCombatLoopAction ||
                actionMetricLastActionName == "add all loot" ||
                actionMetricLastActionName == "store loot" ||
                actionMetricLastActionName == "loot" ||
                actionMetricLastActionName == "move to loot" ||
                actionMetricLastActionName == "clean quest log" ||
                actionMetricLastActionName == "emote" ||
                actionMetricLastActionName == "jump" ||
                actionMetricLastActionName == "drink" ||
                actionMetricLastActionName == "food" ||
                actionMetricLastActionName == "move random" ||
                actionMetricLastActionName == "request named travel target" ||
                actionMetricLastActionName == "rpg work" ||
                actionMetricLastActionName == "rpg use" ||
                actionMetricLastActionName == "rpg stay" ||
                actionMetricLastActionName == "rpg emote" ||
                actionMetricLastActionName == "rpg sell" ||
                actionMetricLastActionName == "rpg cancel";
            const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool casting = bot->IsNonMeleeSpellCasted(true, false, true);
            const bool victimTarget = bot->GetVictim() == loopTarget;
            const bool victimInReach =
                victimTarget &&
                (bot->CanReachWithMeleeAutoAttack(loopTarget) ||
                 (IsRanged(bot) && bot->IsWithinLOSInMap(loopTarget, true) &&
                  sServerFacade.GetDistance2d(bot, loopTarget) <= GetRange("spell") + sPlayerbotAIConfig.contactDistance));
            const bool genericTargetLoopAction = !loopAction && !passiveCombatLoopAction;
            const ObjectGuid loopTargetGuid = loopTarget->GetObjectGuid();
            const bool fillerRetryDue =
                targetFastLaneGuid != loopTargetGuid ||
                !targetFastLaneNextRetry ||
                now >= targetFastLaneNextRetry ||
                noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS;
            const bool fillerNeedsCombatPrime =
                targetFillerLoopAction &&
                fillerRetryDue &&
                !casting &&
                (!victimInReach ||
                 noProgressSecs >= 8 ||
                 actionMetricNoProgressTicks >= 20 ||
                 actionMetricSameActionStreak >= 30);

            if (fillerNeedsCombatPrime)
            {
                TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                const TravelStatus oldTravelStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
                bool travelCleared = false;

                aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
                aiObjectContext->GetValue<LootObject>("loot target")->Reset();

                if (travelTarget && !victimInReach)
                {
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    travelTarget->SetForced(false);
                    aiObjectContext->GetValue<bool>("travel target active")->Reset();
                    aiObjectContext->ClearValues("no active travel destinations");
                    travelCleared = true;
                }

                bot->SetSelectionGuid(loopTarget->GetObjectGuid());
                bot->SetTarget(loopTarget);

                const bool forcedAttack = DoSpecificAction("attack", Event("target filler fast lane", "combat owns tick", bot), true);
                bool directProgress = false;
                if (!forcedAttack && !bot->IsNonMeleeSpellCasted(true, false, true))
                {
                    const bool rangedBot = IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
                    const bool inLos = bot->IsWithinLOSInMap(loopTarget, true);
                    const float distance = sServerFacade.GetDistance2d(bot, loopTarget);
                    const float spellRange = GetRange("spell");

                    if (!rangedBot || !inLos || distance > spellRange + sPlayerbotAIConfig.contactDistance)
                    {
                        bot->Attack(loopTarget, true);
                        if (MotionMaster* mm = bot->GetMotionMaster())
                        {
                            // Ranged bots close only to keep-away (flee) distance,
                            // not melee reach -- see the current-target fast lane.
                            if (rangedBot)
                                mm->MoveChase(loopTarget, GetRange("flee"), bot->GetAngle(loopTarget));
                            else
                                mm->MoveChase(loopTarget);
                            directProgress = true;
                        }
                    }
                    else
                    {
                        bot->Attack(loopTarget, false);
                        // Ranged attack selection is only a setup step. Do not
                        // call this progress unless a spell/combat state starts.
                        aiObjectContext->GetValue<Unit*>("dps target")->Set(loopTarget);
                        directProgress = TryImmediateRangedCombatAction(this, bot, loopTarget) ||
                            bot->IsNonMeleeSpellCasted(true, false, true);
                    }
                    OnCombatStarted();
                }

                const bool nowMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool nowCasting = bot->IsNonMeleeSpellCasted(true, false, true);
                const bool nowCombat = sServerFacade.IsInCombat(bot);
                const bool nowRangedBot = IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
                const bool nowVictimInReach =
                    bot->GetVictim() == loopTarget &&
                    (bot->CanReachWithMeleeAutoAttack(loopTarget) ||
                     (nowRangedBot && bot->IsWithinLOSInMap(loopTarget, true) &&
                      sServerFacade.GetDistance2d(bot, loopTarget) <= GetRange("spell") + sPlayerbotAIConfig.contactDistance));
                const bool passiveRangedVictimOnly = nowRangedBot && nowVictimInReach && !nowCombat && !nowCasting && !nowMoving && !directProgress;
                const bool usefulMovingProgress =
                    nowMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
                const bool countableMovingProgress =
                    usefulMovingProgress &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !nowVictimInReach);
                const bool meleeSwinging =
                    bot->GetVictim() == loopTarget &&
                    bot->CanReachWithMeleeAutoAttack(loopTarget) &&
                    bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                const bool meleeMovingWithoutContact =
                    !nowRangedBot &&
                    nowMoving &&
                    bot->GetVictim() == loopTarget &&
                    !bot->CanReachWithMeleeAutoAttack(loopTarget);
                bool combatConnectKick = false;
                if (!nowCasting &&
                    !meleeSwinging &&
                    (!usefulMovingProgress || nowVictimInReach || meleeMovingWithoutContact) &&
                    noProgressSecs >= VISIBLE_IDLE_WARN_SECONDS)
                {
                    combatConnectKick = ForceCombatConnect(this, bot, loopTarget, "target-filler-stall");
                }

                const bool postKickMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool postKickCasting = bot->IsNonMeleeSpellCasted(true, false, true);
                const bool postKickMeleeSwinging =
                    bot->GetVictim() == loopTarget &&
                    bot->CanReachWithMeleeAutoAttack(loopTarget) &&
                    bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                const bool postKickUsefulMoving =
                    postKickMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
                const bool postKickCountableMoving =
                    postKickUsefulMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !nowVictimInReach);
                const bool usefulDirectProgress =
                    directProgress &&
                    (postKickCasting || postKickMeleeSwinging || postKickCountableMoving);
                const bool freshVictimSetup =
                    nowVictimInReach &&
                    !passiveRangedVictimOnly &&
                    (postKickCasting || postKickMeleeSwinging || postKickCountableMoving) &&
                    noProgressSecs < 2;
                const bool primedCombat =
                    usefulDirectProgress ||
                    postKickCountableMoving ||
                    postKickCasting ||
                    postKickMeleeSwinging ||
                    (combatConnectKick && (postKickCountableMoving || postKickCasting || postKickMeleeSwinging)) ||
                    freshVictimSetup;
                targetFastLaneGuid = loopTargetGuid;
                if (primedCombat)
                {
                    targetFastLaneFailures = 0;
                    targetFastLaneNextRetry = now + (nowVictimInReach ? 3 : 1);
                }
                else
                {
                    ++targetFastLaneFailures;
                    targetFastLaneNextRetry = now + ((!postKickMoving && !postKickCasting && !postKickMeleeSwinging) ? 1 :
                        std::min<uint32>(5, 1 + targetFastLaneFailures));
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") &&
                    (!primedCombat || urand(1, 50) == 1))
                {
                    std::ostringstream out;
                    out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                        << " target=" << loopTarget->GetName()
                        << " guid=" << loopTarget->GetGUIDLow()
                        << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, loopTarget)
                        << " noProgressSec=" << noProgressSecs
                        << " noProgressTicks=" << actionMetricNoProgressTicks
                        << " sameActionStreak=" << actionMetricSameActionStreak
                        << " forcedAttack=" << (forcedAttack ? 1 : 0)
                        << " direct=" << (directProgress ? 1 : 0)
                        << " usefulDirect=" << (usefulDirectProgress ? 1 : 0)
                        << " moving=" << (postKickMoving ? 1 : 0)
                        << " usefulMoving=" << (postKickUsefulMoving ? 1 : 0)
                        << " countableMoving=" << (postKickCountableMoving ? 1 : 0)
                        << " casting=" << (postKickCasting ? 1 : 0)
                        << " combat=" << (nowCombat ? 1 : 0)
                        << " victimReach=" << (nowVictimInReach ? 1 : 0)
                        << " meleeSwing=" << (postKickMeleeSwinging ? 1 : 0)
                        << " connectKick=" << (combatConnectKick ? 1 : 0)
                        << " freshVictimSetup=" << (freshVictimSetup ? 1 : 0)
                        << " passiveRangedVictimOnly=" << (passiveRangedVictimOnly ? 1 : 0)
                        << " travelCleared=" << (travelCleared ? 1 : 0)
                        << " oldTravelStatus=" << static_cast<uint32>(oldTravelStatus)
                        << " nextRetrySec=" << (targetFastLaneNextRetry > now ? static_cast<uint32>(targetFastLaneNextRetry - now) : 0)
                        << " failures=" << targetFastLaneFailures
                        << " success=" << (primedCombat ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "TargetFillerCombatPrime", std::to_string(loopTarget->GetGUIDLow()), out.str());
                }

                if (primedCombat)
                {
                    TrackActionMetrics(false, true, staleReset);
                    return;
                }
            }

            const bool frozenTargetFillerLoop =
                targetFillerLoopAction &&
                !moving &&
                !casting &&
                !victimTarget &&
                targetFastLaneGuid == loopTargetGuid &&
                targetFastLaneFailures >= 2 &&
                noProgressSecs >= 6;
            const bool loopMeleeSwinging =
                bot->GetVictim() == loopTarget &&
                bot->CanReachWithMeleeAutoAttack(loopTarget) &&
                bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
            const bool loopUsefulMoving =
                moving &&
                (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
            const bool loopCountableMoving =
                loopUsefulMoving &&
                (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !victimTarget ||
                 !bot->CanReachWithMeleeAutoAttack(loopTarget));
            const bool fakeMovingTargetFillerLoop =
                targetFillerLoopAction &&
                moving &&
                !casting &&
                !loopMeleeSwinging &&
                !loopCountableMoving &&
                targetFastLaneGuid == loopTargetGuid &&
                targetFastLaneFailures >= 3 &&
                noProgressSecs >= 7;
            const bool longFakeMovingTargetLoop =
                moving &&
                !casting &&
                !loopMeleeSwinging &&
                !loopCountableMoving &&
                noProgressSecs >= 30;
            const bool severeLoop =
                frozenTargetFillerLoop ||
                fakeMovingTargetFillerLoop ||
                longFakeMovingTargetLoop ||
                noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS &&
                ((targetFillerLoopAction && (actionMetricSameActionStreak >= 25 || actionMetricNoProgressTicks >= 40 || noProgressSecs >= 20)) ||
                 (loopAction && (actionMetricSameActionStreak >= 120 || actionMetricNoProgressTicks >= 200)) ||
                 (passiveCombatLoopAction && (actionMetricSameActionStreak >= 25 || actionMetricNoProgressTicks >= 40)) ||
                 (genericTargetLoopAction && !moving && (actionMetricSameActionStreak >= 160 || actionMetricNoProgressTicks >= 220)));

            if (severeLoop)
            {
                const ObjectGuid targetGuid = loopTarget->GetObjectGuid();
                const std::string targetName = loopTarget->GetName();
                const bool hasAttackers = aiObjectContext->GetValue<bool>("has attackers")->Get();
                const bool wasVictim = bot->GetVictim() == loopTarget;
                bool travelCleared = ClearExpiredTravelAfterStaleTargetReset();
                TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                const TravelStatus oldTravelStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;

                if (targetFillerLoopAction && !hasAttackers && !sServerFacade.IsInCombat(bot) && travelTarget)
                {
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    travelTarget->SetForced(false);
                    aiObjectContext->GetValue<bool>("travel target active")->Reset();
                    aiObjectContext->ClearValues("no active travel destinations");
                    travelCleared = true;
                }

                if (wasVictim)
                    bot->AttackStop(true);

                aiObjectContext->GetValue<Unit*>("old target")->Reset();
                aiObjectContext->GetValue<Unit*>("current target")->Reset();
                aiObjectContext->GetValue<ObjectGuid>("pre-selected next target")->Reset();
                aiObjectContext->GetValue<Unit*>("pull target")->Reset();
                aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
                aiObjectContext->GetValue<Unit*>("dps target")->Reset();
                bot->SetSelectionGuid(ObjectGuid());

                if (!hasAttackers)
                {
                    bot->CombatStop(false);
                    OnCombatEnded();
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                        << " target=" << targetName
                        << " guid=" << targetGuid.GetCounter()
                        << " dist=" << std::fixed << std::setprecision(2) << bot->GetDistance(loopTarget)
                        << " dist2d=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, loopTarget)
                        << " noProgressSec=" << noProgressSecs
                        << " noProgressTicks=" << actionMetricNoProgressTicks
                        << " sameActionStreak=" << actionMetricSameActionStreak
                        << " moving=" << (moving ? 1 : 0)
                        << " fakeMoving=" << ((fakeMovingTargetFillerLoop || longFakeMovingTargetLoop) ? 1 : 0)
                        << " combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0)
                        << " victim=" << (wasVictim ? 1 : 0)
                        << " attackers=" << (hasAttackers ? 1 : 0)
                        << " oldTravelStatus=" << static_cast<uint32>(oldTravelStatus)
                        << " travelStatus=" << (travelTarget ? static_cast<uint32>(travelTarget->GetStatus()) : 0)
                        << " filler=" << (targetFillerLoopAction ? 1 : 0)
                        << " expiredTravelReset=" << (travelCleared ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "TargetLoopHardReset", std::to_string(targetGuid.GetCounter()), out.str());
                }

                ResetStaleTargetState();
                staleReset = true;
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        if (currentTarget &&
            currentTarget->IsInWorld() &&
            currentTarget->GetMapId() == bot->GetMapId() &&
            !sServerFacade.IsFriendlyTo(bot, currentTarget) &&
            !sServerFacade.UnitIsDead(currentTarget))
        {
            const time_t now = time(0);
            const ObjectGuid targetGuid = currentTarget->GetObjectGuid();
            if (targetFastLaneGuid != targetGuid)
            {
                targetFastLaneGuid = targetGuid;
                targetFastLaneNextRetry = 0;
                targetFastLaneFailures = 0;
            }

            const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool casting = bot->IsNonMeleeSpellCasted(true, false, true);
            const bool rangedTargetBot = IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
            const bool victimTarget = bot->GetVictim() == currentTarget;
            const bool victimInReach =
                victimTarget &&
                (bot->CanReachWithMeleeAutoAttack(currentTarget) ||
                 (rangedTargetBot && bot->IsWithinLOSInMap(currentTarget, true) &&
                  sServerFacade.GetDistance2d(bot, currentTarget) <= GetRange("spell") + sPlayerbotAIConfig.contactDistance));
            const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
            const bool usefulMovingProgress =
                moving &&
                (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
            const bool countableMovingProgress =
                usefulMovingProgress &&
                (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !victimInReach);
            const bool meleeSwinging =
                victimTarget &&
                bot->CanReachWithMeleeAutoAttack(currentTarget) &&
                bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
            const bool freshVictimSetup =
                victimInReach &&
                (casting || meleeSwinging || countableMovingProgress) &&
                noProgressSecs < 2;
            const bool productive = countableMovingProgress || casting || meleeSwinging || freshVictimSetup;
            if (productive)
            {
                targetFastLaneNextRetry = 0;
                targetFastLaneFailures = 0;
            }

            const bool needsFastLane =
                noProgressSecs >= 3 ||
                actionMetricNoProgressTicks >= 10 ||
                actionMetricSameActionStreak >= 20;
            const bool retryDue = !targetFastLaneNextRetry || now >= targetFastLaneNextRetry || noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS;
            if (!productive && needsFastLane && retryDue)
            {
                const bool forcedAttack = DoSpecificAction("attack", Event(), true);
                const bool forcedMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool forcedCasting = bot->IsNonMeleeSpellCasted(true, false, true);
                const bool forcedVictimInReach =
                    bot->GetVictim() == currentTarget &&
                    (bot->CanReachWithMeleeAutoAttack(currentTarget) ||
                     (rangedTargetBot && bot->IsWithinLOSInMap(currentTarget, true) &&
                      sServerFacade.GetDistance2d(bot, currentTarget) <= GetRange("spell") + sPlayerbotAIConfig.contactDistance));
                const bool forcedUsefulMoving =
                    forcedMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
                const bool forcedCountableMoving =
                    forcedUsefulMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !forcedVictimInReach);
                const bool forcedMeleeSwinging =
                    bot->GetVictim() == currentTarget &&
                    bot->CanReachWithMeleeAutoAttack(currentTarget) &&
                    bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                const bool forcedFreshVictimSetup =
                    forcedVictimInReach &&
                    (forcedCasting || forcedMeleeSwinging || forcedCountableMoving) &&
                    noProgressSecs < 2;
                const bool forcedProgress =
                    forcedCountableMoving ||
                    forcedCasting ||
                    forcedMeleeSwinging ||
                    forcedFreshVictimSetup;
                bool directProgress = false;
                if (!forcedProgress &&
                    !sServerFacade.isMoving(bot) &&
                    !bot->isMovingOrTurning())
                {
                    const bool rangedBot = IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
                    const bool inLos = bot->IsWithinLOSInMap(currentTarget, true);
                    const float distance = sServerFacade.GetDistance2d(bot, currentTarget);
                    const float spellRange = GetRange("spell");

                    bot->SetSelectionGuid(currentTarget->GetObjectGuid());
                    bot->SetTarget(currentTarget);

                    // A ranged bot already within spell range + LOS must NOT be
                    // force-chased into melee here: MoveChase closes to melee
                    // reach, parking casters at 3-5yd where the engine's ranged
                    // repositioning then fights to back them out, interrupting
                    // every cast (the perpetual-cast / "fake running" loop). Let
                    // such bots fall through to the cast branch below. Mirrors the
                    // correct ordering in the target-filler fast lane (~7082).
                    const bool rangedInSpellRange =
                        rangedBot && inLos &&
                        distance <= spellRange + sPlayerbotAIConfig.contactDistance;

                    if (bot->GetVictim() == currentTarget &&
                        !bot->CanReachWithMeleeAutoAttack(currentTarget) &&
                        !rangedInSpellRange)
                    {
                        if (MotionMaster* mm = bot->GetMotionMaster())
                        {
                            mm->MoveChase(currentTarget);
                            directProgress = true;
                        }
                    }
                    else if (!rangedBot)
                    {
                        bot->Attack(currentTarget, true);
                        if (!bot->CanReachWithMeleeAutoAttack(currentTarget))
                        {
                            if (MotionMaster* mm = bot->GetMotionMaster())
                            {
                                mm->MoveChase(currentTarget);
                                directProgress = true;
                            }
                        }
                        const bool meleeActionStarted =
                            bot->GetVictim() == currentTarget &&
                            bot->CanReachWithMeleeAutoAttack(currentTarget) &&
                            bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                        directProgress = directProgress ||
                            sServerFacade.isMoving(bot) ||
                            bot->isMovingOrTurning() ||
                            meleeActionStarted;
                    }
                    else if (inLos && distance <= spellRange + sPlayerbotAIConfig.contactDistance)
                    {
                        bot->Attack(currentTarget, false);
                        OnCombatStarted();
                        // Selecting a ranged victim is not useful by itself; the
                        // following engine pass must actually cast or enter combat.
                        aiObjectContext->GetValue<Unit*>("dps target")->Set(currentTarget);
                        directProgress = TryImmediateRangedCombatAction(this, bot, currentTarget) ||
                            bot->IsNonMeleeSpellCasted(true, false, true);
                    }
                    else if (MotionMaster* mm = bot->GetMotionMaster())
                    {
                        // Out of spell range / no LOS. Ranged bots close only to
                        // their keep-away (flee) distance, not melee reach -- a
                        // bare MoveChase parks them point-blank where the engine's
                        // ranged positioning immediately backs them out, breaking
                        // the cast. Mirrors FleeAction's ranged chase.
                        if (rangedBot)
                            mm->MoveChase(currentTarget, GetRange("flee"), bot->GetAngle(currentTarget));
                        else
                            mm->MoveChase(currentTarget);
                        OnCombatStarted();
                        directProgress = true;
                    }
                }

                const bool nowMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool nowRangedBot = IsRanged(bot) && bot->getClass() != CLASS_PALADIN;
                const bool nowVictimInReach =
                    bot->GetVictim() == currentTarget &&
                    (bot->CanReachWithMeleeAutoAttack(currentTarget) ||
                     (nowRangedBot && bot->IsWithinLOSInMap(currentTarget, true) &&
                      sServerFacade.GetDistance2d(bot, currentTarget) <= GetRange("spell") + sPlayerbotAIConfig.contactDistance));
                const bool nowCasting = bot->IsNonMeleeSpellCasted(true, false, true);
                const bool nowCombat = sServerFacade.IsInCombat(bot);
                const bool passiveRangedVictimOnly = nowRangedBot && nowVictimInReach && !nowCombat && !nowCasting && !nowMoving && !directProgress;
                const bool nowUsefulMoving =
                    nowMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
                const bool nowCountableMoving =
                    nowUsefulMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !nowVictimInReach);
                const bool nowMeleeSwinging =
                    bot->GetVictim() == currentTarget &&
                    bot->CanReachWithMeleeAutoAttack(currentTarget) &&
                    bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                const bool meleeMovingWithoutContact =
                    !nowRangedBot &&
                    nowMoving &&
                    bot->GetVictim() == currentTarget &&
                    !bot->CanReachWithMeleeAutoAttack(currentTarget);
                bool combatConnectKick = false;
                if (!nowCasting &&
                    !nowMeleeSwinging &&
                    (!nowUsefulMoving || nowVictimInReach || meleeMovingWithoutContact) &&
                    noProgressSecs >= VISIBLE_IDLE_WARN_SECONDS)
                {
                    combatConnectKick = ForceCombatConnect(this, bot, currentTarget, "target-fast-lane-stall");
                }

                const bool postKickMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool postKickCasting = bot->IsNonMeleeSpellCasted(true, false, true);
                const bool postKickMeleeSwinging =
                    bot->GetVictim() == currentTarget &&
                    bot->CanReachWithMeleeAutoAttack(currentTarget) &&
                    bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                const bool postKickUsefulMoving =
                    postKickMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || actionMetricMoveProgressTicks > 0);
                const bool postKickCountableMoving =
                    postKickUsefulMoving &&
                    (noProgressSecs < VISIBLE_IDLE_WARN_SECONDS || !nowVictimInReach);
                const bool usefulDirectProgress =
                    directProgress &&
                    (postKickCasting || postKickMeleeSwinging || postKickCountableMoving);
                const bool freshPostActionVictimSetup =
                    nowVictimInReach &&
                    !passiveRangedVictimOnly &&
                    (postKickCasting || postKickMeleeSwinging || postKickCountableMoving) &&
                    noProgressSecs < 2;
                const bool success =
                    usefulDirectProgress ||
                    postKickCountableMoving ||
                    postKickCasting ||
                    postKickMeleeSwinging ||
                    (combatConnectKick && (postKickCountableMoving || postKickCasting || postKickMeleeSwinging)) ||
                    freshPostActionVictimSetup;
                if (success)
                {
                    targetFastLaneNextRetry = 0;
                    targetFastLaneFailures = 0;
                }
                else
                {
                    ++targetFastLaneFailures;
                    targetFastLaneNextRetry = now + ((!postKickMoving && !postKickCasting && !postKickMeleeSwinging) ? 1 :
                        std::min<uint32>(5, 1 + targetFastLaneFailures));
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") &&
                    ((!success && (targetFastLaneFailures <= 3 || urand(1, 10) == 1)) || (success && urand(1, 25) == 1)))
                {
                    std::ostringstream out;
                    out << "target=" << currentTarget->GetName()
                        << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, currentTarget)
                        << " result=" << (forcedAttack ? 1 : 0)
                        << " forcedProgress=" << (forcedProgress ? 1 : 0)
                        << " forcedUsefulMoving=" << (forcedUsefulMoving ? 1 : 0)
                        << " forcedCountableMoving=" << (forcedCountableMoving ? 1 : 0)
                        << " forcedMeleeSwing=" << (forcedMeleeSwinging ? 1 : 0)
                        << " forcedFreshVictimSetup=" << (forcedFreshVictimSetup ? 1 : 0)
                        << " direct=" << (directProgress ? 1 : 0)
                        << " usefulDirect=" << (usefulDirectProgress ? 1 : 0)
                        << " success=" << (success ? 1 : 0)
                        << " failures=" << targetFastLaneFailures
                        << " nextRetrySec=" << (targetFastLaneNextRetry > now ? static_cast<uint32>(targetFastLaneNextRetry - now) : 0)
                        << " state=" << static_cast<uint32>(currentState)
                        << " victim=" << ((bot->GetVictim() == currentTarget) ? 1 : 0)
                        << " victimReach=" << (nowVictimInReach ? 1 : 0)
                        << " passiveRangedVictimOnly=" << (passiveRangedVictimOnly ? 1 : 0)
                        << " moving=" << (postKickMoving ? 1 : 0)
                        << " usefulMoving=" << (postKickUsefulMoving ? 1 : 0)
                        << " countableMoving=" << (postKickCountableMoving ? 1 : 0)
                        << " casting=" << (postKickCasting ? 1 : 0)
                        << " meleeSwing=" << (postKickMeleeSwinging ? 1 : 0)
                        << " connectKick=" << (combatConnectKick ? 1 : 0)
                        << " freshVictimSetup=" << (freshPostActionVictimSetup ? 1 : 0)
                        << " combat=" << (sServerFacade.IsInCombat(bot) ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "TargetFastLaneAttack", std::to_string(currentTarget->GetGUIDLow()), out.str());
                }

                if (success)
                {
                    TrackActionMetrics(false, true, staleReset);
                    return;
                }

                const bool deadTargetStare =
                    targetFastLaneFailures >= 3 &&
                    noProgressSecs >= 7 &&
                    !postKickMoving &&
                    !postKickCasting &&
                    !postKickMeleeSwinging &&
                    !sServerFacade.IsInCombat(bot);
                const bool fakeMovingTargetStare =
                    targetFastLaneFailures >= 3 &&
                    noProgressSecs >= 7 &&
                    postKickMoving &&
                    !postKickCasting &&
                    !postKickMeleeSwinging &&
                    actionMetricMoveProgressTicks == 0;

                if (deadTargetStare || fakeMovingTargetStare)
                {
                    const ObjectGuid targetGuid = currentTarget->GetObjectGuid();
                    const std::string targetName = currentTarget->GetName();
                    if (!sServerFacade.UnitIsDead(currentTarget))
                        DeferTargetGuidForBot(bot, targetGuid, fakeMovingTargetStare ? 12 : 8);
                    aiObjectContext->GetValue<Unit*>("old target")->Reset();
                    aiObjectContext->GetValue<Unit*>("current target")->Reset();
                    aiObjectContext->GetValue<ObjectGuid>("pre-selected next target")->Reset();
                    aiObjectContext->GetValue<Unit*>("pull target")->Reset();
                    aiObjectContext->GetValue<Unit*>("dps target")->Reset();
                    aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
                    bot->SetSelectionGuid(ObjectGuid());
                    ResetStaleTargetState();

                    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                    {
                        std::ostringstream out;
                        out << "target=" << targetName
                            << " noProgressSec=" << noProgressSecs
                            << " failures=" << targetFastLaneFailures
                            << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, currentTarget)
                            << " state=" << static_cast<uint32>(currentState);
                        if (fakeMovingTargetStare)
                            out << " fakeMoving=1";
                        sPlayerbotAIConfig.logEvent(this, "TargetFastLaneHardClear", std::to_string(targetGuid.GetCounter()), out.str());
                    }

                    targetFastLaneGuid = ObjectGuid();
                    targetFastLaneNextRetry = 0;
                    targetFastLaneFailures = 0;
                    bool questRequested = false;
                    bool travelChosen = false;
                    bool targetSelected = false;
                    bool attackAnything = false;
                    bool grindRequested = false;
                    bool travelMoved = false;
                    bool rpgChosen = false;
                    bool rpgMoved = false;
                    bool randomMoved = false;
                    bool idleNudged = false;
                    PushVisibleWork(this, aiObjectContext, bot, "target-fast-lane-hard-clear",
                        questRequested, travelChosen, targetSelected, attackAnything, grindRequested, travelMoved,
                        rpgChosen, rpgMoved, randomMoved, idleNudged);

                    Unit* newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
                    LootObject newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
                    GuidPosition newRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
                    TravelTarget* newTravel = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                    const bool movingAfterClear = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                    const bool directedMovementWork =
                        HasDirectedMovementWork(newTravel, newRpgTarget, travelMoved, rpgMoved, movingAfterClear);
                    const bool workAssigned =
                        HasConcreteRecoveryWork(bot, newTravel, newTarget, newLootTarget, movingAfterClear,
                            travelMoved, false);

                    TrackActionMetrics(false, workAssigned, true);
                    return;
                }
            }
        }
        else
        {
            targetFastLaneGuid = ObjectGuid();
            targetFastLaneNextRetry = 0;
            targetFastLaneFailures = 0;
        }
    }

    if (!minimal &&
        currentState == BotState::BOT_STATE_NON_COMBAT &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        if (RecoverTravelNoMotion(time(0)))
        {
            TrackActionMetrics(false, true, staleReset);
            return;
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !botDead &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        !bot->IsTaxiFlying() &&
        !bot->IsBeingTeleported() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        !sServerFacade.IsInCombat(bot) &&
        !aiObjectContext->GetValue<Unit*>("current target")->Get() &&
        !aiObjectContext->GetValue<Unit*>("dps target")->Get() &&
        aiObjectContext->GetValue<LootObject>("loot target")->Get().IsEmpty() &&
        (sServerFacade.isMoving(bot) || bot->isMovingOrTurning()))
    {
        const time_t now = time(0);
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
        const bool moveStallAction =
            actionMetricLastActionName.empty() ||
            actionMetricLastActionName == "none" ||
            actionMetricLastActionName == "move to travel target" ||
            actionMetricLastActionName == "move to rpg target" ||
            actionMetricLastActionName == "choose rpg target" ||
            actionMetricLastActionName == "choose group travel target" ||
            actionMetricLastActionName == "check values" ||
            actionMetricLastActionName == "update pve strats" ||
            actionMetricLastActionName == "emote" ||
            actionMetricLastActionName == "jump" ||
            actionMetricLastActionName == "drink" ||
            actionMetricLastActionName == "food" ||
            actionMetricLastActionName == "clean quest log" ||
            actionMetricLastActionName == "add all loot" ||
            actionMetricLastActionName == "store loot" ||
            actionMetricLastActionName == "accept trade" ||
            actionMetricLastActionName == "invite nearby" ||
            actionMetricLastActionName == "rpg work" ||
            actionMetricLastActionName == "rpg use" ||
            actionMetricLastActionName == "rpg stay" ||
            actionMetricLastActionName == "rpg emote" ||
            actionMetricLastActionName == "rpg duel" ||
            actionMetricLastActionName == "rpg cancel" ||
            actionMetricLastActionName == "request quest travel target" ||
            actionMetricLastActionName == "choose travel target" ||
            actionMetricLastActionName == "reset travel target";

        if (moveStallAction && noProgressSecs >= 12)
        {
            TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
            TravelDestination* destination = travelTarget ? travelTarget->GetDestination() : nullptr;
            const TravelStatus oldStatus = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
            bool travelCleared = false;

            StopMoving();
            bot->InterruptMoving(true);

            if (travelTarget &&
                (oldStatus == TravelStatus::TRAVEL_STATUS_COOLDOWN ||
                 oldStatus == TravelStatus::TRAVEL_STATUS_EXPIRED ||
                 oldStatus == TravelStatus::TRAVEL_STATUS_WORK ||
                 noProgressSecs >= 18))
            {
                travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                travelTarget->SetForced(false);
                aiObjectContext->GetValue<bool>("travel target active")->Reset();
                aiObjectContext->ClearValues("no active travel destinations");
                travelCleared = true;
            }

            aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
            aiObjectContext->GetValue<LootObject>("loot target")->Reset();

            bool questRequested = false;
            bool travelChosen = false;
            bool targetSelected = false;
            bool attackAnything = false;
            bool grindRequested = false;
            bool travelMoved = false;
            bool rpgChosen = false;
            bool rpgMoved = false;
            bool randomMoved = false;
            bool idleNudged = false;

            if (!travelCleared && travelTarget &&
                (oldStatus == TravelStatus::TRAVEL_STATUS_READY ||
                 oldStatus == TravelStatus::TRAVEL_STATUS_TRAVEL))
            {
                travelMoved = DoSpecificAction("move to travel target",
                    Event("movement no progress", "travel retry", bot), true);
            }

            if (!HasUsefulAssignedWork(bot, travelTarget, aiObjectContext->GetValue<Unit*>("current target")->Get(),
                    aiObjectContext->GetValue<LootObject>("loot target")->Get(),
                    sServerFacade.isMoving(bot) || bot->isMovingOrTurning(), travelMoved))
            {
                PushVisibleWork(this, aiObjectContext, bot, "movement-no-progress", questRequested, travelChosen,
                    targetSelected, attackAnything, grindRequested, travelMoved, rpgChosen, rpgMoved,
                    randomMoved, idleNudged, true);
            }
            Unit* newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
            LootObject newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
            GuidPosition newRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
            TravelTarget* newTravel = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
            TravelDestination* newDestination = newTravel ? newTravel->GetDestination() : nullptr;
            if (newTarget &&
                !bot->IsNonMeleeSpellCasted(true, false, true) &&
                !(bot->GetVictim() == newTarget && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING)))
            {
                attackAnything = ForceConcreteTargetWork(this, bot, newTarget, "movement-no-progress-target-handoff") ||
                    attackAnything;
                newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
                newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
                newRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
                newTravel = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                newDestination = newTravel ? newTravel->GetDestination() : nullptr;
            }
            const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
            const bool directedMovementWork =
                HasDirectedMovementWork(newTravel, newRpgTarget, travelMoved, rpgMoved, movingAfter);
            const bool workAssigned =
                HasConcreteRecoveryWork(bot, newTravel, newTarget, newLootTarget, movingAfter,
                    travelMoved, false);

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                    << " noProgressSec=" << noProgressSecs
                    << " oldStatus=" << static_cast<uint32>(oldStatus)
                    << " newStatus=" << (newTravel ? static_cast<uint32>(newTravel->GetStatus()) : 0)
                    << " cleared=" << (travelCleared ? 1 : 0)
                    << " travelMove=" << (travelMoved ? 1 : 0)
                    << " quest=" << (questRequested ? 1 : 0)
                    << " choose=" << (travelChosen ? 1 : 0)
                    << " selectTarget=" << (targetSelected ? 1 : 0)
                    << " attackAnything=" << (attackAnything ? 1 : 0)
                    << " grindRequest=" << (grindRequested ? 1 : 0)
                    << " rpgChoose=" << (rpgChosen ? 1 : 0)
                    << " rpgMove=" << (rpgMoved ? 1 : 0)
                    << " randomMove=" << (randomMoved ? 1 : 0)
                    << " idleNudge=" << (idleNudged ? 1 : 0)
                    << " movingAfter=" << (movingAfter ? 1 : 0)
                    << " directedMove=" << (directedMovementWork ? 1 : 0)
                    << " target=" << (newTarget ? newTarget->GetName() : "none")
                    << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                    << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                    << " workAssigned=" << (workAssigned ? 1 : 0)
                    << " destinationPtr=" << (newDestination ? 1 : (destination ? 1 : 0));
                sPlayerbotAIConfig.logEvent(this, "MovementNoProgressReset", "", out.str());
            }

            if (workAssigned)
            {
                TrackActionMetrics(false, true, staleReset || travelCleared);
                return;
            }

            if (movingAfter || newRpgTarget ||
                (newTravel && !IsActiveTravelWork(newTravel) && !IsPreparedTravelWork(newTravel)))
            {
                StopMoving();
                bot->InterruptMoving(true);
                aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();

                if (newTravel && !IsActiveTravelWork(newTravel) && !IsPreparedTravelWork(newTravel))
                {
                    newTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    newTravel->SetForced(false);
                    aiObjectContext->GetValue<bool>("travel target active")->Reset();
                }

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 5) == 1)
                {
                    std::ostringstream out;
                    out << "reason=movement-no-progress"
                        << " noProgressSec=" << noProgressSecs
                        << " movingAfter=" << (movingAfter ? 1 : 0)
                        << " travelStatus=" << (newTravel ? static_cast<uint32>(newTravel->GetStatus()) : 0)
                        << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                        << " target=" << (newTarget ? newTarget->GetName() : "none")
                        << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                        << " randomMove=" << (randomMoved ? 1 : 0)
                        << " idleNudge=" << (idleNudged ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "NoConcreteWorkClear", "", out.str());
                }
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        sServerFacade.IsInCombat(bot) &&
        !aiObjectContext->GetValue<bool>("has attackers")->Get() &&
        !aiObjectContext->GetValue<Unit*>("dps target")->Get())
    {
        Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
        const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(time(0) - actionMetricLastProgress) : 0;
        const bool staleCombat =
            !moving &&
            !bot->GetVictim() &&
            !currentTarget &&
            lootTarget.IsEmpty() &&
            (noProgressSecs >= 5 || actionMetricNoProgressTicks >= 3 || actionMetricSameActionStreak >= 20);

        if (staleCombat)
        {
            ObjectGuid attackTarget = aiObjectContext->GetValue<ObjectGuid>("attack target")->Get();
            aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
            aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
            bot->CombatStop(false);
            OnCombatEnded();

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " attackTarget=" << (attackTarget ? 1 : 0);
                sPlayerbotAIConfig.logEvent(this, "CombatIdleNoTargetReset", "", out.str());
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
        const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(time(0) - actionMetricLastProgress) : 0;
        const bool lootStallAction =
            actionMetricLastActionName.empty() ||
            actionMetricLastActionName == "none" ||
            actionMetricLastActionName == "check values" ||
            actionMetricLastActionName == "emote" ||
            actionMetricLastActionName == "jump" ||
            actionMetricLastActionName == "clean quest log" ||
            actionMetricLastActionName == "add all loot" ||
            actionMetricLastActionName == "loot" ||
            actionMetricLastActionName == "open loot" ||
            actionMetricLastActionName == "store loot" ||
            actionMetricLastActionName == "move random" ||
            actionMetricLastActionName == "move to loot" ||
            actionMetricLastActionName == "move to travel target" ||
            actionMetricLastActionName == "move to rpg target" ||
            actionMetricLastActionName == "choose rpg target" ||
            actionMetricLastActionName == "choose travel target" ||
            actionMetricLastActionName == "reset travel target" ||
            actionMetricLastActionName == "update pve strats" ||
            actionMetricLastActionName == "accept trade" ||
            actionMetricLastActionName == "invite nearby" ||
            actionMetricLastActionName == "rpg work" ||
            actionMetricLastActionName == "rpg use" ||
            actionMetricLastActionName == "rpg stay" ||
            actionMetricLastActionName == "rpg emote" ||
            actionMetricLastActionName == "rpg duel" ||
            actionMetricLastActionName == "rpg cancel" ||
            actionMetricLastActionName == "request quest travel target";

        if (!lootTarget.IsEmpty() &&
            !moving &&
            lootStallAction &&
            noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS &&
            (actionMetricNoProgressTicks >= 20 || actionMetricSameActionStreak >= 20))
        {
            Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
            const bool combatPressure =
                sServerFacade.IsInCombat(bot) ||
                aiObjectContext->GetValue<bool>("has attackers")->Get() ||
                aiObjectContext->GetValue<Unit*>("dps target")->Get() ||
                (currentTarget && currentTarget->IsInWorld() && !sServerFacade.UnitIsDead(currentTarget));
            const bool possible = !combatPressure && lootTarget.IsLootPossible(bot);
            const bool closeEnough = possible && IsLootObjectCloseEnoughToOpen(bot, lootTarget);
            bool opened = false;
            bool movedToLoot = false;
            bool cleared = false;
            bool clearedTravel = false;

            if (possible)
            {
                aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
                aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();

                TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget && travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
                {
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    travelTarget->SetForced(false);
                    aiObjectContext->GetValue<bool>("travel target active")->Reset();
                    clearedTravel = true;
                }
            }

            if (closeEnough)
                opened = DoSpecificAction("open loot", Event("loot stall fast lane", lootTarget.guid, bot), true);
            else if (possible)
                movedToLoot = DoSpecificAction("move to loot", Event("loot stall fast lane", lootTarget.guid, bot), true);

            const bool failedLootRescue =
                !opened &&
                !movedToLoot &&
                (combatPressure ||
                 !possible ||
                 noProgressSecs >= 20 ||
                 actionMetricNoProgressTicks >= 45 ||
                 actionMetricSameActionStreak >= 45);

            if (failedLootRescue)
            {
                DeferLootGuidForBot(bot, lootTarget.guid, combatPressure ? 5 : 20);
                if (LootObjectStack* availableLoot = aiObjectContext->GetValue<LootObjectStack*>("available loot")->Get())
                    availableLoot->Remove(lootTarget.guid);
                aiObjectContext->GetValue<LootObject>("loot target")->Reset();

                TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget &&
                    (travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK ||
                     travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN))
                {
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                    travelTarget->SetForced(false);
                    aiObjectContext->GetValue<bool>("travel target active")->Reset();
                }

                cleared = true;
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "action=" << (actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName)
                    << " noProgressSec=" << noProgressSecs
                    << " noProgressTicks=" << actionMetricNoProgressTicks
                    << " sameActionStreak=" << actionMetricSameActionStreak
                    << " combatPressure=" << (combatPressure ? 1 : 0)
                    << " possible=" << (possible ? 1 : 0)
                    << " close=" << (closeEnough ? 1 : 0)
                    << " opened=" << (opened ? 1 : 0)
                    << " moved=" << (movedToLoot ? 1 : 0)
                    << " cleared=" << (cleared ? 1 : 0)
                    << " clearedTravel=" << (clearedTravel ? 1 : 0)
                    << " guid=" << lootTarget.guid.GetCounter()
                    << " target=" << (currentTarget ? currentTarget->GetName() : "none");
                sPlayerbotAIConfig.logEvent(this, "LootStallFastLane", std::to_string(lootTarget.guid.GetCounter()), out.str());
            }

            if (opened || movedToLoot || cleared)
            {
                TrackActionMetrics(false, true, staleReset);
                return;
            }
        }
    }

    if (!minimal &&
        currentState != BotState::BOT_STATE_DEAD &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true) &&
        !sServerFacade.IsInCombat(bot) &&
        !aiObjectContext->GetValue<bool>("has attackers")->Get() &&
        !aiObjectContext->GetValue<Unit*>("dps target")->Get())
    {
        TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
        if (travelTarget && travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
        {
            TravelDestination* destination = travelTarget->GetDestination();
            const bool nullDestination = !destination || dynamic_cast<NullTravelDestination*>(destination);
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            travelTarget->SetForced(false);
            aiObjectContext->GetValue<bool>("travel target active")->Reset();
            aiObjectContext->ClearValues("no active travel destinations");
            aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();
            aiObjectContext->GetValue<ObjectGuid>("attack target")->Reset();
            aiObjectContext->GetValue<LootObject>("loot target")->Reset();

            Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
            LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
            const bool shouldSeedWork = !currentTarget && lootTarget.IsEmpty();
            bool combatEnded = false;
            if (nullDestination && IsStateActive(BotState::BOT_STATE_COMBAT) && shouldSeedWork)
            {
                OnCombatEnded();
                combatEnded = true;
            }

            if (nullDestination)
            {
                if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 10) == 1)
                {
                    std::ostringstream out;
                    out << "state=" << static_cast<uint32>(currentState)
                        << " seedWork=" << (shouldSeedWork ? 1 : 0)
                        << " combatEnded=" << (combatEnded ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "TravelNullExpiredReset", "", out.str());
                }

                // Do not return here. Null/expired cleanup is bookkeeping; let
                // the idle fast lane below assign real work in this same tick.
            }
            else
            {
                const bool questRequested = shouldSeedWork ? DoSpecificAction("request quest travel target", Event(), true) : false;
                const bool targetSelected = (!questRequested && shouldSeedWork) ? DoSpecificAction("select new target", Event(), true) : false;
                const bool randomMoved = (!questRequested && !targetSelected && shouldSeedWork) ? DoSpecificAction("move random", Event("expired travel rescue", "idle rescue", bot), true) : false;
                Unit* newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
                LootObject newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
                const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool fallbackMovementWork = HasFallbackMovementWork(randomMoved, false, movingAfter);
                const bool workAssigned =
                    HasConcreteRecoveryWork(bot, travelTarget, newTarget, newLootTarget, movingAfter, false, false) ||
                    fallbackMovementWork;

                if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
                {
                    std::ostringstream out;
                    out << "state=" << static_cast<uint32>(currentState)
                        << " destinationPtr=" << (destination ? 1 : 0)
                        << " seedWork=" << (shouldSeedWork ? 1 : 0)
                        << " quest=" << (questRequested ? 1 : 0)
                        << " selectTarget=" << (targetSelected ? 1 : 0)
                        << " randomMove=" << (randomMoved ? 1 : 0)
                        << " nudgeOnly=" << (randomMoved && !workAssigned ? 1 : 0)
                        << " fallbackMove=" << (fallbackMovementWork ? 1 : 0)
                        << " movingAfter=" << (movingAfter ? 1 : 0)
                        << " target=" << (newTarget ? newTarget->GetName() : "none")
                        << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                        << " workAssigned=" << (workAssigned ? 1 : 0);
                    sPlayerbotAIConfig.logEvent(this, "TravelExpiredFastReset", "", out.str());
                }

                if (workAssigned)
                {
                    TrackActionMetrics(false, true, staleReset);
                    return;
                }
            }
        }
    }

    if (!minimal &&
        currentState == BotState::BOT_STATE_NON_COMBAT &&
        !HasActivePlayerMaster() &&
        !HasRealPlayerMaster() &&
        !bot->IsTaxiFlying() &&
        !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        LootObject lootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
        const bool moving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
        const bool combat = sServerFacade.IsInCombat(bot);
        TravelTarget* travelTarget = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
        GuidPosition existingRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
        const bool movingWithAssignedWork =
            moving && (IsActiveTravelWork(travelTarget) || IsPreparedTravelWork(travelTarget) || existingRpgTarget);

        if ((!moving || !movingWithAssignedWork) && !combat && !currentTarget && lootTarget.IsEmpty())
        {
            TravelDestination* destination = travelTarget ? travelTarget->GetDestination() : nullptr;
            const TravelStatus status = travelTarget ? travelTarget->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
            const bool nullDestination = !destination || dynamic_cast<NullTravelDestination*>(destination);
            const bool preparingTravel = status == TravelStatus::TRAVEL_STATUS_PREPARE;
            const bool movingWithoutAssignedWork = moving && !movingWithAssignedWork;
            const time_t now = time(0);
            const uint32 noProgressSecs = actionMetricLastProgress ? static_cast<uint32>(now - actionMetricLastProgress) : 0;
            const bool recentUnassignedMovement =
                movingWithoutAssignedWork && noProgressSecs < VISIBLE_IDLE_WARN_SECONDS;
            const bool idleAction =
                actionMetricLastActionName.empty() ||
                actionMetricLastActionName == "none" ||
                actionMetricLastActionName == "check values" ||
                actionMetricLastActionName == "update pve strats" ||
                actionMetricLastActionName == "emote" ||
                actionMetricLastActionName == "cannibalize" ||
                actionMetricLastActionName == "drink" ||
                actionMetricLastActionName == "food" ||
                actionMetricLastActionName == "clean quest log" ||
                actionMetricLastActionName == "add all loot" ||
                actionMetricLastActionName == "loot" ||
                actionMetricLastActionName == "store loot" ||
                actionMetricLastActionName == "move random" ||
                actionMetricLastActionName == "move to travel target" ||
                actionMetricLastActionName == "move to rpg target" ||
                actionMetricLastActionName == "choose rpg target" ||
                actionMetricLastActionName == "choose travel target" ||
                actionMetricLastActionName == "request quest travel target" ||
                actionMetricLastActionName == "request named travel target" ||
                actionMetricLastActionName == "select new target" ||
                actionMetricLastActionName == "accept invitation" ||
                actionMetricLastActionName == "invite nearby" ||
                actionMetricLastActionName == "attack anything" ||
                actionMetricLastActionName == "attack my target" ||
                actionMetricLastActionName == "rpg work" ||
                actionMetricLastActionName == "rpg use" ||
                actionMetricLastActionName == "rpg stay" ||
                actionMetricLastActionName == "rpg emote" ||
                actionMetricLastActionName == "rpg sell" ||
                actionMetricLastActionName == "rpg cancel" ||
                actionMetricLastActionName == "reset travel target";
            const bool aliveNoWorkStall =
                !recentUnassignedMovement &&
                (idleAction || movingWithoutAssignedWork) &&
                (movingWithoutAssignedWork || actionMetricNoProgressTicks >= 2 || noProgressSecs >= 3 || actionMetricSameActionStreak >= 20);
            const bool cooldownNoWorkStall =
                travelTarget &&
                status == TravelStatus::TRAVEL_STATUS_COOLDOWN &&
                aliveNoWorkStall &&
                noProgressSecs >= VISIBLE_IDLE_RESCUE_SECONDS;
            const bool travelWorkStall =
                travelTarget &&
                status == TravelStatus::TRAVEL_STATUS_WORK &&
                actionMetricSameActionStreak >= 20 &&
                (actionMetricLastActionName == "check values" ||
                 actionMetricLastActionName == "request quest travel target" ||
                 actionMetricLastActionName == "reset travel target");
            const bool staleTravel =
                !recentUnassignedMovement &&
                ((preparingTravel && noProgressSecs >= 20) ||
                (!preparingTravel &&
                (
                !travelTarget ||
                status == TravelStatus::TRAVEL_STATUS_NONE ||
                status == TravelStatus::TRAVEL_STATUS_EXPIRED ||
                (nullDestination &&
                 (status == TravelStatus::TRAVEL_STATUS_TRAVEL ||
                  status == TravelStatus::TRAVEL_STATUS_WORK)) ||
                (status == TravelStatus::TRAVEL_STATUS_COOLDOWN && nullDestination) ||
                cooldownNoWorkStall ||
                aliveNoWorkStall ||
                travelWorkStall)));

            if (staleTravel)
            {
                if (movingWithoutAssignedWork)
                    StopMoving();

                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);

                aiObjectContext->GetValue<bool>("travel target active")->Reset();
                const bool aliveNoWorkTravelReseed =
                    aliveNoWorkStall &&
                    (noProgressSecs >= 3 || actionMetricNoProgressTicks >= 5);
                const bool shouldClearNoActiveDestinations =
                    aliveNoWorkTravelReseed ||
                    travelWorkStall ||
                    cooldownNoWorkStall ||
                    status == TravelStatus::TRAVEL_STATUS_EXPIRED ||
                    (nullDestination &&
                     (status == TravelStatus::TRAVEL_STATUS_TRAVEL ||
                      status == TravelStatus::TRAVEL_STATUS_WORK)) ||
                    (status == TravelStatus::TRAVEL_STATUS_COOLDOWN && !nullDestination);
                if (shouldClearNoActiveDestinations)
                {
                    // Keep known-empty generic travel purposes (especially Grind)
                    // suppressed; otherwise idle rescue immediately re-requests the
                    // same empty world scan. Quest gets a targeted retry because
                    // quest guide/hub state changes frequently and is progression-critical.
                    aiObjectContext->GetValue<bool>("no active travel destinations", "quest")->Reset();

                    if (sPlayerbotAIConfig.hasLog("bot_events.csv") && aliveNoWorkTravelReseed && urand(1, 10) == 1)
                    {
                        std::ostringstream out;
                        out << "status=" << static_cast<uint32>(status)
                            << " nullDest=" << (nullDestination ? 1 : 0)
                            << " movingWithoutWork=" << (movingWithoutAssignedWork ? 1 : 0)
                            << " noProgressSec=" << noProgressSecs
                            << " sameActionStreak=" << actionMetricSameActionStreak
                            << " lastAction=" << actionMetricLastActionName
                            << " clearedQuestOnly=1";
                        sPlayerbotAIConfig.logEvent(this, "IdleTravelReseed", "", out.str());
                    }
                }

                bool questRequested = false;
                bool travelChosen = false;
                bool travelRefreshed = false;
                bool targetSelected = false;
                bool attackAnything = false;
                bool grindRequested = false;
                bool travelMoved = false;
                bool rpgChosen = false;
                bool rpgMoved = false;
                bool randomMoved = false;
                bool idleNudged = false;

                const bool pushedVisibleWork = PushVisibleWork(this, aiObjectContext, bot, "idle-work-fast-lane",
                    questRequested, travelChosen,
                    targetSelected, attackAnything, grindRequested, travelMoved, rpgChosen, rpgMoved,
                    randomMoved, idleNudged, true);

                const bool allowIdleFallbackMovement = true;
                if (allowIdleFallbackMovement &&
                    !pushedVisibleWork &&
                    !HasUsefulAssignedWork(bot, travelTarget, aiObjectContext->GetValue<Unit*>("current target")->Get(),
                        aiObjectContext->GetValue<LootObject>("loot target")->Get(),
                        sServerFacade.isMoving(bot) || bot->isMovingOrTurning(),
                        HasDirectedMovementWork(travelTarget, aiObjectContext->GetValue<GuidPosition>("rpg target")->Get(),
                            travelMoved, rpgMoved, sServerFacade.isMoving(bot) || bot->isMovingOrTurning())))
                {
                    travelRefreshed = DoSpecificAction("refresh travel target", Event(), true);
                    if (travelRefreshed)
                        travelMoved = MoveAssignedTravelWork(this, aiObjectContext, bot, "idle-work-fast-lane") || travelMoved;
                }

                TravelTarget* currentTravel = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                TravelDestination* currentDestination = currentTravel ? currentTravel->GetDestination() : nullptr;
                TravelStatus newStatus = currentTravel ? currentTravel->GetStatus() : TravelStatus::TRAVEL_STATUS_NONE;
                Unit* newTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
                LootObject newLootTarget = aiObjectContext->GetValue<LootObject>("loot target")->Get();
                GuidPosition newRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
                const bool movingAfter = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                const bool directedMovementWork =
                    HasDirectedMovementWork(currentTravel, newRpgTarget, travelMoved, rpgMoved, movingAfter);
                const bool fallbackMovementWork =
                    HasFallbackMovementWork(randomMoved, idleNudged, movingAfter);
                const bool workAssigned =
                    HasConcreteRecoveryWork(bot, currentTravel, newTarget, newLootTarget, movingAfter, travelMoved, false) ||
                    directedMovementWork ||
                    fallbackMovementWork;

                if (sPlayerbotAIConfig.hasLog("bot_events.csv") &&
                    ((workAssigned && urand(1, 10) == 1) || (!workAssigned && urand(1, 20) == 1)))
                {
                    std::ostringstream out;
                    out << "oldStatus=" << static_cast<uint32>(status)
                        << " newStatus=" << static_cast<uint32>(newStatus)
                        << " nullDest=" << (nullDestination ? 1 : 0)
                        << " aliveNoWork=" << (aliveNoWorkStall ? 1 : 0)
                        << " movingWithoutWork=" << (movingWithoutAssignedWork ? 1 : 0)
                        << " noProgressSec=" << noProgressSecs
                        << " workStall=" << (travelWorkStall ? 1 : 0)
                        << " quest=" << (questRequested ? 1 : 0)
                        << " choose=" << (travelChosen ? 1 : 0)
                        << " refresh=" << (travelRefreshed ? 1 : 0)
                        << " selectTarget=" << (targetSelected ? 1 : 0)
                        << " attackAnything=" << (attackAnything ? 1 : 0)
                        << " grindRequest=" << (grindRequested ? 1 : 0)
                        << " rpgChoose=" << (rpgChosen ? 1 : 0)
                        << " travelMove=" << (travelMoved ? 1 : 0)
                        << " rpgMove=" << (rpgMoved ? 1 : 0)
                        << " randomMove=" << (randomMoved ? 1 : 0)
                        << " idleNudge=" << (idleNudged ? 1 : 0)
                        << " nudgeOnly=" << ((randomMoved || idleNudged) && !workAssigned ? 1 : 0)
                        << " directedMove=" << (directedMovementWork ? 1 : 0)
                        << " fallbackMove=" << (fallbackMovementWork ? 1 : 0)
                        << " movingAfter=" << (movingAfter ? 1 : 0)
                        << " target=" << (newTarget ? newTarget->GetName() : "none")
                        << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                        << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                        << " workAssigned=" << (workAssigned ? 1 : 0)
                        << " destinationPtr=" << (currentDestination ? 1 : (destination ? 1 : 0));
                    sPlayerbotAIConfig.logEvent(this, "IdleWorkFastLane", "", out.str());
                }

                if (workAssigned)
                {
                    TrackActionMetrics(false, true, staleReset);
                    return;
                }

                if (sServerFacade.isMoving(bot) || bot->isMovingOrTurning() || newRpgTarget ||
                    (currentTravel && !IsActiveTravelWork(currentTravel) && !IsPreparedTravelWork(currentTravel)))
                {
                    StopMoving();
                    bot->InterruptMoving(true);
                    aiObjectContext->GetValue<GuidPosition>("rpg target")->Reset();

                    if (newTarget &&
                        !bot->IsNonMeleeSpellCasted(true, false, true) &&
                        !(bot->GetVictim() == newTarget &&
                          bot->CanReachWithMeleeAutoAttack(newTarget) &&
                          bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING)))
                    {
                        DeferTargetGuidForBot(bot, newTarget->GetObjectGuid(), 12);
                        ClearCurrentCombatTarget(aiObjectContext, bot, newTarget, false);
                        ResetStaleTargetState();
                        ResetTargetScanCaches(aiObjectContext);
                        newTarget = nullptr;
                    }

                    if (!newLootTarget.IsEmpty())
                    {
                        DeferLootGuidForBot(bot, newLootTarget.guid, 10);
                        if (LootObjectStack* availableLoot = aiObjectContext->GetValue<LootObjectStack*>("available loot")->Get())
                            availableLoot->Remove(newLootTarget.guid);
                        aiObjectContext->GetValue<LootObject>("loot target")->Reset();
                        newLootTarget = LootObject();
                    }

                    if (currentTravel && !IsActiveTravelWork(currentTravel) && !IsPreparedTravelWork(currentTravel))
                    {
                        currentTravel->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                        currentTravel->SetForced(false);
                        aiObjectContext->GetValue<bool>("travel target active")->Reset();
                    }

                    if (sPlayerbotAIConfig.hasLog("bot_events.csv") && urand(1, 4) == 1)
                    {
                        std::ostringstream out;
                        out << "noProgressSec=" << noProgressSecs
                            << " oldStatus=" << static_cast<uint32>(status)
                            << " newStatus=" << static_cast<uint32>(newStatus)
                            << " movingAfter=" << ((sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0)
                            << " rpgTarget=" << (newRpgTarget ? 1 : 0)
                            << " target=" << (newTarget ? newTarget->GetName() : "none")
                            << " loot=" << (!newLootTarget.IsEmpty() ? 1 : 0)
                            << " destinationPtr=" << (currentDestination ? 1 : (destination ? 1 : 0));
                        sPlayerbotAIConfig.logEvent(this, "IdleNoConcreteWorkClear", "", out.str());
                    }

                    bool rescueQuestRequested = false;
                    bool rescueTravelChosen = false;
                    bool rescueTargetSelected = false;
                    bool rescueAttackAnything = false;
                    bool rescueGrindRequested = false;
                    bool rescueTravelMoved = false;
                    bool rescueRpgChosen = false;
                    bool rescueRpgMoved = false;
                    bool rescueRandomMoved = false;
                    bool rescueIdleNudged = false;

                    PushVisibleWork(this, aiObjectContext, bot, "idle-no-concrete-clear",
                        rescueQuestRequested, rescueTravelChosen, rescueTargetSelected, rescueAttackAnything,
                        rescueGrindRequested, rescueTravelMoved, rescueRpgChosen, rescueRpgMoved,
                        rescueRandomMoved, rescueIdleNudged, true, false);

                    TravelTarget* rescueTravel = aiObjectContext->GetValue<TravelTarget*>("travel target")->Get();
                    LootObject rescueLoot = aiObjectContext->GetValue<LootObject>("loot target")->Get();
                    GuidPosition rescueRpgTarget = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
                    const bool rescueMoving = sServerFacade.isMoving(bot) || bot->isMovingOrTurning();
                    const bool rescueDirectedWork =
                        HasDirectedMovementWork(rescueTravel, rescueRpgTarget, rescueTravelMoved, rescueRpgMoved, rescueMoving);
                    const bool rescueFallbackWork =
                        HasFallbackMovementWork(rescueRandomMoved, rescueIdleNudged, rescueMoving);
                    const bool rescueWorkAssigned =
                        HasConcreteRecoveryWork(bot, rescueTravel, aiObjectContext->GetValue<Unit*>("current target")->Get(),
                            rescueLoot, rescueMoving, rescueTravelMoved, false) ||
                        rescueDirectedWork ||
                        rescueFallbackWork;

                    if (sPlayerbotAIConfig.hasLog("bot_events.csv") && (rescueWorkAssigned || urand(1, 5) == 1))
                    {
                        std::ostringstream out;
                        out << "quest=" << (rescueQuestRequested ? 1 : 0)
                            << " choose=" << (rescueTravelChosen ? 1 : 0)
                            << " grindRequest=" << (rescueGrindRequested ? 1 : 0)
                            << " travelMove=" << (rescueTravelMoved ? 1 : 0)
                            << " rpgChoose=" << (rescueRpgChosen ? 1 : 0)
                            << " rpgMove=" << (rescueRpgMoved ? 1 : 0)
                            << " randomMove=" << (rescueRandomMoved ? 1 : 0)
                            << " idleNudge=" << (rescueIdleNudged ? 1 : 0)
                            << " movingAfter=" << (rescueMoving ? 1 : 0)
                            << " directedMove=" << (rescueDirectedWork ? 1 : 0)
                            << " fallbackMove=" << (rescueFallbackWork ? 1 : 0)
                            << " workAssigned=" << (rescueWorkAssigned ? 1 : 0)
                            << " target=" << (aiObjectContext->GetValue<Unit*>("current target")->Get() ?
                                aiObjectContext->GetValue<Unit*>("current target")->Get()->GetName() : "none")
                            << " loot=" << (!rescueLoot.IsEmpty() ? 1 : 0);
                        sPlayerbotAIConfig.logEvent(this, "IdleNoConcreteWorkRescue", "", out.str());
                    }

                    if (rescueWorkAssigned)
                    {
                        TrackActionMetrics(false, true, staleReset);
                        return;
                    }
                }
            }
        }
    }

    } // end if (kEnableLegacyFastLanes) - calculated mode runs the engine directly

    // Per-tick decision trace (EnableActionLog): the bot's pre-decision state, so
    // the per-bot log under logs/bots/ shows position/target/flags alongside the
    // engine's A:<action> lines - enough to spot movement/target thrash & ordering.
    if (ai::botdiag::IsActionLogEnabled())
    {
        Unit* ctTrace = aiObjectContext->GetValue<Unit*>("current target")->Get();
        LootObject lootTrace = aiObjectContext->GetValue<LootObject>("loot target")->Get();
        GuidPosition rpgTrace = aiObjectContext->GetValue<GuidPosition>("rpg target")->Get();
        bool travelActiveTrace = aiObjectContext->GetValue<bool>("travel target active")->Get();
        ai::botdiag::BotActionLog::Write(this, "STATE",
            "pos=%.1f,%.1f,%.1f mov=%d combat=%d hp=%u tgt=%s loot=%d rpg=%d travelActive=%d last=%s",
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            (sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0,
            sServerFacade.IsInCombat(bot) ? 1 : 0,
            (uint32)bot->GetHealthPercent(),
            ctTrace ? ctTrace->GetName() : "none",
            lootTrace.IsEmpty() ? 0 : 1,
            rpgTrace ? 1 : 0,
            travelActiveTrace ? 1 : 0,
            GetLastExecutedAction(currentState) ? const_cast<Action*>(GetLastExecutedAction(currentState))->getName().c_str() : "none");
    }

    // COMBAT-STATE RECONCILE (bot-cam evidence): OnCombatStarted() is ACTION-driven -- it only
    // fires when the bot itself initiates an attack (AttackAction/PullActions). A bot dragged into
    // combat by a mob aggro or by its own pet's pull therefore stays in the NON-COMBAT engine and
    // runs RPG busywork (check mail / feed pet / choose rpg target) while being hit -- watched a
    // hunter "check mail" for 12s mid-fight, HP bleeding 100->59. Force the combat engine whenever
    // we are genuinely in combat with a live threat. ChangeEngine() only does work on a real
    // transition (currentEngine != engine), so this is a cheap no-op once we're already in combat.
    if (bot->IsInCombat() && currentState == BotState::BOT_STATE_NON_COMBAT &&
        (bot->GetVictim() || !bot->getAttackers().empty()))
    {
        OnCombatStarted();
    }

    SC_PHASE("DoNextAction.engineDoNextAction", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-engine", bot ? bot->GetGUIDLow() : 0);
    // AUTONOMOUS FSM: for out-of-combat autonomous random bots, the deterministic FSM owns the
    // decision (execute-or-commit, no thrash). It either HANDLES the tick (movement/grind/loot/
    // relocate) or DELEGATEs to the relevance engine for productive non-move work (quests/rest).
    // Combat is untouched: the combat reconcile above has already flipped currentState to COMBAT
    // when genuinely fighting, so the FSM gate below is false and the combat engine runs as before.
    bool actionExecuted;
    bool fsmExecuted = false;
    // MIND (the decision-making rewrite): autonomous random bots run the
    // intent layer. Non-combat: the slimmed engine gets a reactive pass first
    // (packet features: store loot, invites, trade, buffs, food) and the mind
    // owns the proactive decision when the engine declines. Combat: the mind
    // pins the target, chases and queues the next task; the engine runs the
    // in-range rotation. Supersedes the inline FSM branches below (which
    // remain the AiPlayerbot.Mind=0 rollback path).
    const bool mindOwned = sPlayerbotAIConfig.mindEnabled
        && sRandomPlayerbotMgr.IsRandomBot(bot)
        && !HasRealPlayerMaster()
        && !HasActivePlayerMaster()
        && bot->IsAlive()
        && !bot->InBattleGround();   // BG bots keep the dedicated BG strategy stack
    if (mindOwned && (currentState == BotState::BOT_STATE_NON_COMBAT || currentState == BotState::BOT_STATE_COMBAT))
    {
        if (!botMind)
            botMind.reset(new mind::BotMind(this, bot));
        if (currentState == BotState::BOT_STATE_NON_COMBAT)
        {
            // BUSY HOLD first (mid-cast/taxi): skip BOTH passes — re-running
            // the engine mid-cast cancels the cast (the FSM's short-circuit).
            // Otherwise DUAL PASS, disjoint domains: the slimmed engine
            // handles stationary/reactive features (it owns no movement
            // strategies), then the mind takes its proactive step and OWNS
            // the update delay — engine bookkeeping actions set multi-second
            // durations that would otherwise put the fleet to sleep.
            if (botMind->BusyHold())
                actionExecuted = true;
            else
            {
                const bool engineActed = currentEngine->DoNextAction(NULL, 0, (minimal || min), bot->IsTaxiFlying());
                const bool mindActed = botMind->Step(minimal || min);
                actionExecuted = engineActed || mindActed;
            }
        }
        else
        {
            bool mindExecuted = false;
            if (botMind->CombatStep((minimal || min), &mindExecuted))
                actionExecuted = mindExecuted;
            else
                actionExecuted = currentEngine->DoNextAction(NULL, 0, (minimal || min), bot->IsTaxiFlying());
        }
    }
    else if (sPlayerbotAIConfig.autonomousFsm && !sPlayerbotAIConfig.mindEnabled
        && currentState == BotState::BOT_STATE_NON_COMBAT
        && sRandomPlayerbotMgr.IsRandomBot(bot)
        && !HasRealPlayerMaster()
        && !HasActivePlayerMaster()
        && AutonomousFsmTick(this, bot, (minimal || min), &fsmExecuted) == FSM_HANDLED)
    {
        actionExecuted = fsmExecuted;
    }
    else if (sPlayerbotAIConfig.combatDirector && !sPlayerbotAIConfig.mindEnabled
        && currentState == BotState::BOT_STATE_COMBAT
        && sRandomPlayerbotMgr.IsRandomBot(bot)
        && !HasRealPlayerMaster()
        && !HasActivePlayerMaster()
        && CombatFsmTick(this, bot, &fsmExecuted) == FSM_HANDLED)
    {
        actionExecuted = fsmExecuted;   // director forced a snappy chase; engine skipped this tick
    }
    else
    {
        actionExecuted = currentEngine->DoNextAction(NULL, 0, (minimal || min), bot->IsTaxiFlying());
    }
    SC_PHASE("DoNextAction.afterEngine", bot ? bot->GetName() : "(null)");
    MapManager::SetContinentUpdatePhase("bot-engine-done", bot ? bot->GetGUIDLow() : 0);

    TrackActionMetrics((minimal || min), actionExecuted, staleReset);

    // BOT-CAM: complete per-second snapshot of this bot (my eyes on the watched fleet). Gated by
    // the action-log flag so it only runs for the small watched fleet, never at 10-15k scale.
    if (ai::botdiag::IsActionLogEnabled())
    {
        WriteBotCam(this, bot);
        CorrectBotZ(bot);   // snap bots that are stuck UNDER the terrain back to ground (logged)
    }

    // ABANDON UNREACHABLE LOOT: if we're out of combat and have been chasing a loot corpse we cannot
    // path to (no NET movement for the window -- e.g. running in place against a fence), defer it so
    // we stop wasting time and pick something reachable. Only when still FAR from the corpse (>12yd)
    // so a bot actually standing on a corpse looting it is never interrupted. (See LootPursuitStuck.)
    if (!bot->IsInCombat() && !bot->GetVictim())
    {
        LootObject stuckLoot = aiObjectContext->GetValue<LootObject>("loot target")->Get();
        if (!stuckLoot.IsEmpty())
        {
            WorldObject* lwo = stuckLoot.GetWorldObject(bot);
            const float lootDist = lwo ? bot->GetDistance(lwo) : 999.0f;
            if (lootDist > 12.0f && LootPursuitStuck(bot, stuckLoot.guid))
            {
                DeferLootGuidForBot(bot, stuckLoot.guid, 60);   // blacklist this unreachable corpse 60s
                if (LootObjectStack* avail = aiObjectContext->GetValue<LootObjectStack*>("available loot")->Get())
                    avail->Remove(stuckLoot.guid);
                aiObjectContext->GetValue<LootObject>("loot target")->Reset();
            }
        }
    }

    // LEARNING: sample (state, action) for combat decisions. RecordCombatDecision
    // self-gates (in-combat only, ~1/sec/bot) so this is cheap. Joined to the fight's
    // reward (fightId) -> (state, action, reward) tuples for rotation-policy learning.
    if (actionExecuted)
    {
        const Action* lastExec = GetLastExecutedAction(currentState);
        if (lastExec)
            sBotLearningMgr.RecordCombatDecision(this, const_cast<Action*>(lastExec)->getName());
    }

    if (!bot->IsInWorld()) //Teleport out of bg
        return;

    if (minimal)
    {
        bool isAutonomousRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot) && !HasRealPlayerMaster();

        if (!isAutonomousRandomBot && !MovementAction::MinimalMove(this) && !bot->isAFK() && !bot->InBattleGround() && !HasRealPlayerMaster())
            bot->ToggleAFK();
        else if (isAutonomousRandomBot && bot->isAFK())
            bot->ToggleAFK();

        SetAIInternalUpdateDelay(sPlayerbotAIConfig.passiveDelay);
        return;
    }
    else if (bot->isAFK())
        bot->ToggleAFK();


    Group *group = bot->GetGroup();

    //Remove bot masters not in our group.
    if (master && master != bot && !HasActivePlayerMaster() && (!group || group->GetLeaderGuid() != master->GetObjectGuid()))
    {
        master = IsRealPlayer() ? bot : nullptr;
        SetMaster(master);
        ResetStrategies();
    }

    // test BG master set
    if ((!master || !HasActivePlayerMaster()) && group && !IsRealPlayer())
    {
        //Ideally we want to have the leader as master.
        Player* newMaster = GetGroupMaster();
        Player* playerMaster = nullptr;

        //Are there any non-bot players in the group?
        if (!newMaster || newMaster->GetPlayerbotAI())
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->getSource();

                if (!member)
                    continue;

                if (member == bot)
                    continue;

                if (member == newMaster)
                    continue;

                if (!member->IsInWorld())
                    continue;

                if (!member->IsInGroup(bot, true))
                    continue;

                //Do not make bots your master if they are nog group leader.
                if (member->GetPlayerbotAI() && !bot->InBattleGround())
                    continue;

                if (bot->InBattleGround())
                    continue;

                // same BG
                if (bot->InBattleGround() && bot->GetBattleGround()->GetTypeId() == BATTLEGROUND_AV && !member->GetPlayerbotAI() && member->InBattleGround() && bot->GetMapId() == member->GetMapId())
                {
                    // TODO disable move to objective if have master in bg
                    continue;

                    if (!group->SameSubGroup(bot, member))
                        continue;

                    if (member->GetLevel() < bot->GetLevel())
                        continue;

                    // follow real player only if he has more honor/arena points
                    bool isArena = false;
#ifndef MANGOSBOT_ZERO
                    if (bot->GetBattleGround()->IsArena())
                        isArena = true;
#endif
                    if (isArena)
                    {
                        if (group->IsLeader(member->GetObjectGuid()))
                        {
                            playerMaster = member;
                            break;
                        }
                        else
                            continue;
                    }
                    else
                    {
#ifndef MANGOSBOT_ZERO
                        uint32 honorpts = member->GetHonorPoints();
                        if (bot->GetHonorPoints() && honorpts < bot->GetHonorPoints())
                            continue;
#else
                        if (bot->GetHonorRankInfo().rank >= member->GetHonorRankInfo().rank)
                            continue;
#endif
                    }

                    playerMaster = member;
                    continue;
                }

                newMaster = member;
                break;
            }

        if (!newMaster && playerMaster)
        {
            newMaster = playerMaster;
        }

        if (newMaster && (!master || master != newMaster) && bot != newMaster)
        {
            master = newMaster;
            SetMaster(newMaster);
            ResetStrategies();

            if (sRandomPlayerbotMgr.IsFreeBot(bot))
            {
                std::string defaultMovementStrategy = GetDefaultMovementStrategy();
                ChangeStrategy("+" + defaultMovementStrategy, BotState::BOT_STATE_NON_COMBAT);
            }

            if (GetMaster() == GetGroupMaster())
            {
                TellPlayer(master, BOT_TEXT("hello_follow"));
            }
            else
            {
                TellPlayer(master, BOT_TEXT("hello"));
            }
        }
    }

    // fix bots in BG not having proper strategies
#ifdef MANGOSBOT_ZERO
    if (bot->InBattleGround() && !HasStrategy("battleground", BotState::BOT_STATE_NON_COMBAT))
        ResetStrategies();
#else
    if ((bot->InBattleGround() && (!bot->IsBeingTeleported() && !bot->InArena()) && !HasStrategy("battleground", BotState::BOT_STATE_NON_COMBAT)) || ((!bot->IsBeingTeleported()&&bot->InArena()) && !HasStrategy("arena", BotState::BOT_STATE_NON_COMBAT)))
        ResetStrategies();
#endif

    if (master && master->IsInWorld())
	{
		if (master->m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE) && sServerFacade.GetDistance2d(bot, master) < 20.0f) bot->m_movementInfo.AddMovementFlag(MOVEFLAG_WALK_MODE);
		else bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);

        if (master->IsSitState() && aiInternalUpdateDelay < 1000)
        {
            if (!sServerFacade.isMoving(bot) && sServerFacade.GetDistance2d(bot, master) < 10.0f)
                bot->SetStandState(UNIT_STAND_STATE_SIT);
        }
        else if (aiInternalUpdateDelay < 1000)
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        if (!group && sRandomPlayerbotMgr.IsFreeBot(bot) && !IsRealPlayer())
        {
            bot->GetPlayerbotAI()->SetMaster(nullptr);
        }
	}
	else
    {
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);

        // Free bots should default back to run so travel/RPG movement cannot inherit a stale slow-walk state.
        if (bot->IsWalking() || !bot->HasUnitState(UNIT_STAT_RUNNING))
            bot->SetWalk(false, true);

        if ((aiInternalUpdateDelay < 1000) && bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
    }

#ifndef MANGOSBOT_ZERO
    if (bot->IsFlying() && !bot->IsTaxiFlying())
    {
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_FLYING);
#ifdef MANGOSBOT_ONE
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING2))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_FLYING2);
#endif
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_CAN_FLY))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_CAN_FLY);
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_LEVITATING))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_LEVITATING);
    }
#endif

    if (bot->IsTaxiFlying())
    {
        return;
    }
}

void PlayerbotAI::ReInitCurrentEngine()
{
    InterruptSpell();
    currentEngine->Init();
}

void PlayerbotAI::ChangeStrategy(const std::string& names, BotState type)
{
    if(type == BotState::BOT_STATE_ALL)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            Engine* engine = engines[i];
            if (engine)
            {
                engine->ChangeStrategy(names);
            }
        }
    }
    else
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            engine->ChangeStrategy(names);
        }
    }
}

void PlayerbotAI::PrintStrategies(Player* requester, BotState type)
{
    if (type == BotState::BOT_STATE_ALL)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            Engine* engine = engines[i];
            if (engine)
            {
                engine->PrintStrategies(requester, BotStateToString(BotState(i)));
            }
        }
    }
    else
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            engine->PrintStrategies(requester, BotStateToString(type));
        }
    }
}

void PlayerbotAI::ClearStrategies(BotState type)
{
    if (type == BotState::BOT_STATE_ALL)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            Engine* engine = engines[i];
            if (engine)
            {
                engine->removeAllStrategies();
            }
        }
    }
    else
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            engine->removeAllStrategies();
        }
    }
}

std::list<std::string_view> PlayerbotAI::GetStrategies(BotState type)
{
    // Can't get all strategies for all engines
    if (type != BotState::BOT_STATE_ALL)
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            return engine->GetStrategies();
        }
    }

    return std::list<std::string_view>();
}

bool PlayerbotAI::CanDoSpecificAction(const std::string& name, bool isUseful, bool isPossible)
{
    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        Engine* engine = engines[i];
        if(engine)
        {
            if(engine->CanExecuteAction(name, isUseful, isPossible))
            {
                return true;
            }
        }
    }

    return false;
}

bool PlayerbotAI::DoSpecificAction(const std::string& name, Event event, bool silent)
{
    Player* requester = event.getOwner();
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        Engine* engine = engines[i];
        if(engine)
        {
            ActionResult res = engine->ExecuteAction(name, event);
            switch (res)
            {
                case ACTION_RESULT_OK:
                {
                    if (!silent)
                    {
                        PlaySound(TEXTEMOTE_NOD);
                    }

                    return true;
                }

                case ACTION_RESULT_IMPOSSIBLE:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": impossible";
                        TellError(requester, out.str());
                        PlaySound(TEXTEMOTE_NO);
                    }

                    return false;
                }

                case ACTION_RESULT_USELESS:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": useless";
                        TellError(requester, out.str());
                        PlaySound(TEXTEMOTE_NO);
                    }

                    return false;
                }

                case ACTION_RESULT_FAILED:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": failed";
                        TellError(requester, out.str());
                    }

                    return false;
                }

                default:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": unknown action";
                        TellError(requester, out.str());
                    }

                    return false;
                }
            }
        }
        else
        {
            if (!silent)
            {
                std::ostringstream out;
                out << name << ": engine not ready";
                TellError(requester, out.str());
            }
        }
    }

    return false;
}

bool PlayerbotAI::PlaySound(uint32 emote)
{
    if (EmotesTextSoundEntry const* soundEntry = FindTextSoundEmoteFor(emote, bot->getRace(), bot->getGender()))
    {
        bot->PlayDistanceSound(soundEntry->SoundId);
        return true;
    }

    return false;
}

bool PlayerbotAI::PlayEmote(uint32 emote)
{
    WorldPacket data(SMSG_TEXT_EMOTE);
    data << (TextEmotes)emote;
    data << urand(0, EmoteAction::GetNumberOfEmoteVariants((TextEmotes)emote, bot->getRace(), bot->getGender()) - 1);
    data << ((master && (sServerFacade.GetDistance2d(bot, master) < 30.0f) && urand(0, 1)) ? master->GetObjectGuid() : (bot->GetSelectionGuid() && urand(0, 1)) ? bot->GetSelectionGuid() : ObjectGuid());
    bot->GetSession()->HandleTextEmoteOpcode(data);

    return false;
}

bool PlayerbotAI::ContainsStrategy(StrategyType type)
{
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        if(engines[i])
        {
            if (engines[i]->ContainsStrategy(type))
            {
                return true;
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasStrategy(const std::string& name, BotState type)
{
    // Can't check the strategy for all engines at once
    if(type != BotState::BOT_STATE_ALL)
    {
        const uint8 typeIndex = (uint8)type;
        if (engines[typeIndex])
        {
            return engines[typeIndex]->HasStrategy(name);
        }
    }

    return false;
}

void PlayerbotAI::ResetStrategies(bool autoLoad)
{
    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        engines[i]->initMode = true;
        engines[i]->removeAllStrategies();
    }

    AiFactory::AddDefaultCombatStrategies(bot, this, engines[(uint8)BotState::BOT_STATE_COMBAT]);
    AiFactory::AddDefaultNonCombatStrategies(bot, this, engines[(uint8)BotState::BOT_STATE_NON_COMBAT]);
    AiFactory::AddDefaultDeadStrategies(bot, this, engines[(uint8)BotState::BOT_STATE_DEAD]);
    AiFactory::AddDefaultReactionStrategies(bot, this, reactionEngine);
    if (autoLoad && HasPlayerRelation()) sPlayerbotDbStore.Load(this);

    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        engines[i]->initMode = false;
        engines[i]->Init();
    }
}

bool PlayerbotAI::IsRanged(Player* player, bool inGroup)
{
    PlayerbotAI* botAi = player->GetPlayerbotAI();
    if (botAi)
    {
        bool isRanged = botAi->ContainsStrategy(STRATEGY_TYPE_RANGED);
        if (isRanged)
            return true;

        if (inGroup)
        {
            const bool isExplicitMelee = botAi->ContainsStrategy(STRATEGY_TYPE_MELEE) || botAi->ContainsStrategy(STRATEGY_TYPE_TANK);
            if (isExplicitMelee)
                return false;

            switch (botAi->GetTalentSpec())
            {
                case PlayerTalentSpec::TALENT_SPEC_PALADIN_HOLY:
                case PlayerTalentSpec::TALENT_SPEC_HUNTER_BEAST_MASTERY:
                case PlayerTalentSpec::TALENT_SPEC_HUNTER_MARKSMANSHIP:
                case PlayerTalentSpec::TALENT_SPEC_HUNTER_SURVIVAL:
                case PlayerTalentSpec::TALENT_SPEC_PRIEST_DISCIPLINE:
                case PlayerTalentSpec::TALENT_SPEC_PRIEST_HOLY:
                case PlayerTalentSpec::TALENT_SPEC_PRIEST_SHADOW:
                case PlayerTalentSpec::TALENT_SPEC_SHAMAN_ELEMENTAL:
                case PlayerTalentSpec::TALENT_SPEC_SHAMAN_RESTORATION:
                case PlayerTalentSpec::TALENT_SPEC_MAGE_ARCANE:
                case PlayerTalentSpec::TALENT_SPEC_MAGE_FIRE:
                case PlayerTalentSpec::TALENT_SPEC_MAGE_FROST:
                case PlayerTalentSpec::TALENT_SPEC_WARLOCK_AFFLICTION:
                case PlayerTalentSpec::TALENT_SPEC_WARLOCK_DEMONOLOGY:
                case PlayerTalentSpec::TALENT_SPEC_WARLOCK_DESTRUCTION:
                case PlayerTalentSpec::TALENT_SPEC_DRUID_BALANCE:
                case PlayerTalentSpec::TALENT_SPEC_DRUID_RESTORATION:
                    return true;

                case PlayerTalentSpec::TALENT_SPEC_PALADIN_PROTECTION:
                case PlayerTalentSpec::TALENT_SPEC_PALADIN_RETRIBUTION:
                case PlayerTalentSpec::TALENT_SPEC_SHAMAN_ENHANCEMENT:
                case PlayerTalentSpec::TALENT_SPEC_DRUID_FERAL:
                    return false;

                default:
                    break;
            }
        }
    }

    switch (player->getClass())
    {
    case CLASS_PALADIN:
    case CLASS_WARRIOR:
    case CLASS_ROGUE:
#ifdef MANGOSBOT_TWO
    case CLASS_DEATH_KNIGHT:
#endif
        return false;
    case CLASS_DRUID:
        return !HasAnyAuraOf(player, "cat form", "bear form", "dire bear form", NULL);
    }
    return true;
}

bool PlayerbotAI::IsMelee(Player* player, bool inGroup)
{
    return !IsRanged(player, inGroup);
}

bool PlayerbotAI::IsTank(Player* player, bool inGroup)
{
    PlayerbotAI* botAi = player->GetPlayerbotAI();
    if (botAi)
    {
        bool isTank = botAi->ContainsStrategy(STRATEGY_TYPE_TANK);
        if (inGroup || isTank)
            return isTank;
    }

    BotRoles botRoles = AiFactory::GetPlayerRoles(player);

    return (botRoles & BOT_ROLE_TANK) != 0;
}

bool PlayerbotAI::IsHeal(Player* player, bool inGroup)
{
    PlayerbotAI* botAi = player->GetPlayerbotAI();
    if (botAi)
    {
        bool isHeal = botAi->ContainsStrategy(STRATEGY_TYPE_HEAL);
        if (inGroup || isHeal)
            return isHeal;
    }

    BotRoles botRoles = AiFactory::GetPlayerRoles(player);

    return (botRoles & BOT_ROLE_HEALER) != 0;
}

namespace MaNGOS
{

    class UnitByGuidInRangeCheck
    {
    public:
        UnitByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range) : i_obj(obj), i_range(range), i_guid(guid) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(Unit* u)
        {
            return u->GetObjectGuid() == i_guid && i_obj->IsWithinDistInMap(u, i_range);
        }
    private:
        WorldObject const* i_obj;
        float i_range;
        ObjectGuid i_guid;
    };

    class GameObjectByGuidInRangeCheck
    {
    public:
        GameObjectByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range) : i_obj(obj), i_range(range), i_guid(guid) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(GameObject* u)
        {
            if (u && i_obj->IsWithinDistInMap(u, i_range) && sServerFacade.isSpawned(u) && u->GetGOInfo() && u->GetObjectGuid() == i_guid)
                return true;

            return false;
        }
    private:
        WorldObject const* i_obj;
        float i_range;
        ObjectGuid i_guid;
    };

};


Unit* PlayerbotAI::GetUnit(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return sObjectAccessor.GetUnit(*bot, guid);
}

Unit* PlayerbotAI::GetUnit(CreatureDataPair const* creatureDataPair)
{
    if (!creatureDataPair)
        return NULL;

    ObjectGuid guid(HIGHGUID_UNIT, creatureDataPair->second.creature_id[0], creatureDataPair->first);

    if (!guid)
        return NULL;

    Map* map = sMapMgr.FindMap(creatureDataPair->second.position.mapid);

    if (!map)
        return NULL;

    return map->GetUnit(guid);
}


Creature* PlayerbotAI::GetCreature(ObjectGuid guid) const
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return map->GetCreature(guid);
}

Creature* PlayerbotAI::GetAnyTypeCreature(ObjectGuid guid) const
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return map->GetAnyTypeCreature(guid);
}

GameObject* PlayerbotAI::GetGameObject(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return map->GetGameObject(guid);
}

GameObject* PlayerbotAI::GetGameObject(GameObjectDataPair const* gameObjectDataPair)
{
    if (!gameObjectDataPair)
        return NULL;

    ObjectGuid guid(HIGHGUID_GAMEOBJECT, gameObjectDataPair->second.id, gameObjectDataPair->first);

    if (!guid)
        return NULL;

    Map* map = sMapMgr.FindMap(gameObjectDataPair->second.position.mapid);

    if (!map)
        return NULL;

    return map->GetGameObject(guid);
}

WorldObject* PlayerbotAI::GetWorldObject(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return map->GetWorldObject(guid);
}

std::vector<Player*> PlayerbotAI::GetPlayersInGroup()
{
    std::vector<Player*> members;

    Group* group = bot->GetGroup();

    if (!group)
        return members;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();

        if (member->GetPlayerbotAI() && !member->GetPlayerbotAI()->IsRealPlayer())
            continue;

        members.push_back(ref->getSource());
    }

    return members;
}

void PlayerbotAI::DropQuest(uint32 questIdToDrop)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (questId == questIdToDrop)
        {
            bot->SetQuestSlot(slot, 0);

            //We ignore unequippable quest items in this case, its' still be equipped
            bot->TakeQuestSourceItem(questId, false);

            bot->SetQuestStatus(questId, QUEST_STATUS_NONE);
            bot->getQuestStatusMap()[questId].m_rewarded = false;

            // Clean the quest's collected items out of the bags (the old TODO): dropped/finished
            // quests otherwise leave orphaned quest items cluttering inventory forever. Only destroy
            // ITEM_CLASS_QUEST items that NO other still-held quest requires, so we never break a
            // shared quest item.
            if (Quest const* dropped = sObjectMgr.GetQuestTemplate(questId))
            {
                for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    uint32 reqItem = dropped->ReqItemId[i];
                    if (!reqItem)
                        continue;

                    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(reqItem);
                    if (!proto || proto->Class != ITEM_CLASS_QUEST)
                        continue;   // only purge quest-bound items; leave normal/usable items alone

                    bool neededElsewhere = false;
                    for (uint16 s2 = 0; s2 < MAX_QUEST_LOG_SIZE && !neededElsewhere; ++s2)
                    {
                        uint32 otherId = bot->GetQuestSlotQuestId(s2);
                        if (!otherId || otherId == questId)
                            continue;
                        if (Quest const* other = sObjectMgr.GetQuestTemplate(otherId))
                            for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
                                if (other->ReqItemId[j] == reqItem) { neededElsewhere = true; break; }
                    }

                    if (!neededElsewhere)
                    {
                        uint32 have = bot->GetItemCount(reqItem, true);
                        if (have)
                            bot->DestroyItemCount(reqItem, have, true, false);
                    }
                }
            }

            return;
        }
    }
}

std::vector<const Quest*> PlayerbotAI::GetAllCurrentQuests()
{
    std::vector<const Quest*> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        result.push_back(sObjectMgr.GetQuestTemplate(questId));
    }

    return result;
}

std::vector<const Quest*> PlayerbotAI::GetCurrentIncompleteQuests()
{
    std::vector<const Quest*> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
        {
            result.push_back(sObjectMgr.GetQuestTemplate(questId));
        }
    }

    return result;
}

std::set<uint32> PlayerbotAI::GetAllCurrentQuestIds()
{
    std::set<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        result.insert(questId);
    }

    return result;
}

std::set<uint32> PlayerbotAI::GetCurrentIncompleteQuestIds()
{
    std::set<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
        {
            result.insert(questId);
        }
    }

    return result;
}

const Quest* PlayerbotAI::GetCurrentIncompleteQuestWithId(uint32 pQuestId)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (pQuestId == questId && status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            return sObjectMgr.GetQuestTemplate(questId);
    }

    return nullptr;
}

bool PlayerbotAI::HasCurrentIncompleteQuestWithId(uint32 pQuestId)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (pQuestId == questId && status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            return true;
    }

    return false;
}

/*
* @return vector of pair<quest, count>
*/
std::vector<std::pair<const Quest*, uint32>> PlayerbotAI::GetCurrentQuestsRequiringItemId(uint32 itemId)
{
    std::vector<std::pair<const Quest*, uint32>> result;

    if (!itemId)
    {
        return result;
    }

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        const Quest* quest = sObjectMgr.GetQuestTemplate(questId);
        for (uint8 i = 0; i < std::size(quest->ReqItemId); ++i)
        {
            if (quest->ReqItemId[i] == itemId)
            {
                result.push_back(std::pair(quest, quest->ReqItemCount[i]));
                break;
            }
        }
    }

    return result;
}

const AreaTableEntry* PlayerbotAI::GetCurrentArea()
{
    return GetAreaEntryByAreaID(sServerFacade.GetAreaId(bot));
}

const AreaTableEntry* PlayerbotAI::GetCurrentZone()
{
    return GetAreaEntryByAreaID(sTerrainMgr.GetZoneId(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
}

/*
* @return localized area_name
*/
std::string PlayerbotAI::GetLocalizedAreaName(const AreaTableEntry* entry)
{
    // Penqle's area_name is single char* (no locale array).
    return entry && entry->area_name ? std::string(entry->area_name) : std::string();
}

bool PlayerbotAI::IsInCapitalCity()
{
    AreaTableEntry const* current_area = GetAreaEntryByAreaID(sServerFacade.GetAreaId(bot));
    if (!current_area)
    {
        return false;
    }
    return current_area->flags & AREA_FLAG_CAPITAL;
}

ChatChannelSource PlayerbotAI::GetChatChannelSource(Player* bot, uint32 type, std::string channelName)
{
    if (type == CHAT_MSG_CHANNEL)
    {
        if (channelName == "World")
        {
            return ChatChannelSource::SRC_WORLD;
        }
        else
        {
            ChannelMgr* cMgr = channelMgr(bot->GetTeam());
            if (!cMgr)
            {
                return ChatChannelSource::SRC_UNDEFINED;
            }

            const Channel* channel = cMgr->GetChannel(channelName, bot);

            if (channel)
            {
                switch (channel->GetChannelId())
                {
                case ChatChannelId::GENERAL:
                {
                    return ChatChannelSource::SRC_GENERAL;
                }
                case ChatChannelId::TRADE:
                {
                    return ChatChannelSource::SRC_TRADE;
                }
                case ChatChannelId::LOCAL_DEFENSE:
                {
                    return ChatChannelSource::SRC_LOCAL_DEFENSE;
                }
                case ChatChannelId::WORLD_DEFENSE:
                {
                    return ChatChannelSource::SRC_WORLD_DEFENSE;
                }
                case ChatChannelId::LOOKING_FOR_GROUP:
                {
                    return ChatChannelSource::SRC_LOOKING_FOR_GROUP;
                }
                case ChatChannelId::GUILD_RECRUITMENT:
                {
                    return ChatChannelSource::SRC_GUILD_RECRUITMENT;
                }
                default:
                    return ChatChannelSource::SRC_UNDEFINED;
                }
            }
        }
    }
    else
    {
        switch (type)
        {
        case CHAT_MSG_WHISPER:
        {
            return ChatChannelSource::SRC_WHISPER;
        }
        case CHAT_MSG_SAY:
        {
            return ChatChannelSource::SRC_SAY;
        }
        case CHAT_MSG_YELL:
        {
            return ChatChannelSource::SRC_YELL;
        }
        case CHAT_MSG_GUILD:
        {
            return ChatChannelSource::SRC_GUILD;
        }
        case CHAT_MSG_PARTY:
#ifdef MANGOSBOT_TWO
        case CHAT_MSG_PARTY_LEADER:
#endif
        {
            return ChatChannelSource::SRC_PARTY;
        }
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        {
            return ChatChannelSource::SRC_RAID;
        }
        case CHAT_MSG_EMOTE:
        {
            return ChatChannelSource::SRC_EMOTE;
        }
        case CHAT_MSG_TEXT_EMOTE:
        {
            return ChatChannelSource::SRC_TEXT_EMOTE;
        }
        default:
            return ChatChannelSource::SRC_UNDEFINED;
        }
    }

    return ChatChannelSource::SRC_UNDEFINED;
}

bool PlayerbotAI::SayToGuild(std::string msg, bool likePlayer)
{
    if (msg.empty())
    {
        return false;
    }

    if (bot->GetGuildId())
    {
        if (Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId()))
        {

            auto guildSnap = sRandomPlayerbotMgr.GetRealPlayerSnapshot();
            for (auto const& player : guildSnap->players)
            {
                if (player.guildId == bot->GetGuildId())
                {
                    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
                        sPlayerbotAIConfig.llmBotToBotChatChance))
                    {
                        WorldPacket packet_template(CMSG_MESSAGECHAT);

                        uint32 lang = LANG_UNIVERSAL;

                        packet_template << CHAT_MSG_GUILD;
                        packet_template << LANG_UNIVERSAL;
                        packet_template << msg;

                        std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

                        bot->GetSession()->QueuePacket(std::move(packetPtr));
                        return true;
                    }
                    break;
                }
            }
            if (!guild->HasRankRight(bot->GetRank(), GR_RIGHT_GCHATSPEAK))
            {
                return false;
            }

            guild->BroadcastToGuild(bot->GetSession(), msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToWorld(std::string msg)
{
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    //no zone
    if (Channel* worldChannel = cMgr->GetChannel("World", bot))
    {
        worldChannel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
        return true;
    }

    return false;
}

bool PlayerbotAI::SayToGeneral(std::string msg)
{
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    AreaTableEntry const* current_zone = GetCurrentZone();
    if (!current_zone)
    {
        return false;
    }

    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone
        if (channel && channel->GetChannelId() == ChatChannelId::GENERAL
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(current_zone)))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToTrade(std::string msg)
{
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    //can only be used in major cities (bugged in TBC, players only join City versions if they logout/login within a city)
    if (!IsInCapitalCity())
    {
        return false;
    }

#ifndef MANGOSBOT_ZERO
    //Workaround for TBC - just reroute to General chat (if bot is in city)
    //Trade and GuildRecruitment channels are broken in TBC
    //Currently in magons TBC, if you switch zones, then you join "Trade - <zone>" and "GuildRecruitment - <zone>"
    //which is a core bug, should be "Trade - City" and "GuildRecruitment - City" in both 1.12 and TBC
    //but if you (actual player) logout in a city and log back in - you join "City" versions
    return SayToGeneral(msg);
#else
    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        if (channel && channel->GetChannelId() == ChatChannelId::TRADE
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(GetAreaEntryByAreaID(ImportantAreaId::CITY))))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
#endif
}

bool PlayerbotAI::SayToLFG(std::string msg)
{
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

#ifndef MANGOSBOT_ZERO
    //Workaround for TBC - just reroute to General chat
    //LFG requires to be joined in LFG queue, so no point in using it
    return SayToGeneral(msg);
#else
    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone
        if (channel && channel->GetChannelId() == ChatChannelId::LOOKING_FOR_GROUP)
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
#endif
}

bool PlayerbotAI::SayToLocalDefense(std::string msg)
{
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    AreaTableEntry const* current_zone = GetCurrentZone();
    if (!current_zone)
    {
        return false;
    }

    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone
        if (channel && channel->GetChannelId() == ChatChannelId::LOCAL_DEFENSE
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(current_zone)))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToWorldDefense(std::string msg)
{
#ifdef MANGOSBOT_ZERO
    //check if 11 honor rank
    if (bot->GetHonorRankInfo().rank < 11)
    {
        return false;
    }
#endif
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        if (channel && channel->GetChannelId() == ChatChannelId::WORLD_DEFENSE)
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToGuildRecruitment(std::string msg)
{
    //check for bot's level? level 60?
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    //can only be used in major cities
    if (!IsInCapitalCity())
    {
        return false;
    }

#ifndef MANGOSBOT_ZERO
    //Workaround for TBC - just reroute to General chat (if bot is in city)
    //Trade and GuildRecruitment channels are broken in TBC
    //Currently in magons TBC, if you switch zones, then you join "Trade - <zone>" and "GuildRecruitment - <zone>"
    //which is a core bug, should be "Trade - City" and "GuildRecruitment - City" in both 1.12 and TBC
    //but if you (actual player) logout in a city and log back in - you join "City" versions
    return SayToGeneral(msg);
#else
    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone (can only be used in major cities)
        if (channel && channel->GetChannelId() == ChatChannelId::GUILD_RECRUITMENT
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(GetAreaEntryByAreaID(ImportantAreaId::CITY))))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
#endif
}

bool PlayerbotAI::SayToParty(std::string msg, bool likePlayer)
{
    if (!bot->GetGroup())
    {
        return false;
    }

    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
        sPlayerbotAIConfig.llmBotToBotChatChance))
    {
        for (auto reciever : GetPlayersInGroup())
        {
            if (likePlayer || reciever->isRealPlayer())
            {
                WorldPacket packet_template(CMSG_MESSAGECHAT);

                uint32 lang = LANG_UNIVERSAL;

                packet_template << CHAT_MSG_PARTY;
                packet_template << LANG_UNIVERSAL;
                packet_template << msg;

                std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

                bot->GetSession()->QueuePacket(std::move(packetPtr));
                return true;
            }
        }
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, msg.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());

    for (auto reciever : GetPlayersInGroup())
    {
        sServerFacade.SendPacket(reciever, data);
    }

    return true;
}

bool PlayerbotAI::SayToRaid(std::string msg)
{
    if (!bot->GetGroup() || !bot->GetGroup()->IsRaidGroup())
    {
        return false;
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID, msg.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());

    for (auto reciever : GetPlayersInGroup())
    {
        sServerFacade.SendPacket(reciever, data);
    }

    return true;
}

bool PlayerbotAI::Yell(std::string msg, bool likePlayer)
{
    uint32 lang = LANG_UNIVERSAL;
    if (bot->GetTeam() == ALLIANCE)
    {
        lang =  LANG_COMMON;
    }
    else
    {
        lang = LANG_ORCISH;
    }

    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
        sPlayerbotAIConfig.llmBotToBotChatChance))
    {
        if (likePlayer || this->HasPlayerNearby(sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL)))
        {
            WorldPacket packet_template(CMSG_MESSAGECHAT);

            packet_template << CHAT_MSG_YELL;
            packet_template << lang;
            packet_template << msg;

            std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

            bot->GetSession()->QueuePacket(std::move(packetPtr));
            return true;
        }
    }

    bot->Yell(msg, lang);

    return true;
}

bool PlayerbotAI::Say(std::string msg, bool likePlayer)
{
    uint32 lang = LANG_UNIVERSAL;
    if (bot->GetTeam() == ALLIANCE)
    {
        lang =  LANG_COMMON;
    }
    else
    {
        lang = LANG_ORCISH;
    }

    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
        sPlayerbotAIConfig.llmBotToBotChatChance))
    {
        if (likePlayer || this->HasPlayerNearby(sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_SAY)))
        {

            WorldPacket packet_template(CMSG_MESSAGECHAT);

            packet_template << CHAT_MSG_SAY;
            packet_template << lang;
            packet_template << msg;

            std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

            bot->GetSession()->QueuePacket(std::move(packetPtr));
            return true;
        }
    }

    bot->Say(msg, lang);

    return true;
}

bool PlayerbotAI::Whisper(std::string msg, std::string receiverName, bool likePlayer)
{
    ObjectGuid receiver = sObjectMgr.GetPlayerGuidByName(receiverName);
    Player* rPlayer = sObjectMgr.GetPlayer(receiver);

    if (!rPlayer)
    {
        return false;
    }

    if (rPlayer == bot)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, msg.c_str(), LANG_UNIVERSAL, bot->GetChatTag(), bot->GetObjectGuid(), bot->GetName());
        rPlayer->GetSession()->SendPacket(data);

        return true;
    }

    if (bot->GetTeam() == ALLIANCE)
    {
        bot->Whisper(msg, LANG_COMMON, receiver);
    }
    else
    {
        bot->Whisper(msg, LANG_ORCISH, receiver);
    }

    return true;
}

bool PlayerbotAI::TellPlayerNoFacing(Player* player, std::string text, PlayerbotSecurityLevel securityLevel, bool isPrivate, bool noRepeat, bool ignoreSilent)
{
    if(!player)
        return false;

    if (!ignoreSilent && HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT))
        return false;

    time_t lastSaid = whispers[text];
    if (!noRepeat || !lastSaid || (time(0) - lastSaid) >= sPlayerbotAIConfig.repeatDelay / 1000)
    {
        whispers[text] = time(0);

        if (m_recordMessages)
        {
            m_recordedMessages.push_back(text);
        }

        std::vector<Player*> recievers;

        ChatMsg type = CHAT_MSG_SYSTEM;

        if (!isPrivate && bot->GetGroup())
        {
            recievers = GetPlayersInGroup();
            if(!recievers.empty())
            {
                type = bot->GetGroup()->IsRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
            }
        }

        if (type == CHAT_MSG_SYSTEM && HasRealPlayerMaster())
            type = CHAT_MSG_WHISPER;

        if (type == CHAT_MSG_SYSTEM && (sPlayerbotAIConfig.randomBotSayWithoutMaster || HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT)))
            type = CHAT_MSG_SAY;

        if (type == CHAT_MSG_SYSTEM && player->isRealPlayer())
            type = CHAT_MSG_WHISPER;

        if ((sPlayerbotAIConfig.hasLog("chat_log.csv") && HasStrategy("debug log", BotState::BOT_STATE_NON_COMBAT)) || HasStrategy("debug logname", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
            out << bot->GetName() << ",";
            out << std::fixed << std::setprecision(2);

            out << std::to_string(bot->getRace()) << ",";
            out << std::to_string(bot->getClass()) << ",";
            float subLevel = GetLevelFloat();

            out << subLevel << ",";
            out << std::fixed << std::setprecision(2);
            WorldPosition(bot).printWKT(out);

            out << type << ",";
            out << text;

            if (HasStrategy("debug logname", BotState::BOT_STATE_NON_COMBAT))
            {
                std::string fileName = "chat_log_";
                fileName += bot->GetName();
                fileName += ".csv";
                if (!sPlayerbotAIConfig.isLogOpen(fileName))
                    sPlayerbotAIConfig.openLog(fileName, "a", true);
                sPlayerbotAIConfig.log(fileName, out.str().c_str());
            }
            else
                sPlayerbotAIConfig.log("chat_log.csv", out.str().c_str());
        }

        WorldPacket data;

        switch (type)
        {
            case CHAT_MSG_SAY:
            {
                bot->Say(text, PlayerbotChatLanguage(bot));
                return true;
            }

            case CHAT_MSG_RAID:
            {
                this->SayToRaid(text.c_str());

                return true;
            }
            case CHAT_MSG_PARTY:
            {
                SayToParty(text.c_str());

                return true;
            }

            case CHAT_MSG_WHISPER:
            {
                if (!IsTellAllowed(player, securityLevel))
                    return false;

                if (!HasRealPlayerMaster() && !player->isRealPlayer())
                    return false;

                whispers[text] = time(0);

                if (currentChat.second >= time(0))
                   type = currentChat.first;

                if (type == CHAT_MSG_ADDON)
                {
                    text = "BOT\t" + text;

                    ChatHandler::BuildChatPacket(data, type == CHAT_MSG_ADDON ? CHAT_MSG_PARTY : type, text.c_str(), type == CHAT_MSG_ADDON ? LANG_ADDON : LANG_UNIVERSAL, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());
                    sServerFacade.SendPacket(player, data);
                    return true;
                }

                this->Whisper(text, player->GetName());
                return true;
            }

            default:
                return true;
        }
    }

    return true;
}

bool PlayerbotAI::TellError(Player* player, std::string text, PlayerbotSecurityLevel securityLevel, bool ignoreSilent)
{
    if (!IsTellAllowed(player, securityLevel) || !IsSafe(player) || player->GetPlayerbotAI())
        return false;

    if (!ignoreSilent && HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT))
        return false;

    PlayerbotMgr* mgr = player->GetPlayerbotMgr();
    if (mgr) mgr->TellError(bot->GetName(), text);

    return false;
}

bool PlayerbotAI::IsTellAllowed(Player* player, PlayerbotSecurityLevel securityLevel)
{
    if (!player || player->IsBeingTeleported())
        return false;

    if (!GetSecurity()->CheckLevelFor(securityLevel, true, player))
        return false;

    if (sPlayerbotAIConfig.whisperDistance && !bot->GetGroup() && sRandomPlayerbotMgr.IsFreeBot(bot) &&
            player->GetSession()->GetSecurity() < SEC_GAMEMASTER &&
            (bot->GetMapId() != player->GetMapId() || sServerFacade.GetDistance2d(bot, player) > sPlayerbotAIConfig.whisperDistance))
        return false;

    return true;
}

bool PlayerbotAI::TellPlayer(Player* player, std::string text, PlayerbotSecurityLevel securityLevel, bool isPrivate, bool ignoreSilent)
{
    if (!TellPlayerNoFacing(player, text, securityLevel, isPrivate, ignoreSilent))
        return false;

    if (player && !player->IsBeingTeleported() && !sServerFacade.isMoving(bot) && !sServerFacade.IsInCombat(bot) && bot->GetMapId() == player->GetMapId() && !bot->IsTaxiFlying() && !bot->IsFlying())
    {
        if (!sServerFacade.IsInFront(bot, player, sPlayerbotAIConfig.sightDistance, EMOTE_ANGLE_IN_FRONT))
            sServerFacade.SetFacingTo(bot, player);

        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
    }

    return true;
}

bool IsRealAura(Player* bot, Aura const* aura, Unit* unit)
{
    if (!aura)
        return false;

    if (!sServerFacade.IsHostileTo(unit, bot))
        return true;

    uint32 stacks = aura->GetStackAmount();
    if (stacks >= aura->GetSpellProto()->StackAmount)
        return true;

    if (aura->GetCaster() == bot || IsPositiveSpell(aura->GetSpellProto()) || aura->IsAreaAura())
        return true;

    return false;
}

bool PlayerbotAI::HasAura(std::string name, Unit* unit, bool maxStack, bool checkIsOwner, int maxAuraAmount, bool hasMyAura, int minDuration, int auraTypeId)
{
    if (!unit)
        return false;

    std::wstring wnamepart;

    std::vector<uint32> ids = chatHelper.SpellIds(name);

    if (ids.empty())
    {
        //sLog.outError("Please add %s to spell list", name.c_str());
        if (!Utf8toWStr(name, wnamepart))
            return 0;

        wstrToLower(wnamepart);
    }

    int auraAmount = 0;

	for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
	{
        if (auraTypeId != TOTAL_AURAS && auraType != auraTypeId)
            continue;

		Unit::AuraList const& auras = unit->GetAurasByType((AuraType)auraType);

        if (auras.empty())
            continue;

        for (Unit::AuraList::const_iterator i = auras.begin(); i != auras.end(); i++)
        {
            Aura* aura = *i;
            if (!aura)
                continue;

            if (ids.empty())
            {
                const std::string auraName = aura->GetSpellProto()->SpellName[0];
                if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
                    continue;
            }
            else
            {
                if (std::find(ids.begin(), ids.end(), aura->GetSpellProto()->Id) == ids.end())
                    continue;
            }

			if (IsRealAura(bot, aura, unit))
            {
                if (hasMyAura && aura->GetHolder())
                {
                    if (aura->GetHolder()->GetCasterGuid() == bot->GetObjectGuid())
                        return true;
                    else
                        continue;
                }

                if (checkIsOwner && aura->GetHolder())
                {
                    if (aura->GetHolder()->GetCasterGuid() != bot->GetObjectGuid())
                        continue;
                }

                uint32 maxStackAmount = aura->GetSpellProto()->StackAmount;
                uint32 maxProcCharges = aura->GetSpellProto()->procCharges;

                if (maxStack)
                {
                    if (maxStackAmount && aura->GetStackAmount() >= maxStackAmount)
                        auraAmount++;

                    if (maxProcCharges && aura->GetHolder()->GetAuraCharges() >= maxProcCharges)
                        auraAmount++;
                }
                else
                    auraAmount++;

                bool minDurationPassed = true;

                if (minDuration > 0)
                {
                    int32 auraDuration = aura->GetHolder()->GetAuraDuration();
                    minDurationPassed = minDuration <= auraDuration;
                }

                if (maxAuraAmount < 0 && minDurationPassed)
                    return auraAmount > 0;
            }
		}
    }

    if (maxAuraAmount >= 0)
    {
        return auraAmount == maxAuraAmount || (auraAmount > 0 && auraAmount <= maxAuraAmount);
    }

    return false;
}

bool PlayerbotAI::HasAura(uint32 spellId, Unit* unit, bool checkOwner)
{
    Aura* aura = GetAura(spellId, unit, checkOwner);
    return aura != nullptr;
}

Aura* PlayerbotAI::GetAura(uint32 spellId, Unit* unit, bool checkIsOwner)
{
    Aura* aura = nullptr;
    if (spellId != 0 && unit)
    {
        for (uint32 effect = EFFECT_INDEX_0; effect <= EFFECT_INDEX_2; effect++)
        {
            Aura* auraTmp = ((Unit*)unit)->GetAura(spellId, (SpellEffectIndex)effect);
            if (IsRealAura(bot, auraTmp, (Unit*)unit))
            {
                if (checkIsOwner)
                {
                    if (aura->GetHolder() && aura->GetHolder()->GetCasterGuid() == bot->GetObjectGuid())
                    {
                        aura = auraTmp;
                        break;
                    }
                }
                else
                {
                    aura = auraTmp;
                    break;
                }
            }
        }
    }

    return aura;
}

Aura* PlayerbotAI::GetAura(std::string name, Unit* unit, bool checkIsOwner)
{
    if (!name.empty() && unit)
    {
        std::wstring wnamepart;
        std::vector<uint32> ids = chatHelper.SpellIds(name);

        if (ids.empty())
        {
            //sLog.outError("Please add %s to spell list", name.c_str());
            if (!Utf8toWStr(name, wnamepart))
                return 0;

            wstrToLower(wnamepart);
        }

        for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
        {
            Unit::AuraList const& auras = unit->GetAurasByType((AuraType)auraType);

            if (auras.empty())
                continue;

            for (Unit::AuraList::const_iterator i = auras.begin(); i != auras.end(); i++)
            {
                Aura* aura = *i;
                if (!aura)
                    continue;

                if (ids.empty())
                {
                    const std::string auraName = aura->GetSpellProto()->SpellName[0];
                    if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
                        continue;
                }
                else
                {
                    if (std::find(ids.begin(), ids.end(), aura->GetSpellProto()->Id) == ids.end())
                        continue;
                }

                if (IsRealAura(bot, aura, unit))
                {
                    if (checkIsOwner && aura->GetHolder())
                    {
                        if (aura->GetHolder()->GetCasterGuid() == bot->GetObjectGuid())
                        {
                            return aura;
                        }
                    }
                    else
                    {
                        return aura;
                    }
                }
            }
        }
    }

    return nullptr;
}

std::vector<Aura*> PlayerbotAI::GetAuras(Unit* unit, bool allAuras, bool positive)
{
    std::vector<Aura*> outAuras;
    for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
    {
        Unit::AuraList const& auras = unit->GetAurasByType((AuraType)auraType);

        if (auras.empty())
            continue;

        for (Unit::AuraList::const_iterator i = auras.begin(); i != auras.end(); i++)
        {
            Aura* aura = *i;
            if (aura)
            {
                if (allAuras || (positive && aura->IsPositive()) || (!positive && !aura->IsPositive()))
                {
                    outAuras.push_back(aura);
                }
            }
        }
    }

    return outAuras;
}

bool PlayerbotAI::HasAnyAuraOf(Unit* player, ...)
{
    if (!player)
        return false;

    va_list vl;
    va_start(vl, player);

    const char* cur;
    do {
        cur = va_arg(vl, const char*);
        if (cur && HasAura(cur, player)) {
            va_end(vl);
            return true;
        }
    }
    while (cur);

    va_end(vl);
    return false;
}

bool PlayerbotAI::GetSpellRange(std::string name, float* maxRange, float* minRange)
{
    // Copied from Spell::GetMinMaxRange
    const uint32 spellId = aiObjectContext->GetValue<uint32>("spell id", name)->Get();
    if(spellId)
    {
        const SpellEntry* spellInfo = sServerFacade.LookupSpellInfo(spellId);
        if (spellInfo)
        {
            const SpellRangeEntry* spellRangeEntry = sServerFacade.LookupSpellRangeEntry(spellInfo->rangeIndex);
            if(spellRangeEntry)
            {
                float spellMinRange = 0.0f, spellMaxRange = 0.0f, rangeMod = 0.0f;
                if (spellRangeEntry->Flags & SPELL_RANGE_FLAG_MELEE)
                {
                    rangeMod = bot->GetCombinedCombatReach(bot, true, 0.f);
                }
                else
                {
                    float meleeRange = 0.0f;
                    if (spellRangeEntry->Flags & SPELL_RANGE_FLAG_RANGED)
                    {
                        meleeRange = bot->GetCombinedCombatReach(bot, true, 0.f);
                    }

                    spellMinRange = spellRangeEntry->minRange + meleeRange;
                    spellMaxRange = spellRangeEntry->maxRange;
                }

                if (spellInfo->HasAttribute(SPELL_ATTR_USES_RANGED_SLOT))
                {
                    if (Item* rangedWeapon = bot->GetWeaponForAttack(RANGED_ATTACK))
                    {
                        spellMaxRange *= rangedWeapon->GetProto()->RangedModRange * 0.01f;
                    }
                }

                if (Player* modOwner = bot->GetSpellModOwner())
                {
                    modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_RANGE, spellMaxRange);
                }

                spellMaxRange += rangeMod;

                if(maxRange)
                {
                    *maxRange = spellMaxRange;
                }

                if(minRange)
                {
                    *minRange = spellMinRange;
                }

                return true;
            }
        }
    }

    return false;
}

uint32 PlayerbotAI::GetSpellCastDuration(Spell* spell)
{
    uint32 spellDuration = 0;
    if(spell)
    {
        const SpellEntry* const pSpellInfo = spell->m_spellInfo;
        spellDuration = spell->GetCastTime();
        if (IsChanneledSpell(pSpellInfo))
        {
            int32 duration = GetSpellDuration(pSpellInfo);
            if (duration > 0)
            {
                spellDuration += duration;
            }
        }

        spellDuration = ceil(spellDuration);

        // fix Feign Death
        if (pSpellInfo->Id == 5384)
        {
            spellDuration = 1000;
        }
        // fix cannibalize
        else if (pSpellInfo->Id == 20577)
        {
            spellDuration = 10000;
        }

        uint32 globalCooldown = CalculateGlobalCooldown(pSpellInfo->Id);
        if (spellDuration < (int32)globalCooldown)
        {
            spellDuration = globalCooldown;
        }

        // Projectile (traveling) spells -- Fireball, Frostbolt, Shoot, etc. -- deal
        // their damage only AFTER the missile reaches the target. The core schedules
        // delivery at floor(dist / speed * 1000) ms (Spell.cpp), but GetCastTime()
        // does NOT include that travel time. Without accounting for it the bot's next
        // cast fires ~reactDelay (100ms) after the cast bar ends -- i.e. while the
        // previous missile is still in flight and BEFORE its damage lands -- and the
        // engagement restarts before the hit registers. The damage is lost: the bot
        // burns mana casting into a mob that stays at full HP. This is invisible
        // against HOSTILE mobs (they aggro the bot on sight, so combat starts without
        // needing the bot's damage to land) but FATAL against NEUTRAL mobs, which can
        // ONLY enter combat from the bot's own damage -- measured 82% combat-start
        // failure for ranged bots vs 1% for melee (instant) bots on the same mobs.
        // Waiting out the missile travel lets the hit land and start combat first,
        // exactly like the instant melee swing that succeeds 99% of the time.
        if (pSpellInfo->speed > 0.0f && bot)
        {
            if (Unit* unitTarget = spell->m_targets.getUnitTarget())
            {
                float dist = bot->GetDistance(unitTarget);
                spellDuration += (uint32) floor(dist / pSpellInfo->speed * 1000.0f);
            }
        }
    }

    return spellDuration + sPlayerbotAIConfig.reactDelay;
}

bool PlayerbotAI::HasSpell(std::string name) const
{
    return HasSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get());
}

bool PlayerbotAI::HasSpell(uint32 spellid) const
{
    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid))
    {
        return true;
    }
    else if (bot->HasSpell(spellid))
    {
        return true;
    }

    return false;
}

bool PlayerbotAI::CanCastSpell(std::string name, Unit* target, uint8 effectMask, Item* itemTarget, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    return CanCastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target, 0, true, itemTarget, ignoreRange, ignoreInCombat, ignoreMount, checkResult);
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, Unit* target, uint8 effectMask, bool checkHasSpell, Item* itemTarget, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    if (!spellid)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid) && pet->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_CAST_OK;
        }

        return true;
    }

    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        // Spells that can be casted while out of control
        const std::list<uint32> ignoreOutOfControllSpells = { 642, 1020, 1499, 1953, 7744, 11958, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        if (std::find(ignoreOutOfControllSpells.begin(), ignoreOutOfControllSpells.end(), spellid) == ignoreOutOfControllSpells.end())
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_NOT_IN_CONTROL;
            }

            return false;
        }
    }

    if (!target)
        target = bot;

    if (checkHasSpell && !HasSpell(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    if (!bot->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_READY;
        }

        return false;
    }

	SpellEntry const *spellInfo = sServerFacade.LookupSpellInfo(spellid);
	if (!spellInfo)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    // already active next melee swing spell
    if (IsNextMeleeSwingSpell(spellInfo))
    {
        Spell* autorepeatSpell = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
        if (autorepeatSpell)
        {
            if (autorepeatSpell->m_spellInfo->Id == spellInfo->Id)
            {
                if (checkResult)
                {
                    *checkResult = SPELL_FAILED_SPELL_IN_PROGRESS;
                }

                return false;
            }
        }
    }

	if (!itemTarget)
	{
        // Consider neutral spells (spells that are neither positive or negative (e.g. feign death, hunter traps, ...)
        const std::list<uint32> neutralSpells = { 1499, 5384, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        const bool neutralSpell = std::find(neutralSpells.begin(), neutralSpells.end(), spellid) != neutralSpells.end();
        if(!neutralSpell)
        {
            const bool positiveSpell = IsPositiveSpell(spellInfo);
            if (positiveSpell && sServerFacade.IsHostileTo(bot, target))
            {
                if (checkResult)
                {
                    *checkResult = SPELL_FAILED_TARGET_ENEMY;
                }

                return false;
            }

            if (!positiveSpell && sServerFacade.IsFriendlyTo(bot, target))
            {
                if (checkResult)
                {
                    *checkResult = SPELL_FAILED_TARGET_FRIENDLY;
                }
                
                return false;
            }
        }

        bool damage = false;
        for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
        {
            // direct damage
            if (spellInfo->Effect[(SpellEffectIndex)i] == SPELL_EFFECT_SCHOOL_DAMAGE)
            {
                damage = true;
                break;
            }
            // periodic damage
            if (spellInfo->Effect[(SpellEffectIndex)i] == SPELL_EFFECT_APPLY_AURA && spellInfo->EffectBasePoints[i] >= 0)
            {
                if (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE)
                {
                    damage = true;
                    break;
                }
            }
        }

        if (!damage)
        {
            for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
            {
                bool immune = target->IsImmuneToSpellEffect(spellInfo, (SpellEffectIndex)i, false);
                if (immune)
                {
                    if (checkResult)
                    {
                        *checkResult = SPELL_FAILED_IMMUNE;
                    }

                    return false;
                }
            }
        }

        if (!ignoreRange && bot != target && sServerFacade.GetDistance2d(bot, target) > sPlayerbotAIConfig.sightDistance)
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_OUT_OF_RANGE;
            }

            return false;
        }
	}

	ObjectGuid oldSel = bot->GetSelectionGuid();
	//bot->SetSelectionGuid(target->GetObjectGuid());
	Spell *spell = new Spell(bot, spellInfo, false);

    spell->m_targets.setUnitTarget(target);
    spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get());
    spell->m_targets.setItemTarget(spell->GetCastItem());

    SpellCastResult result = spell->CheckCast(true);
    delete spell;
	//if (oldSel)
	//	bot->SetSelectionGuid(oldSel);

    if (checkResult)
    {
        *checkResult = result;
    }

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_LINE_OF_SIGHT:
            return ignoreRange;
        case SPELL_FAILED_AFFECTING_COMBAT:
            return ignoreInCombat;
        case SPELL_FAILED_NOT_MOUNTED:
            return ignoreMount;
        default:
            return false;
    }
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, GameObject* goTarget, uint8 effectMask, bool checkHasSpell, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    if (!spellid)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid) && pet->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_CAST_OK;
        }

        return true;
    }

    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        // Spells that can be casted while out of control
        const std::list<uint32> ignoreOutOfControllSpells = { 642, 1020, 1499, 1953, 7744, 11958, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        if (std::find(ignoreOutOfControllSpells.begin(), ignoreOutOfControllSpells.end(), spellid) == ignoreOutOfControllSpells.end())
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_NOT_IN_CONTROL;
            }

            return false;
        }
    }

    if (checkHasSpell && !HasSpell(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    if (!bot->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_READY;
        }

        return false;
    }

    SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellid);
    if (!spellInfo)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    bool damage = false;
    for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
    {
        if (spellInfo->Effect[(SpellEffectIndex)i] == SPELL_EFFECT_SCHOOL_DAMAGE)
        {
            damage = true;
            break;
        }
    }

    if (sServerFacade.GetDistance2d(bot, goTarget) > sPlayerbotAIConfig.sightDistance)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_OUT_OF_RANGE;
        }

        return false;
    }

    //ObjectGuid oldSel = bot->GetSelectionGuid();
    bot->SetSelectionGuid(goTarget->GetObjectGuid());
    Spell* spell = new Spell(bot, spellInfo, false);

    spell->m_targets.setGOTarget(goTarget);
    spell->SetCastItem(aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get());
    spell->m_targets.setItemTarget(spell->GetCastItem());

    SpellCastResult result = spell->CheckCast(true);
    delete spell;
    //if (oldSel)
    //    bot->SetSelectionGuid(oldSel);

    if (checkResult)
    {
        *checkResult = result;
    }

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        case SPELL_FAILED_OUT_OF_RANGE:
            return ignoreRange;
        case SPELL_FAILED_AFFECTING_COMBAT:
            return ignoreInCombat;
        case SPELL_FAILED_NOT_MOUNTED:
            return ignoreMount;
        default:
            return false;
    }
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, float x, float y, float z, uint8 effectMask, bool checkHasSpell, Item* itemTarget, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    if (!spellid)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid) && pet->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_CAST_OK;
        }

        return true;
    }

    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        // Spells that can be casted while out of control
        const std::list<uint32> ignoreOutOfControllSpells = { 642, 1020, 1499, 1953, 7744, 11958, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        if (std::find(ignoreOutOfControllSpells.begin(), ignoreOutOfControllSpells.end(), spellid) == ignoreOutOfControllSpells.end())
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_NOT_IN_CONTROL;
            }

            return false;
        }
    }

    if (checkHasSpell && !HasSpell(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellid);
    if (!spellInfo)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    if (!itemTarget)
    {
        if (sqrt(bot->GetDistance(x,y,z)) > sPlayerbotAIConfig.sightDistance)
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_OUT_OF_RANGE;
            }

            return false;
        }
    }

    Spell* spell = new Spell(bot, spellInfo, false);

    spell->m_targets.setDestination(x, y, z);
    spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get());
    spell->m_targets.setItemTarget(spell->GetCastItem());

    SpellCastResult result = spell->CheckCast(true);
    delete spell;

    if (checkResult)
    {
        *checkResult = result;
    }

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        case SPELL_FAILED_OUT_OF_RANGE:
            return ignoreRange;
        case SPELL_FAILED_AFFECTING_COMBAT:
            return ignoreInCombat;
        case SPELL_FAILED_NOT_MOUNTED:
            return ignoreMount;
        default:
            return false;
    }
}

uint8 PlayerbotAI::GetHealthPercent(const Unit& target) const
{
   return (static_cast<float>(target.GetHealth()) / target.GetMaxHealth()) * 100;
}

uint8 PlayerbotAI::GetHealthPercent() const
{
   return GetHealthPercent(*bot);
}

uint8 PlayerbotAI::GetManaPercent(const Unit& target) const
{
   return (static_cast<float>(target.GetPower(POWER_MANA)) / target.GetMaxPower(POWER_MANA)) * 100;
}

uint8 PlayerbotAI::GetManaPercent() const
{
   return GetManaPercent(*bot);
}

bool PlayerbotAI::CastSpell(std::string name, Unit* target, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    bool result = CastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target, itemTarget, waitForSpell, outSpellDuration);
    if (result)
    {
        aiObjectContext->GetValue<time_t>("last spell cast time", name)->Set(time(0));
    }

    return result;
}

bool PlayerbotAI::CastSpell(uint32 spellId, Unit* target, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    if (!spellId)
        return false;

    if (!target)
        target = bot;

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellId))
    {
        return CastPetSpell(spellId, target);
    }

    // HEALER DISCIPLINE (dungeon/raid/BG groups): a healer-role bot in instanced content with
    // a group NEVER casts offensive spells -- every offensive global is mana not available for
    // the next heal, and dungeon wipes trace back to healers smiting at 40% mana. Utility and
    // heals (positive spells) pass through untouched.
    if (IsInstancedContent() && bot->GetGroup() && target && target != bot && !bot->IsFriendlyTo(target))
    {
        if (AiFactory::GetPlayerRoles(bot) & BOT_ROLE_HEALER)
        {
            const SpellEntry* offCheck = sServerFacade.LookupSpellInfo(spellId);
            if (offCheck && !IsPositiveSpell(offCheck))
                return false;
        }
    }

    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
    aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    MotionMaster &mm = *bot->GetMotionMaster();

    if (bot->IsFlying() || bot->IsTaxiFlying())
        return false;

    const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!pSpellInfo)
        return false;

	//bot->clearUnitState(UNIT_STAT_CHASE);
	//bot->clearUnitState(UNIT_STAT_FOLLOW);

	bool failWithDelay = false;
    SpellCastResult setupFailure = SPELL_CAST_OK;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "spell=" << pSpellInfo->SpellName[0]
                << " target=" << (target ? target->GetName() : "self")
                << " immediateAttempt=1";
            if (target)
                out << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target);
            sPlayerbotAIConfig.logEvent(this, "StandForCast", std::to_string(spellId), out.str());
        }
    }

	ObjectGuid oldSel = bot->GetSelectionGuid();
	bot->SetSelectionGuid(target->GetObjectGuid());

    WorldObject* faceTo = target;
    if (!sServerFacade.IsInFront(bot, faceTo, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
    {
        sServerFacade.SetFacingTo(bot, faceTo, true);
        bot->SetInFront(target);
        if (!sServerFacade.IsInFront(bot, faceTo, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT) && !HasRealPlayerMaster())
        {
            failWithDelay = true;
            if (setupFailure == SPELL_CAST_OK)
                setupFailure = SPELL_FAILED_NOT_INFRONT;
        }
    }

    if (failWithDelay)
    {
        SetAIInternalUpdateDelay(waitForSpell ? sPlayerbotAIConfig.globalCoolDown : 250);

        if(outSpellDuration)
        {
            *outSpellDuration = waitForSpell ? sPlayerbotAIConfig.globalCoolDown : 250;
        }

        LogSpellCastFailure(this, bot, target, pSpellInfo, setupFailure, "setup", false, target == bot && IsPositiveSpell(pSpellInfo));
        return false;
    }

    // THRASH INSTRUMENTATION (phase 1): the cast is about to happen (passed setup/facing).
    // Count it, detect the bot interrupting its OWN in-progress cast, and detect re-casting
    // the same heal in a tight window. Read-only of bot state + per-bot timestamps; cheap.
    {
        extern std::atomic<uint64_t> g_botCasts;
        extern std::atomic<uint64_t> g_botCastInterrupts;
        extern std::atomic<uint64_t> g_botHealCasts;
        extern std::atomic<uint64_t> g_botHealRecasts;
        extern std::atomic<uint64_t> g_botHealRecastTight;
        g_botCasts.fetch_add(1, std::memory_order_relaxed);
        if (Spell* cur = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            if (cur->getState() == SPELL_STATE_CASTING && cur->m_spellInfo && cur->m_spellInfo->Id != spellId)
                g_botCastInterrupts.fetch_add(1, std::memory_order_relaxed);
        }
        if (IsHealSpell(pSpellInfo))
        {
            const uint32 nowMs = WorldTimer::getMSTime();
            g_botHealCasts.fetch_add(1, std::memory_order_relaxed);
            if (thrashLastHealSpellId == spellId && thrashLastHealCastMs)
            {
                const uint32 sinceMs = nowMs - thrashLastHealCastMs;
                if (sinceMs < 4000)
                    g_botHealRecasts.fetch_add(1, std::memory_order_relaxed);
                // CAST-TIME-ACCURATE cancellation signal: re-casting the SAME heal faster than that
                // heal's own cast time means the previous identical heal could not have completed ->
                // it was CANCELED (not a completed-then-recast). castMs==0 (instant heals) can't be
                // canceled, so they're correctly excluded. A high tight_pct here is unambiguous.
                const uint32 castMs = GetSpellCastTime(pSpellInfo, bot);
                if (castMs > 0 && sinceMs < castMs)
                {
                    g_botHealRecastTight.fetch_add(1, std::memory_order_relaxed);
                    // TRACE the cancellation context (sampled 1/25) -> logs/heal_cancel.csv. The
                    // key field is lastAction: what the bot DID between the two heal casts (i.e.
                    // what canceled the first). genType = active motion generator. This pinpoints
                    // the mechanism (movement/chase vs something else) instead of guessing.
                    static std::atomic<uint32> sampCtr{0};
                    if ((sampCtr.fetch_add(1, std::memory_order_relaxed) % 25) == 0)
                    {
                        FILE* hf = fopen("logs/heal_cancel.csv", "a");
                        if (!hf) hf = fopen("../logs/heal_cancel.csv", "a");
                        if (hf)
                        {
                            fprintf(hf, "%s,lvl=%u,cls=%u,spell=%u,sinceMs=%u,castMs=%u,moving=%d,genType=%u,lastAction=%s,nonMeleeCast=%d,inCombat=%d,hp=%u\n",
                                bot->GetName(), bot->GetLevel(), bot->getClass(), spellId, sinceMs, castMs,
                                (sServerFacade.isMoving(bot) || bot->isMovingOrTurning()) ? 1 : 0,
                                bot->GetMotionMaster() ? (uint32)bot->GetMotionMaster()->GetCurrentMovementGeneratorType() : 999u,
                                actionMetricLastActionName.empty() ? "none" : actionMetricLastActionName.c_str(),
                                bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0,
                                bot->IsInCombat() ? 1 : 0, (uint32)bot->GetHealthPercent());
                            fclose(hf);
                        }
                    }
                }
            }
            thrashLastHealSpellId = spellId;
            thrashLastHealCastMs = nowMs;
        }
    }

    Spell *spell = new Spell(bot, pSpellInfo, false);

    SpellCastTargets targets;
    if ((pSpellInfo->Targets & TARGET_FLAG_ITEM) || spellId == 1804)
    {
        spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get());
        targets.setItemTarget(spell->GetCastItem());

        if (bot->GetTradeData())
        {
            bot->GetTradeData()->SetSpell(spellId);
			delete spell;
            return true;
        }
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        if (aoe.coord_x != 0)
            targets.setDestination(aoe.coord_x, aoe.coord_y, aoe.coord_z);
        else if (target && target->GetObjectGuid() != bot->GetObjectGuid())
            targets.setDestination(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setDestination(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
    else
    {
        targets.setUnitTarget(target);
    }

    if (spellId == 1953) // simulate blink coordinates
    {
        float angle = bot->GetOrientation();
        float distance = 20.0f;
        float fx = bot->GetPositionX() + cos(angle) * distance;
        float fy = bot->GetPositionY() + sin(angle) * distance;
        float fz = bot->GetPositionZ();

        float ox, oy, oz;
        bot->GetPosition(ox, oy, oz);
//#ifdef MANGOSBOT_TWO
//        bot->GetMap()->GetHitPosition(ox, oy, oz + max_height, fx, fy, fz, bot->GetPhaseMask(), -0.5f);
//#else
//        bot->GetMap()->GetHitPosition(ox, oy, oz + 2.0f, fx, fy, fz, -0.5f);
//#endif
        bot->UpdateAllowedPositionZ(fx, fy, fz);
        targets.setDestination(fx, fy, fz);
    }

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
        GameObject* go = GetGameObject(loot.guid);
        if (go && sServerFacade.isSpawned(go))
        {
            targets.setGOTarget(go);
            faceTo = go;
        }
        else
        {
            Unit* creature = GetUnit(loot.guid);
            if (creature)
            {
                targets.setUnitTarget(creature);
                faceTo = creature;
            }
        }
    }

    const bool castTimeSpell = (GetSpellCastTime(pSpellInfo, bot, spell) > 0) ||
        (IsChanneledSpell(pSpellInfo) && (GetSpellDuration(pSpellInfo) > 0));
    const bool selfPositiveCast = target == bot && IsPositiveSpell(pSpellInfo);

    if (Spell* activeSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        const uint32 activeRemaining = activeSpell->GetCastedTime();
        uint32 activeDelay = GetSpellInProgressRecheckDelay(activeRemaining);

        SetAIInternalUpdateDelay(activeDelay);
        if (outSpellDuration)
            *outSpellDuration = activeDelay;

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "spell=" << pSpellInfo->SpellName[0]
                << " activeSpell=" << (activeSpell->m_spellInfo ? activeSpell->m_spellInfo->SpellName[0] : "unknown")
                << " target=" << (target ? target->GetName() : "self")
                << " castTime=" << (castTimeSpell ? 1 : 0)
                << " activeRemainingMs=" << activeRemaining
                << " deferMs=" << activeDelay
                << " selfPositive=" << (selfPositiveCast ? 1 : 0)
                << " moving=" << (sServerFacade.isMoving(bot) ? 1 : 0)
                << " combat=" << (bot->IsInCombat() ? 1 : 0);
            if (target)
                out << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target);
            sPlayerbotAIConfig.logEvent(this, "SpellCastDeferredInProgress", std::to_string(spellId), out.str());
        }

        spell->cancel();
        return true;
    }

    // Fail the cast if the bot is moving and the spell is a casting/channeled spell.
    // Self-heals are the exception: autonomous bots should plant their feet and
    // continue the cast instead of repeatedly stopping, failing, and resuming combat movement.
    //
    // We must also clear an ACTIVE movement generator (e.g. a ChaseMovementGenerator left
    // behind by combat reach) even when the bot is momentarily not moving at this instant.
    // Otherwise that generator keeps repositioning the bot a fraction of a second into the
    // cast and cancels it -- so the spell never lands, never deals damage, never generates
    // threat, and the creature (empty threat list) instantly evades and heals back to full
    // (measured: stuck casts have botThreat=0/threatEmpty=1; working casts have botThreat>0).
    // This is the dominant cause of "bot fireballs a mob, it resets to full HP, bot OOMs".
    const bool hasActiveMovementGen =
        bot->GetMotionMaster() &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != HOME_MOTION_TYPE;
    if ((sServerFacade.isMoving(bot) || hasActiveMovementGen) && castTimeSpell)
    {
        // always fail when jumping
        if (IsJumping() || bot->IsFalling())
        {
            SetAIInternalUpdateDelay(waitForSpell ? sPlayerbotAIConfig.globalCoolDown : 250);
            if (outSpellDuration)
                *outSpellDuration = waitForSpell ? sPlayerbotAIConfig.globalCoolDown : 250;
            LogSpellCastFailure(this, bot, target, pSpellInfo, SPELL_FAILED_MOVING, "moving-airborne", castTimeSpell, selfPositiveCast);
            spell->cancel();
            return false;
        }

        StopMoving();
        if (target && target != bot)
        {
            sServerFacade.SetFacingTo(bot, target, true);
            bot->SetInFront(target);
        }

        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "spell=" << pSpellInfo->SpellName[0]
                << " target=" << (target ? target->GetName() : "self")
                << " selfPositive=" << (selfPositiveCast ? 1 : 0)
                << " movingStopped=1"
                << " immediateAttempt=1";
            if (target)
                out << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target);
            sPlayerbotAIConfig.logEvent(this, selfPositiveCast ? "SelfHealCastAfterStop" : "CombatCastAfterStop",
                std::to_string(spellId), out.str());
        }
    }

    for (uint32 j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        uint8 slot = 0;
        switch (pSpellInfo->Effect[j])
        {
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT1: slot = 0; break;
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT2: slot = 1; break;
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT3: slot = 2; break;
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT4: slot = 3; break;
        default: continue;
        }

        if (ObjectGuid guid = bot->m_ObjectSlotGuid[slot])
        {
            if (GameObject* obj = bot ? bot->GetMap()->GetGameObject(guid) : nullptr)
            {
                //Object is not mine because I created an object with same guid on different map. 
                //Make object mine, remove it from my list and give it back to the original owner.
                if (obj->GetOwnerGuid() != bot->GetObjectGuid())
                {
                    ObjectGuid ownerGuid = obj->GetOwnerGuid();
                    obj->SetOwnerGuid(bot->GetObjectGuid());
                    obj->SetLootState(GO_JUST_DEACTIVATED);

                    bot->RemoveGameObject(obj, false, pSpellInfo->Id != obj->GetSpellId());
                    bot->m_ObjectSlotGuid[slot].Clear();

                    obj->SetOwnerGuid(ownerGuid);
                }
            }
        }
    }


    SpellCastResult spellSuccess = spell->SpellStart(&targets);

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        if (!spell->m_targets.getItemTarget())
        {
            LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
            if (!loot.IsLootPossible(bot))
            {
                spell->cancel();
                //delete spell;
                return false;
            }
        }
    }

    if (spellSuccess != SPELL_CAST_OK)
    {
        if (spellSuccess == SPELL_FAILED_SPELL_IN_PROGRESS)
        {
            uint32 activeRemaining = 0;
            std::string activeSpellName = "unknown";
            for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
            {
                Spell* current = bot->GetCurrentSpell(static_cast<CurrentSpellTypes>(i));
                if (!current || current->getState() == SPELL_STATE_FINISHED)
                    continue;

                if (current->m_spellInfo)
                    activeSpellName = current->m_spellInfo->SpellName[0];

                uint32 currentDelay = current->GetCastedTime();
                activeRemaining = std::max(activeRemaining, currentDelay);
            }

            uint32 activeDelay = GetSpellInProgressRecheckDelay(activeRemaining);

            SetAIInternalUpdateDelay(activeDelay);
            if (outSpellDuration)
                *outSpellDuration = activeDelay;

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                out << "spell=" << pSpellInfo->SpellName[0]
                    << " activeSpell=" << activeSpellName
                    << " target=" << (target ? target->GetName() : "self")
                    << " castTime=" << (castTimeSpell ? 1 : 0)
                    << " activeRemainingMs=" << activeRemaining
                    << " deferMs=" << activeDelay
                    << " selfPositive=" << (selfPositiveCast ? 1 : 0)
                    << " moving=" << (sServerFacade.isMoving(bot) ? 1 : 0)
                    << " combat=" << (bot->IsInCombat() ? 1 : 0);
                if (target)
                    out << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target);
                sPlayerbotAIConfig.logEvent(this, "SpellCastDeferredInProgress", std::to_string(spellId), out.str());
            }

            return true;
        }

        if (castTimeSpell)
        {
            SetAIInternalUpdateDelay(waitForSpell ? sPlayerbotAIConfig.globalCoolDown : 250);
            if (outSpellDuration)
                *outSpellDuration = waitForSpell ? sPlayerbotAIConfig.globalCoolDown : 250;
        }

        LogSpellCastFailure(this, bot, target, pSpellInfo, spellSuccess, "spell-start", castTimeSpell, selfPositiveCast);
        return false;
    }

    // DO NOT artificially prime combat here. The previous code called
    // creature->EnterCombatWithTarget(bot) + bot->SetInCombatWith(creature) the instant
    // a hostile spell BEGAN casting, before any damage landed. That produced "telepathic"
    // combat: the creature aggroed and ran in with ZERO threat, then on its next AI tick
    // SelectHostileTarget found no real threat and EVADED (heals to 100%, drops combat) —
    // the exact bug seen in-game. Real combat must be established by the spell actually
    // hitting and dealing damage, identical to how a real player engages a mob.
    // Combat diagnostic: when a bot starts a hostile spell at a creature, capture the
    // target's REAL state so we can tell why a cast may not establish combat — is the
    // bot actually hostile to it (faction), is the creature evading (damage-immune), is
    // its HP dropping (is damage landing?), and is it in combat with the bot. Watching
    // targetHp across repeated casts at the same entry tells us evade vs faction vs
    // damage-not-landing without guessing.
    if (target && IsHostileCombatSpellForTarget(bot, target, pSpellInfo) && sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        Creature* diagCreature = target->ToCreature();
        std::ostringstream out;
        out << "spell=" << pSpellInfo->SpellName[0]
            << " target=" << target->GetName()
            << " entry=" << target->GetEntry()
            << " hostileTo=" << (bot->IsHostileTo(target) ? 1 : 0)
            << " targetHpPct=" << (diagCreature ? static_cast<int>(diagCreature->GetHealthPercent()) : -1)
            << " creatureFaction=" << (diagCreature ? diagCreature->GetFactionId() : 0u)
            << " evadeMode=" << ((diagCreature && diagCreature->IsInEvadeMode()) ? 1 : 0)
            // Creature-side evade detail: notReachMs is how long the creature has been
            // unable to path to its victim (>3000 => soft-evade + HP regen; >24000 =>
            // full evade home). evadeUnreach=1 means it is in the soft-evade/regen window
            // (the "takes damage then heals back to full" Mode-B loop). homeMotion=1 means
            // it is running back to spawn. combatResets counts how many times it has been
            // pulled-then-evaded -- a climbing value at the same creature = bot re-pulling
            // a mob it can never reach. This shows whether evading creatures RECOVER.
            << " notReachMs=" << (diagCreature ? diagCreature->m_TargetNotReachableTimer : 0u)
            << " evadeUnreach=" << ((diagCreature && diagCreature->IsEvadeBecauseTargetNotReachable()) ? 1 : 0)
            << " homeMotion=" << ((diagCreature && diagCreature->GetMotionMaster() && diagCreature->GetMotionMaster()->GetCurrentMovementGeneratorType() == HOME_MOTION_TYPE) ? 1 : 0)
            << " combatResets=" << (diagCreature ? diagCreature->GetCombatResetCount() : 0u)
            // Threat state: does our damage STICK as threat on the creature? botThreat is
            // this bot's current threat on the creature; threatEmpty=1 means the creature's
            // threat list is empty (=> on its next AI tick it has no victim and EVADES + heals
            // to full -- the "boar resets to full" bug). If we keep dealing damage but
            // botThreat stays 0 / threatEmpty stays 1, threat is not being retained.
            << " botThreat=" << (diagCreature ? static_cast<int>(sServerFacade.GetThreatManager(diagCreature).getThreat(bot)) : -1)
            << " threatEmpty=" << ((diagCreature && sServerFacade.GetThreatManager(diagCreature).isThreatListEmpty()) ? 1 : 0)
            << " targetInCombat=" << (target->IsInCombat() ? 1 : 0)
            << " targetVictimIsBot=" << ((target->GetVictim() == bot) ? 1 : 0)
            << " botInCombat=" << (bot->IsInCombat() ? 1 : 0)
            << " ranged=" << (IsRanged(bot) ? 1 : 0)
            << " dist=" << std::fixed << std::setprecision(2) << sServerFacade.GetDistance2d(bot, target)
            << " los=" << (bot->IsWithinLOSInMap(target, true) ? 1 : 0)
            // botFloat = how far the bot is ABOVE the ground (botZ - terrain/vmap height).
            // If the bot floats a few feet, a ground creature reaches the ground BELOW it but
            // can't melee-reach the floating bot (vertical gap) -> unreachable -> evade + heal.
            << " botFloat=" << std::fixed << std::setprecision(2)
            << (bot->GetMap() ? bot->GetPositionZ() - bot->GetMap()->GetHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) : 0.0f);
        sPlayerbotAIConfig.logEvent(this, "HostileCastDiag", std::to_string(target->GetEntry()), out.str());
    }

    PlayAttackEmote(6);

    if(waitForSpell)
    {
        WaitForSpellCast(spell);
    }

    if(outSpellDuration)
    {
        *outSpellDuration = GetSpellCastDuration(spell);
    }

    if (spell->GetCastTime() || (IsChanneledSpell(pSpellInfo) && GetSpellDuration(pSpellInfo) > 0))
        aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetObjectGuid(), time(0));

    aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (oldSel)
        bot->SetSelectionGuid(oldSel);

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " <<ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
}

bool PlayerbotAI::CastSpell(uint32 spellId, GameObject* goTarget, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    if (!spellId)
        return false;

    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
    aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    MotionMaster& mm = *bot->GetMotionMaster();

    if (bot->IsFlying() || bot->IsTaxiFlying())
        return false;

    //bot->clearUnitState(UNIT_STAT_CHASE);
    //bot->clearUnitState(UNIT_STAT_FOLLOW);

    bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

    WorldObject* faceTo = goTarget;
    if (!sServerFacade.IsInFront(bot, faceTo, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
    {
        sServerFacade.SetFacingTo(bot, faceTo);
        //failWithDelay = true;
    }

    if (failWithDelay)
    {
        if (waitForSpell)
        {
            SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        }

        if (outSpellDuration)
        {
            *outSpellDuration = sPlayerbotAIConfig.globalCoolDown;
        }

        return false;
    }

    const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    Spell* spell = new Spell(bot, pSpellInfo, false);

    SpellCastTargets targets;
    if (pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        if (aoe.coord_x != 0)
            targets.setDestination(aoe.coord_x, aoe.coord_y, aoe.coord_z);
        else if (goTarget && goTarget->GetObjectGuid() != bot->GetObjectGuid())
            targets.setDestination(goTarget->GetPositionX(), goTarget->GetPositionY(), goTarget->GetPositionZ());
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setDestination(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }

    targets.setGOTarget(goTarget);
    spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get());
    targets.setItemTarget(spell->GetCastItem());

    if (goTarget->GetGoType() == GAMEOBJECT_TYPE_CHEST)
    {
        auto context = GetAiObjectContext();
        AI_VALUE(LootObjectStack*, "available loot")->Add(goTarget->GetObjectGuid());
    }

    if (spellId == 1953) // simulate blink coordinates
    {
        float angle = bot->GetOrientation();
        float distance = 20.0f;
        float fx = bot->GetPositionX() + cos(angle) * distance;
        float fy = bot->GetPositionY() + sin(angle) * distance;
        float fz = bot->GetPositionZ();

        float ox, oy, oz;
        bot->GetPosition(ox, oy, oz);
        //#ifdef MANGOSBOT_TWO
        //        bot->GetMap()->GetHitPosition(ox, oy, oz + max_height, fx, fy, fz, bot->GetPhaseMask(), -0.5f);
        //#else
        //        bot->GetMap()->GetHitPosition(ox, oy, oz + 2.0f, fx, fy, fz, -0.5f);
        //#endif
        bot->UpdateAllowedPositionZ(fx, fy, fz);
        targets.setDestination(fx, fy, fz);
    }

    // Fail the cast if the bot is moving and the spell is a casting/channeled spell
    if (sServerFacade.isMoving(bot) && ((GetSpellCastTime(pSpellInfo, bot, spell) > 0) || (IsChanneledSpell(pSpellInfo) && (GetSpellDuration(pSpellInfo) > 0))))
    {
        // always fail when jumping
        if (IsJumping() || bot->IsFalling())
        {
            spell->cancel();
            return false;
        }

        StopMoving();

        // fail if not with real player to avoid movement glitches
        if (!HasActivePlayerMaster())
        {
            if (waitForSpell)
            {
                SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
            }

            spell->cancel();
            return false;
        }
    }

    SpellCastResult spellSuccess = spell->SpellStart(&targets);
    if (spellSuccess != SPELL_CAST_OK)
        return false;

    PlayAttackEmote(6);

    if (waitForSpell)
    {
        WaitForSpellCast(spell);
    }

    if (outSpellDuration)
    {
        *outSpellDuration = GetSpellCastDuration(spell);
    }

    if (spell->GetCastTime() || (IsChanneledSpell(pSpellInfo) && GetSpellDuration(pSpellInfo) > 0))
        aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, goTarget->GetObjectGuid(), time(0));

    aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " << ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
}

bool PlayerbotAI::CastSpell(uint32 spellId, float x, float y, float z, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    if (!spellId)
        return false;

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellId))
    {
        return CastPetSpell(spellId, nullptr);
    }

    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
    aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    MotionMaster& mm = *bot->GetMotionMaster();

    if (bot->IsFlying() || bot->IsTaxiFlying())
        return false;

    bot->clearUnitState(UNIT_STAT_CHASE);
    bot->clearUnitState(UNIT_STAT_FOLLOW);

    bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

    ObjectGuid oldSel = bot->GetSelectionGuid();

    if (!sServerFacade.isMoving(bot)) sServerFacade.SetFacingTo(bot, bot->GetAngleAt(bot->GetPositionX(), bot->GetPositionY(), x, y));

    if (failWithDelay)
    {
        if (waitForSpell)
        {
            SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        }

        if (outSpellDuration)
        {
            *outSpellDuration = sPlayerbotAIConfig.globalCoolDown;
        }

        return false;
    }

    const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    Spell* spell = new Spell(bot, pSpellInfo, false);

    SpellCastTargets targets;
    if ((pSpellInfo->Targets & TARGET_FLAG_ITEM) || spellId == 1804)
    {
        spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get());
        targets.setItemTarget(spell->GetCastItem());

        if (bot->GetTradeData())
        {
            bot->GetTradeData()->SetSpell(spellId);
            delete spell;
            return true;
        }
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        targets.setDestination(x, y, z);
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setDestination(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
    else
    {
        return false;
    }

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        return false;
    }

    // Fail the cast if the bot is moving and the spell is a casting/channeled spell
    if (sServerFacade.isMoving(bot) && ((GetSpellCastTime(pSpellInfo, bot, spell) > 0) || (IsChanneledSpell(pSpellInfo) && (GetSpellDuration(pSpellInfo) > 0))))
    {
        // always fail when jumping
        if (IsJumping() || bot->IsFalling())
        {
            spell->cancel();
            return false;
        }

        StopMoving();

        // fail if not with real player to avoid movement glitches
        if (!HasActivePlayerMaster())
        {
            if (waitForSpell)
            {
                SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
            }

            spell->cancel();
            return false;
        }
    }

    spell->SpellStart(&targets);

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        if (!spell->m_targets.getItemTarget())
        {
            LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
            if (!loot.IsLootPossible(bot))
            {
                spell->cancel();
                //delete spell;
                return false;
            }
        }
    }

    if (waitForSpell)
    {
        WaitForSpellCast(spell);
    }

    if (outSpellDuration)
    {
        *outSpellDuration = GetSpellCastDuration(spell);
    }

    aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, bot->GetObjectGuid(), time(0));
    aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (oldSel)
        bot->SetSelectionGuid(oldSel);

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " << ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
}

bool PlayerbotAI::CastPetSpell(uint32 spellId, Unit* target)
{
    Pet* pet = bot->GetPet();
    if (pet && spellId && pet->HasSpell(spellId))
    {
        auto IsAutocastActive = [&pet, &spellId]() -> bool
        {
            for (AutoSpellList::iterator i = pet->m_autospells.begin(); i != pet->m_autospells.end(); ++i)
            {
                if (*i == spellId)
                {
                    return true;
                }
            }

            return false;
        };

        // Send pet spell action packet
        uint8 flag = ACT_PASSIVE;
        if (IsAutocastable(spellId))
        {
            flag = IsAutocastActive() ? ACT_ENABLED : ACT_DISABLED;
        }

        uint32 command = (flag << 24) | spellId;

        WorldPacket data(CMSG_PET_ACTION);
        data << pet->GetObjectGuid();
        data << command;
        data << (target ? target->GetObjectGuid() : ObjectGuid());
        bot->GetSession()->HandlePetAction(data);
        return true;
    }

    return false;
}

bool PlayerbotAI::CanCastVehicleSpell(uint32 spellId, Unit* target)
{
#ifdef MANGOSBOT_TWO
    if (!spellId)
        return false;

    TransportInfo* transportInfo = bot->GetTransportInfo();
    if (!transportInfo || !transportInfo->IsOnVehicle())
        return false;

    Unit* vehicle = (Unit*)transportInfo->GetTransport();

    // do not allow if no spells
    VehicleSeatEntry const* seat = transportInfo ? vehicle->GetVehicleInfo()->GetSeatEntry(transportInfo->GetTransportSeat()) : nullptr;
    if (seat && !seat->HasFlag(SEAT_FLAG_CAN_CAST))
        return false;

    bool canControl = seat ? (seat->HasFlag(SEAT_FLAG_CAN_CONTROL)) : false;

    if (!vehicle)
        return false;

    Unit* spellTarget = target;

    if (!spellTarget)
        spellTarget = vehicle;

    if (!spellTarget)
        return false;

#ifdef MANGOS
    if (vehicle->HasSpellCooldown(spellId))
        return false;
#endif
#ifdef CMANGOS
    if (!vehicle->IsSpellReady(spellId))
        return false;
#endif

    SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!spellInfo)
        return false;

    // check BG siege position set in BG Tactics
    PositionEntry siegePos = GetAiObjectContext()->GetValue<ai::PositionMap&>("position")->Get()["bg siege"];

    // do not cast spell on self if spell is location based
    if (!(siegePos.isSet() || spellTarget != vehicle) && spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return false;

    uint32 CastingTime = !IsChanneledSpell(spellInfo) ? GetSpellCastTime(spellInfo, vehicle) : GetSpellDuration(spellInfo);

    if (CastingTime && vehicle->IsMoving())
        return false;

    if (vehicle != spellTarget && sServerFacade.GetDistance2d(vehicle, spellTarget) > 120.0f)
        return false;

    if (!target && siegePos.isSet())
    {
        if (sServerFacade.GetDistance2d(vehicle, siegePos.x, siegePos.y) > 120.0f)
            return false;
    }

    Spell* spell = new Spell(vehicle, spellInfo, false);

    WorldLocation dest;
    if (siegePos.isSet())
        dest = WorldLocation(bot->GetMapId(), siegePos.x, siegePos.y, siegePos.z, 0);
    else if (spellTarget != vehicle)
        dest = WorldLocation(spellTarget->GetMapId(), spellTarget->GetPosition());

    if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        spell->m_targets.setDestination(dest.coord_x, dest.coord_y, dest.coord_z);
    else if (spellTarget != vehicle)
    {
        spell->m_targets.setUnitTarget(spellTarget);
    }

    SpellCastResult result = spell->CheckCast(true);
    delete spell;

    switch (result)
    {
    case SPELL_FAILED_NOT_INFRONT:
    case SPELL_FAILED_NOT_STANDING:
    case SPELL_FAILED_UNIT_NOT_INFRONT:
    case SPELL_FAILED_MOVING:
    case SPELL_FAILED_TRY_AGAIN:
    case SPELL_CAST_OK:
        return true;
    default:
        return false;
    }
#endif
    return false;
}

bool PlayerbotAI::CastVehicleSpell(uint32 spellId, Unit* target, float projectileSpeed, bool needTurn)
{
#ifdef MANGOSBOT_TWO
    if (!spellId)
        return false;

    TransportInfo* transportInfo = bot->GetTransportInfo();
    if (!transportInfo || !transportInfo->IsOnVehicle())
        return false;

    Unit* vehicle = (Unit*)transportInfo->GetTransport();

    // do not allow if no spells
    VehicleSeatEntry const* seat = transportInfo ? vehicle->GetVehicleInfo()->GetSeatEntry(transportInfo->GetTransportSeat()) : nullptr;
    if (!seat->HasFlag(SEAT_FLAG_CAN_CAST))
        return false;

    bool canControl = seat ? (seat->HasFlag(SEAT_FLAG_CAN_CONTROL)) : false;
    bool canTurn = seat ? (seat->HasFlag(SEAT_FLAG_ALLOW_TURNING)) : false;

    if (!vehicle)
        return false;

    Unit* spellTarget = target;

    if (!spellTarget)
        spellTarget = vehicle;

    if (!spellTarget)
        return false;

    SpellEntry const* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);

    // check BG siege position set in BG Tactics
    PositionEntry siegePos = GetAiObjectContext()->GetValue<ai::PositionMap&>("position")->Get()["bg siege"];
    if (!target && siegePos.isSet())
    {
        if (sServerFacade.GetDistance2d(vehicle, siegePos.x, siegePos.y) > 120.0f)
            return false;
    }

    // do not cast spell on self if spell is location based
    if (!(siegePos.isSet() || spellTarget != vehicle) && pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return false;

    if (canControl)
    {
        //aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
        //aiObjectContext->GetValue<time_t>("stay time")->Set(0);
    }

    MotionMaster& mm = *vehicle->GetMotionMaster();

    //bot->clearUnitState(UNIT_STAT_CHASE);
    //bot->clearUnitState(UNIT_STAT_FOLLOW);

    //ObjectGuid oldSel = bot->GetSelectionGuid();
    //bot->SetSelectionGuid(target->GetObjectGuid());

    // turn vehicle if target is not in front
    bool failWithDelay = false;
    if (spellTarget != vehicle && (canControl || canTurn) && needTurn)
    {
        if (!sServerFacade.IsInFront(vehicle, spellTarget, 100.0f, CAST_ANGLE_IN_FRONT))
        {
            vehicle->SetFacingToObject(spellTarget);
            failWithDelay = true;
        }
    }

    if (siegePos.isSet() && (canControl || canTurn))
    {
        vehicle->SetFacingTo(vehicle->GetAngle(siegePos.x, siegePos.y));
    }

    if (failWithDelay)
    {
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        return false;
    }

    Spell* spell = new Spell(vehicle, pSpellInfo, false);

    SpellCastTargets targets;
    if ((spellTarget != vehicle || siegePos.isSet()) && pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation dest;
        if (spellTarget != vehicle)
            dest = WorldLocation(spellTarget->GetMapId(), spellTarget->GetPosition());
        else if (siegePos.isSet())
            dest = WorldLocation(bot->GetMapId(), siegePos.x + frand(-5.0f, 5.0f), siegePos.y + frand(-5.0f, 5.0f), siegePos.z, 0.0f);
        else
            return false;

        targets.setDestination(dest.coord_x, dest.coord_y, dest.coord_z);
        targets.setSpeed(projectileSpeed);
        float distanceToDest = sqrt(vehicle->GetPosition().GetDistance(Position(dest.coord_x, dest.coord_y, dest.coord_z, 0.0f)));
        float elev = 0.01f;
        if (distanceToDest < 25.0f)
            elev = 0.04f;
        else if (distanceToDest < 55.0f)
            elev = 0.22f;
        else if (distanceToDest < 85.0f)
            elev = 0.42f;
        else if (distanceToDest < 95.0f)
            elev = 0.70f;
        else if (distanceToDest < 110.0f)
            elev = 0.88f;
        else
            elev = 1.0f;

        targets.setElevation(elev);
    }
    if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setSource(vehicle->GetPositionX(), vehicle->GetPositionY(), vehicle->GetPositionZ());
    }

    if (target && !(pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION))
    {
        targets.setUnitTarget(spellTarget);
    }

#ifdef MANGOS
    spell->prepare(&targets);
#endif
#ifdef CMANGOS
    spell->SpellStart(&targets);
#endif

    if (canControl && !vehicle->IsStopped() && spell->GetCastTime())
    {
        vehicle->StopMoving();
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        spell->cancel();
        //delete spell;
        return false;
    }

    WaitForSpellCast(spell);

    //aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetObjectGuid(), time(0));
    //aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting Vehicle Spell" << ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
#endif
    return false;
}

bool PlayerbotAI::IsInVehicle(bool canControl, bool canCast, bool canAttack, bool canTurn, bool fixed, std::string vehicleName)
{
#ifdef MANGOSBOT_TWO
    TransportInfo* transportInfo = bot->GetTransportInfo();
    if (!transportInfo || !transportInfo->GetTransport() || !transportInfo->IsOnVehicle())
        return false;

    // get vehicle
    Unit* vehicle = (Unit*)transportInfo->GetTransport();
    if (!vehicle || !vehicle->IsAlive())
        return false;

    if (!vehicleName.empty())
    {
        std::wstring wnamepart;

        if (!Utf8toWStr(vehicleName, wnamepart))
            return 0;

        wstrToLower(wnamepart);
        char firstSymbol = tolower(vehicleName[0]);
        int spellLength = wnamepart.length();

        const char* name = vehicle->GetName();
        if (tolower(name[0]) != firstSymbol || strlen(name) != spellLength || !Utf8FitTo(name, wnamepart))
            return false;
    }

    if (!vehicle->GetVehicleInfo())
        return false;

    // get seat
    VehicleSeatEntry const* seat = vehicle->GetVehicleInfo()->GetSeatEntry(transportInfo->GetTransportSeat());
    if (!seat)
        return false;

    if (!(canControl || canCast || canAttack || canTurn || fixed))
        return true;

    if (canControl)
        return seat->HasFlag(SEAT_FLAG_CAN_CONTROL) && (vehicle->GetVehicleInfo()->GetVehicleEntry()->m_flags & VEHICLE_FLAG_FIXED_POSITION) == 0;

    if (canCast)
        return seat->HasFlag(SEAT_FLAG_CAN_CAST);

    if (canAttack)
        return seat->HasFlag(SEAT_FLAG_CAN_ATTACK);

    if (canTurn)
        return seat->HasFlag(SEAT_FLAG_ALLOW_TURNING);

    if (fixed)
        return vehicle->GetVehicleInfo()->GetVehicleEntry()->m_flags & VEHICLE_FLAG_FIXED_POSITION;

#endif
    return false;
}

void PlayerbotAI::WaitForSpellCast(Spell *spell)
{
    SetAIInternalUpdateDelay(GetSpellCastDuration(spell));
}

void PlayerbotAI::InterruptSpell(bool withMeleeAndAuto)
{
    for (int type = CURRENT_MELEE_SPELL; type < CURRENT_CHANNELED_SPELL; type++)
    {
        if (!withMeleeAndAuto)
        {
            if (type == CURRENT_MELEE_SPELL || type == CURRENT_AUTOREPEAT_SPELL)
                continue;
        }
        Spell* currentSpell = bot->GetCurrentSpell((CurrentSpellTypes)type);
        if (currentSpell && currentSpell->CanBeInterrupted())
        {
            // Protect an out-of-combat hostile PULL cast: a damaging generic spell aimed
            // at a creature while the bot is not yet in combat. The bot must hold still
            // and let it land so its damage flags the (often neutral) mob into combat --
            // exactly like a real player. The engine's frequent re-init / target-reselect
            // / reaction churn (ReInitCurrentEngine, SelectNewTargetAction, reactions, and
            // the movement path) was funneling through here and interrupting these casts
            // every ~0.3s, so for NEUTRAL mobs -- which can ONLY enter combat from the
            // bot's own damage -- the pull never landed: the mob stayed at full HP and the
            // bot looped, burned mana, OOM'd, and gave up (measured: ranged bots cast 40+
            // times with ZERO deliveries; melee, instant with no cast bar, succeeded 99%).
            // Once the hit lands the bot is in combat and this guard releases, so in-combat
            // interrupts (kick targets, repositioning, urgent reactions) are unaffected.
            if (type == CURRENT_GENERIC_SPELL && IsCastingHostilePull(currentSpell))
                continue;

            bot->InterruptSpell((CurrentSpellTypes)type);
            SpellInterrupted(currentSpell->m_spellInfo->Id);
        }
    }
}

// LOD COLD dormancy. When a bot has no real player in its zone/map it becomes invisible
// via Greater Invisibility (spell 16380, the same aura GM "gm visible off" uses) and drops
// combat, so it cannot aggro, fight, or die while its brain runs on the slow COLD interval.
// VISIBILITY_OFF removes it from the world's visibility/aggro scan (creatures can't see it)
// but it stays logged in (still in /who, still wandering). Reversed instantly when a player
// approaches and it promotes to HOT/WARM. Idempotent: only acts on the actual transition.
// THE INSTANCED FAST BRAIN, structurally: the open-world brain schedules its next thought
// per-action (0.5-2s action durations, react waits, strategy yields -- fine for 10k bots
// grinding). Inside a dungeon/raid/BG every one of those delays funnels through here and is
// clamped to InstanceReactDelay (10ms): the bot re-evaluates EVERY map cycle -- range checks,
// target swaps, interrupts, kiting all react at frame speed. Cheap because instanced bots are
// capped at 40-80. EXCEPTION: an in-flight cast keeps its full delay -- clamping mid-cast
// re-runs the engine which CANCELS the cast (the historical recast-loop bug).
void PlayerbotAI::SetAIInternalUpdateDelay(const uint32 delay)
{
    uint32 d = delay;
    // never clamp in-flight casts (re-running the engine cancels them). For MOVEMENT:
    // out-of-combat movers keep their travel wait (10ms re-picks jittered the destination and
    // froze bots), but IN-COMBAT movers re-path every 200ms -- a chasing bot that only
    // re-evaluates at spline end aims at where its target WAS and permanently falls behind
    // (the "bots suck at sticking to their target" report). 200ms tracks a strafing player
    // without spline-restart thrash.
    if (IsInstancedContent() && bot && !bot->IsNonMeleeSpellCasted(true, false, true))
    {
        if (!bot->IsMoving())
            d = std::min(d, sPlayerbotAIConfig.instanceReactDelay);
        else if (bot->IsInCombat())
            d = std::min(d, 200u);
    }
    PlayerbotAIBase::SetAIInternalUpdateDelay(d);
}

float PlayerbotAI::GetJumpiness() const
{
    // stable per-bot personality: hash the guid into 0.3x - 2.5x
    uint32 h = bot->GetGUIDLow() * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return 0.3f + float(h % 1000) * 0.0022f;
}

void PlayerbotAI::SetColdDormant(bool dormant)
{
    // LOD COLD dormancy RE-ENABLED 2026-06-30 for scale (10-15k bots). The previous breakage --
    // VISIBILITY_OFF bots whose pull-casts couldn't aggro mobs -- is avoided because the dormancy
    // GATE in UpdateAI now SKIPS the bot's entire AI while dormant, so a dormant bot never tries
    // to cast/pull while invisible. Only bots with no real player nearby are dormant, so the
    // invisibility is harmless (nobody is looking). They wake (visible + AI resumes) the instant a
    // real player approaches. Rollback: AiPlayerbot.LODColdUpdateMs = 0 + restart.
    if (dormant == coldDormant || !bot || !bot->IsInWorld())
        return;

    coldDormant = dormant;

    if (dormant)
    {
        bot->CombatStop(true);
        bot->GetHostileRefManager().deleteReferences();
        // SELECTIVE park-visibility: only CITY RESIDENTS stay visible (they sit in capitals to
        // populate them, waking when a player approaches to do town activities). Every other
        // parked bot goes invisible -- otherwise thousands of dense starter-zone bots stay
        // visible and their O(n^2) visibility updates balloon the continent tick and starve all
        // brains (the "bots stand still" regression). Invisible far bots are cheap; the player
        // only sees active bots near them anyway.
        const bool keepVisible = sPlayerbotAIConfig.parkVisible
            && sRandomPlayerbotMgr.IsCityResident(bot->GetGUIDLow());
        if (!keepVisible)
            bot->SetVisibility(VISIBILITY_OFF);
    }
    else
    {
        if (bot->GetVisibility() != VISIBILITY_ON)
            bot->SetVisibility(VISIBILITY_ON);                   // visible again
    }
}

// True when `spell` is an out-of-combat hostile pull: a damaging spell aimed at a
// creature other than the bot, while the bot is not yet in combat. See the note in
// PlayerbotAI::InterruptSpell.
bool PlayerbotAI::IsCastingHostilePull(Spell* spell)
{
    if (!spell || !spell->m_spellInfo || !bot || bot->IsInCombat())
        return false;

    bool isDamage = false;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spell->m_spellInfo->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE) { isDamage = true; break; }
    if (!isDamage)
        return false;

    Unit* target = spell->m_targets.getUnitTarget();
    return target && target->GetTypeId() == TYPEID_UNIT && target->GetObjectGuid() != bot->GetObjectGuid();
}

bool PlayerbotAI::RemoveAura(const std::string& name)
{
    const Aura* aura = GetAura(name, bot);
    if (aura)
    {
        const uint32 spellId = aura->GetSpellProto()->Id;
        if (spellId > 0)
        {
            bot->RemoveAurasDueToSpell(spellId);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::IsInterruptableSpellCasting(Unit* target, std::string spell, uint8 effectMask)
{
	uint32 spellid = aiObjectContext->GetValue<uint32>("spell id", spell)->Get();
	if (!spellid || !target->IsNonMeleeSpellCasted(true))
		return false;

	SpellEntry const *spellInfo = sServerFacade.LookupSpellInfo(spellid);
	if (!spellInfo)
		return false;

	for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
	{
		if ((spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_COMBAT) && spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
			return true;

		if ((spellInfo->Effect[i] == SPELL_EFFECT_INTERRUPT_CAST) &&
			!target->IsImmuneToSpellEffect(spellInfo, (SpellEffectIndex)i, true))
			return true;

        if ((spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA) && spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_SILENCE)
            return true;
	}

	return false;
}

bool PlayerbotAI::HasAuraToDispel(Unit* target, uint32 dispelType)
{
    bool isFriend = sServerFacade.IsFriendlyTo(bot, target);
	for (uint32 type = SPELL_AURA_NONE; type < TOTAL_AURAS; ++type)
	{
		Unit::AuraList const& auras = target->GetAurasByType((AuraType)type);
		for (Unit::AuraList::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
		{
			const Aura* aura = *itr;
			const SpellEntry* entry = aura->GetSpellProto();
			uint32 spellId = entry->Id;

			bool isPositiveSpell = IsPositiveSpell(spellId);
			if (isPositiveSpell && isFriend)
				continue;

			if (!isPositiveSpell && !isFriend)
				continue;

			if (sPlayerbotAIConfig.dispelAuraDuration && aura->GetAuraDuration() && aura->GetAuraDuration() < (int32)sPlayerbotAIConfig.dispelAuraDuration)
			    return false;

			if (canDispel(entry, dispelType))
				return true;
		}
	}
	return false;
}

#ifndef WIN32
inline int strcmpi(const char* s1, const char* s2)
{
    for (; *s1 && *s2 && (toupper(*s1) == toupper(*s2)); ++s1, ++s2);
    return *s1 - *s2;
}
#endif

bool PlayerbotAI::canDispel(const SpellEntry* entry, uint32 dispelType)
{
    if (entry->Dispel != dispelType)
        return false;

    return entry->SpellName[0].empty() ||
        (strcmpi(entry->SpellName[0].c_str(), "demon skin") &&
        strcmpi(entry->SpellName[0].c_str(), "mage armor") &&
        strcmpi(entry->SpellName[0].c_str(), "frost armor") &&
        strcmpi(entry->SpellName[0].c_str(), "wavering will") &&
        strcmpi(entry->SpellName[0].c_str(), "chilled") &&
        strcmpi(entry->SpellName[0].c_str(), "mana tap") &&
        strcmpi(entry->SpellName[0].c_str(), "ice armor"));
}

bool PlayerbotAI::IsHealSpell(const SpellEntry* spell)
{
    // Holy Light/Flash of Light
    if (spell->SpellFamilyName == SPELLFAMILY_PALADIN)
    {
        if (spell->SpellIconID == 70 ||
            spell->SpellIconID == 242)
            return true;
    }

    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (spell->Effect[i])
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                return true;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            case SPELL_EFFECT_APPLY_AREA_AURA_PET:
            {
                switch (spell->EffectApplyAuraName[i])
                {
                    case SPELL_AURA_PERIODIC_HEAL:
                        return true;
                }
                break;
            }
        }
    }
    return false;
}

bool PlayerbotAI::IsMiningNode(const GameObject* go)
{
    if (go)
    {
        const GameObjectInfo* goInfo = go->GetGOInfo();
        if (goInfo)
        {
            const LockEntry* lockInfo = sLockStore.LookupEntry(goInfo->GetLockId());
            if (lockInfo)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                    {
                        const uint32 skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        if (skillId == SKILL_MINING)
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool PlayerbotAI::IsHerb(const GameObject* go)
{
    if (go)
    {
        const GameObjectInfo* goInfo = go->GetGOInfo();
        if (goInfo)
        {
            const LockEntry* lockInfo = sLockStore.LookupEntry(goInfo->GetLockId());
            if (lockInfo)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                    {
                        const uint32 skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        if (skillId == SKILL_HERBALISM)
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasSpellItems(uint32 spellId, const Item* castItem) const
{
    const SpellEntry* spellEntry = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
    if (spellEntry)
    {
        if (HasCheat(BotCheatMask::item))
        {
            return true;
        }
        else
        {
            if (!bot->CanNoReagentCast(spellEntry))
            {
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                {
                    if (spellEntry->Reagent[i] <= 0)
                    {
                        continue;
                    }

                    uint32 itemid = spellEntry->Reagent[i];
                    uint32 itemcount = spellEntry->ReagentCount[i];

                    // if CastItem is also spell reagent
                    if (castItem && castItem->GetEntry() == itemid)
                    {
                        const ItemPrototype* proto = castItem->GetProto();
                        if (!proto)
                        {
                            return false;
                        }

                        for (uint8 s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                        {
                            // CastItem will be used up and does not count as reagent
                            int32 charges = castItem->GetSpellCharges(s);
                            if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                            {
                                ++itemcount;
                                break;
                            }
                        }
                    }

                    if (!bot->HasItemCount(itemid, itemcount))
                    {
                        return false;
                    }
                }
            }

            // check totem-item requirements (items presence in inventory)
            uint32 totems = MAX_SPELL_TOTEMS;
            for (auto i : spellEntry->Totem)
            {
                if (i != 0)
                {
                    if (bot->HasItemCount(i, 1))
                    {
                        totems -= 1;
                    }
                }
                else
                {
                    totems -= 1;
                }
            }

            if (totems != 0)
            {
                return false;
            }
        }
    }

    return false;
}

void PlayerbotAI::DurabilityLoss(Item* item, double percent)
{
    if (item)
    {
        const uint32 pCurrDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        const uint32 pMaxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (pMaxDurability)
        {
            if (!HasCheat(BotCheatMask::repair))
            {
                // Break item
                uint32 pDurabilityLoss = std::max(uint32(pMaxDurability * percent), 1U);
                bot->DurabilityPointsLoss(item, pDurabilityLoss);
            }
            else if (pCurrDurability < pMaxDurability)
            {
                // Repair if broken
#ifdef MANGOSBOT_ZERO
                bot->DurabilityRepair(item->GetPos(), false, 0.0f);
#else
                bot->DurabilityRepair(item->GetPos(), false, 0.0f, false);
#endif
            }
        }
    }
}

bool IsAlliance(uint8 race)
{
    return race == RACE_HUMAN || race == RACE_DWARF || race == RACE_NIGHTELF ||
           race == RACE_HIGH_ELF ||
#ifndef MANGOSBOT_ZERO
           race == RACE_DRAENEI ||
#endif
           race == RACE_GNOME;
}

uint32 PlayerbotAI::GetFixedBotNumber(BotTypeNumber typeNumber, uint32 maxNum, float cyclePerMin, bool ignoreGuid)
{
    uint8 seedNumber = uint8(typeNumber);
    std::mt19937 rng(seedNumber);
    uint32 randseed = rng();                                       //Seed random number
    uint32 randnum = randseed;                                     //Semi-random.
    
    if (!ignoreGuid)
        randnum += bot->GetGUIDLow();                              //but fixed number for each bot.

    if (cyclePerMin > 0)
    {
        uint32 cycle = floor(WorldTimer::getMSTime() / (1000));    //Semi-random number adds 1 each second.
        cycle = cycle * cyclePerMin / 60;                          //Cycles cyclePerMin per minute.
        randnum += cycle;                                          //Make the random number cylce.
    }
    randnum = (randnum % (maxNum+1));                              //Loops the randomnumber at maxNum. Bassically removes all the numbers above 99.
    return randnum;                                                //Now we have a number unique for each bot between 0 and maxNum that increases by cyclePerMin.
}

GrouperType PlayerbotAI::GetGrouperType()
{
    AiObjectContext* context = GetAiObjectContext();
    int32 grouperOverride = AI_VALUE2(int32, "manual saved int", "grouper override");

    if (grouperOverride >= 0)
        return GrouperType(grouperOverride);

    uint32 maxGroupType = sPlayerbotAIConfig.randomBotRaidNearby ? 100 : 95;
    uint32 grouperNumber = GetFixedBotNumber(BotTypeNumber::GROUPER_TYPE_NUMBER, maxGroupType, 0);

    //20% solo
    //50% member
    //20% leader
    //10% raider

    if (grouperNumber < 20 && !HasRealPlayerMaster())
        return GrouperType::SOLO;
    if (grouperNumber < 75)
        return GrouperType::MEMBER;
    if (grouperNumber < 80 || bot->GetLevel() < 3)
        return GrouperType::LEADER_2;
    if (grouperNumber < 85 || bot->GetLevel() < 5)
        return GrouperType::LEADER_3;
    if (grouperNumber < 90 || bot->GetLevel() < 7)
        return GrouperType::LEADER_4;
    if (grouperNumber <= 95 || bot->GetLevel() < 9)
        return GrouperType::LEADER_5;
#ifdef MANGOSBOT_ZERO
    if (grouperNumber <= 97)
        return GrouperType::RAIDER_20;
#else
    if (grouperNumber <= 97)
        return GrouperType::RAIDER_10;
#endif
   return GrouperType::RAIDER_MAX;
}

GuilderType PlayerbotAI::GetGuilderType()
{
    AiObjectContext* context = GetAiObjectContext();
    int32 guilderOverride = AI_VALUE2(int32, "manual saved int", "guilder override");

    if (guilderOverride >= 0)
        return GuilderType(guilderOverride);

    uint32 grouperNumber = GetFixedBotNumber(BotTypeNumber::GUILDER_TYPE_NUMBER, 100, 0);

    if (grouperNumber < 20 && !HasRealPlayerMaster())
        return GuilderType::SOLO;
    if (grouperNumber < 30)
        return GuilderType::TINY;
    if (grouperNumber < 40)
        return GuilderType::SMALL;
    if (grouperNumber < 60)
        return GuilderType::MEDIUM;
    if (grouperNumber < 80)
        return GuilderType::LARGE;

    return GuilderType::MASSIVE;
}

uint32 PlayerbotAI::GetMaxPreferedGuildSize()
{
    uint32 maxSize = (uint8)GetGuilderType();

    if (!bot->GetGuildId()) return maxSize;

    Guild* guild =sGuildMgr.GetGuildById(bot->GetGuildId());

    uint32 maxRank = guild->GetRanksSize();

    MemberSlot* botMember = guild->GetMemberSlot(bot->GetObjectGuid());

    if (!botMember->RankId) return maxSize;

    uint32 memberMod = (botMember->RankId * 20) / maxRank;

    return (maxSize * (100 - memberMod)) / 100;
}

bool PlayerbotAI::HasPlayerNearby(WorldPosition pos, float range)
{
    if (!range)
        range = pos.getVisibilityDistance();

    float sqRange = range * range;
    // Runs on MAP THREADS constantly (LOD, dormant wake probes, say/yell listen checks).
    // Read the world-thread-built snapshot; NEVER iterate the raw players map or touch
    // Player pointers here -- that race was the whole 2026-07-04 SIGSEGV family.
    auto snap = sRandomPlayerbotMgr.GetRealPlayerSnapshot();
    for (auto const& s : snap->players)
    {
        // GM observers still count for bot LOD. Otherwise an invisible GM can
        // stand in a crowd and every bot nearby is treated as background work.
        if (s.mapId != bot->GetMapId())
            continue;

        if (pos.sqDistance(WorldPosition(s.mapId, s.x, s.y, s.z)) < sqRange)
            return true;

        // if player is far check farsight/cinematic camera
        if (s.hasCam && pos.sqDistance(WorldPosition(s.mapId, s.camX, s.camY, s.camZ)) < sqRange)
            return true;
    }

    return false;
}

bool PlayerbotAI::HasPlayerNearby(float range)
{
    return HasPlayerNearby(bot, range);
}

bool PlayerbotAI::HasManyPlayersNearby(uint32 trigerrValue, float range)
{
    float sqRange = range * range;
    uint32 found = 0;

    // snapshot read -- same map-thread safety rule as HasPlayerNearby (the old raw loop
    // also skipped the null check entirely and deref'd freed players on relog)
    auto snap = sRandomPlayerbotMgr.GetRealPlayerSnapshot();
    for (auto const& s : snap->players)
    {
        if (s.mapId != bot->GetMapId())
            continue;
        const float dx = s.x - bot->GetPositionX(), dy = s.y - bot->GetPositionY();
        if (dx * dx + dy * dy < sqRange)
        {
            found++;

            if (found >= trigerrValue)
                return true;
        }
    }
    return false;
}

bool PlayerbotAI::ChannelHasRealPlayer(std::string channelName)
{
    if (ChannelMgr* cMgr = channelMgr(bot->GetTeam()))
    {

        if (Channel* chn = cMgr->GetChannel(channelName, bot))
        {
            ChannelAcces* chna = reinterpret_cast<ChannelAcces*>(chn);

            auto snap = sRandomPlayerbotMgr.GetRealPlayerSnapshot();
            for (auto const& s : snap->players)
                if (chna->IsOn(ObjectGuid(HIGHGUID_PLAYER, s.guidLow)))
                    return true;
        }
    }

    return false;
}

/*
enum ActivityType
{
    GRIND_ACTIVITY = 1,
    RPG_ACTIVITY = 2,
    TRAVEL_ACTIVITY = 3,
    OUT_OF_PARTY_ACTIVITY = 4,
    PACKET_ACTIVITY = 5,
    DETAILED_MOVE_ACTIVITY = 6,
    PARTY_ACTIVITY = 7
    ALL_ACTIVITY = 8
};

   General function to check if a bot is allowed to be active or not.
   This function should be checked first before doing heavy-workload.


*/

ActivePiorityType PlayerbotAI::GetPriorityType()
{
    //First priority - priorities disabled or has player master. Always active.
    if (sPlayerbotAIConfig.disableActivityPriorities || HasRealPlayerMaster())
        return ActivePiorityType::HAS_REAL_PLAYER_MASTER;

    //Self bot in a group with a bot master.
    if (IsRealPlayer())
        return ActivePiorityType::IS_REAL_PLAYER;

    Group* group = bot->GetGroup();
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();

            if (!member || !member->IsInWorld())
                continue;

            if (member == bot)
                continue;

            if (!member->GetPlayerbotAI() || (member->GetPlayerbotAI() && member->GetPlayerbotAI()->HasRealPlayerMaster()))
                return ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER;
        }
    }

    if (bot->IsBeingTeleported()) //We might end up in a bg so stay active.
        return ActivePiorityType::IN_BATTLEGROUND;

    if (WorldPosition(bot).isBg())
        return ActivePiorityType::IN_BATTLEGROUND;

    AiObjectContext* context = GetAiObjectContext();

    if (AI_VALUE2(bool, "manual bool", "is running test"))
        return ActivePiorityType::IS_RUNNING_TEST;

    if (!WorldPosition(bot).isOverworld())
    {
        if (!sPlayerbotAIConfig.enableMinimalMove)
            return ActivePiorityType::IN_INSTANCE;
        else
        {
            LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
            if (lastMove.lastPath.empty())
                return ActivePiorityType::IN_INSTANCE;
        }
    }

    if (HasPlayerNearby())
        return ActivePiorityType::VISIBLE_FOR_PLAYER;

    if (sPlayerbotAIConfig.guildOrderAlwaysActive && bot->IsInWorld() && bot->GetGuildId())
    {
        AiObjectContext* context = GetAiObjectContext();
        GuildOrder guildOrder = AI_VALUE(GuildOrder, "guild order");
        if (guildOrder.IsValid())
            return ActivePiorityType::IS_ALWAYS_ACTIVE;
    }

    if (sServerFacade.IsInCombat(bot))
        return ActivePiorityType::IN_COMBAT;

    if (HasPlayerNearby(WorldPosition(bot).getVisibilityDistance() + sPlayerbotAIConfig.reactDistance))
        return ActivePiorityType::NEARBY_PLAYER;

    if (sPlayerbotAIConfig.IsFreeAltBot(bot) || HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
        return ActivePiorityType::IS_ALWAYS_ACTIVE;

    if (bot->InBattleGroundQueue())
        return ActivePiorityType::IN_BG_QUEUE;

    bool isLFG = false;
#ifdef MANGOSBOT_TWO
    /*if (group)
    {
    if (sLFGMgr.GetQueueInfo(group->GetObjectGuid()))
    {
    isLFG = true;
    }
    }
    if (sLFGMgr.GetQueueInfo(bot->GetObjectGuid()))
    {
    isLFG = true;
    }*/
#endif

    if (isLFG)
        return ActivePiorityType::IN_LFG;

    if (sPlayerbotAIConfig.enableMinimalMove)
    {
        AiObjectContext* context = GetAiObjectContext();
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (lastMove.lastPath.empty() && !urand(0, 5))
            return ActivePiorityType::NO_PATH;
    }

    //If has real players - slow down continents without player
    //This means we first disable bots in a different continent/area.
    // MAP-THREAD SAFE: read the world-thread real-player view; the old raw players-map
    // iteration + GetSocial()->HasFriend deref here (called via AllowActivity from every
    // map thread) was the primary crash site of the 2026-07-04 SIGSEGV family.
    auto rpView = sRandomPlayerbotMgr.GetRealPlayerSnapshot();
    if (rpView->players.empty())
        return ActivePiorityType::IN_EMPTY_SERVER;

    // friends always active (pre-computed on the world thread)
    if (rpView->friendBotGuids.count(bot->GetGUIDLow()))
        return ActivePiorityType::PLAYER_FRIEND;

    // real guild always active if member+
    if (IsInRealGuild())
        return ActivePiorityType::PLAYER_GUILD;

    if (bot->IsBeingTeleported() || !bot->IsInWorld() || !bot->GetMap()->HasRealPlayers())
        return ActivePiorityType::IN_INACTIVE_MAP;

    if (!bot->GetMap()->HasActiveZone(bot->GetZoneId()))
        return ActivePiorityType::IN_ACTIVE_MAP;

    return ActivePiorityType::IN_ACTIVE_AREA;
}

//Returns the lower and upper bracket for bots to be active.
//Ie. 10,20 means all bots in this bracket will be inactive below 10% activityMod, all bots in this bracket will be active above 20% activityMod and scale between those values.
std::pair<uint32, uint32> PlayerbotAI::GetPriorityBracket(ActivePiorityType type)
{
    switch (type)
    {
    case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
    case ActivePiorityType::IS_REAL_PLAYER:
    case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
    case ActivePiorityType::VISIBLE_FOR_PLAYER:
    case ActivePiorityType::IN_BATTLEGROUND:
    case ActivePiorityType::IS_RUNNING_TEST:
        return { 0,0 };
    case ActivePiorityType::IN_INSTANCE:
        return { 0,5 };
    case ActivePiorityType::IS_ALWAYS_ACTIVE:
        return { 0,0 };
    case ActivePiorityType::IN_COMBAT:
    {
        if (sPlayerbotAIConfig.limitCombatActivity)
            return { 99,100 };

        return { 0,10 };
    }
    case ActivePiorityType::IN_BG_QUEUE:
        return { 0,20 };
    case ActivePiorityType::IN_LFG:
        return { 0,30 };
    case ActivePiorityType::NEARBY_PLAYER:
        return { 0,0 };
    case ActivePiorityType::PLAYER_FRIEND:
    case ActivePiorityType::PLAYER_GUILD:
        return { 0,50 };
    case ActivePiorityType::NO_PATH:
        return { 50, 99};
    case ActivePiorityType::IN_ACTIVE_AREA:
    case ActivePiorityType::IN_EMPTY_SERVER:
        return { 50,100 }; //Note lower 100 means multiply by activity percentage.
    case ActivePiorityType::IN_ACTIVE_MAP:
        return { 70,100 };
    case ActivePiorityType::IN_INACTIVE_MAP:
        return { 80,100 };
    default :
        return { 90, 100 };
    }

    return { 90, 100 };
}

bool PlayerbotAI::AllowActive(ActivityType activityType)
{
    ActivePiorityType type = GetPriorityType();

    if (activityType == DETAILED_MOVE_ACTIVITY)
    {
        switch (type)
        {
        case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
        case ActivePiorityType::IS_REAL_PLAYER:
        case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
        case ActivePiorityType::IN_INSTANCE:
        case ActivePiorityType::VISIBLE_FOR_PLAYER:
        case ActivePiorityType::IN_COMBAT:
        case ActivePiorityType::NEARBY_PLAYER:
        case ActivePiorityType::IS_RUNNING_TEST:
            return true;
            break;
        case ActivePiorityType::IS_ALWAYS_ACTIVE:
        case ActivePiorityType::IN_BG_QUEUE:
        case ActivePiorityType::IN_LFG:
        case ActivePiorityType::PLAYER_FRIEND:
        case ActivePiorityType::PLAYER_GUILD:
        case ActivePiorityType::NO_PATH:
        case ActivePiorityType::IN_ACTIVE_AREA:
        case ActivePiorityType::IN_EMPTY_SERVER:
        case ActivePiorityType::IN_ACTIVE_MAP:
        case ActivePiorityType::IN_INACTIVE_MAP:
        default:
            break;
        }
    }
    else if (activityType == REACT_ACTIVITY)
    {
        switch (type)
        {
        case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
        case ActivePiorityType::IS_REAL_PLAYER:
        case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
        case ActivePiorityType::IN_INSTANCE:
        case ActivePiorityType::IS_ALWAYS_ACTIVE:
        case ActivePiorityType::IS_RUNNING_TEST:
            return true;
        case ActivePiorityType::VISIBLE_FOR_PLAYER:
            if (sPlayerbotAIConfig.forceActiveWhenNearPlayer)
                return true;
            break;
        case ActivePiorityType::IN_COMBAT:
        case ActivePiorityType::NEARBY_PLAYER:
        case ActivePiorityType::IN_BG_QUEUE:
        case ActivePiorityType::IN_LFG:
        case ActivePiorityType::PLAYER_FRIEND:
        case ActivePiorityType::PLAYER_GUILD:
        case ActivePiorityType::NO_PATH:
        case ActivePiorityType::IN_ACTIVE_AREA:
        case ActivePiorityType::IN_EMPTY_SERVER:
        case ActivePiorityType::IN_ACTIVE_MAP:
        case ActivePiorityType::IN_INACTIVE_MAP:
        default:
            break;
        }
    }

    std::pair<uint8, uint8> priorityBracket = GetPriorityBracket(type);

    float activityPercentage = sRandomPlayerbotMgr.getActivityPercentage(); //Activity between 0 and 100.

    if (!priorityBracket.second) //No scaling
        return true;

    if (priorityBracket.first >= activityPercentage)
        return false;
    if (priorityBracket.second <= activityPercentage && priorityBracket.second < 100)
        return true;

    float activePerc = (activityPercentage - priorityBracket.first) / (priorityBracket.second - priorityBracket.first);

    activePerc *= (priorityBracket.second == 100) ? sPlayerbotAIConfig.botActiveAlone : 100;

    uint32 ActivityNumber = GetFixedBotNumber(BotTypeNumber::ACTIVITY_TYPE_NUMBER, 100, activePerc * 0.01f); //The last number if the amount it cycles per min. Currently set to 1% of the active bots.

    return ActivityNumber <= (activePerc);           //The given percentage of bots should be active and rotate 1% of those active bots each minute.
}

bool PlayerbotAI::AllowActivity(ActivityType activityType, bool checkNow)
{
    if (!allowActiveCheckTimer[activityType])
        allowActiveCheckTimer[activityType] = time(NULL);

    if (!checkNow && time(NULL) < (allowActiveCheckTimer[activityType] + 5))
        return allowActive[activityType];

    bool allowed = AllowActive(activityType);
    allowActive[activityType] = allowed;
    allowActiveCheckTimer[activityType] = time(NULL);
    return allowed;
}

bool PlayerbotAI::HasCheat(BotCheatMask mask) const
{
    if (((uint32)mask & (uint32)cheatMask) != 0)
        return true;

    if (sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        if (((uint32)mask & sPlayerbotAIConfig.rndBotCheatMask) != 0)
            return true;
    }
    else
    {
        if (((uint32)mask & sPlayerbotAIConfig.botCheatMask) != 0)
            return true;
    }

    return false;
}

bool PlayerbotAI::IsOpposing(Player* player)
{
    return IsOpposing(player->getRace(), bot->getRace());
}

bool PlayerbotAI::IsOpposing(uint8 race1, uint8 race2)
{
    return (IsAlliance(race1) && !IsAlliance(race2)) || (!IsAlliance(race1) && IsAlliance(race2));
}

void PlayerbotAI::RemoveShapeshift()
{
    RemoveAura("bear form");
    RemoveAura("dire bear form");
    RemoveAura("moonkin form");
    RemoveAura("travel form");
    RemoveAura("cat form");
    RemoveAura("flight form");
    RemoveAura("swift flight form");
    RemoveAura("aquatic form");
    RemoveAura("ghost wolf");
    RemoveAura("tree of life");
}

uint32 PlayerbotAI::GetEquipGearScore(Player* player, bool withBags, bool withBank)
{
    std::vector<uint32> gearScore(EQUIPMENT_SLOT_END);
    uint32 twoHandScore = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            _fillGearScoreData(player, item, &gearScore, twoHandScore);
    }

    if (withBags)
    {
        // check inventory
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        // check bags
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* item2 = pBag->GetItemByPos(j))
                        _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                }
            }
        }
    }

    if (withBank)
    {
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (item->IsBag())
                {
                    Bag* bag = (Bag*)item;
                    for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    {
                        if (Item* item2 = bag->GetItemByPos(j))
                            _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                    }
                }
            }
        }
    }

    uint8 count = EQUIPMENT_SLOT_END - 2;   // ignore body and tabard slots
    uint32 sum = 0;

    // check if 2h hand is higher level than main hand + off hand
    if (gearScore[EQUIPMENT_SLOT_MAINHAND] + gearScore[EQUIPMENT_SLOT_OFFHAND] < twoHandScore * 2)
    {
        gearScore[EQUIPMENT_SLOT_OFFHAND] = 0;  // off hand is ignored in calculations if 2h weapon has higher score
        --count;
        gearScore[EQUIPMENT_SLOT_MAINHAND] = twoHandScore;
    }

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
       sum += gearScore[i];
    }

    if (count)
    {
        uint32 res = uint32(sum / count);
        return res;
    }
    else
        return 0;
}

uint32 PlayerbotAI::GetEquipStatsValue(Player* player)
{
    uint32 statsValue = 0;
    uint32 specId = sRandomItemMgr.GetPlayerSpecId(player);

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            statsValue += sRandomItemMgr.GetStatWeight(item->GetProto()->ItemId, specId);
    }

    return statsValue;
}

void PlayerbotAI::_fillGearScoreData(Player *player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore)
{
    if (!item)
        return;

    if (player->CanUseItem(item->GetProto()) != EQUIP_ERR_OK)
        return;

    uint8 type   = item->GetProto()->InventoryType;
    uint32 level = item->GetProto()->ItemLevel;

    switch (type)
    {
        case INVTYPE_2HWEAPON:
            twoHandScore = std::max(twoHandScore, level);
            break;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
            (*gearScore)[SLOT_MAIN_HAND] = std::max((*gearScore)[SLOT_MAIN_HAND], level);
            break;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:
            (*gearScore)[EQUIPMENT_SLOT_OFFHAND] = std::max((*gearScore)[EQUIPMENT_SLOT_OFFHAND], level);
            break;
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_RANGED:
        case INVTYPE_QUIVER:
        case INVTYPE_RELIC:
            (*gearScore)[EQUIPMENT_SLOT_RANGED] = std::max((*gearScore)[EQUIPMENT_SLOT_RANGED], level);
            break;
        case INVTYPE_HEAD:
            (*gearScore)[EQUIPMENT_SLOT_HEAD] = std::max((*gearScore)[EQUIPMENT_SLOT_HEAD], level);
            break;
        case INVTYPE_NECK:
            (*gearScore)[EQUIPMENT_SLOT_NECK] = std::max((*gearScore)[EQUIPMENT_SLOT_NECK], level);
            break;
        case INVTYPE_SHOULDERS:
            (*gearScore)[EQUIPMENT_SLOT_SHOULDERS] = std::max((*gearScore)[EQUIPMENT_SLOT_SHOULDERS], level);
            break;
        case INVTYPE_BODY:
            (*gearScore)[EQUIPMENT_SLOT_BODY] = std::max((*gearScore)[EQUIPMENT_SLOT_BODY], level);
            break;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            (*gearScore)[EQUIPMENT_SLOT_CHEST] = std::max((*gearScore)[EQUIPMENT_SLOT_CHEST], level);
            break;
        case INVTYPE_WAIST:
            (*gearScore)[EQUIPMENT_SLOT_WAIST] = std::max((*gearScore)[EQUIPMENT_SLOT_WAIST], level);
            break;
        case INVTYPE_LEGS:
            (*gearScore)[EQUIPMENT_SLOT_LEGS] = std::max((*gearScore)[EQUIPMENT_SLOT_LEGS], level);
            break;
        case INVTYPE_FEET:
            (*gearScore)[EQUIPMENT_SLOT_FEET] = std::max((*gearScore)[EQUIPMENT_SLOT_FEET], level);
            break;
        case INVTYPE_WRISTS:
            (*gearScore)[EQUIPMENT_SLOT_WRISTS] = std::max((*gearScore)[EQUIPMENT_SLOT_WRISTS], level);
            break;
        case INVTYPE_HANDS:
            (*gearScore)[EQUIPMENT_SLOT_HANDS] = std::max((*gearScore)[EQUIPMENT_SLOT_HANDS], level);
            break;
        // equipped gear score check uses both rings and trinkets for calculation, assume that for bags/banks it is the same
        // with keeping second highest score at second slot
        case INVTYPE_FINGER:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_FINGER1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = (*gearScore)[EQUIPMENT_SLOT_FINGER1];
                (*gearScore)[EQUIPMENT_SLOT_FINGER1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_FINGER2] < level)
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = level;
            break;
        }
        case INVTYPE_TRINKET:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_TRINKET1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = (*gearScore)[EQUIPMENT_SLOT_TRINKET1];
                (*gearScore)[EQUIPMENT_SLOT_TRINKET1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_TRINKET2] < level)
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = level;
            break;
        }
        case INVTYPE_CLOAK:
            (*gearScore)[EQUIPMENT_SLOT_BACK] = std::max((*gearScore)[EQUIPMENT_SLOT_BACK], level);
            break;
        default:
            break;
    }
}

std::string PlayerbotAI::BotStateToString(BotState state)
{
    switch (state)
    {
        case BotState::BOT_STATE_COMBAT: return "Combat";
        case BotState::BOT_STATE_NON_COMBAT: return "Non Combat";
        case BotState::BOT_STATE_DEAD: return "Dead";
        case BotState::BOT_STATE_REACTION: return "Reaction";
        default: return "";
    }
}

std::string PlayerbotAI::GetDefaultMovementStrategy()
{
    // Player master -> follow
    if (HasActivePlayerMaster())
        return "follow";

    // Bot/no master -> wander
    return "wander";
}

void PlayerbotAI::EnsureDefaultMovementStrategy(Player* requester)
{
    std::string movement = GetDefaultMovementStrategy();

    for (BotState state : { BotState::BOT_STATE_REACTION, BotState::BOT_STATE_NON_COMBAT })
    {
        if (HasStrategy("stay", state))
            ChangeStrategy("-stay", state);

        if (!HasStrategy(movement, state))
            ChangeStrategy("+" + movement, state);
    }

    if (requester)
    {
        TellPlayerNoFacing(
            requester,
            "Welcome back!",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
            false
        );
    }
}

std::string PlayerbotAI::HandleRemoteCommand(std::string command)
{
    if (command == "state")
    {
        switch (currentState)
        {
        case BotState::BOT_STATE_COMBAT:
            return "combat";
        case BotState::BOT_STATE_DEAD:
            return "dead";
        case BotState::BOT_STATE_NON_COMBAT:
            return "non-combat";
        default:
            return "unknown";
        }
    }
    else if (command == "position")
    {
        std::ostringstream out; out << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << " " << bot->GetMapId() << " " << bot->GetOrientation();
        uint32 area = sServerFacade.GetAreaId(bot);
        if (const AreaTableEntry* areaEntry = GetAreaEntryByAreaID(area))
        {
            if (AreaTableEntry const* zoneEntry = areaEntry->zone ? GetAreaEntryByAreaID(areaEntry->zone) : areaEntry)
                out << " |" << zoneEntry->area_name[0] << "|";
        }
        return out.str();
    }
    else if (command == "tpos")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return "";
        }

        std::ostringstream out; out << target->GetPositionX() << " " << target->GetPositionY() << " " << target->GetPositionZ() << " " << target->GetMapId() << " " << target->GetOrientation();
        return out.str();
    }
    else if (command == "target")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return "";
        }

        return target->GetName();
    }
    else if (command == "hp")
    {
        int pct = (int)((static_cast<float> (bot->GetHealth()) / bot->GetMaxHealth()) * 100);
        std::ostringstream out; out << pct << "%";

        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return out.str();
        }

        pct = (int)((static_cast<float> (target->GetHealth()) / target->GetMaxHealth()) * 100);
        out << " / " << pct << "%";
        return out.str();
    }
    else if (command == "combat")
    {
        std::ostringstream out;

        bool unitFlagInCombat = bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IN_COMBAT);
        out << "UNIT_FLAG_IN_COMBAT: " << (unitFlagInCombat ? "SET" : "clear");

        out << ", IsInCombat(): " << (bot->IsInCombat() ? "true" : "false");

        out << ", CMaNGOS attackers: " << bot->getAttackers().size();

        Unit* victim = bot->GetVictim();
        out << ", victim: " << (victim ? victim->GetName() : "none");

        out << " | BotAI: current target: ";
        Unit* aiTarget = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (aiTarget)
        {
            out << aiTarget->GetName() << " (" << aiTarget->GetObjectGuid().GetCounter() << ")";
            bool isInvalid = GetAiObjectContext()->GetValue<bool>("invalid target", "current target")->Get();
            out << ", invalid: " << (isInvalid ? "YES" : "no");
        }
        else
        {
            out << "none";
        }

        bool hasAttackers = GetAiObjectContext()->GetValue<bool>("has attackers")->Get();
        out << ", has attackers: " << (hasAttackers ? "true" : "false");

        out << " | Selection: " << bot->GetSelectionGuid().GetCounter();

        return out.str();
    }
    else if (command == "strategy")
    {
        return currentEngine->ListStrategies();
    }
    else if (command == "action")
    {
        return currentEngine->GetLastAction();
    }
    else if (command == "values")
    {
        return GetAiObjectContext()->FormatValues();
    }
    else if (command == "travel")
    {
        std::ostringstream out;

        TravelTarget* target = GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();

        if (target->GetGroupmember() && target->GetGroupmember().GetPlayer())
            out << target->GetGroupmember().GetPlayer()->GetName() << "'s ";

        if (target->GetDestination()) {
            out << "Target: destinationPtr=1";
        }

        if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
        {
            out << "\nStatus: ";
            if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
                out << "ready";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                out << "preparing";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
                out << (target->IsForced() ? "forced traveling" : "traveling");
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                out << "working";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
                out << "cooldown";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << "expired";

            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " [for " << (target->GetTimeLeft() / 1000) << "s]";

            if (target->GetRetryCount(true) || target->GetRetryCount(false))
                out << "(retry " << target->GetRetryCount(true) << "/" << target->GetRetryCount(false) << ")";           
        }

        return out.str();
    }
    else if (command == "traveldetail")
    {
        std::ostringstream out;

        TravelTarget* target = GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();

        if (target->GetGroupmember() && target->GetGroupmember().GetPlayer())
            out << target->GetGroupmember().GetPlayer()->GetName() << "'s ";

        if (target->GetDestination()) {
            out << target->GetDestination()->GetShortName() << " travel target";

            out << "\nTarget: destinationPtr=1";

            out << "\nDistance " << round(target->GetDestination()->DistanceTo(bot)) << "y";

            if (*target->GetPosition())
            {
                out << "\nLocation: " << target->GetPosition()->getAreaName();
                out << " (" << round(target->GetPosition()->distance(bot)) << "y)";
            }
        }
        
        if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
        {
            out << "\nStatus: ";
            if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
                out << "ready";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                out << "preparing";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
                out << (target->IsForced() ? "forced traveling" : "traveling");
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                out << "working";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
                out << "cooldown";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << "expired";

            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " [for " << (target->GetTimeLeft() / 1000) << "s]";

            if (target->GetRetryCount(true) || target->GetRetryCount(false))
                out << "(retry " << target->GetRetryCount(true) << "/" << target->GetRetryCount(false) << ")";

            if (target->GetConditions().size())
            {
                out << "\nConditions: ";

                for (auto& condition : target->GetConditions())
                {
                    AiObjectContext* context = GetAiObjectContext();
                    out << condition;
                    if (AI_VALUE(bool, condition))
                        out << " (true)";
                    else
                        out << " (false)";

                    if (condition != target->GetConditions().back())
                        out << ", ";
                }
            }
        }

        return out.str();
    }
    else if (command.find("budget") == 0)
    {
        std::string sub;

        if (command.size() > 7)
            sub = command.substr(7);

        std::ostringstream out;

        AiObjectContext* context = GetAiObjectContext();

        if (sub.empty())
        {
            out << "Current money: " << ChatHelper::formatMoney(bot->GetMoney()) << " free to use:" << ChatHelper::formatMoney(AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::anything)) << "\n";
            out << "Purpose: Needed | Available\n";
        }

        for (uint32 i = 1; i < (uint32)NeedMoneyFor::anything; i++)
        {
            NeedMoneyFor needMoneyFor = NeedMoneyFor(i);

            std::unordered_map<NeedMoneyFor, std::string> needStrMap =
            { { NeedMoneyFor::none, "nothing"},
              { NeedMoneyFor::repair, "repair"},
              { NeedMoneyFor::ammo, "ammo"},
              { NeedMoneyFor::spells, "spells"},
              { NeedMoneyFor::travel, "travel"},
              { NeedMoneyFor::consumables, "consumables"},
              { NeedMoneyFor::gear, "gear"},
              { NeedMoneyFor::guild, "guild"},
              { NeedMoneyFor::tradeskill, "tradeskill"},
              { NeedMoneyFor::skilltraining, "skilltraining"},
              { NeedMoneyFor::ah, "ah"},
              { NeedMoneyFor::mount, "mount"},
              { NeedMoneyFor::anything, "anything"}};

            if (!sub.empty() && needStrMap.at(needMoneyFor).find(sub) == std::string::npos)
                continue;

            out <<  needStrMap.at(needMoneyFor);

            out << ": " << ChatHelper::formatMoney(AI_VALUE2(uint32, "money needed for", i)) << " | " << ChatHelper::formatMoney(AI_VALUE2(uint32, "free money for", i));

            if (i != (uint32)NeedMoneyFor::mount)
                out << "\n";
        }

        return out.str();
    }
    std::ostringstream out; out << "invalid command: " << command;
    return out.str();
}

bool PlayerbotAI::HasSkill(SkillType skill)
{
    return bot->HasSkill(skill) && bot->GetSkillValue(skill) > 0;
}

bool ChatHandler::HandlePlayerbotCommand(char* args)
{
    return PlayerbotMgr::HandlePlayerbotMgrCommand(this, args);
}

bool ChatHandler::HandleRandomPlayerbotCommand(char* args)
{
    return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(this, args);
}

//Dummy handler until Chat.h can be modified.
bool ChatHandler::HandleAhBotCommand(char* args)
{
    return false;
}

float PlayerbotAI::GetRange(std::string type)
{
    float val = 0;

    if (type == "follow" && bot->GetGroup() && bot->GetGroup()->IsRaidGroup())
        type = "followraid";

    if (aiObjectContext) val = aiObjectContext->GetValue<float>("range", type)->Get();
    if (abs(val) >= 0.1f) return val;

    if (type == "spell") return sPlayerbotAIConfig.spellDistance;
    if (type == "shoot") return sPlayerbotAIConfig.shootDistance;
    if (type == "flee") return sPlayerbotAIConfig.fleeDistance;
    if (type == "heal") return sPlayerbotAIConfig.healDistance;
    if (type == "follow") return sPlayerbotAIConfig.followDistance;
    if (type == "guard") return sPlayerbotAIConfig.sightDistance;
    if (type == "followraid") return sPlayerbotAIConfig.raidFollowDistance;
    if (type == "wandermin") return sPlayerbotAIConfig.wanderMinDistance;
    if (type == "wandermax") return sPlayerbotAIConfig.wanderMaxDistance;
    if (type == "attack") return 0;
    return 0;
}

//Copy from reputation GetFactionReaction
ReputationRank PlayerbotAI::GetFactionReaction(FactionTemplateEntry const* thisTemplate, FactionTemplateEntry const* otherTemplate)
{
    MANGOS_ASSERT(thisTemplate)
        MANGOS_ASSERT(otherTemplate)

        // Original logic begins

        if (otherTemplate->factionGroupMask & thisTemplate->enemyGroupMask)
            return REP_HOSTILE;

    if (thisTemplate->enemyFaction[0] && otherTemplate->faction)
    {
        for (unsigned int i : thisTemplate->enemyFaction)
        {
            if (i == otherTemplate->faction)
                return REP_HOSTILE;
        }
    }

    if (otherTemplate->factionGroupMask & thisTemplate->friendGroupMask)
        return REP_FRIENDLY;

    if (thisTemplate->friendFaction[0] && otherTemplate->faction)
    {
        for (unsigned int i : thisTemplate->friendFaction)
        {
            if (i == otherTemplate->faction)
                return REP_FRIENDLY;
        }
    }

    if (thisTemplate->factionGroupMask & otherTemplate->friendGroupMask)
        return REP_FRIENDLY;

    if (otherTemplate->friendFaction[0] && thisTemplate->faction)
    {
        for (unsigned int i : otherTemplate->friendFaction)
        {
            if (i == thisTemplate->faction)
                return REP_FRIENDLY;
        }
    }
    return REP_NEUTRAL;
}

bool PlayerbotAI::AddAura(Unit* unit, uint32 spellId)
{
    if (!unit)
        return false;

    // Use the core aura path so holder ownership and AddSpellAuraHolder failure
    // handling stay consistent. The old local clone could delete a holder after
    // AddSpellAuraHolder had already consumed/removed it.
    return unit->AddAura(spellId, 0, unit) != nullptr;
}

void PlayerbotAI::InventoryIterateItems(IterateItemsVisitor* visitor, IterateItemsMask mask)
{
    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BAGS)
        InventoryIterateItemsInBags(visitor);

    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP)
        InventoryIterateItemsInEquip(visitor);

    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BANK)
        InventoryIterateItemsInBank(visitor);

    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BUYBACK)
        InventoryIterateItemsInBuyBack(visitor);
}

std::vector<Bag*> PlayerbotAI::GetEquippedAnyBags()
{
    std::vector<Bag*> bags;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            bags.push_back(bag);
        }
    }

    return bags;
}

std::vector<Bag*> PlayerbotAI::GetEquippedQuivers()
{
    std::vector<Bag*> bags;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (bag->GetProto()->Class == ITEM_CLASS_QUIVER)
            {
                bags.push_back(bag);
            }
        }
    }

    return bags;
}

std::vector<Item*> PlayerbotAI::GetInventoryAndEquippedItems()
{
    std::vector<Item*> items;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    items.push_back(pItem);
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; slot++)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            items.push_back(pItem);
        }
    }

    return items;
}

std::vector<Item*> PlayerbotAI::GetInventoryItems()
{
    std::vector<Item*> items;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    items.push_back(pItem);
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    return items;
}

uint32 PlayerbotAI::GetInventoryItemsCountWithId(uint32 itemId)
{
    uint32 count = 0;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetProto()->ItemId == itemId)
                    {
                        count += pItem->GetCount();
                    }
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                count += pItem->GetCount();
            }
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                count += pItem->GetCount();
            }
        }
    }

    return count;
}

bool PlayerbotAI::HasItemInInventory(uint32 itemId)
{

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetProto()->ItemId == itemId)
                    {
                        return true;
                    }
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                return true;
            }
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                return true;
            }
        }
    }

    return false;
}

/*
* @return true if has stacks which are not full for these items
*/
bool PlayerbotAI::HasNotFullStacksInBagsForLootItems(LootItemList &lootItemList)
{
    for (auto lootItem : lootItemList)
    {
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* pItem = pBag->GetItemByPos(j))
                    {
                        if (pItem->GetProto()->ItemId == lootItem.itemId
                            && pItem->GetCount() < pItem->GetMaxStackCount())
                        {
                            return true;
                        }
                    }
                }
            }
        }

        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (pItem->GetProto()->ItemId == lootItem.itemId
                    && pItem->GetCount() < pItem->GetMaxStackCount())
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasQuestItemsInWOLootList(WorldObject* wo)
{
    if (!wo)
        return false;

    LootItemList lootItemList = {};

    // Penqle's loot is on Creature/GameObject; dispatch via cast.
    Loot* woLoot = nullptr;
    if (wo->IsCreature()) woLoot = ((Creature*)wo)->m_loot;
    else if (wo->IsGameObject()) woLoot = ((GameObject*)wo)->m_loot;
    if (woLoot)
        woLoot->GetLootItemsListFor(bot, lootItemList);

    if (HasQuestItemsInLootList(lootItemList))
    {
        return true;
    }

    return false;
}

bool PlayerbotAI::HasQuestItemsInLootList(LootItemList &lootItemList)
{
    for (auto lootItem : lootItemList)
    {
        if (lootItem.lootItemType == LOOTITEM_TYPE_QUEST)
        {
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::CanLootSomethingFromWO(WorldObject* wo)
{
    if (!wo)
        return false;

    ObjectGuid guid = wo->GetObjectGuid();
    if (guid.IsCreature())
    {
        Creature* creature = GetCreature(guid);
        if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        {
            if (creature->m_loot->GetGoldAmount() > 0)
            {
                return true;
            }
            LootItemList lootItemList = {};

            if (creature->m_loot)
                creature->m_loot->GetLootItemsListFor(bot, lootItemList);

            if (HasNotFullStacksInBagsForLootItems(lootItemList))
            {
                return true;
            }
        }
    }
    else if (wo->IsGameObject())
    {
        GameObject* go = GetGameObject(guid);
        if (go)
        {
            LootItemList lootItemList = {};

            if (go->m_loot)
                go->m_loot->GetLootItemsListFor(bot, lootItemList);

            if (HasNotFullStacksInBagsForLootItems(lootItemList))
            {
                return true;
            }
        }
    }

    return false;
}

void PlayerbotAI::InventoryIterateItemsInBags(IterateItemsVisitor* visitor)
{
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            if (!visitor->Visit(pItem))
                return;

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            if (!visitor->Visit(pItem))
                return;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                if (Item* pItem = pBag->GetItemByPos(j))
                    if (!visitor->Visit(pItem))
                        return;
}

void PlayerbotAI::InventoryIterateItemsInEquip(IterateItemsVisitor* visitor)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; slot++)
    {
        Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pItem)
            continue;

        if (!visitor->Visit(pItem))
            return;
    }
}

void PlayerbotAI::InventoryIterateItemsInBank(IterateItemsVisitor* visitor)
{
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; slot++)
    {
        Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pItem)
            continue;

        if (!visitor->Visit(pItem))
            return;
    }

    for (int i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pBag)
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* pItem = pBag->GetItemByPos(j))
                    {
                        if (!pItem)
                            continue;

                        if (!visitor->Visit(pItem))
                            return;
                    }
                }
            }
        }
    }

}

void PlayerbotAI::InventoryIterateItemsInBuyBack(IterateItemsVisitor* visitor)
{
    for (uint8 slot = BUYBACK_SLOT_START; slot < BUYBACK_SLOT_END; slot++)
    {
        Item* pItem = bot->GetItemFromBuyBackSlot(slot);
        if (!pItem)
            continue;

        if (!visitor->Visit(pItem))
            return;
    }
}

bool compare_items(const ItemPrototype* proto1, const ItemPrototype* proto2)
{
    if (proto1->Class != proto2->Class)
        return proto1->Class > proto2->Class;

    if (proto1->SubClass != proto2->SubClass)
        return proto1->SubClass < proto2->SubClass;

    if (proto1->Quality != proto2->Quality)
        return proto1->Quality < proto2->Quality;

    if (proto1->ItemLevel != proto2->ItemLevel)
        return proto1->ItemLevel > proto2->ItemLevel;

    return false;
}

bool compare_items_by_level(const Item* item1, const Item* item2)
{
    return compare_items(item1->GetProto(), item2->GetProto());
}

void PlayerbotAI::InventoryTellItems(Player* player, std::map<uint32, int> itemMap, std::map<uint32, bool> soulbound)
{
    std::list<ItemPrototype const*> items;
    for (std::map<uint32, int>::iterator i = itemMap.begin(); i != itemMap.end(); i++)
    {
        items.push_back(sObjectMgr.GetItemPrototype(i->first));
    }

    items.sort(compare_items);

    uint32 oldClass = -1;
    for (std::list<ItemPrototype const*>::iterator i = items.begin(); i != items.end(); i++)
    {
        ItemPrototype const* proto = *i;

        if (proto->Class != oldClass)
        {
            oldClass = proto->Class;
            switch (proto->Class)
            {
            case ITEM_CLASS_CONSUMABLE:
                TellPlayer(player, "--- consumable ---");
                break;
            case ITEM_CLASS_CONTAINER:
                TellPlayer(player, "--- container ---");
                break;
            case ITEM_CLASS_WEAPON:
                TellPlayer(player, "--- weapon ---");
                break;
            case ITEM_CLASS_ARMOR:
                TellPlayer(player, "--- armor ---");
                break;
            case ITEM_CLASS_REAGENT:
                TellPlayer(player, "--- reagent ---");
                break;
            case ITEM_CLASS_PROJECTILE:
                TellPlayer(player, "--- projectile ---");
                break;
            case ITEM_CLASS_TRADE_GOODS:
                TellPlayer(player, "--- trade goods ---");
                break;
            case ITEM_CLASS_RECIPE:
                TellPlayer(player, "--- recipe ---");
                break;
            case ITEM_CLASS_QUIVER:
                TellPlayer(player, "--- quiver ---");
                break;
            case ITEM_CLASS_QUEST:
                TellPlayer(player, "--- quest items ---");
                break;
            case ITEM_CLASS_KEY:
                TellPlayer(player, "--- keys ---");
                break;
            case ITEM_CLASS_MISC:
                TellPlayer(player, "--- other ---");
                break;
            }
        }

        InventoryTellItem(player, proto, itemMap[proto->ItemId], soulbound[proto->ItemId]);
    }
}

void PlayerbotAI::InventoryTellItem(Player* player, ItemPrototype const* proto, int count, bool soulbound)
{
    std::ostringstream out;
    out << GetChatHelper()->formatItem(proto, count);
    if (soulbound)
        out << " (soulbound)";
    TellPlayer(player, out.str());
}

#define VISIT_MASK(visitmask) \
    InventoryIterateItems(&visitor, visitmask);\
    found.insert(visitor.GetResult().begin(), visitor.GetResult().end())

#define VISIT \
    InventoryIterateItems(&visitor, mask);\
    found.insert(visitor.GetResult().begin(), visitor.GetResult().end())

#define RETURN_SORT_FOUND \
    std::list<Item*> result; \
    for (std::set<Item*>::iterator i = found.begin(); i != found.end(); ++i)\
        result.push_back(*i); \
    result.sort(compare_items_by_level); \
    return result

#define RETURN_FOUND \
    std::list<Item*> result; \
    for (std::set<Item*>::iterator i = found.begin(); i != found.end(); ++i)\
        result.push_back(*i); \
    return result


std::list<Item*> PlayerbotAI::InventoryParseItems(std::string text, IterateItemsMask mask)
{
    if (text.empty())
    {
        std::list<Item*> result;
        return result;
    }

    AiObjectContext* context = aiObjectContext;

    std::set<Item*> found;
    size_t pos = text.find(" ");
    int count = pos != std::string::npos ? atoi(text.substr(pos + 1).c_str()) : 999;

    //Look for item id's in the command.
    ItemIds ids = GetChatHelper()->parseItems(text);
    if (!ids.empty() && text.find("usage ") == std::string::npos)
    {
        for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
        {
            FindItemByIdVisitor visitor(*i);
            VISIT;
        }

        //We want to stop looking if we found items from links or ids. However if the command is like "all 3" itemId 3 will be found. If so keep looking for more items.
        if (text.find("Hfound:") != -1 || text.find("Hitem:") != -1 || pos == std::string::npos)
        {
            RETURN_SORT_FOUND;
        }
    }

    if (text == "all" || text == "*")
    {
        FindAllItemVisitor visitor;
        VISIT_MASK(mask);
        RETURN_SORT_FOUND;
    }
    else if (text == "equip")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_EQUIP);
        RETURN_SORT_FOUND;
    }
    else if (text == "inventory")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BAGS))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        RETURN_SORT_FOUND;
    }
    else if (text == "bank")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BANK))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BANK);
        RETURN_SORT_FOUND;
    }
    else if (text == "buyback")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BUYBACK))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BUYBACK);
        RETURN_SORT_FOUND;
    }
    else if (text == "food" || text == "conjured food")
    {
        FindFoodVisitor visitor(bot, 11, (text == "conjured food"));
        VISIT;
    }
    else if (text == "drink" || text == "water" || text == "conjured drink" || text == "conjured water")
    {
        FindFoodVisitor visitor(bot, 59, (text == "conjured drink" || text == "conjured water"));
        VISIT;
    }
    else if (text == "mana potion")
    {
        FindPotionVisitor visitor(bot, SPELL_EFFECT_ENERGIZE);
        VISIT;
    }
    else if (text == "healing potion")
    {
        FindPotionVisitor visitor(bot, SPELL_EFFECT_HEAL);
        VISIT;
    }
    else if (text == "mount")
    {
        FindMountVisitor visitor(bot);
        VISIT;
    }
    else if (text == "pet")
    {
        FindPetVisitor visitor(bot);
        VISIT;
    }
    else if (text == "ammo")
    {
        Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (pItem)
        {
            FindAmmoVisitor visitor(bot, pItem->GetProto()->SubClass);
            VISIT;
        }
    }
    else if (text == "recipe")
    {
        FindRecipeVisitor visitor(bot);
        VISIT;
    }
    else if (text == "open")
    {
        FindOpenItemVisitor visitor(bot);
        VISIT;
    }
    else if (text == "quest")
    {
        FindQuestItemVisitor visitor(bot);
        VISIT;
    }
    else if (text.find("usage ") == 0)
    {
        FindItemUsageVisitor visitor(bot, ItemUsage(stoi(text.substr(6))));
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    }
    else if (text == "tradeskill")
    {
        FindItemUsageVisitor visitor(bot, ItemUsage::ITEM_USAGE_SKILL);
        VISIT;
    }
    else if (text == "use")
    {
        FindItemUsageVisitor visitor(bot, ItemUsage::ITEM_USAGE_USE);
        VISIT;
    }
    else if (text == "ah")
    {
        FindItemUsageVisitor visitor(bot, ItemUsage::ITEM_USAGE_AH);
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);

        visitor = FindItemUsageVisitor(bot, ItemUsage::ITEM_USAGE_BROKEN_AH);
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    }
    else if (text == "vendor")
    {
        bool vendorAH = AI_VALUE(uint8, "bag space") > 80 && !urand(0, 10);
        FindVendorItemsVisitor visitor(bot, vendorAH);
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);      
    }

    uint32 quality = GetChatHelper()->parseItemQuality(text);
    if (quality != MAX_ITEM_QUALITY)
    {
        FindItemsByQualityVisitor visitor(quality, count);
        VISIT;
    }

    uint32 itemClass = MAX_ITEM_CLASS, itemSubClass = 0;
    GetChatHelper()->parseItemClass(text, &itemClass, &itemSubClass);
    if (itemClass != MAX_ITEM_CLASS)
    {
        FindItemsByClassVisitor visitor(itemClass, itemSubClass);
        VISIT;
    }

    uint32 fromSlot = GetChatHelper()->parseSlot(text);
    if (fromSlot != EQUIPMENT_SLOT_END)
    {
        if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, fromSlot);
            if (item)
                found.insert(item);
        }

        FindItemBySlotVisitor visitor(bot, fromSlot);
        VISIT;
    }

    ItemIds outfit = InventoryFindOutfitItems(text);
    if (!outfit.empty())
    {
        FindItemByIdsVisitor visitor(outfit);
        VISIT;
    }

    if (found.size() == 0 && quality == MAX_ITEM_QUALITY && itemClass == MAX_ITEM_CLASS && fromSlot == EQUIPMENT_SLOT_END && outfit.empty())
    {
        FindNamedItemVisitor visitor(bot, text);
        VISIT;
    }

    RETURN_SORT_FOUND;
}

uint32 PlayerbotAI::InventoryGetItemCount(FindItemVisitor* visitor, IterateItemsMask mask)
{
    InventoryIterateItems(visitor, mask);
    uint32 count = 0;
    std::list<Item*>& items = visitor->GetResult();
    for (std::list<Item*>::iterator i = items.begin(); i != items.end(); ++i)
    {
        Item* item = *i;
        count += item->GetCount();
    }
    return count;
}

ItemIds PlayerbotAI::InventoryFindOutfitItems(std::string name)
{
    AiObjectContext* context = GetAiObjectContext();
    std::list<std::string>& outfits = AI_VALUE(std::list<std::string>&, "outfit list");
    for (std::list<std::string>::iterator i = outfits.begin(); i != outfits.end(); ++i)
    {
        std::string outfit = *i;
        if (name == InventoryParseOutfitName(outfit))
            return InventoryParseOutfitItems(outfit);
    }
    return std::set<uint32>();
}

void PlayerbotAI::AccelerateRespawn(Creature* creature, float accelMod)
{
    if (!creature)
        return;

    std::string skipReason = "none";
    uint32 nearbyFriendlyPlayers = 0;
    uint32 playersAfterThreshold = 0;
    float accelKnob = 0.0f;
    bool hostileCreature = false;
    bool autoComputedAccel = !accelMod;
    uint32 originalRespawnDelay = creature->GetRespawnDelay() * IN_MILLISECONDS;
    uint32 m_respawnDelay = originalRespawnDelay;
    uint32 m_corpseAccelerationDecayDelay = 0;

    auto logRespawnAccel = [&](bool respawnAccelerated, bool preserveLootCorpse)
    {
        if (!sPlayerbotAIConfig.hasLog("bot_events.csv"))
            return;

        std::ostringstream out;
        out << "entry=" << creature->GetEntry()
            << ",auto_computed=" << (autoComputedAccel ? 1 : 0)
            << ",skip_reason=" << skipReason
            << ",nearby_friendly_players=" << nearbyFriendlyPlayers
            << ",threshold=" << sPlayerbotAIConfig.respawnModThreshold
            << ",players_after_threshold=" << playersAfterThreshold
            << ",players_after_cap=" << playersAfterThreshold
            << ",max_count=" << sPlayerbotAIConfig.respawnModMax
            << ",hostile=" << (hostileCreature ? 1 : 0)
            << ",knob_pct=" << accelKnob
            << ",accel_mod=" << accelMod
            << ",base_respawn_ms=" << originalRespawnDelay
            << ",respawn_ms=" << m_respawnDelay
            << ",corpse_ms=" << m_corpseAccelerationDecayDelay
            << ",accelerated=" << (respawnAccelerated ? 1 : 0)
            << ",preserve_candidate=" << (preserveLootCorpse ? 1 : 0)
            << ",loot=" << (creature->m_loot ? 1 : 0);
        sPlayerbotAIConfig.logEvent(this, "RespawnAccel", creature->GetName(), out.str());
    };

    if (!sPlayerbotAIConfig.respawnModForPlayerBots && HasRealPlayerMaster())
    {
        skipReason = "real_player_master_disabled";
        logRespawnAccel(false, false);
        return;
    }

    if (!sPlayerbotAIConfig.respawnModForInstances && !WorldPosition(creature).isOverworld())
    {
        skipReason = "instance_disabled";
        logRespawnAccel(false, false);
        return;
    }

    AiObjectContext* context = aiObjectContext;
    if (!accelMod)
    {
        if (!sPlayerbotAIConfig.respawnModHostile && !sPlayerbotAIConfig.respawnModNeutral)
        {
            skipReason = "respawn_mods_disabled";
            logRespawnAccel(false, false);
            return;
        }

        uint32 playersNr = AI_VALUE_LAZY(std::list<ObjectGuid>, "nearest friendly players").size() + 1;
        nearbyFriendlyPlayers = playersNr;

        if (playersNr <= sPlayerbotAIConfig.respawnModThreshold)
        {
            skipReason = "below_bot_threshold";
            logRespawnAccel(false, false);
            return;
        }

        playersNr = std::min(playersNr - sPlayerbotAIConfig.respawnModThreshold, sPlayerbotAIConfig.respawnModMax);
        playersAfterThreshold = playersNr;
        hostileCreature = creature->CanAttackOnSight(bot);
        accelKnob = hostileCreature ? sPlayerbotAIConfig.respawnModHostile : sPlayerbotAIConfig.respawnModNeutral;

        accelMod = playersNr * accelKnob * 0.01f;
    }

    if (!accelMod)
    {
        skipReason = "zero_accel_mod";
        logRespawnAccel(false, false);
        return;
    }

    if (accelMod >= 1)
    {
        m_respawnDelay = 0;
        m_corpseAccelerationDecayDelay = 0;
    }
    else
    {
        CreatureData const* data = sObjectMgr.GetCreatureData(creature->GetDbGuid());

        if (!data)
        {
            skipReason = "missing_spawn_data";
            logRespawnAccel(false, false);
            return;
        }

        m_respawnDelay = data->GetRandomRespawnTime() * IN_MILLISECONDS;
        originalRespawnDelay = m_respawnDelay;
        m_corpseAccelerationDecayDelay = MINIMUM_LOOTING_TIME;

        uint32 totalDelay = m_respawnDelay + m_corpseAccelerationDecayDelay;

        if (m_respawnDelay < totalDelay * accelMod)
        {
            if ((m_corpseAccelerationDecayDelay- ((totalDelay * accelMod) - m_respawnDelay)) < 0)
                sLog.outError("m_corpseAccelerationDecayDelay: %d, totalDelay: %d, accelMod: %f, m_respawnDelay: %d", m_corpseAccelerationDecayDelay, totalDelay, accelMod, m_respawnDelay);

            m_corpseAccelerationDecayDelay -= (totalDelay * accelMod) - m_respawnDelay;
            m_respawnDelay = 0;
        }
        else
            m_respawnDelay -= totalDelay * accelMod;
    }

    uint32 respawnDelaySeconds = m_respawnDelay / IN_MILLISECONDS;
    creature->SetRespawnDelay(respawnDelaySeconds, true);

    // Creature death already computed m_respawnTime once, so keep the live respawn clock
    // aligned with the accelerated delay we just applied.
    if (creature->GetRespawnTime() > time(nullptr))
    {
        time_t remainingRespawn = creature->GetRespawnTime() - time(nullptr);
        if (respawnDelaySeconds < static_cast<uint32>(remainingRespawn))
            creature->SetRespawnTime(respawnDelaySeconds);
    }
    else if (respawnDelaySeconds)
    {
        // Death handling can reach this path before the core has written a future
        // respawn timestamp. If we only update when one already exists, the preserved
        // corpse flow later sees respawnTime==0 and treats the spawn as instantly ready.
        creature->SetRespawnTime(respawnDelaySeconds);
    }
    else if (!respawnDelaySeconds)
    {
        creature->SetRespawnTime(0);
    }

    creature->SaveRespawnTime();

    uint32 corpseLootedDelayMs = 0;
    if (sWorld.getConfig(CONFIG_FLOAT_RATE_CORPSE_DECAY_LOOTED) > 0.0f)
        corpseLootedDelayMs = static_cast<uint32>((creature->GetCorpseDelay() * IN_MILLISECONDS) * sWorld.getConfig(CONFIG_FLOAT_RATE_CORPSE_DECAY_LOOTED));
    else if (m_respawnDelay > 0)
        corpseLootedDelayMs = m_respawnDelay / 3;

    bool hasLootPtr = creature->m_loot;
    bool respawnAccelerated = (m_respawnDelay < originalRespawnDelay || !m_respawnDelay);
    bool preserveLootCorpse = respawnAccelerated && (m_respawnDelay > 0) && (m_respawnDelay < corpseLootedDelayMs);
    skipReason = "applied";
    logRespawnAccel(respawnAccelerated, preserveLootCorpse);

    if (!preserveLootCorpse && sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "entry=" << creature->GetEntry()
            << ",loot=" << (hasLootPtr ? 1 : 0)
            << ",accelerated=" << (respawnAccelerated ? 1 : 0);
        sPlayerbotAIConfig.logEvent(this, "RespawnPreserveSkip", creature->GetName(), out.str());
    }

    if (preserveLootCorpse)
    {
        if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
        {
            std::ostringstream out;
            out << "entry=" << creature->GetEntry()
                << ",respawn_time=" << creature->GetRespawnTime()
                << ",delegated_to_core=1";
            sPlayerbotAIConfig.logEvent(this, "RespawnCoreDelegated", creature->GetName(), out.str());
        }

        return;
    }
    m_corpseAccelerationDecayDelay = std::max<uint32>(m_corpseAccelerationDecayDelay, 20 * IN_MILLISECONDS);
    MANGOS_ASSERT(m_corpseAccelerationDecayDelay < 24 * HOUR * static_cast<uint32>(IN_MILLISECONDS));
    creature->SetCorpseDecayTimer(m_corpseAccelerationDecayDelay);

    if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
    {
        std::ostringstream out;
        out << "entry=" << creature->GetEntry() << ",timer_ms=" << m_corpseAccelerationDecayDelay;
        sPlayerbotAIConfig.logEvent(this, "RespawnCorpseTimer", creature->GetName(), out.str());
    }
}

std::list<Unit*> PlayerbotAI::GetAllHostileUnitsAroundWO(WorldObject* wo, float distanceAround)
{
    std::list<Unit*> hostileUnits;
    if (!wo || !wo->IsInWorld())
        return hostileUnits;

    Unit* observer = dynamic_cast<Unit*>(wo);
    if (!observer)
        observer = bot;

    if (!observer || !observer->IsInWorld())
        return hostileUnits;

    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(wo, observer, distanceAround);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(hostileUnits, u_check);
    Cell::VisitAllObjects(wo, searcher, distanceAround);

    //bugs out with players very rarely - returns friendly as hostile to bots

    return hostileUnits;
}

std::list<Unit*> PlayerbotAI::GetAllHostileNPCNonPetUnitsAroundWO(WorldObject* wo, float distanceAround)
{
    std::list<Unit*> hostileUnitsNonPlayers;
    for (auto hostileUnit : GetAllHostileUnitsAroundWO(wo, distanceAround))
    {
        if (!hostileUnit->IsPlayer() && !hostileUnit->IsCorpse())
        {
            if (hostileUnit->IsCreature())
            {
                Creature* creature = GetCreature(hostileUnit->GetObjectGuid());
                if (!creature || (creature && creature->IsPet()))
                {
                    continue;
                }
            }

            hostileUnitsNonPlayers.push_back(hostileUnit);
        }
    }

    return hostileUnitsNonPlayers;
}

void PlayerbotAI::SendDelayedPacket(WorldSession* session, futurePackets futPackets)
{
    std::thread t([session, futPacket = std::move(futPackets)]() mutable {
        for (auto& delayedPacket : futPacket.get())
        {
            if (delayedPacket.second)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayedPacket.second));

            std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(delayedPacket.first));
            session->QueuePacket(std::move(packetPtr));
        }
    });

    t.detach();
}

void PlayerbotAI::ReceiveDelayedPacket(futurePackets futPackets)
{
    PacketHandlingHelper* handler = &botOutgoingPacketHandlers;
    std::thread t([handler, futPackets = std::move(futPackets)]() mutable {
        for (auto& delayedPacket : futPackets.get())
        {            
            handler->AddPacket(delayedPacket.first);
            if(delayedPacket.second)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayedPacket.second));
        }
        });

    t.detach();
}

PlayerbotHolder* PlayerbotAI::GetHolder() const
{
    if (sRandomPlayerbotMgr.IsRandomBot(bot))
        return &sRandomPlayerbotMgr;

    if (bot->GetMaster())
        return static_cast<Player*>(bot->GetMaster())->GetPlayerbotMgr();

    return bot->GetPlayerbotMgr();
}

std::string PlayerbotAI::InventoryParseOutfitName(std::string outfit)
{
    int pos = outfit.find("=");
    if (pos == -1) return "";
    return outfit.substr(0, pos);
}

ItemIds PlayerbotAI::InventoryParseOutfitItems(std::string text)
{
    ItemIds itemIds;

    uint8 pos = text.find("=") + 1;
    while (pos < text.size())
    {
        int endPos = text.find(',', pos);
        if (endPos == -1)
            endPos = text.size();

        std::string idC = text.substr(pos, endPos - pos);
        uint32 id = atol(idC.c_str());
        pos = endPos + 1;
        if (id)
            itemIds.insert(id);
    }

    return itemIds;
}

void PlayerbotAI::Ping(float x, float y)
{
    WorldPacket data(MSG_MINIMAP_PING, (8 + 4 + 4));
    data << bot->GetObjectGuid();
    data << x;
    data << y;

    if (bot->GetGroup())
    {
        bot->GetGroup()->BroadcastPacket(
#ifdef MANGOS
            & data,
#endif
#ifdef CMANGOS
            data,
#endif
            true, -1, bot->GetObjectGuid());
    }
    else
    {
        bot->GetSession()->SendPacket(
#ifdef MANGOS
            & data
#endif
#ifdef CMANGOS
            data
#endif
            );
    }
}

void PlayerbotAI::Poi(float x, float y, std::string icon_name, Player* player, uint32 flags, uint32 icon, uint32 icon_data)
{
    if (!player)
        player = master;

    if (!player)
        return;

    WorldPacket data(SMSG_GOSSIP_POI, (4 + 4 + 4 + 4 + 4 + 10)); // guess size
    data << flags;
    data << x;
    data << y;
    data << icon;
    data << icon_data;
    data << icon_name;

    sServerFacade.SendPacket(player, data);
}

//Find Poison ...Natsukawa
Item* PlayerbotAI::FindPoison() const
{
   // list out items in main backpack
   for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
   {
      Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      if (pItem)
      {
         const ItemPrototype* const pItemProto = pItem->GetProto();

         if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
            continue;

         if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == 6)
            return pItem;
      }
   }
   // list out items in other removable backpacks
   for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
   {
      const Bag* const pBag = (Bag *)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
      if (pBag)
         for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
         {
            Item* const pItem = bot->GetItemByPos(bag, slot);
            if (pItem)
            {
               const ItemPrototype* const pItemProto = pItem->GetProto();

               if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
                  continue;

               if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == 6)
                  return pItem;
            }
         }
   }
   return NULL;
}

Item* PlayerbotAI::FindConsumable(uint32 displayId) const
{
   // list out items in main backpack
   for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
   {
      Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      if (pItem)
      {
         const ItemPrototype* const pItemProto = pItem->GetProto();

         if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
            continue;

         if ((pItemProto->Class == ITEM_CLASS_CONSUMABLE || pItemProto->Class == ITEM_CLASS_TRADE_GOODS) && pItemProto->DisplayInfoID == displayId)
            return pItem;
      }
   }
   // list out items in other removable backpacks
   for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
   {
      const Bag* const pBag = (Bag *)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
      if (pBag)
         for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
         {
            Item* const pItem = bot->GetItemByPos(bag, slot);
            if (pItem)
            {
               const ItemPrototype* const pItemProto = pItem->GetProto();

               if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
                  continue;

               if ((pItemProto->Class == ITEM_CLASS_CONSUMABLE || pItemProto->Class == ITEM_CLASS_TRADE_GOODS) && pItemProto->DisplayInfoID == displayId)
                  return pItem;
            }
         }
   }
   return NULL;
}

Item* PlayerbotAI::FindBandage() const
{
   // list out items in main backpack
   for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
   {
      Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      if (pItem)
      {
         const ItemPrototype* const pItemProto = pItem->GetProto();

         if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
            continue;
#ifdef MANGOSBOT_ZERO
         if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_FOOD)
#else
         if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_BANDAGE)
#endif
            return pItem;
      }
   }
   // list out items in other removable backpacks
   for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
   {
      const Bag* const pBag = (Bag *)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
      if (pBag)
         for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
         {
            Item* const pItem = bot->GetItemByPos(bag, slot);
            if (pItem)
            {
               const ItemPrototype* const pItemProto = pItem->GetProto();

               if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
                  continue;

#ifdef MANGOSBOT_ZERO
               if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_FOOD)
#else
               if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_BANDAGE)
#endif
                  return pItem;
            }
         }
   }
   return nullptr;
}

static const uint32 uPriorizedSharpStoneIds[8] =
{
    ADAMANTITE_SHARPENING_DISPLAYID, FEL_SHARPENING_DISPLAYID, ELEMENTAL_SHARPENING_DISPLAYID, DENSE_SHARPENING_DISPLAYID,
    SOLID_SHARPENING_DISPLAYID, HEAVY_SHARPENING_DISPLAYID, COARSE_SHARPENING_DISPLAYID, ROUGH_SHARPENING_DISPLAYID
};

static const uint32 uPriorizedWeightStoneIds[8] =
{
    ADAMANTITE_WEIGHTSTONE_DISPLAYID, FEL_WEIGHTSTONE_DISPLAYID, ELEMENTAL_SHARPENING_DISPLAYID, DENSE_WEIGHTSTONE_DISPLAYID,
    SOLID_WEIGHTSTONE_DISPLAYID, HEAVY_WEIGHTSTONE_DISPLAYID, COARSE_WEIGHTSTONE_DISPLAYID, ROUGH_WEIGHTSTONE_DISPLAYID
};

/**
 * FindStoneFor()
 * return Item* Returns sharpening/weight stone item eligible to enchant a bot weapon
 *
 * params:weapon Item* the weap�n the function should search and return a enchanting item for
 * return nullptr if no relevant item is found in bot inventory, else return a sharpening or weight
 * stone based on the weapon subclass
 *
 */
Item* PlayerbotAI::FindStoneFor(Item* weapon) const
{
    Item* stone;
    ItemPrototype const* pProto = weapon->GetProto();
    if (pProto && pProto->Class == ITEM_CLASS_WEAPON && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2
        || pProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2
        || pProto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER))
    {
        for (uint8 i = 0; i < countof(uPriorizedSharpStoneIds); ++i)
        {
            stone = FindConsumable(uPriorizedSharpStoneIds[i]);
            if (stone)
                return stone;
        }
    }
    else if (pProto && pProto->Class == ITEM_CLASS_WEAPON && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2))
    {
        for (uint8 i = 0; i < countof(uPriorizedWeightStoneIds); ++i)
        {
            stone = FindConsumable(uPriorizedWeightStoneIds[i]);
            if (stone)
                return stone;
        }
    }

    return nullptr;
}

static const uint32 uPriorizedWizardOilIds[5] =
{
    MINOR_WIZARD_OIL, LESSER_WIZARD_OIL, BRILLIANT_WIZARD_OIL, WIZARD_OIL, SUPERIOR_WIZARD_OIL
};

static const uint32 uPriorizedManaOilIds[4] =
{
   MINOR_MANA_OIL, LESSER_MANA_OIL, BRILLIANT_MANA_OIL, SUPERIOR_MANA_OIL,
};

Item* PlayerbotAI::FindOilFor(Item* weapon) const
{
    Item* oil = nullptr;
    ItemPrototype const* pProto = weapon->GetProto();

    const std::vector<uint32> uPriorizedWizardOilIds = { MINOR_WIZARD_OIL, LESSER_WIZARD_OIL, BRILLIANT_WIZARD_OIL, WIZARD_OIL, SUPERIOR_WIZARD_OIL };
    const std::vector<uint32> uPriorizedManaOilIds = { MINOR_MANA_OIL, LESSER_MANA_OIL, BRILLIANT_MANA_OIL, SUPERIOR_MANA_OIL };

    if (pProto && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || pProto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF || pProto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER))
    {
        for (uint8 i = 0; i < uPriorizedWizardOilIds.size(); i++)
        {
            oil = FindConsumable(uPriorizedWizardOilIds[i]);
            if (!oil && i < uPriorizedManaOilIds.size())
            {
                oil = FindConsumable(uPriorizedManaOilIds[i]);
            }

            if(oil)
            {
                break;
            }
        }
    }
    else if (pProto && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2))
    {
        for (uint8 i = 0; i < uPriorizedManaOilIds.size(); i++)
        {
            oil = FindConsumable(uPriorizedManaOilIds[i]);
            if (!oil && i < uPriorizedWizardOilIds.size())
            {
                oil = FindConsumable(uPriorizedWizardOilIds[i]);
            }

            if (oil)
            {
                break;
            }
        }
    }

    return oil;
}

//  on self
void PlayerbotAI::ImbueItem(Item* item)
{
   ImbueItem(item, TARGET_FLAG_SELF, ObjectGuid());
}

//  item on unit
void PlayerbotAI::ImbueItem(Item* item, Unit* target)
{
   if (!target)
      return;

   ImbueItem(item, TARGET_FLAG_UNIT, target->GetObjectGuid());
}

//  item on equipped item
void PlayerbotAI::ImbueItem(Item* item, uint8 targetInventorySlot)
{
   if (targetInventorySlot >= EQUIPMENT_SLOT_END)
      return;

   Item* const targetItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, targetInventorySlot);
   if (!targetItem)
      return;

   ImbueItem(item, TARGET_FLAG_ITEM, targetItem->GetObjectGuid());
}

// generic item use method
#ifdef MANGOSBOT_ZERO
void PlayerbotAI::ImbueItem(Item* item, uint16 targetFlag, ObjectGuid targetGUID)
#else
void PlayerbotAI::ImbueItem(Item* item, uint32 targetFlag, ObjectGuid targetGUID)
#endif
{
   if (!item)
      return;

   uint8 bagIndex = item->GetBagSlot();
   uint8 slot = item->GetSlot();
   uint8 cast_count = 0;
   ObjectGuid item_guid = item->GetObjectGuid();

   uint32 spellId = 0;
   uint8 spell_index = 0;
   for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
   {
      if (item->GetProto()->Spells[i].SpellId > 0)
      {
         spellId = item->GetProto()->Spells[i].SpellId;
         spell_index = i;
         break;
      }
   }

#ifdef CMANGOS
   std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_USE_ITEM, 20));
#endif
#ifdef MANGOS
   WorldPacket* packet = new WorldPacket(CMSG_USE_ITEM);
#endif

   *packet << bagIndex;
   *packet << slot;
   *packet << spell_index;
#ifdef MANGOSBOT_ZERO
   *packet << targetFlag;
#endif
#ifdef MANGOSBOT_ONE
   *packet << cast_count;
   *packet << item_guid;
   *packet << targetFlag;
#endif
#ifdef MANGOSBOT_TWO
   *packet << spellId << item_guid << uint32(0) << uint8(0);
   *packet << targetFlag;
#endif

#ifdef CMANGOS
   if (targetFlag & (TARGET_FLAG_UNIT | TARGET_FLAG_ITEM | TARGET_FLAG_GAMEOBJECT))
#endif
#ifdef MANGOS
   if (targetFlag & (TARGET_FLAG_UNIT | TARGET_FLAG_ITEM | TARGET_FLAG_OBJECT))
#endif
      *packet << targetGUID.WriteAsPacked();

#ifdef CMANGOS
   bot->GetSession()->QueuePacket(std::move(packet));
#endif
#ifdef MANGOS
   bot->GetSession()->QueuePacket(packet);
#endif
}

void PlayerbotAI::EnchantItemT(uint32 spellid, uint8 slot, Item* item)
{
    Item* pItem = nullptr;
    if (item)
        pItem = item;
    else
        pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

   if (!pItem)
    return;

   if (pItem->GetSlot() != slot)
       return;

#ifdef CMANGOS
   if (pItem->GetOwner() == nullptr)
       return;
#endif

#ifdef MANGOS
   SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellid);
#else
   SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellid);
#endif
   if (!spellInfo)
      return;

   uint32 enchantid = spellInfo->EffectMiscValue[0];
   if (!enchantid)
   {
      sLog.outError("%u: Invalid enchantid %s %s" , enchantid , " report to devs", bot->GetName());
      return;
   }

   if (!((1 << pItem->GetProto()->SubClass) & spellInfo->EquippedItemSubClassMask) &&
      !((1 << pItem->GetProto()->InventoryType) & spellInfo->EquippedItemInventoryTypeMask))
   {

      sLog.outError("%s: items could not be enchanted, wrong item type equipped", bot->GetName());

      return;
   }

#ifdef MANGOSBOT_TWO
   EnchantmentSlot enchantSlot = spellInfo->Effect[0] == SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC ? PRISMATIC_ENCHANTMENT_SLOT : PERM_ENCHANTMENT_SLOT;
#else
   EnchantmentSlot enchantSlot = PERM_ENCHANTMENT_SLOT;
#endif

   bot->ApplyEnchantment(pItem, enchantSlot, false);
   pItem->SetEnchantment(enchantSlot, enchantid, 0, 0);
   bot->ApplyEnchantment(pItem, enchantSlot, true);

   sLog.outDetail("%s: items was enchanted successfully!", bot->GetName());
}

uint32 PlayerbotAI::GetBuffedCount(Player* player, std::string spellname)
{
    Group* group = bot->GetGroup();
    uint32 bcount = 0;

    if (group)
    {
        for (GroupReference *gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();
            if (!member || !member->IsInWorld() && member->GetMapId() != bot->GetMapId())
                continue;

            if (!member->IsInGroup(player, true))
                continue;

            if (HasAura(spellname, member))
                bcount++;
        }
    }
    return bcount;
}

bool PlayerbotAI::CanMove()
{
    // do not allow if not vehicle driver
    if (IsInVehicle() && !IsInVehicle(true))
    {
        return false;
    }

    if (sServerFacade.IsFrozen(bot))
    {
        return false;
    }
    if (sServerFacade.IsInRoots(bot))
    {
        return false;
    }
    if (sServerFacade.IsFeared(bot))
    {
        return false;
    }
    if (sServerFacade.IsCharmed(bot))
    {
        return false;
    }
    if (bot->IsStunned())
    {
        return false;
    }
    if (bot->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
    {
        return false;
    }
    if (bot->IsPolymorphed())
    {
        return false;
    }
    if (bot->IsTaxiFlying())
    {
        return false;
    }
    if (sServerFacade.UnitIsDead(bot) && !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        return false;
    }
    if (bot->IsBeingTeleported())
    {
        return false;
    }
    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        return false;
    }
    if (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CLIENT_CONTROL_LOST))
    {
        return false;
    }
    if (IsJumping())
    {
        return false;
    }
#ifdef MANGOSBOT_ONE
    if (bot->IsFalling())
    {
        return false;
    }
    if (bot->IsJumping())
    {
        return false;
    }
#else
    if (bot->IsFalling())
    {
        return false;
    }
#endif

    MotionMaster& mm = *bot->GetMotionMaster();
    MovementGeneratorType currentMotion = mm.GetCurrentMovementGeneratorType();
    
#ifdef CMANGOS
    if (currentMotion == TAXI_MOTION_TYPE)
    {
        return false;
    }
    if (currentMotion == FALL_MOTION_TYPE)
    {
        return false;
    }
#endif
#ifdef MANGOS
    if (currentMotion == FLIGHT_MOTION_TYPE)
    {
        return false;
    }
#endif
    return true;
}

void PlayerbotAI::StopMoving()
{
    if (bot->IsTaxiFlying())
        return;

    if (IsInVehicle())
        return;

    if (bot->IsBeingTeleportedFar())
        return;

    if (bot->GetTransport())
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_ONTRANSPORT);
    else
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_NONE);

    bot->InterruptMoving(true);
    MovementInfo mInfo = bot->m_movementInfo;
    float x, y, z;
    bot->GetPosition(x, y, z);
    float o = bot->GetPosition().o;
    mInfo.ChangePosition(x, y, z, o);
    WorldPacket data(MSG_MOVE_STOP);
#ifdef MANGOSBOT_TWO
    data << bot->GetObjectGuid().WriteAsPacked();
#endif
    data << mInfo;
    bot->GetSession()->HandleMovementOpcodes(data);

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        bot->GetMotionMaster()->Clear(false, true);
        bot->GetMotionMaster()->MoveIdle();
    }

    // Interrupt-stops freeze the bot at the last mid-spline Z (often ~0.5-1y above ground: navmesh
    // pad) and no gravity ever drops it -> the visible "floats when standing still". Settle now.
    SettleToGround(bot);
}

bool PlayerbotAI::IsInRealGuild()
{
    if (!bot->GetGuildId())
        return false;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    if (!guild)
        return false;

    uint32 leaderAccount = sObjectMgr.GetPlayerAccountIdByGUID(guild->GetLeaderGuid());
    if (!leaderAccount)
        return false;

    return !sPlayerbotAIConfig.IsInRandomAccountList(leaderAccount);
}

bool PlayerbotAI::HasPlayerRelation()
{
    if (HasRealPlayerMaster())
        return true;

    if (IsInRealGuild())
        return true;

    if (IsPlayerFriend())
        return true;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;

    // NOTE: we deliberately do NOT scan sRandomPlayerbotMgr.GetPlayers() here and dereference each
    // real Player to read its social/friend list. This runs on parallel map-update worker threads,
    // and a real player logging out (main thread) frees its Player object -> dereferencing the
    // (snapshotted) pointer segfaulted in Player::GetSocial() (the recurring SIGSEGV under
    // AcceptInvitationAction/AcceptDuelAction->ResetStrategies). A snapshot copy can't fix it: it
    // pins the map structure, not the lifetime of the Player objects it points at. The friend
    // relationship is already established at login (OnBotLogin queries character_social and calls
    // SetPlayerFriend) and is read above via IsPlayerFriend(); a mid-session friend add is picked
    // up on the bot's next login. Correctness cost is negligible; the crash is eliminated.
    return false;
}

void PlayerbotAI::QueueChatResponse(uint32 msgType, ObjectGuid guid1, ObjectGuid guid2, std::string message, std::string chanName, std::string name, bool noDelay)
{
    std::scoped_lock lock(chatRepliesMutex);
    // AI-chat replies were instant (noDelay) — inhumanly fast. Give them reading+thinking time;
    // the 200ms/char typing drip in LinesToPackets then paces the actual sentences.
    chatReplies.push(ChatQueuedReply(msgType, guid1.GetCounter(), guid2.GetCounter(), message, chanName, name, time(0) + (noDelay ? urand(4, 9) : urand(inCombat ? 15 : 10, inCombat ? 30 : 20))));
}

bool PlayerbotAI::PlayAttackEmote(float chanceMultiplier)
{
    auto group = bot->GetGroup();
    if (group) chanceMultiplier /= (group->GetMembersCount() - 1);
    if ((float)urand(0, 10000) * chanceMultiplier < sPlayerbotAIConfig.attackEmoteChance * 10000.0f && bot->IsInCombat())
    {
        std::vector<uint32> sounds;
        sounds.push_back(TEXTEMOTE_OPENFIRE);
        sounds.push_back(305);
        sounds.push_back(307);
        PlaySound(sounds[urand(0, sounds.size() - 1)]);
        return true;
    }

    return false;
}

void PlayerbotAI::QueuePacket(WorldPacket& pkt)
{
    // Penqle WorldPacket has deleted copy-assign; copy-construct + move-assign.
    std::unique_ptr<WorldPacket> packet(new WorldPacket(pkt));
    bot->GetSession()->QueuePacket(std::move(packet));
}

float PlayerbotAI::GetLevelFloat() const
{
    float level = bot->GetLevel();

    uint32 nextLevelXp = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);

    if (!nextLevelXp)
        return level;

    uint32 xp = bot->GetUInt32Value(PLAYER_XP);

    while (xp > nextLevelXp)
    {
        level++;
        xp -= nextLevelXp;

        nextLevelXp = sObjectMgr.GetXPForLevel(level);
    }

    level += float(xp) / float(nextLevelXp);

    return level;
}

bool PlayerbotAI::CanSpellClick(Player* bot, uint32 entry)
{
#ifdef MANGOSBOT_TWO
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(entry);

    if (clickPair.first != clickPair.second)
    {
        for (SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
        {
            if (itr->second.questStart)
            {
                // not in expected required quest state
                if (!bot || ((!itr->second.questStartCanActive || !bot->IsActiveQuest(itr->second.questStart)) && !bot->GetQuestRewardStatus(itr->second.questStart)))
                    return false;
            }

            if (itr->second.questEnd)
            {
                // not in expected forbidden quest state
                if (!bot || bot->GetQuestRewardStatus(itr->second.questEnd))
                    return false;
            }

            return true;
        }
    }
#endif
    return false;
}

bool PlayerbotAI::CanSpellClick(ObjectGuid guid) const
{
#ifdef MANGOSBOT_TWO
    if (!guid.IsCreatureOrVehicle())
        return false;

    Creature* creature = GetAnyTypeCreature(guid);

    if (!creature)
        return CanSpellClick(bot, guid.GetEntry());

    // Check if there are spell click entries for this creature
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(guid.GetEntry());
    if (clickPair.first == clickPair.second)
        return false;

    // Check if any of the spell click entries fit the requirements for this bot
    for (SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        if (itr->second.IsFitToRequirements(bot, creature))
        {
            return true;
        }
    }
#endif
    return false;
}

bool PlayerbotAI::HandleSpellClick(uint32 entry) 
{
#ifdef MANGOSBOT_TWO
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(entry);

    if (clickPair.first == clickPair.second)
        return false;

    Creature* creature = nullptr;

    AiObjectContext* context = aiObjectContext;
    std::list<ObjectGuid> guids = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
    for (auto& guid : guids)
    {
        if (!guid.IsCreatureOrVehicle())
            continue;

        if (guid.GetEntry() != entry)
            continue;

        return HandleSpellClick(guid);

        break;
    }  
#endif
    return false;
}

bool PlayerbotAI::HandleSpellClick(ObjectGuid guid)
{
#ifdef MANGOSBOT_TWO
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(guid.GetEntry());

    if (clickPair.first == clickPair.second)
        return false;

    Creature* creature = nullptr;

    creature = GetAnyTypeCreature(guid);

    if (!creature)
        return false;

    for (SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        if (itr->second.IsFitToRequirements(bot, creature))
        {
            if (sScriptDevAIMgr.OnNpcSpellClick(bot, creature, itr->second.spellId))
                return true;

            Unit* caster = (itr->second.castFlags & 0x1) ? (Unit*)bot : (Unit*)creature;
            Unit* target = (itr->second.castFlags & 0x2) ? (Unit*)bot : (Unit*)creature;

            if (itr->second.spellId)
            {
                caster->CastSpell(target, itr->second.spellId, TRIGGERED_OLD_TRIGGERED);
                return true;
            }
            else
                sLog.outError("WorldSession::HandleSpellClick: npc_spell_click with entry %u has 0 in spell_id. Not handled custom case?", creature->GetEntry());
        }
    }
#endif
    return false;
}
