#ifndef MANGOSSERVER_TURTLELFGMGR_H
#define MANGOSSERVER_TURTLELFGMGR_H

#include "Common.h"

#include <array>
#include <map>
#include <string>
#include <vector>

class Player;

class TurtleLFGMgr
{
    public:
        bool HandleAddonMessage(Player* player, const std::string& message);

    private:
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
        bool HandleQueueJoin(Player* player);
        bool HandleQueueLeave(Player* player);
        bool HandleQueueStatus(Player* player);
        bool HandleRolecheckResponse(Player* player);
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