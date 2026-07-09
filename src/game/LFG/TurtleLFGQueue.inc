// TurtleLFG queue system — implements the LFT addon's queue-from-anywhere dungeon finder
// (protocol reverse-engineered from Interface/FrameXML/LFT/LFT.lua). Included from
// TurtleLFGMgr.cpp. Matchmaking runs on the world thread (Update); addon messages arrive on
// session threads and are guarded by m_queueMutex.
//
// Design notes:
// - Bots never see the addon: their offer slots are pre-accepted, they are invited and
//   teleported entirely server-side (Playerbot_* host hooks).
// - The server owns the 90s role-check and offer timers (the client shows but never enforces).
// - S2C_ROLECHECK_START must carry instance codes: an empty list flips the addon into its
//   browse-tab peer-to-peer flow and the queue hangs.

#include "GameEventMgr.h"
#include "Maps/MapManager.h"
#include "ObjectMgr.h"

namespace
{
    struct LfgDungeonSeed { char const* code; uint32 map; uint32 minLevel; uint32 maxLevel; };
    // codes + level bands mirror LFT_Data.lua (patch-9 client); maps resolved from
    // areatrigger_teleport (Turtle customs) and classic instance list.
    LfgDungeonSeed const kLfgDungeonSeeds[] = {
        { "rfc", 389, 13, 19 }, { "fmh", 822, 13, 20 }, { "wc", 43, 16, 25 },
        { "dm", 36, 16, 24 }, { "sfk", 33, 20, 28 }, { "stocks", 34, 23, 32 },
        { "bfd", 48, 22, 31 }, { "windhorn", 820, 25, 33 }, { "dmr", 816, 26, 35 },
        { "gnomer", 90, 28, 37 }, { "rfk", 47, 29, 36 }, { "smgy", 189, 30, 37 },
        { "smlib", 189, 32, 40 }, { "swc", 818, 32, 40 }, { "tcg", 802, 33, 39 },
        { "smarmory", 189, 34, 42 }, { "smcath", 189, 35, 45 }, { "rfd", 129, 35, 44 },
        { "swd", 818, 35, 44 }, { "ulda", 70, 41, 50 }, { "zf", 209, 42, 51 },
        { "gilneas", 815, 43, 52 }, { "maraorange", 349, 43, 51 }, { "marapurple", 349, 45, 52 },
        { "maraprincess", 349, 46, 54 }, { "st", 109, 49, 58 }, { "brdarena", 230, 50, 60 },
        { "hfq", 808, 51, 60 }, { "brd", 230, 54, 60 }, { "brdemp", 230, 54, 60 },
        { "lbrs", 229, 55, 60 }, { "dme", 429, 55, 60 }, { "dmn", 429, 57, 60 },
        { "dmt", 429, 57, 60 }, { "dmw", 429, 57, 60 }, { "scholo", 289, 58, 60 },
        { "stratud", 329, 58, 60 }, { "stratlive", 329, 58, 60 }, { "kc", 800, 60, 60 },
        { "cotbm", 269, 60, 60 }, { "swv", 35, 60, 60 },
    };

    Player* FindQueuePlayer(ObjectGuid guid)
    {
        Player* player = sObjectAccessor.FindPlayer(guid);
        return (player && player->IsInWorld()) ? player : nullptr;
    }
}

void TurtleLFGMgr::LoadDungeons()
{
    uint32 loaded = 0;
    for (LfgDungeonSeed const& seed : kLfgDungeonSeeds)
    {
        DungeonInfo info;
        info.code = seed.code;
        info.map = seed.map;
        info.minLevel = seed.minLevel;
        info.maxLevel = seed.maxLevel;

        if (AreaTriggerTeleport const* at = sObjectMgr.GetMapEntranceTrigger(seed.map))
        {
            info.x = at->destination.coord_x;
            info.y = at->destination.coord_y;
            info.z = at->destination.coord_z;
            info.o = at->destination.orientation;
            info.map = at->destination.mapid;
            info.valid = true;
            ++loaded;
        }
        else
            sLog.outErrorDb("TurtleLFG: no entrance areatrigger for dungeon '%s' (map %u) — code disabled", seed.code, seed.map);

        m_dungeons[info.code] = info;
    }
    sLog.outString("TurtleLFG: loaded %u/%u queue dungeons", loaded, (uint32)(sizeof(kLfgDungeonSeeds) / sizeof(kLfgDungeonSeeds[0])));
}

