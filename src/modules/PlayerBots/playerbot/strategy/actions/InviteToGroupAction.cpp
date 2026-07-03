
#include "playerbot/playerbot.h"
#include "InviteToGroupAction.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/Formations.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "Guild/GuildMgr.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <set>

namespace ai
{
    namespace
    {
        struct ActiveQuestObjectiveKey
        {
            uint32 questId = 0;
            uint8 objective = 0;
            int32 entry = 0;

            bool IsValid() const { return questId && entry; }

            bool Matches(ActiveQuestObjectiveKey const& other) const
            {
                return IsValid() && other.IsValid() &&
                    questId == other.questId &&
                    objective == other.objective &&
                    entry == other.entry;
            }
        };

        ActiveQuestObjectiveKey GetActiveQuestObjectiveKey(Player* player)
        {
            if (!player || !player->GetPlayerbotAI() || !player->GetPlayerbotAI()->GetAiObjectContext())
                return {};

            TravelTarget* target = player->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (!target || !target->GetDestination())
                return {};

            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_READY &&
                target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL &&
                target->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK)
                return {};

            QuestObjectiveTravelDestination* objective = dynamic_cast<QuestObjectiveTravelDestination*>(target->GetDestination());
            if (!objective)
                return {};

            return { objective->GetQuestId(), objective->GetObjective(), objective->GetEntry() };
        }

        uint32 GetGroupSize(Player* player)
        {
            return player && player->GetGroup() ? player->GetGroup()->GetMembersCount() : 0;
        }

        bool ItemNameSuggestsCreature(ItemPrototype const* proto, CreatureInfo const* creatureInfo)
        {
            if (!proto || !creatureInfo)
                return false;

            std::string itemName = proto->Name1;
            std::string creatureName = creatureInfo->name;
            std::transform(itemName.begin(), itemName.end(), itemName.begin(), ::tolower);
            std::transform(creatureName.begin(), creatureName.end(), creatureName.begin(), ::tolower);

            return !itemName.empty() && !creatureName.empty() && itemName.find(creatureName) != std::string::npos;
        }

        bool HasSuggestedCreatureDropSource(ItemPrototype const* proto, std::list<int32> const& dropEntries)
        {
            for (int32 entry : dropEntries)
                if (entry > 0 && ItemNameSuggestsCreature(proto, sObjectMgr.GetCreatureTemplate(uint32(entry))))
                    return true;

            return false;
        }

        std::set<uint32> GetNeededQuestCreatureEntries(Player* bot)
        {
            std::set<uint32> entries;
            if (!bot)
                return entries;

            QuestStatusMap& questStatusMap = bot->getQuestStatusMap();
            for (auto const& [questId, questStatus] : questStatusMap)
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (!quest || !quest->IsActive() || questStatus.m_status != QUEST_STATUS_INCOMPLETE)
                    continue;

                for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
                {
                    if (quest->ReqCreatureOrGOCount[objective] &&
                        questStatus.m_creatureOrGOcount[objective] < quest->ReqCreatureOrGOCount[objective] &&
                        quest->ReqCreatureOrGOId[objective] > 0)
                    {
                        entries.insert(uint32(quest->ReqCreatureOrGOId[objective]));
                    }

                    if (!quest->ReqItemCount[objective] || questStatus.m_itemcount[objective] >= quest->ReqItemCount[objective])
                        continue;

                    uint32 itemId = quest->ReqItemId[objective];
                    while (itemId)
                    {
                        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
                        std::list<int32> dropEntries = GAI_VALUE2(std::list<int32>, "item drop list", itemId);
                        const bool hasSuggestedSource = HasSuggestedCreatureDropSource(proto, dropEntries);

                        for (int32 entry : dropEntries)
                        {
                            if (entry <= 0)
                                continue;

                            if (hasSuggestedSource && !ItemNameSuggestsCreature(proto, sObjectMgr.GetCreatureTemplate(uint32(entry))))
                                continue;

                            entries.insert(uint32(entry));
                        }

                        itemId = ItemUsageValue::ItemCreatedFrom(itemId);
                    }
                }
            }

