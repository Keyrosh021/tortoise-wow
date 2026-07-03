#include "Config/Config.h"
#include <atomic>

#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/PerformanceMonitor.h"
#include "strategy/values/LastMovementValue.h"
#include "AccountMgr.h"
#include "ObjectMgr.h"
#include "Database/DatabaseEnv.h"
#include "PlayerbotAI.h"
#include "BotDiagnostics.h"  // for SC_LOG
#include "Objects/Player.h"
#include "playerbot/AiFactory.h"
#include "PlayerbotCommandServer.h"
#include "MemoryMonitor.h"

#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "FleeManager.h"
#include "playerbot/ServerFacade.h"

#include "Battlegrounds/BattleGround.h"
#include "Battlegrounds/BattleGroundMgr.h"
#include "Chat/ChannelMgr.h"
#include "Guild/GuildMgr.h"
#include "World/WorldState.h"
#include "PlayerbotLoginMgr.h"
#include "Transports/Transport.h"

#ifndef MANGOSBOT_ZERO
#ifdef CMANGOS
#include "Arena/ArenaTeam.h"
#endif
#ifdef MANGOS
#include "ArenaTeam.h"
#endif
#endif

#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/GuidPosition.h"
#include "Maps/MapManager.h"
#include "Group/Group.h"
#include <iomanip>
#include <unordered_map>
#include <algorithm>
#include <float.h>
#include <chrono>
#include <sstream>
#include <thread>
#include <cstdio>
#include <ctime>
#include <map>
#include <vector>
#include <set>

#if PLATFORM == PLATFORM_WINDOWS
#include "windows.h"
#include "psapi.h"
#endif

using namespace ai;
using namespace MaNGOS;

INSTANTIATE_SINGLETON_1(RandomPlayerbotMgr);

namespace
{
    bool ExecuteRandomBotEventWrite(std::string sql)
    {
        for (uint8 attempt = 0; attempt < 3; ++attempt)
        {
            if (CharacterDatabase.DirectExecute(sql.c_str()))
                return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
        }

        return false;
    }

    uint64 GetSteadyMs()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

#ifdef CMANGOS
#include <boost/thread/thread.hpp>
#endif

#ifdef MANGOS
class PrintStatsThread: public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.PrintStats(); return 0; }
};
#endif
#ifdef CMANGOS
void PrintStatsThread(uint32 requesterGuid)
{
    sRandomPlayerbotMgr.PrintStats(requesterGuid);
}
#endif

void activatePrintStatsThread(uint32 requesterGuid)
{
#ifdef MANGOS
    PrintStatsThread *thread = new PrintStatsThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(PrintStatsThread, requesterGuid);
    t.detach();
#endif
}

#ifdef MANGOS
class CheckBgQueueThread : public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.CheckBgQueue(); return 0; }
};
#endif
#ifdef CMANGOS
void CheckBgQueueThread()
{
    sRandomPlayerbotMgr.CheckBgQueue();
}
#endif

void activateCheckBgQueueThread()
{
#ifdef MANGOS
    CheckBgQueueThread *thread = new CheckBgQueueThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(CheckBgQueueThread);
    t.detach();
#endif
}

#ifdef MANGOS
class CheckLfgQueueThread : public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.CheckLfgQueue(); return 0; }
};
#endif
#ifdef CMANGOS
void CheckLfgQueueThread()
{
    sRandomPlayerbotMgr.CheckLfgQueue();
}
#endif

void activateCheckLfgQueueThread()
{
#ifdef MANGOS
    CheckLfgQueueThread *thread = new CheckLfgQueueThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(CheckLfgQueueThread);
    t.detach();
#endif
}

#ifdef MANGOS
class CheckPlayersThread : public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.CheckPlayers(); return 0; }
};
#endif
#ifdef CMANGOS
void CheckPlayersThread()
{
    sRandomPlayerbotMgr.CheckPlayers();
}
#endif

void activateCheckPlayersThread()
{
#ifdef MANGOS
    CheckPlayersThread *thread = new CheckPlayersThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(CheckPlayersThread);
    t.detach();
#endif
}

class botPIDImpl
{
public:
    botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd);
    ~botPIDImpl();
    double calculate(double setpoint, double pv);
    void adjust(double Kp, double Ki, double Kd) { _Kp = Kp; _Ki = Ki; _Kd = Kd; }
    void reset() { _integral = 0; }

private:
    double _dt;
    double _max;
    double _min;
    double _Kp;
    double _Ki;
    double _Kd;
    double _pre_error;
    double _integral;
};


botPID::botPID(double dt, double max, double min, double Kp, double Ki, double Kd)
{
    pimpl = new botPIDImpl(dt, max, min, Kp, Ki, Kd);
}
void botPID::adjust(double Kp, double Ki, double Kd)
{
    pimpl->adjust(Kp, Ki, Kd);
}
void botPID::reset()
{
    pimpl->reset();
}
double botPID::calculate(double setpoint, double pv)
{
    return pimpl->calculate(setpoint, pv);
}
botPID::~botPID()
{
    delete pimpl;
}


/**
 * Implementation
 */
botPIDImpl::botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd) :
    _dt(dt),
    _max(max),
    _min(min),
    _Kp(Kp),
    _Ki(Ki),
    _Kd(Kd),
    _pre_error(0),
    _integral(0)
{
}

double botPIDImpl::calculate(double setpoint, double pv)
{

    // Calculate error
    double error = setpoint - pv;

    // Proportional term
    double Pout = _Kp * error;

    // Integral term
    _integral += error * _dt;

    double Iout = _Ki * _integral;

    // Derivative term
    double derivative = (error - _pre_error) / _dt;
    double Dout = _Kd * derivative;

    // Calculate total output
    double output = Pout + Iout + Dout;

    // Restrict to max/min
    if (output > _max)
    {
        output = _max;
        _integral -= error * _dt; //Stop integral buildup at max
    }
    else if (output < _min)
    {
        output = _min;
        _integral -= error * _dt; //Stop integral buildup at min
    }

    // Save error to previous error
    _pre_error = error;

    return output;
}

botPIDImpl::~botPIDImpl()
{
}

RandomPlayerbotMgr::RandomPlayerbotMgr() 
: PlayerbotHolder()
, processTicks(0)
, loginProgressBar(NULL)
{
    StartUpdateWatchdog();

    if (sPlayerbotAIConfig.enabled && sPlayerbotAIConfig.randomBotAutologin)
    {
        sPlayerbotCommandServer.Start();
        PrepareTeleportCache();

        for (int i = BG_BRACKET_ID_FIRST; i < MAX_BATTLEGROUND_BRACKETS; ++i)
        {
            for (int j = BATTLEGROUND_QUEUE_AV; j < MAX_BATTLEGROUND_QUEUE_TYPES; ++j)
            {
                BgPlayers[j][i][0] = 0;
                BgPlayers[j][i][1] = 0;
                BgBots[j][i][0] = 0;
                BgBots[j][i][1] = 0;
                ArenaBots[j][i][0][0] = 0;
                ArenaBots[j][i][0][1] = 0;
                ArenaBots[j][i][1][0] = 0;
                ArenaBots[j][i][1][1] = 0;
                NeedBots[j][i][0] = false;
                NeedBots[j][i][1] = false;
            }
        }

        //1) Proportional: Amount activity is adjusted based on diff being above or below wanted diff. (100 wanted diff & 0.1 p = 150 diff = -5% activity)
        //2) Integral: Same as proportional but builds up each tick. (100 wanted diff & 0.01 i = 150 diff = -0.5% activity each tick)
        //3) Derative: Based on speed of diff. (+5 diff last tick & 0.05 d = -0.25% activity)
        pid.adjust(0.05,0.001,0.05);
        BgCheckTimer = 0;
        LfgCheckTimer = 0;
        PlayersCheckTimer = 0;
        EventTimeSyncTimer = 0;
        OfflineGroupBotsTimer = 0;
        guildsDeleted = false;
        arenaTeamsDeleted = false;

        std::list<uint32> availableBots = GetBots();

        for (auto& bot : availableBots)
        {
            if(GetEventValue(bot,"login"))
                SetEventValue(bot, "login", 0, 0);
        }

#ifndef MANGOSBOT_ZERO
        // load random bot team members
        auto results = CharacterDatabase.PQuery("SELECT guid FROM arena_team_member");
        if (results)
        {
            sLog.outString("Loading arena team bot members...");
            do
            {
                Field* fields = results->Fetch();
                uint32 lowguid = fields[0].GetUInt32();
                arenaTeamMembers.push_back(lowguid);
            } while (results->NextRow());
        }
#endif
        // sync event timers
        SyncEventTimers();

        for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
        {
            if (!sMapStore.LookupEntry(i))
                continue;

            uint32 mapId = sMapStore.LookupEntry(i)->MapID;
            facingFix[mapId] = {};
        }

        showLoginWarning = true;
    }
}

RandomPlayerbotMgr::~RandomPlayerbotMgr()
{
    StopUpdateWatchdog();
}

const char* RandomPlayerbotMgr::GetWatchdogPhaseName(UpdateWatchdogPhase phase)
{
    switch (phase)
    {
        case UpdateWatchdogPhase::Idle: return "idle";
        case UpdateWatchdogPhase::UpdateSessions: return "UpdateSessions";
        case UpdateWatchdogPhase::ScaleBotActivity: return "ScaleBotActivity";
        case UpdateWatchdogPhase::AsyncBotLogin: return "AsyncBotLogin";
        case UpdateWatchdogPhase::AddRandomBots: return "AddRandomBots";
        case UpdateWatchdogPhase::CheckPlayers: return "CheckPlayers";
        case UpdateWatchdogPhase::CheckLfgQueue: return "CheckLfgQueue";
        case UpdateWatchdogPhase::CheckBgQueue: return "CheckBgQueue";
        case UpdateWatchdogPhase::AddOfflineGroupBots: return "AddOfflineGroupBots";
        case UpdateWatchdogPhase::ProcessBotLoop: return "ProcessBotLoop";
        case UpdateWatchdogPhase::LoginQueue: return "LoginQueue";
        case UpdateWatchdogPhase::LoginFreeBots: return "LoginFreeBots";
        case UpdateWatchdogPhase::LogPlayerLocation: return "LogPlayerLocation";
        case UpdateWatchdogPhase::DelayedFacingFix: return "DelayedFacingFix";
        case UpdateWatchdogPhase::MirrorAh: return "MirrorAh";
        case UpdateWatchdogPhase::PerfInit: return "PerfInit";
        case UpdateWatchdogPhase::DatabasePing: return "DatabasePing";
        case UpdateWatchdogPhase::HolderUpdate: return "PlayerbotHolder::UpdateAIInternal";
    }

    return "unknown";
}

uint64 RandomPlayerbotMgr::GetWatchdogSteadyMs()
{
    return GetSteadyMs();
}

void RandomPlayerbotMgr::StartUpdateWatchdog()
{
    m_updateWatchdogStop = false;
    m_updateWatchdogPhase = static_cast<uint32>(UpdateWatchdogPhase::Idle);
    m_updateWatchdogPhaseSinceMs = GetWatchdogSteadyMs();
    m_updateWatchdogThread = std::thread([this]()
    {
        uint64 lastLoggedSince = 0;
        while (!m_updateWatchdogStop.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            const auto phase = static_cast<UpdateWatchdogPhase>(m_updateWatchdogPhase.load());
            if (phase == UpdateWatchdogPhase::Idle)
                continue;

            const uint64 phaseSinceMs = m_updateWatchdogPhaseSinceMs.load();
            const uint64 nowMs = GetWatchdogSteadyMs();
            const uint64 ageMs = nowMs > phaseSinceMs ? nowMs - phaseSinceMs : 0;

            if (ageMs < 10000)
                continue;

            if (phaseSinceMs == lastLoggedSince && ageMs < 30000)
                continue;

            lastLoggedSince = phaseSinceMs;
            sLog.outError(
                "RandomPlayerbotMgr watchdog: stuck in %s for " UI64FMTD "ms [available %u|online %u|playersMap " SIZEFMTD "]",
                GetWatchdogPhaseName(phase), ageMs, m_updateWatchdogAvailableBots.load(),
                m_updateWatchdogOnlineBots.load(), players.size());
        }
    });
}

void RandomPlayerbotMgr::StopUpdateWatchdog()
{
    m_updateWatchdogStop = true;
    if (m_updateWatchdogThread.joinable())
        m_updateWatchdogThread.join();
}

void RandomPlayerbotMgr::SetUpdateWatchdogPhase(UpdateWatchdogPhase phase, uint32 availableBotCount, uint32 onlineBotCount)
{
    m_updateWatchdogAvailableBots = availableBotCount;
    m_updateWatchdogOnlineBots = onlineBotCount;
    m_updateWatchdogPhase = static_cast<uint32>(phase);
    m_updateWatchdogPhaseSinceMs = GetWatchdogSteadyMs();
}

int RandomPlayerbotMgr::GetMaxAllowedBotCount()
{
    return GetEventValue(0, "bot_count");
}

inline void print_line(Unit* bot, const std::vector<std::pair<int, int>> line, bool is_sqDist_greater_200)
{
    std::ostringstream out;
    out << bot->GetName() << ",";
    out << std::fixed << std::setprecision(1);
    out << "\"LINESTRING(";
    for (auto& p : line)
    {
        out << p.first << " " << p.second << (&p == &line.back() ? "" : ",");
    }    
    out << ")\",";
    out << bot->GetOrientation() << ",";
    out << std::to_string(bot->getRace()) << ",";
    out << std::to_string(bot->getClass()) << ",";
    out << (is_sqDist_greater_200 ? "1" : "0");
    sPlayerbotAIConfig.log("player_paths.csv", out.str().c_str());
}

inline void print_path(Unit* bot, std::vector<std::pair<int, int>>& log)
{
    std::vector<std::pair<int, int>> line;

    std::pair<int, int> lastP = {0, 0};

    for (auto& p : log)
    {
        if (lastP.first && lastP.second && pow(lastP.first - p.first, 2) + pow(lastP.second - p.second, 2) > 200 * 200)
        {
            if (line.size()>1)
                print_line(bot, line, false);      //Print previous path.
            print_line(bot, {lastP, p}, true); //Print jump.
            line.clear();
        }
        line.push_back(p);
        lastP = p;
    }
    if (line.size() > 1)
        print_line(bot, line, false); //Print remaining path.
}

void RandomPlayerbotMgr::LogPlayerLocation()
{
    botCount = 0;
    activeBots = 0;
    if (sPlayerbotAIConfig.randomBotAutologin)
    {
        ForEachPlayerbot([&](Player* bot) {
            // Guard against bots that are mid-teardown / not in world: dereferencing their
            // PlayerbotAI here raced/segv'd in AllowActivity (crash#3). IsInWorld gates that.
            if (bot && bot->IsInWorld() && bot->GetPlayerbotAI())
            {

                botCount++;
                if (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY))
                {
                    activeBots++;
                }
            }
        });
    }

    for (auto i : GetPlayersCopy())
    {
        Player* bot = i.second;
        if (!bot || !bot->IsInWorld())
            continue;
        if (bot->GetPlayerbotAI())
        {
            botCount++;
            if (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY))
                activeBots++;
        }
    }

    if (sPlayerbotAIConfig.hasLog("player_location.csv"))
    {
        try
        {
            sPlayerbotAIConfig.openLog("player_location.csv", "w");

            if (sPlayerbotAIConfig.hasLog("player_route.csv"))
                sPlayerbotAIConfig.openLog("player_route.csv", "w");

            if (sPlayerbotAIConfig.randomBotAutologin)
            {
                ForEachPlayerbot([&](Player* bot) {
                    std::ostringstream out;
                    out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                    out << "RND" << ",";
                    out << bot->GetName() << ",";
                    out << std::fixed << std::setprecision(2);
                    WorldPosition(bot).printWKT(out);
                    out << bot->GetOrientation() << ",";
                    out << std::to_string(bot->getRace()) << ",";
                    out << std::to_string(bot->getClass()) << ",";
                    out << bot->GetMapId() << ",";
                    out << bot->GetLevel() << ",";
                    out << bot->GetHealth() << ",";
                    out << bot->GetPowerPercent() << ",";
                    out << bot->GetMoney() << ",";

                    if (bot->GetPlayerbotAI())
                    {
                        out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGrouperType())) << ",";
                        out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGuilderType())) << ",";
                        out << (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                        out << (bot->GetPlayerbotAI()->IsActive() ? "active" : "delay") << ",";
                        out << bot->GetPlayerbotAI()->HandleRemoteCommand("state") << ",";
                        PlayerbotAI* ai = bot->GetPlayerbotAI();
                        AiObjectContext* context = ai->GetAiObjectContext();

                        out << (AI_VALUE(bool, "should get money") ? "should get money" : "has enough money") << ",";

                        if (sPlayerbotAIConfig.hasLog("player_route.csv") && WorldPosition(bot))
                        {
                            LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");

                            std::vector<PathNodePoint> fullPath = lastMove.lastPath.getPath();

                            if (!fullPath.empty())
                            {
                                std::vector<std::pair<std::vector<WorldPosition>, bool>> splitPath;

                                bool currentWalkable = fullPath[0].isWalkable();
                                std::vector<WorldPosition> currentSegment;
                                currentSegment.push_back(fullPath[0].point);

                                for (size_t i = 1; i < fullPath.size(); i++)
                                {
                                    bool walkable = fullPath[i].isWalkable();

                                    if (walkable != currentWalkable)
                                    {
                                        // End current segment, start new one beginning with the last point
                                        splitPath.push_back({currentSegment, currentWalkable});
                                        currentSegment.clear();
                                        currentSegment.push_back(fullPath[i - 1].point); // shared junction point
                                        currentWalkable = walkable;
                                    }

                                    currentSegment.push_back(fullPath[i].point);
                                }

                                splitPath.push_back({currentSegment, currentWalkable});

                                uint32 segmentNr = 0;

                                for (auto& [segement, walkable] : splitPath)
                                {
                                    segmentNr++;
                                    std::ostringstream out;
                                    out << bot->GetName() << ",";
                                    out << std::fixed << std::setprecision(1);

                                    out << segmentNr << ",";

                                    WorldPosition().printWKT(segement, out, 1, false);

                                    out << bot->GetOrientation() << ",";
                                    out << std::to_string(bot->getRace()) << ",";
                                    out << std::to_string(bot->getClass()) << ",";
                                    out << (walkable ? "1" : "0") << ",";
                                    out << lastMove.moveEvent.getSource();
                                    sPlayerbotAIConfig.log("player_route.csv", out.str().c_str());
                                }
                            }
                        }
                    }
                    else
                    {
                        out << 0 << "," << 0 << ",err,err,err,err,";
                    }

                    out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                    out << (bot->IsDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive") << ",";

                    if (bot->GetGroup())
                        WorldPosition(bot).printWKT({bot, sObjectMgr.GetPlayer(bot->GetGroup()->GetLeaderGuid())}, out, 1);

                    sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());

                    if (sPlayerbotAIConfig.hasLog("player_paths.csv") && WorldPosition(bot))
                    {
                        auto& botMoveLog = playerBotMoveLog[bot->GetObjectGuid().GetCounter()];

                        std::pair<int32, int32> curDisplayPos = std::make_pair(WorldPosition(bot).getDisplayX(), WorldPosition(bot).getDisplayY());

                        botMoveLog.push_back(curDisplayPos);

                        if (botMoveLog.size() > 100)
                        {
                            print_path(bot, botMoveLog);
                            botMoveLog.clear();
                            botMoveLog.push_back(curDisplayPos); //Start next path at current position.
                        }
                    }
                });
            }

            for (auto i : GetPlayers())
            {
                Player* bot = i.second;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "PLR" << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPercent() << ",";
                out << bot->GetMoney() << ",";
                if (bot->GetPlayerbotAI())
                {
                    out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGrouperType())) << ",";
                    out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGuilderType())) << ",";
                    out << (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (bot->GetPlayerbotAI()->IsActive() ? "active" : "delay") << ",";
                    out << bot->GetPlayerbotAI()->HandleRemoteCommand("state") << ",";
                    PlayerbotAI* ai = bot->GetPlayerbotAI();
                    AiObjectContext* context = ai->GetAiObjectContext();

                    out << (AI_VALUE(bool, "should get money") ? "should get money" : "has enough money") << ",";
                }
                else
                {
                    out << 0 << "," << 0 << ",player,player,player,player,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->IsDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive") << ",";

                if (bot->GetGroup())
                    WorldPosition(bot).printWKT({bot, sObjectMgr.GetPlayer(bot->GetGroup()->GetLeaderGuid())}, out, 1);

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());

                if (sPlayerbotAIConfig.hasLog("player_paths.csv") && WorldPosition(bot))
                {
                    auto& botMoveLog = playerBotMoveLog[bot->GetObjectGuid().GetCounter()];

                    std::pair<int32, int32> curDisplayPos = std::make_pair(WorldPosition(bot).getDisplayX(), WorldPosition(bot).getDisplayY());

                    botMoveLog.push_back(curDisplayPos);

                    if (botMoveLog.size() > 100)
                    {
                        print_path(bot, botMoveLog);
                        botMoveLog.clear();
                        botMoveLog.push_back(curDisplayPos); //Start next path at current position.
                    }
                }
            }
        }
        catch (...)
        {
            return;
            //This is to prevent some thread-unsafeness. Crashes would happen if bots get added or removed.
            //We really don't care here. Just skip a log. Making this thread-safe is not worth the effort.
        }
    }
    if (sPlayerbotAIConfig.hasLog("transport.csv"))
    {
        sPlayerbotAIConfig.openLog("transport.csv", "w");
        for (auto& [mapId, map] : sMapMgr.Maps())
        {
            for (auto& transport : WorldPosition(map->GetId(), 1, 1).getTransports())
            {
                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                if (transport->GetName() == nullptr || transport->GetName()[0] == '\0')
                {
                    GameObjectInfo const* data = sGOStorage.LookupEntry<GameObjectInfo>(transport->GetEntry());
                    out << data->name << ",";
                }
                else
                    out << transport->GetName() << ",";

                out << transport->GetEntry() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(transport).printWKT(out);
                out << transport->GetOrientation();

                sPlayerbotAIConfig.log("transport.csv", out.str().c_str());
            }
        }
    }
}

namespace
{
    template <class Callback>
    uint32 ForEachCircularBotSlice(std::list<uint32> const& bots, size_t& cursor, uint32 maxVisits, Callback&& callback)
    {
        const size_t total = bots.size();
        if (!total || !maxVisits)
            return 0;

        if (cursor >= total)
            cursor %= total;

        auto it = bots.begin();
        std::advance(it, cursor);

        uint32 visited = 0;
        while (visited < maxVisits)
        {
            callback(*it);
            ++visited;
            ++it;
            if (it == bots.end())
                it = bots.begin();
        }

        cursor = (cursor + visited) % total;
        return visited;
    }
}