uint8 TurtleLFGMgr::RolesMaskFromString(const std::string& roles)
{
    uint8 mask = 0;
    for (std::string const& r : Split(roles, kArrayDelimiter))
    {
        if (r == "t") mask |= ROLE_TANK;
        else if (r == "h") mask |= ROLE_HEALER;
        else if (r == "d") mask |= ROLE_DAMAGE;
    }
    return mask;
}

std::string TurtleLFGMgr::RoleCode(uint8 role)
{
    if (role & ROLE_TANK) return "t";
    if (role & ROLE_HEALER) return "h";
    return "d";
}

bool TurtleLFGMgr::ClassCanUseRoles(uint8 classId, uint8 mask)
{
    // mirror of LFT_ClassRoles (tank, healer, damage)
    uint8 allowed = 0;
    switch (classId)
    {
        case CLASS_DRUID:
        case CLASS_PALADIN:
        case CLASS_SHAMAN:  allowed = ROLE_TANK | ROLE_HEALER | ROLE_DAMAGE; break;
        case CLASS_PRIEST:  allowed = ROLE_HEALER | ROLE_DAMAGE; break;
        case CLASS_WARRIOR: allowed = ROLE_TANK | ROLE_DAMAGE; break;
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_ROGUE:
        case CLASS_WARLOCK: allowed = ROLE_DAMAGE; break;
        default: return false;
    }
    return mask != 0 && (mask & ~allowed) == 0;
}

// ---------------- senders ----------------

void TurtleLFGMgr::SendQueueJoined(Player* player) const
{
    if (player) player->SendAddonMessage(kPrefix, "S2C_QUEUE_JOINED");
}

void TurtleLFGMgr::SendQueueStatus(Player* player, const std::vector<std::string>& codes) const
{
    if (!player) return;
    std::string list;
    for (std::string const& c : codes) { if (!list.empty()) list += kArrayDelimiter; list += c; }
    player->SendAddonMessage(kPrefix, "S2C_UPDATE_QUEUE_STATUS;queued;" + list);
}

void TurtleLFGMgr::SendRolecheckStart(Player* player, const std::vector<std::string>& codes) const
{
    if (!player) return;
    std::string list;
    for (std::string const& c : codes) { if (!list.empty()) list += kArrayDelimiter; list += c; }
    player->SendAddonMessage(kPrefix, "S2C_ROLECHECK_START;" + list);
}

void TurtleLFGMgr::SendRolecheckInfo(Player* toPlayer, const std::string& name, uint8 roles) const
{
    if (!toPlayer) return;
    std::string r;
    if (roles & ROLE_TANK)   { r += "t"; }
    if (roles & ROLE_HEALER) { if (!r.empty()) r += kArrayDelimiter; r += "h"; }
    if (roles & ROLE_DAMAGE) { if (!r.empty()) r += kArrayDelimiter; r += "d"; }
    toPlayer->SendAddonMessage(kPrefix, "S2C_ROLECHECK_INFO;" + name + ";" + r);
}

void TurtleLFGMgr::SendOfferNew(Player* player, const std::string& code, uint8 role) const
{
    if (player) player->SendAddonMessage(kPrefix, "S2C_OFFER_NEW;" + code + ";" + RoleCode(role));
}

void TurtleLFGMgr::SendOfferUpdate(const Offer& offer) const
{
    uint32 tank = 0, heal = 0, dps = 0;
    for (Offer::Slot const& slot : offer.slots)
    {
        if (!slot.accepted) continue;
        if (slot.assignedRole & ROLE_TANK) tank = 1;
        else if (slot.assignedRole & ROLE_HEALER) heal = 1;
        else ++dps;
    }
    std::string msg = "S2C_OFFER_UPDATE_COUNT;" + std::to_string(tank) + ":" + std::to_string(heal) + ":" + std::to_string(dps);
    for (Offer::Slot const& slot : offer.slots)
        if (!slot.isBot && slot.accepted)
            if (Player* p = FindQueuePlayer(slot.guid))
                p->SendAddonMessage(kPrefix, msg);
}

