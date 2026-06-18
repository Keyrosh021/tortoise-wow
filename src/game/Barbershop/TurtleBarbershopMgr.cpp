#include "Barbershop/TurtleBarbershopMgr.h"

#include "Player.h"
#include "SharedDefines.h"
#include "Util.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

namespace
{
    constexpr char kPrefix[] = "TW_BARBERSHOP";
    constexpr char kFieldDelimiter = ';';

    enum CustomizationType : uint8_t
    {
        CUSTOMIZATION_STYLE = 1,
        CUSTOMIZATION_COLOR = 2,
        CUSTOMIZATION_FACIAL_HAIR = 3,
    };

    struct CustomizationCounts
    {
        uint8_t male = 0;
        uint8_t female = 0;
    };

    using CustomizationTable = std::array<CustomizationCounts, RACE_GOBLIN + 1>;

    CustomizationTable BuildStyleTable()
    {
        CustomizationTable table = {};
        table[RACE_HUMAN] = {17, 26};
        table[RACE_ORC] = {13, 14};
        table[RACE_DWARF] = {17, 19};
        table[RACE_NIGHTELF] = {12, 12};
        table[RACE_UNDEAD] = {16, 17};
        table[RACE_TAUREN] = {13, 12};
        table[RACE_GNOME] = {12, 12};
        table[RACE_TROLL] = {10, 10};
        table[RACE_GOBLIN] = {14, 17};
        return table;
    }

    CustomizationTable BuildColorTable()
    {
        CustomizationTable table = {};
        table[RACE_HUMAN] = {10, 10};
        table[RACE_ORC] = {8, 8};
        table[RACE_DWARF] = {10, 10};
        table[RACE_NIGHTELF] = {10, 10};
        table[RACE_UNDEAD] = {11, 11};
        table[RACE_TAUREN] = {9, 9};
        table[RACE_GNOME] = {10, 10};
        table[RACE_TROLL] = {10, 10};
        table[RACE_GOBLIN] = {5, 5};
        return table;
    }

    CustomizationTable BuildFacialHairTable()
    {
        CustomizationTable table = {};
        table[RACE_HUMAN] = {9, 7};
        table[RACE_ORC] = {11, 7};
        table[RACE_DWARF] = {21, 6};
        table[RACE_NIGHTELF] = {14, 10};
        table[RACE_UNDEAD] = {17, 8};
        table[RACE_TAUREN] = {9, 7};
        table[RACE_GNOME] = {12, 7};
        table[RACE_TROLL] = {14, 10};
        table[RACE_GOBLIN] = {10, 5};
        return table;
    }

    CustomizationTable const kStyleCounts = BuildStyleTable();
    CustomizationTable const kColorCounts = BuildColorTable();
    CustomizationTable const kFacialHairCounts = BuildFacialHairTable();

    std::unordered_map<uint32, TurtleBarbershopMgr::SessionState> g_sessions;

    uint8 GetGenderIndex(Player const* player)
    {
        return player->GetGender() == GENDER_FEMALE ? 1 : 0;
    }

    uint8 GetCustomizationCount(Player const* player, uint8 customization)
    {
        if (!player || player->GetRace() > RACE_GOBLIN)
            return 0;

        CustomizationCounts counts = {};
        switch (customization)
        {
            case CUSTOMIZATION_STYLE:
                counts = kStyleCounts[player->GetRace()];
                break;
            case CUSTOMIZATION_COLOR:
                counts = kColorCounts[player->GetRace()];
                break;
            case CUSTOMIZATION_FACIAL_HAIR:
                counts = kFacialHairCounts[player->GetRace()];
                break;
            default:
                return 0;
        }

        return GetGenderIndex(player) == 0 ? counts.male : counts.female;
    }

    TurtleBarbershopMgr::AppearanceState ReadAppearance(Player const* player)
    {
        TurtleBarbershopMgr::AppearanceState state;
        state.hairStyle = player->GetByteValue(PLAYER_BYTES, 2);
        state.hairColor = player->GetByteValue(PLAYER_BYTES, 3);
        state.facialHair = player->GetByteValue(PLAYER_BYTES_2, 0);
        return state;
    }

    std::string BuildMessage(char const* messageType, std::initializer_list<uint32> fields)
    {
        std::string message = messageType;
        for (uint32 field : fields)
        {
            message.push_back(kFieldDelimiter);
            message += std::to_string(field);
        }
        return message;
    }

