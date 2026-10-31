/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Map.h"
#include "Opcodes.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "Timer.h"
#include "WorldSession.h"

#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

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
    bool AutoSummon;

    void Load()
    {
        Enabled = sConfigMgr->GetOption<bool>("PetLoot.Enable", true);
        PetId = sConfigMgr->GetOption<uint32>("PetLoot.PetId", 0); // 0 = any non-combat pet
        Radius = sConfigMgr->GetOption<float>("PetLoot.Radius", 50.0f);
        AutoSummon = sConfigMgr->GetOption<bool>("PetLoot.AutoSummon.Enable", true);
    }
};

// The companion a player picked with .companion auto. Only players who have
// picked one get this, so bots and everyone else skip the update check.
struct AutoSummonData : DataMap::Base
{
    uint32 SpellId = 0;
    uint32 CheckTimer = 0;
    // Dismissed by hand: stay away until the player summons a companion again
    bool Paused = false;
    // Set from a command this session, so the login load is older
    bool Chosen = false;
};

static constexpr char const* AutoSummonKey = "PetLootAutoSummon";
static constexpr uint32 AutoSummonCheckMs = 2000;

// Companion spells summon a SUMMON_TYPE_MINIPET creature
static bool IsCompanionSpell(SpellInfo const* spellInfo)
{
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        if (effect.Effect == SPELL_EFFECT_SUMMON)
            if (SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(effect.MiscValueB))
                if (properties->Type == SUMMON_TYPE_MINIPET)
                    return true;

    return false;
}

static Creature* GetCompanion(Player* player)
{
    ObjectGuid critterGuid = player->GetCritterGUID();
    return critterGuid ? ObjectAccessor::GetCreature(*player, critterGuid) : nullptr;
}

// Times a companion would be out of place or the summon would get in the way.
// Taxis despawn companions; the next check after landing brings it back.
static bool CanAutoSummon(Player* player)
{
    return player->IsAlive() && !player->IsInCombat() && !player->IsInFlight() && !player->IsFlying()
        && !player->GetVehicle() && !player->IsBeingTeleported() && !player->IsNonMeleeSpellCast(false)
        && !player->HasStealthAura() && !player->HasInvisibilityAura()
        && !player->GetMap()->IsBattleArena();
}

// True while an item on the corpse waits on a group roll or the master looter
static bool HasRollingItem(Loot const& loot)
{
    for (LootItem const& item : loot.items)
        if (item.is_blocked && !item.is_looted)
            return true;

    for (LootItem const& item : loot.quest_items)
        if (item.is_blocked && !item.is_looted)
            return true;

    return false;
}

// Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
// older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
// neither. Looking for both at compile time lets the module build on all of them.
template <typename Session, typename = void>
struct HasIsHeadless : std::false_type { };

template <typename Session>
struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

template <typename Session, typename = void>
struct HasIsBot : std::false_type { };

template <typename Session>
struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

template <typename Session>
static bool IsBotSession(Session* session)
{
    if constexpr (HasIsHeadless<Session>::value)
        return session->IsHeadless();
    else if constexpr (HasIsBot<Session>::value)
        return session->IsBot();
    else
        return false;
}

// The loot window's rule for who may take an item (see Loot's permission views), which
// StoreLootItem doesn't check: under round robin, and for items below the loot threshold
// under group, need-before-greed and master loot, the item belongs to the round-robin
// looter until they release the corpse. A roll winner whose bags were full keeps theirs too.
// A bot's turn is left to the pets: bots are there to help the players, and a bot that never
// opens the corpse would hold its turn until the corpse despawns.
static bool BelongsToSomeoneElse(Player* player, Loot const& loot, LootItem const& item)
{
    // Free-for-all and conditional (quest) items go to anyone they're for.
    if (item.freeforall || !item.conditions.empty())
        return false;

    if (item.rollWinnerGUID && item.rollWinnerGUID != player->GetGUID())
        return true;

    if (!loot.roundRobinPlayer || loot.roundRobinPlayer == player->GetGUID())
        return false;

    if (Player* turn = ObjectAccessor::FindConnectedPlayer(loot.roundRobinPlayer))
        if (turn->GetSession() && IsBotSession(turn->GetSession()))
            return false;

    // Left the group since the kill: the turn is still someone else's.
    Group* group = player->GetGroup();
    if (!group)
        return true;

    switch (group->GetLootMethod())
    {
        case ROUND_ROBIN:
            return true;
        case GROUP_LOOT:
        case NEED_BEFORE_GREED:
        case MASTER_LOOT:
            return item.is_underthreshold;
        default:
            return false;
    }
}

