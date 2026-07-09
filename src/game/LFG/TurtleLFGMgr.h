#ifndef MANGOSSERVER_TURTLELFGMGR_H
#define MANGOSSERVER_TURTLELFGMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <array>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class Player;

// Bot-fill hooks (implemented by the PlayerBots module in HostHooks.cpp; weak stubs in
// PlayerbotStubs.cpp when BUILD_PLAYERBOTS=OFF). Must only be called from the world thread.
std::vector<ObjectGuid> Playerbot_SelectLfgFill(uint32 minLevel, uint32 maxLevel,
    uint32 needTank, uint32 needHeal, uint32 needDps, std::vector<ObjectGuid> const& exclude);
void Playerbot_PrepareLfgBot(ObjectGuid botGuid, ObjectGuid masterGuid);
uint8 Playerbot_GetRoleOf(Player* player);   // 1 tank, 2 healer, 4 dps (spec-based)

class TurtleLFGMgr
{
    public:
        bool HandleAddonMessage(Player* player, const std::string& message);
        void LoadDungeons();          // world startup: resolve dungeon codes -> map + entrance
        void Update(uint32 diff);     // world tick: matchmaker, timeouts, bot-fill

    private:
        // ---------------- queue system ----------------
        static constexpr uint8 ROLE_TANK   = 1;
        static constexpr uint8 ROLE_HEALER = 2;
        static constexpr uint8 ROLE_DAMAGE = 4;
        static constexpr time_t ROLECHECK_TIMEOUT = 90;
        static constexpr time_t OFFER_TIMEOUT     = 90;

        enum class QueueState { Queued, Offered };

        struct DungeonInfo
        {
            std::string code;
            uint32 map = 0;
            uint32 minLevel = 0;
            uint32 maxLevel = 60;
            float x = 0, y = 0, z = 0, o = 0;
            bool valid = false;
        };

        struct QueuedPlayer
        {
            ObjectGuid guid;
            uint8 roles = 0;
            std::vector<std::string> dungeons;
            time_t joinTime = 0;
            QueueState state = QueueState::Queued;
            uint32 offerId = 0;
        };

        struct PendingRoleCheck
        {
            ObjectGuid leader;
            uint8 leaderRoles = 0;
            std::vector<std::string> dungeons;
            std::map<ObjectGuid, uint8> responses;  // member -> roles (0 = unanswered)
            time_t deadline = 0;
        };

        struct Offer
        {
            uint32 id = 0;
            std::string dungeonCode;
            struct Slot { ObjectGuid guid; uint8 assignedRole = 0; bool accepted = false; bool isBot = false; };
            std::vector<Slot> slots;                // 1 tank, 1 healer, 3 damage
            time_t deadline = 0;
        };

        bool HandleQueueJoinPayload(Player* player, const std::string& message);
        bool HandleRolecheckResponsePayload(Player* player, const std::string& message);
        void RemoveFromQueue(Player* player, bool notify);
        void TryMatchDungeons();
        bool TryCreateOffer(const std::string& code, std::vector<QueuedPlayer*>& humans);
        void ResolveOffer(Offer& offer);
        void CancelOffer(Offer& offer, ObjectGuid decliner);
        void FinishRoleCheck(uint32 groupId, PendingRoleCheck& rc);
        void AbortRoleCheck(uint32 groupId, PendingRoleCheck& rc);
        void DropStaleEntries();
        void RefillLiveRuns();

        static uint8 RolesMaskFromString(const std::string& roles);
        static std::string RoleCode(uint8 role);
        static bool ClassCanUseRoles(uint8 classId, uint8 mask);

        void SendQueueJoined(Player* player) const;
        void SendQueueStatus(Player* player, const std::vector<std::string>& codes) const;
        void SendRolecheckStart(Player* player, const std::vector<std::string>& codes) const;
        void SendRolecheckInfo(Player* toPlayer, const std::string& name, uint8 roles) const;
        void SendOfferNew(Player* player, const std::string& code, uint8 role) const;
        void SendOfferUpdate(const Offer& offer) const;
        void SendOfferComplete(Player* player) const;

        std::map<ObjectGuid, QueuedPlayer> m_lfgQueue;
        std::map<uint32, PendingRoleCheck> m_roleChecks;   // by core group id
        std::map<uint32, Offer> m_offers;
        std::map<std::string, DungeonInfo> m_dungeons;
        struct LiveRun { uint32 groupId = 0; std::string dungeonCode; ObjectGuid leader; time_t lastFill = 0; };
        std::map<uint32, LiveRun> m_liveRuns;              // groupId -> run (auto-refill on kick)
        uint32 m_nextOfferId = 1;
        uint32 m_updateTimer = 0;
        mutable std::mutex m_queueMutex;   // chat-thread handlers vs world-thread Update

        // ---------------- groups board (existing) ----------------
        struct SignupEntry
        {
            std::string name;
            std::string className;
            uint8 roleIndex = 0;
        };

        struct GroupEntry
        {
            uint32 id = 0;
            uint32 creatorGuid = 0;
            std::string creatorName;
            std::string creatorClass;
            std::string title;
            std::string description;
            uint32 category = 1;
            std::array<uint32, 3> limits = { 0, 0, 0 };
            std::array<std::vector<SignupEntry>, 3> signups;
        };

        bool HandleGroupsStatus(Player* player);
        bool HandleGroupsList(Player* player);
        bool HandleGroupDetails(Player* player, const std::string& message);
        bool HandleNewGroup(Player* player, const std::string& message);
        bool HandleUpdateGroup(Player* player, const std::string& message);
        bool HandleDeleteGroup(Player* player, const std::string& message);
        bool HandleSignup(Player* player, const std::string& message);
        bool HandleQueueLeave(Player* player);
        bool HandleQueueStatus(Player* player);
        bool HandleOfferAccept(Player* player);

        void SendGroupsStatus(Player* player) const;
        void SendGroupsList(Player* player) const;
        void SendGroupDetails(Player* player, uint32 groupId) const;
        void BroadcastGroupsList() const;
        void SendQueueError(Player* player, const std::string& error) const;
        void SendQueueLeft(Player* player) const;
        void SendInvalidGroup(Player* player, uint32 groupId) const;

        GroupEntry* FindGroup(uint32 groupId);
        GroupEntry const* FindGroup(uint32 groupId) const;
        GroupEntry* FindGroupByCreator(uint32 creatorGuid);

        bool CanManageGroup(Player* player, std::string& error) const;
        void UpsertSignup(GroupEntry& group, const SignupEntry& signup);
        void RemoveSignup(GroupEntry& group, const std::string& playerName);

        static std::vector<std::string> Split(std::string const& value, char delimiter, size_t maxParts = 0);
        static std::array<uint32, 3> ParseLimits(std::string const& value);
        static std::string BuildGroupJson(GroupEntry const& group);
        static std::string BuildSignupJson(SignupEntry const& signup);
        static std::string GetClassName(uint8 classId);
        static uint8 NormalizeRoleIndex(uint32 roleIndex);

        uint32 m_nextGroupId = 1;
        std::map<uint32, GroupEntry> m_groups;
};

extern TurtleLFGMgr sTurtleLFGMgr;

#endif