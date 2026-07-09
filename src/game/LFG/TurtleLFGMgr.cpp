#include "LFG/TurtleLFGMgr.h"

#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace
{
    constexpr char kPrefix[] = "TW_LFG";
    constexpr char kFieldDelimiter = ';';
    constexpr char kArrayDelimiter = ':';

    template <typename Callback>
    void ForEachOnlinePlayer(Callback&& callback)
    {
        HashMapHolder<Player>::ReadGuard guard(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
        {
            if (itr.second && itr.second->GetSession())
                callback(itr.second);
        }
    }
}

TurtleLFGMgr sTurtleLFGMgr;

bool TurtleLFGMgr::HandleAddonMessage(Player* player, std::string const& message)
{
    if (!player || message.compare(0, strlen(kPrefix), kPrefix) != 0)
        return false;

    std::string payload = message.substr(strlen(kPrefix));
    // The client's SendAddonMessage(prefix, msg, channel) transmits "TW_LFG\t<msg>" — a TAB
    // separator, not ';'. Strip any prefix separator or the dispatch below never matches and
    // every request is silently swallowed ("Find Group does nothing").
    while (!payload.empty() && (payload[0] == kFieldDelimiter || payload[0] == '\t' || payload[0] == ' '))
        payload.erase(0, 1);

    if (payload.compare(0, strlen("C2S_GET_GROUPS_STATUS"), "C2S_GET_GROUPS_STATUS") == 0)
        return HandleGroupsStatus(player);
    if (payload.compare(0, strlen("C2S_GET_GROUPS_LIST"), "C2S_GET_GROUPS_LIST") == 0)
        return HandleGroupsList(player);
    if (payload.compare(0, strlen("C2S_GET_GROUP_DETAILS"), "C2S_GET_GROUP_DETAILS") == 0)
        return HandleGroupDetails(player, payload);
    if (payload.compare(0, strlen("C2S_NEW_GROUP"), "C2S_NEW_GROUP") == 0)
        return HandleNewGroup(player, payload);
    if (payload.compare(0, strlen("C2S_UPDATE_GROUP"), "C2S_UPDATE_GROUP") == 0)
        return HandleUpdateGroup(player, payload);
    if (payload.compare(0, strlen("C2S_DELETE_GROUP"), "C2S_DELETE_GROUP") == 0)
        return HandleDeleteGroup(player, payload);
    if (payload.compare(0, strlen("C2S_SIGNUP"), "C2S_SIGNUP") == 0)
        return HandleSignup(player, payload);
    if (payload.compare(0, strlen("C2S_QUEUE_JOIN"), "C2S_QUEUE_JOIN") == 0)
        return HandleQueueJoinPayload(player, payload);
    if (payload.compare(0, strlen("C2S_QUEUE_LEAVE"), "C2S_QUEUE_LEAVE") == 0)
        return HandleQueueLeave(player);
    if (payload.compare(0, strlen("C2S_GET_QUEUE_STATUS"), "C2S_GET_QUEUE_STATUS") == 0)
        return HandleQueueStatus(player);
    if (payload.compare(0, strlen("C2S_ROLECHECK_RESPONSE"), "C2S_ROLECHECK_RESPONSE") == 0)
        return HandleRolecheckResponsePayload(player, payload);
    if (payload.compare(0, strlen("C2S_OFFER_ACCEPT"), "C2S_OFFER_ACCEPT") == 0)
        return HandleOfferAccept(player);

    return true;
}

bool TurtleLFGMgr::HandleGroupsStatus(Player* player)
{
    SendGroupsStatus(player);
    return true;
}

bool TurtleLFGMgr::HandleGroupsList(Player* player)
{
    SendGroupsList(player);
    return true;
}

bool TurtleLFGMgr::HandleGroupDetails(Player* player, std::string const& message)
{
    std::vector<std::string> parts = Split(message, kFieldDelimiter, 2);
    if (parts.size() < 2)
        return true;

    uint32 groupId = 0;
    try
    {
        groupId = std::stoul(parts[1]);
    }
    catch (...)
    {
        return true;
    }

    if (!FindGroup(groupId))
    {
        SendInvalidGroup(player, groupId);
        return true;
    }

    SendGroupDetails(player, groupId);
    return true;
}

bool TurtleLFGMgr::HandleNewGroup(Player* player, std::string const& message)
{
    std::string error;
    if (!CanManageGroup(player, error))
    {
        SendQueueError(player, error);
        return true;
    }

    std::vector<std::string> parts = Split(message, kFieldDelimiter, 6);
    if (parts.size() < 5)
        return true;

    GroupEntry* existing = FindGroupByCreator(player->GetGUIDLow());
    if (existing)
        m_groups.erase(existing->id);

    GroupEntry group;
    group.id = m_nextGroupId++;
    group.creatorGuid = player->GetGUIDLow();
    group.creatorName = player->GetName();
    group.creatorClass = GetClassName(player->GetClass());
    group.title = parts[1];
    group.description = parts[2];

    try
    {
        group.category = std::stoul(parts[3]);
    }
    catch (...)
    {
        group.category = 1;
    }

    group.limits = ParseLimits(parts[4]);

    m_groups[group.id] = group;
    BroadcastGroupsList();
    SendGroupDetails(player, group.id);
    return true;
}

bool TurtleLFGMgr::HandleUpdateGroup(Player* player, std::string const& message)
{
    std::string error;
    if (!CanManageGroup(player, error))
    {
        SendQueueError(player, error);
        return true;
    }

    std::vector<std::string> parts = Split(message, kFieldDelimiter, 6);
    if (parts.size() < 5)
        return true;

    uint32 groupId = 0;
    try
    {
        groupId = std::stoul(parts[1]);
    }
    catch (...)
    {
        return true;
    }

    GroupEntry* group = FindGroup(groupId);
    if (!group || group->creatorGuid != player->GetGUIDLow())
    {
        SendInvalidGroup(player, groupId);
        return true;
    }

    group->title = parts[2];
    group->description = parts[3];
    group->limits = ParseLimits(parts[4]);

    if (parts.size() >= 6)
    {
        group->signups = {};
        std::vector<std::string> signups = Split(parts[5], kFieldDelimiter);
        for (std::string const& signupValue : signups)
        {
            if (signupValue.empty())
                continue;

            std::vector<std::string> signupParts = Split(signupValue, kArrayDelimiter, 2);
            if (signupParts.size() < 2)
                continue;

            uint32 roleId = 0;
            try
            {
                roleId = std::stoul(signupParts[1]);
            }
            catch (...)
            {
                continue;
            }

            SignupEntry signup;
            signup.name = signupParts[0];
            signup.roleIndex = NormalizeRoleIndex(roleId);
            signup.className = "PRIEST";
            if (Player* signupPlayer = ObjectAccessor::FindPlayerByName(signup.name.c_str()))
                signup.className = GetClassName(signupPlayer->GetClass());

            UpsertSignup(*group, signup);
        }
    }

    BroadcastGroupsList();
    SendGroupDetails(player, groupId);
    return true;
}

bool TurtleLFGMgr::HandleDeleteGroup(Player* player, std::string const& message)
{
    std::vector<std::string> parts = Split(message, kFieldDelimiter, 2);
    if (parts.size() < 2)
        return true;

    uint32 groupId = 0;
    try
    {
        groupId = std::stoul(parts[1]);
    }
    catch (...)
    {
        return true;
    }

    GroupEntry* group = FindGroup(groupId);
    if (!group || group->creatorGuid != player->GetGUIDLow())
    {
        SendInvalidGroup(player, groupId);
        return true;
    }

    m_groups.erase(groupId);
    BroadcastGroupsList();
    return true;
}

bool TurtleLFGMgr::HandleSignup(Player* player, std::string const& message)
{
    std::vector<std::string> parts = Split(message, kFieldDelimiter, 4);
    if (parts.size() < 4)
        return true;

    uint32 groupId = 0;
    uint32 roleId = 0;
    try
    {
        groupId = std::stoul(parts[2]);
        roleId = std::stoul(parts[3]);
    }
    catch (...)
    {
        return true;
    }

    GroupEntry* group = FindGroup(groupId);
    if (!group)
    {
        SendInvalidGroup(player, groupId);
        return true;
    }

    if (parts[1] == "true")
    {
        SignupEntry signup;
        signup.name = player->GetName();
        signup.className = GetClassName(player->GetClass());
        signup.roleIndex = NormalizeRoleIndex(roleId);
        UpsertSignup(*group, signup);
    }
    else
    {
        RemoveSignup(*group, player->GetName());
    }

    BroadcastGroupsList();
    SendGroupDetails(player, groupId);
    if (Player* creator = ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER, group->creatorGuid)))
        SendGroupDetails(creator, groupId);
    return true;
}

