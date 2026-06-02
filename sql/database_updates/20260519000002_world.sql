-- Paladin Spells--

-- Restore the Holy Might ranks to the Holy Strike family flag.
-- The imported flattened rows lost the full 64-bit value and stopped matching Paladin proc filters.
REPLACE INTO `spell_mod` (`Id`, `SpellFamilyFlags`, `Comment`)
VALUES
    (51350, 2199023255552, 'Holy Might rank 1: restore Holy Strike family flag'),
    (51351, 2199023255552, 'Holy Might rank 2: restore Holy Strike family flag'),
    (51352, 2199023255552, 'Holy Might rank 3: restore Holy Strike family flag'),
    (51353, 2199023255552, 'Holy Might rank 4: restore Holy Strike family flag'),
    (51354, 2199023255552, 'Holy Might rank 5: restore Holy Strike family flag');

-- Holy Strike proc handling is now done in C++ at cast time. Keep the source auras from
-- auto-proccing through broad spell_template proc flags, or they will fire from unrelated spells.
REPLACE INTO `spell_mod` (`Id`, `procChance`, `procFlags`, `Comment`)
VALUES
    (51355, 0, 0, 'Holy Strike rank 1: proc handling moved to C++'),
    (51356, 0, 0, 'Holy Strike rank 2: proc handling moved to C++'),
    (51357, 0, 0, 'Holy Strike rank 3: proc handling moved to C++'),
    (51358, 0, 0, 'Holy Strike rank 4: proc handling moved to C++'),
    (51359, 0, 0, 'Holy Strike rank 5: proc handling moved to C++');

-- Blessings: align server-side aura durations with the client Spell.dbc values so the actual buff
-- timer matches the 10 minute / 30 minute tooltip durations shown in game.
REPLACE INTO `spell_mod` (`Id`, `DurationIndex`, `Comment`)
VALUES
    (1038, 6, 'Blessing duration: 10 minutes'),
    (19740, 6, 'Blessing duration: 10 minutes'),
    (19742, 6, 'Blessing duration: 10 minutes'),
    (19834, 6, 'Blessing duration: 10 minutes'),
    (19835, 6, 'Blessing duration: 10 minutes'),
    (19836, 6, 'Blessing duration: 10 minutes'),
    (19837, 6, 'Blessing duration: 10 minutes'),
    (19838, 6, 'Blessing duration: 10 minutes'),
    (19850, 6, 'Blessing duration: 10 minutes'),
    (19852, 6, 'Blessing duration: 10 minutes'),
    (19853, 6, 'Blessing duration: 10 minutes'),
    (19854, 6, 'Blessing duration: 10 minutes'),
    (19977, 6, 'Blessing duration: 10 minutes'),
    (19978, 6, 'Blessing duration: 10 minutes'),
    (19979, 6, 'Blessing duration: 10 minutes'),
    (20217, 6, 'Blessing duration: 10 minutes'),
    (20911, 6, 'Blessing duration: 10 minutes'),
    (20912, 6, 'Blessing duration: 10 minutes'),
    (20913, 6, 'Blessing duration: 10 minutes'),
    (20914, 6, 'Blessing duration: 10 minutes'),
    (25290, 6, 'Blessing duration: 10 minutes'),
    (25291, 6, 'Blessing duration: 10 minutes'),
    (25782, 30, 'Greater Blessing duration: 30 minutes'),
    (25890, 30, 'Greater Blessing duration: 30 minutes'),
    (25894, 30, 'Greater Blessing duration: 30 minutes'),
    (25895, 30, 'Greater Blessing duration: 30 minutes'),
    (25898, 30, 'Greater Blessing duration: 30 minutes'),
    (25899, 30, 'Greater Blessing duration: 30 minutes'),
    (25916, 30, 'Greater Blessing duration: 30 minutes'),
    (25918, 30, 'Greater Blessing duration: 30 minutes');

-- Sacred Duty's removed passive rows are left untouched in imported spell_template. 

-- Crusader Strike rank damage remained at the earlier 35/50/66/80/100% values in imported data.
-- Spell effect 31 stores weapon damage as "percent minus 1", so the later 10-point nerf becomes
-- 24/39/55/69/89 here to land on 25/40/56/70/90% weapon damage in game.
REPLACE INTO `spell_effect_mod` (`Id`, `EffectIndex`, `EffectBasePoints`, `Comment`)
VALUES
    (47314, 0, 24, 'Crusader Strike rank 1: 25% weapon damage'),
    (47315, 0, 39, 'Crusader Strike rank 2: 40% weapon damage'),
    (47316, 0, 55, 'Crusader Strike rank 3: 56% weapon damage'),
    (47317, 0, 69, 'Crusader Strike rank 4: 70% weapon damage'),
    (47318, 0, 89, 'Crusader Strike rank 5: 90% weapon damage');

-- 70% threat reduction on all Exorcism ranks. UPDATES | Dated: December 20 2024 [Revised: December 22]
DELETE FROM `spell_threat`
WHERE `entry` IN (879, 5614, 5615, 10312, 10313, 10314);

INSERT INTO `spell_threat` (`entry`, `Threat`, `multiplier`, `ap_bonus`)
VALUES
    (879, 0, 0.3, 0),
    (5614, 0, 0.3, 0),
    (5615, 0, 0.3, 0),
    (10312, 0, 0.3, 0),
    (10313, 0, 0.3, 0),
    (10314, 0, 0.3, 0);

-- Redemption 6-piece Exorcism cooldown bonus stores its reduction in milliseconds.
-- Keep the base points at -5001 so the aura resolves to a 5 second cooldown reduction.
REPLACE INTO `spell_mod` (`Id`, `SpellFamilyName`, `Comment`)
VALUES
    (51823, 10, 'Redemption 6-piece: paladin family binding');