// Marks a corpse that already has a CorpseSparkleCheckEvent queued
struct SparkleCheckPending : DataMap::Base
{
    uint32 Until = 0;
};

// The core never re-sends a corpse's sparkle when a group roll ends (it
// only deactivates chests), and none of its roll hooks fire for disenchant
// or mailed wins. Whoever opened the corpse released it while the items
// were still rolling, so it keeps sparkling, even empty, until someone
// opens it again. Watch the corpse until the rolls finish, then do what
// releasing the loot would have done.
class CorpseSparkleCheckEvent : public BasicEvent
{
public:
    static constexpr uint32 CheckIntervalMs = 1000;
    static constexpr uint32 MaxChecks = 90; // rolls time out after 60s

    explicit CorpseSparkleCheckEvent(Creature* creature) : _creature(creature) { }

    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        if (_creature->IsAlive() || !_creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
            return Finish();

        Loot& loot = _creature->loot;
        if (loot.isLooted())
        {
            _creature->AllLootRemovedFromCorpse();
            _creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            loot.clear();
            return Finish();
        }

        if (HasRollingItem(loot) && ++_checks < MaxChecks)
        {
            _creature->m_Events.AddEvent(this, _creature->m_Events.CalculateTime(CheckIntervalMs));
            return false;
        }

        // Something is left, maybe only for some players. The sparkle is
        // worked out per player, so resend it.
        _creature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
        return Finish();
    }

private:
    bool Finish()
    {
        _creature->CustomData.GetDefault<SparkleCheckPending>("PetLootSparkleCheck")->Until = 0;
        return true;
    }

    Creature* _creature;
    uint32 _checks = 0;
};

static void ScheduleSparkleCheck(Creature* creature)
{
    constexpr uint32 watchMs = CorpseSparkleCheckEvent::MaxChecks * CorpseSparkleCheckEvent::CheckIntervalMs;

    // Until is a deadline rather than a flag, so it lapses on its own if
    // the event gets dropped without running
    SparkleCheckPending* pending = creature->CustomData.GetDefault<SparkleCheckPending>("PetLootSparkleCheck");
    uint32 now = getMSTime();
    if (pending->Until && getMSTimeDiff(now, pending->Until) <= watchMs)
        return;

    pending->Until = now + watchMs;
    creature->m_Events.AddEvent(new CorpseSparkleCheckEvent(creature), creature->m_Events.CalculateTime(CorpseSparkleCheckEvent::CheckIntervalMs));
}

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

        // Only a body that still sparkles. It may have respawned or been
        // emptied in the second it took the pet to get there. Skinning loot
        // is the skinner's alone, and pickpocket loot isn't the pet's either.
        if (victim->IsAlive() || !victim->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)
            || victim->loot.loot_type == LOOT_SKINNING || victim->loot.loot_type == LOOT_PICKPOCKETING)
        {
            pet->GetMotionMaster()->MoveFollow(player, 1.0f, 0.0f);
            return true;
        }

        pet->HandleEmoteCommand(EMOTE_ONESHOT_LOOT);

        // The core fills corpse loot when the creature dies. Empty loot means
        // nothing dropped or it was already taken; refilling it here would
        // roll the loot table again and create extra drops.

        // Someone (usually a bot) may have opened the corpse before the pet
        // got there; the core then already started the group rolls.
        bool lootInitialized = victim->loot.loot_type != LOOT_NONE;

        // Mark corpse loot as already initialized,
        // otherwise manual looting will start group rolls again. Keep a
        // type that's already set: achievements count loot by it, and a
        // body opened as fishing loot (a caught fish) must stay fishing.
        if (!lootInitialized)
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

            if (BelongsToSomeoneElse(player, victim->loot, lootItem))
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
        else
        {
            // The sparkle is worked out per player but only sent when the
            // flag changes. Resend it so players with nothing left to take
            // (the rest is rolling or belongs to someone else) lose it.
            victim->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);

            if (HasRollingItem(victim->loot))
                ScheduleSparkleCheck(victim);
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