void TurtleLFGMgr::SendOfferComplete(Player* player) const
{
    if (player) player->SendAddonMessage(kPrefix, "S2C_OFFER_COMPLETE");
}

// ---------------- handlers ----------------

bool TurtleLFGMgr::HandleQueueJoinPayload(Player* player, const std::string& message)
{
    if (!player)
        return true;

    if (player->IsHardcore())
    {
        SendQueueError(player, "hardcore");
        return true;
    }

    std::vector<std::string> parts = Split(message, kFieldDelimiter, 3);
    if (parts.size() < 3)
    {
        SendQueueError(player, "invalid");
        return true;
    }

    std::vector<std::string> codes = Split(parts[1], kArrayDelimiter);
    uint8 roles = RolesMaskFromString(parts[2]);

    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (m_lfgQueue.count(player->GetObjectGuid()))
    {
        // re-join replaces the previous selection
        m_lfgQueue.erase(player->GetObjectGuid());
    }

    if (codes.empty() || roles == 0)
    {
        SendQueueError(player, "invalid");
        return true;
    }

    if (!ClassCanUseRoles(player->getClass(), roles))
    {
        SendQueueError(player, "requirements_role");
        return true;
    }

    Group* group = player->GetGroup();
    if (group && group->IsRaidGroup())
    {
        SendQueueError(player, "invalid");
        return true;
    }
    if (group && !group->IsLeader(player->GetObjectGuid()))
    {
        SendQueueError(player, "not_leader");
        return true;
    }

    // validate codes + level (party rule: every member must fit the band)
    uint32 groupMin = player->GetLevel(), groupMax = player->GetLevel();
    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld())
            {
                SendQueueError(player, "member_offline");
                return true;
            }
            groupMin = std::min(groupMin, member->GetLevel());
            groupMax = std::max(groupMax, member->GetLevel());
        }
        if (group->GetMembersCount() >= 5)
        {
            SendQueueError(player, "already_in_team");
            return true;
        }
    }

    for (std::string const& code : codes)
    {
        auto it = m_dungeons.find(code);
        if (it == m_dungeons.end() || !it->second.valid)
        {
            SendQueueError(player, "invalid");
            return true;
        }
        if (groupMin < it->second.minLevel || groupMin < 13)
        {
            SendQueueError(player, "requirements_level");
            return true;
        }
    }

    if (group)
    {
        // party path: role-check the other members first
        PendingRoleCheck rc;
        rc.leader = player->GetObjectGuid();
        rc.leaderRoles = roles;
        rc.dungeons = codes;
        rc.deadline = time(nullptr) + ROLECHECK_TIMEOUT;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == player) continue;
            rc.responses[member->GetObjectGuid()] = 0;
            SendRolecheckStart(member, codes);
        }
        if (rc.responses.empty())
        {
            // "party" of one (members offline filtered earlier) — treat as solo below
        }
        else
        {
            m_roleChecks[group->GetId()] = rc;
            player->SendAddonMessage(kPrefix, "S2C_UPDATE_QUEUE_STATUS;pending");
            return true;
        }
    }

    QueuedPlayer qp;
    qp.guid = player->GetObjectGuid();
    qp.roles = roles;
    qp.dungeons = codes;
    qp.joinTime = time(nullptr);
    m_lfgQueue[qp.guid] = qp;

    SendQueueJoined(player);
    SendQueueStatus(player, codes);
    return true;
}

