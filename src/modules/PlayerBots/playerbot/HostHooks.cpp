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
