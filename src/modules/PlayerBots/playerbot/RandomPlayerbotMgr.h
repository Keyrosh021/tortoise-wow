#ifndef _RandomPlayerbotMgr_H
#define _RandomPlayerbotMgr_H

#include "Common.h"
#include "PlayerbotAIBase.h"
#include "PlayerbotMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "WorldPosition.h"
#include <atomic>
#include <mutex>
#include <map>
#include <list>
#include <thread>

class WorldPacket;
class Player;
class Unit;
class Object;
class Item;

class CachedEvent
{
public:
    CachedEvent() : value(0), lastChangeTime(0), validIn(0), data("") {}
    CachedEvent(const CachedEvent& other) : value(other.value), lastChangeTime(other.lastChangeTime), validIn(other.validIn), data(other.data) {}
    CachedEvent(uint32 value, uint32 lastChangeTime, uint32 validIn, std::string data = "") : value(value), lastChangeTime(lastChangeTime), validIn(validIn), data(data) {}

public:
    bool IsEmpty() { return !lastChangeTime; }

public:
    uint32 value, lastChangeTime, validIn;
    std::string data;
};

class PerformanceMonitorOperation;

//https://gist.github.com/bradley219/5373998

class botPIDImpl;
class botPID
{
public:
    // Kp -  proportional gain
    // Ki -  Integral gain
    // Kd -  derivative gain
    // dt -  loop interval time
    // max - maximum value of manipulated variable
    // min - minimum value of manipulated variable
    botPID(double dt, double max, double min, double Kp, double Ki, double Kd);
    void adjust(double Kp, double Ki, double Kd);
    void reset();
   
    double calculate(double setpoint, double pv);
    ~botPID();

private:
    botPIDImpl* pimpl;
};

class RandomPlayerbotMgr : public PlayerbotHolder
{
    public:
        enum class UpdateWatchdogPhase : uint32
        {
            Idle = 0,
            UpdateSessions,
            ScaleBotActivity,
            AsyncBotLogin,
            AddRandomBots,
            CheckPlayers,
            CheckLfgQueue,
            CheckBgQueue,
            AddOfflineGroupBots,
            ProcessBotLoop,
            LoginQueue,
            LoginFreeBots,
            LogPlayerLocation,
            DelayedFacingFix,
            MirrorAh,
            PerfInit,
            DatabasePing,
            HolderUpdate
        };

        RandomPlayerbotMgr();
        virtual ~RandomPlayerbotMgr() override;
        static RandomPlayerbotMgr& instance()
        {
            static RandomPlayerbotMgr instance;
            return instance;
        }

        virtual void UpdateAIInternal(uint32 elapsed, bool minimal = false) override;
private:
        void ScaleBotActivity();
        void LogPlayerLocation();
        void DelayedFacingFix();
        void LoginFreeBots();
public:
        static void DatabasePing(QueryResult* result, uint32 pingStart, std::string db);
        void SetDatabaseDelay(std::string db, uint32 delay) {databaseDelay[db] = delay;}
        uint32 GetDatabaseDelay(std::string db) {if(databaseDelay.find(db) == databaseDelay.end()) return 0; return databaseDelay[db];}

        void LoadNamedLocations();
        bool AddNamedLocation(std::string const& name, WorldLocation const& location);
        bool GetNamedLocation(std::string const& name, WorldLocation& location);