bool TurtleLFGMgr::HandleRolecheckResponsePayload(Player* player, const std::string& message)
{
    if (!player)
        return true;

    std::vector<std::string> parts = Split(message, kFieldDelimiter, 2);
    uint8 roles = parts.size() >= 2 ? RolesMaskFromString(parts[1]) : 0;

    std::lock_guard<std::mutex> lock(m_queueMutex);

    Group* group = player->GetGroup();
    if (!group)
        return true;
    auto it = m_roleChecks.find(group->GetId());
    if (it == m_roleChecks.end())
        return true;

    PendingRoleCheck& rc = it->second;
    auto rit = rc.responses.find(player->GetObjectGuid());
    if (rit == rc.responses.end())
        return true;

    if (roles == 0 || !ClassCanUseRoles(player->getClass(), roles))
    {
        SendQueueError(player, "requirements_role");
        AbortRoleCheck(it->first, rc);
        m_roleChecks.erase(it);
        return true;
    }

    rit->second = roles;

    // broadcast the pick to the party
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->getSource())
            SendRolecheckInfo(member, player->GetName(), roles);

    bool allAnswered = true;
    for (auto const& kv : rc.responses)
        if (!kv.second) { allAnswered = false; break; }

    if (allAnswered)
    {
        FinishRoleCheck(it->first, rc);
        m_roleChecks.erase(it);
    }
    return true;
}

void TurtleLFGMgr::FinishRoleCheck(uint32 /*groupId*/, PendingRoleCheck& rc)
{
    time_t now = time(nullptr);
    auto enqueue = [&](ObjectGuid guid, uint8 roles)
    {
        QueuedPlayer qp;
        qp.guid = guid;
        qp.roles = roles;
        qp.dungeons = rc.dungeons;
        qp.joinTime = now;
        m_lfgQueue[guid] = qp;
        if (Player* p = FindQueuePlayer(guid))
        {
            SendQueueJoined(p);
            SendQueueStatus(p, rc.dungeons);
        }
    };
    enqueue(rc.leader, rc.leaderRoles);
    for (auto const& kv : rc.responses)
        enqueue(kv.first, kv.second);
}

void TurtleLFGMgr::AbortRoleCheck(uint32 /*groupId*/, PendingRoleCheck& rc)
{
    if (Player* leader = FindQueuePlayer(rc.leader))
        SendQueueLeft(leader);
    for (auto const& kv : rc.responses)
        if (Player* member = FindQueuePlayer(kv.first))
            SendQueueLeft(member);
}

bool TurtleLFGMgr::HandleQueueLeave(Player* player)
{
    if (!player)
        return true;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    RemoveFromQueue(player, true);
    return true;
}

void TurtleLFGMgr::RemoveFromQueue(Player* player, bool notify)
{
    ObjectGuid guid = player->GetObjectGuid();

    // pending role check (as leader or member) aborts the whole check
    if (Group* group = player->GetGroup())
    {
        auto rcIt = m_roleChecks.find(group->GetId());
        if (rcIt != m_roleChecks.end() &&
            (rcIt->second.leader == guid || rcIt->second.responses.count(guid)))
        {
            AbortRoleCheck(rcIt->first, rcIt->second);
            m_roleChecks.erase(rcIt);
            return;
        }
    }

    auto it = m_lfgQueue.find(guid);
    if (it == m_lfgQueue.end())
    {
        if (notify) SendQueueLeft(player);
        return;
    }

    if (it->second.state == QueueState::Offered)
    {
        auto oit = m_offers.find(it->second.offerId);
        if (oit != m_offers.end())
        {
            CancelOffer(oit->second, guid);   // erases decliner + requeues the rest
            m_offers.erase(oit);
            return;
        }
    }

    m_lfgQueue.erase(it);
    if (notify) SendQueueLeft(player);
}

bool TurtleLFGMgr::HandleQueueStatus(Player* player)
{
    if (!player)
        return true;
    if (player->IsHardcore())
    {
        SendQueueError(player, "hardcore");
        return true;
    }

    std::lock_guard<std::mutex> lock(m_queueMutex);
    auto it = m_lfgQueue.find(player->GetObjectGuid());
    if (it != m_lfgQueue.end())
        SendQueueStatus(player, it->second.dungeons);
    else if (Group* group = player->GetGroup())
    {
        auto rcIt = m_roleChecks.find(group->GetId());
        if (rcIt != m_roleChecks.end())
            player->SendAddonMessage(kPrefix, "S2C_UPDATE_QUEUE_STATUS;pending");
    }
    return true;
}

