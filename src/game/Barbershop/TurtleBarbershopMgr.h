#pragma once

#include <cstdint>
#include <string>

class Player;

class TurtleBarbershopMgr
{
public:
    struct AppearanceState
    {
        uint8_t hairStyle = 0;
        uint8_t hairColor = 0;
        uint8_t facialHair = 0;
    };

    struct SessionState
    {
        AppearanceState original;
        AppearanceState preview;
    };

    bool HandleAddonMessage(Player* player, std::string const& message);
    bool OpenBarbershop(Player* player);
    void HandlePlayerLogout(Player* player);

private:
    bool HandlePreviewStyle(Player* player, std::string const& payload);
    bool HandlePreviewColor(Player* player, std::string const& payload);
    bool HandlePreviewFacialHair(Player* player, std::string const& payload);
    bool HandlePurchase(Player* player);
    bool HandleReset(Player* player);
    bool HandleClose(Player* player);

    SessionState* GetSession(Player* player);
    SessionState const* GetSession(Player const* player) const;

    void ApplyAppearance(Player* player, AppearanceState const& state) const;
    void CloseSession(Player* player, bool restoreAppearance, bool notifyClient);
    void SendOpenBarber(Player* player, SessionState const& session) const;
    void SendUpdatePreview(Player* player, SessionState const& session) const;
    void SendPurchaseResult(Player* player, SessionState const& session) const;
    void SendError(Player* player, uint8_t errorCode) const;

    uint32_t CalculateCost(Player const* player, SessionState const& session) const;
    bool TryUpdatePreview(Player* player, uint8_t customization, uint8_t selectedIndex);
};

extern TurtleBarbershopMgr sTurtleBarbershopMgr;