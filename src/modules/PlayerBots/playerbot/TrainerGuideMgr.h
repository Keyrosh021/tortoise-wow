#pragma once

#include "Common.h"
#include "SharedDefines.h"

#include <mutex>
#include <vector>

class Player;

namespace ai
{
    struct TrainerGuideEntry
    {
        TrainerType trainerType = TRAINER_TYPE_CLASS;
        uint32 requirement = 0;
        int32 trainerEntry = 0;
        uint32 priority = 0;
        bool enabled = true;
    };

    class TrainerGuideMgr
    {
    public:
        TrainerGuideMgr() = default;

        void Load();
        void Reload();

        std::vector<int32> FilterTrainerEntries(Player* bot, TrainerType trainerType, const std::vector<int32>& eligibleEntries, uint32 maxCount = 64);
        std::vector<int32> FilterTrainerEntries(Player* bot, TrainerType trainerType, const std::vector<int32>& eligibleEntries, const std::vector<uint32>& requirementHints, uint32 maxCount = 64);
        bool HasGuideData();

    private:
        uint32 GetRequirement(Player* bot, TrainerType trainerType) const;
        void SeedDiscoveredTrainerRequirements();

        std::mutex loadMutex;
        bool loaded = false;
        std::vector<TrainerGuideEntry> entries;
    };
}

#define sTrainerGuideMgr MaNGOS::Singleton<ai::TrainerGuideMgr>::Instance()
