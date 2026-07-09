#pragma once

#include "Config/Config.h"
#include "Talentspec.h"
#include "SharedDefines.h"
#include "SystemConfig.h"

class Player;
class PlayerbotMgr;
class ChatHandler;

#if PLATFORM == PLATFORM_WINDOWS
inline std::string _D_AIPLAYERBOT_CONFIG = "aiplayerbot.conf";
#else
inline std::string _D_AIPLAYERBOT_CONFIG = SYSCONFDIR "aiplayerbot.conf";
#endif

// ---- Learning A/B experiment ----------------------------------------------
// Deterministic per-bot cohort so EVERY server run is a live experiment: each
// cohort runs a different parameter value, every fight is tagged with its cohort
// + reward (see BotLearningMgr), and the learning report tool compares avg reward
// by cohort to tell us which value wins. To change what we're learning each run,
// edit the per-cohort value function(s) below; then read tools/bot_learning_report.py
// after the run. Walk away with a learned result every single run.
namespace BotExperiment
{
    constexpr uint32 COHORTS = 3;
    inline uint32 Cohort(uint32 guidLow) { return guidLow % COHORTS; }

    // CURRENT EXPERIMENT: LEARNED-POLICY STRENGTH. The engine nudges each action's
    // relevance by its learned value (avg fight reward when used in this state), scaled
    // by this per-cohort strength. Cohort 0 = 0.0 (CONTROL: pure hand-coded behavior),
    // cohort 1 = 0.6, cohort 2 = 1.0 (full learned bias). Comparing avg reward by cohort
    // tells us whether acting on the learned rotation actually IMPROVES outcomes, and how
    // strongly to apply it. (Prior experiment learned flee<=50% HP, now baked as default.)
    inline float PolicyStrength(uint32 guidLow)
    {
        static const float values[COHORTS] = { 0.0f, 0.6f, 1.0f };
        return values[Cohort(guidLow)];
    }
}

enum class BotCheatMask : uint32
{
    none = 0,
    taxi = 1 << 0,
    gold = 1 << 1,
    health = 1 << 2,
    mana = 1 << 3,
    power = 1 << 4,
    item = 1 << 5,
    cooldown = 1 << 6,
    repair = 1 << 7,
    movespeed = 1 << 8,
    attackspeed = 1 << 9,
    breath = 1 << 10,
    glyph = 1 << 11,
    quest = 1 << 12,    
    maxMask = 1 << 13
};

enum class BotAutoLogin : uint32
{
    DISABLED = 0,
    LOGIN_ALL_WITH_MASTER = 1,
    LOGIN_ONLY_ALWAYS_ACTIVE = 2
};

enum class BotSelfBotLevel : uint32
{
    DISABLED = 0,
    GM_ONLY = 1,
    ACTIVE_BY_COMMAND = 2,
    ALWAYS_ALLOWED = 3,
    ACTIVE_BY_LOGIN = 4, 
    ALWAYS_ACTIVE = 5
};

enum class BotAlwaysOnline : uint32
{
    DISABLED = 0,
    ACTIVE = 1,
    DISABLED_BY_COMMAND = 2
};

enum class BotLoginCriteriaType : uint8
{
    RACECLASS = 0,
    LEVEL = 1,
    RANGE_TO_PLAYER = 2,
    MAX_LOGIN_CRITERIA = 3
};

#define MAX_GEAR_PROGRESSION_LEVEL 6

class ConfigAccess
{
private:
    std::string m_filename;
    std::string m_envVarPrefix;
    std::unordered_map<std::string, std::string> m_entries; // keys are converted to lower case.  values cannot be.

public:
    std::vector<std::string> GetValues(const std::string& name) const;
    std::mutex m_configLock;
};

struct ParsedUrl {
    std::string hostname;
    std::string path;
    int port;
    bool https;
};

//GlyphPrioritySpecMap[specId][level] = {{glyphItemId, prereqTalentSpell}};
using GlyphPriority = std::pair<uint32, uint32>;
using GlyphPriorityList = std::vector<GlyphPriority>;
using GlyphPriorityLevelMap = std::unordered_map<uint32, GlyphPriorityList>;
using GlyphPrioritySpecMap = std::unordered_map<uint32, GlyphPriorityLevelMap>;