    bool ParseIndex(std::string const& payload, uint8& value)
    {
        Tokenizer parts(payload, kFieldDelimiter, 2);
        if (parts.size() < 2)
            return false;

        try
        {
            unsigned long parsed = std::stoul(std::string(parts[1]));
            if (parsed > 255)
                return false;
            value = static_cast<uint8>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}

TurtleBarbershopMgr sTurtleBarbershopMgr;

bool TurtleBarbershopMgr::HandleAddonMessage(Player* player, std::string const& message)
{
    if (!player || message.compare(0, std::strlen(kPrefix), kPrefix) != 0)
        return false;

    std::string payload = message.substr(std::strlen(kPrefix));
    if (!payload.empty() && (payload[0] == '\t' || payload[0] == kFieldDelimiter))
        payload.erase(0, 1);

    if (payload.compare(0, std::strlen("C2S_PREVIEW_STYLE"), "C2S_PREVIEW_STYLE") == 0)
        return HandlePreviewStyle(player, payload);
    if (payload.compare(0, std::strlen("C2S_PREVIEW_COLOR"), "C2S_PREVIEW_COLOR") == 0)
        return HandlePreviewColor(player, payload);
    if (payload.compare(0, std::strlen("C2S_PREVIEW_FACIAL_HAIR"), "C2S_PREVIEW_FACIAL_HAIR") == 0)
        return HandlePreviewFacialHair(player, payload);
    if (payload.compare(0, std::strlen("C2S_PURCHASE"), "C2S_PURCHASE") == 0)
        return HandlePurchase(player);
    if (payload.compare(0, std::strlen("C2S_RESET"), "C2S_RESET") == 0)
        return HandleReset(player);
    if (payload.compare(0, std::strlen("C2S_CLOSE"), "C2S_CLOSE") == 0)
        return HandleClose(player);

    SendError(player, 5);
    return true;
}

bool TurtleBarbershopMgr::OpenBarbershop(Player* player)
{
    if (!player)
        return false;

    if (GetSession(player))
    {
        SendError(player, 3);
        return true;
    }

    SessionState session;
    session.original = ReadAppearance(player);
    session.preview = session.original;

    g_sessions[player->GetGUIDLow()] = session;
    SendOpenBarber(player, g_sessions[player->GetGUIDLow()]);
    return true;
}

void TurtleBarbershopMgr::HandlePlayerLogout(Player* player)
{
    CloseSession(player, true, false);
}

bool TurtleBarbershopMgr::HandlePreviewStyle(Player* player, std::string const& payload)
{
    uint8 selectedIndex = 0;
    if (!ParseIndex(payload, selectedIndex))
    {
        SendError(player, 5);
        return true;
    }

    return TryUpdatePreview(player, CUSTOMIZATION_STYLE, selectedIndex);
}

bool TurtleBarbershopMgr::HandlePreviewColor(Player* player, std::string const& payload)
{
    uint8 selectedIndex = 0;
    if (!ParseIndex(payload, selectedIndex))
    {
        SendError(player, 5);
        return true;
    }

    return TryUpdatePreview(player, CUSTOMIZATION_COLOR, selectedIndex);
}

bool TurtleBarbershopMgr::HandlePreviewFacialHair(Player* player, std::string const& payload)
{
    uint8 selectedIndex = 0;
    if (!ParseIndex(payload, selectedIndex))
    {
        SendError(player, 5);
        return true;
    }

    return TryUpdatePreview(player, CUSTOMIZATION_FACIAL_HAIR, selectedIndex);
}

bool TurtleBarbershopMgr::HandlePurchase(Player* player)
{
    SessionState* session = GetSession(player);
    if (!session)
    {
        SendError(player, 1);
        return true;
    }

    uint32 cost = CalculateCost(player, *session);
    if (cost > player->GetMoney())
    {
        SendError(player, 2);
        return true;
    }

    if (cost > 0)
        player->ModifyMoney(-static_cast<int32>(cost));

    session->original = session->preview;
    SendPurchaseResult(player, *session);
    SendUpdatePreview(player, *session);
    return true;
}

bool TurtleBarbershopMgr::HandleReset(Player* player)
{
    SessionState* session = GetSession(player);
    if (!session)
    {
        SendError(player, 1);
        return true;
    }

    session->preview = session->original;
    ApplyAppearance(player, session->preview);
    SendUpdatePreview(player, *session);
    return true;
}

bool TurtleBarbershopMgr::HandleClose(Player* player)
{
    if (!GetSession(player))
    {
        SendError(player, 1);
        return true;
    }

    CloseSession(player, true, true);
    return true;
}

TurtleBarbershopMgr::SessionState* TurtleBarbershopMgr::GetSession(Player* player)
{
    if (!player)
        return nullptr;

    auto itr = g_sessions.find(player->GetGUIDLow());
    return itr == g_sessions.end() ? nullptr : &itr->second;
}

TurtleBarbershopMgr::SessionState const* TurtleBarbershopMgr::GetSession(Player const* player) const
{
    if (!player)
        return nullptr;

    auto itr = g_sessions.find(player->GetGUIDLow());
    return itr == g_sessions.end() ? nullptr : &itr->second;
}

void TurtleBarbershopMgr::ApplyAppearance(Player* player, AppearanceState const& state) const
{
    if (!player)
        return;

    player->SetByteValue(PLAYER_BYTES, 2, state.hairStyle);
    player->SetByteValue(PLAYER_BYTES, 3, state.hairColor);
    player->SetByteValue(PLAYER_BYTES_2, 0, state.facialHair);
    player->UpdateAppearance();
    player->DirectSendPublicValueUpdate({PLAYER_BYTES, PLAYER_BYTES_2, UNIT_FIELD_DISPLAYID});
}

void TurtleBarbershopMgr::CloseSession(Player* player, bool restoreAppearance, bool notifyClient)
{
    if (!player)
        return;

    auto itr = g_sessions.find(player->GetGUIDLow());
    if (itr == g_sessions.end())
        return;

    if (restoreAppearance)
        ApplyAppearance(player, itr->second.original);

    g_sessions.erase(itr);

    if (notifyClient)
        player->SendAddonMessage(kPrefix, "S2C_CLOSE_BARBER");
}

void TurtleBarbershopMgr::SendOpenBarber(Player* player, SessionState const& session) const
{
    player->SendAddonMessage(kPrefix, BuildMessage("S2C_OPEN_BARBER", {
        0,
        session.original.hairStyle,
        GetCustomizationCount(player, CUSTOMIZATION_STYLE),
        session.original.hairColor,
        GetCustomizationCount(player, CUSTOMIZATION_COLOR),
        session.original.facialHair,
        GetCustomizationCount(player, CUSTOMIZATION_FACIAL_HAIR)
    }));
}

void TurtleBarbershopMgr::SendUpdatePreview(Player* player, SessionState const& session) const
{
    player->SendAddonMessage(kPrefix, BuildMessage("S2C_UPDATE_PREVIEW", {
        CalculateCost(player, session),
        session.preview.hairStyle,
        session.preview.hairColor,
        session.preview.facialHair
    }));
}

void TurtleBarbershopMgr::SendPurchaseResult(Player* player, SessionState const& session) const
{
    player->SendAddonMessage(kPrefix, BuildMessage("S2C_PURCHASE_RESULT", {
        1,
        session.preview.hairStyle,
        session.preview.hairColor,
        session.preview.facialHair
    }));
}

void TurtleBarbershopMgr::SendError(Player* player, uint8_t errorCode) const
{
    if (player)
        player->SendAddonMessage(kPrefix, BuildMessage("S2C_ERROR", {errorCode}));
}

uint32_t TurtleBarbershopMgr::CalculateCost(Player const* player, SessionState const& session) const
{
    if (!player)
        return 0;

    uint32 level = std::max<uint32>(1, player->GetLevel());
    uint32 cost = 0;

    if (session.preview.hairStyle != session.original.hairStyle)
        cost += level * 250;
    if (session.preview.hairColor != session.original.hairColor)
        cost += level * 100;
    if (session.preview.facialHair != session.original.facialHair)
        cost += level * 200;

    return cost;
}

bool TurtleBarbershopMgr::TryUpdatePreview(Player* player, uint8_t customization, uint8_t selectedIndex)
{
    SessionState* session = GetSession(player);
    if (!session)
    {
        SendError(player, 1);
        return true;
    }

    uint8 count = GetCustomizationCount(player, customization);
    if (count == 0 || selectedIndex >= count)
    {
        SendError(player, 4);
        return true;
    }

    switch (customization)
    {
        case CUSTOMIZATION_STYLE:
            session->preview.hairStyle = selectedIndex;
            break;
        case CUSTOMIZATION_COLOR:
            session->preview.hairColor = selectedIndex;
            break;
        case CUSTOMIZATION_FACIAL_HAIR:
            session->preview.facialHair = selectedIndex;
            break;
        default:
            SendError(player, 5);
            return true;
    }

    ApplyAppearance(player, session->preview);
    SendUpdatePreview(player, *session);
    return true;
}