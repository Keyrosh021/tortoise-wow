// Host-side glue for bot lifecycle and dispatch. Implements:
//   - Player::{Create,Remove}Playerbot{AI,Mgr}, Player::isRealPlayer
//   - Player::UpdatePlayerbotHooks (per-Player tick)
//   - World::{Update,Init}Playerbots* (world-tick driver, startup init)
//   - Player_DispatchBotOutgoing{Packet,ChatCommand} (free functions called
//     from WorldSession; the bot-AI null-check happens here so the host
//     call sites stay unconditional)
//
// Lives in the bot module so it sees both the host headers and the bot
// module's full PlayerbotAI / PlayerbotMgr types — the host declares the
// methods, only the bot module satisfies the linker with real bodies. The
// matching BUILD_PLAYERBOTS=OFF stubs live in src/game/PlayerbotStubs.cpp.

#include "playerbot/playerbot.h"
#include "Objects/Player.h"
#include "World.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/RandomPlayerbotFactory.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "BotDiagnostics.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "PerfStats.h"
#include "strategy/values/SharedValueContext.h"

namespace
{
    bool IsVisiblePriority(ActivePiorityType type)
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

    bool ShouldDeferPlayerbotHook(Player* bot, PlayerbotAI* ai, uint32 accumulatedDiff)
    {
        if (!bot || !ai)
            return false;

        // COLD-DORMANT SKIP (the 10k responsiveness fix): a parked bot that is NOT in the active
        // brain set doesn't need a per-tick hook. Process it at most every ~2s so the map's
        // per-tick budget goes to the ~200 active bots + real players -- otherwise the cycle is
        // diluted across all 10k and the active bots near a player tick every ~2s and look frozen.
        // Active-cohort bots and any bot in combat/attacked are never deferred (instant wake kept).
        if (ai->IsColdDormant() && bot->IsInWorld() && !bot->IsInCombat() && bot->getAttackers().empty()
            && sRandomPlayerbotMgr.IsRandomBot(bot)
            && !sRandomPlayerbotMgr.IsActiveCohort(bot->GetGUIDLow()))
            return accumulatedDiff < 2000u;

        const uint32 pressure = PerfStats::GetBotPressureLevel();
        if (pressure == PerfStats::BOT_PRESSURE_NORMAL)
            return false;

        if (!sRandomPlayerbotMgr.IsRandomBot(bot) || ai->HasActivePlayerMaster() || ai->HasRealPlayerMaster())
            return false;

        if (!ai->IsStateActive(BotState::BOT_STATE_NON_COMBAT))
            return false;

        if (!bot->IsAlive() || bot->IsInCombat() || bot->GetLootGuid() || bot->IsTaxiFlying() ||
            bot->IsBeingTeleported() || bot->IsNonMeleeSpellCasted(true) || bot->isMovingOrTurning())
            return false;

        const uint32 minInterval = pressure >= PerfStats::BOT_PRESSURE_CRITICAL ? 500u : 250u;
        return accumulatedDiff < minInterval;
    }
}

void Player::CreatePlayerbotAI()
{
    if (!m_playerbotAI)
        m_playerbotAI = new PlayerbotAI(this);
}

void Player::RemovePlayerbotAI()
{
    if (m_playerbotAI)
    {
        delete m_playerbotAI;
        m_playerbotAI = nullptr;
    }
}

void Player::CreatePlayerbotMgr()
{
    if (!m_playerbotMgr)
        m_playerbotMgr = new PlayerbotMgr(this);

    // Register this real player with the random-bot manager so bots can detect them
    // (HasPlayerNearby / LOD wake / near-player prioritization all scan
    // sRandomPlayerbotMgr.GetPlayers()). The matching RandomPlayerbotMgr::OnPlayerLogin
    // was dead code -- never called anywhere -- so the tracked-players list stayed empty
    // and NO bot ever saw a real player. This is the one login-time, world-thread,
    // real-player-only hook, so it's the correct (and low-race) place to wire it.
    sRandomPlayerbotMgr.OnPlayerLogin(this);
}

void Player::RemovePlayerbotMgr()
{
    if (m_playerbotMgr)
    {
        // Log out the master's alt bots first; otherwise their PlayerbotAI
        // outlives the mgr and they linger in-world with a dangling master.
        m_playerbotMgr->LogoutAllBots();
        delete m_playerbotMgr;
        m_playerbotMgr = nullptr;
    }
}

bool Player::isRealPlayer() const
{
    return !m_playerbotAI || m_playerbotAI->IsRealPlayer();
}