void RandomPlayerbotMgr::LogNearbyCensus()
{
    // NOTE: RandomPlayerbotMgr::GetPlayers() holds the BOTS this manager owns, not
    // real players. Enumerate the global in-world player map instead and split it
    // into real observers (no PlayerbotAI) and bots (have a PlayerbotAI).
    std::vector<Player*> observers, bots;
    for (auto& it : sObjectAccessor.GetPlayers())
    {
        Player* p = it.second;
        if (!p || !p->IsInWorld())
            continue;
        if (p->GetPlayerbotAI())
            bots.push_back(p);
        else
            observers.push_back(p);
    }
    if (observers.empty() || bots.empty())
        return;

    static const char* CN[] = { "", "War", "Pal", "Hun", "Rog", "Pri", "", "Sha", "Mag", "Lock", "", "Dru" };
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));

    FILE* f = nullptr;    // aggregate census -> logs/nearby_census.csv
    FILE* fb = nullptr;   // per-bot list     -> logs/nearby_bots.csv
    for (Player* player : observers)
    {
        // VARIABLE range = the player's actual render/visibility distance (varies per
        // map/area, dynamic visibilities), not a fixed 120y.
        const float range = player->GetVisibilityDistance();

        uint32 total = 0, dead = 0, fighting = 0, moving = 0, idle = 0, casting = 0, looting = 0;
        // idle breakdown: WHY is an alive, out-of-combat, not-moving bot doing nothing?
        uint32 idleRest = 0, idleLowHp = 0, idleLowMana = 0, idleStuck = 0;
        uint32 sumLevel = 0;
        std::map<uint8, uint32> classCount;

        for (Player* bot : bots)
        {
            if (bot == player || bot->GetMapId() != player->GetMapId())
                continue;
            const float dist = player->GetDistance(bot);
            if (dist > range)
                continue;

            ++total;
            sumLevel += bot->GetLevel();
            classCount[bot->getClass()]++;

            // classify into a single activity label (also drives the per-bot list)
            const char* act;
            if (!bot->IsAlive())                { ++dead;    act = "dead"; }
            else if (bot->IsInCombat())         { ++fighting;
                                                  if (bot->IsNonMeleeSpellCasted(true)) { ++casting; act = "casting"; }
                                                  else act = "fighting"; }
            else if (bot->GetLootGuid())        { ++looting; act = "looting"; }
            else if (bot->isMovingOrTurning())  { ++moving;  act = "moving"; }
            else
            {
                ++idle;
                uint32 maxMana = bot->GetMaxPower(POWER_MANA);
                bool lowMana = maxMana > 0 && (bot->GetPower(POWER_MANA) * 100 < maxMana * 50);
                if (bot->GetStandState() != UNIT_STAND_STATE_STAND) { ++idleRest;    act = "rest"; }
                else if (bot->GetHealthPercent() < 60.0f)           { ++idleLowHp;   act = "lowHP"; }
                else if (lowMana)                                   { ++idleLowMana; act = "lowMana"; }
                else                                                { ++idleStuck;   act = "STUCK"; }
            }

            // per-bot list row: ts,observer,bot,level,class,activity,distance
            if (!fb)
            {
                fb = fopen("logs/nearby_bots.csv", "a");
                if (!fb) fb = fopen("../logs/nearby_bots.csv", "a");
            }
            if (fb)
                fprintf(fb, "%s,%s,%s,%u,%s,%s,%.0f\n",
                    ts, player->GetName(), bot->GetName(), bot->GetLevel(),
                    (bot->getClass() < 12 ? CN[bot->getClass()] : "?"), act, dist);
        }

        if (!total)
            continue;

        if (!f)
        {
            f = fopen("logs/nearby_census.csv", "a");
            if (!f) f = fopen("../logs/nearby_census.csv", "a");
            if (!f) { if (fb) fclose(fb); return; }
        }

        std::ostringstream classes;
        for (auto& c : classCount)
            classes << (c.first < 12 ? CN[c.first] : "?") << ":" << c.second << " ";

        fprintf(f, "%s,%s,map%u,%u,%u,%u,%u,%u,%u,%u,%.0f,%s,rest=%u,lowhp=%u,lowmana=%u,stuck=%u,range=%.0f\n",
            ts, player->GetName(), player->GetMapId(),
            total, fighting, moving, idle, dead, casting, looting,
            (float)sumLevel / total, classes.str().c_str(),
            idleRest, idleLowHp, idleLowMana, idleStuck, range);
    }
    if (f)
        fclose(f);
    if (fb)
        fclose(fb);
}

// Fleet-wide per-bot diagnostics. Runs on the main thread (UpdateAIInternal) every ~60s.
// Writes a per-bot detail row to logs/bot_diag.csv and one aggregate row to
// logs/bot_fleet.csv. READ-ONLY and race-tolerant: only reads atomic-ish unit fields
// (level/class/health/power/flags/standstate/movement) and the long-lived TravelTarget
// status/destination pointers (never the std::string lastAction, which would race with
// the bot's own map-thread update). Built to answer "why are bots stuck and dying?".
// Needed kill-quest creature entries for a bot (incomplete kill objectives only — cheap
// quest-log read; skips item objectives). Used by the diagnostic's "is this bot fighting
// its quest target?" success metric.
static void CollectNeededKillEntries(Player* bot, std::set<uint32>& out)
{
    if (!bot)
        return;
    for (auto const& kv : bot->getQuestStatusMap())
    {
        if (kv.second.m_status != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr.GetQuestTemplate(kv.first);
        if (!quest)
            continue;
        for (uint32 o = 0; o < QUEST_OBJECTIVES_COUNT; ++o)
            if (quest->ReqCreatureOrGOCount[o] &&
                kv.second.m_creatureOrGOcount[o] < quest->ReqCreatureOrGOCount[o] &&
                quest->ReqCreatureOrGOId[o] > 0)
                out.insert(uint32(quest->ReqCreatureOrGOId[o]));
    }
}

void RandomPlayerbotMgr::LogBotDiagSample()
{
    std::vector<Player*> bots;
    for (auto& it : sObjectAccessor.GetPlayers())
    {
        Player* p = it.second;
        if (p && p->IsInWorld() && p->GetPlayerbotAI())
            bots.push_back(p);
    }
    if (bots.empty())
        return;

    FILE* fd = fopen("logs/bot_diag.csv", "a");
    if (!fd) fd = fopen("../logs/bot_diag.csv", "a");
    if (!fd) return;

    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));

    // aggregate buckets
    uint32 nDead=0, nCombat=0, nMoving=0, nRest=0;
    uint32 nNoGoal=0, nTravelStuck=0, nWork=0, nCooldown=0, nExpired=0, nPrepareReady=0, nOtherIdle=0;
    uint32 nLowHp=0; uint64 sumLevel=0;
    // OUTCOME scorecard (ROSTER-IMMUNE): fleet-average level was confounded by RandomPlayerbotMgr
    // rotating bots in/out. Instead sum each PRESENT bot's POSITIVE gain in (level + fraction into
    // level) and money since the previous diag, via a persistent per-GUID map -> true questing/xp/
    // gold throughput that bot turnover can't fake. These + credits_min become the learning reward.
    static std::unordered_map<uint32,double> s_lastProg;
    static std::unordered_map<uint32,uint64> s_lastMoney;
    double gainedProgress = 0.0, gainedMoney = 0.0;
    // HESITATION snapshot: melee bot in combat with an intended current target IN MELEE RANGE
    // but NOT actually swinging it = "stands there with a target, not attacking" (the user's #1
    // visible complaint). nCombatMelee = denominator (such bots in melee range of their target).
    uint32 nHesitate=0, nCombatMelee=0;
    // success metric: of bots that have a kill-quest, how many are actually fighting that
    // quest's target creature (vs idling / fighting something else)?
    uint32 nHasKillQuest=0, nFightQuestTarget=0;
    // PROOF of target availability: for a sample of NON-combat kill-quest bots, is a LIVE
    // instance of their target creature actually near them (that they fail to engage)?
    uint32 targetSearchSamples=0, nIdleKQ=0, nTgtNone=0, nTgtLe60=0, nTgtLe200=0, nTgtLe500=0;
    FILE* fts = fopen("logs/target_search.csv", "a");
    if (!fts) fts = fopen("../logs/target_search.csv", "a");
    // UNDILUTED engagement: of bots whose quest mob is actually ALIVE within 80yd (i.e. they are
    // at their quest spot), how many are engaged with it? + group cohesion: are grouped members
    // staying near their leader (follow working) or scattered?
    uint32 nNearMob=0, nEngagedNearMob=0;
    uint32 nGrpMember=0, nGrpFollowing=0; uint64 sumLeaderDist=0;
    uint32 nGrpLeaders=0, nFullGroups=0; uint64 sumGroupSize=0;
    uint32 nQuestBots=0; uint64 sumLogQuests=0, sumIncompleteQuests=0;   // quest-starvation tracking
    uint32 nTargetIdle=0;   // bots with a creature TARGETED but NOT attacking it = reaction-lag state

    for (Player* bot : bots)
    {
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        uint8 cls = bot->getClass();
        uint32 lvl = bot->GetLevel();
        sumLevel += lvl;
        {
            const uint32 xp = bot->GetUInt32Value(PLAYER_XP);
            const uint32 nextXp = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
            const double prog = (double)lvl + (nextXp ? (double)xp / (double)nextXp : 0.0);
            const uint64 money = bot->GetMoney();
            const uint32 g = bot->GetGUIDLow();
            auto itp = s_lastProg.find(g);
            if (itp != s_lastProg.end())
            {
                const double d = prog - itp->second;
                if (d > 0.0 && d < 5.0) gainedProgress += d;   // <5 guards level-reset/relog artifacts
            }
            s_lastProg[g] = prog;
            auto itm = s_lastMoney.find(g);
            if (itm != s_lastMoney.end())
            {
                const int64 d = (int64)money - (int64)itm->second;
                if (d > 0) gainedMoney += (double)d;
            }
            s_lastMoney[g] = money;
        }
        bool alive  = bot->IsAlive();
        bool combat = bot->IsInCombat();
        bool moving = bot->isMovingOrTurning();
        bool sit    = bot->GetStandState() != UNIT_STAND_STATE_STAND;
        float hpPct = bot->GetHealthPercent();
        uint32 maxMana = bot->GetMaxPower(POWER_MANA);
        float manaPct = maxMana ? (bot->GetPower(POWER_MANA) * 100.0f / maxMana) : -1.0f;

        // travel target (long-lived ManualSetValue pointer; status/dest are race-tolerant)
        TravelTarget* tt = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        bool hasDest = tt && tt->GetDestination();
        uint8 status = tt ? (uint8)tt->GetStatus() : 0;
        uint32 moveRetry = tt ? tt->GetRetryCount(true) : 0;
        uint32 extRetry  = tt ? tt->GetRetryCount(false) : 0;
        float tdist = (tt && hasDest && tt->GetPosition()) ? tt->Distance(bot) : -1.0f;

        // HESITATION: a melee bot in combat whose intended "current target" is in melee range
        // but it isn't actually swinging that target. Runs on the main thread after map updates
        // (no concurrent bot update), same as the GetVictim()/travel reads below.
        if (combat && alive && ai && !ai->IsRanged(bot))
        {
            Unit* curTgt = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            if (curTgt && bot->CanReachWithMeleeAttack(curTgt))
            {
                ++nCombatMelee;
                const bool swinging = (bot->GetVictim() == curTgt) && bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                if (!swinging)
                    ++nHesitate;
            }
        }

        // classification (for the aggregate)
        const char* cat;
        if (!alive)            { ++nDead;   cat = "dead"; }
        else if (combat)       { ++nCombat; cat = "combat"; }
        else if (moving)       { ++nMoving; cat = "moving"; }
        else if (sit)          { ++nRest;   cat = "rest"; }
        else
        {
            // alive, out of combat, standing still — WHY? use travel status
            switch ((TravelStatus)status)
            {
                case TravelStatus::TRAVEL_STATUS_TRAVEL:   ++nTravelStuck;  cat = "travel_notmoving"; break;
                case TravelStatus::TRAVEL_STATUS_WORK:     ++nWork;         cat = "work"; break;
                case TravelStatus::TRAVEL_STATUS_COOLDOWN: ++nCooldown;     cat = "cooldown"; break;
                case TravelStatus::TRAVEL_STATUS_EXPIRED:  ++nExpired;      cat = "expired"; break;
                case TravelStatus::TRAVEL_STATUS_PREPARE:
                case TravelStatus::TRAVEL_STATUS_READY:    ++nPrepareReady; cat = "prepare"; break;
                default:
                    if (!hasDest) { ++nNoGoal;    cat = "no_goal"; }
                    else          { ++nOtherIdle; cat = "idle_other"; }
                    break;
            }
        }
        if (alive && hpPct < 35.0f) ++nLowHp;

        // success metric + PROOF of target availability.
        if (alive)
        {
            // reaction-lag: bot has a CREATURE targeted but isn't in combat with it -> it decided
            // to fight but hasn't executed yet (the decision->action gap).
            if (bot->GetSelectionGuid().IsCreature() && !combat)
                ++nTargetIdle;

            // quest-starvation tracking: how many quests does the bot ACTUALLY hold in its live log?
            {
                ++nQuestBots;
                for (uint8 qslot = 0; qslot < MAX_QUEST_LOG_SIZE; ++qslot)
                {
                    uint32 lq = bot->GetQuestSlotQuestId(qslot);
                    if (!lq) continue;
                    ++sumLogQuests;
                    if (bot->GetQuestStatus(lq) == QUEST_STATUS_INCOMPLETE)
                        ++sumIncompleteQuests;
                }
            }

            std::set<uint32> needed;
            CollectNeededKillEntries(bot, needed);
            if (!needed.empty())
            {
                ++nHasKillQuest;
                // UNDILUTED engagement: is the bot actually AT its quest mob (one alive within
                // 80yd)? and if so, is it engaged with it (in combat, within ~15yd)?
                {
                    float nd = 1e9f;
                    for (uint32 e : needed)
                    {
                        std::list<Creature*> fnd;
                        bot->GetCreatureListWithEntryInGrid(fnd, e, 80.0f);
                        for (Creature* c : fnd)
                            if (c && c->IsAlive())
                            {
                                float d = bot->GetDistance(c);
                                if (d < nd) nd = d;
                            }
                    }
                    if (nd < 1e8f)
                    {
                        ++nNearMob;
                        if (combat && nd <= 15.0f) ++nEngagedNearMob;
                    }
                }
                if (combat)
                {
                    if (Unit* victim = bot->GetVictim())
                        if (needed.count(victim->GetEntry()))
                            ++nFightQuestTarget;
                }
                else if (targetSearchSamples < 120)
                {
                    // PROVE it: is a LIVE instance of this bot's target creature actually within
                    // grid range (that it's failing to engage)? Searches loaded grids around the
                    // bot for each needed entry and records the nearest ALIVE one.
                    ++targetSearchSamples;
                    ++nIdleKQ;
                    float nearest = -1.0f;      // nearest ALIVE target (any, incl tapped)
                    float nearestEng = -1.0f;   // nearest ENGAGEABLE (bot's own IsPossibleTarget: free/valid)
                    for (uint32 e : needed)
                    {
                        std::list<Creature*> found;
                        bot->GetCreatureListWithEntryInGrid(found, e, 500.0f);
                        for (Creature* c : found)
                            if (c && c->IsAlive())
                            {
                                float dd = bot->GetDistance(c);
                                if (nearest < 0.0f || dd < nearest) nearest = dd;
                                if (ai::PossibleAttackTargetsValue::IsPossibleTarget(c, bot, 500.0f, false))
                                    if (nearestEng < 0.0f || dd < nearestEng) nearestEng = dd;
                            }
                    }
                    // bucket on the ENGAGEABLE distance: a free, valid target the bot SHOULD fight
                    if (nearestEng < 0.0f)         ++nTgtNone;   // no engageable target (alive ones are tapped/unreachable)
                    else if (nearestEng <= 60.0f)  ++nTgtLe60;   // free target <60yd but bot idle = BUG
                    else if (nearestEng <= 200.0f) ++nTgtLe200;
                    else                           ++nTgtLe500;
                    if (fts)
                        fprintf(fts, "%s,%s,%u,%u,%s,zone%u,nearest_alive=%.0f,nearest_engageable=%.0f\n",
                            ts, bot->GetName(), lvl, cls, cat, bot->GetZoneId(), nearest, nearestEng);
                }
            }

            // group cohesion: are grouped NON-leader bots staying near their leader (follow
            // working) or scattered? Measures whether the cohesion fix actually keeps them together.
            if (Group* g = bot->GetGroup())
            {
                ObjectGuid leaderGuid = g->GetLeaderGuid();
                if (leaderGuid != bot->GetObjectGuid())
                {
                    ++nGrpMember;
                    if (Player* leader = sObjectMgr.GetPlayer(leaderGuid))
                        if (leader->IsInWorld() && leader->GetMapId() == bot->GetMapId())
                        {
                            float ld = bot->GetDistance(leader);
                            sumLeaderDist += (uint64)ld;
                            if (ld <= 40.0f) ++nGrpFollowing;
                        }
                }
                else
                {
                    // bot IS the leader -> measure group sizes (are camp groups reaching 5?)
                    ++nGrpLeaders;
                    uint32 sz = g->GetMembersCount();
                    sumGroupSize += sz;
                    if (sz >= 5) ++nFullGroups;
                }
            }
        }

        // per-bot detail row
        fprintf(fd, "%s,%s,%u,%u,%u,%u,%u,%d,%d,%d,%d,%.0f,%.0f,%s,%u,%u,%u,%.0f\n",
            ts, bot->GetName(), bot->GetGUIDLow(), lvl, cls, bot->GetMapId(), bot->GetZoneId(),
            alive?1:0, combat?1:0, moving?1:0, sit?1:0, hpPct, manaPct,
            cat, status, moveRetry, extRetry, tdist);
    }
    fclose(fd);
    if (fts) fclose(fts);

    // aggregate row
    FILE* fa = fopen("logs/bot_fleet.csv", "a");
    if (!fa) fa = fopen("../logs/bot_fleet.csv", "a");
    if (fa)
    {
        uint32 n = (uint32)bots.size();
        fprintf(fa, "%s,%u,%.1f,combat=%u,moving=%u,rest=%u,travel_notmoving=%u,work=%u,cooldown=%u,expired=%u,prepare=%u,no_goal=%u,idle_other=%u,dead=%u,lowhp=%u,has_killquest=%u,fight_questtarget=%u,idle_kq_sampled=%u,tgt_none=%u,tgt_le60=%u,tgt_le200=%u,tgt_le500=%u\n",
            ts, n, n ? (double)sumLevel / n : 0.0,
            nCombat, nMoving, nRest, nTravelStuck, nWork, nCooldown, nExpired, nPrepareReady, nNoGoal, nOtherIdle, nDead, nLowHp,
            nHasKillQuest, nFightQuestTarget,
            nIdleKQ, nTgtNone, nTgtLe60, nTgtLe200, nTgtLe500);
        // undiluted engagement + cohesion (separate line, prefixed COH, same timestamp)
        // credits/shared = real bot kill-quest objective increments since last sample;
        // shared = subset received purely via group-sharing (member, not the killer) -> the grouping payoff.
        extern std::atomic<uint64_t> g_botKillCredits;
        extern std::atomic<uint64_t> g_botSharedCredits;
        static uint64_t lastCredits = 0, lastShared = 0;
        static uint32 lastCreditMs = 0;
        uint64_t curCredits = g_botKillCredits.load(std::memory_order_relaxed);
        uint64_t curShared  = g_botSharedCredits.load(std::memory_order_relaxed);
        uint32 nowMs = WorldTimer::getMSTime();
        uint32 dtMs = lastCreditMs ? (nowMs - lastCreditMs) : 0;
        uint64_t dCredits = curCredits - lastCredits;
        uint64_t dShared  = curShared  - lastShared;
        double perMin = dtMs ? 60000.0 / (double)dtMs : 0.0;
        lastCredits = curCredits; lastShared = curShared; lastCreditMs = nowMs;
        fprintf(fa, "%s,COH,near_mob=%u,engaged_near_mob=%u,grp_members=%u,grp_following40y=%u,avg_leader_dist=%.0f,"
                    "credits=%llu,shared=%llu,credits_min=%.0f,shared_min=%.0f,shared_pct=%.0f,"
                    "groups=%u,avg_grp_size=%.1f,full5_groups=%u,avg_log_quests=%.1f,avg_incomplete=%.1f,"
                    "target_idle=%u\n",
            ts, nNearMob, nEngagedNearMob, nGrpMember, nGrpFollowing,
            nGrpMember ? (double)sumLeaderDist / nGrpMember : 0.0,
            (unsigned long long)dCredits, (unsigned long long)dShared,
            (double)dCredits * perMin, (double)dShared * perMin,
            dCredits ? (100.0 * (double)dShared / (double)dCredits) : 0.0,
            nGrpLeaders, nGrpLeaders ? (double)sumGroupSize / nGrpLeaders : 0.0, nFullGroups,
            nQuestBots ? (double)sumLogQuests / nQuestBots : 0.0,
            nQuestBots ? (double)sumIncompleteQuests / nQuestBots : 0.0,
            nTargetIdle);

        // THRASH line (same timestamp): self-interruption + heal-recast RATES. This is the
        // baseline-before-fix instrument for the decision-thrash work -- interrupt_pct =
        // % of casts that abandoned an in-progress cast; recast_pct = % of heals re-cast within
        // 4s. Watch these fall when the commitment arbiter ships. Per-min from deltas like credits.
        extern std::atomic<uint64_t> g_botCasts;
        extern std::atomic<uint64_t> g_botCastInterrupts;
        extern std::atomic<uint64_t> g_botHealCasts;
        extern std::atomic<uint64_t> g_botHealRecasts;
        extern std::atomic<uint64_t> g_botHealRecastTight;
        extern std::atomic<uint64_t> g_botSelectNewTarget;
        extern std::atomic<uint64_t> g_botSelectNewTargetAlive;
        static uint64_t lastCasts = 0, lastInt = 0, lastHeal = 0, lastRecast = 0, lastTight = 0, lastSnt = 0, lastSntA = 0;
        static uint32 lastThrashMs = 0;
        uint64_t cCasts = g_botCasts.load(std::memory_order_relaxed);
        uint64_t cInt   = g_botCastInterrupts.load(std::memory_order_relaxed);
        uint64_t cHeal  = g_botHealCasts.load(std::memory_order_relaxed);
        uint64_t cRecast= g_botHealRecasts.load(std::memory_order_relaxed);
        uint64_t cTight = g_botHealRecastTight.load(std::memory_order_relaxed);
        uint64_t cSnt   = g_botSelectNewTarget.load(std::memory_order_relaxed);
        uint64_t cSntA  = g_botSelectNewTargetAlive.load(std::memory_order_relaxed);
        uint32 tNowMs = WorldTimer::getMSTime();
        uint32 tDt = lastThrashMs ? (tNowMs - lastThrashMs) : 0;
        uint64_t dCasts = cCasts - lastCasts, dInt = cInt - lastInt, dHeal = cHeal - lastHeal, dRecast = cRecast - lastRecast, dTight = cTight - lastTight;
        uint64_t dSnt = cSnt - lastSnt, dSntA = cSntA - lastSntA;
        double tPerMin = tDt ? 60000.0 / (double)tDt : 0.0;
        lastCasts = cCasts; lastInt = cInt; lastHeal = cHeal; lastRecast = cRecast; lastTight = cTight; lastSnt = cSnt; lastSntA = cSntA; lastThrashMs = tNowMs;
        fprintf(fa, "%s,THRASH,casts_min=%.0f,interrupts_min=%.0f,interrupt_pct=%.1f,heals_min=%.0f,recasts_min=%.0f,recast_pct=%.1f,tight_pct=%.1f,selectnew_min=%.0f,selectnew_alive_pct=%.1f,hesitate_pct=%.1f,hesitate_n=%u\n",
            ts,
            (double)dCasts * tPerMin, (double)dInt * tPerMin,
            dCasts ? (100.0 * (double)dInt / (double)dCasts) : 0.0,
            (double)dHeal * tPerMin, (double)dRecast * tPerMin,
            dHeal ? (100.0 * (double)dRecast / (double)dHeal) : 0.0,
            dHeal ? (100.0 * (double)dTight / (double)dHeal) : 0.0,
            (double)dSnt * tPerMin,
            dSnt ? (100.0 * (double)dSntA / (double)dSnt) : 0.0,
            nCombatMelee ? (100.0 * (double)nHesitate / (double)nCombatMelee) : 0.0,
            nCombatMelee);

        // OUTCOME line (same timestamp): the user's real KPIs as fleet RATES, robust over long
        // windows. levels_min = sum-of-(level+xp%) gained per minute across the fleet; xp_min_per_bot
        // = levels/min/bot (throughput); gold_min = copper gained per minute; dead = bots currently
        // dead (death pressure). This is the scorecard we drive UP every run and the learning reward.
        {
            static uint32 lastOutMs = 0;
            uint32 oNowMs = WorldTimer::getMSTime();
            uint32 oDt = lastOutMs ? (oNowMs - lastOutMs) : 0;
            double oPerMin = oDt ? 60000.0 / (double)oDt : 0.0;
            uint32 n2 = (uint32)bots.size();
            const bool warm = lastOutMs != 0;   // first diag only seeds the per-GUID map
            lastOutMs = oNowMs;
            fprintf(fa, "%s,OUTCOME,bots=%u,levels_min=%.3f,levels_min_per_bot=%.4f,gold_min=%.1f,gold_min_per_bot=%.2f,dead=%u,avg_level=%.2f\n",
                ts, n2,
                warm ? gainedProgress * oPerMin : 0.0, (warm && n2) ? gainedProgress * oPerMin / n2 : 0.0,
                warm ? gainedMoney * oPerMin / 10000.0 : 0.0, (warm && n2) ? gainedMoney * oPerMin / 10000.0 / n2 : 0.0,
                nDead, n2 ? (double)sumLevel / n2 : 0.0);
        }
        fclose(fa);
    }
}

// The quest kill entry this bot is CAMPING (a needed kill mob with an alive instance within
// sight). 0 if not parked at a quest mob. Used by FormQuestCampGroups to cluster co-located
// bots who need the SAME mob. Bounded + short-circuits to keep the grid cost small.
static uint32 BotCampKillEntry(Player* bot)
{
    if (!bot)
        return 0;

    std::set<uint32> needed;
    CollectNeededKillEntries(bot, needed);
    if (needed.empty())
        return 0;

    uint32 checked = 0;
    for (uint32 e : needed)
    {
        if (++checked > 8)
            break;

        std::list<Creature*> insts;
        bot->GetCreatureListWithEntryInGrid(insts, e, sPlayerbotAIConfig.sightDistance);
        for (Creature* c : insts)
            if (c && c->IsAlive())
                return e;
    }
    return 0;
}