bool TurtleLFGMgr::HandleOfferAccept(Player* player)
{
    if (!player)
        return true;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    auto it = m_lfgQueue.find(player->GetObjectGuid());
    if (it == m_lfgQueue.end() || it->second.state != QueueState::Offered)
        return true;

    auto oit = m_offers.find(it->second.offerId);
    if (oit == m_offers.end())
        return true;

    Offer& offer = oit->second;
    bool all = true;
    for (Offer::Slot& slot : offer.slots)
    {
        if (slot.guid == player->GetObjectGuid())
            slot.accepted = true;
        if (!slot.accepted)
            all = false;
    }
    SendOfferUpdate(offer);

    if (all)
    {
        ResolveOffer(offer);
        for (Offer::Slot const& slot : offer.slots)
            m_lfgQueue.erase(slot.guid);
        m_offers.erase(oit);
    }
    return true;
}

// ---------------- matchmaker ----------------

void TurtleLFGMgr::Update(uint32 diff)
{
    m_updateTimer += diff;
    if (m_updateTimer < 2000)
        return;
    m_updateTimer = 0;

    std::lock_guard<std::mutex> lock(m_queueMutex);

    time_t now = time(nullptr);

    // expire role checks
    for (auto it = m_roleChecks.begin(); it != m_roleChecks.end();)
    {
        if (now > it->second.deadline)
        {
            AbortRoleCheck(it->first, it->second);
            it = m_roleChecks.erase(it);
        }
        else
            ++it;
    }

    // expire offers
    for (auto it = m_offers.begin(); it != m_offers.end();)
    {
        if (now > it->second.deadline)
        {
            CancelOffer(it->second, ObjectGuid());
            it = m_offers.erase(it);
        }
        else
            ++it;
    }

    DropStaleEntries();
    RefillLiveRuns();
    TryMatchDungeons();
}

// AUTO-REFILL (user mandate): a kicked/lost member leaves a role hole in a live LFT run — fill it
// with a fresh bot of the same role and teleport it straight to the leader. Run ends (and tracking
// stops) when the leader logs off, leaves the dungeon map, or the group dissolves.
void TurtleLFGMgr::RefillLiveRuns()
{
    time_t now = time(nullptr);
    for (auto it = m_liveRuns.begin(); it != m_liveRuns.end();)
    {
        LiveRun& run = it->second;
        Player* leader = FindQueuePlayer(run.leader);
        Group* group = leader ? leader->GetGroup() : nullptr;
        auto dit = m_dungeons.find(run.dungeonCode);
        if (!leader || !group || group->GetId() != run.groupId || dit == m_dungeons.end() ||
            leader->GetMapId() != dit->second.map)
        {
            it = m_liveRuns.erase(it);
            continue;
        }
        if (group->GetMembersCount() >= 5 || now - run.lastFill < 10)
        {
            ++it;
            continue;
        }

        // count missing roles (target comp 1t/1h/3d)
        uint32 haveTank = 0, haveHeal = 0, members = 0;
        std::vector<ObjectGuid> exclude;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member) continue;
            ++members;
            exclude.push_back(member->GetObjectGuid());
            uint8 roles = Playerbot_GetRoleOf(member);
            if (roles & 1) ++haveTank;
            else if (roles & 2) ++haveHeal;
        }
        uint32 slots = 5 - members;
        uint32 needTank = haveTank ? 0 : 1;
        uint32 needHeal = haveHeal ? 0 : 1;
        if (needTank + needHeal > slots) needHeal = slots > needTank ? slots - needTank : 0;
        uint32 needDps = slots - std::min(slots, needTank + needHeal);

        std::vector<ObjectGuid> fills = Playerbot_SelectLfgFill(
            dit->second.minLevel, dit->second.maxLevel, needTank, needHeal, needDps, exclude);
        if (fills.size() < needTank + needHeal + needDps)
        {
            run.lastFill = now;   // promotion may need a beat; retry in 10s
            ++it;
            continue;
        }

        for (ObjectGuid guid : fills)
        {
            Player* bot = FindQueuePlayer(guid);
            if (!bot) continue;
            if (group->AddMember(guid, bot->GetName()))
            {
                Playerbot_PrepareLfgBot(guid, run.leader);
                if (!bot->IsInCombat() && !bot->IsTaxiFlying())
                    bot->TeleportTo(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), 0.0f);
            }
        }
        group->BroadcastGroupUpdate();
        run.lastFill = now;
        sLog.outString("TurtleLFG: refilled %u slot(s) for %s's run (%s)", (uint32)fills.size(), leader->GetName(), run.dungeonCode.c_str());
        ++it;
    }
}

