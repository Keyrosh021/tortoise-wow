UPDATE `ai_playerbot_random_bots`
SET `event` = ''
WHERE `event` IS NULL;

DELETE t1
FROM `ai_playerbot_random_bots` t1
INNER JOIN `ai_playerbot_random_bots` t2
    ON t1.`owner` = t2.`owner`
   AND t1.`bot` = t2.`bot`
   AND t1.`event` = t2.`event`
   AND t1.`id` < t2.`id`;

ALTER TABLE `ai_playerbot_random_bots`
    MODIFY `event` varchar(45) NOT NULL,
    ADD UNIQUE KEY `uq_ai_playerbot_random_bots_owner_bot_event` (`owner`, `bot`, `event`),
    ADD KEY `idx_ai_playerbot_random_bots_event_owner_bot` (`event`, `owner`, `bot`);
