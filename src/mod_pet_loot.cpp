/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Creature.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Map.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <vector>

// Configuration class to store module settings
class PetLootConfig
{
public:
    static PetLootConfig* instance()
    {
        static PetLootConfig instance;
        return &instance;
    }

    bool Enabled;
    uint32 PetId;
    float Radius;

    void Load()
    {
        Enabled = sConfigMgr->GetOption<bool>("PetLoot.Enable", true);
        PetId = sConfigMgr->GetOption<uint32>("PetLoot.PetId", 0); // 0 = any non-combat pet
        Radius = sConfigMgr->GetOption<float>("PetLoot.Radius", 50.0f);
    }
};

// Event to handle the actual looting after the pet reaches the corpse
class PetLootEvent : public BasicEvent
{
public:
    PetLootEvent(ObjectGuid playerGuid, ObjectGuid victimGuid, ObjectGuid petGuid)
        : _playerGuid(playerGuid), _victimGuid(victimGuid), _petGuid(petGuid) { }

    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        Player* player = ObjectAccessor::FindPlayer(_playerGuid);
        if (!player)
            return true;

        Creature* victim = ObjectAccessor::GetCreature(*player, _victimGuid);
        Creature* pet = ObjectAccessor::GetCreature(*player, _petGuid);

        if (!victim || !pet)
            return true;

        if (pet->GetMap() != player->GetMap())
            return true;

        pet->HandleEmoteCommand(EMOTE_ONESHOT_LOOT);

        // The core fills corpse loot when the creature dies. Empty loot means
        // nothing dropped or it was already taken; refilling it here would
        // roll the loot table again and create extra drops.

        // Someone (usually a bot) may have opened the corpse before the pet
        // got there; the core then already started the group rolls.
        bool lootInitialized = victim->loot.loot_type != LOOT_NONE;

        // Mark corpse loot as already initialized,
        // otherwise manual looting will start group rolls again.
        victim->loot.loot_type = LOOT_CORPSE;

        Group* group = player->GetGroup();
        LootMethod lootMethod = group ? group->GetLootMethod() : FREE_FOR_ALL;

        // Trigger group loot system once. Starting it a second time would
        // roll every item again and hand out a duplicate of each.
        if (group && lootMethod != FREE_FOR_ALL && !lootInitialized)
        {
            switch (lootMethod)
            {
                case GROUP_LOOT:
                    group->GroupLoot(&victim->loot, victim);
                    break;

                case NEED_BEFORE_GREED:
                    group->NeedBeforeGreed(&victim->loot, victim);
                    break;

                case MASTER_LOOT:
                    group->MasterLoot(&victim->loot, victim);
                    break;

                default:
                    break;
            }
        }

        // Handle Gold
        if (uint32 gold = victim->loot.gold)
        {
            victim->loot.NotifyMoneyRemoved();

            if (group)
            {
                std::vector<Player*> playersNear;

                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member)
                        continue;

                    if (player->IsAtLootRewardDistance(member))
                        playersNear.push_back(member);
                }

                if (!playersNear.empty())
                {
                    uint32 goldPerPlayer = uint32(gold / playersNear.size());

                    for (Player* member : playersNear)
                    {
                        member->ModifyMoney(goldPerPlayer);
                        member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, goldPerPlayer);

                        WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                        data << uint32(goldPerPlayer);
                        data << uint8(playersNear.size() > 1 ? 0 : 1);
                        member->SendDirectMessage(&data);
                    }
                }
            }
            else
            {
                player->ModifyMoney(gold);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);

                // Send standard loot notification
                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                data << uint32(gold);
                data << uint8(1); // "You loot..."
                player->GetSession()->SendPacket(&data);
            }

            victim->loot.gold = 0;
        }

        // Handle Items
        uint32 items_count = uint32(victim->loot.items.size());
        uint32 max_slot = victim->loot.GetMaxSlotInLootFor(player);

        for (uint32 i = 0; i < max_slot; ++i)
        {
            // Quest item slots
            if (i >= items_count)
            {
                InventoryResult msg;
                player->StoreLootItem(uint8(i), &victim->loot, msg);
                continue;
            }

            LootItem& lootItem = victim->loot.items[i];

            if (lootItem.is_looted)
                continue;

            // Items under roll / master loot must stay handled by the group system.
            if (lootItem.is_blocked)
                continue;

            // Skip items allocated to other players (round-robin / master loot).
            const AllowedLooterSet& allowed = lootItem.GetAllowedLooters();
            if (!allowed.empty() && !allowed.count(player->GetGUID()))
                continue;

            InventoryResult msg;
            player->StoreLootItem(uint8(i), &victim->loot, msg);
        }

        // Cleanup corpse visual and decay if empty
        if (victim->loot.isLooted())
        {
            victim->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
            victim->AllLootRemovedFromCorpse();
        }

        // Pet returns to player
        pet->GetMotionMaster()->MoveFollow(player, 1.0f, 0.0f);

        return true;
    }

private:
    ObjectGuid _playerGuid;
    ObjectGuid _victimGuid;
    ObjectGuid _petGuid;
};

class PetLootWorldScript : public WorldScript
{
public:
    PetLootWorldScript() : WorldScript("PetLootWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        PetLootConfig::instance()->Load();
    }
};

class PetLootPlayerScript : public PlayerScript
{
public:
    PetLootPlayerScript() : PlayerScript("PetLootPlayerScript") { }

    void OnPlayerCreatureKill(Player* killer, Creature* victim) override
    {
        ProcessPetLoot(killer, victim);
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* victim) override
    {
        ProcessPetLoot(petOwner, victim);
    }

private:
    // The killer is often a party member (in dungeons usually a bot), so
    // check every group member's pet, not just the killer's.
    void ProcessPetLoot(Player* killer, Creature* victim)
    {
        if (!PetLootConfig::instance()->Enabled)
            return;

        if (!killer || !victim)
            return;

        Group* group = killer->GetGroup();
        if (!group)
        {
            if (victim->GetLootRecipient() == killer)
                TrySendPet(killer, victim);
            return;
        }

        // Only the group that tapped the creature may loot it
        if (victim->GetLootRecipientGroup() != group)
            return;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (member->IsInMap(victim))
                    TrySendPet(member, victim);
    }

    void TrySendPet(Player* player, Creature* victim)
    {
        // Check if the player has a non-combat pet summoned
        ObjectGuid critterGuid = player->GetCritterGUID();
        if (critterGuid.IsEmpty())
            return;

        Creature* critter = ObjectAccessor::GetCreature(*player, critterGuid);
        if (!critter)
            return;

        // PetId 0 lets any non-combat pet loot; otherwise only that entry
        uint32 petId = PetLootConfig::instance()->PetId;
        if (petId && critter->GetEntry() != petId)
            return;

        // Distance check between player and victim
        float dist = player->GetDistance(victim);
        if (dist > PetLootConfig::instance()->Radius)
            return;

        // Visual: Pet moves to the corpse
        critter->GetMotionMaster()->MovePoint(1, victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ());

        // Schedule the looting event
        uint32 delay = 1000;
        player->m_Events.AddEvent(new PetLootEvent(player->GetGUID(), victim->GetGUID(), critter->GetGUID()), player->m_Events.CalculateTime(delay));
    }
};

void Addmod_pet_lootScripts()
{
    new PetLootWorldScript();
    new PetLootPlayerScript();
}