// CAMP-GROUPING MANAGER (~every 12s, main thread alongside the other diagnostics). Real fix for
// "waiting bots aren't grouped": a central pass that finds ungrouped (or under-full-leader) random
// bots parked at the SAME quest kill mob and within 75yd of each other, and force-forms them into
// groups of up to 5 (GROUP_LOOT). Because they're already co-located inside the 74yd shared-credit
// radius, the group pays off immediately (one member's kill credits all 5) and each bot's own
// group-camp logic keeps them together -- no cross-thread AI mutation needed. This drives
// avg_grp_size toward 5 far faster/harder than the distributed per-bot invite dance.
// File-local (not a member) so it needs no header change / wide recompile.
static void FormQuestCampGroups()
{
    // Cluster inside the 74yd group-credit radius so grouped members get the leader's kill credit
    // immediately (no convergence window). Wider clustering (110yd) was tried and did NOT raise
    // avg group size -- same-mob bots sit ~2-3 per camp area, so 5 are rarely within reach either
    // way -- while making bots wait to group dropped total throughput. 75yd is the safe sweet spot.
    const float CLUSTER = 75.0f;

    // 1) gather camping, groupable random bots keyed by the quest mob they're parked at
    std::unordered_map<uint32, std::vector<Player*>> campers;
    for (auto& it : sObjectAccessor.GetPlayers())
    {
        Player* bot = it.second;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsBeingTeleported())
            continue;
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai || ai->HasRealPlayerMaster() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;
        if (bot->InBattleGround() || bot->InBattleGroundQueue())
            continue;

        if (Group* g = bot->GetGroup())
        {
            if (g->IsRaidGroup() || g->GetMembersCount() >= 5)
                continue;                                   // full / raid -> not a recruiter
            if (!g->IsLeader(bot->GetObjectGuid()))
                continue;                                   // only leaders top up their group
        }

        uint32 entry = BotCampKillEntry(bot);
        if (!entry)
            continue;

        campers[entry].push_back(bot);
    }

    // 2) per quest mob, greedily build groups of up to 5 from co-located campers
    uint32 groupsFormed = 0, botsGrouped = 0;
    for (auto& kv : campers)
    {
        std::vector<Player*>& list = kv.second;
        if (list.size() < 2)
            continue;

        std::vector<bool> used(list.size(), false);
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (used[i])
                continue;

            Player* seed = list[i];
            Group* group = seed->GetGroup();               // null, or an under-full group seed leads
            uint32 size = group ? group->GetMembersCount() : 1;
            used[i] = true;
            if (size >= 5)
                continue;

            bool createdHere = false;
            for (size_t j = 0; j < list.size() && size < 5; ++j)
            {
                if (used[j])
                    continue;

                Player* cand = list[j];
                if (cand->GetGroup())
                    continue;                               // only pull ungrouped bots in
                if (cand->GetMapId() != seed->GetMapId())
                    continue;
                if (seed->GetDistance(cand) > CLUSTER)
                    continue;
                if (abs(int32(cand->GetLevel()) - int32(seed->GetLevel())) > 4)
                    continue;                               // keep them within the leave/level gate

                if (!group)
                {
                    group = new Group();
                    if (!group->Create(seed->GetObjectGuid(), seed->GetName()))
                    {
                        delete group;
                        group = nullptr;
                        break;
                    }
                    sObjectMgr.AddGroup(group);
                    group->SetLootMethod(GROUP_LOOT);
                    createdHere = true;
                }

                if (group->AddMember(cand->GetObjectGuid(), cand->GetName()))
                {
                    used[j] = true;
                    ++size;
                    ++botsGrouped;
                }
            }

            if (group)
            {
                group->BroadcastGroupUpdate();
                if (createdHere)
                    ++groupsFormed;
            }
        }
    }

    // Write directly (like the COH line) -- bot_fleet.csv isn't in the hasLog allowlist.
    if (groupsFormed || botsGrouped)
    {
        FILE* fa = fopen("logs/bot_fleet.csv", "a");
        if (!fa) fa = fopen("../logs/bot_fleet.csv", "a");
        if (fa)
        {
            time_t t = time(nullptr); struct tm* lt = localtime(&t);
            char ts[16]; strftime(ts, sizeof(ts), "%H:%M:%S", lt);
            fprintf(fa, "%s,CAMPGROUP,new_groups=%u,bots_grouped=%u,camp_mobs=%u\n",
                ts, groupsFormed, botsGrouped, (uint32)campers.size());
            fclose(fa);
        }
    }
}

// PROOF report (every ~5min): which quest mobs are idle kill-quest bots starving on the most,
// and for each: total spawn locations vs how many are ALIVE right now MAP-WIDE, plus how
// clustered the demanding bots are. Answers "is there a live target anywhere, and are bots
// spread across the spawns?". Writes logs/stuck_mobs.csv. The EntryGuidps copy is heavy, so this
// runs infrequently.
void RandomPlayerbotMgr::LogStuckMobReport()
{
    // 1. demand: which quest creatures do idle (alive, non-combat) kill-quest bots need?
    std::unordered_map<uint32, uint32> demand;
    std::unordered_map<uint32, std::vector<std::pair<float, float>>> botPos;
    std::unordered_map<uint32, std::vector<Player*>> botSample;   // a few demanding bots per entry (for grid search)
    for (auto& it : sObjectAccessor.GetPlayers())
    {
        Player* b = it.second;
        if (!b || !b->IsInWorld() || !b->GetPlayerbotAI() || !b->IsAlive() || b->IsInCombat())
            continue;
        std::set<uint32> needed;
        CollectNeededKillEntries(b, needed);
        for (uint32 e : needed)
        {
            ++demand[e];
            auto& v = botPos[e];
            if (v.size() < 4000) v.emplace_back(b->GetPositionX(), b->GetPositionY());
            auto& s = botSample[e];
            if (s.size() < 25) s.push_back(b);   // grid-search from these to find ALIVE instances near the starving bots
        }
    }
    if (demand.empty())
        return;

    std::vector<std::pair<uint32, uint32>> ranked(demand.begin(), demand.end());
    std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.second > b.second; });

    // 2. global entry->spawns map (one heavy copy; cached value)
    EntryGuidps guidps = GAI_VALUE(EntryGuidps, "entry guidps");

    FILE* f = fopen("logs/stuck_mobs.csv", "a");
    if (!f) f = fopen("../logs/stuck_mobs.csv", "a");
    if (!f) return;
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));

    int reported = 0;
    for (auto const& pr : ranked)
    {
        if (reported++ >= 20) break;
        const uint32 entry = pr.first;
        const uint32 dem = pr.second;
        CreatureInfo const* ci = sObjectMgr.GetCreatureTemplate(entry);
        std::string name = ci ? ci->name : "?";
        for (char& c : name) if (c == ',') c = ' ';

        uint32 totalSpawns = 0, aliveGuidLookup = 0;
        auto git = guidps.find(entry);
        if (git != guidps.end())
            for (AsyncGuidPosition& gp : git->second)
            {
                ++totalSpawns;
                Map* m = sMapMgr.FindMap(gp.getMapId(), 0);
                if (m)
                {
                    Creature* c = m->GetCreature(gp);
                    if (c && c->IsAlive())
                        ++aliveGuidLookup;
                }
            }

        // ALIVE near the starving bots, via the PROVEN grid method: search 500yd around a sample
        // of demanding bots and union the live instances found. This is the trustworthy count.
        std::set<uint32> aliveGuids;
        for (Player* sb : botSample[entry])
        {
            if (!sb || !sb->IsInWorld()) continue;
            std::list<Creature*> found;
            sb->GetCreatureListWithEntryInGrid(found, entry, 500.0f);
            for (Creature* c : found)
                if (c && c->IsAlive())
                    aliveGuids.insert(c->GetGUIDLow());
        }

        // how clustered are the demanding bots? count distinct ~100yd cells they occupy.
        std::set<uint64> cells;
        for (auto const& p : botPos[entry])
            cells.insert(((uint64)(uint32)(int32)(p.first / 100.0f) << 32) ^ (uint32)(int32)(p.second / 100.0f));

        fprintf(f, "%s,%u,%s,demand_bots=%u,total_spawns=%u,alive_near_bots=%u,alive_guidlookup=%u,bot_clusters=%u\n",
            ts, entry, name.c_str(), dem, totalSpawns, (uint32)aliveGuids.size(), aliveGuidLookup, (uint32)cells.size());
    }
    fclose(f);
}

void RandomPlayerbotMgr::UpdateAIInternal(uint32 elapsed, bool minimal)
{
#ifdef MEMORY_MONITOR
    sMemoryMonitor.Print();
    sMemoryMonitor.LogCount(sConfig.GetStringDefault("LogsDir") + "/" + "memory.csv");
#endif

    DrainQueuedBotLogouts(); // map-worker logout requests run here, on the world thread

    const uint32 totalUpdateStart = WorldTimer::getMSTime();
    uint32 updateSessionsTime = 0;
    uint32 scaleActivityTime = 0;
    uint32 asyncLoginTime = 0;
    uint32 addRandomBotsTime = 0;
    uint32 checkPlayersTime = 0;
    uint32 checkLfgTime = 0;
    uint32 checkBgTime = 0;
    uint32 addOfflineGroupBotsTime = 0;
    uint32 processBotLoopTime = 0;
    uint32 loginQueueTime = 0;
    uint32 loginFreeBotsTime = 0;
    uint32 logPlayerLocationTime = 0;
    uint32 delayedFacingFixTime = 0;
    uint32 mirrorAhTime = 0;
    uint32 perfInitTime = 0;
    uint32 databasePingTime = 0;
    uint32 processBotCalls = 0;
    uint32 processBotInspected = 0;
    uint32 updateBotsProcessed = 0;
    uint32 loginQueueInspected = 0;
    uint32 loginAttemptsStarted = 0;

    // Live "what are the bots near me doing" census (~every 3s) -> logs/nearby_census.csv.
    {
        const uint32 nowMs = WorldTimer::getMSTime();
        if (!m_nextCensusMs || nowMs >= m_nextCensusMs)
        {
            m_nextCensusMs = nowMs + 3000;
            LogNearbyCensus();
        }
        if (!m_nextDiagMs || nowMs >= m_nextDiagMs)
        {
            m_nextDiagMs = nowMs + 60000;   // fleet-wide diagnostics every 60s
            LogBotDiagSample();
        }
        if (!m_nextStuckMs || nowMs >= m_nextStuckMs)
        {
            m_nextStuckMs = nowMs + 300000;   // stuck-mob proof report every 5min (heavy)
            LogStuckMobReport();
        }
        static uint32 s_nextCampGroupMs = 0;   // static-local: no header change needed
        if (!s_nextCampGroupMs || nowMs >= s_nextCampGroupMs)
        {
            s_nextCampGroupMs = nowMs + 12000;   // proactively group co-located same-quest bots
            FormQuestCampGroups();
        }
    }

    // tick random bots' sessions so
    // teleport ACKs (HandleTeleportAck) and queued packets get processed.
    // See PlayerbotMgr::UpdateAIInternal for the rationale — same call,
    // same purpose, applied to the random-bot pool.
    uint32 phaseStart = WorldTimer::getMSTime();
    uint32 availableBotCount = 0;
    uint32 onlineBotCount = 0;
    auto setPhaseState = [this, &availableBotCount, &onlineBotCount](UpdateWatchdogPhase phase)
    {
        SetUpdateWatchdogPhase(phase, availableBotCount, onlineBotCount);
    };
    setPhaseState(UpdateWatchdogPhase::UpdateSessions);
    UpdateSessions(elapsed);
    updateSessionsTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    if (!sPlayerbotAIConfig.randomBotAutologin || !sPlayerbotAIConfig.enabled)
        return;

    if (!playersLevel)
        playersLevel = sPlayerbotAIConfig.syncLevelNoPlayer;

    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::ScaleBotActivity);
    ScaleBotActivity();
    scaleActivityTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    if (sPlayerbotAIConfig.asyncBotLogin)
    {
        phaseStart = WorldTimer::getMSTime();
        setPhaseState(UpdateWatchdogPhase::AsyncBotLogin);
        auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "AsyncBotLogin");
        sPlayerBotLoginMgr.Update(players);
        pmo.reset();
        asyncLoginTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
    }

    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (!maxAllowedBotCount || ((uint32)maxAllowedBotCount < sPlayerbotAIConfig.minRandomBots || (uint32)maxAllowedBotCount > sPlayerbotAIConfig.maxRandomBots))
    {
        maxAllowedBotCount = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);
        SetEventValue(0, "bot_count", maxAllowedBotCount,
            urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval, sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
    }

    std::list<uint32> availableBots = GetBots();    
    availableBotCount = availableBots.size();
    onlineBotCount = GetPlayerbotsAmount();
    
    SetAIInternalUpdateDelay(sPlayerbotAIConfig.randomBotUpdateInterval);

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT,
        onlineBotCount < maxAllowedBotCount ? "RandomPlayerbotMgr::Login" : "RandomPlayerbotMgr::UpdateAIInternal");

    if (time(nullptr) > (EventTimeSyncTimer + 30))
        SaveCurTime();

    if (availableBotCount < maxAllowedBotCount && !sWorld.IsShutdowning())
    {
        bool logInAllowed = true;
        if (sPlayerbotAIConfig.randomBotLoginWithPlayer)
        {
            logInAllowed = !players.empty();
        }

        if (logInAllowed)
        {
            phaseStart = WorldTimer::getMSTime();
            setPhaseState(UpdateWatchdogPhase::AddRandomBots);
            AddRandomBots();
            addRandomBotsTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
        }
    }

    if (sPlayerbotAIConfig.syncLevelWithPlayers && players.size())
    {
        if (time(nullptr) > (PlayersCheckTimer + 60))
        {
            phaseStart = WorldTimer::getMSTime();
            setPhaseState(UpdateWatchdogPhase::CheckPlayers);
            CheckPlayers();
            checkPlayersTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
        }
    }

    if (sPlayerbotAIConfig.randomBotJoinLfg && players.size())
    {
        if (time(nullptr) > (LfgCheckTimer + 30))
        {
            phaseStart = WorldTimer::getMSTime();
            setPhaseState(UpdateWatchdogPhase::CheckLfgQueue);
            CheckLfgQueue();
            checkLfgTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
        }
    }

    if (sPlayerbotAIConfig.randomBotJoinBG/* && players.size()*/)
    {
        if (time(nullptr) > (BgCheckTimer + 30))
        {
            phaseStart = WorldTimer::getMSTime();
            setPhaseState(UpdateWatchdogPhase::CheckBgQueue);
            CheckBgQueue();
            checkBgTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
        }
    }

    if (time(nullptr) > (OfflineGroupBotsTimer + 5) && players.size())
    {
        phaseStart = WorldTimer::getMSTime();
        setPhaseState(UpdateWatchdogPhase::AddOfflineGroupBots);
        AddOfflineGroupBots();
        addOfflineGroupBotsTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
    }

    uint32 updateBots = sPlayerbotAIConfig.randomBotsPerInterval == 0 ? UINT32_MAX : sPlayerbotAIConfig.randomBotsPerInterval;

    //Update bots
    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::ProcessBotLoop);
    uint32 processScanBudget = availableBotCount;
    if (!sPlayerbotAIConfig.disableBotOptimizations)
    {
        const uint32 desiredUpdates = sPlayerbotAIConfig.randomBotsPerInterval == 0 ? 128u : sPlayerbotAIConfig.randomBotsPerInterval;
        processScanBudget = std::min<uint32>(availableBotCount, std::max<uint32>(desiredUpdates * 8, 1024u));
    }

    processBotInspected = ForEachCircularBotSlice(availableBots, processBotCursor, processScanBudget, [&](uint32 bot)
    {
        if (!GetPlayerBot(bot))
            return;

        ++processBotCalls;
        if (ProcessBot(bot))
        {
            if (updateBots != UINT32_MAX)
                --updateBots;
            ++updateBotsProcessed;
        }
    });
    processBotLoopTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    uint32 maxLogins = sPlayerbotAIConfig.randomBotsMaxLoginsPerInterval;

    //Log in bots
    if (sRandomPlayerbotMgr.GetDatabaseDelay("CharacterDatabase") < 10 * IN_MILLISECONDS && !sPlayerbotAIConfig.asyncBotLogin && onlineBotCount < maxAllowedBotCount && maxLogins > 0)
    {
        phaseStart = WorldTimer::getMSTime();
        setPhaseState(UpdateWatchdogPhase::LoginQueue);
        uint32 loginScanBudget = availableBotCount;
        if (!sPlayerbotAIConfig.disableBotOptimizations)
            loginScanBudget = std::min<uint32>(availableBotCount, std::max<uint32>(maxLogins * 12, 600u));

        loginQueueInspected = ForEachCircularBotSlice(availableBots, loginBotCursor, loginScanBudget, [&](uint32 bot)
        {
            if (!maxLogins || onlineBotCount >= maxAllowedBotCount)
                return;

            if (GetPlayerBot(bot))
                return;

            if (!eventCache[bot].empty() && GetEventValue(bot, "login"))
            {
                onlineBotCount++;
                return;
            }

            if (GetEventValue(bot, "login"))
                onlineBotCount++;

            if (onlineBotCount >= maxAllowedBotCount)
                return;

            if (ProcessBot(bot)) {
                --maxLogins;
                ++onlineBotCount;
                ++loginAttemptsStarted;
            }
        });
        loginQueueTime = WorldTimer::getMSTimeDiffToNow(phaseStart);
    }

    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::LoginFreeBots);
    LoginFreeBots();
    loginFreeBotsTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    //sLog.outString("[char %d, bot %d]", CharacterDatabase.m_threadBody->m_sqlQueue.size(), CharacterDatabase.m_threadBody->m_sqlQueue.size());
   
    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::LogPlayerLocation);
    LogPlayerLocation();
    logPlayerLocationTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::DelayedFacingFix);
    DelayedFacingFix();
    delayedFacingFixTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::MirrorAh);
    MirrorAh();
    mirrorAhTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::PerfInit);
    for (auto& [mapId, map] : sMapMgr.Maps())
    {
        sPerformanceMonitor.Init(map->GetId(), map->GetInstanceId());
    }
    perfInitTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    //Ping character database.
    phaseStart = WorldTimer::getMSTime();
    setPhaseState(UpdateWatchdogPhase::DatabasePing);
    CharacterDatabase.AsyncPQuery(&RandomPlayerbotMgr::DatabasePing, sWorld.GetCurrentMSTime(), std::string("CharacterDatabase"), "SELECT 1");
    databasePingTime = WorldTimer::getMSTimeDiffToNow(phaseStart);

    const uint32 totalUpdateTime = WorldTimer::getMSTimeDiffToNow(totalUpdateStart);
    if (sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_SESSIONS_UPDATE) &&
        totalUpdateTime > sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_SESSIONS_UPDATE))
    {
        sLog.out(LOG_PERFORMANCE,
            "RandomPlayerbotMgr update: %ums [sessions %ums|scale %ums|asyncLogin %ums|addBots %ums|checkPlayers %ums|checkLfg %ums|checkBg %ums|offlineGroups %ums|processBotLoop %ums|loginQueue %ums|loginFree %ums|logLoc %ums|facing %ums|mirrorAh %ums|perfInit %ums|dbPing %ums] [available %u|online %u|target %u|playersMap " SIZEFMTD "|processInspected %u|processCalls %u|processed %u|loginInspected %u|loginStarts %u]",
            totalUpdateTime, updateSessionsTime, scaleActivityTime, asyncLoginTime, addRandomBotsTime,
            checkPlayersTime, checkLfgTime, checkBgTime, addOfflineGroupBotsTime, processBotLoopTime,
            loginQueueTime, loginFreeBotsTime, logPlayerLocationTime, delayedFacingFixTime,
            mirrorAhTime, perfInitTime, databasePingTime, availableBotCount, onlineBotCount,
            maxAllowedBotCount, players.size(), processBotInspected, processBotCalls, updateBotsProcessed,
            loginQueueInspected, loginAttemptsStarted);
    }

    setPhaseState(UpdateWatchdogPhase::HolderUpdate);
    PlayerbotHolder::UpdateAIInternal(elapsed, minimal);
    SetUpdateWatchdogPhase(UpdateWatchdogPhase::Idle, availableBotCount, onlineBotCount);
}

void RandomPlayerbotMgr::ScaleBotActivity()
{
    float activityPercentage = getActivityPercentage();

    //if (activityPercentage >= 100.0f || activityPercentage <= 0.0f) pid.reset(); //Stop integer buildup during max/min activity

    //    % increase/decrease                   wanted diff                                         , avg diff
    float activityPercentageMod = pid.calculate(sRandomPlayerbotMgr.GetPlayers().empty() ? sPlayerbotAIConfig.diffEmpty : sPlayerbotAIConfig.diffWithPlayer, sWorld.GetAverageDiff());

    activityPercentage = activityPercentageMod + 50;

    //Cap the percentage between 0 and 100.
    activityPercentage = std::max(0.0f, std::min(100.0f, activityPercentage));

    setActivityPercentage(activityPercentage);

    if (sPlayerbotAIConfig.hasLog("activity_pid.csv"))
    {
        double virtualMemUsedByMe = 0;
#if PLATFORM == PLATFORM_WINDOWS
        PROCESS_MEMORY_COUNTERS_EX pmc;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        virtualMemUsedByMe = pmc.PrivateUsage;
#endif

        std::ostringstream out;
        out << sWorld.GetCurrentMSTime() << ", ";

        out << sWorld.GetCurrentDiff() << ",";
        out << sWorld.GetAverageDiff() << ",";
        out << sWorld.GetMaxDiff() << ",";
        out << virtualMemUsedByMe << ",";
        out << activityPercentage << ",";
        out << activityPercentageMod << ",";
        out << activeBots << ",";
        out << GetPlayerbotsAmount() << ",";

        float totalLevel = 0, totalGold = 0, totalGearscore = 0;

        if (sPlayerbotAIConfig.randomBotAutologin)
        {
            ForEachPlayerbot([&](Player* bot)
            {
                if (bot->GetPlayerbotAI()->AllowActivity())
                {
                    std::string bracket = "level:" + std::to_string(bot->GetLevel() / 10);

                    float level = bot->GetPlayerbotAI()->GetLevelFloat();
                    totalLevel += level;
                    float gold = bot->GetMoney() / 10000;
                    totalGold += gold;
                    float gearscore = bot->GetPlayerbotAI()->GetEquipGearScore(bot, false, false);
                    totalGearscore += gearscore;

                    const uint32 botGuid = bot->GetObjectGuid().GetCounter();
                    PushMetric(botPerformanceMetrics[bracket], botGuid, level);
                    PushMetric(botPerformanceMetrics["gold"], botGuid, gold);
                    PushMetric(botPerformanceMetrics["gearscore"], botGuid, gearscore);
                }
            });
        }

        out << std::fixed << std::setprecision(4);
        out << totalLevel << ",";

        for (uint8 i = 0; i < (DEFAULT_MAX_LEVEL / 10) + 1; i++)
        {
            out << GetMetricDelta(botPerformanceMetrics["level:" + std::to_string(i)]) * 12 * 60 << ",";
        }

        out << totalGold << ",";
        out << GetMetricDelta(botPerformanceMetrics["gold"]) * 12 * 60 << ",";
        out << totalGearscore << ",";
        out << GetMetricDelta(botPerformanceMetrics["gearscore"]) * 12 * 60 << ",";
        //out << CharacterDatabase.m_threadBody->m_sqlQueue.size();

        sPlayerbotAIConfig.log("activity_pid.csv", out.str().c_str());
    }
}