class PlayerbotAIConfig
{
public:
    PlayerbotAIConfig();
    static PlayerbotAIConfig& instance()
    {
        static PlayerbotAIConfig instance;
        return instance;
    }

public:
    bool Initialize();
    bool IsInRandomAccountList(uint32 id);
    bool IsFreeAltBot(uint32 guid);
    bool IsFreeAltBot(Player* player) {return IsFreeAltBot(player->GetGUIDLow());}
    bool IsInRandomQuestItemList(uint32 id);
	bool IsInPvpProhibitedZone(uint32 id);

    bool enabled;
    bool allowGuildBots;
    bool allowMultiAccountAltBots;
    uint32 globalCoolDown, reactDelay, maxWaitForMove, expireActionTime, dispelAuraDuration, passiveDelay, repeatDelay,
        errorDelay, rpgDelay, sitDelay, returnDelay, lootDelay;
    float sightDistance, spellDistance, reactDistance, grindDistance, lootDistance, groupMemberLootDistance, groupMemberLootDistanceWithActiveMaster,
        gatheringDistance, groupMemberGatheringDistance, groupMemberGatheringDistanceWithActiveMaster, shootDistance,
        fleeDistance, tooCloseDistance, meleeDistance, followDistance, raidFollowDistance, wanderMinDistance, wanderMaxDistance, whisperDistance, contactDistance,
        aoeRadius, rpgDistance, targetPosRecalcDistance, farDistance, healDistance, aggroDistance, proximityDistance, maxFreeMoveDistance, freeMoveDelay, walkDistance;
    uint32 criticalHealth, lowHealth, mediumHealth, almostFullHealth;
    uint32 lowMana, mediumMana;