        static bool HandlePlayerbotConsoleCommand(ChatHandler* handler, char const* args);
        bool IsRandomBot(Player* bot);
        bool IsRandomBot(uint32 bot);
        bool IsFreeBot(Player* bot) { return IsRandomBot(bot) || sPlayerbotAIConfig.IsFreeAltBot(bot); }
        bool IsFreeBot(uint32 bot) { return IsRandomBot(bot) || sPlayerbotAIConfig.IsFreeAltBot(bot); }
        void InstaRandomize(Player* bot);
        void Randomize(Player* bot);
        void RandomizeFirst(Player* bot);
        void UpdateGearSpells(Player* bot);
        void ScheduleTeleport(uint32 bot, uint32 time = 0);
        void ScheduleChangeStrategy(uint32 bot, uint32 time = 0);
        void HandleCommand(uint32 type, const std::string& text, Player& fromPlayer, std::string channelName = "", Team team = TEAM_BOTH_ALLOWED, uint32 lang = LANG_UNIVERSAL);
        std::string HandleRemoteCommand(std::string request);
        void OnPlayerLogout(Player* player);
        void OnPlayerLogin(Player* player);
        void OnPlayerLoginError(uint32 bot);
        Player* GetRandomPlayer();
        PlayerBotMap& GetPlayers() { return players; };
        // Thread-safe snapshot of the players map. The map is mutated on the main thread
        // (OnPlayerLogin/Logout/MovePlayerBot) while bot AI on parallel map-update worker
        // threads iterates it (e.g. HasPlayerRelation) -> a concurrent erase frees a Player*
        // mid-iteration -> SIGSEGV in Player::GetSocial(). The map is tiny (real players +
        // rare transfers), so copying it under the lock is cheap. Readers on worker threads
        // MUST use this, not the raw GetPlayers() reference. Writers lock playersMutex too.
        PlayerBotMap GetPlayersCopy() { std::lock_guard<std::mutex> lock(playersMutex); return players; }
        std::mutex& GetPlayersMutex() { return playersMutex; }
        // THREAD-SAFE real-player snapshot for MAP-THREAD readers (HasPlayerNearby /
        // HasManyPlayersNearby / guild-chat checks). Iterating the raw `players` map from map
        // threads while the world thread mutates it on login/logout was the root cause of the
        // entire 2026-07-04 SIGSEGV family (LogPlayerLocation/AllowActivity frames + the
        // frameless heap-corruption crash). Rebuilt once per world tick from LIVE lookups;
        // readers copy the shared_ptr and touch NO Player pointers at all.
        struct RealPlayerSnap
        {
            uint32 guidLow;
            uint32 guildId;
            uint32 mapId;
            float x, y, z;
            float camX, camY, camZ;
            bool hasCam;
        };
        struct RealPlayerView
        {
            std::vector<RealPlayerSnap> players;
            // bots on any real player's friend list (world thread pre-computes this so
            // GetPriorityType never touches Player::GetSocial from a map thread)
            std::unordered_set<uint32> friendBotGuids;
        };
        std::shared_ptr<const RealPlayerView> GetRealPlayerSnapshot()
        {
            std::lock_guard<std::mutex> lock(m_rpSnapMutex);
            return m_rpSnap;
        }
        void RebuildRealPlayerSnapshot();   // world thread only (UpdateAIInternal)
        // PROXIMITY BRAIN ALLOCATOR (the 10k illusion): a fixed budget of ActiveBrainBudget
        // bot brains, always given to the bots NEAREST to real players so everything a player
        // can see plays like a human, while the rest of the fleet stands parked-but-visible.
        // Nearest-first ordering makes zones "warm up" as a player approaches. With no players
        // online the budget rotates through the fleet (slow window) so the world progresses.
        // Rebuilt every 2s on the world thread; map threads read the published set.
        std::shared_ptr<const std::unordered_set<uint32>> GetActiveBrainSet() const
        {
            std::lock_guard<std::mutex> lock(m_abMutex);
            return m_activeBrains;
        }
        void RebuildActiveBrainSet();       // world thread only (UpdateAIInternal)
        // one-time fleet level spread toward hash-assigned 1-60 targets (paced, idempotent,
        // never de-levels); teleports each spread bot to a level-appropriate zone
        void SpreadFleetLevels();
        bool RealPlayerActive();
        // CITY LIFE: teleport a hash-band of bots to capital cities, level them high + gear them,
        // and sit them AFK so capitals feel populated. Paced, world-thread only, idempotent.
        void PopulateCapitalCities();
        void ContainLowLevelBots();
        // Instant BG pop: when a real player queues a BG, immediately enqueue + port enough bots
        // (both factions, matching bracket) server-side so the queue pops now (TurtleLFG-style).
        void InstantFillBgQueue(Player* realPlayer, uint32 bgTypeId, uint32 bracketId);
        void QueueInstantFillBg(uint32 playerGuidLow, uint32 bgTypeId, uint32 bracketId); // MAP-THREAD SAFE entry
        void DrainBgFillRequests();   // world thread: perform the queued fills
        void SpawnMockBattlegrounds(); // bot-only BGs for autonomous tactics observation
        void EvacuateStrandedBgBots(); // pull bots out of dead/stuck mock BGs (crash source) when mocks off
        void MockBgWatch();            // 20s telemetry on every live WSG: scores/flags/AFK bots
        void DrainPendingBgPorts();   // port enqueued bots into their BG the tick they get invited
        void PromoteSixtyCampaign();  // hold L60 population at Level60Target: promote low bots, gear T2, teleport to L60 zones
        void ZoneMismatchSweep();     // teleport high-level bots squatting in low-level zones to level-appropriate zones
        bool IsCityResident(uint32 guidLow) const;
        // DUNGEON/RAID ENTRANCE LOITERERS ("actor roles"): rotate level-appropriate bots to loiter at
        // dungeon/raid entrances (fake group-forming); some walk in and zone in. ROTATES so different
        // bots play the role over time (feels like different people running dungeons).
        void PopulateDungeonEntrances();
        bool IsDungeonLoiterer(uint32 guidLow) const;
        bool GetDungeonLoiterSpot(uint32 guidLow, uint32& mapId, float& x, float& y, float& z, bool& zoneIn) const;
        void EndDungeonRole(uint32 guidLow);   // free the slot for rotation
        bool GetDungeonZoneInDest(uint32 guidLow, uint32& mapId, float& x, float& y, float& z) const;
        void MarkLoitererZonedIn(uint32 guidLow);
        bool LoitererDwellExpired(uint32 guidLow) const;
        // MAP-THREAD SAFE teleport request: TeleportTo/RandomTeleportForLevel must NEVER run from a
        // map-thread AI tick (documented corruption/crash source). Map threads enqueue here; the
        // world thread executes via DrainBotTeleports(). forLevel=true -> RandomTeleportForLevel.
        void QueueBotTeleport(uint32 guidLow, uint32 mapId, float x, float y, float z, bool forLevel, bool resurrect = false);
        void DrainBotTeleports();      // world thread only
        Player* GetPlayer(uint32 playerGuid);
        // Live census: for each REAL player online, tally what the bots within render
        // distance are doing (fighting/moving/idle/dead/...) -> logs/nearby_census.csv.
        // Read it with tools/nearby_census.py. Called on a ~3s timer from UpdateAIInternal.
        void LogNearbyCensus();
        // Fleet-wide per-bot diagnostics (every ~60s): one aggregate row to
        // logs/bot_fleet.csv + a per-bot detail row to logs/bot_diag.csv, so we can
        // study WHY bots are stuck (no goal vs travel-not-moving vs cooldown/expired)
        // and correlate with deaths. Main-thread, race-tolerant reads only.
        void LogBotDiagSample();
        // PROOF: top quest mobs idle bots starve on + map-wide alive count + bot clustering.
        void LogStuckMobReport();
        // 10K TWO-TIER FLEET: is this bot in the fully-active cohort? Deterministic hash of
        // (guid, rotation epoch) so ~ActiveCohortSize of MaxRandomBots are active at any time
        // with no shared state; membership rotates every CohortRotateMinutes. Everyone else
        // is LOD-dormant unless a real player is nearby. Safe from any thread.
        bool IsActiveCohort(uint32 guidLow) const;
        // Synthetic progression for dormant bots (world thread only, called from
        // UpdateAIInternal): measures real xp/gold rates from the active cohort into
        // per-level-bracket EMAs, applies rate*SyntheticRateFactor to dormant bots ~1/min.
        // Dormant bots keep leveling and earning without costing map-thread CPU.
        void UpdateSyntheticProgress();
        // Self-healing raid gear: naked max-level bots (incl. ones inside a player's raid)
        // get the deterministic T2 kit automatically, every 30s. World thread only.
        void SweepNakedMaxLevelBots();
        void PrintStats(uint32 requesterGuid);
        double GetBuyMultiplier(Player* bot);
        double GetSellMultiplier(Player* bot);
        void AddTradeDiscount(Player* bot, Player* master, int32 value);
        void SetTradeDiscount(Player* bot, Player* master, uint32 value);
        uint32 GetTradeDiscount(Player* bot, Player* master);
        void Refresh(Player* bot);
        void RandomTeleportForLevel(Player* bot, bool activeOnly);
        void RandomTeleportForLevel(Player* bot) { return RandomTeleportForLevel(bot, true); }
        // Nearest level+faction-appropriate mob-spawn cluster on the bot's map (from the spawn cache),
        // at least minDist away (so it's a DIFFERENT camp, not the depleted one the bot stands on).
        // Used by the FSM to make an idle bot ALWAYS walk to content instead of standing around.
        bool GetNearestGrindSpot(Player* bot, float minDist, float& outX, float& outY, float& outZ);
        void RandomTeleportForRpg(Player* bot, bool activeOnly);
        void RandomTeleportForRpg(Player* bot) { return RandomTeleportForRpg(bot, true); }
        int GetMaxAllowedBotCount();
        bool ProcessBot(Player* player);
        void Revive(Player* player);
        void ChangeStrategy(Player* player);
        uint32 GetValue(Player* bot, std::string type);
        uint32 GetValue(uint32 bot, std::string type);
        int32 GetValueValidTime(uint32 bot, std::string event);
        std::string GetData(uint32 bot, std::string type);
        void SetValue(uint32 bot, std::string type, uint32 value, std::string data = "", int32 validIn = -1);
        void SetValue(Player* bot, std::string type, uint32 value, std::string data = "", int32 validIn = -1);
        void Remove(Player* bot);
        void Hotfix(Player* player, uint32 version);
        uint32 GetBattleMasterEntry(Player* bot, BattleGroundTypeId bgTypeId, bool fake = false);
        const CreatureDataPair* GetCreatureDataByEntry(uint32 entry);
        uint32 GetCreatureGuidByEntry(uint32 entry);
        void LoadBattleMastersCache();
        std::map<uint32, std::map<uint32, std::map<uint32, bool> > > NeedBots;
        std::map<uint32, std::map<uint32, std::map<uint32, uint32> > > BgBots;
        std::map<uint32, std::map<uint32, std::map<uint32, uint32> > > VisualBots;
        std::map<uint32, std::map<uint32, std::map<uint32, uint32> > > BgPlayers;
        std::map<uint32, std::map<uint32, std::map<uint32, std::map<uint32, uint32> > > > ArenaBots;
        std::map<uint32, std::map<uint32, std::map<uint32, uint32> > > Rating;
        std::map<uint32, std::map<uint32, std::map<uint32, uint32> > > Supporters;
        std::map<Team, std::vector<uint32>> LfgDungeons;
        void CheckBgQueue();
        void CheckLfgQueue();
        void CheckPlayers();
        void SaveCurTime();
        void SyncEventTimers();
        void AddOfflineGroupBots();
        static Item* CreateTempItem(uint32 item, uint32 count, Player const* player, uint32 randomPropertyId = 0);
        static InventoryResult CanEquipUnseenItem(Player* player, uint8 slot, uint16& dest, uint32 item);

