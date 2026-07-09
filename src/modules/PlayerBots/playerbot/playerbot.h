#pragma once

#include "Spells/Spell.h"
#include "WorldPacket.h"
#include "LootMgr.h"
#include "GossipDef.h"
#include "Chat/Chat.h"
#include "Common.h"
#include "World.h"
#include "Spells/SpellMgr.h"
#include "ObjectMgr.h"
#include "Objects/Unit.h"
#include "SharedDefines.h"
#include "Movement/MotionMaster.h"
#include "Spells/SpellAuras.h"
#include "Guild/Guild.h"

#include "playerbotDefs.h"
#include "playerbot/PlayerbotAIAware.h"
#include "PlayerbotMgr.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "ChatHelper.h"
#include "BroadcastHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotDbStore.h"

#define MANGOSBOT_VERSION 2

// Cross-faction readability: when AllowTwoSide.Interaction.Chat is on, bots must speak
// UNIVERSAL. The core only rewrites Common/Orcish -> Universal for REAL clients (inside
// HandleMessagechatOpcode); bot AI calls Unit::Say/Yell directly, so without this the
// opposite faction sees garbled Orcish/Common even with crossfaction chat enabled.
inline Language PlayerbotChatLanguage(Player* bot)
{
    return (bot && sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHAT))
        ? LANG_UNIVERSAL
        : ((bot && bot->GetTeam() == ALLIANCE) ? LANG_COMMON : LANG_ORCISH);
}

std::vector<std::string> split(std::string const& s, char delim);
void split(std::vector<std::string>& dest, std::string const& str, char const* delim);
#ifndef WIN32
int strcmpi(std::string s1, std::string s2);
#endif