// World-tick driver. RandomPlayerbotMgr ticks both the login queue and every
// active bot's PlayerbotAI.
void World::UpdatePlayerbotsTick(uint32 diff)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    if (m_pendingRandomBotAutoCreate)
    {
        m_pendingRandomBotAutoCreate = false;
        sLog.outString("Playerbots first world tick refreshing shared travel caches");
        sSharedObjectContext.Reset();
        sTravelMgr.ReloadQuestTravelTable();
        if (sPlayerbotAIConfig.randomBotAutoCreate)
        {
            sLog.outString("Playerbots first world tick invoking RandomPlayerbotFactory::CreateRandomBots()");
            RandomPlayerbotFactory::CreateRandomBots();
        }
        else
        {
            sLog.outString("Playerbots first world tick skipping random bot auto-create because config is disabled");
        }
        return;
    }

    sRandomPlayerbotMgr.UpdateAI(diff);
}

// Per-Player bot tick:
//  - if `this` is a bot (m_playerbotAI != null), tick the AI
//  - if `this` is a real master driving bots (m_playerbotMgr != null), tick
//    the mgr so its alt-bot squad responds to the master's actions
//
// SC_PHASE tags around each call site let the crash handler identify which
// call faulted when bots self-destruct mid-tick (logout, far-teleport).
void Player::UpdatePlayerbotHooks(uint32 diff)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    if (PerfStats::g_suppressPlayerbotHooks)
    {
        m_playerbotSuppressedUpdateMs = std::min<uint32>(m_playerbotSuppressedUpdateMs + diff, 5000);
        PerfStats::RecordPlayerbotHookSuppressed(diff);
        return;
    }

    uint32 const effectiveDiff = std::min<uint32>(diff + m_playerbotSuppressedUpdateMs, 5000);
    if (m_playerbotAI && ShouldDeferPlayerbotHook(this, m_playerbotAI, effectiveDiff))
    {
        m_playerbotSuppressedUpdateMs = effectiveDiff;
        PerfStats::RecordPlayerbotHookSuppressed(diff);
        return;
    }

    uint32 const hookStart = WorldTimer::getMSTime();
    if (m_playerbotAI)
    {
        MapManager::SetContinentUpdatePhase("bot-ai", GetGUIDLow());
        SC_PHASE("Player::UpdatePlayerbotHooks/ai.UpdateAI", GetName());
        m_playerbotSuppressedUpdateMs = 0;
        uint32 const aiStart = WorldTimer::getMSTime();
        bool const visibleLane = IsVisiblePriority(m_playerbotAI->GetPriorityType());
        m_playerbotAI->UpdateAI(effectiveDiff);
        PerfStats::RecordPlayerbotAiLod(WorldTimer::getMSTimeDiffToNow(aiStart), visibleLane);
        MapManager::SetContinentUpdatePhase("players", GetGUIDLow());
    }
    if (m_playerbotMgr)
    {
        MapManager::SetContinentUpdatePhase("bot-mgr", GetGUIDLow());
        SC_PHASE("Player::UpdatePlayerbotHooks/mgr.UpdateAI", GetName());
        uint32 const mgrStart = WorldTimer::getMSTime();
        m_playerbotMgr->UpdateAI(effectiveDiff);
        PerfStats::RecordPlayerbotMgr(WorldTimer::getMSTimeDiffToNow(mgrStart));
        MapManager::SetContinentUpdatePhase("players", GetGUIDLow());
    }
    PerfStats::RecordPlayerbotHook(WorldTimer::getMSTimeDiffToNow(hookStart));
}

// One-shot startup init. Singleton bot managers (RandomPlayerbotMgr,
// PlayerBotLoginMgr, etc.) lazy-instantiate on first reference; we just
// need the config file loaded here. No-op when AiPlayerbot.Enabled=0.
void World::InitPlayerbotsAtStartup()
{
    sPlayerbotAIConfig.Initialize();
}

void World::InitPlayerbotsAfterPlayerInfo()
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    sLog.outString("Playerbots post-player-info init reached: enabled=%u randomBotAutoCreate=%u",
        sPlayerbotAIConfig.enabled ? 1u : 0u, sPlayerbotAIConfig.randomBotAutoCreate ? 1u : 0u);

    m_pendingRandomBotAutoCreate = true;
    sLog.outString("Playerbots post-player-info init armed shared travel-cache refresh for first world tick");
}

// Outgoing-packet interceptor (called from WorldSession::SendPacket). For a
// real player returns false → packet goes to the network normally. For a
// bot Player returns true after handing the packet to the AI to react to
// (group invites → auto-accept, BG status, vendor errors, ...); the AI
// suppresses the network send.
bool Player_DispatchBotOutgoingPacket(Player* player, WorldPacket const& packet)
{
    if (!player) return false;
    PlayerbotAI* ai = player->GetPlayerbotAI();
    if (!ai) return false;
    ai->HandleBotOutgoingPacket(packet);
    return true;
}