#include "TurtleLFGQueue.inc"

void TurtleLFGMgr::SendGroupsStatus(Player* player) const
{
    if (!player)
        return;

    player->SendAddonMessage(kPrefix, player->IsHardcore() ? "S2C_GROUPS_STATUS;0" : "S2C_GROUPS_STATUS;1");
}

void TurtleLFGMgr::SendGroupsList(Player* player) const
{
    if (!player)
        return;

    player->SendAddonMessage(kPrefix, "S2C_GROUPS_LIST_UPDATE;start");
    for (auto const& itr : m_groups)
        player->SendAddonMessage(kPrefix, "S2C_GROUPS_LIST_UPDATE;" + BuildGroupJson(itr.second));
    player->SendAddonMessage(kPrefix, "S2C_GROUPS_LIST_UPDATE;end");
}

void TurtleLFGMgr::SendGroupDetails(Player* player, uint32 groupId) const
{
    if (!player)
        return;

    GroupEntry const* group = FindGroup(groupId);
    if (!group)
    {
        SendInvalidGroup(player, groupId);
        return;
    }

    player->SendAddonMessage(kPrefix, "S2C_UPDATE_GROUP;" + std::to_string(groupId) + ";start;" + BuildGroupJson(*group));
    for (uint8 index = 0; index < group->signups.size(); ++index)
    {
        uint8 roleCode = index == 2 ? 0 : static_cast<uint8>(index + 1);
        for (SignupEntry const& signup : group->signups[index])
            player->SendAddonMessage(kPrefix, "S2C_UPDATE_GROUP;" + std::to_string(groupId) + ";" + std::to_string(roleCode) + ";" + BuildSignupJson(signup));
    }
    player->SendAddonMessage(kPrefix, "S2C_UPDATE_GROUP;" + std::to_string(groupId) + ";end");
}

