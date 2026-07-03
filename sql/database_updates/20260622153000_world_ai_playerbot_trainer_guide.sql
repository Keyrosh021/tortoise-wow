CREATE TABLE IF NOT EXISTS `ai_playerbot_trainer_guide` (
  `trainer_type` TINYINT UNSIGNED NOT NULL,
  `requirement` INT UNSIGNED NOT NULL DEFAULT 0,
  `trainer_entry` MEDIUMINT UNSIGNED NOT NULL,
  `priority` INT UNSIGNED NOT NULL DEFAULT 0,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `note` VARCHAR(100) NOT NULL DEFAULT '',
  PRIMARY KEY (`trainer_type`, `requirement`, `trainer_entry`),
  KEY `idx_type_req_priority` (`trainer_type`, `requirement`, `enabled`, `priority`)
);

INSERT IGNORE INTO `ai_playerbot_trainer_guide`
  (`trainer_type`, `requirement`, `trainer_entry`, `priority`, `enabled`, `note`)
SELECT
  `trainer_type`,
  CASE
    WHEN `trainer_type` IN (0, 3) THEN `trainer_class`
    WHEN `trainer_type` = 1 THEN `trainer_race`
    ELSE 0
  END AS `requirement`,
  `entry`,
  0,
  1,
  LEFT(`name`, 100)
FROM `creature_template`
WHERE `trainer_type` IN (0, 1, 2, 3)
  AND (`trainer_type` = 2 OR `trainer_class` <> 0 OR `trainer_race` <> 0);

INSERT IGNORE INTO `ai_playerbot_trainer_guide`
  (`trainer_type`, `requirement`, `trainer_entry`, `priority`, `enabled`, `note`)
SELECT
  2,
  nt.`reqskill`,
  ct.`entry`,
  10,
  1,
  LEFT(CONCAT(ct.`name`, ' skill route'), 100)
FROM `creature_template` ct
JOIN `npc_trainer` nt ON nt.`entry` = IF(ct.`trainer_id` <> 0, ct.`trainer_id`, ct.`entry`)
WHERE ct.`trainer_type` = 2
  AND nt.`reqskill` <> 0;

INSERT IGNORE INTO `ai_playerbot_trainer_guide`
  (`trainer_type`, `requirement`, `trainer_entry`, `priority`, `enabled`, `note`)
SELECT
  2,
  ntt.`reqskill`,
  ct.`entry`,
  10,
  1,
  LEFT(CONCAT(ct.`name`, ' skill route'), 100)
FROM `creature_template` ct
JOIN `npc_trainer_template` ntt ON ntt.`entry` = IF(ct.`trainer_id` <> 0, ct.`trainer_id`, ct.`entry`)
WHERE ct.`trainer_type` = 2
  AND ntt.`reqskill` <> 0;