// Chat dispatcher: feeds the master's chat to every bot that listens. Without
// this, in-party "+heal" / "stay" / "co" commands don't reach any bot. Bots
// owned by the master and matching random bots both get the message.
void Player_DispatchBotChatCommand(Player* master, uint32 type, std::string const& msg, uint32 lang, std::string const& targetName)
{
    if (!master || !sPlayerbotAIConfig.enabled)
        return;

    if (type == CHAT_MSG_WHISPER)
    {
        std::string normalizedTarget = targetName;
        if (!normalizePlayerName(normalizedTarget))
            return;

        if (PlayerbotMgr* mgr = master->GetPlayerbotMgr())
        {
            mgr->ForEachPlayerbot([&](Player* bot)
            {
                if (bot && bot->GetName() == normalizedTarget)
                    bot->GetPlayerbotAI()->HandleCommand(type, msg, *master, lang);
            });
        }

        sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* bot)
        {
            if (bot && bot->GetName() == normalizedTarget)
                bot->GetPlayerbotAI()->HandleCommand(type, msg, *master, lang);
        });

        return;
    }

    if (PlayerbotMgr* mgr = master->GetPlayerbotMgr())
        mgr->HandleCommand(type, msg, lang);

    sRandomPlayerbotMgr.HandleCommand(type, msg, *master, "", master->GetTeam(), lang);
}

// ---------------- Turtle LFG bot-fill hooks ----------------
// Called from TurtleLFGMgr::Update on the world thread only (GetAllBots iteration is not
// thread-safe elsewhere). Selects idle, level-appropriate random bots by their real spec role,
// ordered tank, healer, dps, and preps them to follow the queuing player.

#include "LFG/TurtleLFGMgr.h"
#include "playerbot/AiFactory.h"

std::vector<ObjectGuid> Playerbot_SelectLfgFill(uint32 minLevel, uint32 maxLevel,
    uint32 needTank, uint32 needHeal, uint32 needDps, std::vector<ObjectGuid> const& exclude)
{
    // Endgame fills (raids, raidfill at 60) take LEVEL-CAP bots ONLY — a 55-60 band let
    // L57s into MC. Raising the floor here covers both the pick-existing path below and
    // the promotion fallback (which then levels+T2-gears candidates to exactly 60).
    if (maxLevel >= sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        minLevel = maxLevel;

    std::vector<ObjectGuid> tanks, heals, dps;

    sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return;
        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            return;
        if (bot->GetGroup() || bot->IsInCombat() || bot->InBattleGround() || bot->InBattleGroundQueue())
            return;
        if (bot->GetLevel() < minLevel || bot->GetLevel() > maxLevel)
            return;
        if (bot->IsHardcore())
            return;
        if (std::find(exclude.begin(), exclude.end(), bot->GetObjectGuid()) != exclude.end())
            return;
        if (!bot->GetPlayerbotAI() || bot->GetPlayerbotAI()->HasRealPlayerMaster())
            return;

        BotRoles roles = AiFactory::GetPlayerRoles(bot);
        if ((roles & BOT_ROLE_TANK) && tanks.size() < needTank + 2)
            tanks.push_back(bot->GetObjectGuid());
        else if ((roles & BOT_ROLE_HEALER) && heals.size() < needHeal + 2)
            heals.push_back(bot->GetObjectGuid());
        else if ((roles & BOT_ROLE_DPS) && dps.size() < needDps + 4)
            dps.push_back(bot->GetObjectGuid());
    });

    if (tanks.size() < needTank || heals.size() < needHeal || dps.size() < needDps)
    {
        // ON-DEMAND PROMOTION: a real player is waiting and the bracket has no suitable bots
        // (fleet may be all low-level). Promote idle bots to the dungeon's band with a full
        // factory randomize (level+gear+spells+talents). Promotion is permanent — it organically
        // builds the high-level pool. Class-filtered per missing role; role verified post-roll.
        // Endgame content (raids: MC/BWL/Ony/AQ/Naxx all cap at 60) must fill with LEVEL 60
        // bots only — min+2 put L57s into MC. Leveling dungeons keep filling at the bracket
        // floor (+2) so a L17 SFK group gets L17-19 bots, not 60s.
        const uint32 maxPlayerLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        const uint32 targetLevel = (maxLevel >= maxPlayerLevel)
            ? maxPlayerLevel
            : std::min(maxLevel, minLevel + 2);
        auto tryPromote = [&](uint32 needed, std::vector<ObjectGuid>& bucket, uint32 roleBit,
                              std::initializer_list<uint8> classes)
        {
            if (bucket.size() >= needed)
                return;
            std::vector<Player*> candidates;
            sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* bot)
            {
                if (candidates.size() >= (needed + 3) * 3)
                    return;
                if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                    return;
                if (!sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup() || bot->IsInCombat() ||
                    bot->InBattleGround() || bot->InBattleGroundQueue() || bot->IsHardcore())
                    return;
                if (!bot->GetPlayerbotAI() || bot->GetPlayerbotAI()->HasRealPlayerMaster())
                    return;
                if (std::find(exclude.begin(), exclude.end(), bot->GetObjectGuid()) != exclude.end())
                    return;
                if (std::find(classes.begin(), classes.end(), bot->getClass()) == classes.end())
                    return;
                candidates.push_back(bot);
            });
            for (Player* bot : candidates)
            {
                if (bucket.size() >= needed)
                    break;
                // DisableRandomLevels guards natural fleet leveling; an on-demand fill promotion
                // is intentional — set the level explicitly, then roll the kit for it.
                if (bot->GetLevel() < targetLevel)
                {
                    bot->GiveLevel(targetLevel);
                    bot->InitTalentForLevel();
                }
                PlayerbotFactory factory(bot, targetLevel);
                factory.Randomize(false, false, true);
                if (targetLevel >= 60)
                {
                    // raid fills must arrive raid-ready: deterministic T2 kit, not random rolls
                    std::string gearFails;
                    const uint32 equipped = PlayerbotGearRaidTier(bot, gearFails);
                    sLog.outString("TurtleLFG: raid-geared %s equipped=%u %s%s", bot->GetName(),
                        equipped, gearFails.empty() ? "" : "FAILS: ", gearFails.c_str());
                }
                BotRoles roles = AiFactory::GetPlayerRoles(bot);
                if (roles & roleBit)
                {
                    bucket.push_back(bot->GetObjectGuid());
                    sLog.outString("TurtleLFG: promoted bot %s to L%u as fill (role %u)",
                        bot->GetName(), targetLevel, roleBit);
                }
            }
        };

        tryPromote(needTank, tanks, BOT_ROLE_TANK, { CLASS_WARRIOR, CLASS_PALADIN, CLASS_DRUID });
        tryPromote(needHeal, heals, BOT_ROLE_HEALER, { CLASS_PRIEST, CLASS_DRUID, CLASS_SHAMAN, CLASS_PALADIN });
        tryPromote(needDps, dps, BOT_ROLE_DPS, { CLASS_MAGE, CLASS_ROGUE, CLASS_HUNTER, CLASS_WARLOCK, CLASS_WARRIOR });
    }

    if (tanks.size() < needTank || heals.size() < needHeal || dps.size() < needDps)
        return {};   // promotion couldn't cover it either (no idle bots at all)

    std::vector<ObjectGuid> out;
    for (uint32 i = 0; i < needTank; ++i) out.push_back(tanks[i]);
    for (uint32 i = 0; i < needHeal; ++i) out.push_back(heals[i]);
    for (uint32 i = 0; i < needDps; ++i) out.push_back(dps[i]);
    return out;
}