void TurtleLFGMgr::BroadcastGroupsList() const
{
    ForEachOnlinePlayer([this](Player* onlinePlayer)
    {
        if (!onlinePlayer->IsHardcore())
            SendGroupsList(onlinePlayer);
    });
}

void TurtleLFGMgr::SendQueueError(Player* player, std::string const& error) const
{
    if (player)
        player->SendAddonMessage(kPrefix, "S2C_QUEUE_ERROR;" + error);
}

void TurtleLFGMgr::SendQueueLeft(Player* player) const
{
    if (player)
        player->SendAddonMessage(kPrefix, "S2C_QUEUE_LEFT");
}

void TurtleLFGMgr::SendInvalidGroup(Player* player, uint32 groupId) const
{
    if (!player)
        return;

    player->SendAddonMessage(kPrefix, "S2C_UPDATE_GROUP;" + std::to_string(groupId) + ";invalid");
}

TurtleLFGMgr::GroupEntry* TurtleLFGMgr::FindGroup(uint32 groupId)
{
    auto itr = m_groups.find(groupId);
    return itr != m_groups.end() ? &itr->second : nullptr;
}

TurtleLFGMgr::GroupEntry const* TurtleLFGMgr::FindGroup(uint32 groupId) const
{
    auto itr = m_groups.find(groupId);
    return itr != m_groups.end() ? &itr->second : nullptr;
}

TurtleLFGMgr::GroupEntry* TurtleLFGMgr::FindGroupByCreator(uint32 creatorGuid)
{
    for (auto& itr : m_groups)
    {
        if (itr.second.creatorGuid == creatorGuid)
            return &itr.second;
    }
    return nullptr;
}