        bool AddRandomBot(uint32 bot);
        bool CreateRandomBot(const std::string& name, uint8 race, uint8 cls, uint32 level);
        bool DeleteRandomBot(ObjectGuid guid);
        virtual void MovePlayerBot(uint32 guid, PlayerbotHolder* newHolder) override;

        std::map<Team, std::map<BattleGroundTypeId, std::list<uint32> > > getBattleMastersCache() { return BattleMastersCache; }

        float getActivityMod() { return activityMod; }
        float getActivityPercentage() { return activityMod * 100.0f; }
        void setActivityPercentage(float percentage) { activityMod = percentage / 100.0f; }

        void PrintTeleportCache();

        void AddFacingFix(uint32 mapId, uint32 instanceId, ObjectGuid guid) { facingFix[mapId][instanceId].push_back(std::make_pair(guid,time(0))); }

        bool arenaTeamsDeleted, guildsDeleted = false;

        std::mutex m_ahActionMutex;

        const std::vector<AuctionEntry>& GetAhPrices(uint32 itemId) {
            static const std::vector<AuctionEntry> emptyVector; // Avoid returning dangling refs
            auto it = ahMirror.find(itemId);
            return (it != ahMirror.end()) ? it->second : emptyVector;}
        uint32 GetPlayersLevel() { return playersLevel; }
    protected:
        virtual void OnBotLoginInternal(Player * const bot) override;
    private:
        //pid values are set in constructor
        botPID pid = botPID(1, 50, -50, 0, 0, 0);
        float activityMod = 0.25;
        std::map<std::string, uint32> databaseDelay;
        uint32 GetEventValue(uint32 bot, std::string event);
        std::string GetEventData(uint32 bot, std::string event);
        uint32 SetEventValue(uint32 bot, std::string event, uint32 value, uint32 validIn, std::string data = "");
        std::list<uint32> GetBots();
        std::list<uint32> GetBgBots(uint32 bracket);
        time_t BgCheckTimer;
        time_t LfgCheckTimer;
        time_t PlayersCheckTimer;
        time_t EventTimeSyncTimer;
        time_t OfflineGroupBotsTimer;
        uint32 AddRandomBots();
        bool ProcessBot(uint32 bot);
        void ScheduleRandomize(uint32 bot, uint32 time);
        void RandomTeleport(Player* bot);
        void RandomTeleport(Player* bot, std::vector<WorldLocation> &locs, bool hearth = false, bool activeOnly = false);
        uint32 GetZoneLevel(uint16 mapId, float teleX, float teleY, float teleZ);
        void PrepareTeleportCache();
        typedef std::list<std::string> (RandomPlayerbotMgr::*ConsoleCommandHandler) (std::string param);
        typedef std::list<std::string> (RandomPlayerbotMgr::*ConsolePlayerCommandHandler) (Player*);