void TurtleLFGMgr::DropStaleEntries()
{
    for (auto it = m_lfgQueue.begin(); it != m_lfgQueue.end();)
    {
        if (it->second.state == QueueState::Queued && !FindQueuePlayer(it->first))
            it = m_lfgQueue.erase(it);
        else
            ++it;
    }
}

void TurtleLFGMgr::TryMatchDungeons()
{
    // gather queued (non-offered) humans per dungeon code, oldest joiner first
    std::map<std::string, std::vector<QueuedPlayer*>> byCode;
    for (auto& kv : m_lfgQueue)
        if (kv.second.state == QueueState::Queued)
            for (std::string const& code : kv.second.dungeons)
                byCode[code].push_back(&kv.second);

    for (auto& kv : byCode)
    {
        std::sort(kv.second.begin(), kv.second.end(),
            [](QueuedPlayer* a, QueuedPlayer* b) { return a->joinTime < b->joinTime; });
        if (TryCreateOffer(kv.first, kv.second))
            return;   // one offer per tick keeps state simple
    }
}

bool TurtleLFGMgr::TryCreateOffer(const std::string& code, std::vector<QueuedPlayer*>& humans)
{
    auto dit = m_dungeons.find(code);
    if (dit == m_dungeons.end() || !dit->second.valid)
        return false;
    DungeonInfo const& dungeon = dit->second;

    // greedy role assignment over humans: scarcest role first (tank, healer, then dps)
    ObjectGuid tank, healer;
    std::vector<ObjectGuid> dps;
    std::vector<std::pair<QueuedPlayer*, uint8>> assigned;

    for (QueuedPlayer* qp : humans)
    {
        if (tank.IsEmpty() && (qp->roles & ROLE_TANK))
            { tank = qp->guid; assigned.push_back({ qp, ROLE_TANK }); continue; }
        if (healer.IsEmpty() && (qp->roles & ROLE_HEALER))
            { healer = qp->guid; assigned.push_back({ qp, ROLE_HEALER }); continue; }
        if (dps.size() < 3 && (qp->roles & ROLE_DAMAGE))
            { dps.push_back(qp->guid); assigned.push_back({ qp, ROLE_DAMAGE }); }
    }

    if (assigned.empty())
        return false;

    // bot fill for the remaining slots
    uint32 needTank = tank.IsEmpty() ? 1 : 0;
    uint32 needHeal = healer.IsEmpty() ? 1 : 0;
    uint32 needDps = 3 - (uint32)dps.size();

    std::vector<ObjectGuid> exclude;
    for (auto const& pr : assigned)
        exclude.push_back(pr.first->guid);

    std::vector<ObjectGuid> bots;
    if (needTank || needHeal || needDps)
    {
        bots = Playerbot_SelectLfgFill(dungeon.minLevel, dungeon.maxLevel,
            needTank, needHeal, needDps, exclude);
        if (bots.size() < needTank + needHeal + needDps)
            return false;   // not enough suitable bots yet; try next tick
    }

    Offer offer;
    offer.id = m_nextOfferId++;
    offer.dungeonCode = code;
    offer.deadline = time(nullptr) + OFFER_TIMEOUT;

    for (auto const& pr : assigned)
    {
        Offer::Slot slot;
        slot.guid = pr.first->guid;
        slot.assignedRole = pr.second;
        offer.slots.push_back(slot);
        pr.first->state = QueueState::Offered;
        pr.first->offerId = offer.id;
    }

    // bots arrive ordered tank, healer, dps from the hook
    uint32 botIdx = 0;
    auto pushBot = [&](uint8 role)
    {
        Offer::Slot slot;
        slot.guid = bots[botIdx++];
        slot.assignedRole = role;
        slot.accepted = true;
        slot.isBot = true;
        offer.slots.push_back(slot);
    };
    for (uint32 i = 0; i < needTank; ++i) pushBot(ROLE_TANK);
    for (uint32 i = 0; i < needHeal; ++i) pushBot(ROLE_HEALER);
    for (uint32 i = 0; i < needDps; ++i) pushBot(ROLE_DAMAGE);

    m_offers[offer.id] = offer;

    for (auto const& pr : assigned)
        if (Player* p = FindQueuePlayer(pr.first->guid))
            SendOfferNew(p, code, pr.second);
    SendOfferUpdate(m_offers[offer.id]);   // shows pre-accepted bots immediately
    return true;
}