            return entries;
        }

        // True if `player` has an INCOMPLETE kill-quest objective for creature `entry`.
        // Travel-state independent (scans the quest log directly), unlike
        // GetActiveQuestObjectiveKey which only resolves in READY/TRAVEL/WORK. This lets a
        // bot match a co-located player who needs the same mob even while that player is
        // idle/cooldown standing at the camp -> raises group COVERAGE at contested spawns.
        bool PlayerNeedsKillEntry(Player* player, uint32 entry)
        {
            if (!player || !entry)
                return false;

            QuestStatusMap& questStatusMap = player->getQuestStatusMap();
            for (auto const& [questId, questStatus] : questStatusMap)
            {
                if (questStatus.m_status != QUEST_STATUS_INCOMPLETE)
                    continue;

                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (!quest)
                    continue;

                for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
                {
                    if (quest->ReqCreatureOrGOId[objective] > 0 &&
                        uint32(quest->ReqCreatureOrGOId[objective]) == entry &&
                        questStatus.m_creatureOrGOcount[objective] < quest->ReqCreatureOrGOCount[objective])
                        return true;
                }
            }

            return false;
        }

        // The kill creature entry of a quest objective this bot is CAMPING right now: an
        // incomplete kill objective with an ALIVE instance within sight (the bot is parked at the
        // spawn, typically because the mob is tapped/contested). 0 if the bot isn't at a quest mob.
        // This is what lets WAITING bots form a same-kill group of up to 5 regardless of their
        // GrouperType/level cap, so a camp coalesces into groups that each work one mob = ~5x.
        // Bounded loop (<=8 objectives) + short-circuit keep the grid cost small.
        uint32 CampingQuestKillEntry(Player* bot)
        {
            if (!bot)
                return 0;

            uint32 checked = 0;
            QuestStatusMap& questStatusMap = bot->getQuestStatusMap();
            for (auto const& [questId, questStatus] : questStatusMap)
            {
                if (questStatus.m_status != QUEST_STATUS_INCOMPLETE)
                    continue;

                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (!quest)
                    continue;

                for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
                {
                    if (quest->ReqCreatureOrGOId[objective] <= 0)
                        continue;
                    if (questStatus.m_creatureOrGOcount[objective] >= quest->ReqCreatureOrGOCount[objective])
                        continue;

                    if (++checked > 8)
                        return 0;

                    uint32 entry = uint32(quest->ReqCreatureOrGOId[objective]);
                    std::list<Creature*> insts;
                    bot->GetCreatureListWithEntryInGrid(insts, entry, sPlayerbotAIConfig.sightDistance);
                    for (Creature* c : insts)
                        if (c && c->IsAlive())
                            return entry;
                }
            }
            return 0;
        }

        bool HasVisibleQuestObjectiveTarget(PlayerbotAI* ai, Player* bot)
        {
            if (!ai || !bot || !bot->IsInWorld() || !ai->GetAiObjectContext())
                return false;

            std::set<uint32> objectiveEntries = GetNeededQuestCreatureEntries(bot);
            if (objectiveEntries.empty())
                return false;

            ActiveQuestObjectiveKey activeObjective = GetActiveQuestObjectiveKey(bot);

            Value<std::list<ObjectGuid>>* possibleTargetsValue = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets");
            if (!possibleTargetsValue)
                return false;

            std::list<ObjectGuid> possibleTargets = possibleTargetsValue->Get();
            for (ObjectGuid const& guid : possibleTargets)
            {
                if (!guid.IsCreature() || !objectiveEntries.count(guid.GetEntry()))
                    continue;

                if (activeObjective.IsValid() && activeObjective.entry > 0 && int32(guid.GetEntry()) != activeObjective.entry)
                    continue;

                Unit* target = ai->GetUnit(guid);
                if (!target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId() || sServerFacade.UnitIsDead(target))
                    continue;

                if (sServerFacade.IsFriendlyTo(bot, target))
                    continue;

                if (bot->IsWithinLOSInMap(target, true))
                    return true;
            }

            return false;
        }
    }

    bool InviteToGroupAction::Invite(Player* inviter, Player* player)
    {
        if (!player)
            return false;

        if (inviter == player)
            return false;

        if (!player->GetPlayerbotAI() && !ai->GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_INVITE, true, player))
            return false;

        if (Group* group = inviter->GetGroup())
        {
            if(player->GetPlayerbotAI() && !player->GetPlayerbotAI()->IsRealPlayer())
                if (!group->IsRaidGroup() && group->GetMembersCount() > 4)
                    group->ConvertToRaid();
        }

        WorldPacket p;
        uint32 roles_mask = 0;
        p << player->GetName();
        p << roles_mask;
        inviter->GetSession()->HandleGroupInviteOpcode(p);

        return true;
    }

    bool JoinGroupAction::Execute(Event& event)
    {
        if (bot->InBattleGround())
            return false;

        if (bot->InBattleGroundQueue())
            return false;

        Player* master = event.getOwner();

        Group* group = master->GetGroup();

        if (group)
        {
            if (group->IsFull())
            {
                if(group->IsRaidGroup())
                    return false;

                if(event.getSource() == "create group")
                    group->ConvertToRaid();
                else 
                    return false;
            }

            if (bot->GetGroup() == group)
                return false;
        }

        if (bot->GetGroup())
        {
            if (ai->HasRealPlayerMaster() && master != ai->GetMaster())
                return false;

            if (!ai->DoSpecificAction("leave", event, true))
                return false;
        }

        if (bot->GetGroupInvite())
            bot->GetGroupInvite()->RemoveInvite(bot);

        bool invite = Invite(master, bot);

        if (invite && (event.getSource() == "create group"))
        {
            if (!ai->DoSpecificAction("accept invitation", event, true))
                return false;
        }

        return invite;
    }

    std::unordered_map<uint8, std::unordered_map<BotRoles, uint32>> LfgAction::AllowedClassRoleNr(uint8 groupSize)
    {
        std::unordered_map<uint8, std::unordered_map<BotRoles, uint32>> allowedClassNr;

        if (groupSize == 5)
        {
            allowedClassNr[0][BOT_ROLE_TANK] = 1;
            allowedClassNr[0][BOT_ROLE_HEALER] = 1;
            allowedClassNr[0][BOT_ROLE_DPS] = 3;
        }
        else if (groupSize == 40)
        {
            allowedClassNr[0][BOT_ROLE_TANK] = 4;
            allowedClassNr[0][BOT_ROLE_HEALER] = 16;
            allowedClassNr[0][BOT_ROLE_DPS] = 20;

            allowedClassNr[CLASS_PALADIN][BOT_ROLE_TANK] = 0;
            allowedClassNr[CLASS_DRUID][BOT_ROLE_TANK] = 1;

            allowedClassNr[CLASS_DRUID][BOT_ROLE_HEALER] = 3;
            allowedClassNr[CLASS_PALADIN][BOT_ROLE_HEALER] = 4;
            allowedClassNr[CLASS_SHAMAN][BOT_ROLE_HEALER] = 4;
            allowedClassNr[CLASS_PRIEST][BOT_ROLE_HEALER] = 11;

            allowedClassNr[CLASS_WARRIOR][BOT_ROLE_DPS] = 8;
            allowedClassNr[CLASS_PALADIN][BOT_ROLE_DPS] = 4;
            allowedClassNr[CLASS_HUNTER][BOT_ROLE_DPS] = 4;
            allowedClassNr[CLASS_ROGUE][BOT_ROLE_DPS] = 6;
            allowedClassNr[CLASS_PRIEST][BOT_ROLE_DPS] = 1;
            allowedClassNr[CLASS_SHAMAN][BOT_ROLE_DPS] = 4;
            allowedClassNr[CLASS_MAGE][BOT_ROLE_DPS] = 15;
            allowedClassNr[CLASS_WARLOCK][BOT_ROLE_DPS] = 4;
            allowedClassNr[CLASS_DRUID][BOT_ROLE_DPS] = 1;
        }
        else if (groupSize == 25)
        {
            allowedClassNr[0][BOT_ROLE_TANK] = 3;
            allowedClassNr[0][BOT_ROLE_HEALER] = 7;
            allowedClassNr[0][BOT_ROLE_DPS] = 15;
        }
        else if (groupSize == 20)
        {
            allowedClassNr[0][BOT_ROLE_TANK] = 2;
            allowedClassNr[0][BOT_ROLE_HEALER] = 5;
            allowedClassNr[0][BOT_ROLE_DPS] = 13;
        }
        else if (groupSize == 10)
        {
            allowedClassNr[0][BOT_ROLE_TANK] = 2;
            allowedClassNr[0][BOT_ROLE_HEALER] = 3;
            allowedClassNr[0][BOT_ROLE_DPS] = 5;
        }
        else
        {
            allowedClassNr[0][BOT_ROLE_TANK] = groupSize;
            allowedClassNr[0][BOT_ROLE_HEALER] = groupSize;
            allowedClassNr[0][BOT_ROLE_DPS] = groupSize;
        }
        return allowedClassNr;
    }

    std::unordered_map<uint8, std::unordered_map<BotRoles, uint32>> LfgAction::AllowedClassRoleNr(Player* player, uint8 groupSize)
    {
        std::unordered_map<uint8, std::unordered_map<BotRoles, uint32>> allowedClassNr = AllowedClassRoleNr(groupSize);

        Group* group = player->GetGroup();

        if (!group)
        {
            BotRoles role = PlayerbotAI::IsTank(player, false) ? BOT_ROLE_TANK : PlayerbotAI::IsHeal(player, false) ? BOT_ROLE_HEALER :
                                                                                                                      BOT_ROLE_DPS;
            uint8 cls = (Classes)player->getClass();

            if (allowedClassNr[0][role] > 0)
                allowedClassNr[0][role]--;

            if (allowedClassNr[cls].find(role) != allowedClassNr[cls].end() && allowedClassNr[cls][role] > 0)
                allowedClassNr[cls][role]--;

            return allowedClassNr;
        }

        Player* groupMaster = sObjectMgr.GetPlayer(group->GetLeaderGuid());

        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            // Only add group member targets that are alive and near the player
            Player* player = sObjectMgr.GetPlayer(itr->guid);

            if (!PlayerbotAI::IsSafe(groupMaster, player))
                continue;

            BotRoles role = PlayerbotAI::IsTank(player, false) ? BOT_ROLE_TANK : PlayerbotAI::IsHeal(player, false) ? BOT_ROLE_HEALER : BOT_ROLE_DPS;
            uint8 cls = (Classes)player->getClass();

            if (allowedClassNr[0][role] > 0)
                allowedClassNr[0][role]--;

            if (allowedClassNr[cls].find(role) != allowedClassNr[cls].end() && allowedClassNr[cls][role] > 0)
                allowedClassNr[cls][role]--;
        }

        return allowedClassNr;
    }

    bool LfgAction::Execute(Event& event)
    {
        Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
        if (bot->InBattleGround())
            return false;

        if (bot->InBattleGroundQueue())
            return false;

        if (!ai->IsSafe(requester))
            return false;

        if (requester->GetLevel() == DEFAULT_MAX_LEVEL && bot->GetLevel() != DEFAULT_MAX_LEVEL)
            return false;

        if (requester->GetLevel() > bot->GetLevel() + 4 || bot->GetLevel() > requester->GetLevel() + 4)
            return false;

        std::string param = event.getParam();

        if (!param.empty() && param != "40" && param != "25" && param != "20" && param != "10" && param != "5")
        {
            ai->TellError(requester, BOT_TEXT2("Unknown group size. Valid sizes for lfg are 40, 25, 20, 10 and 5.", {}));
            return false;
        }

        Group* group = requester->GetGroup();

        std::unordered_map<uint8, std::unordered_map<BotRoles, uint32>> allowedClassNr = AllowedClassRoleNr(5);

        BotRoles role = ai->IsTank(requester, false) ? BOT_ROLE_TANK : (ai->IsHeal(requester, false) ? BOT_ROLE_HEALER : BOT_ROLE_DPS);
        Classes cls = (Classes)requester->getClass();

        if (!Qualified::isValidNumberString(param))
           param = "5";

        if (group)
        {
            //If no input use max raid for raid groups.
            if (param.empty() && group->IsRaidGroup())
#ifdef MANGOSBOT_ZERO
                param = "40";
#else
                /// Default to TBC Raiding. Max size 25
                param = "25";
#endif

            if (group->IsFull())
            {
                if (param.empty() || param == "5" || group->IsRaidGroup())
                    return false; //Group or raid is full so stop trying.
                else
                    group->ConvertToRaid(); //We want a raid but are in a group so convert and continue.
            }
        }

        allowedClassNr = AllowedClassRoleNr(requester, stoi(param));  

        role = ai->IsTank(bot, false) ? BOT_ROLE_TANK : (ai->IsHeal(bot, false) ? BOT_ROLE_HEALER : BOT_ROLE_DPS);
        cls = (Classes)bot->getClass();

        if (allowedClassNr[0][role] == 0)
            return false;

        if (allowedClassNr[cls].find(role) != allowedClassNr[cls].end() && allowedClassNr[cls][role] == 0)
            return false;        

        if (bot->GetGroup())
        {
            if (ai->HasRealPlayerMaster())
                return false;

            if (!ai->DoSpecificAction("leave", event, true))
                return false;
        }

        bool invite = Invite(requester, bot);

        if (invite)
        {
            if (!ai->DoSpecificAction("accept invitation", event, true))
                return false;

            std::map<std::string, std::string> placeholders;
            placeholders["%role"] = (role == BOT_ROLE_TANK ? "tank" : (role == BOT_ROLE_HEALER ? "healer" : "dps"));
            placeholders["%spotsleft"] = std::to_string(allowedClassNr[0][role] - 1);

            if (allowedClassNr[0][role] > 1)
                ai->TellPlayer(requester, BOT_TEXT2("Joining as %role, %spotsleft %role spots left.", placeholders));
            else
                ai->TellPlayer(requester, BOT_TEXT2("Joining as %role.", placeholders));

            return true;
        }

        return false;
    }

    bool InviteNearbyToGroupAction::Execute(Event& event)
    {
        if (!bot->GetGroup())  //Select a random formation to copy.
        {
            std::vector<std::string> formations = { "melee","queue","chaos","circle","line","shield","arrow","near","far"};
            FormationValue* value = (FormationValue*)context->GetValue<Formation*>("formation");
            std::string newFormation = formations[urand(0, formations.size() - 1)];
            value->Load(newFormation);
        }

        ActiveQuestObjectiveKey botObjective = GetActiveQuestObjectiveKey(bot);
        // The kill mob we're camping (quest-log based, works even when idle/cooldown with no
        // active-objective key) -> used to match co-located bots who need the SAME mob.
        const uint32 campEntry = CampingQuestKillEntry(bot);
        Player* bestPlayer = nullptr;
        int32 bestScore = std::numeric_limits<int32>::min();
        float bestDistance = std::numeric_limits<float>::max();

        std::list<ObjectGuid> nearGuids = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("nearest friendly players")->Get();
        for (auto& i : nearGuids)
        {
            Player* player = sObjectMgr.GetPlayer(i);

            if (!player)
                continue;

            if (player == bot)
                continue;

            if (player->GetMapId() != bot->GetMapId())
                continue;

#ifdef MANGOSBOT_TWO
            if (player->InSamePhase(bot->GetPhaseMask()))
                continue;
#endif

            Group* playerGroup = player->GetGroup();
            if (player->GetGroup())
                continue;

            if (!sPlayerbotAIConfig.randomBotInvitePlayer && player->isRealPlayer())
                continue;

            Group* group = bot->GetGroup();

            if (player->isDND())
                continue;

            if (player->IsBeingTeleported())
                continue;

            PlayerbotAI* botAi = player->GetPlayerbotAI();
            ActiveQuestObjectiveKey playerObjective = GetActiveQuestObjectiveKey(player);
            // Match on the strict active-objective key OR (coverage) on the target simply
            // NEEDING the same kill mob, even if either bot is idle/cooldown at the camp with no
            // active travel-target key. campEntry covers the waiting-at-a-tapped-mob case (the
            // inviter has no valid objective key but is standing at a quest spawn it needs).
            const bool sameQuestObjective = botObjective.Matches(playerObjective) ||
                (botObjective.IsValid() && botObjective.entry > 0 &&
                    PlayerNeedsKillEntry(player, uint32(botObjective.entry))) ||
                (campEntry > 0 && PlayerNeedsKillEntry(player, campEntry));

            // Only hard-require a same objective when WE have a concrete one (valid key or a camp
            // mob); otherwise fall through to the generic proximity grouping below.
            if (((botObjective.IsValid() && botObjective.entry > 0) || campEntry > 0) && !sameQuestObjective)
                continue;

            if (botAi)
            {
                if (botAi->GetGrouperType() == GrouperType::SOLO && !botAi->HasRealPlayerMaster() && !sameQuestObjective) //Do not invite solo players, unless we are camping the same objective.
                    continue;

                if (botAi->HasActivePlayerMaster()) //Do not invite alts of active players. 
                    continue;
            }

            if (abs(int32(player->GetLevel() - bot->GetLevel())) > (sameQuestObjective ? 5 : 2))
                continue;

            const float distance = sServerFacade.GetDistance2d(bot, player);
            if (distance > (sameQuestObjective ? sPlayerbotAIConfig.sightDistance : sPlayerbotAIConfig.spellDistance))
                continue;

            int32 score = sameQuestObjective ? 10000 : 0;
            score -= int32(distance);
            score -= abs(int32(player->GetLevel() - bot->GetLevel())) * 25;
            if (botAi && botAi->GetGrouperType() == GrouperType::MEMBER)
                score += 100;
            if (!playerGroup)
                score += 50;

            if (!bestPlayer || score > bestScore || (score == bestScore && distance < bestDistance))
            {
                bestPlayer = player;
                bestScore = score;
                bestDistance = distance;
            }
        }

        if (bestPlayer)
        {
            Group* group = bot->GetGroup();

            //When inviting the 5th member of the group convert to raid for future invites.
            if (group && !botObjective.IsValid() && ai->GetGrouperType() > GrouperType::LEADER_5 && !group->IsRaidGroup() && bot->GetGroup()->GetMembersCount() > 3)
                group->ConvertToRaid();

            Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
            if (sPlayerbotAIConfig.inviteChat && (sRandomPlayerbotMgr.IsFreeBot(bot) || !ai->HasActivePlayerMaster()))
            {
                if (guild && bestPlayer && bot->IsInGuild(bestPlayer->GetGuildId()))
                {
                    BroadcastHelper::BroadcastGuildGroupOrRaidInvite(
                        ai,
                        bot,
                        bestPlayer,
                        group,
                        guild
                    );
                }
                else
                {

                    std::map<std::string, std::string> placeholders;
                    placeholders["%player"] = bestPlayer->GetName();

                    // Quest-directed invite: if the inviter is on a kill quest, name the mob and
                    // quest so the invite reads like real coordination ("need X for Y") instead of
                    // a generic "wanna group?". botObjective is the inviter's active kill objective.
                    bool questDirected = false;
                    if (botObjective.IsValid() && botObjective.entry > 0)
                    {
                        CreatureInfo const* ci = sObjectMgr.GetCreatureTemplate(uint32(botObjective.entry));
                        Quest const* q = sObjectMgr.GetQuestTemplate(botObjective.questId);
                        if (ci && q)
                        {
                            placeholders["%mob"] = ci->name;
                            placeholders["%quest"] = q->GetTitle();
                            questDirected = true;
                        }
                    }

                    const char* textKey = (group && group->IsRaidGroup()) ? "join_raid"
                                        : (questDirected ? "join_group_quest" : "join_group");
                    bot->Say(BOT_TEXT2(textKey, placeholders), (bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH));
                }
            }

            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                std::ostringstream out;
                ActiveQuestObjectiveKey targetObjective = GetActiveQuestObjectiveKey(bestPlayer);
                out << "target=" << bestPlayer->GetName()
                    << " dist=" << std::fixed << std::setprecision(2) << bestDistance
                    << " score=" << bestScore
                    << " sameObjective=" << (botObjective.Matches(targetObjective) ? 1 : 0)
                    << " questId=" << targetObjective.questId
                    << " objective=" << uint32(targetObjective.objective)
                    << " entry=" << targetObjective.entry
                    << " botGroupSize=" << GetGroupSize(bot)
                    << " targetGroupSize=" << GetGroupSize(bestPlayer);
                sPlayerbotAIConfig.logEvent(ai, "InviteNearbySelected", bestPlayer->GetName(), out.str());
            }

            return Invite(bot, bestPlayer);
        }

        return false;
    }

    bool InviteNearbyToGroupAction::isUseful()
    {
        if (!sPlayerbotAIConfig.randomBotGroupNearby)
            return false;

        if (bot->InBattleGround())
            return false;

        if (bot->InBattleGroundQueue())
            return false;

        if (HasVisibleQuestObjectiveTarget(ai, bot))
            return false;

        GrouperType grouperType = ai->GetGrouperType();

        ActiveQuestObjectiveKey botObjective = GetActiveQuestObjectiveKey(bot);

        // A bot parked at a contested quest mob always tries to build a same-kill group of up to 5
        // (overriding its GrouperType/level cap and the SOLO/MEMBER "don't lead" rule). This runs
        // ALONGSIDE the central manager: keeping per-bot invites gives the best total throughput
        // (measured credits_min ~10 vs ~5 when deferring entirely to the manager).
        const uint32 campEntry = CampingQuestKillEntry(bot);
        const bool questCamping = campEntry > 0;

        if (!questCamping &&
            (grouperType == GrouperType::SOLO || (grouperType == GrouperType::MEMBER && !botObjective.IsValid())))
            return false;

        Group* group = bot->GetGroup();

        if (group)
        {
            if (group->IsRaidGroup() && group->IsFull())
                return false;

            if (ai->GetGroupMaster() != bot)
                return false;

            uint32 memberCount = group->GetMembersCount();

            // Cap = 5 when camping a quest kill or working a real objective; else GrouperType pref.
            const uint32 cap = (questCamping || botObjective.IsValid()) ? 5u : uint32(uint8(grouperType));
            if (memberCount >= cap)
                return false;
        }

        if (ai->HasActivePlayerMaster()) //Alts do not invite randomly
           return false;

        return true;
    }

    std::vector<Player*> InviteGuildToGroupAction::getGuildMembers()
    {
        Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());

        FindGuildMembers worker;
        guild->BroadcastWorker(worker);

        return worker.GetResult();
    }

    bool InviteGuildToGroupAction::Execute(Event& event)
    {
        Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());

        for (auto& member : getGuildMembers())
        {
            Player* player = member;

            if (!player)
                continue;

            if (player == bot)
                continue;

            if (player->GetGroup())
                continue;

            if (player->isDND())
                continue;

            if (!sPlayerbotAIConfig.randomBotInvitePlayer && player->isRealPlayer())
                continue;

            if (player->IsBeingTeleported())
                continue;

            if (player->GetMapId() != bot->GetMapId() && player->GetLevel() < 30)
                continue;

#ifdef MANGOSBOT_TWO
            if (player->GetMapId() == 609 && player->GetMapId() != bot->GetMapId())
                continue;
#endif

            if (WorldPosition(player).distance(bot) > 1000 && player->GetLevel() < 15)
                continue;

            PlayerbotAI* playerAi = player->GetPlayerbotAI();

            if (playerAi)
            {
                if (playerAi->GetGrouperType() == GrouperType::SOLO && !playerAi->HasRealPlayerMaster()) //Do not invite solo players.
                    continue;

                if (playerAi->HasActivePlayerMaster()) //Do not invite alts of active players.
                    continue;

                if (player->GetLevel() > bot->GetLevel() + 5) //Invite higher levels that need money so they can grind money and help out.
                {
                    if (!ai->IsSafe(player) || !PAI_VALUE(bool, "should get money"))
                        continue;
                }
            }

            if (bot->GetLevel() > player->GetLevel() + 5) //Do not invite members that too low level or risk dragging them to deadly places.
                continue;

            if (!playerAi && sServerFacade.GetDistance2d(bot, player) > sPlayerbotAIConfig.sightDistance)
                continue;

            Group* group = bot->GetGroup();
            //When inviting the 5th member of the group convert to raid for future invites.
            if (group && ai->GetGrouperType() > GrouperType::LEADER_5 && !group->IsRaidGroup() && bot->GetGroup()->GetMembersCount() > 3)
            {
                group->ConvertToRaid();
            }

            if (sPlayerbotAIConfig.inviteChat && (sRandomPlayerbotMgr.IsFreeBot(bot) || !ai->HasActivePlayerMaster()))
            {
                BroadcastHelper::BroadcastGuildGroupOrRaidInvite(
                    ai,
                    bot,
                    player,
                    group,
                    guild
                );
            }

            return Invite(bot, player);
        }

        return false;
    }
}