        std::string consoleCmdParams;

        std::list<std::string> HandleHelp(std::string param);
        std::list<std::string> HandleConsoleReset(std::string param);
        std::list<std::string> HandleConsoleStats(std::string param);
        std::list<std::string> HandleConsoleReload(std::string param);
        std::list<std::string> HandleConsoleUpdate(std::string param);
        std::list<std::string> HandleConsolePid(std::string param);
        std::list<std::string> HandleConsoleDiff(std::string param);
        std::list<std::string> HandleConsoleCleanMap(std::string param);
        std::list<std::string> HandleConsoleLoginDebug(std::string param);
        std::list<std::string> HandleConsolePathCheck(std::string param);
        // Override virtual methods from PlayerbotHolder
        virtual uint32 GetOrCreateAccount(Player* master, std::string& error) override;
        virtual void OnBotDeleted(uint32 botGuid, uint32 accountId) override;

    public:
        static std::string GetCommandTexts(const std::string& command);
        static std::unordered_map<std::string, std::string> GetCommandTexts();
        std::list<std::string> HandleRandomizeFirst(Player* bot);
        std::list<std::string> HandleUpdateGearSpells(Player* bot);
        std::list<std::string> HandleRefresh(Player* bot);
        std::list<std::string> HandleRandomTeleportForLevel(Player* bot);
        std::list<std::string> HandleRandomTeleportForRpg(Player* bot);
        std::list<std::string> HandleRevive(Player* bot);
        std::list<std::string> HandleRandomTeleport(Player* bot);
        std::list<std::string> HandleChangeStrategy(Player* bot);
        std::list<std::string> HandleRemove(Player* bot);