// The Journal addon's Pets tab whispers itself under this prefix: "G" asks for the pick,
// "S:<spell>" picks one (0 = off). The answer is "A:<spell>" (0 = none), or "OFF" when the
// realm has auto-summon turned off.
static constexpr std::string_view AddonPrefix = "PETLOOT\t";

static void SendAutoSummonState(Player* player)
{
    WorldSession* session = player->GetSession();
    if (!session || IsBotSession(session))
        return;

    PetLootConfig* config = PetLootConfig::instance();
    AutoSummonData* data = player->CustomData.Get<AutoSummonData>(AutoSummonKey);
    std::string payload = !config->Enabled || !config->AutoSummon ? "OFF"
        : Acore::StringFormat("A:{}", data ? data->SpellId : 0);

    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(AddonPrefix) + payload);
    session->SendPacket(&packet);
}

// Picks the companion (nullptr = off) and saves it. Returns why it can't, or nullptr.
static char const* SetAutoSummon(Player* player, SpellInfo const* spellInfo)
{
    if (spellInfo && !IsCompanionSpell(spellInfo))
        return "That isn't a companion.";

    if (spellInfo && !player->HasSpell(spellInfo->Id))
        return "You don't have that companion.";

    AutoSummonData* data = player->CustomData.GetDefault<AutoSummonData>(AutoSummonKey);
    data->SpellId = spellInfo ? spellInfo->Id : 0;
    data->Chosen = true;
    data->Paused = false;
    data->CheckTimer = 0;

    if (spellInfo)
        CharacterDatabase.Execute("REPLACE INTO `mod_pet_loot_autosummon` (`guid`, `spell`) VALUES ({}, {})",
            player->GetGUID().GetCounter(), spellInfo->Id);
    else
        CharacterDatabase.Execute("DELETE FROM `mod_pet_loot_autosummon` WHERE `guid` = {}", player->GetGUID().GetCounter());

    // Keeps an open Journal in step with a pick made by command
    SendAutoSummonState(player);
    return nullptr;
}

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

    // A player (usually a bot) opening the corpse is what starts the group
    // rolls when no pet got there first.
    void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot) override
    {
        if (!PetLootConfig::instance()->Enabled || !player || !loot || !lootGuid.IsCreatureOrVehicle())
            return;

        if (!HasRollingItem(*loot))
            return;

        if (Creature* creature = player->GetMap()->GetCreature(lootGuid))
            if (!creature->IsAlive())
                ScheduleSparkleCheck(creature);
    }

    void OnPlayerLogin(Player* player) override
    {
        WorldSession* session = player->GetSession();
        if (IsBotSession(session))
            return;

        ObjectGuid const guid = player->GetGUID();
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT `spell` FROM `mod_pet_loot_autosummon` WHERE `guid` = {}", guid.GetCounter()))
            .WithCallback([session, guid](QueryResult result)
            {
                Player* player = session->GetPlayer();
                if (!player || player->GetGUID() != guid || !result)
                    return;

                AutoSummonData* data = player->CustomData.GetDefault<AutoSummonData>(AutoSummonKey);
                if (data->Chosen)
                    return;

                data->SpellId = result->Fetch()[0].Get<uint32>();
                data->CheckTimer = AutoSummonCheckMs;

                // The Journal may have asked before this load finished
                SendAutoSummonState(player);
            }));
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        AutoSummonData* data = player->CustomData.Get<AutoSummonData>(AutoSummonKey);
        if (!data || !data->SpellId)
            return;

        if (data->CheckTimer > diff)
        {
            data->CheckTimer -= diff;
            return;
        }

        data->CheckTimer = AutoSummonCheckMs;

        PetLootConfig* config = PetLootConfig::instance();
        if (!config->Enabled || !config->AutoSummon)
            return;

        // Any companion counts, so one the player summoned themselves stays out
        if (GetCompanion(player))
        {
            data->Paused = false;
            return;
        }

        if (data->Paused || !CanAutoSummon(player) || !player->HasSpell(data->SpellId))
            return;

        // Triggered: no cast bar or global cooldown, and it doesn't dismount
        player->CastSpell(player, data->SpellId, TRIGGERED_FULL_MASK);
    }

    // The Journal addon whispers itself; swallow those so they never show up as chat
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || receiver != player || !msg.starts_with(AddonPrefix))
            return true;

        std::string_view request = std::string_view(msg).substr(AddonPrefix.size());
        PetLootConfig* config = PetLootConfig::instance();
        if (config->Enabled && config->AutoSummon && request.starts_with("S:"))
        {
            uint32 spellId = Acore::StringTo<uint32>(request.substr(2)).value_or(0);
            if (!spellId)
                SetAutoSummon(player, nullptr);
            else if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId); !spellInfo || SetAutoSummon(player, spellInfo))
                SendAutoSummonState(player); // refused: the box goes back to what's picked
            return false;
        }

        SendAutoSummonState(player);
        return false;
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        CharacterDatabase.Execute("DELETE FROM `mod_pet_loot_autosummon` WHERE `guid` = {}", guid.GetCounter());
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

