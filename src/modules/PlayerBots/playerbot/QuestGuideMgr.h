#pragma once

#include "Common.h"
#include <mutex>

class Player;

namespace ai
{
    struct QuestGuideQuest
    {
        uint32 questId = 0;
        std::string role;
        uint32 priority = 0;
        bool required = false;
        std::string chainGroup;
    };

    struct QuestGuideHub
    {
        uint32 id = 0;
        std::string name;
        uint32 faction = 0;
        uint32 raceMask = 0;
        uint32 classMask = 0;
        uint32 minLevel = 0;
        uint32 maxLevel = 0;
        uint32 mapId = 0;
        uint32 areaId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 priority = 0;
        std::string nextHubIds;
        std::vector<QuestGuideQuest> quests;
    };

    struct QuestGuideHubIntent
    {
        bool valid = false;
        uint32 hubId = 0;
        std::string hubName;
        uint32 mapId = 0;
        uint32 areaId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        int32 score = 0;
        uint32 nativeRouteMask = 0;
        uint32 preferredRouteMask = 0;
        bool crossRace = false;
        uint32 pickupCandidates = 0;
        uint32 activeQuests = 0;
        uint32 rewardedQuests = 0;
        uint32 requiredQuests = 0;
        uint32 rewardedRequiredQuests = 0;
        std::string reason;
        std::string nextHubIds;
    };

    class QuestGuideMgr
    {
    public:
        QuestGuideMgr() = default;

        void Load();
        void Reload();

        std::vector<uint32> GetQuestGiverQuestIds(Player* bot, uint32 maxCount = 8, std::string* debugTrace = nullptr);
        std::vector<uint32> GetQuestGiverQuestIdsForHub(Player* bot, uint32 hubId, uint32 maxCount = 8, std::string* debugTrace = nullptr);
        QuestGuideHubIntent SelectNextHub(Player* bot, std::string* debugTrace = nullptr);
        bool HasGuideData();

    private:
        const QuestGuideHub* FindHub(uint32 hubId) const;
        bool MatchesBot(const QuestGuideHub& hub, Player* bot) const;
        bool IsCrossRaceCandidate(const QuestGuideHub& hub, Player* bot) const;
        uint32 GetNativeRouteMask(Player* bot) const;
        uint32 GetPreferredRouteMask(Player* bot) const;
        bool IsPickupRole(const std::string& role) const;
        int32 ScoreHub(const QuestGuideHub& hub, Player* bot) const;
        std::vector<uint32> GetQuestGiverQuestIdsFromHubs(Player* bot, const std::vector<const QuestGuideHub*>& candidateHubs, uint32 maxCount, std::string* debugTrace = nullptr);

        std::mutex loadMutex;
        bool loaded = false;
        std::vector<QuestGuideHub> hubs;
    };
}

#define sQuestGuideMgr MaNGOS::Singleton<ai::QuestGuideMgr>::Instance()
