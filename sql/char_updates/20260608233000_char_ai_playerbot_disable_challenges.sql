UPDATE `characters`
SET `mortality_status` = 0
WHERE `guid` IN (SELECT DISTINCT `bot` FROM `ai_playerbot_random_bots`);

DELETE FROM `character_spell`
WHERE `spell` IN (50000, 50004, 50008, 50001, 50014, 50071)
  AND `guid` IN (SELECT DISTINCT `bot` FROM `ai_playerbot_random_bots`);

DELETE FROM `character_aura`
WHERE `spell` IN (50000, 50004, 50008, 50001, 50014, 50071)
  AND `guid` IN (SELECT DISTINCT `bot` FROM `ai_playerbot_random_bots`);

DELETE FROM `character_aura_suspended`
WHERE `spell` IN (50000, 50004, 50008, 50001, 50014, 50071)
  AND `guid` IN (SELECT DISTINCT `bot` FROM `ai_playerbot_random_bots`);
