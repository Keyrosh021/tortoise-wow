# Paladin Fixes Patch Notes

- The Weapon Skill formula has been altered.

- Holy Strike / Holy Might
  - Holy Might ranks 51350-51354 were rebound to the correct Holy Might family flag so proc and aura matching works again.
  - Holy Strike helper auras 51355-51359 no longer auto-proc from unrelated spells through overly broad proc flags.
  - Holy Might now refreshes only from actual Holy Strike casts, so missed, dodged, or parried Holy Strikes do not incorrectly drop the buff.
  - Fixed the healing and mana back for Holy Strikes to also hit 4 nearby allies regardless of party.
  - Added the custom 71.4% spellpower scaling.

- Crusader Strike
  - Crusader Strike helper ranks 47314-47318 teach the actual cast spells, with final weapon damage values 35/50/66/80/100%.
  - Crusader Strike ranks and helper spells now use the Paladin Crusader Strike family mask.
  - Crusader Strike now uses normalized weapon damage handling.
  - Crusader Strike now gains 20% of Holy spell power as bonus damage.
  - Blessed Strikes and Righteous Strikes now correctly recognize live Crusader Strike ranks and helper spell variants even when family flags are incomplete.

- Judgment of Righteousness
  - Now uses melee miss/crit values.

- Seal and Judgment of Wisdom
  - Added handlers for the missing 2 new ranks.

- Seal and Judgement of Command
  - Seal of Command PPM increased from 7 to 9.
  - Seal of Command now has a small Spell Power modifier, and Judgement of Command now has a small Attack Power modifier.

- Repentance
  - Now applies the Repent debuff on Immune targets lasting 20 seconds with the 8% scaling from attack power.
  - Fixed Repent spell family so it wouldnt get overriden by judgments.

- Judgement of the Crusader
  - Judgement of the Crusader target-side scaling was retuned using explicit coefficients for paladin holy finishers.
  - Judgement of Righteousness target-side coefficient is fixed at 50%.
  - Needs more tuning and research for closer damage scaling per spells.

- Consecration
  - Consecration periodic has the frontloaded progression instead of flat repeated ticks.
  - Base damage per tick is increased by 8% before the frontload curve is applied.

- Holy Shock
  - Holy Shock damage uses a 43% spellpower coefficient.
  - Holy Shock rank 4 spell-chain data was fixed so it drops correctly on respec.
  - Blessed Strikes can now properly reset Holy Shock cooldowns when triggered by Crusader Strike.
  - Added the 5% chance and +1% per 100 spellpower chance to reset cooldown.

- Exorcism
  - All Exorcism ranks now generate 70% less threat.

- Divine Strike
  - Divine Strike scales with 0.3% spellpower.

- Holy Shield
  - Holy Shield retuned for mana cost, block chance, and on-hit damage.
  - Holy Shield proc-damage ranks fixed 1.5x threat multiplier so threat matches the intended behavior.
  - Holy Shield damage now scales from 15% spellpower.
  - Holy Shield is intentionally excluded from the newer target-side Judgement of the Crusader tuning pending further verification.

- Blessing of Sanctuary
  - Blessing of Sanctuary blocked-hit Holy damage fixed to 1% spellpower.

- Retribution Aura
  - Retribution Aura scales from 1% spellpower.

- Shield Specialization
  - Shield Specialization mana-on-block now respects a 5 second internal cooldown.

- Righteous Defense
  - Righteous Defense talent ranks 51328-51330 now only function while Righteous Fury is active.
  - The triggered damage-taken reduction aura is refreshed instead of stacking multiple overlapping copies.

- Zealous Defense / Righteous Strikes
  - Zealous Defense charges are now consumed only by successful blocks instead of any incoming hit.
  - Righteous Strikes correctly procs from Crusader Strike.
  - Righteous Strikes target-trigger handling now refreshes the one-hit absorb instead of leaving stale state behind.
  - The aura 224 blocked-damage reduction is now applied correctly to blocked melee swings and blocked melee/ranged spell hits.

- Blessing of Sacrifice
  - Redirected Blessing of Sacrifice damage no longer breaks crowd control on the damage recipient.

- Daybreak
  - Daybreak now only procs from critical heals.
  - Base healing was reduced from 289 to a flat 248.
  - Healing spellpower scaling was reduced from 43% to 32%.

- Holy Light
  - Holy Light rank 1 now uses a 20% spellpower coefficient.

- Blessed Strikes
  - Blessed Strikes now evaluates on Crusader Strike cast end, so missed, dodged, and parried casts still follow the intended proc path.

- Righteous Strikes
  - Righteous Strikes ranks 51341-51345 now proc from Crusader Strike.