// Dismissing the companion by hand means the player wants it gone, so
// auto-summon waits until they call one out again.
class PetLootServerScript : public ServerScript
{
public:
    PetLootServerScript() : ServerScript("PetLootServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_DISMISS_CRITTER)
            return true;

        if (Player* player = session->GetPlayer())
            if (AutoSummonData* data = player->CustomData.Get<AutoSummonData>(AutoSummonKey))
                data->Paused = true;

        return true;
    }
};

class PetLootCommandScript : public CommandScript
{
public:
    PetLootCommandScript() : CommandScript("PetLootCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable companionCommandTable =
        {
            { "auto", HandleAutoCommand,   SEC_PLAYER, Console::No },
            { "off",  HandleOffCommand,    SEC_PLAYER, Console::No },
            { "",     HandleStatusCommand, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "companion", companionCommandTable }
        };

        return commandTable;
    }

    // .companion auto [companion link]: without a link, the companion out now
    static bool HandleAutoCommand(ChatHandler* handler, Optional<SpellInfo const*> spell)
    {
        if (!CheckEnabled(handler))
            return false;

        Player* player = handler->GetPlayer();
        SpellInfo const* spellInfo = spell ? *spell : nullptr;

        if (!spellInfo)
        {
            Creature* companion = GetCompanion(player);
            if (!companion)
            {
                handler->SendErrorMessage("Summon the companion you want first, or shift-click it from your Companions tab: .companion auto [Companion]");
                return false;
            }

            spellInfo = sSpellMgr->GetSpellInfo(companion->GetUInt32Value(UNIT_CREATED_BY_SPELL));
        }

        if (!spellInfo)
        {
            handler->SendErrorMessage("That isn't a companion.");
            return false;
        }

        if (char const* error = SetAutoSummon(player, spellInfo))
        {
            handler->SendErrorMessage(error);
            return false;
        }

        handler->PSendSysMessage("{} will now come out on its own whenever you have no companion. Type .companion off to stop.",
            spellInfo->SpellName[handler->GetSessionDbcLocale()]);
        return true;
    }

    static bool HandleOffCommand(ChatHandler* handler)
    {
        if (!CheckEnabled(handler))
            return false;

        SetAutoSummon(handler->GetPlayer(), nullptr);

        handler->SendSysMessage("Auto-summon is off. Your companion stays where you leave it.");
        return true;
    }

    static bool HandleStatusCommand(ChatHandler* handler)
    {
        if (!CheckEnabled(handler))
            return false;

        AutoSummonData* data = handler->GetPlayer()->CustomData.Get<AutoSummonData>(AutoSummonKey);
        SpellInfo const* spellInfo = data && data->SpellId ? sSpellMgr->GetSpellInfo(data->SpellId) : nullptr;

        if (spellInfo)
            handler->PSendSysMessage("Auto-summon: {}. Type .companion off to stop, or .companion auto to pick another.",
                spellInfo->SpellName[handler->GetSessionDbcLocale()]);
        else
            handler->SendSysMessage("Auto-summon is off. Summon a companion and type .companion auto to have it come out on its own.");

        return true;
    }

private:
    static bool CheckEnabled(ChatHandler* handler)
    {
        PetLootConfig* config = PetLootConfig::instance();
        if (config->Enabled && config->AutoSummon)
            return true;

        handler->SendErrorMessage("Companion auto-summon is turned off on this realm.");
        return false;
    }
};

void Addmod_pet_lootScripts()
{
    new PetLootWorldScript();
    new PetLootPlayerScript();
    new PetLootServerScript();
    new PetLootCommandScript();
}
