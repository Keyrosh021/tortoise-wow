#include "playerbot/playerbot.h"
#include "GuideFollowMgr.h"

#include "Database/DatabaseEnv.h"
#include "Objects/Player.h"

ai::GuideFollowMgr sGuideFollowMgr;

using namespace ai;

void GuideFollowMgr::Load()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_loaded)
        return;

    // cursor persistence
    WorldDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS ai_playerbot_guide_cursor ("
        "bot_guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        "guide_id INT UNSIGNED NOT NULL,"
        "step_idx INT UNSIGNED NOT NULL DEFAULT 0)");

    uint32 steps = 0;
    if (auto result = WorldDatabase.Query(
        "SELECT guide_id, guide_name, faction, race_mask, class_mask, min_level, max_level,"
        " seq_no, action, quest_id, objective_idx, zone_name, x_pct, y_pct, detail, next_guide"
        " FROM ai_playerbot_leveling_step ORDER BY guide_id, seq_no"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 gid = f[0].GetUInt32();
            Guide& g = m_guides[gid];
            if (!g.id)
            {
                g.id = gid;
                g.name = f[1].GetCppString();
                std::string fac = f[2].GetCppString();
                g.faction = fac == "A" ? 1 : (fac == "H" ? 2 : 0);
                g.raceMask = f[3].GetUInt32();
                g.classMask = f[4].GetUInt32();
                g.minLevel = f[5].GetUInt32();
                g.maxLevel = f[6].GetUInt32();
                g.nextGuide = f[15].GetCppString();
                m_guidesByName.insert({ g.name, gid });
            }
            StepRow s;
            s.guideId = gid;
            s.seqNo = f[7].GetUInt32();
            s.action = f[8].GetCppString();
            s.questId = f[9].GetUInt32();
            s.objectiveIdx = f[10].GetInt32();
            s.zoneName = f[11].GetCppString();
            s.xPct = f[12].GetFloat();
            s.yPct = f[13].GetFloat();
            s.detail = f[14].GetCppString();
            s.faction = g.faction;
            s.raceMask = g.raceMask;
            s.classMask = g.classMask;
            g.steps.push_back(s);
            ++steps;
        } while (result->NextRow());
    }

    if (auto result = WorldDatabase.Query("SELECT bot_guid, guide_id, step_idx FROM ai_playerbot_guide_cursor"))
    {
        do
        {
            Field* f = result->Fetch();
            Cursor c;
            c.guideId = f[1].GetUInt32();
            c.stepIdx = f[2].GetUInt32();
            c.loaded = true;
            m_cursors[f[0].GetUInt32()] = c;
        } while (result->NextRow());
    }

    m_loaded = true;
    sLog.outString("GuideFollowMgr: loaded %u guides, %u steps, %u cursors",
        (uint32)m_guides.size(), steps, (uint32)m_cursors.size());
}

GuideFollowMgr::Guide const* GuideFollowMgr::PickGuide(Player* bot) const
{
    uint8 botFaction = (bot->GetTeam() == ALLIANCE) ? 1 : 2;
    uint32 level = bot->GetLevel();
    Guide const* best = nullptr;
    for (auto const& kv : m_guides)
    {
        Guide const& g = kv.second;
        if (!g.minLevel || !g.maxLevel)
            continue;                                    // attunement/dungeon guides: not for leveling
        if (g.faction && g.faction != botFaction)
            continue;
        if (g.raceMask && !(g.raceMask & (1 << (bot->getRace() - 1))))
            continue;
        if (g.classMask && !(g.classMask & (1 << (bot->getClass() - 1))))
            continue;
        if (level < g.minLevel || level > g.maxLevel)
            continue;
        // prefer the tightest band that still fits (most specific route)
        if (!best || (g.maxLevel - g.minLevel) < (best->maxLevel - best->minLevel))
            best = &g;
    }
    return best;
}

GuideFollowMgr::Guide const* GuideFollowMgr::FindGuideByName(std::string const& name, Player* bot) const
{
    uint8 botFaction = (bot->GetTeam() == ALLIANCE) ? 1 : 2;
    auto range = m_guidesByName.equal_range(name);
    for (auto it = range.first; it != range.second; ++it)
    {
        Guide const& g = m_guides.at(it->second);
        if (!g.faction || g.faction == botFaction)
            return &g;
    }
    return nullptr;
}

