#include "setup_pickup_trigger_popup.hpp"

#include <defs/geode.hpp>
#include <globed/constants.hpp>
#include <util/math.hpp>

using namespace geode::prelude;

constexpr int ItemId = 0x50;

bool PickupPopupHook::init(EffectGameObject* p0, CCArray* p1) {
    if (!SetupPickupTriggerPopup::init(p0, p1)) return false;

    // find item id label
    for (auto child : CCArrayExt<CCNode*>(m_mainLayer->getChildren())) {
        if (auto label = typeinfo_cast<CCLabelBMFont*>(child)) {
            if (std::string_view(label->getString()) == "ItemID") {
                m_fields->itemIdLabel = label;
                break;
            }
        }
    }

    // find item id input node
    m_fields->itemIdInputNode = typeinfo_cast<CCTextInputNode*>(m_mainLayer->getChildByTag(80));

    if (!m_fields->itemIdLabel || !m_fields->itemIdInputNode) {
        log::warn("Failed to modify pickup popup, item id label: {}, input node: {}, both should not be null!", m_fields->itemIdLabel, m_fields->itemIdInputNode);
        return true;
    }

    auto toggler = CCMenuItemExt::createTogglerWithStandardSprites(0.7f, [this](auto obj) {
        bool isGlobed = !obj->isOn();

        bool prevDisableDelegate = m_disableTextDelegate;
        m_disableTextDelegate = true;

        if (isGlobed) {
            this->updateValue(ItemId, globed::CUSTOM_ITEM_ID_W_START);
        } else {
            this->updateValue(ItemId, 0);
        }

        m_disableTextDelegate = prevDisableDelegate;
    });

    auto menu = getChildOfType<CCMenu>(m_mainLayer, 0);

    if (!menu) return true;

    float btnY = 0.f;

    // move multi activate button above
    for (auto elem : CCArrayExt<CCNode*>(m_multiTriggerContainer)) {
        if (typeinfo_cast<CCMenuItemToggler*>(elem)) {
            btnY = elem->getPositionY();
        }

        elem->setPositionY(elem->getPositionY() + 30.f);
    }

    Build(toggler)
        .parent(menu)
        .pos(78.f, btnY)
        .id("globed-mode-btn"_spr);

    Build<CCLabelBMFont>::create("Globed", "bigFont.fnt")
        .scale(0.35f)
        .parent(menu)
        .pos(98.f, btnY)
        .anchorPoint(0.f, 0.5f)
        .id("globed-mode-tooltip"_spr);

    if (p0) {
        // toggle globed mode
        this->onUpdateValue(ItemId, p0->m_itemID);

        if (globed::isWritableCustomItem(p0->m_itemID)) {
            toggler->toggle(true);
        }
    }

    return true;
}

static int getNextFreeGlobedItemId() {
    // TODO: fix
    auto editor = LevelEditorLayer::get();

    int highestId = globed::CUSTOM_ITEM_ID_W_START;

    for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
        if (obj->m_unk4D0 != 1 || obj->m_objectType != GameObjectType::Collectible) continue;

        auto eobj = static_cast<EffectGameObject*>(obj);
        if (eobj->m_collectibleIsPickupItem || eobj->m_objectID == 0x719) {
            int itemId = eobj->m_itemID;

            if (itemId > highestId && itemId < globed::CUSTOM_ITEM_ID_END) {
                highestId = itemId;
            }
        }
    }

    return highestId;
}

void PickupPopupHook::onPlusButton(cocos2d::CCObject* sender) {
    if (m_fields->globedMode) {
        // int id = getNextFreeGlobedItemId();
        int id = globed::CUSTOM_ITEM_ID_W_START;
        // log::debug("Setting next free item id: {}", id);
        m_disableTextDelegate = true;
        this->updateValue(ItemId, id);
        m_disableTextDelegate = false;
    } else {
        SetupPickupTriggerPopup::onPlusButton(sender);
    }
}

void PickupPopupHook::toggleGlobedMode(bool state, int itemId) {
    auto& fields = *m_fields.self();

    fields.globedMode = state;

    bool prevDisableDelegate = m_disableTextDelegate;
    m_disableTextDelegate = true;

    if (state) {
        fields.itemIdLabel->setString("Globed ID");
        fields.itemIdInputNode->setString(std::to_string(itemId - globed::CUSTOM_ITEM_ID_W_START));
    } else {
        fields.itemIdLabel->setString("ItemID");
        fields.itemIdInputNode->setString(std::to_string(itemId));
    }

    m_disableTextDelegate = prevDisableDelegate;
}

void PickupPopupHook::onUpdateValue(int p0, float p1) {
    if (p0 != ItemId) return; // not item id

    int itemId = (int)p1;

    this->toggleGlobedMode(globed::isWritableCustomItem(itemId), itemId);
}