bool TurtleLFGMgr::CanManageGroup(Player* player, std::string& error) const
{
    if (!player)
        return false;

    if (player->IsHardcore())
    {
        error = "hardcore";
        return false;
    }

    if (Group* group = player->GetGroup())
    {
        if (!group->IsLeader(player->GetObjectGuid()))
        {
            error = "not_leader";
            return false;
        }
    }

    return true;
}

void TurtleLFGMgr::UpsertSignup(GroupEntry& group, SignupEntry const& signup)
{
    RemoveSignup(group, signup.name);

    uint8 roleIndex = signup.roleIndex;
    if (roleIndex < 1 || roleIndex > 3)
        roleIndex = 3;

    group.signups[roleIndex - 1].push_back(signup);
}

void TurtleLFGMgr::RemoveSignup(GroupEntry& group, std::string const& playerName)
{
    for (auto& signups : group.signups)
    {
        signups.erase(
            std::remove_if(signups.begin(), signups.end(), [&playerName](SignupEntry const& signup)
            {
                return signup.name == playerName;
            }),
            signups.end());
    }
}

std::vector<std::string> TurtleLFGMgr::Split(std::string const& value, char delimiter, size_t maxParts)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size())
    {
        if (maxParts && parts.size() + 1 == maxParts)
        {
            parts.push_back(value.substr(start));
            return parts;
        }

        size_t delimiterPos = value.find(delimiter, start);
        if (delimiterPos == std::string::npos)
        {
            parts.push_back(value.substr(start));
            return parts;
        }

        parts.push_back(value.substr(start, delimiterPos - start));
        start = delimiterPos + 1;
    }

    return parts;
}

std::array<uint32, 3> TurtleLFGMgr::ParseLimits(std::string const& value)
{
    std::array<uint32, 3> limits = { 0, 0, 0 };
    std::vector<std::string> parts = Split(value, kArrayDelimiter, 3);
    for (size_t index = 0; index < limits.size() && index < parts.size(); ++index)
    {
        try
        {
            limits[index] = std::stoul(parts[index]);
        }
        catch (...)
        {
            limits[index] = 0;
        }
    }
    return limits;
}

std::string TurtleLFGMgr::BuildGroupJson(GroupEntry const& group)
{
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();

    document.AddMember("id", group.id, allocator);
    document.AddMember("title", rapidjson::Value(group.title.c_str(), allocator), allocator);
    document.AddMember("description", rapidjson::Value(group.description.c_str(), allocator), allocator);
    document.AddMember("category", group.category, allocator);
    document.AddMember("creator", rapidjson::Value(group.creatorName.c_str(), allocator), allocator);
    document.AddMember("class", rapidjson::Value(group.creatorClass.c_str(), allocator), allocator);

    rapidjson::Value limitArray(rapidjson::kArrayType);
    rapidjson::Value confirmedArray(rapidjson::kArrayType);
    for (size_t index = 0; index < group.limits.size(); ++index)
    {
        limitArray.PushBack(group.limits[index], allocator);
        confirmedArray.PushBack(static_cast<uint32>(group.signups[index].size()), allocator);
    }

    document.AddMember("limit", limitArray, allocator);
    document.AddMember("numConfirmed", confirmedArray, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

std::string TurtleLFGMgr::BuildSignupJson(SignupEntry const& signup)
{
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();

    document.AddMember("name", rapidjson::Value(signup.name.c_str(), allocator), allocator);
    document.AddMember("class", rapidjson::Value(signup.className.c_str(), allocator), allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

std::string TurtleLFGMgr::GetClassName(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR: return "WARRIOR";
        case CLASS_PALADIN: return "PALADIN";
        case CLASS_HUNTER: return "HUNTER";
        case CLASS_ROGUE: return "ROGUE";
        case CLASS_PRIEST: return "PRIEST";
        case CLASS_SHAMAN: return "SHAMAN";
        case CLASS_MAGE: return "MAGE";
        case CLASS_WARLOCK: return "WARLOCK";
        case CLASS_DRUID: return "DRUID";
        default: return "PRIEST";
    }
}

uint8 TurtleLFGMgr::NormalizeRoleIndex(uint32 roleIndex)
{
    if (roleIndex == 1 || roleIndex == 2 || roleIndex == 3)
        return static_cast<uint8>(roleIndex);

    return 3;
}