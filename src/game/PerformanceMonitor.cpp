#include "PerformanceMonitor.h"
#include "Chat.h"
#include "World.h"
#include "MapManager.h"
#include "WorldSession.h"
#include "ObjectMgr.h"
#include "Config/Config.h"
#include "PerfStats.h"
#include "Util.h"
#include "Log.h"
#include <cerrno>
#include <cstdio>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

PerformanceMonitor sPerfMonitor;

namespace
{
bool ExistingFileHasContent(std::string const& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

std::string ArchiveTimestamp()
{
    time_t t = time(nullptr);
    tm* aTm = localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", aTm);
    return std::string(buf);
}

void EnsureDirectory(std::string const& path)
{
    if (!path.empty() && mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        sLog.outError("PerformanceMonitor: failed to create archive dir %s", path.c_str());
}

void ArchivePerfTraceBeforeTruncate(std::string const& logsDir, std::string const& path)
{
    if (!ExistingFileHasContent(path))
        return;

    std::string archiveDir = logsDir + "archive";
    EnsureDirectory(archiveDir);

    std::string base = archiveDir + "/server_perf_" + ArchiveTimestamp();
    std::string archivePath = base + ".csv";
    for (uint32 index = 1; ExistingFileHasContent(archivePath); ++index)
        archivePath = base + "_" + std::to_string(index) + ".csv";

    if (rename(path.c_str(), archivePath.c_str()) != 0)
        sLog.outError("PerformanceMonitor: failed to archive %s to %s", path.c_str(), archivePath.c_str());
}
}

PerformanceMonitor::PerformanceMonitor()
{
	gPerfMonitorInterface = this;
}

void PerformanceMonitor::Initialize()
{
	// in seconds
	uint32 IntervalReportValue = sWorld.getConfig(CONFIG_UINT32_PERFORMANCE_REPORT_INTERVAL);

	// convert to milliseconds
	IntervalReport.SetInterval(IntervalReportValue * 1000);
	IntervalReport.SetCurrent(0);

    m_perfTraceFile = OpenPerfTraceFile();
    ResetWorldPhaseAggregate();
}

void PerformanceMonitor::FrameStart()
{
	if (!g_bEnableStatGather)
	{
		return;
	}
	Tick.FrameStart();
	WorldSleep.FrameStart();
	WorldTick.FrameStart();
	TEST0.FrameStart();
	UpdateSession.FrameStart();
	MapManager.FrameStart();

	const MapManager::MapMapType& Maps = sMapMgr.Maps();
	for (MapManager::MapMapType::const_iterator iter = Maps.cbegin(); iter != Maps.cend(); iter++)
	{
		iter->second->MovementPerfTimer.FrameStart();
		iter->second->SpellPerfTimer.FrameStart();
		iter->second->UpdateTimer.FrameStart();
        iter->second->DynamicTreePerfTimer.FrameStart();
        iter->second->SessionPerfTimer.FrameStart();
        iter->second->PlayersPerfTimer.FrameStart();
        iter->second->CellsPerfTimer.FrameStart();
        iter->second->ObjectUpdatesPerfTimer.FrameStart();
        iter->second->RelocationPerfTimer.FrameStart();
        iter->second->PlayersPostVisibilityPerfTimer.FrameStart();
        iter->second->CorpsePerfTimer.FrameStart();
        iter->second->GridStatePerfTimer.FrameStart();
        iter->second->ScriptsPerfTimer.FrameStart();
        iter->second->InstanceDataPerfTimer.FrameStart();
        iter->second->WeatherPerfTimer.FrameStart();
        iter->second->WaitPerfTimer.FrameStart();
	}
}

void PerformanceMonitor::FrameEnd(uint32 delta)
{
	if (!g_bEnableStatGather)
	{
		return;
	}
	WorldSleep.FrameEnd();
	Tick.FrameEnd();
	WorldTick.FrameEnd();
	TEST0.FrameEnd();
	UpdateSession.FrameEnd();
	MapManager.FrameEnd();

	const MapManager::MapMapType& Maps = sMapMgr.Maps();
	for (MapManager::MapMapType::const_iterator iter = Maps.cbegin(); iter != Maps.cend(); iter++)
	{
		Map* CurrentMap = iter->second;
		CurrentMap->MovementPerfTimer.FrameEnd();
		CurrentMap->SpellPerfTimer.FrameEnd();
		CurrentMap->UpdateTimer.FrameEnd();
        CurrentMap->DynamicTreePerfTimer.FrameEnd();
        CurrentMap->SessionPerfTimer.FrameEnd();
        CurrentMap->PlayersPerfTimer.FrameEnd();
        CurrentMap->CellsPerfTimer.FrameEnd();
        CurrentMap->ObjectUpdatesPerfTimer.FrameEnd();
        CurrentMap->RelocationPerfTimer.FrameEnd();
        CurrentMap->PlayersPostVisibilityPerfTimer.FrameEnd();
        CurrentMap->CorpsePerfTimer.FrameEnd();
        CurrentMap->GridStatePerfTimer.FrameEnd();
        CurrentMap->ScriptsPerfTimer.FrameEnd();
        CurrentMap->InstanceDataPerfTimer.FrameEnd();
        CurrentMap->WeatherPerfTimer.FrameEnd();
        CurrentMap->WaitPerfTimer.FrameEnd();
	}

	QPC_Counter = CPU::qpc_counter;
	CPU::qpc_counter = 0;

	IntervalReport.Update(delta);
	if (IntervalReport.Passed())
	{
		IntervalReport.Reset();
		ReportPerformanceToDB();
	}
}

void PerformanceMonitor::SetReportInterval(uint32 IntervalInSeconds)
{
	IntervalReport.SetInterval(IntervalInSeconds * 1000);
}

void PerformanceMonitor::RecordWorldPhaseSnapshot(WorldPhaseSnapshot const& snapshot)
{
    m_worldPhaseAggregate.samples++;
    m_worldPhaseAggregate.sessionsUpdateMs += snapshot.sessionsUpdateMs;
    m_worldPhaseAggregate.transportUpdateMs += snapshot.transportUpdateMs;
    m_worldPhaseAggregate.mapMgrUpdateMs += snapshot.mapMgrUpdateMs;
    m_worldPhaseAggregate.battlegroundUpdateMs += snapshot.battlegroundUpdateMs;
    m_worldPhaseAggregate.lfgUpdateMs += snapshot.lfgUpdateMs;
    m_worldPhaseAggregate.guardUpdateMs += snapshot.guardUpdateMs;
    m_worldPhaseAggregate.zoneScriptUpdateMs += snapshot.zoneScriptUpdateMs;
    m_worldPhaseAggregate.dynamicVisUpdateMs += snapshot.dynamicVisUpdateMs;
    m_worldPhaseAggregate.playerbotUpdateMs += snapshot.playerbotUpdateMs;
    m_worldPhaseAggregate.asyncQueriesMs += snapshot.asyncQueriesMs;
    m_worldPhaseAggregate.worldUpdateMs += snapshot.worldUpdateMs;
    m_worldPhaseAggregate.maxWorldUpdateMs = std::max(m_worldPhaseAggregate.maxWorldUpdateMs, snapshot.worldUpdateMs);
    m_worldPhaseAggregate.maxSessionsUpdateMs = std::max(m_worldPhaseAggregate.maxSessionsUpdateMs, snapshot.sessionsUpdateMs);
    m_worldPhaseAggregate.maxMapMgrUpdateMs = std::max(m_worldPhaseAggregate.maxMapMgrUpdateMs, snapshot.mapMgrUpdateMs);
    m_worldPhaseAggregate.maxPlayerbotUpdateMs = std::max(m_worldPhaseAggregate.maxPlayerbotUpdateMs, snapshot.playerbotUpdateMs);
    m_worldPhaseAggregate.maxAsyncQueriesMs = std::max(m_worldPhaseAggregate.maxAsyncQueriesMs, snapshot.asyncQueriesMs);
    m_worldPhaseAggregate.maxQueuedSessions = std::max(m_worldPhaseAggregate.maxQueuedSessions, snapshot.queuedSessions);
    m_worldPhaseAggregate.maxActiveSessions = std::max(m_worldPhaseAggregate.maxActiveSessions, snapshot.activeSessions);
    m_worldPhaseAggregate.maxAverageDiffMs = std::max(m_worldPhaseAggregate.maxAverageDiffMs, snapshot.averageDiffMs);
    m_worldPhaseAggregate.maxCurrentDiffMs = std::max(m_worldPhaseAggregate.maxCurrentDiffMs, snapshot.currentDiffMs);
    m_worldPhaseAggregate.last = snapshot;
    PerfStats::UpdateBotPressure(snapshot.worldUpdateMs, snapshot.mapMgrUpdateMs, snapshot.averageDiffMs, snapshot.currentDiffMs);
}

void PerformanceMonitor::ReportCPU(ChatHandler& Handler)
{
	/// #### CPU ####
	Handler.PSendSysMessage("CPU Performance report");
	Handler.PSendSysMessage("QPC counter: %u", QPC_Counter);

	auto GetPercentOfLambda = [](XStatTimer Local, XStatTimer Global) -> float
		{
			return 100.0f * float(Local.result) / float(Global.result);
		};

	auto ReportStatWithParentLambda = [&Handler, &GetPercentOfLambda](const char* Name, const XStatTimer& Timer, const XStatTimer& Parent, int32 Level = 1)
		{
			char BeginShit[256];
			if (Level == 0)
			{
				memset(BeginShit, 0, sizeof(BeginShit));
			}
			else
			{
				int32 Cursor = 0;
				for (int32 i = 0; i < Level; i++)
				{
					BeginShit[Cursor++] = '-';
					BeginShit[Cursor++] = '>';
					BeginShit[Cursor++] = ' ';
				}
				BeginShit[Cursor] = 0;
			}
			Handler.PSendSysMessage(" %s %s: [%u] %2.2fms, %2.1f%%, min: %2.2fms, max: %2.2fms", 
				BeginShit, Name, Timer.count, Timer.result, GetPercentOfLambda(Timer, Parent), Timer.MinResult, Timer.MaxResult);
		};

	Handler.PSendSysMessage("Tick: %2.2f", Tick.result);
	Handler.PSendSysMessage("WorldSleep: %2.2f", WorldSleep.result);

	ReportStatWithParentLambda("TEST0", TEST0, Tick);
	ReportStatWithParentLambda("WorldTick", WorldTick, Tick);
	ReportStatWithParentLambda("UpdateSession", UpdateSession, WorldTick, 2);
	ReportStatWithParentLambda("MapManager", MapManager, WorldTick, 2);

	/// #### Maps ####
	Handler.PSendSysMessage("Map performance report. Some maps update async");

	const MapManager::MapMapType& Maps = sMapMgr.Maps();
	for (MapManager::MapMapType::const_iterator iter = Maps.cbegin(); iter != Maps.cend(); iter++)
	{
		Map* CurrentMap = iter->second;
		Handler.PSendSysMessage("Map: %u, InstanceID: %u", CurrentMap->GetId(), CurrentMap->GetInstanceId());
		ReportStatWithParentLambda("Update", CurrentMap->UpdateTimer, MapManager);
		ReportStatWithParentLambda("Movement", CurrentMap->MovementPerfTimer, CurrentMap->UpdateTimer, 2);
		ReportStatWithParentLambda("Spell", CurrentMap->SpellPerfTimer, CurrentMap->UpdateTimer, 2);
	}
}

void PerformanceMonitor::ReportMemory(ChatHandler& Handler)
{
	Handler.PSendSysMessage("Memory Performance report");
	int64 ProcessMemory = (int64)Memory::GetProcessMemory(); // fix later
	int64 AllTrackedBytes = 0;
	std::lock_guard guard{ MemBytesGuard };

	std::unordered_map<std::string, int64> FixedMap;

	for (auto& [key, value] : MemBytes)
	{
		FixedMap[key] += value;
		AllTrackedBytes += value;
	}

	auto GetValueAsMbLambda = [](int64 InValue) -> double
		{
			double fConvertValue = InValue;
			fConvertValue /= 1024.0; // to kbytes
			fConvertValue /= 1024.0; // to mbytes
			return fConvertValue;
		};

	double fProcessMemory = GetValueAsMbLambda(ProcessMemory);
	double fTrackedMemory = GetValueAsMbLambda(AllTrackedBytes);
	Handler.PSendSysMessage("All process memory: %.2fMb (Tracked: %.2fMb)", fProcessMemory, fTrackedMemory);

	for (auto& [key, value] : FixedMap)
	{
		Handler.PSendSysMessage("-> %s - %.2fMb", key.c_str(), GetValueAsMbLambda(value));
	}
}

void PerformanceMonitor::ReportPerformanceToDB()
{
    if (!m_perfTraceFile)
        m_perfTraceFile = OpenPerfTraceFile();

    const uint64 sampleCount = std::max<uint64>(1, m_worldPhaseAggregate.samples);
    const World::SessionMap& sessions = sWorld.GetAllSessions();

    uint32 onlineSessions = 0;
    uint32 connectedSessions = 0;
    uint64 sessionLatencyTotal = 0;
    uint32 sessionLatencyMax = 0;
    uint64 totalQueuedPackets = 0;
    uint32 maxSessionQueuedPackets = 0;
    uint32 queuedPacketSessions = 0;

    for (auto const& [accountId, session] : sessions)
    {
        if (!session)
            continue;

        ++onlineSessions;
        if (session->IsConnected())
            ++connectedSessions;

        const uint32 latency = session->GetLatency();
        sessionLatencyTotal += latency;
        sessionLatencyMax = std::max(sessionLatencyMax, latency);

        const uint32 queuedPackets = static_cast<uint32>(session->GetTotalQueuedPacketCount());
        totalQueuedPackets += queuedPackets;
        maxSessionQueuedPackets = std::max(maxSessionQueuedPackets, queuedPackets);
        if (queuedPackets)
            ++queuedPacketSessions;
    }

    uint32 busiestMapId = 0;
    uint32 busiestMapInstanceId = 0;
    uint32 busiestMapPlayers = 0;
    double busiestMapUpdateMs = 0.0;
    double busiestMovementMs = 0.0;
    double busiestSpellMs = 0.0;
    double busiestDynamicTreeMs = 0.0;
    double busiestSessionMs = 0.0;
    double busiestPlayersMs = 0.0;
    double busiestCellsMs = 0.0;
    double busiestObjectUpdatesMs = 0.0;
    double busiestRelocationMs = 0.0;
    double busiestPlayersPostVisibilityMs = 0.0;
    double busiestCorpseMs = 0.0;
    double busiestGridStateMs = 0.0;
    double busiestScriptsMs = 0.0;
    double busiestInstanceDataMs = 0.0;
    double busiestWeatherMs = 0.0;
    double busiestWaitMs = 0.0;
    uint32 totalLoadedGrids = 0;
    uint32 map0LoadedGrids = 0;
    uint32 map1LoadedGrids = 0;
    uint32 gridsActiveTotal = 0;
    uint32 gridsIdleTotal = 0;
    uint32 gridsRemovalTotal = 0;
    uint32 gridsUnloadLockedTotal = 0;
    uint32 gridsUnloadExplicitLockedTotal = 0;
    uint32 gridsUnloadActiveLockedTotal = 0;
    uint32 gridsUnloadActiveLockRefsTotal = 0;
    uint32 gridsWithActiveObjectsTotal = 0;
    uint32 gridsNearPlayersTotal = 0;
    uint32 gridsNearActiveNonPlayersTotal = 0;
    uint32 gridsNearAnyTotal = 0;
    uint32 gridsUnloadLockedMap0 = 0;
    uint32 gridsUnloadLockedMap1 = 0;
    uint32 gridsUnloadExplicitLockedMap0 = 0;
    uint32 gridsUnloadExplicitLockedMap1 = 0;
    uint32 gridsUnloadActiveLockedMap0 = 0;
    uint32 gridsUnloadActiveLockedMap1 = 0;
    uint32 gridsUnloadActiveLockRefsMap0 = 0;
    uint32 gridsUnloadActiveLockRefsMap1 = 0;
    uint32 gridsNearAnyMap0 = 0;
    uint32 gridsNearAnyMap1 = 0;
    uint32 map0Players = 0;
    uint32 map1Players = 0;
    uint32 map0ActiveNonPlayers = 0;
    uint32 map1ActiveNonPlayers = 0;
    uint32 pendingObjectUpdatesTotal = 0;
    uint32 pendingRelocationsTotal = 0;
    uint32 pendingObjectUpdatesMap0 = 0;
    uint32 pendingObjectUpdatesMap1 = 0;
    uint32 pendingRelocationsMap0 = 0;
    uint32 pendingRelocationsMap1 = 0;
    uint32 maxPendingObjectUpdatesMapId = 0;
    uint32 maxPendingObjectUpdatesMapInstanceId = 0;
    uint32 maxPendingObjectUpdates = 0;
    uint32 maxPendingRelocationsMapId = 0;
    uint32 maxPendingRelocationsMapInstanceId = 0;
    uint32 maxPendingRelocations = 0;
    const uint64 processMemoryBytes = Memory::GetProcessMemory();

    const MapManager::MapMapType& maps = sMapMgr.Maps();
    for (auto const& [key, map] : maps)
    {
        if (!map)
            continue;

        uint32 loadedGrids = 0;
        for (auto itr = map->begin(); itr != map->end(); ++itr)
            ++loadedGrids;

        const Map::GridUnloadDiagnostics gridDiagnostics = map->CollectGridUnloadDiagnostics();
        const uint32 pendingObjectUpdates = map->GetPendingObjectUpdateCount();
        const uint32 pendingRelocations = map->GetPendingRelocationCount();

        totalLoadedGrids += loadedGrids;
        gridsActiveTotal += gridDiagnostics.activeStateGrids;
        gridsIdleTotal += gridDiagnostics.idleStateGrids;
        gridsRemovalTotal += gridDiagnostics.removalStateGrids;
        gridsUnloadLockedTotal += gridDiagnostics.unloadLockedGrids;
        gridsUnloadExplicitLockedTotal += gridDiagnostics.unloadExplicitLockedGrids;
        gridsUnloadActiveLockedTotal += gridDiagnostics.unloadActiveLockedGrids;
        gridsUnloadActiveLockRefsTotal += gridDiagnostics.unloadActiveLockRefs;
        gridsWithActiveObjectsTotal += gridDiagnostics.activeObjectsInGrid;
        gridsNearPlayersTotal += gridDiagnostics.nearPlayers;
        gridsNearActiveNonPlayersTotal += gridDiagnostics.nearActiveNonPlayers;
        gridsNearAnyTotal += gridDiagnostics.nearAny;
        pendingObjectUpdatesTotal += pendingObjectUpdates;
        pendingRelocationsTotal += pendingRelocations;

        if (map->GetId() == 0)
        {
            map0LoadedGrids += loadedGrids;
            map0Players += map->GetPlayers().getSize();
            gridsUnloadLockedMap0 += gridDiagnostics.unloadLockedGrids;
            gridsUnloadExplicitLockedMap0 += gridDiagnostics.unloadExplicitLockedGrids;
            gridsUnloadActiveLockedMap0 += gridDiagnostics.unloadActiveLockedGrids;
            gridsUnloadActiveLockRefsMap0 += gridDiagnostics.unloadActiveLockRefs;
            gridsNearAnyMap0 += gridDiagnostics.nearAny;
            pendingObjectUpdatesMap0 += pendingObjectUpdates;
            pendingRelocationsMap0 += pendingRelocations;
        }
        else if (map->GetId() == 1)
        {
            map1LoadedGrids += loadedGrids;
            map1Players += map->GetPlayers().getSize();
            gridsUnloadLockedMap1 += gridDiagnostics.unloadLockedGrids;
            gridsUnloadExplicitLockedMap1 += gridDiagnostics.unloadExplicitLockedGrids;
            gridsUnloadActiveLockedMap1 += gridDiagnostics.unloadActiveLockedGrids;
            gridsUnloadActiveLockRefsMap1 += gridDiagnostics.unloadActiveLockRefs;
            gridsNearAnyMap1 += gridDiagnostics.nearAny;
            pendingObjectUpdatesMap1 += pendingObjectUpdates;
            pendingRelocationsMap1 += pendingRelocations;
        }

        if (pendingObjectUpdates > maxPendingObjectUpdates)
        {
            maxPendingObjectUpdates = pendingObjectUpdates;
            maxPendingObjectUpdatesMapId = map->GetId();
            maxPendingObjectUpdatesMapInstanceId = map->GetInstanceId();
        }

        if (pendingRelocations > maxPendingRelocations)
        {
            maxPendingRelocations = pendingRelocations;
            maxPendingRelocationsMapId = map->GetId();
            maxPendingRelocationsMapInstanceId = map->GetInstanceId();
        }

        const double updateMs = map->UpdateTimer.result;
        if (updateMs > busiestMapUpdateMs)
        {
            busiestMapUpdateMs = updateMs;
            busiestMapId = map->GetId();
            busiestMapInstanceId = map->GetInstanceId();
            busiestMapPlayers = map->GetPlayers().getSize();
            busiestMovementMs = map->MovementPerfTimer.result;
            busiestSpellMs = map->SpellPerfTimer.result;
            busiestDynamicTreeMs = map->DynamicTreePerfTimer.result;
            busiestSessionMs = map->SessionPerfTimer.result;
            busiestPlayersMs = map->PlayersPerfTimer.result;
            busiestCellsMs = map->CellsPerfTimer.result;
            busiestObjectUpdatesMs = map->ObjectUpdatesPerfTimer.result;
            busiestRelocationMs = map->RelocationPerfTimer.result;
            busiestPlayersPostVisibilityMs = map->PlayersPostVisibilityPerfTimer.result;
            busiestCorpseMs = map->CorpsePerfTimer.result;
            busiestGridStateMs = map->GridStatePerfTimer.result;
            busiestScriptsMs = map->ScriptsPerfTimer.result;
            busiestInstanceDataMs = map->InstanceDataPerfTimer.result;
            busiestWeatherMs = map->WeatherPerfTimer.result;
            busiestWaitMs = map->WaitPerfTimer.result;
        }

        if (map->GetId() == 0)
            map0ActiveNonPlayers += map->GetActiveNonPlayersCount();
        else if (map->GetId() == 1)
            map1ActiveNonPlayers += map->GetActiveNonPlayersCount();
    }

    const uint32 avgLatency = onlineSessions ? static_cast<uint32>(sessionLatencyTotal / onlineSessions) : 0;

    if (m_perfTraceFile)
    {
        const WorldPhaseSnapshot& last = m_worldPhaseAggregate.last;
        std::ostringstream line;
        line << sWorld.GetGameTime() << ','
             << sWorld.GetUptime() << ','
             << sampleCount << ','
             << Tick.result << ','
             << WorldSleep.result << ','
             << WorldTick.result << ','
             << UpdateSession.result << ','
             << MapManager.result << ','
             << (double)m_worldPhaseAggregate.worldUpdateMs / sampleCount << ','
             << m_worldPhaseAggregate.maxWorldUpdateMs << ','
             << (double)m_worldPhaseAggregate.sessionsUpdateMs / sampleCount << ','
             << m_worldPhaseAggregate.maxSessionsUpdateMs << ','
             << (double)m_worldPhaseAggregate.mapMgrUpdateMs / sampleCount << ','
             << m_worldPhaseAggregate.maxMapMgrUpdateMs << ','
             << (double)m_worldPhaseAggregate.playerbotUpdateMs / sampleCount << ','
             << m_worldPhaseAggregate.maxPlayerbotUpdateMs << ','
             << (double)m_worldPhaseAggregate.asyncQueriesMs / sampleCount << ','
             << m_worldPhaseAggregate.maxAsyncQueriesMs << ','
             << last.transportUpdateMs << ','
             << last.battlegroundUpdateMs << ','
             << last.lfgUpdateMs << ','
             << last.guardUpdateMs << ','
             << last.zoneScriptUpdateMs << ','
             << last.dynamicVisUpdateMs << ','
             << onlineSessions << ','
             << connectedSessions << ','
             << sWorld.GetQueuedSessionCount() << ','
             << avgLatency << ','
             << sessionLatencyMax << ','
             << totalQueuedPackets << ','
             << maxSessionQueuedPackets << ','
             << queuedPacketSessions << ','
             << PerfStats::g_totalPlayers << ','
             << PerfStats::g_totalCreatures << ','
             << PerfStats::g_totalUnits << ','
             << PerfStats::g_totalGameObjects << ','
             << PerfStats::g_totalCorpses << ','
             << PerfStats::g_totalMaps << ','
             << processMemoryBytes << ','
             << pendingObjectUpdatesTotal << ','
             << pendingRelocationsTotal << ','
             << pendingObjectUpdatesMap0 << ','
             << pendingObjectUpdatesMap1 << ','
             << pendingRelocationsMap0 << ','
             << pendingRelocationsMap1 << ','
             << maxPendingObjectUpdatesMapId << ','
             << maxPendingObjectUpdatesMapInstanceId << ','
             << maxPendingObjectUpdates << ','
             << maxPendingRelocationsMapId << ','
             << maxPendingRelocationsMapInstanceId << ','
             << maxPendingRelocations << ','
             << totalLoadedGrids << ','
             << map0LoadedGrids << ','
             << map1LoadedGrids << ','
             << gridsActiveTotal << ','
             << gridsIdleTotal << ','
             << gridsRemovalTotal << ','
             << gridsUnloadLockedTotal << ','
             << gridsUnloadExplicitLockedTotal << ','
             << gridsUnloadActiveLockedTotal << ','
             << gridsUnloadActiveLockRefsTotal << ','
             << gridsWithActiveObjectsTotal << ','
             << gridsNearPlayersTotal << ','
             << gridsNearActiveNonPlayersTotal << ','
             << gridsNearAnyTotal << ','
             << gridsUnloadLockedMap0 << ','
             << gridsUnloadLockedMap1 << ','
             << gridsUnloadExplicitLockedMap0 << ','
             << gridsUnloadExplicitLockedMap1 << ','
             << gridsUnloadActiveLockedMap0 << ','
             << gridsUnloadActiveLockedMap1 << ','
             << gridsUnloadActiveLockRefsMap0 << ','
             << gridsUnloadActiveLockRefsMap1 << ','
             << gridsNearAnyMap0 << ','
             << gridsNearAnyMap1 << ','
             << map0Players << ','
             << map1Players << ','
             << map0ActiveNonPlayers << ','
             << map1ActiveNonPlayers << ','
             << PerfStats::g_slowestMapId << ','
             << PerfStats::g_slowestMapUpdateTime << ','
             << PerfStats::g_slowestMapInstanceId << ','
             << PerfStats::g_slowestMapPlayers << ','
             << PerfStats::g_slowestMapActiveNonPlayers << ','
             << PerfStats::g_slowestMapPendingObjectUpdates << ','
             << PerfStats::g_slowestMapPendingRelocations << ','
             << PerfStats::g_slowestMapWaitIterations << ','
             << PerfStats::g_slowestMapSessionsMs << ','
             << PerfStats::g_slowestMapPlayersMs << ','
             << PerfStats::g_slowestMapCellsMs << ','
             << PerfStats::g_slowestMapObjectsMs << ','
             << PerfStats::g_slowestMapRelocationMs << ','
             << PerfStats::g_slowestMapPlayers2Ms << ','
             << PerfStats::g_slowestMapWaitMs << ','
             << PerfStats::g_slowestMapWorkId << ','
             << PerfStats::g_slowestMapWorkTime << ','
             << PerfStats::g_slowestMapWorkInstanceId << ','
             << PerfStats::g_slowestMapWorkPlayers << ','
             << PerfStats::g_slowestMapWorkActiveNonPlayers << ','
             << PerfStats::g_slowestMapWorkPendingObjectUpdates << ','
             << PerfStats::g_slowestMapWorkPendingRelocations << ','
             << PerfStats::g_slowestMapWorkWaitIterations << ','
             << PerfStats::g_slowestMapWorkSessionsMs << ','
             << PerfStats::g_slowestMapWorkPlayersMs << ','
             << PerfStats::g_slowestMapWorkCellsMs << ','
             << PerfStats::g_slowestMapWorkObjectsMs << ','
             << PerfStats::g_slowestMapWorkRelocationMs << ','
             << PerfStats::g_slowestMapWorkPlayers2Ms << ','
             << PerfStats::g_slowestMapWorkWaitMs << ','
             << busiestMapId << ','
             << busiestMapInstanceId << ','
             << busiestMapPlayers << ','
             << busiestMapUpdateMs << ','
             << busiestMovementMs << ','
             << busiestSpellMs << ','
             << busiestDynamicTreeMs << ','
             << busiestSessionMs << ','
             << busiestPlayersMs << ','
             << busiestCellsMs << ','
             << busiestObjectUpdatesMs << ','
             << busiestRelocationMs << ','
             << busiestPlayersPostVisibilityMs << ','
             << busiestCorpseMs << ','
             << busiestGridStateMs << ','
             << busiestScriptsMs << ','
             << busiestInstanceDataMs << ','
             << busiestWeatherMs << ','
             << busiestWaitMs << ','
             << PerfStats::g_updatePlayersCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersProcessed.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersSkipped.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersBotProcessed.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersBotSkipped.load(std::memory_order_relaxed) << ','
             << PerfStats::g_updatePlayersWaitBotOnlySkipped.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerUpdateCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerUpdateMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerUpdateMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerUpdateBotCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerUpdateBotMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerUpdateBotMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotHookCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotHookMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotHookMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotHookSuppressedCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotHookSuppressedElapsedMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiVisibleCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiVisibleMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiVisibleMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiBackgroundCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiBackgroundMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotAiBackgroundMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotMgrCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotMgrMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotMgrMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotPlannerChecks.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotPlannerAllowed.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotPlannerDeferred.load(std::memory_order_relaxed) << ','
             << PerfStats::g_playerbotPlannerFastLane.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureLevel.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureScore.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureUpdates.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureDeferredQuest.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureDeferredVisibleObjective.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureDeferredForceChase.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureDeferredStaleRecover.load(std::memory_order_relaxed) << ','
             << PerfStats::g_botPressureDeferredPath.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestRequests.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestRequestMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestRequestMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestFetches.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestStrictPartitionCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestSoftPartitionCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestBroadPartitionCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestAsyncJobs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestAsyncMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestAsyncMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestAsyncEmptyJobs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestAsyncResultPoints.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelPartitionsCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelPartitionsMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelPartitionsMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelPartitionsDestinations.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelPartitionsResultPoints.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelPartitionsEmptyCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestPartitionCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_travelQuestPartitionMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningFlushCalls.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningFlushMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningFlushMaxMs.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningTaskRowsFlushed.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningCombatRowsFlushed.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningTaskRowsDropped.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningCombatRowsDropped.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningTaskQueueSize.load(std::memory_order_relaxed) << ','
             << PerfStats::g_learningCombatQueueSize.load(std::memory_order_relaxed) << ','
             << last.averageDiffMs << ','
             << last.currentDiffMs << ','
             << QPC_Counter
             << '\n';

        fputs(line.str().c_str(), m_perfTraceFile);
        fflush(m_perfTraceFile);
    }

    PerfStats::g_slowestMapId = 0;
    PerfStats::g_slowestMapUpdateTime = 0;
    PerfStats::g_slowestMapInstanceId = 0;
    PerfStats::g_slowestMapPlayers = 0;
    PerfStats::g_slowestMapActiveNonPlayers = 0;
    PerfStats::g_slowestMapPendingObjectUpdates = 0;
    PerfStats::g_slowestMapPendingRelocations = 0;
    PerfStats::g_slowestMapWaitIterations = 0;
    PerfStats::g_slowestMapSessionsMs = 0;
    PerfStats::g_slowestMapPlayersMs = 0;
    PerfStats::g_slowestMapCellsMs = 0;
    PerfStats::g_slowestMapObjectsMs = 0;
    PerfStats::g_slowestMapRelocationMs = 0;
    PerfStats::g_slowestMapPlayers2Ms = 0;
    PerfStats::g_slowestMapWaitMs = 0;
    PerfStats::g_slowestMapWorkId = 0;
    PerfStats::g_slowestMapWorkTime = 0;
    PerfStats::g_slowestMapWorkInstanceId = 0;
    PerfStats::g_slowestMapWorkPlayers = 0;
    PerfStats::g_slowestMapWorkActiveNonPlayers = 0;
    PerfStats::g_slowestMapWorkPendingObjectUpdates = 0;
    PerfStats::g_slowestMapWorkPendingRelocations = 0;
    PerfStats::g_slowestMapWorkWaitIterations = 0;
    PerfStats::g_slowestMapWorkSessionsMs = 0;
    PerfStats::g_slowestMapWorkPlayersMs = 0;
    PerfStats::g_slowestMapWorkCellsMs = 0;
    PerfStats::g_slowestMapWorkObjectsMs = 0;
    PerfStats::g_slowestMapWorkRelocationMs = 0;
    PerfStats::g_slowestMapWorkPlayers2Ms = 0;
    PerfStats::g_slowestMapWorkWaitMs = 0;
    PerfStats::ResetPlayerUpdateStats();
    PerfStats::ResetTravelStats();
    PerfStats::ResetLearningTelemetryStats();

    ResetWorldPhaseAggregate();
}

void PerformanceMonitor::ReportAlloc(const char* Category, size_t Bytes)
{
	std::lock_guard guard{ MemBytesGuard };
	MemBytes[Category] += Bytes;
}

void PerformanceMonitor::ReportDealloc(const char* Category, size_t Bytes)
{
	std::lock_guard guard{ MemBytesGuard };
	MemBytes[Category] -= Bytes;
}

void PerformanceMonitor::ResetWorldPhaseAggregate()
{
    m_worldPhaseAggregate = WorldPhaseAggregate();
}

FILE* PerformanceMonitor::OpenPerfTraceFile()
{
    std::string logsDir = sConfig.GetStringDefault("LogsDir", "");
    if (!logsDir.empty())
    {
        if (logsDir.back() != '/' && logsDir.back() != '\\')
            logsDir.push_back('/');
    }

    const std::string path = logsDir + "server_perf.csv";
    ArchivePerfTraceBeforeTruncate(logsDir, path);

    FILE* file = fopen(path.c_str(), "w");
    if (!file)
        return nullptr;

    static const char* header =
        "game_time,uptime_sec,samples,tick_ms,world_sleep_ms,world_tick_ms,update_session_ms,map_manager_ms,"
        "world_update_avg_ms,world_update_max_ms,sessions_avg_ms,sessions_max_ms,mapmgr_avg_ms,mapmgr_max_ms,"
        "playerbots_avg_ms,playerbots_max_ms,async_avg_ms,async_max_ms,transport_last_ms,bg_last_ms,lfg_last_ms,"
        "guard_last_ms,zone_last_ms,dynvis_last_ms,online_sessions,connected_sessions,queued_sessions,avg_latency_ms,"
        "max_latency_ms,total_queued_packets,max_session_queued_packets,queued_packet_sessions,total_players,"
        "total_creatures,total_units,total_gameobjects,total_corpses,total_maps,process_memory_bytes,"
        "pending_obj_updates_total,pending_relocations_total,pending_obj_updates_map0,pending_obj_updates_map1,"
        "pending_relocations_map0,pending_relocations_map1,max_pending_obj_map_id,max_pending_obj_map_instance_id,"
        "max_pending_obj_count,max_pending_reloc_map_id,max_pending_reloc_map_instance_id,max_pending_reloc_count,"
        "loaded_grids_total,loaded_grids_map0,loaded_grids_map1,grids_active_total,grids_idle_total,"
        "grids_removal_total,grids_unload_locked_total,grids_unload_explicit_locked_total,"
        "grids_unload_active_locked_total,grids_unload_active_lock_refs_total,grids_with_active_objects_total,"
        "grids_near_players_total,grids_near_active_non_players_total,grids_near_any_total,"
        "grids_unload_locked_map0,grids_unload_locked_map1,grids_unload_explicit_locked_map0,"
        "grids_unload_explicit_locked_map1,grids_unload_active_locked_map0,grids_unload_active_locked_map1,"
        "grids_unload_active_lock_refs_map0,grids_unload_active_lock_refs_map1,grids_near_any_map0,"
        "grids_near_any_map1,map0_players,map1_players,"
        "map0_active_non_players,map1_active_non_players,slowest_map_id,slowest_map_ms,"
        "slowest_map_instance_id,slowest_map_players,slowest_map_active_non_players,"
        "slowest_map_pending_obj_updates,slowest_map_pending_relocations,slowest_map_wait_iterations,"
        "slowest_map_sessions_ms,slowest_map_players_ms,slowest_map_cells_ms,slowest_map_obj_updates_ms,"
        "slowest_map_relocation_ms,slowest_map_players_post_vis_ms,slowest_map_wait_ms,"
        "slowest_work_map_id,slowest_work_map_ms,slowest_work_map_instance_id,slowest_work_map_players,"
        "slowest_work_map_active_non_players,slowest_work_map_pending_obj_updates,"
        "slowest_work_map_pending_relocations,slowest_work_map_wait_iterations,"
        "slowest_work_map_sessions_ms,slowest_work_map_players_ms,slowest_work_map_cells_ms,"
        "slowest_work_map_obj_updates_ms,slowest_work_map_relocation_ms,"
        "slowest_work_map_players_post_vis_ms,slowest_work_map_wait_ms,"
        "busiest_map_id,busiest_map_instance_id,busiest_map_players,busiest_map_update_ms,busiest_map_movement_ms,"
        "busiest_map_spell_ms,busiest_map_dyn_tree_ms,busiest_map_sessions_ms,busiest_map_players_ms,"
        "busiest_map_cells_ms,busiest_map_obj_updates_ms,busiest_map_relocation_ms,"
        "busiest_map_players_post_vis_ms,busiest_map_corpses_ms,busiest_map_grid_ms,"
        "busiest_map_scripts_ms,busiest_map_instance_ms,busiest_map_weather_ms,busiest_map_wait_ms,"
        "update_players_calls,update_players_ms,update_players_max_ms,update_players_processed,"
        "update_players_skipped,update_players_bot_processed,update_players_bot_skipped,"
        "update_players_wait_bot_only_skipped,"
        "player_update_calls,player_update_ms,player_update_max_ms,"
        "player_update_bot_calls,player_update_bot_ms,player_update_bot_max_ms,"
        "playerbot_hook_calls,playerbot_hook_ms,playerbot_hook_max_ms,"
        "playerbot_hook_suppressed_calls,playerbot_hook_suppressed_elapsed_ms,"
        "playerbot_ai_calls,playerbot_ai_ms,playerbot_ai_max_ms,"
        "playerbot_ai_visible_calls,playerbot_ai_visible_ms,playerbot_ai_visible_max_ms,"
        "playerbot_ai_background_calls,playerbot_ai_background_ms,playerbot_ai_background_max_ms,"
        "playerbot_mgr_calls,playerbot_mgr_ms,playerbot_mgr_max_ms,"
        "playerbot_planner_checks,playerbot_planner_allowed,playerbot_planner_deferred,playerbot_planner_fast_lane,"
        "bot_pressure_level,bot_pressure_score,bot_pressure_updates,bot_pressure_deferred_quest,"
        "bot_pressure_deferred_visible_objective,bot_pressure_deferred_force_chase,"
        "bot_pressure_deferred_stale_recover,bot_pressure_deferred_path,"
        "travel_quest_requests,travel_quest_request_ms,travel_quest_request_max_ms,travel_quest_fetches,"
        "travel_quest_strict_partition_calls,travel_quest_soft_partition_calls,travel_quest_broad_partition_calls,"
        "travel_quest_async_jobs,travel_quest_async_ms,travel_quest_async_max_ms,travel_quest_async_empty_jobs,"
        "travel_quest_async_result_points,travel_partitions_calls,travel_partitions_ms,travel_partitions_max_ms,"
        "travel_partitions_destinations,travel_partitions_result_points,travel_partitions_empty_calls,"
        "travel_quest_partition_calls,travel_quest_partition_ms,"
        "learning_flush_calls,learning_flush_ms,learning_flush_max_ms,learning_task_rows_flushed,"
        "learning_combat_rows_flushed,learning_task_rows_dropped,learning_combat_rows_dropped,"
        "learning_task_queue_size,learning_combat_queue_size,"
        "avg_diff_ms,current_diff_ms,qpc_counter\n";
    fputs(header, file);
    fflush(file);

    return file;
}
