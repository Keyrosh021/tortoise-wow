-- Paladin spells

-- Holy Might needs its own family bit; the flattened import lost the 64-bit mask.
REPLACE INTO `spell_mod` (`Id`, `SpellFamilyFlags`, `Comment`)
VALUES
    (51350, 2199023255552, 'Holy Might rank 1: family mask'),
    (51351, 2199023255552, 'Holy Might rank 2: family mask'),
    (51352, 2199023255552, 'Holy Might rank 3: family mask'),
    (51353, 2199023255552, 'Holy Might rank 4: family mask'),
    (51354, 2199023255552, 'Holy Might rank 5: family mask');

-- Holy Strike procs are handled in C++; keep these auras from firing off unrelated spells.
REPLACE INTO `spell_mod` (`Id`, `procChance`, `procFlags`, `Comment`)
VALUES
    (51355, 0, 0, 'Holy Strike rank 1: C++ proc'),
    (51356, 0, 0, 'Holy Strike rank 2: C++ proc'),
    (51357, 0, 0, 'Holy Strike rank 3: C++ proc'),
    (51358, 0, 0, 'Holy Strike rank 4: C++ proc'),
    (51359, 0, 0, 'Holy Strike rank 5: C++ proc');

-- Crusader Strike final damage is 25/40/56/70/90%.
-- Effect 31 stores weapon damage as "percent minus 1".
-- Bind the ranks and helper spells to the Crusader Strike family bit.
REPLACE INTO `spell_mod` (`Id`, `SpellFamilyName`, `SpellFamilyFlags`, `Comment`)
VALUES
    (7297, 10, 34359738368, 'Crusader Strike rank 1: family mask'),
    (8825, 10, 34359738368, 'Crusader Strike rank 2: family mask'),
    (8826, 10, 34359738368, 'Crusader Strike rank 3: family mask'),
    (10338, 10, 34359738368, 'Crusader Strike rank 4: family mask'),
    (10339, 10, 34359738368, 'Crusader Strike rank 5: family mask'),
    (47314, 10, 34359738368, 'Crusader Strike rank 1 helper: family mask'),
    (47315, 10, 34359738368, 'Crusader Strike rank 2 helper: family mask'),
    (47316, 10, 34359738368, 'Crusader Strike rank 3 helper: family mask'),
    (47317, 10, 34359738368, 'Crusader Strike rank 4 helper: family mask'),
    (47318, 10, 34359738368, 'Crusader Strike rank 5 helper: family mask');

REPLACE INTO `spell_effect_mod` (`Id`, `EffectIndex`, `EffectBasePoints`, `Comment`)
VALUES
    (47314, 0, 24, 'Crusader Strike rank 1: 25% weapon damage'),
    (47315, 0, 39, 'Crusader Strike rank 2: 40% weapon damage'),
    (47316, 0, 55, 'Crusader Strike rank 3: 56% weapon damage'),
    (47317, 0, 69, 'Crusader Strike rank 4: 70% weapon damage'),
    (47318, 0, 89, 'Crusader Strike rank 5: 90% weapon damage');

-- 70% threat reduction on all Exorcism ranks.
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

-- Righteous Strikes procs from Crusader Strike.
DELETE FROM `spell_proc_event`
WHERE `entry` IN (51341, 51342, 51343, 51344, 51345);

REPLACE INTO `spell_proc_event`
VALUES
    (51341, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51342, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51343, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51344, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0),
    (51345, 0, 10, 34359738368, 0, 0, 0, 0, 0, 100, 0);

-- Blessed Strikes checks Crusader Strike at cast end, including misses and dodges.
DELETE FROM `spell_proc_event`
WHERE `entry` IN (51317, 51318, 51319, 51320, 51321);

REPLACE INTO `spell_proc_event`
VALUES
    (51317, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51318, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51319, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51320, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0),
    (51321, 0, 10, 34359738368, 0, 0, 69904, 524288, 0, 0, 0);

-- These spell_affect rows modify Holy Strike, not the Crusader Strike trigger.
DELETE FROM `spell_affect`
WHERE `entry` IN (51317, 51318, 51319, 51320, 51321, 51341, 51342, 51343, 51344, 51345)
    AND `effectId` IN (1, 2);

REPLACE INTO `spell_affect` (`entry`, `effectId`, `SpellFamilyMask`)
VALUES
        (51317, 1, 4294967296),
        (51317, 2, 4294967296),
        (51318, 1, 4294967296),
        (51318, 2, 4294967296),
        (51319, 1, 4294967296),
        (51319, 2, 4294967296),
        (51320, 1, 4294967296),
        (51320, 2, 4294967296),
        (51321, 1, 4294967296),
        (51321, 2, 4294967296),
        (51341, 1, 4294967296),
        (51341, 2, 4294967296),
        (51342, 1, 4294967296),
        (51342, 2, 4294967296),
        (51343, 1, 4294967296),
        (51343, 2, 4294967296),
        (51344, 1, 4294967296),
        (51344, 2, 4294967296),
        (51345, 1, 4294967296),
        (51345, 2, 4294967296);

-- Holy Shock rank 4 rank-chain fix; needed for respec cleanup.
REPLACE INTO `spell_chain` (`spell_id`, `prev_spell`, `first_spell`, `rank`, `req_spell`)
VALUES
    (51786, 20930, 20473, 4, 0);

-- Zealous Defense should spend its charge only on a successful block.
DELETE FROM `spell_proc_event`
WHERE `entry` IN (51336, 51337, 51338, 51339, 51340);

REPLACE INTO `spell_proc_event`
VALUES
    (51336, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51337, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51338, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51339, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0),
    (51340, 0, 0, 0, 0, 0, 680, 64, 0, 100, 0);

-- Daybreak only applies from critical heals.
REPLACE INTO `spell_proc_event`
VALUES
    (51323, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0);

-- Shield Specialization mana restore cooldown.
REPLACE INTO `spell_proc_event`
VALUES
    (20148, 0, 0, 0, 0, 0, 0, 64, 0, 0, 5),
    (20149, 0, 0, 0, 0, 0, 0, 64, 0, 0, 5),
    (20150, 0, 0, 0, 0, 0, 0, 64, 0, 0, 5);

-- Daybreak retune: flat 248 base healing, 32% scaling in SpellEntry.cpp.
REPLACE INTO `spell_effect_mod` (`Id`, `EffectIndex`, `EffectDieSides`, `Comment`)
VALUES
    (50931, 0, 1, 'Daybreak: base healing reduced to flat 248');

-- Holy Shield retune: visible buff values plus threat on the hidden proc ranks.
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