// pre-dungeon origins for LFT fills so release can send them back where they came from
static std::mutex s_lfgOriginMx;
static std::unordered_map<uint32, WorldLocation> s_lfgOrigins;

bool Playerbot_GetLfgOrigin(uint32 guidLow, WorldLocation& out)
{
    std::lock_guard<std::mutex> lk(s_lfgOriginMx);
    auto it = s_lfgOrigins.find(guidLow);
    if (it == s_lfgOrigins.end())
        return false;
    out = it->second;
    s_lfgOrigins.erase(it);
    return true;
}

void Playerbot_PrepareLfgBot(ObjectGuid botGuid, ObjectGuid masterGuid)
{
    Player* bot = sObjectAccessor.FindPlayer(botGuid);
    Player* master = sObjectAccessor.FindPlayer(masterGuid);
    if (!bot || !bot->GetPlayerbotAI())
        return;

    {
        std::lock_guard<std::mutex> lk(s_lfgOriginMx);
        s_lfgOrigins[bot->GetGUIDLow()] =
            WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
    }

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    ai->Reset(true);
    if (master)
    {
        ai->SetMaster(master);
        ai->ResetStrategies();
    }
}

uint8 Playerbot_GetRoleOf(Player* player)
{
    if (!player)
        return 4;
    BotRoles roles = AiFactory::GetPlayerRoles(player);
    if (roles & BOT_ROLE_TANK) return 1;
    if (roles & BOT_ROLE_HEALER) return 2;
    return 4;
}

void Playerbot_InstantFillBg(Player* realPlayer, uint32 bgTypeId, uint32 bracketId)
{
    // session handlers run on MAP threads -- only record the request; the world thread fills
    sRandomPlayerbotMgr.QueueInstantFillBg(realPlayer->GetGUIDLow(), bgTypeId, bracketId);
}
