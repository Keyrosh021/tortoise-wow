#pragma once

#include "Common.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "AllocatorWithCategory.h"
#include <cstdio>

class ChatHandler;

struct PerformanceMonitor : public IPerfMonitor
{
	PerformanceMonitor();

	void Initialize();
	void FrameStart();
	void FrameEnd(uint32 delta);

	void SetReportInterval(uint32 IntervalInSeconds);

	void ReportCPU(ChatHandler& Handler);
	void ReportMemory(ChatHandler& Handler);

	virtual void ReportAlloc(const char* Category, size_t Bytes) override;
	virtual void ReportDealloc(const char* Category, size_t Bytes) override;

	XStatTimer Tick;
	XStatTimer WorldSleep;
	XStatTimer WorldTick;
	XStatTimer UpdateSession;
	XStatTimer MapManager;
	XStatTimer TEST0;

    struct WorldPhaseSnapshot
    {
        uint32 sessionsUpdateMs = 0;
        uint32 transportUpdateMs = 0;
        uint32 mapMgrUpdateMs = 0;
        uint32 battlegroundUpdateMs = 0;
        uint32 lfgUpdateMs = 0;
        uint32 guardUpdateMs = 0;
        uint32 zoneScriptUpdateMs = 0;
        uint32 dynamicVisUpdateMs = 0;
        uint32 playerbotUpdateMs = 0;
        uint32 asyncQueriesMs = 0;
        uint32 worldUpdateMs = 0;
        uint32 activeSessions = 0;
        uint32 queuedSessions = 0;
        uint32 averageDiffMs = 0;
        uint32 currentDiffMs = 0;
    };

    void RecordWorldPhaseSnapshot(WorldPhaseSnapshot const& snapshot);

protected:
    struct WorldPhaseAggregate
    {
        uint64 samples = 0;
        uint64 sessionsUpdateMs = 0;
        uint64 transportUpdateMs = 0;
        uint64 mapMgrUpdateMs = 0;
        uint64 battlegroundUpdateMs = 0;
        uint64 lfgUpdateMs = 0;
        uint64 guardUpdateMs = 0;
        uint64 zoneScriptUpdateMs = 0;
        uint64 dynamicVisUpdateMs = 0;
        uint64 playerbotUpdateMs = 0;
        uint64 asyncQueriesMs = 0;
        uint64 worldUpdateMs = 0;
        uint32 maxWorldUpdateMs = 0;
        uint32 maxSessionsUpdateMs = 0;
        uint32 maxMapMgrUpdateMs = 0;
        uint32 maxPlayerbotUpdateMs = 0;
        uint32 maxAsyncQueriesMs = 0;
        uint32 maxQueuedSessions = 0;
        uint32 maxActiveSessions = 0;
        uint32 maxAverageDiffMs = 0;
        uint32 maxCurrentDiffMs = 0;
        WorldPhaseSnapshot last;
    };

	uint32 QPC_Counter = 0;

	void ReportPerformanceToDB();
    void ResetWorldPhaseAggregate();
    FILE* OpenPerfTraceFile();

	IntervalTimer IntervalReport;
    WorldPhaseAggregate m_worldPhaseAggregate;
    FILE* m_perfTraceFile = nullptr;

	using MemBytesMap = std::unordered_map<const char*, int64>;
	MemBytesMap MemBytes;
	std::mutex MemBytesGuard;
};

extern PerformanceMonitor sPerfMonitor;