void TurtleLFGMgr::CancelOffer(Offer& offer, ObjectGuid decliner)
{
    for (Offer::Slot const& slot : offer.slots)
    {
        if (slot.isBot)
            continue;
        auto it = m_lfgQueue.find(slot.guid);
        Player* p = FindQueuePlayer(slot.guid);
        bool declined = (slot.guid == decliner) || (!slot.accepted && decliner.IsEmpty());
        if (declined)
        {
            if (it != m_lfgQueue.end()) m_lfgQueue.erase(it);
            if (p) SendQueueLeft(p);
        }
        else if (it != m_lfgQueue.end())
        {
            it->second.state = QueueState::Queued;
            it->second.offerId = 0;
            if (p)
            {
                SendQueueJoined(p);
                SendQueueStatus(p, it->second.dungeons);
            }
        }
    }
}

void TurtleLFGMgr::ResolveOffer(Offer& offer)
{
    DungeonInfo const& dungeon = m_dungeons[offer.dungeonCode];

    // find the real-player leader: an existing party leader, else the first human
    Player* leader = nullptr;
    for (Offer::Slot const& slot : offer.slots)
        if (!slot.isBot)
            if (Player* p = FindQueuePlayer(slot.guid))
            {
                if (!leader) leader = p;
                if (p->GetGroup() && p->GetGroup()->IsLeader(p->GetObjectGuid())) { leader = p; break; }
            }
    if (!leader)
        return;

    for (Offer::Slot const& slot : offer.slots)
        if (!slot.isBot)
            if (Player* p = FindQueuePlayer(slot.guid))
                SendOfferComplete(p);

    Group* group = leader->GetGroup();
    if (!group)
    {
        group = new Group();
        if (!group->Create(leader->GetObjectGuid(), leader->GetName()))
        {
            delete group;
            SendQueueError(leader, "internal");
            return;
        }
        sObjectMgr.AddGroup(group);
        group->SetLootMethod(GROUP_LOOT);
    }

    for (Offer::Slot const& slot : offer.slots)
    {
        if (slot.guid == leader->GetObjectGuid())
            continue;
        Player* member = FindQueuePlayer(slot.guid);
        if (!member)
            continue;
        if (member->GetGroup() == group)
            { /* already partied with leader */ }
        else if (member->GetGroup())
            continue;   // grouped elsewhere since matching; skip (humans were validated at queue time)
        else
            group->AddMember(slot.guid, member->GetName());

        if (slot.isBot)
            Playerbot_PrepareLfgBot(slot.guid, leader->GetObjectGuid());
    }
    group->BroadcastGroupUpdate();

    // teleport everyone to the entrance
    for (Offer::Slot const& slot : offer.slots)
        if (Player* member = FindQueuePlayer(slot.guid))
            if (!member->IsTaxiFlying() && !member->IsInCombat())
                member->TeleportTo(dungeon.map, dungeon.x, dungeon.y, dungeon.z, dungeon.o);

    LiveRun run;
    run.groupId = group->GetId();
    run.dungeonCode = offer.dungeonCode;
    run.leader = leader->GetObjectGuid();
    m_liveRuns[run.groupId] = run;

    sLog.outString("TurtleLFG: formed group for '%s' (leader %s)", offer.dungeonCode.c_str(), leader->GetName());
}
