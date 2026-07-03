-- Human, quest-directed bot group dialogue (inviter announces the mob+quest; accepter replies naturally)
DELETE FROM ai_playerbot_texts WHERE name IN ('join_group_quest','accept_group');
INSERT INTO ai_playerbot_texts (name, text, say_type, reply_type) VALUES
('join_group_quest', "Hey %player, I'm after %mob for %quest - want to team up?", 0, 0),
('join_group_quest', "%player, you on %quest too? Let's clear these %mob together.", 0, 0),
('join_group_quest', "Need a hand with %mob for %quest, %player? Group up?", 0, 0),
('join_group_quest', "%player, grouping for %quest - %mob go down faster with two.", 0, 0),
('join_group_quest', "Killing %mob for %quest, %player. Tag along?", 0, 0),
('accept_group', "Sure %name, send the invite!", 0, 0),
('accept_group', "Yeah, count me in %name.", 0, 0),
('accept_group', "On my way %name - invite me!", 0, 0),
('accept_group', "Sounds good %name, let's do it.", 0, 0),
('accept_group', "Nice, could use the help %name - invite!", 0, 0),
('accept_group', "Sure thing %name.", 0, 0);
