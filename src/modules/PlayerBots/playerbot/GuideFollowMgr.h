#ifndef _GuideFollowMgr_H
#define _GuideFollowMgr_H

#include "Common.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

class Player;

namespace ai
{
    // One actionable leveling-guide step for a bot (RestedXP routes parsed into
    // ai_playerbot_leveling_step; see artifacts/parse_rxp_guides.py).
    struct GuideStep
    {
        uint32 guideId = 0;
        uint32 seqNo = 0;
        std::string action;      // accept | turnin | complete | grind | goto
        uint32 questId = 0;
        int32 objectiveIdx = -1;
        std::string zoneName;
        float xPct = 0, yPct = 0;
        std::string detail;
    };

    // Follows speedrunner leveling routes: per-bot cursor over guide steps, advancing past
    // satisfied/inapplicable steps lazily at query time. The current step's quest feeds travel
    // selection ahead of the generic machinery — the guide already did the thinking.
    class GuideFollowMgr
    {
    public:
        void Load();                                     // world startup: cache all steps + cursors
        bool GetCurrentStep(Player* bot, GuideStep& out); // resolve/advance the bot's cursor
        void ResetCursor(Player* bot);                    // e.g. on randomize/level jump

    private:
        struct StepRow : GuideStep
        {
            uint8 faction = 0;   // 0 both, 1 A, 2 H
            uint32 raceMask = 0, classMask = 0;
        };
        struct Guide
        {
            uint32 id = 0;
            std::string name;
            uint8 faction = 0;
            uint32 raceMask = 0, classMask = 0;
            uint32 minLevel = 0, maxLevel = 0;
            std::string nextGuide;
            std::vector<StepRow> steps;                  // seq order
        };
        struct Cursor
        {
            uint32 guideId = 0; uint32 stepIdx = 0; bool loaded = false;
            // hot-path cache: resolved step + expiry so 1000 bots don't serialize on the mutex
            GuideStep cached; bool cachedValid = false; uint32 cacheUntilMs = 0;
        };

        Guide const* PickGuide(Player* bot) const;
        Guide const* FindGuideByName(std::string const& name, Player* bot) const;
        bool StepApplies(StepRow const& step, Player* bot) const;
        bool StepSatisfied(StepRow const& step, Player* bot) const;
        void SaveCursor(uint32 guidLow, Cursor const& c);

        std::map<uint32, Guide> m_guides;
        std::multimap<std::string, uint32> m_guidesByName;
        std::map<uint32, Cursor> m_cursors;              // bot guidLow -> cursor
        std::mutex m_mutex;
        bool m_loaded = false;
    };
}

extern ai::GuideFollowMgr sGuideFollowMgr;

#endif