void RandomPlayerbotMgr::LoginFreeBots()
{
    if (!sPlayerbotAIConfig.freeAltBots.empty() && sPlayerbotAIConfig.botAutologin != BotAutoLogin::LOGIN_ONLY_ALWAYS_ACTIVE)
    {
        std::vector<std::pair<uint32, uint32>> botsToRemove;

        for (auto [accountId, botGuid] : sPlayerbotAIConfig.freeAltBots)
        {
            ObjectGuid guid(ObjectGuid(HIGHGUID_PLAYER, botGuid));
            Player* bot = sObjectMgr.GetPlayer(guid, false);

            if (!bot)
            {
                sLog.outDetail("Add player %d", botGuid);
                AddPlayerBot(botGuid, accountId);
            }
            else if (!bot->IsBeingTeleported())
            {
                if (sRandomPlayerbotMgr.GetValue(botGuid, "create levelup"))
                {
                    PlayerbotFactory factory(bot, bot->GetLevel());
                    factory.Randomize(true, false);

                    sRandomPlayerbotMgr.SetValue(botGuid, "create levelup", 0);
                }

                Player* master = nullptr;

                if (sRandomPlayerbotMgr.GetValue(botGuid, "create group"))
                {
                    std::string groupWith = sRandomPlayerbotMgr.GetData(botGuid, "create group");

                    if (!groupWith.empty())
                    {
                        master = sObjectAccessor.FindPlayerByName(groupWith.c_str());

                        if (master)
                        {
                            bot->GetPlayerbotAI()->DoSpecificAction("join", Event("create group", "", master));
                        }
                    }

                    sRandomPlayerbotMgr.SetValue(botGuid, "create group", 0);
                }

                if (sRandomPlayerbotMgr.GetValue(botGuid, "create gear"))
                {
                    std::string gear = sRandomPlayerbotMgr.GetData(botGuid, "create gear");
                    if (gear == "empty")
                    {
                        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
                        {
                            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                        }
                    }

                    else if (gear == "green" || gear == "uncommon")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_UNCOMMON);
                        factory.EquipGear();
                    }
                    else if (gear == "blue" || gear == "rare")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_RARE);
                        factory.EquipGear();
                    }
                    else if (gear == "purple" || gear == "epic")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_EPIC);
                        factory.EquipGear();
                    }
                    else if (gear == "upgrade")
                    {
                        PlayerbotFactory factory(bot, master ? master->GetLevel() : bot->GetLevel(), ITEM_QUALITY_NORMAL);
                        factory.UpgradeGear(false);
                    }
                    else if (gear == "sync")
                    {
                        PlayerbotFactory factory(bot, master ? master->GetLevel() : bot->GetLevel(), ITEM_QUALITY_NORMAL);
                        factory.UpgradeGear(true);
                    }
                    else if (gear == "best")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel());
                        factory.EquipGearBest();
                    }
                    else if (gear == "partial")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel());
                        factory.EquipGearPartialUpgrade();
                    }
                    else
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel());
                        factory.EquipGear();
                    }
                }

                if (GetEventValue(botGuid, "test"))
                {
                    PlayerbotAI* ai = bot->GetPlayerbotAI();
                    AiObjectContext* context = ai->GetAiObjectContext();
                    std::string testName = GetEventData(botGuid, "test");
                    testName = std::regex_replace(testName, std::regex("\\'"), "'");
                    std::string strategyName = "test::" + testName;
                    ai->ChangeStrategy("+" + strategyName, BotState::BOT_STATE_NON_COMBAT);
                    SET_AI_VALUE2(bool, "manual bool", "is running test", true);

                    sRandomPlayerbotMgr.SetValue(botGuid, "test", 0);
                }

                if (!IsRandomBot(bot) && GetPlayerBot(guid)) //Place bot in player manager.
                {
                    for (auto& [mGuid, master] : players)
                    {
                        ObjectGuid masterGuid(ObjectGuid(HIGHGUID_PLAYER, mGuid));
                        if (accountId == sObjectMgr.GetPlayerAccountIdByGUID(masterGuid))
                        {
                            PlayerbotMgr* mgr = master->GetPlayerbotMgr();
                            if (mgr)
                            {
                                MovePlayerBot(guid, mgr);
                            }
                        }
                    }
                }

                if (master)
                    bot->TeleportTo(WorldPosition(master));

                BotAlwaysOnline always = BotAlwaysOnline(sRandomPlayerbotMgr.GetValue(botGuid, "always"));
                if (always != BotAlwaysOnline::ACTIVE)
                {
                    botsToRemove.push_back({accountId, botGuid});
                }
            }
        }

        sPlayerbotAIConfig.freeAltBots.remove_if([&](const std::pair<uint32, uint32>& entry) {
            return std::find(botsToRemove.begin(), botsToRemove.end(), entry) != botsToRemove.end();
        });
    }
}

void RandomPlayerbotMgr::DelayedFacingFix()
{
    if (!sPlayerbotAIConfig.turnInRpg)
        return;

    for (auto& fMap : facingFix) {
        for (auto& fInstance : fMap.second) {
            for (auto obj : fInstance.second) {
                if (time(0) - obj.second > 5)
                {
                    if (!obj.first.IsCreature())
                        continue;

                    GuidPosition guidP(obj.first, WorldPosition(fMap.first, 0, 0, 0));

                    Creature* unit = guidP.GetCreature(fInstance.first);

                    if (!unit)
                        continue;

                    CreatureData const* data = guidP.GetCreatureData();

                    if (!data)
                        continue;

                    if (unit->GetOrientation() == data->position.orientation)
                        continue;

                    unit->SetFacingTo(data->position.orientation);
                }
            }
        }
        facingFix[fMap.first].clear();
    }
}

void RandomPlayerbotMgr::DatabasePing(QueryResult* result, uint32 pingStart, std::string db)
{
    sRandomPlayerbotMgr.SetDatabaseDelay(db, sWorld.GetCurrentMSTime() - pingStart);
    delete result;
}

void RandomPlayerbotMgr::LoadNamedLocations()
{
    namedLocations.clear();

    auto result = WorldDatabase.Query("SELECT `name`, `map_id`, `position_x`, `position_y`, `position_z`, `orientation` FROM `ai_playerbot_named_location` WHERE `name` NOT LIKE 'FISH_LOCATION%'");

    if (!result)
    {
        sLog.outString(">> Loaded 0 named locations - table is empty!");
        sLog.outString();
        return;
    }

    uint32 count = 0;
    do
    {
        ++count;

        Field* fields = result->Fetch();

        std::string name = fields[0].GetCppString();
        uint32 mapId = fields[1].GetUInt32();
        float positionX = fields[2].GetFloat();
        float positionY = fields[3].GetFloat();
        float positionZ = fields[4].GetFloat();
        float orientation = fields[5].GetFloat();

        AddNamedLocation(name, WorldLocation(mapId, positionX, positionY, positionZ, orientation));
    } while (result->NextRow());

    sLog.outString(">> Loaded %u named locations", count);
    sLog.outString();
}

bool RandomPlayerbotMgr::AddNamedLocation(std::string const& name, WorldLocation const& location)
{
    if (namedLocations.find(name) != namedLocations.end())
    {
        sLog.outError("RandomPlayerbotMgr::AddNamedLocation: Failed to add named location '%s' - already exists!", name.c_str());
        return false;
    }

    namedLocations[name] = location;

    return true;
}

bool RandomPlayerbotMgr::GetNamedLocation(std::string const& name, WorldLocation& location)
{
    auto itr = namedLocations.find(name);
    if (itr == namedLocations.end())
    {
        sLog.outError("RandomPlayerbotMgr::GetNamedLocation: Named location '%s' not found! Please ensure that your ai_playerbot_named_location table is up to date.", name.c_str());
        return false;
    }

    location = itr->second;

    return true;
}

uint32 RandomPlayerbotMgr::AddRandomBots()
{
    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");    
    uint32 currentAllowedBotCount = maxAllowedBotCount;

    uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    float currentAvgLevel = 0, wantedAvgLevel = 0, randomAvgLevel = 0;

    if(sPlayerbotAIConfig.asyncBotLogin)
        return 0;
  
    if (currentBots.size() < currentAllowedBotCount)
    {
        if (sPlayerbotAIConfig.syncLevelWithPlayers)
        {
            maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel + sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)));

            wantedAvgLevel = maxLevel / 2;
            uint32 botsAmount = 0;
            ForEachPlayerbot([&](Player* bot)
            {
                currentAvgLevel += bot->GetLevel();
                botsAmount++;
            });
                

            if(currentAvgLevel)
            {
                currentAvgLevel = currentAvgLevel / botsAmount;
            }

            randomAvgLevel = (sPlayerbotAIConfig.randomBotMinLevel + std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel+ sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)))) / 2;
        }

        currentAllowedBotCount -= currentBots.size();

        int32 neededAddBots = currentAllowedBotCount;

        currentAllowedBotCount = currentAllowedBotCount*2;      

        CharacterDatabase.AllowAsyncTransactions();
        CharacterDatabase.BeginTransaction();

        bool enoughBotsForCriteria = true;

        for (uint32 noCriteria = 0; noCriteria < 3; noCriteria++)
        {
            int32  classRaceAllowed[MAX_CLASSES][MAX_RACES] = { 0 };

            for (uint32 race = 1; race < MAX_RACES; ++race)
            {
                for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
                {
                    if (sPlayerbotAIConfig.useFixedClassRaceCounts)
                    {
                        classRaceAllowed[cls][race] = sPlayerbotAIConfig.fixedClassRaceCounts[{cls, race}];
                    }
                    else
                    {
                        if (sPlayerbotAIConfig.classRaceProbability[cls][race])
                            classRaceAllowed[cls][race] = ((sPlayerbotAIConfig.classRaceProbability[cls][race] * maxAllowedBotCount / sPlayerbotAIConfig.classRaceProbabilityTotal) + 1) * (noCriteria + 1);
                    }
                }
            }

            for (std::list<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin(); i != sPlayerbotAIConfig.randomBotAccounts.end(); i++)
            {
                uint32 accountId = *i;

                std::unique_ptr<QueryResult> result;

                if (noCriteria == 2)
                {
                    result.reset(CharacterDatabase.PQuery("SELECT guid, level, totaltime, race, class FROM characters WHERE account = '%u'", accountId));
                }
                else
                {
                    bool needToIncrease = wantedAvgLevel && currentAvgLevel + 1 < wantedAvgLevel;
                    bool needToLower = wantedAvgLevel && currentAvgLevel > wantedAvgLevel + 1;
                    bool rndCanIncrease = !sPlayerbotAIConfig.disableRandomLevels && randomAvgLevel > currentAvgLevel;
                    bool rndCanLower = !sPlayerbotAIConfig.disableRandomLevels && randomAvgLevel < currentAvgLevel;

                    std::string query = "SELECT guid, level, totaltime, race, class FROM characters WHERE account = '%u' AND level <= %u";
                    std::string wasRand = sPlayerbotAIConfig.instantRandomize ? "totaltime" : "(level > 1)";

                    if (needToIncrease) //We need more higher level bots.
                    {
                        query += " AND (level > %u";
                        if (rndCanIncrease) //Log in higher level bots or bots that will be randomized.
                            query += " OR !" + wasRand;
                        query += ")";

                        result.reset(CharacterDatabase.PQuery(query.c_str(), accountId, maxLevel, (uint32)wantedAvgLevel));
                    }
                    else
                    {
                        if (needToLower && !rndCanLower) //Do not load unrandomized if it'll only increase level.
                            query += " AND " + wasRand;

                        result.reset(CharacterDatabase.PQuery(query.c_str(), accountId, maxLevel));
                    }
                }

                if (!result)
                    continue;

                do
                {
                    Field* fields = result->Fetch();
                    uint32 guid = fields[0].GetUInt32();
                    uint32 level = fields[1].GetUInt32();
                    uint32 totaltime = fields[2].GetUInt32();
                    uint32 race = fields[3].GetUInt32();
                    uint32 cls = fields[4].GetUInt32();

                    if (GetEventValue(guid, "add"))
                    {
                        if (!noCriteria)
                            classRaceAllowed[cls][race]--;
                        continue;
                    }

                    if (GetEventValue(guid, "logout"))
                        continue;

                    if (GetPlayerBot(guid))
                    {
                        if (!noCriteria)
                            classRaceAllowed[cls][race]--;
                        continue;
                    }

                    if (std::find(currentBots.begin(), currentBots.end(), guid) != currentBots.end())
                    {
                        if (!noCriteria)
                            classRaceAllowed[cls][race]--;
                        continue;
                    }

                    if (classRaceAllowed[cls][race] <= 0)
                        continue;

                    SetEventValue(guid, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
                    SetEventValue(guid, "logout", 0, 0);
                    currentBots.push_back(guid);

                    if(!noCriteria)
                        classRaceAllowed[cls][race]--;

                    if (wantedAvgLevel)
                    {
                        if (sPlayerbotAIConfig.instantRandomize ? totaltime : level > 1)
                            currentAvgLevel += (float)level / currentBots.size();
                        else
                            currentAvgLevel += (float)level + randomAvgLevel; //Use predicted randomized level. This will be wrong but avarage out correct.
                    }

                    currentAllowedBotCount--;
                    neededAddBots--;

                    if (!currentAllowedBotCount)
                        break;

                } while (result->NextRow());

                if (!currentAllowedBotCount)
                    break;
            }

            if (!currentAllowedBotCount)
                break;

            if (showLoginWarning && neededAddBots > 0)
            {
                sLog.outError("Not enough accounts to meet selection criteria. A random selection of bots was activated to fill the server.");

                if (sPlayerbotAIConfig.syncLevelWithPlayers)
                    sLog.outError("Only bots between level %d and %d are selected to sync with player level", uint32((currentAvgLevel + 1 < wantedAvgLevel) ? wantedAvgLevel : 1), maxLevel);

                ChatHelper chat(nullptr);

                for (uint32 race = 1; race < MAX_RACES; ++race)
                {
                    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
                    {

                            int32 moreWanted = classRaceAllowed[cls][race];
                            if (moreWanted > 0)
                            {
                                if (sPlayerbotAIConfig.useFixedClassRaceCounts)
                                {
                                    int32 totalWanted = sPlayerbotAIConfig.fixedClassRaceCounts[{cls, race}];
                                    sLog.outError("%d %s %ss needed but only %d found.", totalWanted, chat.formatRace(race).c_str(), chat.formatClass(cls).c_str(), totalWanted - moreWanted);
                                }
                                else
                                {
                                    int32 totalWanted = ((sPlayerbotAIConfig.classRaceProbability[cls][race] * maxAllowedBotCount / sPlayerbotAIConfig.classRaceProbabilityTotal) + 1);
                                    float percentage = float(sPlayerbotAIConfig.classRaceProbability[cls][race]) * 100.0f / sPlayerbotAIConfig.classRaceProbabilityTotal;
                                    sLog.outError("%d %s %ss needed to get %3.2f%% of total but only %d found.", totalWanted, chat.formatRace(race).c_str(), chat.formatClass(cls).c_str(), percentage, totalWanted - moreWanted);
                                }
                            }
                        
                    }
                }

                showLoginWarning = false;
            }
        }

        CharacterDatabase.CommitTransaction();

        if (currentAllowedBotCount)
            currentAllowedBotCount = std::max(int64(GetEventValue(0, "bot_count")) - int64(currentBots.size()), int64(0));

        if(currentAllowedBotCount && sPlayerbotAIConfig.randomBotAutoCreate && !sPlayerbotAIConfig.useFixedClassRaceCounts)
#ifdef MANGOSBOT_TWO
            sLog.outError("Not enough random bot accounts available. Need %d more!!", (uint32)ceil(currentAllowedBotCount / 10));
#else
            sLog.outError("Not enough random bot accounts available. Need %d more!!", (uint32)ceil(currentAllowedBotCount / 9));
#endif
      
    }

    return currentBots.size();
}

void RandomPlayerbotMgr::LoadBattleMastersCache()
{
    BattleMastersCache.clear();

    sLog.outString("---------------------------------------");
    sLog.outString("          Loading BattleMasters Cache  ");
    sLog.outString("---------------------------------------");
    sLog.outString();

    auto result = WorldDatabase.Query("SELECT `entry`,`bg_template` FROM `battlemaster_entry`");

    uint32 count = 0;

    if (!result)
    {
        sLog.outString(">> Loaded 0 battlemaster entries - table is empty!");
        sLog.outString();
        return;
    }

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        uint32 bgTypeId = fields[1].GetUInt32();

        CreatureInfo const* bmaster = sObjectMgr.GetCreatureTemplate(entry);
        if (!bmaster)
            continue;

#ifdef MANGOS
        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->FactionAlliance);
#endif
#ifdef CMANGOS
        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->Faction);
#endif
        uint32 bmFactionId = bmFaction->faction;
#ifdef MANGOS
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
#endif
#ifdef CMANGOS
#ifdef MANGOSBOT_ONE
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry<FactionEntry>(bmFactionId);
#else
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
#endif
#endif
        uint32 bmParentTeam = bmParentFaction->team;
        Team bmTeam = TEAM_BOTH_ALLOWED;
        if (bmParentTeam == 891)
            bmTeam = ALLIANCE;
        if (bmFactionId == 189)
            bmTeam = ALLIANCE;
        if (bmParentTeam == 892)
            bmTeam = HORDE;
        if (bmFactionId == 66)
            bmTeam = HORDE;

        BattleMastersCache[bmTeam][BattleGroundTypeId(bgTypeId)].insert(BattleMastersCache[bmTeam][BattleGroundTypeId(bgTypeId)].end(), entry);
        sLog.outDetail("Cached Battmemaster #%d for BG Type %d (%s)", entry, bgTypeId, bmTeam == ALLIANCE ? "Alliance" : bmTeam == HORDE ? "Horde" : "Neutral");

    } while (result->NextRow());

    sLog.outString(">> Loaded %u battlemaster entries", count);
    sLog.outString();
}

void RandomPlayerbotMgr::CheckBgQueue()
{
    if (!BgCheckTimer)
        BgCheckTimer = time(nullptr);

    if (time(nullptr) < (BgCheckTimer + 30))
    {
        return;
    }
    else
    {
        BgCheckTimer = time(nullptr);
    }

    sLog.outDetail("Checking BG Queue...");

    for (int i = BG_BRACKET_ID_FIRST; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (int j = BATTLEGROUND_QUEUE_AV; j < MAX_BATTLEGROUND_QUEUE_TYPES; ++j)
        {
            BgPlayers[j][i][0] = 0;
            BgPlayers[j][i][1] = 0;
            BgBots[j][i][0] = 0;
            BgBots[j][i][1] = 0;
            ArenaBots[j][i][0][0] = 0;
            ArenaBots[j][i][0][1] = 0;
            ArenaBots[j][i][1][0] = 0;
            ArenaBots[j][i][1][1] = 0;
            NeedBots[j][i][0] = false;
            NeedBots[j][i][1] = false;
        }
    }

    for (auto i : players)
    {
        Player* player = i.second;

        if (!player || !player->IsInWorld())
            continue;

        if (!player->InBattleGroundQueue())
            continue;

        if (player->InBattleGround() && player->GetBattleGround()->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        for (int i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattleGroundQueueTypeId queueTypeId = player->GetBattleGroundQueueTypeId(i);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            uint32 TeamId = player->GetTeam() == ALLIANCE ? 0 : 1;

            BattleGroundTypeId bgTypeId = sServerFacade.BgTemplateId(queueTypeId);
#ifndef MANGOSBOT_TWO
            BattleGroundBracketId bracketId = sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, player->GetLevel());
#endif
#ifdef MANGOSBOT_TWO
            BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
            uint32 mapId = bg->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, player->GetLevel());
            if (!pvpDiff)
                continue;

            BattleGroundBracketId bracketId = pvpDiff->GetBracketId();
#endif
#ifdef MANGOSBOT_TWO
            /* to fix
            if (ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId))
            {
                BattleGroundQueue& bgQueue = sServerFacade.bgQueue(queueTypeId);
                GroupQueueInfo ginfo;
                uint32 tempT = TeamId;

                if (bgQueue.GetPlayerGroupInfoData(player->GetObjectGuid(), &ginfo))
                {
                    if (ginfo.isRated)
                    {
                        for (uint32 arena_slot = 0; arena_slot < MAX_ARENA_SLOT; ++arena_slot)
                        {
                            uint32 arena_team_id = player->GetArenaTeamId(arena_slot);
                            ArenaTeam* arenateam = sObjectMgr.GetArenaTeamById(arena_team_id);
                            if (!arenateam)
                                continue;
                            if (arenateam->GetType() != arenaType)
                                continue;

                            Rating[queueTypeId][bracketId][1] = arenateam->GetRating();
                        }
                    }
                    TeamId = ginfo.isRated ? 1 : 0;
                }
                if (player->InArena())
                {
                    if (player->GetBattleGround()->IsRated())
                        TeamId = 1;
                    else
                        TeamId = 0;
                }
                ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;
            }
         */
#endif
#ifdef MANGOSBOT_ONE
            if (ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId))
            {
                sWorld.GetBGQueue().GetMessager().AddMessage([queueTypeId, playerId = player->GetObjectGuid(), arenaType = arenaType, bracketId = bracketId, tempT = TeamId](BattleGroundQueue* bgQueue)
                    {
                        uint32 TeamId;
                        GroupQueueInfo ginfo;

                        BattleGroundQueueItem* queueItem = &bgQueue->GetBattleGroundQueue(queueTypeId);
                        Player *player = RandomPlayerbotMgr::instance().GetPlayer(playerId);

                        if (!player)
                            return;

                        if (queueItem->GetPlayerGroupInfoData(player->GetObjectGuid(), &ginfo))
                        {
                            if (ginfo.isRated)
                            {
                                for (uint32 arena_slot = 0; arena_slot < MAX_ARENA_SLOT; ++arena_slot)
                                {
                                    uint32 arena_team_id = player->GetArenaTeamId(arena_slot);
                                    ArenaTeam* arenateam = sObjectMgr.GetArenaTeamById(arena_team_id);
                                    if (!arenateam)
                                        continue;
                                    if (arenateam->GetType() != arenaType)
                                        continue;

                                    sRandomPlayerbotMgr.Rating[queueTypeId][bracketId][1] = arenateam->GetRating();
                                }
                            }
                            TeamId = ginfo.isRated ? 1 : 0;
                        }
                        if (player->InArena())
                        {
                            if (player->GetBattleGround()->IsRated()/* && (ginfo.isRated && ginfo.arenaTeamId && ginfo.arenaTeamRating && ginfo.opponentsTeamRating)*/)
                                TeamId = 1;
                            else
                                TeamId = 0;
                        }
                        sRandomPlayerbotMgr.ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;

                    }
                );
            }
#endif
            if (player->GetPlayerbotAI())
                BgBots[queueTypeId][bracketId][TeamId]++;
            else
                BgPlayers[queueTypeId][bracketId][TeamId]++;

            if (!player->IsInvitedForBattleGroundQueueType(queueTypeId) && (!player->InBattleGround() || player->GetBattleGround()->GetTypeId() != sServerFacade.BgTemplateId(queueTypeId)))
            {
#ifndef MANGOSBOT_ZERO
                if (ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId))
                {
                    NeedBots[queueTypeId][bracketId][TeamId] = true;
                }
                else
                {
                    NeedBots[queueTypeId][bracketId][0] = true;
                    NeedBots[queueTypeId][bracketId][1] = true;
                }
#else
                NeedBots[queueTypeId][bracketId][0] = true;
                NeedBots[queueTypeId][bracketId][1] = true;
#endif
            }
        }
    }

    ForEachPlayerbot([&](Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;

        if (!bot->InBattleGroundQueue())
            return;

        if (!IsFreeBot(bot))
            return;

        if (bot->InBattleGround() && bot->GetBattleGround() && bot->GetBattleGround()->GetStatus() == STATUS_WAIT_LEAVE)
            return;

        for (int i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattleGroundQueueTypeId queueTypeId = bot->GetBattleGroundQueueTypeId(i);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            uint32 TeamId = bot->GetTeam() == ALLIANCE ? 0 : 1;

            BattleGroundTypeId bgTypeId = sServerFacade.BgTemplateId(queueTypeId);

#ifndef MANGOSBOT_TWO
            BattleGroundBracketId bracketId = sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, bot->GetLevel());;
#endif
#ifdef MANGOSBOT_TWO
            BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
            uint32 mapId = bg->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
            if (!pvpDiff)
                continue;

            BattleGroundBracketId bracketId = pvpDiff->GetBracketId();
#endif
#ifdef MANGOSBOT_TWO
            /* to fix
            ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId);
            if (arenaType != ARENA_TYPE_NONE)
            {
                BattleGroundQueue& bgQueue = sServerFacade.bgQueue(queueTypeId);
                GroupQueueInfo ginfo;
                uint32 tempT = TeamId;
                if (bgQueue.GetPlayerGroupInfoData(bot->GetObjectGuid(), &ginfo))
                {
                    TeamId = ginfo.isRated ? 1 : 0;
                }
                if (bot->InArena())
                {
                    if (bot->GetBattleGround()->IsRated())
                        TeamId = 1;
                    else
                        TeamId = 0;
                }
                ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;
            }
        */
#endif
#ifdef MANGOSBOT_ONE
            ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId);
            if (arenaType != ARENA_TYPE_NONE)
            {
                sWorld.GetBGQueue().GetMessager().AddMessage([queueTypeId, botId = bot->GetObjectGuid(), arenaType = arenaType, bracketId = bracketId, tempT = TeamId](BattleGroundQueue* bgQueue)
                    {
                        uint32 TeamId;
                        GroupQueueInfo ginfo;

                        BattleGroundQueueItem* queueItem = &bgQueue->GetBattleGroundQueue(queueTypeId);
                        Player *bot = RandomPlayerbotMgr::instance().GetPlayer(botId);
                        if (!bot)
                            return;

                        if (queueItem->GetPlayerGroupInfoData(bot->GetObjectGuid(), &ginfo))
                        {
                            TeamId = ginfo.isRated ? 1 : 0;
                        }
                        if (bot->InArena())
                        {
                            if (bot->GetBattleGround()->IsRated()/* && (ginfo.isRated && ginfo.arenaTeamId && ginfo.arenaTeamRating && ginfo.opponentsTeamRating)*/)
                                TeamId = 1;
                            else
                                TeamId = 0;
                        }

                        

                        sRandomPlayerbotMgr.ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;

                    }
                );
            }
#endif
            BgBots[queueTypeId][bracketId][TeamId]++;
        }
    });

    for (int i = BG_BRACKET_ID_FIRST; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (int j = BATTLEGROUND_QUEUE_AV; j < MAX_BATTLEGROUND_QUEUE_TYPES; ++j)
        {
            BattleGroundQueueTypeId queueTypeId = BattleGroundQueueTypeId(j);

            if ((BgPlayers[j][i][0] + BgBots[j][i][0] + BgPlayers[j][i][1] + BgBots[j][i][1]) == 0)
                continue;

#ifndef MANGOSBOT_ZERO
            if (ArenaType type = sServerFacade.BgArenaType(queueTypeId))
            {
                sLog.outDetail("ARENA:%s %s: Plr (Skirmish:%d, Rated:%d) Bot (Skirmish:%d, Rated:%d) Total (Skirmish:%d Rated:%d)",
                    type == ARENA_TYPE_2v2 ? "2v2" : type == ARENA_TYPE_3v3 ? "3v3" : "5v5",
                    i == 0 ? "10-19" : i == 1 ? "20-29" : i == 2 ? "30-39" : i == 3 ? "40-49" : i == 4 ? "50-59" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 6) ? "60" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 7) ? "60-69" : i == 6 ? (i == 6 && MAX_BATTLEGROUND_BRACKETS == 16) ? "70-79" : "70" : "80",
                    BgPlayers[j][i][0],
                    BgPlayers[j][i][1],
                    BgBots[j][i][0],
                    BgBots[j][i][1],
                    BgPlayers[j][i][0] + BgBots[j][i][0],
                    BgPlayers[j][i][1] + BgBots[j][i][1]
                );
                continue;
            }
