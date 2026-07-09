#include "playerbot/playerbot.h"
#include "MindLog.h"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace mind
{
    Counters& Log()
    {
        static Counters c;
        return c;
    }

    static std::atomic<uint32> s_dumpDueMs{0};
    static std::mutex s_dumpMx;

    void FlushLogIfDue(uint32 nowMs)
    {
        if (nowMs < s_dumpDueMs.load(std::memory_order_relaxed) || !s_dumpMx.try_lock())
            return;
        if (nowMs >= s_dumpDueMs.load(std::memory_order_relaxed))
        {
            s_dumpDueMs.store(nowMs + 60000, std::memory_order_relaxed);
            FILE* f = fopen("logs/mind.csv", "a");
            if (!f) f = fopen("../logs/mind.csv", "a");
            if (f)
            {
                time_t tt = time(0); struct tm tmv; localtime_r(&tt, &tmv); char ts[32];
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
                Counters& c = Log();
                fprintf(f, "%s,steps=%u,engage=%u,chase=%u,lootRuns=%u,lootOpens=%u,social=%u,"
                           "errands=%u,journey=%u,arrivals=%u,stucks=%u,blacklists=%u,teleports=%u,"
                           "idleFallback=%u,maintenance=%u\n", ts,
                    c.steps.exchange(0), c.engage.exchange(0), c.chase.exchange(0),
                    c.lootRuns.exchange(0), c.lootOpens.exchange(0), c.social.exchange(0),
                    c.errands.exchange(0), c.journey.exchange(0), c.arrivals.exchange(0),
                    c.stucks.exchange(0), c.blacklists.exchange(0), c.teleports.exchange(0),
                    c.idleFallback.exchange(0), c.maintenance.exchange(0));
                fclose(f);
            }
        }
        s_dumpMx.unlock();
    }
}