    uint32 openGoSpell;
    bool randomBotAutologin;
    BotAutoLogin botAutologin;
    std::string randomBotMapsAsString;
    std::vector<uint32> randomBotMaps;
    std::list<uint32> randomBotQuestItems;
    std::list<uint32> randomBotAccounts;
    std::list<uint32> randomBotSpellIds;
    std::list<uint32> randomBotQuestIds;
    std::list<uint32> immuneSpellIds;
    std::list<std::pair<uint32, uint32>> freeAltBots;
    std::list<std::string> toggleAlwaysOnlineAccounts;
    std::list<std::string> toggleAlwaysOnlineChars;
    bool enableRandomTeleports;
    bool enableMinimalMove;
    uint32 randomBotTeleportDistance;
    bool randomBotTeleportNearPlayer;
    uint32 transportTeleportType;
    uint32 randomBotTeleportNearPlayerMaxAmount;
    float randomBotTeleportNearPlayerMaxAmountRadius;
    uint32 randomBotTeleportMinInterval, randomBotTeleportMaxInterval;
    uint32 randomGearMaxLevel;
    uint32 randomGearMaxDiff;
    bool randomGearUpgradeEnabled;
    bool randomGearTabards;
    bool randomGearTabardsReplaceGuild;
    bool randomGearTabardsUnobtainable;
    float randomGearTabardsChance;
    std::list<uint32> randomGearBlacklist;
    std::list<uint32> randomGearWhitelist;
    bool randomGearProgression;
    float randomGearLoweringChance;
    bool rollBadItemsWithPlayer;
    float randomBotMaxLevelChance;
    float randomBotRpgChance;
    float usePotionChance;
    float attackEmoteChance;
    bool randomBotAutoCreate;
    uint32 minRandomBots, maxRandomBots;
    uint32 randomBotUpdateInterval, randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval;
    bool randomBotTimedLogout, randomBotTimedOffline;
    uint32 minRandomBotInWorldTime, maxRandomBotInWorldTime;
    uint32 minRandomBotRandomizeTime, maxRandomBotRandomizeTime;
    uint32 minRandomBotChangeStrategyTime, maxRandomBotChangeStrategyTime;
    uint32 minRandomBotReviveTime, maxRandomBotReviveTime;
    uint32 minRandomBotPvpTime, maxRandomBotPvpTime;
    uint32 randomBotsMaxLoginsPerInterval;
    uint32 randomBotsPerInterval;
    uint32 minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval;
    //Auction house settings
    bool shouldQueryAHListingsOutsideOfAH;
    std::list<uint32> ahOverVendorItemIds;
    std::list<uint32> vendorOverAHItemIds;
    bool botCheckAllAuctionListings;
    bool botsSaveEpics;
    //
    bool randomBotJoinLfg;
    bool logRandomBotJoinLfg;
    bool randomBotJoinBG;
    bool randomBotAutoJoinBG;
    uint32 randomBotBracketCount;
    bool randomBotLoginAtStartup;
    uint32 randomBotTeleLevel;
    bool logInGroupOnly, logValuesPerTick;
    bool fleeingEnabled;
    bool summonAtInnkeepersEnabled;
    std::string combatStrategies, nonCombatStrategies, reactStrategies, deadStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies, randomBotReactStrategies, randomBotDeadStrategies;
    uint32 randomBotMinLevel, randomBotMaxLevel;
    float randomChangeMultiplier;
    uint32 specProbability[MAX_CLASSES][10];
    std::string premadeLevelSpec[MAX_CLASSES][10][91]; //lvl 10 - 100
    uint32 classRaceProbabilityTotal;
    uint32 classRaceProbability[MAX_CLASSES][MAX_RACES];
    bool useFixedClassRaceCounts;
    using ClassRacePair = std::pair<uint8, uint8>;
    std::map<ClassRacePair, uint32> fixedClassRaceCounts;
    uint32 levelProbability[DEFAULT_MAX_LEVEL + 1];
    ClassSpecs classSpecs[MAX_CLASSES];
    GlyphPrioritySpecMap glyphPriorityMap[MAX_CLASSES];
    bool gearProgressionSystemEnabled;
    uint32 gearProgressionSystemItemLevels[MAX_GEAR_PROGRESSION_LEVEL][2];
    int32 gearProgressionSystemItems[MAX_GEAR_PROGRESSION_LEVEL][MAX_CLASSES][4][SLOT_EMPTY];
    std::string commandPrefix, commandSeparator;
    std::string randomBotAccountPrefix;
    uint32 randomBotAccountCount;
    bool deleteRandomBotAccounts;
    uint32 randomBotGuildCount;
    bool deleteRandomBotGuilds;
    uint32 randomBotArenaTeamCount;
    bool deleteRandomBotArenaTeams;
    std::list<uint32> randomBotArenaTeams;
	bool RandombotsWalkingRPG;
	bool RandombotsWalkingRPGInDoors;
    bool humanLikeMovement;
    float humanLikePathJitterChance;
    float humanLikePathJitterRadius;
    float humanLikePathForwardJitterRadius;
    float humanLikeFollowAngleJitter;
    float humanLikeFollowDistanceJitter;
    float humanLikePauseChance;
    uint32 humanLikePauseMinMs;
    uint32 humanLikePauseMaxMs;
    uint32 humanLikePauseCooldownMs;
    float humanLikeJumpChance;
    float humanLikeSpinChance;
    uint32 humanLikeJumpCooldownMs;
    bool boostFollow;
    bool turnInRpg;
    bool globalSoundEffects;
    bool shareTargets;
    std::list<uint32> randomBotGuilds;
	std::list<uint32> pvpProhibitedZoneIds;
    bool enableGreet;
    bool randomBotShowHelmet;
    bool randomBotShowCloak;
    bool disableRandomLevels;
    bool instantRandomize;
    bool gearscorecheck;
    int32 levelCheck;
	bool randomBotPreQuests;
    float playerbotsXPrate;
    bool disableBotOptimizations;
    bool disableActivityPriorities;
    // LOD COLD tier: brain (DoNextAction) interval for bots with no real player in their
    // zone/map. 0 disables the COLD throttle. Slashes bot-AI execution volume for distant
    // bots -> large CPU + crash-rate reduction (crashes scale with execution volume).
    uint32 lodColdUpdateMs;
    // LOD COLD radius (yards): a bot with no real player within this range goes dormant.
    float lodColdRange;
    // 10K TWO-TIER FLEET: exactly ~ActiveCohortSize bots run fully active (full CPU, real
    // questing/killing); every other random bot goes LOD-dormant (invisible, brain parked)
    // unless a real player is nearby. Dormant bots still PROGRESS (xp/gold) via the synthetic
    // progression manager, at rates measured live from the active cohort. Cohort membership
    // rotates every CohortRotateMinutes so all bots get real playtime eventually.
    // ActiveCohortSize = 0 disables the split (everyone active).
    // humanized group-follow (world-anchored slots + reaction latency) for real-player masters
    bool looseFollow;
    uint32 instanceReactDelay;  // faster brain cadence (ms) for bots inside dungeons/raids/BGs
    // virtual observer: with no real players online, plant a synthetic "player" in the
    // world so the proximity allocator + active-vs-parked telemetry run and verify 24/7
    // one-time fleet level-spread: pace bots up to hash-assigned 1-60 targets so the world
    // is populated across all brackets/zones (5k illusion needs mid-level zones alive too)
    // CITY LIFE: a band of bots LIVE in capital cities (sit AFK with gear) to make the world
    // feel full. cityResidents = how many; cityResidentMinLevel = they skew high-level (60s).
    uint32 cityResidents;
    uint32 cityResidentMinLevel;
    bool dungeonLoiterers;   // rotate bots to loiter at dungeon/raid entrances (fake LFG groups)
    bool levelSpreadEnabled;
    uint32 levelSpreadPerTick;
    bool virtualObserver;
    uint32 virtualObserverRotateMinutes;
    uint32 activeBrainBudget;   // proximity allocator: how many bots run full brains (100-200)
    uint32 level60Target;       // grow/hold the fleet's L60 population at this size (promote+gear+teleport)
    float activeRenderRange;    // every bot within this range of a player gets a full-speed brain (uncapped)
    uint32 mockBgGames;         // spawn N bot-only WSGs at boot for autonomous tactics observation
    bool parkVisible;           // parked bots stay visible to populate the world
    uint32 activeCohortSize;    // legacy hash-cohort size (superseded by activeBrainBudget)
    uint32 cohortRotateMinutes;
    bool fastPathEnabled;   // UpdateAI not-due fast path; toggle for A/B + instant rollback
    bool syntheticProgressEnabled;
    float syntheticRateFactor;           // dormant progression as a fraction of measured active rates
    uint32 syntheticXpFallbackPerHour;   // used per bracket (scaled) until live rates exist
    uint32 syntheticGoldFallbackPerHour; // copper/hour fallback
    // SUPERVISOR MODE: keep the whole (small) fleet at full foreground cadence even with NO player
    // online, and write an honest per-second visible-activity trace (logs/supervisor.csv) so bot
    // behaviour can be watched/measured autonomously. Only sane for a small fleet (~50 bots).
    bool supervisorMode;
    // AUTONOMOUS FSM: deterministic state-machine brain that replaces the relevance-churn
    // decision-making for autonomous (no real master), out-of-combat, random bots. Fixes the
    // measured pathology (apm up to 966 with moving=0/combat=0 -- the engine re-picks an action
    // that never executes). Issues ONE concrete action then COMMITS (suppresses re-decision until
    // the action's observable effect resolves or a break event fires). Rollback: set to 0.
    bool autonomousFsm;
    // MIND: the bot intent layer (mind/ module) — owns every locomotion/pursuit
    // decision for autonomous random bots; the engine keeps stationary features
    // (buffs, food, loot dance, quest dialogs, packet reactions) + in-range combat
    // rotation. Supersedes AutonomousFSM + CombatDirector when on. Rollback: 0.
    bool mindEnabled;
    // COMBAT DIRECTOR: snappy "stick to your target" for autonomous random bots in combat. Fixes the
    // measured chase-stall (68% of in-combat bots not moving) + over-thinking (apm median 283, max 916)
    // by forcing a healthy melee bot that is out of range to CHASE its target (persistent MoveChase)
    // with a short re-check delay, instead of letting the engine dither/re-pick. Rollback: set to 0.
    bool combatDirector;
    // AUTONOMOUS PARTIES: form grinding parties from nearby active-cohort autonomous bots so the world
    // has real group content (bots stick to a leader + focus-fire like real players) instead of the
    // 0.7% that ever group. Formation runs on the world thread (creates Groups only, no AI mutation);
    // the follow/assist behavior lives in the map-thread FSM. Rollback: set to 0.
    bool autonomousParties;
    bool forceActiveWhenNearPlayer;
    bool limitCombatActivity;
    bool guildOrderAlwaysActive;
    uint32 botActiveAlone;
    uint32 diffWithPlayer;
    uint32 diffEmpty;
    uint32 minEnchantingBotLevel;
    uint32 randombotStartingLevel;
    bool randomBotSayWithoutMaster;
    bool randomBotInvitePlayer;
    bool randomBotGroupNearby;
    bool randomBotRaidNearby;
    bool randomBotGuildNearby;
    bool randomBotFormGuild;
    bool randomBotRandomPassword;
    bool inviteChat;
    bool botsSilent;
    // Opt-in diagnostic logging: when true, [BOT] log lines and per-bot action
    // log files (logs/bots/<name>_acc<id>_<timestamp>.log) are emitted. Default
    // off so production servers don't pay disk I/O / branch overhead.
    bool enableActionLog;
    bool learnedPolicyEnabled;
    bool enableOffSpecStrategies;
    bool useWanderAsDefaultFollowStrategy;
    std::string defaultFormation;