REPLACE INTO `spell_effect_mod` (`Id`, `EffectIndex`, `EffectBasePoints`, `Comment`)
VALUES
    (51823, 0, -5001, 'Redemption 6-piece: 5 second Exorcism cooldown reduction');

-- The cooldown modifier only applies at runtime when the aura is bound to Exorcism's family mask.
REPLACE INTO `spell_affect` (`entry`, `effectId`, `SpellFamilyMask`)
VALUES
    (51823, 0, 1099511627776);

-- Righteous Strikes procs from Crusader Strike, which is CM1 00000008 / absolute mask 34359738368.
DELETE FROM `spell_proc_event`
WHERE `entry` IN (51341, 51342, 51343, 51344, 51345);

REPLACE INTO `spell_proc_event`
VALUES
    (51341, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51342, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51343, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51344, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51345, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0);

-- Blessed Strikes should evaluate on Crusader Strike cast end so missed, dodged, and parried
-- casts still reach the same proc path as successful casts.
DELETE FROM `spell_proc_event`
WHERE `entry` IN (51317, 51318, 51319, 51320, 51321);

REPLACE INTO `spell_proc_event`
VALUES
    (51317, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51318, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51319, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51320, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51321, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0);

-- Blessed Strikes and Righteous Strikes still point at an older Paladin family mask in
-- spell_affect, so generic aura-to-spell matching misses Crusader Strike unless these rows
-- are rebound to Crusader Strike's live family bit.
DELETE FROM `spell_affect`
WHERE `entry` IN (51317, 51318, 51319, 51320, 51321, 51341, 51342, 51343, 51344, 51345)
    AND `effectId` IN (1, 2);

REPLACE INTO `spell_affect` (`entry`, `effectId`, `SpellFamilyMask`)
VALUES
        (51317, 1, 34359738368),
        (51317, 2, 34359738368),
        (51318, 1, 34359738368),
        (51318, 2, 34359738368),
        (51319, 1, 34359738368),
        (51319, 2, 34359738368),
        (51320, 1, 34359738368),
        (51320, 2, 34359738368),
        (51321, 1, 34359738368),
        (51321, 2, 34359738368),
        (51341, 1, 34359738368),
        (51341, 2, 34359738368),
        (51342, 1, 34359738368),
        (51342, 2, 34359738368),
        (51343, 1, 34359738368),
        (51343, 2, 34359738368),
        (51344, 1, 34359738368),
        (51344, 2, 34359738368),
        (51345, 1, 34359738368),
        (51345, 2, 34359738368);

-- Holy Shock rank 4 fix rank chain so it gets droppd on respec
REPLACE INTO `spell_chain` (`spell_id`, `prev_spell`, `first_spell`, `rank`, `req_spell`)
VALUES
    (51786, 20930, 20473, 4, 0);

-- Zealous Defense should only spend its single charge on a successful block, not on any hit taken.
DELETE FROM `spell_proc_event`
WHERE `entry` IN (51336, 51337, 51338, 51339, 51340);

REPLACE INTO `spell_proc_event`
VALUES
    (51336, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51337, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51338, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51339, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51340, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0);

-- Daybreak should only apply from critical heals
REPLACE INTO `spell_proc_event`
VALUES
    (51323, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0);

-- Shield Specialization mana restore should only proc once every 5 seconds.
REPLACE INTO `spell_proc_event`
VALUES
    (20148, 0, 0, 0, 0, 0, 0, 64, 0, 0, 5),
    (20149, 0, 0, 0, 0, 0, 0, 64, 0, 0, 5),
    (20150, 0, 0, 0, 0, 0, 0, 64, 0, 0, 5);

-- Daybreak: base healing value reduced from 289 to 248 (EffectDieSides 42 -> 1).
-- Healing power scaling reduced from 43% to 32% via CalculateCustomCoefficient in SpellEntry.cpp.
REPLACE INTO `spell_effect_mod` (`Id`, `EffectIndex`, `EffectDieSides`, `Comment`)
VALUES
    (50931, 0, 1, 'Daybreak: base healing reduced to flat 248');

-- Holy Shield's page-42 follow-up landed as both a tooltip/base-value retune on the buff ranks
-- and a real threat increase on the hidden proc-damage ranks. Keep both pieces pinned here so the
-- visible 50% wording matches the actual aggro generated by the Holy Shield damage procs.
REPLACE INTO `spell_mod` (`Id`, `manaCost`, `Comment`)
VALUES
    (20925, 135, 'Holy Shield rank 1: mana retune'),
    (20927, 175, 'Holy Shield rank 2: mana retune'),
    (20928, 215, 'Holy Shield rank 3: mana retune');

REPLACE INTO `spell_effect_mod` (`Id`, `EffectIndex`, `EffectBasePoints`, `Comment`)
VALUES
    (20925, 0, 44, 'Holy Shield rank 1: block chance retune'),
    (20927, 0, 44, 'Holy Shield rank 2: block chance retune'),
    (20928, 0, 44, 'Holy Shield rank 3: block chance retune'),
    (20925, 1, 34, 'Holy Shield rank 1: damage retune'),
    (20927, 1, 47, 'Holy Shield rank 2: damage retune'),
    (20928, 1, 64, 'Holy Shield rank 3: damage retune');

DELETE FROM `spell_threat`
WHERE `entry` IN (20955, 20956, 20957);

INSERT INTO `spell_threat` (`entry`, `Threat`, `multiplier`, `ap_bonus`)
VALUES
    (20955, 0, 1.5, 0),
    (20956, 0, 1.5, 0),
    (20957, 0, 1.5, 0);