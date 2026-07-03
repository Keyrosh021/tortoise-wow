
#include "playerbot/playerbot.h"
#include "LeaveGroupAction.h"
#include <set>

namespace ai
{
    namespace
    {
        // Set of creature entries this player still needs to KILL for incomplete quests.
        std::set<uint32> NeededKillEntries(Player* p)
        {
            std::set<uint32> entries;
            if (!p)
                return entries;

            QuestStatusMap& questStatusMap = p->getQuestStatusMap();
            for (auto const& kv : questStatusMap)
            {
                const QuestStatusData& qs = kv.second;
                if (qs.m_status != QUEST_STATUS_INCOMPLETE)
                    continue;

                Quest const* quest = sObjectMgr.GetQuestTemplate(kv.first);
                if (!quest)
                    continue;

                for (uint32 o = 0; o < QUEST_OBJECTIVES_COUNT; ++o)
                    if (quest->ReqCreatureOrGOId[o] > 0 &&
                        qs.m_creatureOrGOcount[o] < quest->ReqCreatureOrGOCount[o])
                        entries.insert(uint32(quest->ReqCreatureOrGOId[o]));
            }

            return entries;
        }
    }

	bool LeaveGroupAction::Leave(Player* player) 
    {
        if (!player)
            return false;

        Group* group = bot->GetGroup();

        if (ai->HasActivePlayerMaster() && player != bot && player != ai->GetMaster() && player->GetSession() && player->GetSession()->GetSecurity() < SEC_MODERATOR)
            return false;

        bool aiMaster = (ai->GetMaster() && ai->GetMaster()->GetPlayerbotAI());

        ai->TellPlayer(player, BOT_TEXT("goodbye"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_TALK, false);

        bool freeBot = sRandomPlayerbotMgr.IsFreeBot(bot);

        bool shouldStay = freeBot && bot->GetGroup() && player == bot;

        if (!shouldStay)
        {
            if (group)
                sPlayerbotAIConfig.logEvent(ai, "LeaveGroupAction", group->GetLeaderName(), std::to_string(group->GetMembersCount()-1));

            WorldPacket p;
            std::string member = bot->GetName();
            p << uint32(PARTY_OP_LEAVE) << member << uint32(0);
            bot->GetSession()->HandleGroupDisbandOpcode(p);
            if (ai->HasRealPlayerMaster() && ai->GetMaster()->GetObjectGuid() != player->GetObjectGuid())
                bot->Whisper("I left my group", LANG_UNIVERSAL, player->GetObjectGuid());
        }

        if (freeBot)
        {
            bot->GetPlayerbotAI()->SetMaster(nullptr);
        }        

        if(!aiMaster)
            ai->ResetStrategies();
        ai->Reset();

        return true;
	}

    bool LeaveFarAwayAction::isUseful()
    {
        if (bot->InBattleGround())
            return false;

        if (bot->InBattleGroundQueue())
            return false;

        if (!bot->GetGroup())
            return false;      

        Player* groupMaster = ai->GetGroupMaster();

        if (!groupMaster)
            return false;

        if (ai->HasActivePlayerMaster())
            return false;

        for (GroupReference* gref = bot->GetGroup()->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();
            if (!ai->IsSafe(member))
                return false;
        }

        // SEAT EFFICIENCY (random/free bots only, never a real player's group): free the
        // group seat the instant this member stops benefiting the pack, so it goes to a
        // co-located same-objective bot instead. Two fast cases:
        //   (1) drifted out of the 74yd group-credit radius (+buffer) from the leader -> it
        //       receives no shared kill credit there, the seat is wasted.
        //   (2) it and the leader are BOTH on kill quests but share NO target mob -> they no
        //       longer help each other; drop so the leader's kills credit a same-goal bot.
        if (!ai->HasRealPlayerMaster() && groupMaster != bot)
        {
            // 90yd: just beyond the 75yd camp-group clustering radius, so co-located members aren't
            // evicted, but a genuinely-departed member frees its seat.
            if (groupMaster->GetMapId() == bot->GetMapId() &&
                sServerFacade.GetDistance2d(bot, groupMaster) > 90.0f)
                return true;

            std::set<uint32> myGoals = NeededKillEntries(bot);
            std::set<uint32> leaderGoals = NeededKillEntries(groupMaster);
            if (!myGoals.empty() && !leaderGoals.empty())
            {
                bool shares = false;
                for (uint32 e : myGoals)
                    if (leaderGoals.count(e)) { shares = true; break; }
                if (!shares)
                    return true;
            }
        }

        if (!sPlayerbotAIConfig.randomBotGroupNearby)
            return true;

        if (ai->GetGrouperType() == GrouperType::SOLO)
            return true;

        uint32 dCount = AI_VALUE(uint32, "death count");

        if (ai->HasRealPlayerMaster() && !sRandomPlayerbotMgr.IsRandomBot(bot))
            return false;

        if (dCount > 9)
            return true;

        if (dCount > 4 && !ai->HasRealPlayerMaster())
            return true;

        if (bot->GetGuildId() == groupMaster->GetGuildId())
        {
            if (bot->GetLevel() > groupMaster->GetLevel() + 5)
            {
                if(AI_VALUE(bool, "should get money"))
                    return false;
            }
        }

        if (abs(int32(groupMaster->GetLevel() - bot->GetLevel())) > 4)
            return true;

        if (MEM_AI_VALUE(uint32, "experience")->LastChangeDelay() > 15 * MINUTE && MEM_AI_VALUE(uint32, "honor")->LastChangeDelay() > 15 * MINUTE)
            return true;

        return false;
    }
}