#pragma once

#include <atomic>

namespace mind
{
    // Fleet-wide per-minute counters -> logs/mind.csv. Counts ACTIONS the
    // world can observe (engages, loots, quest turn-ins, arrivals), plus the
    // failure modes we hunt (stucks, blacklists, idle fallbacks). O(1) atomic
    // bumps on bot ticks; one line per minute from whichever tick crosses the
    // minute boundary (same pattern as fsm_branch.csv).
    struct Counters
    {
        std::atomic<uint32> steps{0};        // mind proactive steps
        std::atomic<uint32> engage{0};       // committed to a kill target
        std::atomic<uint32> chase{0};        // combat chase ticks forced
        std::atomic<uint32> lootRuns{0};     // committed to a corpse/node
        std::atomic<uint32> lootOpens{0};    // CMSG_LOOT dispatched
        std::atomic<uint32> social{0};       // follow/assist ticks
        std::atomic<uint32> errands{0};      // quest/town NPC interactions started
        std::atomic<uint32> journey{0};      // walking-to-content ticks
        std::atomic<uint32> arrivals{0};     // journey destinations reached
        std::atomic<uint32> stucks{0};       // stuck-detector breaks
        std::atomic<uint32> blacklists{0};   // objects given up on
        std::atomic<uint32> teleports{0};    // unseen stuck-escape teleports
        std::atomic<uint32> idleFallback{0}; // had NOTHING to do (must stay ~0)
        std::atomic<uint32> maintenance{0};  // engine maintenance bursts granted

        // Target-scan diagnosis (why engage stays low): per scan outcome +
        // per-candidate reject reasons.
        std::atomic<uint32> scans{0};        // scans run
        std::atomic<uint32> scanEmpty{0};    // grid returned zero candidates
        std::atomic<uint32> scanFound{0};    // scan pinned a target
        std::atomic<uint32> rejUsable{0};    // IsUsableKillTarget said no (sum of below + misc)
        std::atomic<uint32> rejPack{0};      // pack >= 3 links (solo, non-quest)
        std::atomic<uint32> engageRefused{0};// "attack" action declined the pinned target

        // rejUsable split (why candidates die):
        std::atomic<uint32> rejNotCreature{0}; // player/pet/totem
        std::atomic<uint32> rejInvalid{0};     // IsValidAttackTarget false
        std::atomic<uint32> rejLevel{0};       // > bot level + 2
        std::atomic<uint32> rejNoXp{0};        // grey/no-xp, no quest credit
        std::atomic<uint32> rejTapped{0};      // someone else's tap
        std::atomic<uint32> rejFighting{0};    // fighting another player's fight
        std::atomic<uint32> rejEliteCrit{0};   // elite (solo) or critter
    };

    Counters& Log();
    void FlushLogIfDue(uint32 nowMs);
}
