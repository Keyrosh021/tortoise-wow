-- Make transmog fashionistas send the gossip trigger expected by Turtle_TransmogUI.
SET @TRANSMOG_GOSSIP_MENU := 65000;
SET @TRANSMOG_NPC_TEXT    := 65000;
SET @TRANSMOG_BROADCAST   := 65000;

-- Clean up the first local-only draft ID if this update was tested before this file existed.
DELETE FROM `npc_text` WHERE `ID` = 2594000;
DELETE FROM `broadcast_text` WHERE `entry` = 2594000;

DELETE FROM `gossip_menu` WHERE `entry` = @TRANSMOG_GOSSIP_MENU;
DELETE FROM `npc_text` WHERE `ID` = @TRANSMOG_NPC_TEXT;
DELETE FROM `broadcast_text` WHERE `entry` = @TRANSMOG_BROADCAST;

INSERT INTO `broadcast_text` (`entry`, `male_text`, `female_text`, `chat_type`, `sound_id`, `language_id`, `emote_id1`, `emote_id2`, `emote_id3`, `emote_delay1`, `emote_delay2`, `emote_delay3`)
VALUES (@TRANSMOG_BROADCAST, 'TRANSMOG_TRIGGER', 'TRANSMOG_TRIGGER', 0, 0, 0, 0, 0, 0, 0, 0, 0);

INSERT INTO `npc_text` (`ID`, `BroadcastTextID0`, `Probability0`, `BroadcastTextID1`, `Probability1`, `BroadcastTextID2`, `Probability2`, `BroadcastTextID3`, `Probability3`, `BroadcastTextID4`, `Probability4`, `BroadcastTextID5`, `Probability5`, `BroadcastTextID6`, `Probability6`, `BroadcastTextID7`, `Probability7`)
VALUES (@TRANSMOG_NPC_TEXT, @TRANSMOG_BROADCAST, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

INSERT INTO `gossip_menu` (`entry`, `text_id`, `script_id`, `condition_id`)
VALUES (@TRANSMOG_GOSSIP_MENU, @TRANSMOG_NPC_TEXT, 0, 0);

UPDATE `creature_template`
SET `gossip_menu_id` = @TRANSMOG_GOSSIP_MENU,
    `npc_flags` = (`npc_flags` | 268435457)
WHERE `entry` IN (51290, 51291, 51295);