    uint32 guildMaxBotLimit;

    bool enableBroadcasts;
    uint32 broadcastChanceMaxValue;

    uint32 broadcastToGuildGlobalChance;
    uint32 broadcastToWorldGlobalChance;
    uint32 broadcastToGeneralGlobalChance;
    uint32 broadcastToTradeGlobalChance;
    uint32 broadcastToLFGGlobalChance;
    uint32 broadcastToLocalDefenseGlobalChance;
    uint32 broadcastToWorldDefenseGlobalChance;
    uint32 broadcastToGuildRecruitmentGlobalChance;
    uint32 broadcastToSayGlobalChance;
    uint32 broadcastToYellGlobalChance;

    uint32 broadcastChanceLootingItemPoor;
    uint32 broadcastChanceLootingItemNormal;
    uint32 broadcastChanceLootingItemUncommon;
    uint32 broadcastChanceLootingItemRare;
    uint32 broadcastChanceLootingItemEpic;
    uint32 broadcastChanceLootingItemLegendary;
    uint32 broadcastChanceLootingItemArtifact;

    uint32 broadcastChanceQuestAccepted;
    uint32 broadcastChanceQuestUpdateObjectiveCompleted;
    uint32 broadcastChanceQuestUpdateObjectiveProgress;
    uint32 broadcastChanceQuestUpdateFailedTimer;
    uint32 broadcastChanceQuestUpdateComplete;
    uint32 broadcastChanceQuestTurnedIn;

