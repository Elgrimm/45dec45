/*
 * Copyright 2023 Fluxurion
 */

#include "Flux.h"

// Simple check if the player's quest log has contain the quest by ID
bool Fluxurion::HasQuest(Player* player, uint32 questID)
{
    if (!player)
        return false;

    if (questID == 0)
        return false;

    for (uint8 itr = 0; itr < MAX_QUEST_LOG_SIZE; ++itr)
        if (player->GetQuestSlotQuestId(itr) == questID)
            return true;

    return false;
}


// Add item to the player with the little notification box like u loot something cool
void Fluxurion::AddItemWithToast(Player* player, uint32 itemID, uint16 quantity, std::vector<int32> const& bonusIDs /*= std::vector<int32>()*/)
{
    if (!player)
        return;

    Item* pItem = Item::CreateItem(itemID, quantity, ItemContext::NONE, player);

    for (uint32 bonusId : bonusIDs)
        pItem->AddBonuses(bonusId);

    player->SendDisplayToast(itemID, DisplayToastType::NewItem, false, quantity, DisplayToastMethod::PersonalLoot, 0U, pItem);
    player->StoreNewItemInBestSlots(itemID, quantity, ItemContext::NONE);
}

// Send multiple items to the player via ingame mail also can add multiple bonusID to the item
void Fluxurion::SendABunchOfItemsInMail(Player* player, std::vector<uint32> BunchOfItems, std::string const& subject, std::vector<int32> const& bonusIDs /*= std::vector<int32>()*/)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    std::string _subject(subject);
    MailDraft draft(_subject,
        "This is a system message. Do not answer! Don't forget to take out the items! :)");

    for (const uint32 item : BunchOfItems)
    {
        TC_LOG_INFO("server.worldserver", "[BunchOfItems]: {}.", item);
        if (Item* pItem = Item::CreateItem(item, 1, ItemContext::NONE, player))
        {
            for (int32 bonus : bonusIDs)
                pItem->AddBonuses(bonus);

            pItem->SaveToDB(trans);
            draft.AddItem(pItem);
        }
    }

    draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MailCheckMask(MAIL_CHECK_MASK_COPIED | MAIL_CHECK_MASK_RETURNED));
    CharacterDatabase.CommitTransaction(trans);
}

// Get Loadout items from db2
std::vector<uint32> DB2Manager::GetLowestIdItemLoadOutItemsBy(uint32 classID, uint8 type)
{
    auto itr = _characterLoadout.find(classID);
    if (itr == _characterLoadout.end())
        return std::vector<uint32>();

    uint32 smallest = std::numeric_limits<uint32>::max();
    for (auto const& v : itr->second)
        if (v.second == type)
            if (v.first < smallest)
                smallest = v.first;

    return _characterLoadoutItem.count(smallest) ? _characterLoadoutItem[smallest] : std::vector<uint32>();
}

// Duplicate of StoreNewItemInBestSlots but with bonus
bool Fluxurion::StoreNewItemWithBonus(Player* player, uint32 titem_id, uint32 titem_amount, std::vector<int32> const& bonusIDs /*= std::vector<int32>()*/)
{
    TC_LOG_DEBUG("entities.player.items", "Player::StoreNewItemInBestSlots: Player '{}' ({}) creates initial item (ItemID: {}, Count: {})",
        player->GetName().c_str(), player->GetGUID().ToString().c_str(), titem_id, titem_amount);

    // attempt equip by one
    while (titem_amount > 0)
    {
        uint16 eDest;
        InventoryResult msg = player->CanEquipNewItem(NULL_SLOT, eDest, titem_id, true);
        if (msg != EQUIP_ERR_OK)
            break;

        player->EquipNewItem(eDest, titem_id, ItemContext::NONE, true);

        if (Item* item = player->GetItemByPos(eDest))
            for (int32 bonus : bonusIDs)
                item->AddBonuses(bonus);

        player->AutoUnequipOffhandIfNeed();
        --titem_amount;
    }

    if (titem_amount == 0)
        return true;                                        // equipped

    // attempt store
    ItemPosCountVec sDest;
    // store in main bag to simplify second pass (special bags can be not equipped yet at this moment)
    InventoryResult msg = player->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, sDest, titem_id, titem_amount);
    if (msg == EQUIP_ERR_OK)
    {
        player->StoreNewItem(sDest, titem_id, true, GenerateItemRandomBonusListId(titem_id), GuidSet(), ItemContext::NONE, &bonusIDs, true);
        return true;                                        // stored
    }

    // item can't be added
    TC_LOG_ERROR("entities.player.items", "Player::StoreNewItemInBestSlots: Player '{}' ({}) can't equip or store initial item (ItemID: {}, Race: {}, Class: {}, InventoryResult: {})",
        player->GetName().c_str(), player->GetGUID().ToString().c_str(), titem_id, player->GetRace(), player->GetClass(), msg);
    return false;
}