#endif
            BattleGroundTypeId bgTypeId = sServerFacade.BgTemplateId(queueTypeId);
            std::string _bgType;
            switch (bgTypeId)
            {
            case BATTLEGROUND_AV:
                _bgType = "AV";
                break;
            case BATTLEGROUND_WS:
                _bgType = "WSG";
                break;
            case BATTLEGROUND_AB:
                _bgType = "AB";
                break;
#ifndef MANGOSBOT_ZERO
            case BATTLEGROUND_EY:
                _bgType = "EotS";
                break;
#endif
#ifdef MANGOSBOT_TWO
            case BATTLEGROUND_RB:
                _bgType = "Random";
                break;
            case BATTLEGROUND_SA:
                _bgType = "SotA";
                break;
            case BATTLEGROUND_IC:
                _bgType = "IoC";
                break;
#endif
            default:
                _bgType = "Other";
                break;
            }
            sLog.outDetail("BG:%s %s: Plr (%d:%d) Bot (%d:%d) Total (A:%d H:%d)",
                _bgType.c_str(),
                i == 0 ? "10-19" : i == 1 ? "20-29" : i == 2 ? "30-39" : i == 3 ? "40-49" : i == 4 ? "50-59" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 6) ? "60" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 7) ? "60-69" : i == 6 ? (i == 6 && MAX_BATTLEGROUND_BRACKETS == 16) ? "70-79" : "70" : "80",
                BgPlayers[j][i][0],
                BgPlayers[j][i][1],
                BgBots[j][i][0],
                BgBots[j][i][1],
                BgPlayers[j][i][0] + BgBots[j][i][0],
                BgPlayers[j][i][1] + BgBots[j][i][1]
            );
        }
    }

    sLog.outDetail("BG Queue check finished");
    return;
}

void RandomPlayerbotMgr::CheckLfgQueue()
{
    if (!LfgCheckTimer || time(NULL) > (LfgCheckTimer + 30))
        LfgCheckTimer = time(NULL);

    if (sPlayerbotAIConfig.logRandomBotJoinLfg)
    {
        sLog.outDetail("Checking LFG Queue...");
    }

    // Clear LFG list
    LfgDungeons[HORDE].clear();
    LfgDungeons[ALLIANCE].clear();

    for (auto i : players)
    {
        Player* player = i.second;

        if (!player || !player->IsInWorld())
            continue;

        bool isLFG = false;

#ifdef MANGOSBOT_ZERO
        WorldSafeLocsEntry const* ClosestGrave = player->GetMap()->GetGraveyardManager().GetClosestGraveYard(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->GetTeam());
        uint32 zoneId = ClosestGrave ? ClosestGrave->ID : 0;

        Group* group = player->GetGroup();
        if (group)
        {
            if (sWorld.GetLFGQueue().IsGroupInQueue(group->GetId()))
            {
                isLFG = true;
                LFGGroupQueueInfo lfgInfo;
                sWorld.GetLFGQueue().GetGroupQueueInfo(&lfgInfo, group->GetId());
                uint32 lfgType = (zoneId << 16) | lfgInfo.areaId;
                LfgDungeons[player->GetTeam()].push_back(lfgType);
            }
        }
        else
        {
            if (sWorld.GetLFGQueue().IsPlayerInQueue(player->GetObjectGuid()))
            {
                isLFG = true;
                LFGPlayerQueueInfo lfgInfo;
                sWorld.GetLFGQueue().GetPlayerQueueInfo(&lfgInfo, player->GetObjectGuid());
                uint32 lfgType = (zoneId << 16) | lfgInfo.areaId;
                LfgDungeons[player->GetTeam()].push_back(lfgType);
            }
        }
#endif

#ifdef MANGOSBOT_ONE
        /* todo: Fix with new system
        WorldSafeLocsEntry const* ClosestGrave = player->GetMap()->GetGraveyardManager().GetClosestGraveYard(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->GetTeam());
        uint32 zoneId = ClosestGrave ? ClosestGrave->ID : 0;

        Group* group = player->GetGroup();
        if (group && !group->IsFull())
        {
            if (group->IsLeader(player->GetObjectGuid()))
            {
                if (player->GetSession()->m_lfgInfo.queued && player->GetSession()->LookingForGroup_auto_add && player->m_lookingForGroup.more.isAuto())
                {
                    uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(player->m_lookingForGroup.more.entry));
                    LfgDungeons[player->GetTeam()].push_back(lfgType);
                    isLFG = true;
                }
            }
        }
        else if (!group)
        {
            for (int i = 0; i < MAX_LOOKING_FOR_GROUP_SLOT; ++i)
                if (!player->m_lookingForGroup.group[i].empty() && player->GetSession()->LookingForGroup_auto_join && player->m_lookingForGroup.group[i].isAuto())
                {
                    isLFG = true;
                    uint32 lfgType = (zoneId << 16) | ((0 << 8) | uint8(player->m_lookingForGroup.group[i].entry));
                    LfgDungeons[player->GetTeam()].push_back(lfgType);
                }

            if (!player->m_lookingForGroup.more.empty() && player->GetSession()->LookingForGroup_auto_add && player->m_lookingForGroup.more.isAuto())
            {
                uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(player->m_lookingForGroup.more.entry));
                LfgDungeons[player->GetTeam()].push_back(lfgType);
                isLFG = true;
            }
        }
        */
#endif

#ifdef MANGOSBOT_TWO
        Group* group = player->GetGroup();
        if (group)
        {
            if (group->IsLFGGroup())
            {
                isLFG = true;
                LFGQueueData& lfgData = sWorld.GetLFGQueue().GetQueueData(group->GetObjectGuid());
                if (lfgData.GetState() != LFG_STATE_NONE && lfgData.GetState() < LFG_STATE_DUNGEON)
                {
                    LfgDungeonSet dList = lfgData.GetDungeons();
                    for (auto dungeon : dList)
                    {
                        LfgDungeons[player->GetTeam()].push_back(dungeon);
                    }
                }
            }
        }
        else
        {
            if (player->GetLfgData().GetState() != LFG_STATE_NONE)
            {
                LFGQueueData& lfgData = sWorld.GetLFGQueue().GetQueueData(player->GetObjectGuid());
                isLFG = true;
                if (lfgData.GetState() < LFG_STATE_DUNGEON)
                {
                    LfgDungeonSet dList = lfgData.GetDungeons();
                    for (auto dungeon : dList)
                    {
                        LfgDungeons[player->GetTeam()].push_back(dungeon);
                    }
                }
            }
        }
#endif
    }

#ifdef MANGOSBOT_ONE
    /* todo: Fix with new system
    ForEachPlayerbot([&](Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;

        if (LfgDungeons[bot->GetTeam()].empty())
            return;

        WorldSafeLocsEntry const* ClosestGrave = bot->GetMap()->GetGraveyardManager().GetClosestGraveYard(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(), bot->GetTeam());
        uint32 zoneId = ClosestGrave ? ClosestGrave->ID : 0;

        Group* group = bot->GetGroup();
        if (group && !group->IsFull())
        {
            if (group->IsLeader(bot->GetObjectGuid()))
            {
                if (bot->GetSession()->m_lfgInfo.queued && bot->GetSession()->m_lfgInfo.autofill)
                {
                    uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(bot->m_lookingForGroup.more.entry));
                    LfgDungeons[bot->GetTeam()].push_back(lfgType);
                }
            }
        }
        else if (!group)
        {
            if (!bot->m_lookingForGroup.more.empty() && bot->GetSession()->LookingForGroup_auto_add && bot->m_lookingForGroup.more.isAuto())
            {
                uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(bot->m_lookingForGroup.more.entry));
                LfgDungeons[bot->GetTeam()].push_back(lfgType);
            }
        }
    });
    */
#endif

    if (sPlayerbotAIConfig.logRandomBotJoinLfg)
    {
       if (LfgDungeons[ALLIANCE].size() || LfgDungeons[HORDE].size())
            sLog.outDetail("LFG Queue check finished. There are real players in queue.");
       else
           sLog.outDetail("LFG Queue check finished. No real players in queue.");
    }
    return;
}

void RandomPlayerbotMgr::AddOfflineGroupBots()
{
    time_t now = time(nullptr);
    if (OfflineGroupBotsTimer && now <= (OfflineGroupBotsTimer + 5))
        return;

    OfflineGroupBotsTimer = now;

    uint32 totalCounter = 0;
    // Iterate a snapshot and re-look-up each player LIVE by guid. The `players` map can retain a
    // STALE Player* (freed without being erased) -> dereferencing the stored pointer here
    // (IsInWorld/GetGroup/GetObjectGuid -> Object::GetUInt64Value on freed memory) crashed with
    // SIGSEGV and SIGABRT. GetPlayer() returns null for an offline/gone guid, so we never touch
    // freed memory. The snapshot also keeps iteration valid while the loop body calls
    // AddRandomBot/AddPlayerBot/MovePlayerBot, which can mutate `players`.
    for (const auto& entry : GetPlayersCopy())
    {
        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, entry.first));

        if (!player || !player->IsInWorld() || !player->GetGroup())
            continue;

        Group* group = player->GetGroup();
        if (group && group->IsLeader(player->GetObjectGuid()))
        {
            std::vector<uint32> botsToAdd;
            Group::MemberSlotList const& slots = group->GetMemberSlots();
            for (Group::MemberSlotList::const_iterator i = slots.begin(); i != slots.end(); ++i)
            {
                ObjectGuid member = i->guid;
                if (member == player->GetObjectGuid())
                    continue;

                if (!IsFreeBot(member.GetCounter()))
                    continue;

                if (sObjectMgr.GetPlayer(member))
                    continue;

                if (GetPlayerBot(member))
                    continue;

                botsToAdd.push_back(member.GetCounter());
            }

            if (botsToAdd.empty())
                continue;

            uint32 maxToAdd = urand(1, 5);
            uint32 counter = 0;
            for (auto& guid : botsToAdd)
            {
                if (counter >= maxToAdd)
                    break;

                if (sPlayerbotAIConfig.IsFreeAltBot(guid))
                {
                    for (auto& bot : sPlayerbotAIConfig.freeAltBots)
                    {
                        if (bot.second == guid)
                        {
                            Player* player = GetPlayerBot(bot.second);
                            if (!player)
                            {
                                AddPlayerBot(bot.second, bot.first);
                            }
                        }
                    }
                }
                else
                    AddRandomBot(guid);

                counter++;
                totalCounter++;
            }
        }
    }

    if (totalCounter)
        sLog.outDetail("Added %u offline bots from groups", totalCounter);
}

Item* RandomPlayerbotMgr::CreateTempItem(uint32 item, uint32 count, Player const* player, uint32 randomPropertyId)
{
    if (count < 1)
        return nullptr;                                        // don't create item at zero count

    if (ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(item))
    {
        if (count > pProto->GetMaxStackSize())
            count = pProto->GetMaxStackSize();

        MANGOS_ASSERT(count != 0 && "pProto->Stackable == 0 but checked at loading already");

        Item* pItem = NewItemOrBag(pProto);
        if (pItem->Create(0, item, player ? player->GetObjectGuid() : ObjectGuid()))
        {
            pItem->SetCount(count);
            if (int32 randId = randomPropertyId ? randomPropertyId : Item::GenerateItemRandomPropertyId(item))
                pItem->SetItemRandomProperties(randId);

            return pItem;
        }
        delete pItem;
    }
    return nullptr;
}

InventoryResult RandomPlayerbotMgr::CanEquipUnseenItem(Player* player, uint8 slot, uint16& dest, uint32 item)
{
    dest = 0;
    Item* pItem = RandomPlayerbotMgr::CreateTempItem(item, 1, player);

    if (pItem)
    {
        InventoryResult result = player->CanEquipItem(slot, dest, pItem, true, false);

        pItem->RemoveFromUpdateQueueOf(player);

        if (!player->GetItemUpdateQueue().empty() && !player->GetItemUpdateQueue().back()) //Prevent queue overflow.
            player->GetItemUpdateQueue().pop_back();

        delete pItem;
        return result;
    }

    return EQUIP_ERR_ITEM_NOT_FOUND;
}

void RandomPlayerbotMgr::SaveCurTime()
{
    if (!EventTimeSyncTimer || time(NULL) > (EventTimeSyncTimer + 60))
        EventTimeSyncTimer = time(NULL);

    SetValue(uint32(0), "current_time", uint32(time(nullptr)));
}

void RandomPlayerbotMgr::SyncEventTimers()
{
    uint32 oldTime = GetValue(uint32(0), "current_time");
    if (oldTime)
    {
        uint32 curTime = time(nullptr);
        uint32 timeDiff = curTime - oldTime;
        CharacterDatabase.PExecute("UPDATE ai_playerbot_random_bots SET time = time + %u WHERE owner = 0 AND bot <> 0", timeDiff);
    }
}

void RandomPlayerbotMgr::CheckPlayers()
{
    if (!PlayersCheckTimer || time(NULL) > (PlayersCheckTimer + 60))
        PlayersCheckTimer = time(NULL);

    sLog.outDetail("Checking Players...");

    uint32 newPlayersLevel = 0;

    for (auto i : players)
    {
        Player* player = i.second;

        if (player->IsGameMaster())
            continue;

        //if (player->GetSession()->GetSecurity() > SEC_PLAYER)
        //    continue;

        if (player->GetLevel() > newPlayersLevel)
            newPlayersLevel = player->GetLevel();
    }

    if(playersLevel!= newPlayersLevel)
        sLog.outDetail("Max player level is %d, max bot level changed from %d to %d", newPlayersLevel, playersLevel, newPlayersLevel);
    else
        sLog.outDetail("Max player level is %d, max bot level set to %d", newPlayersLevel, newPlayersLevel);

    playersLevel = newPlayersLevel;

    return;
}

void RandomPlayerbotMgr::ScheduleRandomize(uint32 bot, uint32 time)
{
    SetEventValue(bot, "randomize", 1, time);
}

void RandomPlayerbotMgr::ScheduleTeleport(uint32 bot, uint32 time)
{
    if (!time)
        time = 60 + urand(sPlayerbotAIConfig.randomBotTeleportMinInterval, sPlayerbotAIConfig.randomBotTeleportMaxInterval);
    SetEventValue(bot, "teleport", 1, time);
}

void RandomPlayerbotMgr::ScheduleChangeStrategy(uint32 bot, uint32 time)
{
    if (!time)
        time = urand(sPlayerbotAIConfig.minRandomBotChangeStrategyTime, sPlayerbotAIConfig.maxRandomBotChangeStrategyTime);
    SetEventValue(bot, "change_strategy", 1, time);
}

bool RandomPlayerbotMgr::AddRandomBot(uint32 bot)
{
    SC_LOG("AddRandomBot entry guid=%u", bot);
    Player* player = GetPlayerBot(bot);
    if (player)
    {
        SC_LOG("AddRandomBot guid=%u already online — returning true", bot);
        return true;
    }

    uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER, bot));
    SC_LOG("AddRandomBot guid=%u accountId=%u — checking IsInRandomAccountList", bot, accountId);

    if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
    {
        SC_LOG("AddRandomBot guid=%u — NOT in random account list, FAIL", bot);
        sLog.outError("Bot #%d login fail: Not random bot!", bot);
        return false;
    }

    uint32 loginEv = GetEventValue(bot, "login");
    SC_LOG("AddRandomBot guid=%u — IsInRandomAccountList OK, login event=%u", bot, loginEv);

    // stale-event recovery: if login=1 is set but
    // we already proved (line ~2124, GetPlayerBot returned null) that no
    // actual session exists for this bot, the marker is stale — typically
    // because the previous mangosd crashed mid-tick before the bot's
    // logout cleanup could clear the event. Without this fix, the bot is
    // permanently un-summonable until someone manually `DELETE FROM
    // ai_playerbot_random_bots` and bounces mangosd to clear the in-memory
    // eventCache. We force-reset the event in both DB and cache, then
    // proceed with a fresh login.
    if (loginEv)
    {
        SC_LOG("AddRandomBot guid=%u — login=1 but no live session; treating as stale, force-resetting event", bot);
        SetEventValue(bot, "login", 0, 0);  // updates DB row + eventCache in one call
        loginEv = 0;
    }

    if (!loginEv)
    {
        SC_LOG("AddRandomBot guid=%u — calling AddPlayerBot", bot);
        AddPlayerBot(bot, 0);
        SC_LOG("AddRandomBot guid=%u — AddPlayerBot returned, setting event values", bot);
        SetEventValue(bot, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        SetEventValue(bot, "logout", 0, 0);
        SetEventValue(bot, "login", 1, -1);
        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);
        currentBots.push_back(bot);
        SC_LOG("AddRandomBot guid=%u — DONE, added to currentBots", bot);
        sLog.outDetail("Random bot added #%d", bot);
    }
    else
    {
        SC_LOG("AddRandomBot guid=%u — login event already set, SKIPPING actual login", bot);
    }

    return true;
}

void RandomPlayerbotMgr::MovePlayerBot(uint32 guid, PlayerbotHolder* newHolder)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    {
        std::lock_guard<std::mutex> lock(playersMutex);
        players[guid] = this->GetPlayerBot(guid);
    }
    PlayerbotHolder::MovePlayerBot(guid, newHolder);
}

bool RandomPlayerbotMgr::ProcessBot(uint32 bot)
{
    Player* player = GetPlayerBot(bot);
    if (player && sPlayerbotAIConfig.IsFreeAltBot(player))
    {
        return false;
    }

    PlayerbotAI* ai = player ? player->GetPlayerbotAI() : NULL;

    bool botsAllowedInWorld = !sPlayerbotAIConfig.randomBotLoginWithPlayer || (!players.empty() && sWorld.GetActiveSessionCount() > 0);

    bool isValid = true;
   
    if (sPlayerbotAIConfig.randomBotTimedLogout && !GetEventValue(bot, "add") && !sPlayerbotAIConfig.asyncBotLogin) // RandomBotInWorldTime is expired.
        isValid = false;
    else if(!botsAllowedInWorld)                                               // Logout if all players logged out
        isValid = false;

    //Log out bot
    if (!isValid)
    {
        if (botsAllowedInWorld && player && player->GetGroup())
        {
            SetEventValue(bot, "add", 1, 120);                                 // Delay logout for 2 minutes while in group.
            return false;
        }

        if (!player || !player->IsInWorld())
            sLog.outDetail("Bot #%d: log out", bot);
        else
            sLog.outDetail("Bot #%d %s:%d <%s>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H", player->GetLevel(), player->GetName());

        currentBots.remove(bot);
        SetEventValue(bot, "add", 0, 0);

        if (!player)
        {
            return false;
        }    

        LogoutPlayerBot(bot);

        if (sPlayerbotAIConfig.randomBotTimedOffline)
        {
            uint32 logout = GetEventValue(bot, "logout");

            if (!logout)
                SetEventValue(bot, "logout", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        }

        return false;
    }

    //Log in bot (Added in AddRandomBots)
    if (!player)
    {
        if (!botsAllowedInWorld)
            return false;

        if (GetEventValue(bot, "login"))
            return true;

        AddPlayerBot(bot, 0);

        SetEventValue(bot, "login", 1, -1); // This will be reset to 0 on server startup. Check RandomPlayerbotMgr constructor

        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);

        return true;
    }

    if (!player->IsInWorld() || player->IsBeingTeleported() || player->GetSession()->isLogingOut()) //Skip bots that are in limbo.
        return false;

    if(GetEventValue(bot, "login"))
        SetEventValue(bot, "login", 0, 0); //Bot is no longer loggin in.

    // SPELL-KIT CATCH-UP: a large slice of the fleet was online with ZERO (or a handful of)
    // class spells -- they only white-attacked, never used abilities. Root cause: the class
    // kit is only learned inside the heavy factory Randomize (first login / long-interval
    // schedule), so any bot whose Randomize never ran (or ran before it had a kit) stayed
    // spell-less for hours. LearnClassTrainerSpells is idempotent (only learns GREEN ranks)
    // and now cache-backed (cheap), so run it ONCE per bot per server session here. This block
    // sits on the hot ProcessBot(uint32) sweep path (every online bot, ~1024/interval, with NO
    // player-proximity gate -- unlike ProcessBot(Player*)), so it heals the whole fleet within
    // a couple of intervals without waiting for a re-login or a scheduled randomize, and tops
    // up under-ranked kits too.
    if (ai && sRandomPlayerbotMgr.IsRandomBot(player))
    {
        static std::set<uint32> s_spellKitFixed;
        static std::mutex s_spellKitFixedMutex;
        bool needsFix = false;
        {
            std::lock_guard<std::mutex> guard(s_spellKitFixedMutex);
            needsFix = s_spellKitFixed.insert(bot).second;
        }
        if (needsFix)
        {
            PlayerbotFactory factory(player, player->GetLevel());
            factory.LearnClassTrainerSpells();
        }
    }

    uint32 update = GetEventValue(bot, "update");
    //Update the bot
    if (!update)
    {
        //Clean up expired values
        if (ai && !ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
            ai->GetAiObjectContext()->ClearExpiredValues();

        //Randomize/teleport bot
        if (!sPlayerbotAIConfig.disableRandomLevels)
        {
            if (player->GetGroup() || player->IsTaxiFlying())
                return false;

            bool update = true;
            if (ai)
            {
                if (!sRandomPlayerbotMgr.IsRandomBot(player))
                    update = false;

                if (player->GetGroup() && ai->GetGroupMaster() && (!ai->GetGroupMaster()->GetPlayerbotAI() || ai->GetGroupMaster()->GetPlayerbotAI()->IsRealPlayer()))
                    update = false;

                if (ai->HasPlayerNearby())
                    update = false;
            }
            if (update)
                ProcessBot(player);
        }

        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime * 5);
        SetEventValue(bot, "update", 1, randomTime);
        return true;
    }

    return false;
}

