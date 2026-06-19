#pragma once

#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Action.h"
#include "MovementActions.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "ReviveFromCorpseAction.h"
#include "playerbot/TravelMgr.h"
#include <sstream>
#include <unordered_map>

namespace ai
{
    namespace
    {
        struct AutoReleaseProgress
        {
            time_t startTime = 0;
            time_t lastAttemptTime = 0;
        };

        static std::unordered_map<uint32, AutoReleaseProgress> sAutoReleaseProgress;

        bool ForceBotSpiritRelease(PlayerbotAI* ai, Player* bot, const char* eventName)
        {
            if (!ai || !bot || sServerFacade.IsAlive(bot))
                return false;

            if (bot->GetDeathState() == JUST_DIED)
                bot->KillPlayer();

            if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                bot->BuildPlayerRepop();

            if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                bot->RepopAtGraveyard();
                sPlayerbotAIConfig.logEvent(ai, eventName);
                return true;
            }

            return false;
        }
    }

    class ReleaseSpiritAction : public ChatCommandAction
    {
    public:
        ReleaseSpiritAction(PlayerbotAI* ai, std::string name = "release") : ChatCommandAction(ai, name) {}

    public:
        virtual bool Execute(Event& event) override
        {
            Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
            if (sServerFacade.IsAlive(bot))
                return false;

            if (bot->GetCorpse() && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                ai->TellPlayerNoFacing(requester, "I am already a spirit", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
                return false;
            }

            WorldPacket& p = event.getPacket();
            if (!p.empty() && p.GetOpcode() == CMSG_REPOP_REQUEST)
                ai->TellPlayerNoFacing(requester, "Releasing...", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            else
                ai->TellPlayerNoFacing(requester, "Meet me at the graveyard", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            sLog.outDetail("Bot #%d %s:%d <%s> released", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);
            if (!sServerFacade.IsAlive(bot) && bot->GetCorpse() && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                bot->RepopAtGraveyard();
                sPlayerbotAIConfig.logEvent(ai, "ReleaseSpiritGraveyardHop");
            }
            else if (!sServerFacade.IsAlive(bot) && !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) && !ai->HasActivePlayerMaster())
            {
                ForceBotSpiritRelease(ai, bot, "ReleaseSpiritDirectFallback");
            }

            // add waiting for ress aura
            if (bot->InBattleGround() && !ai->HasAura(2584, bot))
            {
                // cast Waiting for Resurrect
                bot->CastSpell(bot, 2584, TRIGGERED_OLD_TRIGGERED);
            }

            return true;
        }
    };

    class AutoReleaseSpiritAction : public ReleaseSpiritAction 
    {
    public:
        AutoReleaseSpiritAction(PlayerbotAI* ai, std::string name = "auto release") : ReleaseSpiritAction(ai, name) {}

        virtual bool Execute(Event& event) override
        {
            const uint32 botGuid = bot->GetGUIDLow();
            if (sServerFacade.IsAlive(bot))
            {
                sAutoReleaseProgress.erase(botGuid);
                return false;
            }

            if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                sAutoReleaseProgress.erase(botGuid);
                return false;
            }

            const time_t now = time(nullptr);
            AutoReleaseProgress& progress = sAutoReleaseProgress[botGuid];
            if (!progress.startTime)
                progress.startTime = now;
            progress.lastAttemptTime = now;

            if (!ai->HasActivePlayerMaster() && now - progress.startTime >= 15)
            {
                std::ostringstream out;
                out << "stallSec=" << (now - progress.startTime)
                    << " map=" << bot->GetMapId()
                    << " level=" << (uint32)bot->GetLevel()
                    << " deathState=" << static_cast<uint32>(bot->GetDeathState())
                    << " hasCorpse=" << (bot->GetCorpse() ? 1 : 0)
                    << " ghost=" << (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0);
                sPlayerbotAIConfig.logEvent(ai, "AutoReleaseSpiritStall", "", out.str());
                sAutoReleaseProgress.erase(botGuid);
                if (ForceBotSpiritRelease(ai, bot, "AutoReleaseSpiritDirectFallback"))
                    return true;

                return ai->DoSpecificAction("repop", event, true);
            }

            sLog.outDetail("Bot #%d %s:%d <%s> auto released", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);
            if (!sServerFacade.IsAlive(bot) && bot->GetCorpse() && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                bot->RepopAtGraveyard();
                sPlayerbotAIConfig.logEvent(ai, "AutoReleaseSpiritGraveyardHop");
                sAutoReleaseProgress.erase(botGuid);
            }
            else if (!sServerFacade.IsAlive(bot) && !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) && !ai->HasActivePlayerMaster())
            {
                if (ForceBotSpiritRelease(ai, bot, "AutoReleaseSpiritDirectFallback"))
                    sAutoReleaseProgress.erase(botGuid);
            }

            // add waiting for ress aura
            if (bot->InBattleGround() && !ai->HasAura(2584, bot))
            {
                // cast Waiting for Resurrect
                bot->CastSpell(bot, 2584, TRIGGERED_OLD_TRIGGERED);
            }

            sPlayerbotAIConfig.logEvent(ai, "AutoReleaseSpiritAction");

            return true;
        }

        virtual bool isUseful() override
        {
            if (!sServerFacade.UnitIsDead(bot))
                return false;

            if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                return false;

#ifndef MANGOSBOT_ZERO
            if (bot->InArena())
                return false;
#endif

            return true;
        }
    };

    class RepopAction : public SpiritHealerAction 
    {
    public:
        RepopAction(PlayerbotAI* ai, std::string name = "repop") : SpiritHealerAction(ai, name) {}

    public:
        virtual bool Execute(Event& event) override
        {
            Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

            sLog.outDetail("Repop bot #%d %s:%d <%s>", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            SET_AI_VALUE(uint32, "death count", 0);

            if (bot->IsDead())
            {
                bot->ResurrectPlayer(1.0f, false);
                bot->SpawnCorpseBones();
                bot->SaveToDB();
            }

            
            if (!ai->HasRealPlayerMaster())
            {
                if (Group* group = bot->GetGroup())
                {
                    sLog.outDetail("Repop: Removing bot #%d %s:%d <%s> from group", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                    group->RemoveMember(bot->GetObjectGuid(), 0);
                }
            }

            RESET_AI_VALUE(Unit*, "old target");
            RESET_AI_VALUE(Unit*, "current target");
            RESET_AI_VALUE(Unit*, "pull target");
            RESET_AI_VALUE(bool, "combat::self target");
            RESET_AI_VALUE(WorldPosition, "current position");

            bot->SetSelectionGuid(ObjectGuid());
            ai->TellPlayer(requester, BOT_TEXT("hello"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
            sTravelMgr.SetNullTravelTarget(travelTarget);
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            travelTarget->SetExpireIn(1000);

            PlayerInfo const* defaultPlayerInfo = sObjectMgr.GetPlayerInfo(bot->getRace(), bot->getClass());
            if (defaultPlayerInfo)
            {
                sLog.outDetail("Repop: Teleporting bot #%d %s:%d <%s> to spawn", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                //teleport bot to spawn
                bot->TeleportTo(defaultPlayerInfo->mapId, defaultPlayerInfo->positionX, defaultPlayerInfo->positionY, defaultPlayerInfo->positionZ, defaultPlayerInfo->orientation);
                if (bot->isRealPlayer())
                    bot->SendHeartBeat();
            }
            else
            {
                sLog.outDetail("Repop: Teleporting bot #%d %s:%d <%s> to homebind", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                //teleport bot to homebind
                bot->TeleportToHomebind();
                bot->SendHeartBeat();
            }

            sPlayerbotAIConfig.logEvent(ai, "RepopAction");

            return true;
        }

        virtual bool isUseful() override
        {
            if (bot->InBattleGround())
                return false;

            if (ai->HasActivePlayerMaster())
                return false;

            return true;
        }
    };

    class SelfResurrectAction : public ChatCommandAction
    {
    public:
        SelfResurrectAction(PlayerbotAI* ai, std::string name = "self resurrect") : ChatCommandAction(ai, name) {}

    public:
        bool Execute(Event& event) override
        {
            WorldPacket packet(CMSG_SELF_RES);
            bot->GetSession()->HandleSelfResOpcode(packet);
            return true;
        }

        bool isPossible() override
        {
            return ai->IsStateActive(BotState::BOT_STATE_DEAD) && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
        }
    };
}