bool GuideFollowMgr::StepApplies(StepRow const& step, Player* bot) const
{
    if (step.action != "accept" && step.action != "turnin" && step.action != "complete")
        return false;                                    // v1: goto/grind steps are skipped
    if (!step.questId || !sObjectMgr.GetQuestTemplate(step.questId))
        return false;
    return true;
}

bool GuideFollowMgr::StepSatisfied(StepRow const& step, Player* bot) const
{
    QuestStatus status = bot->GetQuestStatus(step.questId);
    bool rewarded = bot->GetQuestRewardStatus(step.questId);

    if (step.action == "accept")
        return rewarded || status != QUEST_STATUS_NONE;  // has it (or already done)
    if (step.action == "complete")
        return rewarded || status == QUEST_STATUS_COMPLETE ||
               (status == QUEST_STATUS_NONE && false);   // not even accepted: NOT satisfied — go accept happens earlier in guide
    if (step.action == "turnin")
        return rewarded;
    return true;
}

bool GuideFollowMgr::GetCurrentStep(Player* bot, GuideStep& out)
{
    if (!bot)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_loaded || m_guides.empty())
        return false;

    Cursor& cur = m_cursors[bot->GetGUIDLow()];

    // Hot-path cache: the travel planner asks often; re-resolving (advance scan + quest-status
    // checks) for 1000 bots serialized on this mutex pinned the world thread. Serve the cached
    // step for 5s; correctness is unaffected (a just-finished step advances at most 5s late).
    const uint32 nowMs = WorldTimer::getMSTime();
    if (cur.cacheUntilMs && nowMs < cur.cacheUntilMs)
    {
        if (!cur.cachedValid)
            return false;
        out = cur.cached;
        return true;
    }
    Guide const* guide = nullptr;
    auto git = m_guides.find(cur.guideId);
    if (cur.guideId && git != m_guides.end())
        guide = &git->second;

    // (re)pick when unset or outgrown (2+ levels past the band's top keeps short overlaps working)
    if (!guide || bot->GetLevel() > guide->maxLevel + 1)
    {
        // follow the chain first if the current guide names a successor
        Guide const* next = nullptr;
        if (guide && !guide->nextGuide.empty())
            next = FindGuideByName(guide->nextGuide, bot);
        if (!next || bot->GetLevel() > next->maxLevel + 1)
            next = PickGuide(bot);
        if (!next)
        {
            cur.cacheUntilMs = nowMs + 15000;   // no route for this bot: don't re-scan for 15s
            cur.cachedValid = false;
            return false;
        }
        guide = next;
        cur.guideId = next->id;
        cur.stepIdx = 0;
        SaveCursor(bot->GetGUIDLow(), cur);
    }

    // advance past satisfied / inapplicable steps
    uint32 idx = cur.stepIdx;
    uint32 advanced = 0;
    while (idx < guide->steps.size())
    {
        StepRow const& s = guide->steps[idx];
        if (StepApplies(s, bot) && !StepSatisfied(s, bot))
            break;
        ++idx;
        ++advanced;
        if (advanced > 4000)
            break;                                       // safety
    }

    if (idx != cur.stepIdx)
    {
        cur.stepIdx = idx;
        SaveCursor(bot->GetGUIDLow(), cur);
    }

    cur.cacheUntilMs = nowMs + 5000;
    if (idx >= guide->steps.size())
    {
        cur.cachedValid = false;
        // guide exhausted: hop the chain next query
        if (!guide->nextGuide.empty())
            if (Guide const* next = FindGuideByName(guide->nextGuide, bot))
            {
                cur.guideId = next->id;
                cur.stepIdx = 0;
                SaveCursor(bot->GetGUIDLow(), cur);
            }
        return false;
    }

    out = guide->steps[idx];
    cur.cached = out;
    cur.cachedValid = true;
    return true;
}

void GuideFollowMgr::ResetCursor(Player* bot)
{
    if (!bot)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cursors.erase(bot->GetGUIDLow());
    WorldDatabase.PExecute("DELETE FROM ai_playerbot_guide_cursor WHERE bot_guid = %u", bot->GetGUIDLow());
}

void GuideFollowMgr::SaveCursor(uint32 guidLow, Cursor const& c)
{
    WorldDatabase.PExecute(
        "REPLACE INTO ai_playerbot_guide_cursor (bot_guid, guide_id, step_idx) VALUES (%u, %u, %u)",
        guidLow, c.guideId, c.stepIdx);
}