bool RandomPlayerbotMgr::ProcessBot(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->GetSession()->isLogingOut())
        return false;

    uint32 bot = player->GetGUIDLow();

    if (player->InBattleGround())
        return false;

    if (player->InBattleGroundQueue())
        return false;

    // only teleport idle bots
    bool idleBot = false;
    TravelTarget* target = player->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (target)
    {
        if (target->GetTravelState() == TravelState::TRAVEL_STATE_IDLE)
            idleBot = true;
    }
    else
        idleBot = true;

    if (idleBot)
    {
        uint32 randomize = GetEventValue(bot, "randomize");
        if (!randomize)
        {
            bool randomiser = true;
            if (player->GetGuildId())
            {
                Guild* guild = sGuildMgr.GetGuildById(player->GetGuildId());
                uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guild->GetLeaderGuid());
                if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
                {
                    int32 rank = guild->GetRank(player->GetObjectGuid());
                    randomiser = rank < 4 ? false : true;
                }
            }

            if (randomiser)
            {
                Randomize(player);
                return true;
            }
        }

        uint32 changeStrategy = GetEventValue(bot, "change_strategy");
        if (!changeStrategy)
        {
            if (sPlayerbotAIConfig.enableRandomTeleports)
            {
                sLog.outDetail("Changing strategy for bot #%d %s:%d <%s>", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
                ChangeStrategy(player);
                ScheduleChangeStrategy(bot);
            }
            else
            {
                sLog.outDetail("Changing strategy for bot #%d %s:%d <%s> is supposed to happen, but enableRandomTeleports = false", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
            }
            return true;
        }

        uint32 teleport = GetEventValue(bot, "teleport");
        if (!teleport && players.size())
        {
            if (sPlayerbotAIConfig.enableRandomTeleports)
            {
                sLog.outDetail("Bot #%d %s:%d <%s>: sent to grind", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
                RandomTeleportForLevel(player, true);
                ScheduleTeleport(bot);
            }
            else
            {
                sLog.outDetail("Bot #%d %s:%d <%s>: supposed to be sent to grind, but enableRandomTeleports = false", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
            }
            return true;
        }
    }

    return false;
}

void RandomPlayerbotMgr::Revive(Player* player)
{
    uint32 bot = player->GetGUIDLow();

    //sLog.outString("Bot %d revived", bot);
    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    if (sServerFacade.GetDeathState(player) == CORPSE)
    {
        RandomTeleport(player);
    }
    else
    {
        RandomTeleportForLevel(player, false);
    }
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot, std::vector<WorldLocation> &locs, bool hearth, bool activeOnly)
{
    if (bot->IsBeingTeleported())
        return;

    if (bot->InBattleGround())
        return;

    if (bot->InBattleGroundQueue())
        return;

	if (bot->GetLevel() < 5)
		return;

    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetObjectGuid()))
        return;

    if (bot->IsTaxiFlying() && bot->GetPlayerbotAI()->HasPlayerNearby())
        return;

    if (locs.empty())
    {
        sLog.outError("Cannot teleport bot %s - no locations available", bot->GetName());
        return;
    }

    std::vector<WorldPosition> tlocs;

    for (auto& loc : locs)
    {
        tlocs.push_back(WorldPosition(loc));
    }

    //Do not teleport to maps disabled in config
    tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [](const WorldPosition& l) {std::vector<uint32>::iterator i = find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), l.getMapId()); return i == sPlayerbotAIConfig.randomBotMaps.end(); }), tlocs.end());

    //Random shuffle based on distance. Closer distances are more likely (but not exclusively) to be at the begin of the list.
    tlocs = WorldPosition(bot).GetNextPoint(tlocs, 0);

    //5% + 0.1% per level chance node on different map in selection.
    //tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [bot](WorldLocation const& l) {return l.position.mapid != bot->GetMapId() && urand(1, 100) > 0.5 * bot->GetLevel(); }), tlocs.end());

    //Continent is about 20.000 large
    //Bot will travel 0-5000 units + 75-150 units per level.
    //tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [bot](WorldLocation const& l) {return l.position.mapid == bot->GetMapId() && sServerFacade.GetDistance2d(bot, l.coord_x, l.coord_y) > urand(0, 5000) + bot->GetLevel() * 15 * urand(5, 10); }), tlocs.end());

    // teleport to active areas only
    if (sPlayerbotAIConfig.randomBotTeleportNearPlayer && activeOnly)
    {
        tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [this](const WorldPosition& l)
        {
            uint32 mapId = l.getMapId();
            Map* tMap = sMapMgr.FindMap(mapId, 0);
            if (tMap && tMap->IsContinent() && tMap->HasActiveZones())
            {
                uint32 zoneId = sTerrainMgr.GetZoneId(mapId, l.coord_x, l.coord_y, l.coord_z);
                if (tMap->HasActiveZone(zoneId))
                {
                    if (sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmount > 0 && sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmountRadius > 0.0f)
                    {
                        uint32 botsNearTeleportPoint = 0;
                        ForEachPlayerbot([&](Player* otherBot)
                        {
                            // Only check the bots that are on the same zone
                            if (otherBot && !otherBot->IsBeingTeleported() && zoneId == otherBot->GetZoneId())
                            {
                                if (l.fDist(WorldPosition(otherBot)) <= sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmountRadius)
                                {
                                    botsNearTeleportPoint++;
                                }
                            }
                        });

                        return botsNearTeleportPoint >= sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmount;
                    }
                    else
                    {
                        return false;
                    }
                }
            }

            return true;
        }),
        tlocs.end());

        /*if (!tlocs.empty())
        {
            tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [bot](const WorldPosition& l)
            {
                uint32 mapId = l.getMapId();
                Map* tMap = sMapMgr.FindMap(mapId, 0);
                if (!tMap || !tMap->IsContinent())
                        return true;

                if (!tMap->HasActiveAreas())
                    return true;

                AreaTableEntry const* area = l.getArea();
                if (area)
                {
                    if (!tMap->HasActiveZone(area->zone ? area->zone : area->ID))
                        return true;
                }
            }), tlocs.end());
        }*/
    }

    // filter starter zones
    tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [bot](const WorldPosition& l)
    {
        uint32 mapId = l.getMapId();
        uint32 zoneId, areaId;
        sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, mapId, l.coord_x, l.coord_y, l.coord_z);
        AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
        if (bot->GetLevel() < 30)
        {
            // Thalassian Highlands (Turtle-WoW high-elf starter): high elves start here,
            // but any ALLIANCE race may travel/quest here. Only keep out the Horde.
            if ((zoneId == 5225 || areaId == 5225 || zoneId == 2040 || areaId == 2040 || zoneId == 1220 || areaId == 1220) &&
                bot->GetTeam() != ALLIANCE)
                return true;
            // Blackstone Island (Turtle-WoW goblin starter, HORDE): goblins start here,
            // other Horde may quest here, but Alliance must never be routed into it.
            if ((zoneId == 5536 || areaId == 5536) && bot->GetTeam() != HORDE)
                return true;
        }

        if (zoneId && zoneId != areaId)
        {
            AreaTableEntry const* zone = GetAreaEntryByAreaID(zoneId);
            if (!zone)
                return true;

            bool isEnemyZone = false;
            switch (zone->team)
            {
            case AREATEAM_ALLY:
                isEnemyZone = bot->GetTeam() != ALLIANCE;
                break;
            case AREATEAM_HORDE:
                isEnemyZone = bot->GetTeam() != HORDE;
                break;
            default:
                isEnemyZone = false;
                break;
            }
            if (isEnemyZone && (bot->GetLevel() < 21 || (zone->flags & AREA_FLAG_CAPITAL)))
                return true;

            // filter other races zones
            if (bot->GetLevel() < 30)
            {
                if ((zoneId == 12 || zoneId == 40) && bot->getRace() != RACE_HUMAN)
                    return true;
                if ((zoneId == 1 || zoneId == 38) && !(bot->getRace() == RACE_DWARF || bot->getRace() == RACE_GNOME))
                    return true;
                if ((zoneId == 85 || zoneId == 130) && bot->getRace() != RACE_UNDEAD)
                    return true;
                if ((zoneId == 141 || zoneId == 148) && bot->getRace() != RACE_NIGHTELF)
                    return true;
                if ((zoneId == 14 || zoneId == 17) && !(bot->getRace() == RACE_ORC || bot->getRace() == RACE_TROLL))
                    return true;
                if ((zoneId == 215) && bot->getRace() != RACE_TAUREN)
                    return true;
                // redridge / duskwood
                if ((zoneId == 44 || zoneId == 10) && bot->GetTeam() != ALLIANCE)
                    return true;
#ifndef MANGOSBOT_ZERO
                if ((zoneId == 3524 || zoneId == 3525) && bot->getRace() != RACE_DRAENEI)
                    return true;
                if ((zoneId == 3430 || zoneId == 3433) && bot->getRace() != RACE_BLOODELF)
                    return true;
#endif
            }
        }

        if (!area)
            return true;

        bool isEnemyZone = false;
        switch (area->team)
        {
        case AREATEAM_ALLY:
            isEnemyZone = bot->GetTeam() != ALLIANCE;
            break;
        case AREATEAM_HORDE:
            isEnemyZone = bot->GetTeam() != HORDE;
            break;
        default:
            isEnemyZone = false;
            break;
        }
        return isEnemyZone && bot->GetLevel() < 21;

    }), tlocs.end());

    if (tlocs.empty())
    {
        if (activeOnly)
        {
            if (hearth)
                return RandomTeleportForRpg(bot, false);
            else
                return RandomTeleportForLevel(bot, false);
        }

        sLog.outError("Cannot teleport bot %s - no locations available", bot->GetName());

        return;
    }

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "RandomTeleportByLocations");

    int index = 0;

    for (int i = 0; i < tlocs.size(); i++)
    {
        for (int attemtps = 0; attemtps < 3; ++attemtps)
        {
            WorldLocation loc = tlocs[i];

#ifdef MANGOSBOT_ONE
            // Teleport to Dark Portal area if event is in progress
            if (sWorldState.GetExpansion() == EXPANSION_NONE && bot->GetLevel() > 54 && urand(0, 100) > 20)
            {
                if (urand(0, 1))
                    loc = WorldLocation(uint32(0), -11772.43f, -3272.84f, -17.9f, 3.32447f);
                else
                    loc = WorldLocation(uint32(0), -11741.70f, -3130.3f, -11.7936f, 3.32447f);
            }
#endif

            float x = loc.coord_x + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) - sPlayerbotAIConfig.grindDistance / 2 : 0);
            float y = loc.coord_y + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) - sPlayerbotAIConfig.grindDistance / 2 : 0);
            float z = loc.coord_z;

            Map* map = sMapMgr.FindMap(loc.mapid, 0);
            if (!map)
                continue;

            uint32 areaId = sTerrainMgr.GetAreaId(loc.mapid, x, y, z);
            AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
            if (!area)
                continue;

#ifndef MANGOSBOT_ZERO
            // Do not teleport to outland before portal opening (allow new races zones)
            if (sWorldState.GetExpansion() == EXPANSION_NONE && (loc.mapid == 571 || (loc.mapid == 530 && area->team != 2 && area->team != 4)))
                continue;
#endif

#ifdef MANGOSBOT_TWO
            float ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
#else
            float ground = map->GetHeight(x, y, z + 0.5f);
#endif
            if (ground <= INVALID_HEIGHT)
                continue;

            z = 0.05f + ground;
            sLog.outDetail("Random teleporting bot %s to %s %f,%f,%f (%u/%zu locations)",
                bot->GetName(), area->area_name[0], x, y, z, attemtps, tlocs.size());

            if (bot->IsTaxiFlying())
                bot->GetMotionMaster()->MovementExpired();

            if (hearth)
                bot->SetHomebindToLocation(loc, area->ID);

            bot->GetMotionMaster()->Clear();
            bot->TeleportTo(loc.mapid, x, y, z, 0);
            bot->SendHeartBeat();
            bot->GetPlayerbotAI()->Reset(true);

            if (bot->GetGroup())
            {
                for (GroupReference* gref = bot->GetGroup()->GetFirstMember(); gref; gref = gref->next())
                {
                    Player* member = gref->getSource();
                    PlayerbotAI* ai = bot->GetPlayerbotAI();
                    if (ai && bot != member)
                    {
                        if (member->IsTaxiFlying())
                            member->GetMotionMaster()->MovementExpired();
                        if (hearth)
                            member->SetHomebindToLocation(loc, area->ID);

                        member->GetMotionMaster()->Clear();
                        member->TeleportTo(loc.mapid, x, y, z, 0);
                        member->SendHeartBeat();
                        member->GetPlayerbotAI()->Reset(true);
                    }

                }
            }
            return;
        }
    }

    sLog.outError("Cannot teleport bot %s - no locations available", bot->GetName());
}

std::vector<std::pair<uint32, uint32>> RandomPlayerbotMgr::RpgLocationsNear(WorldLocation pos, const std::map<uint32, std::map<uint32, std::vector<std::string>>>& areaNames, uint32 radius)
{
    std::vector<std::pair<uint32, uint32>> results;
    float minDist = FLT_MAX;
    WorldPosition areaPos(pos);
    std::string hasZone = "-", wantZone = areaPos.getAreaName(true, true);

    for (uint32 level = 1; level < sPlayerbotAIConfig.randomBotMaxLevel + 1; level++)
    {
        for (uint32 r = 1; r < MAX_RACES; r++)
        {
            uint32 i = 0;
            for (auto p : rpgLocsCacheLevel[r][level])
            {
                std::string currentZone = areaNames.at(level).at(r)[i];
                i++;

                if (currentZone != wantZone && hasZone == wantZone) //If we already have the right id but this location isn't in the right id. Skip it.
                    continue;

                if (currentZone == wantZone && hasZone != wantZone) //If this is the first spot with a good area id use this now.
                    minDist = FLT_MAX;

                float dist = WorldPosition(pos).fDist(p);

                if (dist > radius || dist > minDist)
                    continue;

                if (dist < minDist)
                    results.clear();

                results.push_back(std::make_pair(r, level));

                hasZone = currentZone;

                minDist = dist;
            }
        }
    }

    return results;
}

void RandomPlayerbotMgr::PrepareTeleportCache()
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

    auto results = CharacterDatabase.PQuery("SELECT `map_id`, `x`, `y`, `z`, `level` FROM `ai_playerbot_tele_cache`");
    if (results)
    {
        sLog.outString("Loading random teleport caches for %d levels...", maxLevel);
        do
        {
            Field* fields = results->Fetch();
            uint16 mapId = fields[0].GetUInt16();
            float x = fields[1].GetFloat();
            float y = fields[2].GetFloat();
            float z = fields[3].GetFloat();
            uint16 level = fields[4].GetUInt16();
            WorldLocation loc(mapId, x, y, z, 0);
            locsPerLevelCache[level].push_back(loc);
        } while (results->NextRow());
    }
    else
    {
        sLog.outString("Preparing random teleport caches for %d levels...", maxLevel);
        BarGoLink bar(maxLevel);
        for (uint8 level = 1; level <= maxLevel; level++)
        {
            auto results = WorldDatabase.PQuery("SELECT `map`, `position_x`, `position_y`, `position_z` "
                "FROM (SELECT `map`, `position_x`, `position_y`, `position_z`, t.maxlevel, t.minlevel, "
                "%u - (t.maxlevel + t.minlevel) / 2 delta "
                "FROM creature c INNER JOIN creature_template t ON c.id = t.entry WHERE t.CreatureType != 8 AND t.NpcFlags = 0 AND t.Rank = 0 AND NOT (t.extraFlags & 1024 OR t.extraFlags & 65536 OR t.extraflags & 64 OR t.unitFlags & 256 OR t.unitFlags & 512) AND t.lootid != 0) q "
                "WHERE delta >= 0 AND delta <= %u AND map in (%s)",
                level,
                sPlayerbotAIConfig.randomBotTeleLevel,
                sPlayerbotAIConfig.randomBotMapsAsString.c_str()
            );
            if (results)
            {
                CharacterDatabase.BeginTransaction();
                do
                {
                    Field* fields = results->Fetch();
                    uint16 mapId = fields[0].GetUInt16();
                    float x = fields[1].GetFloat();
                    float y = fields[2].GetFloat();
                    float z = fields[3].GetFloat();
                    WorldLocation loc(mapId, x, y, z, 0);
                    locsPerLevelCache[level].push_back(loc);

                    CharacterDatabase.PExecute("INSERT INTO `ai_playerbot_tele_cache` (`level`, `map_id`, `x`, `y`, `z`) VALUES (%u, %u, %f, %f, %f)",
                        level, mapId, x, y, z);
                } while (results->NextRow());
                CharacterDatabase.CommitTransaction();
            }
            bar.step();
        }
    }

    sLog.outString("Preparing RPG teleport caches for %d factions...", sFactionTemplateStore.GetNumRows());

    results = WorldDatabase.PQuery("SELECT map, position_x, position_y, position_z, "
        "r.race, r.minl, r.maxl "
        "FROM creature c INNER JOIN ai_playerbot_rpg_races r ON c.id = r.entry "
        "WHERE r.race < 15");

    if (results)
    {
        do
        {
            for (uint32 level = 1; level < sPlayerbotAIConfig.randomBotMaxLevel + 1; level++)
            {
                Field* fields = results->Fetch();
                uint16 mapId = fields[0].GetUInt16();
                float x = fields[1].GetFloat();
                float y = fields[2].GetFloat();
                float z = fields[3].GetFloat();
                //uint32 faction = fields[4].GetUInt32();
                //string name = fields[5].GetCppString();
                uint32 race = fields[4].GetUInt32();
                uint32 minl = fields[5].GetUInt32();
                uint32 maxl = fields[6].GetUInt32();

                if (level > maxl || level < minl) continue;

                WorldLocation loc(mapId, x, y, z, 0);
                for (uint32 r = 1; r < MAX_RACES; r++)
                {
                    if (race == r || race == 0) rpgLocsCacheLevel[r][level].push_back(loc);
                }
            }
            //bar.step();
        } while (results->NextRow());
    }

    sLog.outString("Enhancing RPG teleport cache");

    std::map<uint32, std::map<uint32, std::vector<std::string>>> areaNames;

    for (uint32 level = 1; level < sPlayerbotAIConfig.randomBotMaxLevel + 1; level++)
    {
        for (uint32 r = 1; r < MAX_RACES; r++)
        {
            for (auto p : rpgLocsCacheLevel[r][level])
            {
                areaNames[level][r].push_back(WorldPosition(p).getAreaName(true, true));
            }
        }
    }

    std::vector<std::pair<std::pair<uint32, uint32>, WorldPosition>> newPoints;
    std::vector<std::pair<std::pair<uint32, uint32>, GuidPosition>> innPoints;

    //Static portals.
    for (auto& goData : WorldPosition().getGameObjectsNear(0, 0))
    {
        GuidPosition go(goData);

        auto data = sGOStorage.LookupEntry<GameObjectInfo>(go.GetEntry());

        if (!data)
            continue;

        if (data->type != GAMEOBJECT_TYPE_SPELLCASTER)
            continue;

        const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(data->spellcaster.spellId);

        if (pSpellInfo->EffectTriggerSpell[0])
            pSpellInfo = sServerFacade.LookupSpellInfo(pSpellInfo->EffectTriggerSpell[0]);

        if (pSpellInfo->Effect[0] != SPELL_EFFECT_TELEPORT_UNITS && pSpellInfo->Effect[1] != SPELL_EFFECT_TELEPORT_UNITS && pSpellInfo->Effect[2] != SPELL_EFFECT_TELEPORT_UNITS)
            continue;

        SpellTargetPosition const* pos = sSpellMgr.GetSpellTargetPosition(pSpellInfo->Id);

        if (!pos)
            continue;

        std::vector<std::pair<uint32, uint32>> ranges = RpgLocationsNear(WorldPosition(pos), areaNames);

        for (auto& range : ranges)
            newPoints.push_back(std::make_pair(std::make_pair(range.first, range.second), pos));
    }

    //Creatures.
    for (auto& creatureData : WorldPosition().getCreaturesNear(0, 0))
    {
        CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(creatureData->second.creature_id[0]);

        if (!cInfo)
            continue;

        if (cInfo->ExtraFlags & CREATURE_EXTRA_FLAG_INVISIBLE)
            continue;

        std::vector<uint32> allowedNpcFlags;

        allowedNpcFlags.push_back(UNIT_NPC_FLAG_BATTLEMASTER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_BANKER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_AUCTIONEER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_TRAINER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_VENDOR);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_REPAIR);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_INNKEEPER);

        for (auto flag : allowedNpcFlags)
        {          
            if ((cInfo->NpcFlags & flag) != 0)
            {
                std::vector<std::pair<uint32, uint32>> ranges = RpgLocationsNear(WorldPosition(creatureData), areaNames);

                if (cInfo->NpcFlags & UNIT_NPC_FLAG_INNKEEPER)
                {
                    for (auto& range : ranges)
                        innPoints.push_back(std::make_pair(std::make_pair(range.first, range.second), creatureData));
                }
                else
                {
                    for (auto& range : ranges)
                        newPoints.push_back(std::make_pair(std::make_pair(range.first, range.second), creatureData));
                }
                break;
            }
        }
    }

    for (auto newPoint : newPoints)
        rpgLocsCacheLevel[newPoint.first.first][newPoint.first.second].push_back(newPoint.second);
    
    for (auto innPoint : innPoints)
        innCacheLevel[innPoint.first.first][innPoint.first.second].push_back(std::make_pair(innPoint.second, innPoint.second));
}

void RandomPlayerbotMgr::PrintTeleportCache()
{
    sPlayerbotAIConfig.openLog("telecache.csv", "w");

    for (auto& l : sRandomPlayerbotMgr.locsPerLevelCache)
    {
        uint32 level = l.first;
        for (auto& p : l.second)
        {
            std::ostringstream out;
            out << level << ",";
            WorldPosition(p).printWKT(out);
            out << "LEVEL" << ",0," << WorldPosition(p).getAreaName(true, true);
            sPlayerbotAIConfig.log("telecache.csv", out.str().c_str());
        }
    }

    for (auto r : sRandomPlayerbotMgr.rpgLocsCacheLevel)
    {
        uint32 race =  r.first;
        for (auto& l : r.second)
        {
            uint32 level = l.first;
            for (auto& p : l.second)
            {
                std::ostringstream out;
                out << level << ",";
                WorldPosition(p).printWKT(out);
                out << "RPG" << "," << race << "," << WorldPosition(p).getAreaName(true, true);
                sPlayerbotAIConfig.log("telecache.csv", out.str().c_str());
            }
        }
    }
}

void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot, bool activeOnly)
{
    if (bot->InBattleGround())
        return;

    sLog.outDetail("Preparing location to random teleporting bot %s for level %u", bot->GetName(), bot->GetLevel());
    RandomTeleport(bot, locsPerLevelCache[bot->GetLevel()], false, activeOnly);
    Refresh(bot);

    WorldPosition botPos(bot);

    ObjectGuid closestInn;
    float minDistance = -1.0f;
    for (auto& [innGuid, innPosition] : innCacheLevel[bot->getRace()][bot->GetLevel()])
    {
        float distance = botPos.sqDistance(innPosition);
        if (minDistance > 0 || distance >= minDistance)
            continue;

        minDistance = distance;
        closestInn = innGuid;
    }

    if (closestInn)
    {
        WorldPacket data(SMSG_TRAINER_BUY_SUCCEEDED, (8 + 4));
        data << closestInn;
        data << uint32(3286);                                   // Bind
        bot->GetSession()->SendPacket(data);
    }
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot)
{
    if (bot->InBattleGround())
        return;

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "RandomTeleport");
    std::vector<WorldLocation> locs;

    std::list<Unit*> targets;
    float range = sPlayerbotAIConfig.randomBotTeleportDistance;
    MaNGOS::AnyUnitInObjectRangeCheck u_check(bot, range);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(targets, u_check);
    Cell::VisitAllObjects(bot, searcher, range);

    if (!targets.empty())
    {
        for (std::list<Unit *>::iterator i = targets.begin(); i != targets.end(); ++i)
        {
            Unit* unit = *i;
            bot->SetPosition(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), 0);
            FleeManager manager(bot, sPlayerbotAIConfig.sightDistance, 0, true);
            float rx, ry, rz;
            if (manager.CalculateDestination(&rx, &ry, &rz))
            {
                WorldLocation loc(bot->GetMapId(), rx, ry, rz);
                locs.push_back(loc);
            }
        }
    }
    else
    {
        RandomTeleportForLevel(bot, true);
    }

    pmo.reset();

    Refresh(bot);
}

void RandomPlayerbotMgr::InstaRandomize(Player* bot)
{
    sRandomPlayerbotMgr.Randomize(bot);

    if(bot->GetLevel() > sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL))
        sRandomPlayerbotMgr.RandomTeleportForLevel(bot, false);
}

void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetSession()->isLogingOut())
        return;

    bool initialRandom = false;
    if (bot->GetLevel() <= sPlayerbotAIConfig.randombotStartingLevel)
        initialRandom = true;
#ifdef MANGOSBOT_TWO
    else if (bot->GetLevel() < 60 && bot->getClass() == CLASS_DEATH_KNIGHT)
        initialRandom = true;
