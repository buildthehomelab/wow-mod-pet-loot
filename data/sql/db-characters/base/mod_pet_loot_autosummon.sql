-- The companion each character picked with .companion auto
CREATE TABLE IF NOT EXISTS `mod_pet_loot_autosummon` (
  `guid` INT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
