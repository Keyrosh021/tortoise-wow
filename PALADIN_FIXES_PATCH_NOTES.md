# Paladin Fixes Patch Notes

- Holy Strike / Holy Might
  - Holy Might ranks 51350-51354 were rebound to the correct Holy Strike family flag so proc and aura matching works again.
  - Holy Strike helper auras 51355-51359 no longer auto-proc from unrelated spells through overly broad proc flags.
  - Holy Might now refreshes only from actual Holy Strike casts, so missed, dodged, or parried Holy Strikes do not incorrectly drop the buff.
  - Fixed the healing and mana back for Holy Strikes.

- Crusader Strike
  - Crusader Strike ranks 47314-47318 were retuned from the older 35/50/66/80/100% values to 25/40/56/70/90% weapon damage.
  - Crusader Strike now uses normalized weapon damage handling.
  - Crusader Strike now gains 20% of Holy spell power as bonus damage.
  - Blessed Strikes and Righteous Strikes now correctly recognize live Crusader Strike ranks and helper spell variants even when family flags are incomplete.

- Judgment of Righteousness
  - Now uses melee miss/crit values.

- Seal/Judgment of Wisdom
  - Added handlers for the missing 2 new ranks

- Repentance
  - Now applies the Repent debuff on Immune targets lasting 20 seconds with the 20% scaling from attack power.
  - Fixed Repent spell family so it wouldnt get overriden by judgments.

- Judgement of the Crusader and holy-damage interactions
  - Judgement of the Crusader target-side scaling was retuned using explicit coefficients for paladin holy finishers.
  - Judgement of Righteousness now displays as a Judgement instead of the seal-style damage shell.
  - Judgement of Righteousness now uses melee hit and melee crit rules instead of magic hit and spell crit rules.
  - Judgement of Righteousness no longer double-applies percent damage modifiers during damage calculation.
  - Judgement of Righteousness target-side coefficient is fixed at 50%, while the caster-side shell keeps its 73% spellpower behavior.
  - Exorcism target-side Judgement of the Crusader scaling is fixed at 43%.
  - Hammer of Wrath target-side Judgement of the Crusader scaling is fixed at 28.6%.
  - Holy Wrath target-side Judgement of the Crusader scaling is fixed at 19%.
  - Old helper Judgement of the Crusader rows were removed from explicit runtime classification in both C++ repos, leaving only the real learned ranks in the switch.
  - Needs more tuning and research for closer damage scaling per spells.

- Judgement of Wisdom
  - Custom Judgement of Wisdom ranks 4 and 5 now trigger their matching mana-return spells correctly.

- Consecration
  - Consecration periodic has the frontloaded progression instead of flat repeated ticks.
  - Base damage per tick is increased by 8% before the frontload curve is applied.
  - Judgement of the Crusader contribution is distributed evenly across Consecration ticks with a per-tick coefficient of 0.04125.

- Holy Shock
  - Holy Shock damage uses a 43% spellpower coefficient.
  - Holy Shock rank 4 spell-chain data was fixed so it drops correctly on respec.
  - Blessed Strikes can now properly reset Holy Shock cooldowns when triggered by Crusader Strike.
  - Added the 5% chance and +1% per 100 spellpower chance to reset cooldown.

- Exorcism
  - All Exorcism ranks now generate 70% less threat.
  - The Redemption 6-piece cooldown bonus is rebound correctly so it applies a real 5 second Exorcism cooldown reduction.
  - The Redemption T3 6-piece aura is now applied and removed correctly at runtime even when the ItemSet DBC entry is stale.

- Divine Strike
  - Divine Strike now scales for 0.3% spellpower.

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

- Blessings and Greater Blessings
  - Standard Blessings now last 10 minutes.
  - Greater Blessings now last 30 minutes.

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
  - Righteous Strikes spell_proc_event rows were rebound to Crusader Strike's active family bit.
  - Associated spell_affect rows were updated so generic aura-to-spell matching reaches Crusader Strike correctly.