        void MirrorAh();
    private:
        static const char* GetWatchdogPhaseName(UpdateWatchdogPhase phase);
        void StartUpdateWatchdog();
        void StopUpdateWatchdog();
        void SetUpdateWatchdogPhase(UpdateWatchdogPhase phase, uint32 availableBotCount, uint32 onlineBotCount);
        static uint64 GetWatchdogSteadyMs();

        PlayerBotMap players;
        mutable std::mutex playersMutex;
        int processTicks;
        size_t processBotCursor = 0;
        size_t loginBotCursor = 0;
        std::unordered_map<std::string, WorldLocation> namedLocations;
        std::map<uint8, std::vector<WorldLocation> > locsPerLevelCache;
        std::map<uint32, std::vector<WorldLocation> > rpgLocsCache;
		std::map<uint32, std::map<uint32, std::vector<WorldLocation> > > rpgLocsCacheLevel;
        std::map<uint32, std::map<uint32, std::vector<std::pair<ObjectGuid, WorldLocation>> > > innCacheLevel;
        std::map<Team, std::map<BattleGroundTypeId, std::list<uint32> > > BattleMastersCache;
        std::map<uint32, std::map<std::string, CachedEvent> > eventCache;
        std::mutex m_eventCacheMutex;
        BarGoLink* loginProgressBar;
        std::list<uint32> currentBots;
        std::list<uint32> arenaTeamMembers;
        uint32 m_nextCensusMs = 0;   // throttle LogNearbyCensus to ~3s
        uint32 m_nextDiagMs = 0;     // throttle LogBotDiagSample to ~60s
        uint32 m_nextStuckMs = 0;    // throttle LogStuckMobReport to ~5min (heavy)
        uint32 m_nextNakedSweepMs = 0;   // naked-60 gear sweep throttle
        // virtual observer anchor (synthetic player when nobody is online)
        uint32 m_voMapId = 0; float m_voX = 0, m_voY = 0, m_voZ = 0;
        bool m_voValid = false;
        uint32 m_voRotateMs = 0;
        uint32 m_abLogMs = 0;   // allocator csv log throttle
        uint32 m_lvlSpreadCursor = 0;   // level-spread walk position
        uint32 m_lvlSpreadDoneMs = 0;   // set when a full pass found nothing to do
        uint32 m_cityCursor = 0;        // city-populate walk position
        uint32 m_cityNextMs = 0;        // city-populate pacing
        uint32 m_lowContainMs = 0;
        uint32 m_lowContainCursor = 0;
        uint32 m_nakedSweepCursor = 0;
        struct BgFillRequest { uint32 playerGuidLow; uint32 bgTypeId; uint32 bracketId; };
        std::mutex m_bgFillMutex;
        std::vector<BgFillRequest> m_bgFillRequests;
        struct PendingBgPort { uint32 guidLow; uint32 mapId; uint32 queueTypeId; uint32 bgTypeId; uint32 bracketId; };
        std::vector<PendingBgPort> m_pendingBgPort;
        uint32 m_pendingBgPortUntilMs = 0;
        uint32 m_pendingBgKickMs = 0;   // 2s re-kick of the event-scheduled queue update
        uint32 m_pendingBgDiagMs = 0;   // 5s throttle for BGPORT wait diagnostics
        bool   m_mockBgSpawned = false;
        uint32 m_mockWatchMs = 0;
        std::unordered_map<uint32, uint32> m_stuckStreak; // consecutive STUCK censuses per bot (near-player rescue)
        uint32 m_promote60NextMs = 0;
        uint32 m_promote60Cursor = 0;
        uint32 m_zoneFixNextMs = 0;
        uint32 m_zoneFixCursor = 0;
        // dungeon/raid entrance loiterer role pool
        struct DungeonEntrance { uint32 map; float x, y, z; uint8 minLevel; bool isRaid;
                                 uint32 destMap; float destX, destY, destZ; };
        std::vector<DungeonEntrance> m_dungeonEntrances;
        bool m_dungeonBuilt = false;
        struct LoiterRole { uint32 entranceIdx; uint32 expiryMs; bool zoneIn; uint32 zonedInMs = 0; };
        std::unordered_map<uint32, LoiterRole> m_dungeonRoles;   // guidLow -> role
        mutable std::mutex m_dungeonMutex;
        uint32 m_dungeonNextMs = 0;
        uint32 m_dungeonCursor = 0;
        uint32 m_bgEvacNextMs = 0;
        // map-thread-safe teleport request queue (drained on the world thread)
        struct BotTeleRequest { uint32 guid; uint32 map; float x, y, z; bool forLevel; bool resurrect; };
        std::vector<BotTeleRequest> m_teleRequests;
        std::mutex m_teleMutex;
        // proximity brain allocator storage
        std::shared_ptr<const std::unordered_set<uint32>> m_activeBrains = std::make_shared<const std::unordered_set<uint32>>();
        mutable std::mutex m_abMutex;
        uint32 m_abNextMs = 0;          // 2s rebuild throttle
        uint32 m_abWindowStart = 0;     // no-players rotation window anchor
        uint32 m_abWindowRotateMs = 0;  // when to advance the rotation window
        // real-player snapshot storage (see GetRealPlayerSnapshot)
        std::shared_ptr<const RealPlayerView> m_rpSnap = std::make_shared<const RealPlayerView>();
        std::mutex m_rpSnapMutex;
        // 10K synthetic progression state (world-thread only)
        struct SynthState { uint32 lastXp = 0; uint32 lastMoney = 0; uint32 lastMs = 0; uint8 lastLevel = 0; uint8 wasDormant = 2; };
        std::unordered_map<uint32, SynthState> synthStates;
        double synthXpRate[7] = {};   // EMA xp/hour by level/10 bracket, measured from active bots
        double synthGoldRate[7] = {}; // EMA copper/hour
        uint32 synthCursor = 0;
        uint32 synthAppliedBots = 0, synthAppliedXp = 0, synthAppliedMoney = 0;
        uint32 synthLogDueMs = 0;
        uint32 bgBotsCount;
        uint32 playersLevel = 0;
        uint32 botCount = 0;
        uint32 activeBots = 0;        