#endif

    // give bot random level if is above or below level sync
    if (!initialRandom && players.size() && sPlayerbotAIConfig.syncLevelWithPlayers)
    {
        uint32 maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel + sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)));
        if (bot->GetLevel() > maxLevel || (bot->GetLevel() + sPlayerbotAIConfig.syncLevelMaxAbove) < playersLevel)
            initialRandom = true;
    }

    if (initialRandom)
    {
        RandomizeFirst(bot);
        sLog.outDetail("Bot #%d %s:%d <%s>: gear/level randomised", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
    }
    else if (sPlayerbotAIConfig.randomGearUpgradeEnabled)
    {
        UpdateGearSpells(bot);
        sLog.outDetail("Bot #%d %s:%d <%s>: gear upgraded", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
    }
    else
    {
        // schedule randomise
        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
        SetEventValue(bot->GetGUIDLow(), "randomize", 1, randomTime);
    }

    //SetValue(bot, "version", MANGOSBOT_VERSION);
}

void RandomPlayerbotMgr::UpdateGearSpells(Player* bot)
{
    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "UpgradeGear");

    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

    uint32 lastLevel = GetValue(bot, "level");
    uint32 level = bot->GetLevel();
    PlayerbotFactory factory(bot, level);
    factory.Randomize(true, false);

    if (lastLevel != level)
        SetValue(bot, "level", level);

    // schedule randomise
    uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    SetEventValue(bot->GetGUIDLow(), "randomize", 1, randomTime);
}

void RandomPlayerbotMgr::RandomizeFirst(Player* bot)
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

    // if lvl sync is enabled, max level is limited by online players lvl
    if (sPlayerbotAIConfig.syncLevelWithPlayers)
        maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel+ sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)));

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "RandomizeFirst");
    uint32 level = urand(std::max(uint32(sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL)), sPlayerbotAIConfig.randomBotMinLevel), maxLevel);

#ifdef MANGOSBOT_TWO
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
        level = urand(std::max(bot->GetLevel(), sWorld.getConfig(CONFIG_UINT32_START_HEROIC_PLAYER_LEVEL)), std::max(sWorld.getConfig(CONFIG_UINT32_START_HEROIC_PLAYER_LEVEL), maxLevel));
#endif

    if (urand(0, 100) < 100 * sPlayerbotAIConfig.randomBotMaxLevelChance && level < maxLevel)
        level = maxLevel;

#ifndef MANGOSBOT_ZERO
    if (sWorldState.GetExpansion() == EXPANSION_NONE && level > 60)
        level = 60;
#endif

#ifdef MANGOSBOT_TWO
    // do not allow level down death knights
    if (bot->getClass() == CLASS_DEATH_KNIGHT && level < sWorld.getConfig(CONFIG_UINT32_START_HEROIC_PLAYER_LEVEL))
        return;

    // only randomise death knights to min lvl 60
    if (bot->getClass() == CLASS_DEATH_KNIGHT && level < 60)
        level = 60;
#endif

    if (level == sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL))
        return;

    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false, false);

    // schedule randomise
    uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    SetEventValue(bot->GetGUIDLow(), "randomize", 1, randomTime);

    bool hasPlayer = bot->GetPlayerbotAI()->HasRealPlayerMaster();
    bot->GetPlayerbotAI()->Reset(!hasPlayer);

    if (bot->GetGroup() && !hasPlayer)
        bot->RemoveFromGroup();
}

uint32 RandomPlayerbotMgr::GetZoneLevel(uint16 mapId, float teleX, float teleY, float teleZ)
{
	uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

	uint32 level;
    auto results = WorldDatabase.PQuery("SELECT AVG(t.minlevel) minlevel, AVG(t.maxlevel) maxlevel FROM creature c "
            "INNER JOIN creature_template t ON c.id = t.entry "
            "WHERE map = '%u' AND minlevel > 1 AND abs(position_x - '%f') < '%u' AND abs(position_y - '%f') < '%u'",
            mapId, teleX, sPlayerbotAIConfig.randomBotTeleportDistance / 2, teleY, sPlayerbotAIConfig.randomBotTeleportDistance / 2);

    if (results)
    {
        Field* fields = results->Fetch();
        uint8 minLevel = fields[0].GetUInt8();
        uint8 maxLevel = fields[1].GetUInt8();
        level = urand(minLevel, maxLevel);
        if (level > maxLevel)
            level = maxLevel;
    }
    else
    {
        level = urand(1, maxLevel);
    }

    return level;
}

void RandomPlayerbotMgr::Refresh(Player* bot)
{
    if (bot->IsBeingTeleportedFar() || !bot->IsInWorld())
        return;

    if (sServerFacade.UnitIsDead(bot))
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        bot->GetPlayerbotAI()->ResetStrategies();
    }

    if (sPlayerbotAIConfig.disableRandomLevels)
        return;

    if (bot->InBattleGround())
        return;

    sLog.outDetail("Refreshing bot #%d <%s>", bot->GetGUIDLow(), bot->GetName());
    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "Refresh");

    bot->GetPlayerbotAI()->Reset();

    bot->DurabilityRepairAll(false, 1.0f
#ifndef MANGOSBOT_ZERO
        , false
#endif
    );
	bot->SetHealthPercent(100);
	bot->SetPvP(true);

    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.Refresh();

    if (bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

    if (bot->GetMaxPower(POWER_ENERGY) > 0)
        bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));

    uint32 money = bot->GetMoney();
    bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));
}

bool RandomPlayerbotMgr::IsRandomBot(Player* bot)
{
    if (bot && bot->GetPlayerbotAI())
    {
        if (bot->GetPlayerbotAI()->IsRealPlayer())
            return false;
    }
    if (bot)
    {
        if (sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
            return true;

        return IsRandomBot(bot->GetGUIDLow());
    }

    return false;
}

bool RandomPlayerbotMgr::IsRandomBot(uint32 bot)
{
    ObjectGuid guid = ObjectGuid(HIGHGUID_PLAYER, bot);
    if (sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guid)))
        return true;

    return GetEventValue(bot, "add");
}

std::list<uint32> RandomPlayerbotMgr::GetBots()
{
    if (!currentBots.empty()) return currentBots;

    auto results = CharacterDatabase.Query(
            "SELECT bot FROM ai_playerbot_random_bots WHERE owner = 0 AND event = 'add'");

    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 bot = fields[0].GetUInt32();
            currentBots.push_back(bot);
        } while (results->NextRow());
    }

    return currentBots;
}

std::list<uint32> RandomPlayerbotMgr::GetBgBots(uint32 bracket)
{
    //if (!currentBgBots.empty()) return currentBgBots;

    auto results = CharacterDatabase.PQuery(
        "SELECT bot FROM ai_playerbot_random_bots WHERE event = 'bg' AND value = '%d'", bracket);
    std::list<uint32> BgBots;
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 bot = fields[0].GetUInt32();
            BgBots.push_back(bot);
        } while (results->NextRow());
    }

    return BgBots;
}

uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string event)
{
    std::lock_guard<std::mutex> guard(m_eventCacheMutex);

    // load all events at once on first event load
    if (eventCache[bot].empty())
    {
        auto results = CharacterDatabase.PQuery("SELECT `event`, `value`, `time`, validIn, `data` FROM ai_playerbot_random_bots WHERE owner = 0 AND bot = '%u'", bot);
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                // defensive null-handling.
                // Penqle's Field::GetString() returns the bare pointer without null-check;
                // constructing std::string from nullptr is UB and segfaults. The bot's
                // ai_playerbot_random_bots table has `data` column nullable, and most rows
                // store NULL there. Guard against null so the cache load doesn't crash.
                const char* nameStr = fields[0].GetString();
                const char* dataStr = fields[4].GetString();
                std::string eventName = nameStr ? nameStr : "";
                if (eventName.empty())
                    continue; // skip malformed rows entirely
                CachedEvent e;
                e.value = fields[1].GetUInt32();
                e.lastChangeTime = fields[2].GetUInt32();
                e.validIn = fields[3].GetUInt32();
                e.data = dataStr ? std::string(dataStr) : std::string();
                eventCache[bot][eventName] = e;
            } while (results->NextRow());
        }
    }
    CachedEvent e = eventCache[bot][event];

    if ((time(0) - e.lastChangeTime) >= e.validIn && event != "specNo" && event != "specLink" && event != "init" && event != "current_time" && event != "always" && event != "selfbot")
        e.value = 0;

    return e.value;
}

int32 RandomPlayerbotMgr::GetValueValidTime(uint32 bot, std::string event)
{
    std::lock_guard<std::mutex> guard(m_eventCacheMutex);

    if (eventCache.find(bot) == eventCache.end())
        return 0;

    if (eventCache[bot].find(event) == eventCache[bot].end())
        return 0;

    CachedEvent e = eventCache[bot][event];

    return e.validIn-(time(0) - e.lastChangeTime);
}

std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string event)
{
    std::string data = "";
    if (GetEventValue(bot, event))
    {
        std::lock_guard<std::mutex> guard(m_eventCacheMutex);
        CachedEvent e = eventCache[bot][event];
        data = e.data;
    }
    return data;
}

uint32 RandomPlayerbotMgr::SetEventValue(uint32 bot, std::string event, uint32 value, uint32 validIn, std::string data)
{
    std::lock_guard<std::mutex> guard(m_eventCacheMutex);

    std::string eventSql = event;
    CharacterDatabase.escape_string(eventSql);

    std::string dataSql = data;
    CharacterDatabase.escape_string(dataSql);

    std::ostringstream sql;
    bool writeOk = false;

    if (value)
    {
        if (!data.empty())
        {
            sql << "INSERT INTO ai_playerbot_random_bots (owner, bot, `time`, validIn, event, `value`, `data`) VALUES (0, "
                << bot << ", " << static_cast<uint32>(time(0)) << ", " << validIn << ", '" << eventSql << "', " << value
                << ", '" << dataSql << "') ON DUPLICATE KEY UPDATE `time` = VALUES(`time`), validIn = VALUES(validIn), `value` = VALUES(`value`), `data` = VALUES(`data`)";
        }
        else
        {
            sql << "INSERT INTO ai_playerbot_random_bots (owner, bot, `time`, validIn, event, `value`, `data`) VALUES (0, "
                << bot << ", " << static_cast<uint32>(time(0)) << ", " << validIn << ", '" << eventSql << "', " << value
                << ", NULL) ON DUPLICATE KEY UPDATE `time` = VALUES(`time`), validIn = VALUES(validIn), `value` = VALUES(`value`), `data` = NULL";
        }

        writeOk = ExecuteRandomBotEventWrite(sql.str());
    }
    else
    {
        sql << "DELETE FROM ai_playerbot_random_bots WHERE owner = 0 AND bot = " << bot << " AND event = '" << eventSql << "'";
        writeOk = ExecuteRandomBotEventWrite(sql.str());
    }

    if (!writeOk)
    {
        sLog.outError("RandomPlayerbotMgr::SetEventValue failed for bot=%u event=%s value=%u", bot, event.c_str(), value);
        return 0;
    }

    CachedEvent e(value, (uint32)time(0), validIn, data);
    eventCache[bot][event] = e;
    return value;
}

uint32 RandomPlayerbotMgr::GetValue(uint32 bot, std::string type)
{
    return GetEventValue(bot, type);
}

uint32 RandomPlayerbotMgr::GetValue(Player* bot, std::string type)
{
    return GetValue(bot->GetObjectGuid().GetCounter(), type);
}

std::string RandomPlayerbotMgr::GetData(uint32 bot, std::string type)
{
    return GetEventData(bot, type);
}

void RandomPlayerbotMgr::SetValue(uint32 bot, std::string type, uint32 value, std::string data, int32 validIn)
{
    SetEventValue(bot, type, value, validIn == -1 ? 15*24*3600 : validIn, data);
}

void RandomPlayerbotMgr::SetValue(Player* bot, std::string type, uint32 value, std::string data, int32 validIn)
{
    SetValue(bot->GetObjectGuid().GetCounter(), type, value, data, validIn);
}

bool RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        sLog.outError("Playerbot system is currently disabled!");
        return false;
    }

    bool isRA = false;
    
    if (handler->GetSession()) //Client command
        isRA = true;
    else if (static_cast<CliHandler*>(handler) && static_cast<CliHandler*>(handler)->GetAccountId()) //RA call with account.
        isRA = true;

    if (!args || !*args)
    {
        sLog.outError("Usage: rndbot help/stats/update/reset/init/refresh/add/remove/more..");
        if (isRA)
            handler->SendSysMessage("Usage: rndbot help/stats/update/reset/init/refresh/add/remove/more..");

        std::list<std::string> messages = sRandomPlayerbotMgr.HandleHelp("");

        for (auto& msg : messages)
        {
            sLog.outString("%s", msg.c_str());
            if (isRA)
                handler->SendSysMessage(msg.c_str());
        }

        return true;
    }

    std::string cmd = args;

    std::map<std::string, ConsoleCommandHandler> handlers;
    handlers["help"] = &RandomPlayerbotMgr::HandleHelp;
    handlers["reset"] = &RandomPlayerbotMgr::HandleConsoleReset;
    handlers["stats"] = &RandomPlayerbotMgr::HandleConsoleStats;
    handlers["update"] = &RandomPlayerbotMgr::HandleConsoleUpdate;
    handlers["pid "] = &RandomPlayerbotMgr::HandleConsolePid;
    handlers["diff"] = &RandomPlayerbotMgr::HandleConsoleDiff;
    handlers["diff "] = &RandomPlayerbotMgr::HandleConsoleDiff;
    handlers["clean map"] = &RandomPlayerbotMgr::HandleConsoleCleanMap;
    handlers["login debug"] = &RandomPlayerbotMgr::HandleConsoleLoginDebug;

    for (auto& [prefix, consoleHandler] : handlers)
    {
        if (cmd.find(prefix) != 0)
            continue;

        size_t prefixLen = prefix.size();
        std::string param = cmd.size() > prefixLen + 1 ? cmd.substr(prefixLen + 1) : "";

        if (prefix == "stats")
            param = handler->GetSession() ? std::to_string(handler->GetSession()->GetPlayer()->GetObjectGuid()) : "";

        std::list<std::string> messages = (sRandomPlayerbotMgr.*consoleHandler)(param);
        for (auto& msg : messages)
        {
            sLog.outString("%s", msg.c_str());
            if(isRA)
                handler->SendSysMessage(msg.c_str());      
        }

        if (!messages.empty() && (prefix != "help" || param != "commands"))
            return true;
    }

    std::map<std::string, ConsolePlayerCommandHandler> playerHandlers;
    playerHandlers["init"] = &RandomPlayerbotMgr::HandleRandomizeFirst;
    playerHandlers["upgrade"] = &RandomPlayerbotMgr::HandleUpdateGearSpells;
    playerHandlers["refresh"] = &RandomPlayerbotMgr::HandleRefresh;
    playerHandlers["teleport"] = &RandomPlayerbotMgr::HandleRandomTeleportForLevel;
    playerHandlers["rpg"] = &RandomPlayerbotMgr::HandleRandomTeleportForRpg;
    playerHandlers["revive"] = &RandomPlayerbotMgr::HandleRevive;
    playerHandlers["grind"] = &RandomPlayerbotMgr::HandleRandomTeleport;
    playerHandlers["change_strategy"] = &RandomPlayerbotMgr::HandleChangeStrategy;
    playerHandlers["remove"] = &RandomPlayerbotMgr::HandleRemove;

    for (auto& [prefix, playerHandler] : playerHandlers)
    {
        if (cmd.find(prefix) != 0)
            continue;

        size_t prefixLen = prefix.size();
        std::string nameAndParams = cmd.size() > prefixLen + 1 ? cmd.substr(prefixLen + 1) : "";

        std::string name = "%";
        std::string params = "";

        if (!nameAndParams.empty())
        {
            size_t spacePos = nameAndParams.find(' ');
            if (spacePos != std::string::npos)
            {
                name = nameAndParams.substr(0, spacePos);
                params = nameAndParams.substr(spacePos + 1);
            }
            else
            {
                name = nameAndParams;
            }
        }

        sRandomPlayerbotMgr.consoleCmdParams = params;

        bool hasRandomBotCommand = false;

        ConsolePlayerCommandHandler handler_copy = playerHandler;

        sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* bot) {
            std::string botName = bot->GetName();
            if (botName.find(name) == 0)
            {

                std::list<std::string> messages = (sRandomPlayerbotMgr.*handler_copy)(bot);
                for (auto& msg : messages)
                {
                    sLog.outString("%s", msg.c_str());
                    if (isRA)
                        handler->SendSysMessage(msg.c_str());
                    hasRandomBotCommand = true;
                }
            }
        });

        if (hasRandomBotCommand)
            return true;
    }

    std::list<std::string> messages = sRandomPlayerbotMgr.HandlePlayerbotCommand(args, handler->GetSession() ? handler->GetSession()->GetPlayer():nullptr, static_cast<CliHandler*>(handler) ? static_cast<CliHandler*>(handler)->GetAccessLevel() : SEC_PLAYER);
    for (std::list<std::string>::iterator i = messages.begin(); i != messages.end(); ++i)
    {
        sLog.outString("%s", i->c_str());
        if (isRA)
            handler->SendSysMessage(i->c_str());
    }

    if (!messages.empty())
        return true;

    if (isRA)
        handler->SendSysMessage("usage: help/list/reload/more.. or add/init/remove/more.. PLAYERNAME");

    return true;
}

void RandomPlayerbotMgr::HandleCommand(uint32 type, const std::string& text, Player& fromPlayer, std::string channelName, Team team, uint32 lang)
{
    ForEachPlayerbot([&](Player* bot)
    {
        if (type == CHAT_MSG_SAY)
        {
            if (bot->GetMapId() != fromPlayer.GetMapId() || sServerFacade.GetDistance2d(bot, &fromPlayer) > 25)
            {
                return;
            }
        }

        if (type == CHAT_MSG_YELL)
        {
            if (bot->GetMapId() != fromPlayer.GetMapId() || sServerFacade.GetDistance2d(bot, &fromPlayer) > 300)
            {
                return;
            }
        }

        if (team != TEAM_BOTH_ALLOWED && bot->GetTeam() != team)
        {
            return;
        }

        if (type == CHAT_MSG_GUILD && bot->GetGuildId() != fromPlayer.GetGuildId())
        {
            return;
        }

        if (!channelName.empty())
        {
            if (ChannelMgr* cMgr = channelMgr(bot->GetTeam()))
            {
                Channel* chn = cMgr->GetChannel(channelName, bot);
                if (!chn)
                {
                    return;
                }
            }
        }

        bot->GetPlayerbotAI()->HandleCommand(type, text, fromPlayer, lang);
    });
}

void RandomPlayerbotMgr::OnPlayerLogout(Player* player)
{
    bool hadPlayerBot = GetPlayerBot(player->GetGUIDLow());

    DisablePlayerBot(player->GetGUIDLow());

    if (!hadPlayerBot && player->GetPlayerbotAI() && player->GetPlayerbotAI()->IsRealPlayer() && player->GetGroup() && sPlayerbotAIConfig.IsFreeAltBot(player))
        player->GetSession()->SetOffline(); //Prevent groupkick

    ForEachPlayerbot([&](Player* bot) {
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (player == ai->GetMaster())
        {
            ai->SetMaster(NULL);
            if (!bot->InBattleGround())
            {
                ai->ResetStrategies();
            }
        }
    });

    {
        std::lock_guard<std::mutex> lock(playersMutex);
        players.erase(player->GetGUIDLow());
    }
}

void RandomPlayerbotMgr::OnBotLoginInternal(Player * const bot)
{
    sLog.outDetail("%u/%d Bot %s logged in", GetPlayerbotsAmount(), sRandomPlayerbotMgr.GetMaxAllowedBotCount(), bot->GetName());
	//if (loginProgressBar && playerBots.size() < sRandomPlayerbotMgr.GetMaxAllowedBotCount()) { loginProgressBar->step(); }
	//if (loginProgressBar && playerBots.size() >= sRandomPlayerbotMgr.GetMaxAllowedBotCount() - 1) {
    //if (loginProgressBar && playerBots.size() + 1 >= sRandomPlayerbotMgr.GetMaxAllowedBotCount()) {
	//	sLog.outString("All bots logged in");
    //    delete loginProgressBar;
	//}
}

void RandomPlayerbotMgr::OnPlayerLogin(Player* player)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    ForEachPlayerbot([&](Player* bot)
    {
        if (player == bot)
            return;

        Group* group = bot->GetGroup();
        if (!group)
            return;

        for (GroupReference *gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();
            PlayerbotAI* ai = bot->GetPlayerbotAI();
            if (member == player && (!ai->GetMaster() || ai->GetMaster()->GetPlayerbotAI()))
            {
                if (!bot->InBattleGround())
                {
                    ai->SetMaster(player);
                    ai->ResetStrategies();
                    ai->TellPlayer(ai->GetMaster(), BOT_TEXT("hello"));
                }
                break;
            }
        }
    });

    if (IsFreeBot(player))
    {
        uint32 guid = player->GetGUIDLow();
        if (!sPlayerbotAIConfig.IsFreeAltBot(player))
           SetEventValue(guid, "login", 0, 0);
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(playersMutex);
            players[player->GetGUIDLow()] = player;
        }
        sLog.outError("COLDDBG OnPlayerLogin added real player %s map=%u players=%u (bots will wake near them)",
            player->GetName(), player->GetMapId(), (uint32)players.size());
    }
}

void RandomPlayerbotMgr::OnPlayerLoginError(uint32 bot)
{
    SetEventValue(bot, "add", 0, 0);
    SetEventValue(bot, "login", 0, 0);
    currentBots.remove(bot);
}

Player* RandomPlayerbotMgr::GetRandomPlayer()
{
    if (players.empty())
        return NULL;

    uint32 index = urand(0, players.size() - 1);
    return players[index];
}

Player* RandomPlayerbotMgr::GetPlayer(uint32 playerGuid)
{
    PlayerBotMap::const_iterator it = players.find(playerGuid);
    return (it == players.end()) ? nullptr : it->second ? it->second : nullptr;
}

void RandomPlayerbotMgr::PrintStats(uint32 requesterGuid)
{
    Player* requester = GetPlayer(requesterGuid);
    std::stringstream ss; ss << GetPlayerbotsAmount() << " Random Bots online";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    std::map<uint32, int> alliance, horde;
    for (uint32 i = 0; i < 10; ++i)
    {
        alliance[i] = 0;
        horde[i] = 0;
    }

    std::map<uint8, int> perRace, perClass;
    for (uint8 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        perRace[race] = 0;
    }
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        perClass[cls] = 0;
    }

    uint32 dps = 0, heal = 0, tank = 0, active = 0, update = 0, randomize = 0, teleport = 0, changeStrategy = 0, dead = 0, combat = 0, revive = 0, taxi = 0, moving = 0, mounted = 0, afk = 0;
    int stateCount[(uint8)TravelState::MAX_TRAVEL_STATE + 1] = { 0 };
    std::vector<std::pair<Quest const*, int32>> questCount;

    ForEachPlayerbot([this, &dps, &heal, &tank, &active, &update, &randomize, &teleport, &changeStrategy, &dead, &combat, &revive, &taxi, &moving, &mounted, &afk, &alliance, &horde, &perRace, &perClass, &stateCount, &questCount](Player* bot)
    {
        if (IsAlliance(bot->getRace()))
            alliance[bot->GetLevel() / 10]++;
        else
            horde[bot->GetLevel() / 10]++;

        perRace[bot->getRace()]++;
        perClass[bot->getClass()]++;

        if (bot->GetPlayerbotAI()->AllowActivity())
            active++;

        if (bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<bool>("random bot update")->Get())
            update++;

        uint32 botId = bot->GetGUIDLow();
        if (!GetEventValue(botId, "randomize"))
            randomize++;

        if (!GetEventValue(botId, "teleport"))
            teleport++;

        if (!GetEventValue(botId, "change_strategy"))
            changeStrategy++;

        if (bot->IsTaxiFlying())
            taxi++;

        if (bot->IsMoving() && !bot->IsTaxiFlying() && !bot->IsFlying())
            moving++;

        if (bot->IsMounted() && !bot->IsTaxiFlying())
            mounted++;

        if (bot->IsInCombat())
            combat++;

        if (bot->isAFK())
            afk++;

        if (sServerFacade.UnitIsDead(bot))
        {
            dead++;
            //if (!GetEventValue(botId, "dead"))
            //    revive++;
        }

        int spec = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->getClass())
        {
        case CLASS_DRUID:
            if (spec == 2)
                heal++;
            else
                dps++;
            break;
        case CLASS_PALADIN:
            if (spec == 1)
                tank++;
            else if (spec == 0)
                heal++;
            else
                dps++;
            break;
        case CLASS_PRIEST:
            if (spec != 2)
                heal++;
            else
                dps++;
            break;
        case CLASS_SHAMAN:
            if (spec == 2)
                heal++;
            else
                dps++;
            break;
        case CLASS_WARRIOR:
            if (spec == 2)
                tank++;
            else
                dps++;
            break;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            if (spec == 0)
                tank++;
            else
                dps++;
            break;
#endif
        default:
            dps++;
            break;
        }

        TravelTarget* target = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target)
        {
            TravelState state = target->GetTravelState();
            stateCount[(uint8)state]++;            
        }
    });

    ss.str(""); ss << "Bots level:";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

	uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
	for (uint32 i = 0; i < 10; ++i)
    {
        if (!alliance[i] && !horde[i])
            continue;

        uint32 from = i*10;
        uint32 to = std::min(from + 9, maxLevel);
        if (!from) from = 1;

        ss.str(""); ss << "    " << from << ".." << to << ": " << alliance[i] << " alliance, " << horde[i] << " horde";
        sLog.outString("%s", ss.str().c_str());
        if (requester) { requester->SendMessageToPlayer(ss.str()); }
    }

    ss.str(""); ss << "Bots race:";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    for (uint8 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        if (perRace[race])
        {
            ss.str(""); ss << "    " << ChatHelper::formatRace(race) << ": " << perRace[race];
            sLog.outString("%s", ss.str().c_str());
            if (requester) { requester->SendMessageToPlayer(ss.str()); }
        }
    }

    ss.str(""); ss << "Bots class:";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (perClass[cls])
        {
            ss.str(""); ss << "    " << ChatHelper::formatClass(cls) << ": " << perClass[cls];
            sLog.outString("%s", ss.str().c_str());
            if (requester) { requester->SendMessageToPlayer(ss.str()); }
        }
    }

    ss.str(""); ss << "Bots role:";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    tank: " << tank << ", heal: " << heal << ", dps: " << dps;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "Bots status:";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Active: " << active;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Moving: " << moving;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    //sLog.outString("Bots to:");
    //sLog.outString("    update: %d", update);
    //sLog.outString("    randomize: %d", randomize);
    //sLog.outString("    teleport: %d", teleport);
    //sLog.outString("    change_strategy: %d", changeStrategy);
    //sLog.outString("    revive: %d", revive);

    ss.str(""); ss << "    On taxi: " << taxi;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    On mount: " << mounted;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    In combat: " << combat;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Dead: " << dead;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    AFK: " << afk;
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "Bots questing:";
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Picking quests: " << stateCount[(uint8)TravelState::TRAVEL_STATE_TRAVEL_PICK_UP_QUEST] + stateCount[(uint8)TravelState::TRAVEL_STATE_WORK_PICK_UP_QUEST];
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Doing quests: " << stateCount[(uint8)TravelState::TRAVEL_STATE_TRAVEL_DO_QUEST] + stateCount[(uint8)TravelState::TRAVEL_STATE_WORK_DO_QUEST];
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Completing quests: " << stateCount[(uint8)TravelState::TRAVEL_STATE_TRAVEL_HAND_IN_QUEST] + stateCount[(uint8)TravelState::TRAVEL_STATE_WORK_HAND_IN_QUEST];
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }

    ss.str(""); ss << "    Idling: " << stateCount[(uint8)TravelState::TRAVEL_STATE_IDLE];
    sLog.outString("%s", ss.str().c_str());
    if (requester) { requester->SendMessageToPlayer(ss.str()); }
}

