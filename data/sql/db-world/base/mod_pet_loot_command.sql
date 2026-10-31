DELETE FROM `command` WHERE `name` IN ('companion', 'companion auto', 'companion off');

INSERT INTO `command` (`name`, `security`, `help`) VALUES
('companion', 0, 'Syntax: .companion\nShow which companion comes out on its own.'),
('companion auto', 0, 'Syntax: .companion auto [$companion]\nYour summoned companion (or the one you shift-click from the Companions tab) comes out on its own whenever you have none: after logging in, zoning, dying or a flight. Dismiss it by hand and it stays away until you summon a companion again or log in.'),
('companion off', 0, 'Syntax: .companion off\nStop summoning a companion on its own.');