    uint32 broadcastChanceKillNormal;
    uint32 broadcastChanceKillElite;
    uint32 broadcastChanceKillRareelite;
    uint32 broadcastChanceKillWorldboss;
    uint32 broadcastChanceKillRare;
    uint32 broadcastChanceKillUnknown;
    uint32 broadcastChanceKillPet;
    uint32 broadcastChanceKillPlayer;

    uint32 broadcastChanceLevelupGeneric;
    uint32 broadcastChanceLevelupTenX;
    uint32 broadcastChanceLevelupMaxLevel;

    uint32 broadcastChanceSuggestInstance;
    uint32 broadcastChanceSuggestQuest;
    uint32 broadcastChanceSuggestGrindMaterials;
    uint32 broadcastChanceSuggestGrindReputation;
    uint32 broadcastChanceSuggestSell;
    uint32 broadcastChanceSuggestSomething;

    uint32 broadcastChanceSuggestSomethingToxic;

    uint32 broadcastChanceSuggestToxicLinks;
    std::string toxicLinksPrefix;
    uint32 toxicLinksRepliesChance;

    uint32 broadcastChanceSuggestThunderfury;
    uint32 thunderfuryRepliesChance;

    uint32 broadcastChanceGuildManagement;

    uint32 guildRepliesRate;

    uint32 botAcceptDuelMinimumLevel;

    bool talentsInPublicNote;
    bool nonGmFreeSummon;

    BotSelfBotLevel selfBotLevel;
    uint32 iterationsPerTick;

    std::string autoPickReward;
    bool autoEquipUpgradeLoot;
    bool syncQuestWithPlayer;
    bool syncQuestForPlayer;
    std::string autoTrainSpells;
    std::string autoPickTalents;
    bool autoLearnTrainerSpells;
    bool autoLearnQuestSpells;
    bool autoLearnDroppedSpells;
    bool autoDoQuests;
    bool syncLevelWithPlayers;
    uint32 syncLevelMaxAbove, syncLevelNoPlayer;
    bool syncAltLevelToMaster;
    uint32 tweakValue; //Debugging config
    float respawnModNeutral, respawnModHostile;
    uint32 respawnModThreshold, respawnModMax;
    bool respawnModForPlayerBots, respawnModForInstances;
    bool learningTelemetryEnabled;
    uint32 learningTaskSampleIntervalMs;
    uint32 learningFlushIntervalMs;
    uint32 learningFlushMaxRows;
    uint32 learningMaxQueue;
    float learningCombatSampleRate;