double RandomPlayerbotMgr::GetBuyMultiplier(Player* bot)
{
    uint32 id = bot->GetGUIDLow();
    uint32 value = GetEventValue(id, "buymultiplier");
    if (!value)
    {
        value = urand(50, 120);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval, sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "buymultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

double RandomPlayerbotMgr::GetSellMultiplier(Player* bot)
{
    uint32 id = bot->GetGUIDLow();
    uint32 value = GetEventValue(id, "sellmultiplier");
    if (!value)
    {
        value = urand(80, 250);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval, sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "sellmultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

void RandomPlayerbotMgr::AddTradeDiscount(Player* bot, Player* master, int32 value)
{
    if (!master) return;
    uint32 discount = GetTradeDiscount(bot, master);
    int32 result = (int32)discount + value;
    discount = (result < 0 ? 0 : result);

    SetTradeDiscount(bot, master, discount);
}

void RandomPlayerbotMgr::SetTradeDiscount(Player* bot, Player* master, uint32 value)
{
    if (!master) return;
    uint32 botId =  bot->GetGUIDLow();
    uint32 masterId =  master->GetGUIDLow();
    std::ostringstream name; name << "trade_discount_" << masterId;
    SetEventValue(botId, name.str(), value, sPlayerbotAIConfig.maxRandomBotInWorldTime);
}

uint32 RandomPlayerbotMgr::GetTradeDiscount(Player* bot, Player* master)
{
    if (!master) return 0;
    uint32 botId =  bot->GetGUIDLow();
    uint32 masterId = master->GetGUIDLow();
    std::ostringstream name; name << "trade_discount_" << masterId;
    return GetEventValue(botId, name.str());
}

std::string RandomPlayerbotMgr::HandleRemoteCommand(std::string request)
{
    std::string::iterator pos = find(request.begin(), request.end(), ',');
    if (pos == request.end())
    {
        std::ostringstream out; out << "invalid request: " << request;
        return out.str();
    }

    std::string command = std::string(request.begin(), pos);
    uint32 guid = std::atoi(std::string(pos + 1, request.end()).c_str());
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return "invalid guid";

    PlayerbotAI *ai = bot->GetPlayerbotAI();
    if (!ai)
        return "invalid guid";

    return ai->HandleRemoteCommand(command);
}

void RandomPlayerbotMgr::ChangeStrategy(Player* player)
{
    uint32 bot = player->GetGUIDLow();

    if (urand(0, 100) > 100 * sPlayerbotAIConfig.randomBotRpgChance) // select grind / pvp
    {
        sLog.outDetail("Bot #%d %s:%d <%s>: sent to grind spot", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
        // teleport in different places only if players are online
        RandomTeleportForLevel(player, players.size());
        ScheduleTeleport(bot);
    }
    else
    {
        sLog.outDetail("Bot #%d %s:%d <%s>: sent to inn", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
        RandomTeleportForRpg(player, players.size());
        ScheduleTeleport(bot);
    }
}

void RandomPlayerbotMgr::RandomTeleportForRpg(Player* bot, bool activeOnly)
{
    uint32 race = bot->getRace();
    uint32 level = bot->GetLevel();
    sLog.outDetail("Random teleporting bot %s for RPG (%zu locations available)", bot->GetName(), rpgLocsCacheLevel[race][level].size());
    RandomTeleport(bot, rpgLocsCacheLevel[race][level], true, activeOnly);
    Refresh(bot);

    //Travel cooldown for 10 minutes.
    if (bot->GetPlayerbotAI())
    {
        AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        sTravelMgr.SetNullTravelTarget(travelTarget);
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
        travelTarget->SetExpireIn(10 * MINUTE * IN_MILLISECONDS);
    }
}

void RandomPlayerbotMgr::Remove(Player* bot)
{
    SC_LOG("RandomPlayerbotMgr::Remove entry bot=%s",
           bot ? bot->GetName() : "(null)");
    uint32 owner = bot->GetGUIDLow();
    SC_LOG("RandomPlayerbotMgr::Remove guid=%u — deleting random_bots row", owner);
    CharacterDatabase.PExecute("DELETE FROM ai_playerbot_random_bots WHERE owner = 0 AND bot = '%d'", owner);
    eventCache[owner].clear();
    SC_LOG("RandomPlayerbotMgr::Remove guid=%u — calling LogoutPlayerBot", owner);

    LogoutPlayerBot(owner);
    SC_LOG("RandomPlayerbotMgr::Remove guid=%u — DONE", owner);
}

const CreatureDataPair* RandomPlayerbotMgr::GetCreatureDataByEntry(uint32 entry)
{
    if (entry != 0 && sObjectMgr.GetCreatureTemplate(entry))
    {
        FindCreatureData worker(entry, NULL);
        sObjectMgr.DoCreatureData(worker);
        CreatureDataPair const* dataPair = worker.GetResult();
        return dataPair;
    }
    return NULL;
}

uint32 RandomPlayerbotMgr::GetCreatureGuidByEntry(uint32 entry)
{
    uint32 guid = 0;

    CreatureDataPair const* dataPair = sRandomPlayerbotMgr.GetCreatureDataByEntry(entry);
    guid = dataPair->first;

    return guid;
}

uint32 RandomPlayerbotMgr::GetBattleMasterEntry(Player* bot, BattleGroundTypeId bgTypeId, bool fake)
{
    Team team = bot->GetTeam();
    uint32 entry = 0;
    std::vector<uint32> Bms;

    for (auto i = begin(BattleMastersCache[team][bgTypeId]); i != end(BattleMastersCache[team][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    for (auto i = begin(BattleMastersCache[TEAM_BOTH_ALLOWED][bgTypeId]); i != end(BattleMastersCache[TEAM_BOTH_ALLOWED][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    if (Bms.empty())
        return entry;

    float dist1 = FLT_MAX;

    for (auto i = begin(Bms); i != end(Bms); ++i)
    {
        CreatureDataPair const* dataPair = sRandomPlayerbotMgr.GetCreatureDataByEntry(*i);
        if (!dataPair)
            continue;

        CreatureData const* data = &dataPair->second;

        Unit* Bm = sMapMgr.FindMap((uint32)data->position.mapid)->GetUnit(ObjectGuid(HIGHGUID_UNIT, *i, dataPair->first));
        if (!Bm)
            continue;

        if (bot->GetMapId() != Bm->GetMapId())
            continue;

        // return first available guid on map if queue from anywhere
        if (fake)
        {
            entry = *i;
            break;
        }

        AreaTableEntry const* area = GetAreaEntryByAreaID(sServerFacade.GetAreaId(Bm));
        if (!area)
            continue;

        if (area->team == 4 && bot->GetTeam() == ALLIANCE)
            continue;
        if (area->team == 2 && bot->GetTeam() == HORDE)
            continue;

        if (Bm->GetDeathState() == DEAD)
            continue;

        float dist2 = sServerFacade.GetDistance2d(bot, data->position.coord_x, data->position.coord_y);
        if (dist2 < dist1)
        {
            dist1 = dist2;
            entry = *i;
        }
    }

    return entry;
}

void RandomPlayerbotMgr::Hotfix(Player* bot, uint32 version)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    uint32 exp = bot->GetUInt32Value(PLAYER_XP);
    uint32 level = bot->GetLevel();
    uint32 id = bot->GetGUIDLow();

    for (int fix = version; fix <= MANGOSBOT_VERSION; fix++)
    {
        int count = 0;
        switch (fix)
        {
            case 1: // Apply class quests to previously made random bots

                if (level < 10)
                {
                    break;
                }

                for (std::list<uint32>::iterator i = factory.classQuestIds.begin(); i != factory.classQuestIds.end(); ++i)
                {
                    uint32 questId = *i;
                    Quest const *quest = sObjectMgr.GetQuestTemplate(questId);

                    if (!bot->SatisfyQuestClass(quest, false) ||
                        quest->GetMinLevel() > bot->GetLevel() ||
                        !bot->SatisfyQuestRace(quest, false) || bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                        continue;

                    bot->SetQuestStatus(questId, QUEST_STATUS_COMPLETE);
                    bot->RewardQuest(quest, 0, bot, false);
                    bot->SetLevel(level);
                    bot->SetUInt32Value(PLAYER_XP, exp);
                    sLog.outDetail("Bot %d rewarded quest %d",
                        bot->GetGUIDLow(), questId);
                    count++;
                }

                if (count > 0)
                {
                    sLog.outDetail("Bot %d hotfix (Class Quests), %d quests rewarded",
                        bot->GetGUIDLow(), count);
                    count = 0;
                }
                break;
            case 2: // Init Riding skill fix

                if (level < 20)
                {
                    break;
                }
                factory.InitSkills();
                sLog.outDetail("Bot %d hotfix (Riding Skill) applied",
                    bot->GetGUIDLow());
                break;

            default:
                break;
        }
    }
    SetValue(bot, "version", MANGOSBOT_VERSION);
    sLog.outDetail("Bot %d hotfix v%d applied",
        bot->GetGUIDLow(), MANGOSBOT_VERSION);
}

void RandomPlayerbotMgr::MirrorAh()
{
    sRandomPlayerbotMgr.m_ahActionMutex.lock();

    ahMirror.clear();

    std::vector<AuctionHouseType> houses = { (AuctionHouseType)0,(AuctionHouseType)1,(AuctionHouseType)2 };

    //Now loops over all houses. Can probably be faction specific later.
    for (auto house : houses)
    {
        uint32 houseId = 7;
        switch (house)
        {
            case (AuctionHouseType)0:
                houseId = 1;
                break;
            case (AuctionHouseType)1:
                houseId = 6;
                break;
            case (AuctionHouseType)2:
                houseId = 7;
                break;
            default:
                continue;
        }

        AuctionHouseEntry const* houseEntry = sAuctionHouseStore.LookupEntry(houseId);
        if (!houseEntry)
            continue;

        AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(houseEntry);
        if (!auctionHouse)
            continue;

        AuctionHouseObject::AuctionEntryMap const& map = *auctionHouse->GetAuctions();

        for (auto& auction : map)
        {
            if (!auction.second)
                continue;

            AuctionEntry auctionEntry = *auction.second;

            if (!auctionEntry.buyout)
                continue;

            if (!auctionEntry.itemCount)
                continue;

            ahMirror[auctionEntry.itemTemplate].push_back(auctionEntry);
        }
    }
    sRandomPlayerbotMgr.m_ahActionMutex.unlock();
}

typedef std::unordered_map <uint32, std::list<float>> botPerformanceMetric;
std::unordered_map<std::string, botPerformanceMetric> botPerformanceMetrics;

void RandomPlayerbotMgr::PushMetric(botPerformanceMetric& metric, const uint32 bot, const float value, uint32 maxNum) const
{
    metric[bot].push_back(value);

    if (metric[bot].size() > maxNum)
        metric[bot].pop_front();
}

float RandomPlayerbotMgr::GetMetricDelta(botPerformanceMetric& metric) const
{
    float deltaMetric = 0;
    for (auto& botMetric : metric)
    {
        std::list<float> values = botMetric.second;
        if (values.size() > 1)
            deltaMetric += (values.back() - values.front()) / values.size();
    }

    if (metric.empty())
        return 0;

    return deltaMetric / metric.size();
}

std::string RandomPlayerbotMgr::GetCommandTexts(const std::string& command)
{
    auto texts = GetCommandTexts();
    auto it = texts.find(command);
    if (it != texts.end())
        return it->second;
    return "";
}

std::unordered_map<std::string, std::string> RandomPlayerbotMgr::GetCommandTexts()
{
    return std::unordered_map<std::string, std::string>
    {
        {"init", "Randomize the first available bot.\nUsage: init"},
        {"upgrade", "Update gear and spells for all random bots.\nUsage: upgrade"},
        {"refresh", "Log out and log in all random bots to refresh their status.\nUsage: refresh"},
        {"teleport", "Teleport all random bots to a location suitable for their level.\nUsage: teleport"},
        {"rpg", "Teleport all random bots to a location for RPG activities.\nUsage: rpg"},
        {"revive", "Revive all dead random bots.\nUsage: revive"},
        {"grind", "Teleport all random bots to a grinding location.\nUsage: grind"},
        {"change_strategy", "Change the AI strategy for random bots.\nUsage: change_strategy <botname> <strategy>"},
        {"remove", "Remove a random bot from the server.\nUsage: remove <botname>"},
        {"reset", "Reset all random bots and clear event cache.\nUsage: reset"},
        {"diff", "Show server performance metrics.\nUsage: diff [player_diff] [empty_diff]"},
        {"stats", "Print bot statistics.\nUsage: stats"},
        {"update", "Trigger immediate bot AI update.\nUsage: update"},
        {"pid", "Adjust PID controller values.\nUsage: pid p i d"},
        {"clean map", "Unload and reload map files.\nUsage: clean map"},
        {"login debug", "Toggle login debug mode.\nUsage: login debug"},
        {"cmd", "Send command to a bot.\nUsage: cmd <botname> <command>"},
        {"help", "Show help for commands.\nUsage: help [command]"}
    };
}

std::list<std::string> RandomPlayerbotMgr::HandleHelp(std::string param)
{
    std::list<std::string> messages;
        
    if (param.empty())
    {
        messages.push_back("Type 'help commands for all available commands.");
        messages.push_back("Type 'help <command>' for more information on a specific command.");
        return messages;
    }

    if (param == "commands")
    {
        std::string commands = "Commands: ";
        for (auto& [command, help] : GetCommandTexts())
        {
            commands += command + ", ";
        }

        commands = commands.substr(0, commands.size() - 2);
        messages.push_back(commands);
        return messages;
    }
    
    
    std::string helpText = GetCommandTexts(param);
    if (!helpText.empty())
    {
        messages.push_back(helpText);
    }  
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomizeFirst(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomizeFirst(bot);
    messages.push_back("init applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleUpdateGearSpells(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    UpdateGearSpells(bot);
    messages.push_back("upgrade applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRefresh(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    Refresh(bot);
    messages.push_back("refresh applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomTeleportForLevel(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomTeleportForLevel(bot);
    messages.push_back("teleport applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomTeleportForRpg(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomTeleportForRpg(bot);
    messages.push_back("rpg applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRevive(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    Revive(bot);
    messages.push_back("revive applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomTeleport(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomTeleport(bot);
    messages.push_back("grind applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleChangeStrategy(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    ChangeStrategy(bot);
    messages.push_back("change_strategy applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRemove(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    Remove(bot);
    messages.push_back("remove applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleReset(std::string param)
{
    std::list<std::string> messages;
    CharacterDatabase.PExecute("delete from ai_playerbot_random_bots where event not in ('temporary')");
    sRandomPlayerbotMgr.eventCache.clear();
    std::string msg = "Random bots were reset for all players. Please restart the Server.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleStats(std::string param)
{
    if (!Qualified::isValidNumberString(param))
    {
        return {"Stats: Error parsing " + param};
    }

    std::list<std::string> messages;
    std::string msg = "Stats requested.";
    messages.push_back(msg);

    ObjectGuid guid = ObjectGuid(uint64(std::stoull(param)));
    activatePrintStatsThread(guid);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleReload(std::string param)
{
    std::list<std::string> messages;
    sPlayerbotAIConfig.Initialize();
    std::string msg = "Playerbot config reloaded.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleUpdate(std::string param)
{
    std::list<std::string> messages;
    sRandomPlayerbotMgr.UpdateAIInternal(0);
    std::string msg = "Playerbot update triggered.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsolePid(std::string param)
{
    std::list<std::string> messages;
    std::string pids = param.substr(4);
    std::vector<std::string> pid = Qualified::getMultiQualifiers(pids, " ");

    if (pid.size() == 0)
        pid.push_back("0");
    if (pid.size() == 1)
        pid.push_back("0");
    if (pid.size() == 2)
        pid.push_back("0");
    sRandomPlayerbotMgr.pid.adjust(stof(pid[0]), stof(pid[1]), stof(pid[2]));

    std::string msg = "Pid set to p:" + std::to_string(stof(pid[0])) + " i:" + std::to_string(stof(pid[1])) + " d:" + std::to_string(stof(pid[2]));
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleDiff(std::string param)
{
    std::list<std::string> messages;
    if (param.empty())
    {
        std::stringstream ss;
        ss << "Avg diff: " << sWorld.GetAverageDiff() << "\n";
        ss << "Max diff: " << sWorld.GetMaxDiff() << "\n";
        ss << "char db ping: " << sRandomPlayerbotMgr.GetDatabaseDelay("CharacterDatabase") << "\n";
        ss << "Sessions online: " << sWorld.GetActiveSessionCount() << "\n";
        ss << "Bots online: " << sRandomPlayerbotMgr.botCount << " (active: " << sRandomPlayerbotMgr.activeBots << ")";

        messages.push_back(ss.str());
        return messages;
    }
    else if (param.find(" ") != std::string::npos)
    {
        std::vector<std::string> diff = Qualified::getMultiQualifiers(param, " ");
        if (diff.size() == 0)
            diff.push_back("100");
        if (diff.size() == 1)
            diff.push_back(diff[0]);
        sPlayerbotAIConfig.diffWithPlayer = stoi(diff[0]);
        sPlayerbotAIConfig.diffEmpty = stoi(diff[1]);

        std::string msg = "Diff set to " + std::to_string(stoi(diff[0])) + " (player), " + std::to_string(stoi(diff[1])) + " (empty)";
        messages.push_back(msg);
        return messages;
    }
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleCleanMap(std::string param)
{
    std::list<std::string> messages;
    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        if (!sMapStore.LookupEntry(i))
            continue;

        uint32 mapId = sMapStore.LookupEntry(i)->MapID;
        boost::thread t([mapId]() {WorldPosition::unloadMapAndVMaps(mapId); });
        t.detach();
    }

    std::string msg = "Map cleaning initiated.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleLoginDebug(std::string param)
{
    std::list<std::string> messages;
    sPlayerBotLoginMgr.ToggleDebug();
    std::string msg = "Login debug toggled.";
    messages.push_back(msg);
    return messages;
}

uint32 RandomPlayerbotMgr::GetOrCreateAccount(Player* master, std::string& error)
{
    uint32 maxCharsPerAccount = 9;
#ifdef MANGOSBOT_TWO
    maxCharsPerAccount = 10;
#endif

    auto accountNrQr = LoginDatabase.PQuery("SELECT max(replace(lower(username), lower('%s'), '') + 1 - 1) maxAccountNr FROM account WHERE replace(lower(username), lower('%s'), '') != 0", sPlayerbotAIConfig.randomBotAccountPrefix.c_str(), sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

    if (!accountNrQr)
    {
        error = "Failed to find last " + sPlayerbotAIConfig.randomBotAccountPrefix + " account nr.";
        return 0;
    }

    Field* fields = accountNrQr->Fetch();
    // Solo-project fix: start at 0 (not randomBotAccountCount) so the lazy
    // creator reuses the same RNDBOT0..N pool that the startup loader
    // populates into randomBotAccounts. Upstream cmangos starts at
    // randomBotAccountCount, which puts manually-created bots at 200+
    // (outside the trusted-account list) and breaks `.rndbot add` for
    // non-admin users. The loop already skips occupied account numbers
    // (via `accountNumber++` at the bottom on full accounts), so starting
    // at 0 just walks forward to the first slot with room.
    uint32 accountNumber = 0;
    uint32 maxAccountNum = fields[0].GetUInt32();

    for (uint32 i = 0; i < 10000; i++)
    {
        std::ostringstream accountNameStr;
        accountNameStr << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        std::string accountName = accountNameStr.str();

        uint32 accountId = sAccountMgr.GetId(accountName);

        if (!accountId)
        {
            std::string password;
            if (sPlayerbotAIConfig.randomBotRandomPassword)
            {
                for (int i = 0; i < 10; i++)
                    password += (char)urand('!', 'z');
            }
            else
                password = accountName;

            LoginDatabase.BeginTransaction();
#ifndef MANGOSBOT_ZERO
            uint8 max_expansion = MAX_EXPANSION;
            AccountOpResult result = sAccountMgr.CreateAccount(accountName, password, max_expansion);
#else
            AccountOpResult result = sAccountMgr.CreateAccount(accountName, password);
#endif
            LoginDatabase.CommitTransactionDirect();

            if (result == AOR_OK)
            {
                uint32 accountId = sAccountMgr.GetId(accountName);
                if (accountId)
                {
                    sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);
                    return accountId;
                }
            }

            error = "Failed to create account";
            return 0;        
        }

        uint32 charCount = sAccountMgr.GetCharactersCount(accountId);

        if (charCount < maxCharsPerAccount)
        {
            if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
            {
                sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);
            }
            return accountId;
        }

        accountNumber++;
    }

    error = "Failed to find a suitable account.";
    return 0;
}

void RandomPlayerbotMgr::OnBotDeleted(uint32 botGuid, uint32 accountId)
{
    if (accountId > 0 && sPlayerbotAIConfig.IsInRandomAccountList(accountId))
    {
        uint32 maxCharsPerAccount = 9;
    #ifdef MANGOSBOT_TWO
        maxCharsPerAccount = 10;
    #endif
    
        if (sAccountMgr.GetCharactersCount(accountId) == 0)
        {
            std::ostringstream prefix;
            prefix << sPlayerbotAIConfig.randomBotAccountPrefix;
            size_t prefixLen = prefix.str().length();
            
            auto result = LoginDatabase.PQuery("SELECT username FROM account WHERE id = '%u'", accountId);
            if (result)
            {
                std::string username = result->Fetch()[0].GetString();
                if (username.substr(0, prefixLen) == prefix.str())
                {
                    uint32 accountNum = std::stoul(username.substr(prefixLen));
                    if (accountNum >= sPlayerbotAIConfig.randomBotAccountCount)
                    {
                        sAccountMgr.DeleteAccount(accountId);
                        sLog.outString("Deleted empty random bot account: %s (id: %u)", username.c_str(), accountId);
                    }
                }
            }
        }
    }
}