// Custom Gear Giver function which uses characterloadout & characterloadoutitem db2 to give gear to the player.
void Fluxurion::GearUpByLoadout(Player* player, uint32 loadout_purpose, std::vector<int32> const& bonusIDs /*= std::vector<int32>()*/)
{
    uint32 ITEM_HEARTHSTONE = 6948;

    // Get equipped item and store it in bag. If bag is full store it in toBeMailedCurrentEquipment to send it in mail later.
    std::vector<Item*> toBeMailedCurrentEquipment;
    for (int es = EquipmentSlots::EQUIPMENT_SLOT_START; es < EquipmentSlots::EQUIPMENT_SLOT_END; es++)
    {
        if (Item* currentEquiped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, es))
        {
            ItemPosCountVec off_dest;
            if (player->CanStoreItem(NULL_BAG, NULL_SLOT, off_dest, currentEquiped, false) == EQUIP_ERR_OK)
            {
                player->RemoveItem(INVENTORY_SLOT_BAG_0, es, true);
                player->StoreItem(off_dest, currentEquiped, true);
            }
            else
                toBeMailedCurrentEquipment.push_back(currentEquiped);
        }
    }

    // If there are item in the toBeMailedCurrentEquipment vector remove it from inventory and send it in mail.
    if (!toBeMailedCurrentEquipment.empty())
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft draft("Inventory Full: Old Equipment.",
            "To equip your new gear, your old gear had to be unequiped. You did not have enough free bag space, the items that could not be added to your bag you can find in this mail.");

        for (Item* currentEquiped : toBeMailedCurrentEquipment)
        {
            player->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, currentEquiped->GetBagSlot(), true);
            currentEquiped->DeleteFromInventoryDB(trans);                   // deletes item from character's inventory
            currentEquiped->SaveToDB(trans);                                // recursive and not have transaction guard into self, item not in inventory and can be save standalone
            draft.AddItem(currentEquiped);
        }

        draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MailCheckMask(MAIL_CHECK_MASK_COPIED | MAIL_CHECK_MASK_RETURNED));
        CharacterDatabase.CommitTransaction(trans);
    }

    std::vector<uint32> toBeMailedNewItems;

    // Add the new items from loadout.
    for (const uint32 item : sDB2Manager.GetLowestIdItemLoadOutItemsBy(player->GetClass(), loadout_purpose))
        if (item != ITEM_HEARTHSTONE || !player->HasItemCount(ITEM_HEARTHSTONE, 1, true))
            if (!Fluxurion::StoreNewItemWithBonus(player, item, 1, bonusIDs))
                toBeMailedNewItems.push_back(item);

    // If we added more item than free bag slot send the new item as well in mail.
    if (!toBeMailedNewItems.empty())
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft draft("Inventory Full: New Gear.",
            "You did not have enough free bag space to add all your complementary new gear to your bags, those that did not fit you can find in this mail.");

        for (const uint32 item : toBeMailedNewItems)
        {
            if (item != ITEM_HEARTHSTONE || !player->HasItemCount(ITEM_HEARTHSTONE, 1, true))
                if (Item* pItem = Item::CreateItem(item, 1, ItemContext::NONE, player))
                {
                    for (int32 bonus : bonusIDs)
                        pItem->AddBonuses(bonus);

                    if (pItem->GetRequiredLevel() != player->GetLevel())
                        pItem->SetFixedLevel(player->GetLevel());

                    pItem->SaveToDB(trans);
                    draft.AddItem(pItem);
                }
        }

        draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MailCheckMask(MAIL_CHECK_MASK_COPIED | MAIL_CHECK_MASK_RETURNED));
        CharacterDatabase.CommitTransaction(trans);
    }

    player->SaveToDB();
}


bool Fluxurion::CanTakeQuestFromSpell(Player* player, uint32 questGiverSpellId)
{
    // Extra check for legion questline starter spell which has 6 quest
    if (questGiverSpellId == 281351 && (HasQuest(player, 43926) || player->GetQuestStatus(43926) == QUEST_STATUS_REWARDED || HasQuest(player, 40519) || player->GetQuestStatus(40519) == QUEST_STATUS_REWARDED))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(questGiverSpellId, DIFFICULTY_NONE);
    if (!spellInfo)
    {
        TC_LOG_DEBUG("server.CanTakeQuestFromQuestSpell", "Can't get spellinfo for spell: {}", questGiverSpellId);
        return false;
    }

    std::vector<uint32> questIds;
    std::vector<uint32> acceptableQuestIds;

    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        if (effect.Effect == SPELL_EFFECT_QUEST_START)
            questIds.push_back(effect.MiscValue);
    }

    for (uint32 questId : questIds)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            TC_LOG_DEBUG("server.CanTakeQuestFromQuestSpell", "Can't get quest template for quest: {}", questId);
            continue;
        }
        else if (!HasQuest(player, questId) && player->GetQuestStatus(questId) != QUEST_STATUS_REWARDED)
        {
            acceptableQuestIds.push_back(questId);

            TC_LOG_DEBUG("server.CanTakeQuestFromQuestSpell", "Player can take quest: {}", quest->GetQuestId());
        }
    }

    if (acceptableQuestIds.size() > 0)
        return true;

    return false;
}