    bool randomBotLoginWithPlayer;
    bool asyncBotLogin, preloadHolders;
    uint32 freeRoomForNonSpareBots;
    uint32 loginBotsNearPlayerRange;
    std::vector<std::string> defaultLoginCriteria;
    std::vector<std::vector<std::string>> loginCriteria;

    bool jumpInBg;
    bool jumpWithPlayer;
    bool jumpFollow;
    bool jumpChase;
    bool useKnockback;
    float jumpNoCombatChance;
    float jumpMeleeInCombatChance;
    float jumpRandomChance;
    float jumpInPlaceChance;
    float jumpBackwardChance;
    float jumpHeightLimit;
    float jumpVSpeed;
    float jumpHSpeed;

    std::mutex m_logMtx;

    std::list<std::string> allowedLogFiles;
    std::list<std::string> excludedBotEvents;
    std::list<std::string> debugFilter;

    std::unordered_map <std::string, std::pair<FILE*, bool>> logFiles;
    std::unordered_map <std::string, uint32> logFlushCounters;

    uint32 botCheatMask = 0;
    uint32 rndBotCheatMask = 0;

    std::vector<std::string> BotCheatMaskName = { "taxi", "gold", "health", "mana", "power", "item", "cooldown", "repair", "movespeed", "attackspeed", "breath", "glyph", "quest", "maxMask" };

    struct worldBuff{
        uint32 spellId;
        uint32 factionId = 0;
        uint32 classId = 0;
        uint32 specId = 0;
        uint32 minLevel = 0;
        uint32 maxLevel = 0;
        uint32 eventId = 0;
    };

    std::vector<worldBuff> worldBuffs;

    int commandServerPort;
    bool perfMonEnabled;
    bool bExplicitDbStoreSave = false;

    //LM BEGIN
    std::string llmApiEndpoint, llmApiKey, llmApiJson, llmPrePrompt, llmPreRpgPrompt, llmPrompt, llmPostPrompt, llmResponseStartPattern, llmResponseEndPattern, llmResponseDeletePattern, llmResponseSplitPattern;
    uint32 llmEnabled, llmContextLength, llmBotToBotChatChance, llmGenerationTimeout, llmMaxSimultaniousGenerations, llmRpgAIChatChance;
    bool llmCommandBridgeEnabled;
    std::string llmCommandPrompt;
    bool llmGlobalContext;
    ParsedUrl llmEndPointUrl;
    std::set<uint32> llmBlockedReplyChannels;
    //LM END

    uint32 EatDrinkMinDistance = 5;
    uint32 EatDrinkMaxDistance = 1000;

    std::string GetValue(std::string name);
    void SetValue(std::string name, std::string value);

    void loadFreeAltBotAccounts();

    std::string GetTimestampStr();

    bool hasLog(std::string fileName) { return std::find(allowedLogFiles.begin(), allowedLogFiles.end(), fileName) != allowedLogFiles.end(); };
    bool shouldLogBotEvent(const std::string& eventName) const;
    bool openLog(std::string fileName, char const* mode = "a", bool haslog = false);
    bool isLogOpen(std::string fileName) { auto it = logFiles.find(fileName); return it != logFiles.end() && it->second.second;}
    void log(std::string fileName, const char* str, ...);

    void logEvent(PlayerbotAI* ai, std::string eventName, std::string info1 = "", std::string info2 = "");
    void logEvent(PlayerbotAI* ai, std::string eventName, ObjectGuid guid, std::string info2);

    bool CanLogAction(PlayerbotAI* ai, std::string actionName, bool isExecute, std::string lastActionName);

private:
    void LoadTalentSpecs();
    void LoadLLMDefaultPrompts(const std::string& fileName);

    Config config;
};

#define sPlayerbotAIConfig MaNGOS::Singleton<PlayerbotAIConfig>::Instance()