        std::unordered_map<uint32, std::vector<std::pair<int32,int32>>> playerBotMoveLog;
        typedef std::unordered_map <uint32, std::list<float>> botPerformanceMetric;
        std::unordered_map<std::string, botPerformanceMetric> botPerformanceMetrics;
        
        std::vector<std::pair<uint32, uint32>> RpgLocationsNear(const WorldLocation pos, const std::map<uint32, std::map<uint32, std::vector<std::string>>>& areaNames, uint32 radius = 2000);
        void PushMetric(botPerformanceMetric& metric, const uint32 bot, const float value, const uint32 maxNum = 60) const;
        float GetMetricDelta(botPerformanceMetric& metric) const;

        bool showLoginWarning;
        std::unordered_map<uint32, std::unordered_map<uint32, std::vector<std::pair<ObjectGuid, time_t>>>> facingFix;
        std::atomic<bool> m_updateWatchdogStop{false};
        std::atomic<uint32> m_updateWatchdogPhase{static_cast<uint32>(UpdateWatchdogPhase::Idle)};
        std::atomic<uint64> m_updateWatchdogPhaseSinceMs{0};
        std::atomic<uint32> m_updateWatchdogAvailableBots{0};
        std::atomic<uint32> m_updateWatchdogOnlineBots{0};
        std::thread m_updateWatchdogThread;

        //                   itemId,             buyout, count
        std::unordered_map < uint32, std::vector<AuctionEntry>> ahMirror;
};

#define sRandomPlayerbotMgr RandomPlayerbotMgr::instance()

#endif
