# Module: Pet Loot

<p align="center">
  <img src="icon.png" alt="Pet Loot" width="246" height="246">
</p>

This is a module for [AzerothCore](https://www.azerothcore.org).

It lets any summoned non-combat (vanity) pet act as an immersive auto-loot assistant for the player. It can also be limited to a single pet, such as **Bananas** (ID 23234).

## Features
- **Visual Movement**: The pet physically runs to the location of the killed creature before looting.
- **Looting Animation**: Upon reaching the corpse, the pet plays a "Loot" animation (emote) for immersion.
- **Auto-Loot**: Items and gold are automatically handled using AzerothCore native loot systems, including inventory validation, quest requirements, unique item checks, group loot rules, and gold sharing.
- **Party Kills**: The pet loots whatever your group kills, not only your own killing blows, so it works in dungeons where party members (or bots) land most kills.
- **Distance Check**: The pet only loots if the player is within a configurable distance from the corpse (default: 50 yards).
- **Stability**: Uses `Player::StoreLootItem` to ensure compatibility with unique items, bag space, and quest requirements.
- **Group Loot Support**: Fully respects AzerothCore group loot rules (Free For All, Group Loot, Need Before Greed, Master Loot, Round-Robin). The pet automatically triggers the appropriate loot system exactly once, shares gold correctly between nearby group members, and leaves active loot rolls and master-loot distributions fully managed by the core.
- **Safe Group Roll Handling**: Prevents duplicate Need/Greed or Master Loot rolls when players manually open the corpse after the pet has already initialized the loot.
- **No Empty Sparkles**: Once a group roll ends, the corpse stops sparkling for anyone with nothing left to take, including after disenchant wins and items mailed to a full-bagged winner.
- **Auto-Summon**: Pick a companion with `.companion auto` and it comes out on its own whenever you have none: after logging in, zoning, dying or a flight. See [Auto-summon](#auto-summon).
- **Configurable**: You can enable/disable the module, change the Pet ID, and adjust the looting radius via the config file.

## Requirements
- AzerothCore (latest version)
- A player with a non-combat pet summoned (or the pet set in `PetLoot.PetId`).

## Installation
1. Clone the repo into your AzerothCore `modules/` folder as `mod-pet-loot` (the repo is `wow-mod-pet-loot`):

   ```
   git clone https://github.com/buildthehomelab/wow-mod-pet-loot.git mod-pet-loot
   ```
2. Recompile your AzerothCore project.
3. Copy `conf/mod_pet_loot.conf.dist` to `conf/mod_pet_loot.conf`.
4. Enjoy!

## Configuration
- `PetLoot.Enable`: Enable/Disable the module.
- `PetLoot.PetId`: The NPC ID of the looting pet. `0` lets any non-combat pet loot (default: 0).
- `PetLoot.Radius`: Maximum distance for looting (default: 50.0 yards).
- `PetLoot.AutoSummon.Enable`: Let players pick an auto-summoned companion (default: 1).

## Auto-summon
- `.companion auto`: the companion you have out now comes back on its own whenever you have none.
- `.companion auto [Companion]`: pick one without summoning it; shift-click it from the Companions tab to put the link in chat.
- `.companion off`: stop.
- `.companion`: show which one is picked.

The pick is saved per character. The companion is summoned up to 2 seconds after it's needed, but not in combat, on a taxi or a flying mount, in a vehicle, while stealthed or invisible, while casting, or in an arena. Dismissing it by hand keeps it away until you summon any companion yourself or log in again. A companion you summon yourself is never replaced.

## Credits

Forked from [BlaMacfly/mod-pet-loot](https://github.com/BlaMacfly/mod-pet-loot), the original module.

## License
Released under the [GNU AGPL v3 license](LICENSE).
