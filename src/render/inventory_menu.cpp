// The in-game Menu system, drawn from the Xbox 360 release's
// `media/UI/mastermenu_xenon.layout`, `media/UI/charctermenu_xenon.layout`,
// `media/UI/inventorymenu_xenon.layout`, `media/UI/statsmenu_xenon.layout`,
// `media/UI/journalmenu_xenon.layout` and `media/UI/questmenu_xenon.layout`.
#include "render/inventory_menu.h"

#include "core/combine.h"
#include "core/enchanting.h"
#include "core/inventory_screen.h"
#include "core/item_roll.h"
#include "core/item_use.h"
#include "core/log.h"
#include "core/text.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgrePass.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace tl::render {
namespace {

/// A bag row's name: `CEquipment::getFullItemName(false)`'s early return - an
/// unidentified item that authors an `UNIDENTIFIED_NAME` is called that until identified.
std::string row_item_name(const tl::core::ItemStats& item) {
    if (!item.identified && !item.unidentified_name.empty()) return item.unidentified_name;
    return item.display_name.empty() ? item.name : item.display_name;
}

constexpr const char* kMasterLayout = "media/UI/mastermenu_xenon.layout";
constexpr const char* kCharLayout = "media/UI/charctermenu_xenon.layout";
/// The pet's page, the second `CCharacterMenuXenon` (`CGameUI +0x18b4`, X360
/// `CGameUI::create`): the left column while the pet's tray has the
/// focus.
constexpr const char* kPetPageLayout = "media/UI/petmenu_xenon.layout";
constexpr const char* kInventoryLayout = "media/UI/inventorymenu_xenon.layout";
constexpr const char* kStatsLayout = "media/UI/statsmenu_xenon.layout";
constexpr const char* kJournalLayout = "media/UI/journalmenu_xenon.layout";
constexpr const char* kQuestLayout = "media/UI/questmenu_xenon.layout";
constexpr const char* kEnchanterLayout = "media/UI/enchantermenu_xenon.layout";
constexpr const char* kHousingLayout = "media/UI/messagehousing_xenon.layout";
constexpr const char* kPetCommandsLayout = "media/UI/petcommands_xenon.layout";
constexpr const char* kSpellRemoveLayout = "media/UI/spellremove_xenon.layout";

constexpr const char* kConsoleImagesets[] = {
    "media/UI/icons2/widgets.imageset",
    "media/UI/icons2/widgets1.imageset",
    "media/UI/icons2/widgets2.imageset",
    "media/UI/icons2/ratings.imageset",
};

constexpr const char* kItemIconImagesets[] = {
    "media/UI/itemicons/itemicons.imageset",
    "media/UI/itemicons/itemicons1.imageset",
    "media/UI/itemicons/itemicons2.imageset",
    "media/UI/itemicons/itemicons3.imageset",
    "media/UI/itemicons/itemicons4.imageset",
    "media/UI/itemicons/itemicons5.imageset",
    "media/UI/itemicons/itemicons6.imageset",
    "media/UI/itemicons/itemicons7.imageset",
    "media/UI/itemicons/itemicons8.imageset",
    "media/UI/itemicons/itemicons9.imageset",
    "media/UI/itemicons/itemicons10.imageset",
    "media/UI/itemicons/itemicons11.imageset",
    "media/UI/itemicons/itemicons12.imageset",
    "media/UI/itemicons/itemicons13.imageset",
    "media/UI/itemicons/itemicons14.imageset",
    "media/UI/itemicons/itemicons15.imageset",
    "media/UI/itemicons/itemicons16.imageset",
    // The Xbox's own `itemicons13`, renamed at extraction: the console-only sets' icons. Last, so
    // a name the PC sheets hold keeps theirs.
    "media/UI/itemicons/xbox_itemicons13.imageset",
};

float default_extent(const std::string& descriptor, bool horizontal) {
    if (descriptor == "Button") return horizontal ? 250.0f : 30.0f;
    if (descriptor == "Check Box") return horizontal ? 50.0f : 40.0f;
    return horizontal ? 265.0f : 250.0f;
}

int anchor_of(const std::string& word) {
    if (word == "CENTER") return 1;
    if (word == "RIGHT" || word == "BOTTOM") return 2;
    return 0;
}
float font_points(const std::string& name) {
    if (name == "SerifSmall") return kSmallFontSize;
    if (name == "Serif14") return 14.0f;
    if (name == "SerifBig") return kBigFontSize;
    if (name == "SerifHuge") return kHugeFontSize;
    return kFontSize;
}

std::string lowered(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string indexed(const std::string& prefix, size_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04zu", index);
    return prefix + buf;
}

/// Strip CEGUI formatting codes like `|cFFB29560` and `|u` / `|r`, and unescape `\n`.
std::string clean_ui_text(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '|' && i + 1 < text.size()) {
            if (text[i + 1] == 'c' || text[i + 1] == 'C') {
                size_t j = i + 2;
                while (j < text.size() && j < i + 10 &&
                       std::isxdigit(static_cast<unsigned char>(text[j]))) {
                    ++j;
                }
                i = j - 1;
                continue;
            }
            if (text[i + 1] == 'u' || text[i + 1] == 'U' || text[i + 1] == 'r' ||
                text[i + 1] == 'R') {
                ++i;
                continue;
            }
        }
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string wrap_ui_text(const std::string& text, size_t max_chars = 25) {
    if (text.size() <= max_chars) return text;
    std::string res;
    size_t line_len = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t next_space = text.find(' ', i);
        std::string word =
            (next_space == std::string::npos) ? text.substr(i) : text.substr(i, next_space - i);
        if (line_len > 0 && line_len + 1 + word.size() > max_chars) {
            res += "\n" + word;
            line_len = word.size();
        } else {
            if (line_len > 0) {
                res += " ";
                line_len++;
            }
            res += word;
            line_len += word.size();
        }
        if (next_space == std::string::npos) break;
        i = next_space + 1;
    }
    return res;
}

}  // namespace

InventoryMenu::~InventoryMenu() { destroy(); }

const InventoryMenu::Sprite* InventoryMenu::sprite(const std::string& name) const {
    auto found = sprites_.find(lowered(name));
    // A console image the sheets lack, drawn in the PC's like of it.
    if (found == sprites_.end()) found = sprites_.find(lowered(pc_image_for(name)));
    return found == sprites_.end() ? nullptr : &found->second;
}

bool InventoryMenu::load_art(const std::string& group) {
    // The item icons, then the PC's own sets (the panel looks the interface pack leaves out),
    // both optional.
    std::vector<const char*> extra(std::begin(kItemIconImagesets), std::end(kItemIconImagesets));
    for (const char* path : pc_imagesets()) extra.push_back(path);
    sets_.reserve(std::size(kConsoleImagesets) + extra.size());

    // 1. Console widgets imagesets
    for (const char* path : kConsoleImagesets) {
        std::vector<uint8_t> bytes;
        if (!read_menu_resource(path, group, bytes) || bytes.empty()) {
            TL_LOG_INFO("no inventory menu: '%s' is not in the mounted data", path);
            return false;
        }
        tl::formats::Imageset set = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!set.ok) {
            TL_LOG_WARN("inventory menu: %s: %s", path, set.error.c_str());
            return false;
        }
        sets_.push_back(std::move(set));
        const tl::formats::Imageset& stored = sets_.back();

        Ogre::TexturePtr texture =
            Ogre::TextureManager::getSingleton().load(stored.image_file, group);
        if (!texture) {
            TL_LOG_WARN("inventory menu: no texture '%s'", stored.image_file.c_str());
            return false;
        }
        textures_.push_back(texture);
        materials_.push_back(build_menu_material("tl/inv/" + stored.name, texture));

        for (const tl::formats::ImagesetImage& image : stored.images) {
            Sprite entry;
            entry.rect = image;
            entry.texture = texture;
            entry.material = materials_.back();
            sprites_.emplace(lowered(image.name), entry);
        }
    }

    // 2. Item icons imagesets (from main game data)
    for (const char* path : extra) {
        std::vector<uint8_t> bytes;
        if (!read_menu_resource(path, group, bytes) || bytes.empty()) {
            continue;
        }
        tl::formats::Imageset set = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!set.ok) continue;

        sets_.push_back(std::move(set));
        const tl::formats::Imageset& stored = sets_.back();

        Ogre::TexturePtr texture =
            Ogre::TextureManager::getSingleton().load(stored.image_file, group);
        if (!texture) continue;

        textures_.push_back(texture);
        materials_.push_back(build_menu_material("tl/inv/item_" + stored.name, texture));

        for (const tl::formats::ImagesetImage& image : stored.images) {
            Sprite entry;
            entry.rect = image;
            entry.texture = texture;
            entry.material = materials_.back();
            sprites_.emplace(lowered(image.name), entry);
        }
    }

    // 3. Ensure CharacterViewport preview material exists
    auto& mats = Ogre::MaterialManager::getSingleton();
    if (!mats.getByName("tl/inv/preview_material", Ogre::RGN_INTERNAL) &&
        !mats.getByName("tl/inv/preview_material", Ogre::RGN_DEFAULT)) {
        Ogre::MaterialPtr preview_mat = mats.create("tl/inv/preview_material", Ogre::RGN_INTERNAL);
        Ogre::Pass* pass = preview_mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setCullingMode(Ogre::CULL_NONE);
        Ogre::TextureUnitState* unit = pass->createTextureUnitState();
        unit->setTextureName("tl/inv/char_preview");
        unit->setColourOperationEx(Ogre::LBX_SOURCE1, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
        unit->setAlphaOperation(Ogre::LBX_SOURCE1, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
        unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
        materials_.push_back(preview_mat);
    }

    TL_LOG_INFO("inventory menu: %zu named images over %zu sheets", sprites_.size(), sets_.size());
    return true;
}

bool InventoryMenu::create(const std::string& group, unsigned width, unsigned height) {
    destroy();
    width_ = width;
    height_ = height;
    scale_ = static_cast<float>(height_) / 768.0f;

    if (!load_art(group)) {
        destroy();
        return false;
    }
    if (!ensure_menu_font()) {
        destroy();
        return false;
    }

    // Load master layout (outer frame, header, triggers, footer, ContainerFrame)
    std::vector<uint8_t> master_bytes;
    if (!read_menu_resource(kMasterLayout, group, master_bytes) || master_bytes.empty()) {
        TL_LOG_INFO("inventory menu: '%s' not found", kMasterLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout master_layout =
        tl::formats::parse_ui_layout(master_bytes.data(), master_bytes.size());
    if (!master_layout.ok) {
        TL_LOG_WARN("inventory menu: %s: %s", kMasterLayout, master_layout.error.c_str());
        destroy();
        return false;
    }

    // Load character sheet layout (left column)
    std::vector<uint8_t> char_bytes;
    if (!read_menu_resource(kCharLayout, group, char_bytes) || char_bytes.empty()) {
        TL_LOG_INFO("inventory menu: '%s' not found", kCharLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout char_layout =
        tl::formats::parse_ui_layout(char_bytes.data(), char_bytes.size());
    if (!char_layout.ok) {
        TL_LOG_WARN("inventory menu: %s: %s", kCharLayout, char_layout.error.c_str());
        destroy();
        return false;
    }

    // Load inventory menu layout (middle and right columns for Inventory mode)
    std::vector<uint8_t> inv_bytes;
    if (!read_menu_resource(kInventoryLayout, group, inv_bytes) || inv_bytes.empty()) {
        TL_LOG_INFO("inventory menu: '%s' not found", kInventoryLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout inv_layout =
        tl::formats::parse_ui_layout(inv_bytes.data(), inv_bytes.size());
    if (!inv_layout.ok) {
        TL_LOG_WARN("inventory menu: %s: %s", kInventoryLayout, inv_layout.error.c_str());
        destroy();
        return false;
    }

    // Load stats menu layout (middle column for Stats mode)
    std::vector<uint8_t> stats_bytes;
    if (!read_menu_resource(kStatsLayout, group, stats_bytes) || stats_bytes.empty()) {
        TL_LOG_INFO("inventory menu: '%s' not found", kStatsLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout stats_layout =
        tl::formats::parse_ui_layout(stats_bytes.data(), stats_bytes.size());
    if (!stats_layout.ok) {
        TL_LOG_WARN("inventory menu: %s: %s", kStatsLayout, stats_layout.error.c_str());
        destroy();
        return false;
    }

    // Load journal menu layout (right column for Stats mode)
    std::vector<uint8_t> journal_bytes;
    if (!read_menu_resource(kJournalLayout, group, journal_bytes) || journal_bytes.empty()) {
        TL_LOG_INFO(
            "inventory menu: '%s' not found - re-run scripts/extract_console_ui.py "
            "and re-deploy the console UI; the whole menu needs it",
            kJournalLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout journal_layout =
        tl::formats::parse_ui_layout(journal_bytes.data(), journal_bytes.size());
    if (!journal_layout.ok) {
        TL_LOG_WARN("inventory menu: %s: %s", kJournalLayout, journal_layout.error.c_str());
        destroy();
        return false;
    }

    // Load quest menu layout (the whole width, for Quest mode)
    std::vector<uint8_t> quest_bytes;
    if (!read_menu_resource(kQuestLayout, group, quest_bytes) || quest_bytes.empty()) {
        TL_LOG_INFO(
            "inventory menu: '%s' not found - re-run scripts/extract_console_ui.py "
            "and re-deploy the console UI; the whole menu needs it",
            kQuestLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout quest_layout =
        tl::formats::parse_ui_layout(quest_bytes.data(), quest_bytes.size());
    if (!quest_layout.ok) {
        TL_LOG_WARN("inventory menu: %s: %s", kQuestLayout, quest_layout.error.c_str());
        destroy();
        return false;
    }

    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_ = overlays.create("tl/inventory_menu_overlay");
    overlay_->setZOrder(550);

    text_overlay_ = overlays.create("tl/inventory_menu_text_overlay");
    text_overlay_->setZOrder(560);

    panel_root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/inventory_menu/root"));
    panel_root_->setMetricsMode(Ogre::GMM_RELATIVE);
    panel_root_->setPosition(0.0f, 0.0f);
    panel_root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(panel_root_)->setTransparent(true);
    overlay_->add2D(panel_root_);

    text_root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/inventory_menu/text_root"));
    text_root_->setMetricsMode(Ogre::GMM_RELATIVE);
    text_root_->setPosition(0.0f, 0.0f);
    text_root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(text_root_)->setTransparent(true);
    text_overlay_->add2D(text_root_);

    // The floating pair, above both: see the note in the header.
    top_overlay_ = overlays.create("tl/inventory_menu_top_overlay");
    top_overlay_->setZOrder(570);
    top_text_overlay_ = overlays.create("tl/inventory_menu_top_text_overlay");
    top_text_overlay_->setZOrder(580);

    top_panel_root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/inventory_menu/top_root"));
    top_panel_root_->setMetricsMode(Ogre::GMM_RELATIVE);
    top_panel_root_->setPosition(0.0f, 0.0f);
    top_panel_root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(top_panel_root_)->setTransparent(true);
    top_overlay_->add2D(top_panel_root_);

    top_text_root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/inventory_menu/top_text_root"));
    top_text_root_->setMetricsMode(Ogre::GMM_RELATIVE);
    top_text_root_->setPosition(0.0f, 0.0f);
    top_text_root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(top_text_root_)->setTransparent(true);
    top_text_overlay_->add2D(top_text_root_);

    size_t serial = 0;

    // 1. Build master frame
    std::vector<const tl::formats::UiObject*> master_order;
    master_order.reserve(master_layout.objects.size());
    for (const tl::formats::UiObject& obj : master_layout.objects) master_order.push_back(&obj);
    std::stable_sort(master_order.begin(), master_order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* obj : master_order) {
        build(*obj, -1, "master/", serial, false, false, 0.0f, 0.0f);
    }

    const int container_idx = find_node_index_by_widget("master/ContainerFrame");
    if (container_idx < 0) {
        TL_LOG_ERROR("inventory menu: ContainerFrame not found in %s", kMasterLayout);
        destroy();
        return false;
    }

    // 2. Build character sheet in column 0 (off_x = 0)
    std::vector<const tl::formats::UiObject*> char_order;
    char_order.reserve(char_layout.objects.size());
    for (const tl::formats::UiObject& obj : char_layout.objects) char_order.push_back(&obj);
    std::stable_sort(char_order.begin(), char_order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* obj : char_order) {
        build(*obj, container_idx, "char/", serial, false, false, 0.0f, 0.0f);
    }
    // The pet's page in the same place (both panels go into the screen's left
    // sheet), optional: without it the column keeps the character.
    have_pet_page_ = false;
    {
        std::vector<uint8_t> page_bytes;
        if (read_menu_resource(kPetPageLayout, group, page_bytes) && !page_bytes.empty()) {
            const tl::formats::UiLayout page_layout =
                tl::formats::parse_ui_layout(page_bytes.data(), page_bytes.size());
            if (page_layout.ok) {
                std::vector<const tl::formats::UiObject*> page_order;
                for (const tl::formats::UiObject& obj : page_layout.objects) {
                    page_order.push_back(&obj);
                }
                std::stable_sort(
                    page_order.begin(), page_order.end(),
                    [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                        return a->bool_or("ALWAYS ON TOP", false) <
                               b->bool_or("ALWAYS ON TOP", false);
                    });
                for (const tl::formats::UiObject* obj : page_order) {
                    build(*obj, container_idx, "petpage/", serial, false, false, 0.0f, 0.0f);
                }
                have_pet_page_ = true;
            }
        }
    }

    // 3. Build player inventory in column 1 (off_x = 301)
    std::vector<const tl::formats::UiObject*> inv_order;
    inv_order.reserve(inv_layout.objects.size());
    for (const tl::formats::UiObject& obj : inv_layout.objects) inv_order.push_back(&obj);
    std::stable_sort(inv_order.begin(), inv_order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* obj : inv_order) {
        build(*obj, container_idx, "inv/", serial, false, false, 301.0f, 0.0f);
    }

    // 4. Build pet inventory in column 2 (off_x = 602)
    for (const tl::formats::UiObject* obj : inv_order) {
        build(*obj, container_idx, "pet/", serial, false, false, 602.0f, 0.0f);
    }

    // 5. Build stats menu in column 1 (off_x = 301)
    std::vector<const tl::formats::UiObject*> stats_order;
    stats_order.reserve(stats_layout.objects.size());
    for (const tl::formats::UiObject& obj : stats_layout.objects) stats_order.push_back(&obj);
    std::stable_sort(stats_order.begin(), stats_order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* obj : stats_order) {
        build(*obj, container_idx, "stats/", serial, false, false, 301.0f, 0.0f);
    }
    // (`+0xa8`..`+0xb4`): the frame's place less the strength
    // panel's, the two siblings as the layout puts them.
    if (const Node* frame = find_node_by_widget("stats/pieceToMove")) {
        if (const Node* first = find_node_by_widget("stats/strength")) {
            stat_frame_dx_ = frame->off_x - first->off_x;
            stat_frame_dy_ = frame->off_y - first->off_y;
        }
    }

    // 6. Build journal menu in column 2 (off_x = 602)
    std::vector<const tl::formats::UiObject*> journal_order;
    journal_order.reserve(journal_layout.objects.size());
    for (const tl::formats::UiObject& obj : journal_layout.objects) journal_order.push_back(&obj);
    std::stable_sort(journal_order.begin(), journal_order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* obj : journal_order) {
        build(*obj, container_idx, "journal/", serial, false, false, 602.0f, 0.0f);
    }

    // 7. Build the quest tab across the whole container: `questmenu_xenon.layout`'s `background`
    // is 901 x 629 against a 902-wide `ContainerFrame`, so it is built at zero and covers the
    // character sheet too.
    std::vector<const tl::formats::UiObject*> quest_order;
    quest_order.reserve(quest_layout.objects.size());
    for (const tl::formats::UiObject& obj : quest_layout.objects) quest_order.push_back(&obj);
    std::stable_sort(quest_order.begin(), quest_order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* obj : quest_order) {
        build(*obj, container_idx, "quest/", serial, false, false, 0.0f, 0.0f);
    }

    // 7a. The skills tab, the whole container too (`inventory_menu_skills.cpp`).
    container_idx_ = container_idx;
    build_skills_tab(group, container_idx, serial);

    // 7b. The vendor's column, where the character sheet is: the inventory
    // layout a fourth time (merchant, stash, combiner) and, when the console
    // UI carries it, the enchanter family's panes. Hidden until a vendor
    // screen is opened. See `VendorScreen`.
    for (const tl::formats::UiObject* obj : inv_order) {
        build(*obj, container_idx, "vendor/", serial, false, false, 0.0f, 0.0f);
    }
    std::vector<uint8_t> enchanter_bytes;
    have_enchanter_ = false;
    if (read_menu_resource(kEnchanterLayout, group, enchanter_bytes) && !enchanter_bytes.empty()) {
        const tl::formats::UiLayout enchanter_layout =
            tl::formats::parse_ui_layout(enchanter_bytes.data(), enchanter_bytes.size());
        if (enchanter_layout.ok) {
            for (const tl::formats::UiObject& obj : enchanter_layout.objects) {
                build(obj, container_idx, "enchant/", serial, false, false, 0.0f, 0.0f);
            }
            have_enchanter_ = true;
            if (const Node* price = find_node_by_widget("enchant/priceofenchantment")) {
                enchant_price_colour_ = price->colour;
            }
        } else {
            TL_LOG_WARN("inventory menu: %s: %s", kEnchanterLayout, enchanter_layout.error.c_str());
        }
    } else {
        TL_LOG_INFO(
            "inventory menu: '%s' not found - re-run scripts/extract_console_ui.py; "
            "the enchanter's screens will not open",
            kEnchanterLayout);
    }

    // 7c. The item-select dialog (`CMessageItemSelectXenon`): the housing
    // (`CMessageHousingXenon::vfunc_4` loads MEDIA/UI/MESSAGEHOUSING_XENON.UILAYOUT)
    // over the whole screen and floating above the menu, and a tray built from
    // the bag's layout once more inside its root window at (0, 0): `vfunc_4`
    // sets the tray's position to zero and moves the
    // tray's window into the housing's sheet.
    std::vector<uint8_t> housing_bytes;
    have_housing_ = false;
    int housing_root = -1;
    if (read_menu_resource(kHousingLayout, group, housing_bytes) && !housing_bytes.empty()) {
        const tl::formats::UiLayout housing_layout =
            tl::formats::parse_ui_layout(housing_bytes.data(), housing_bytes.size());
        if (housing_layout.ok && !housing_layout.objects.empty()) {
            housing_root = static_cast<int>(nodes_.size());
            for (const tl::formats::UiObject& obj : housing_layout.objects) {
                build(obj, -1, "housing/", serial, false, false, 0.0f, 0.0f, true);
            }
            for (const tl::formats::UiObject* obj : inv_order) {
                build(*obj, housing_root, "select/", serial, false, false, 0.0f, 0.0f, true);
            }
            have_housing_ = true;
        } else {
            TL_LOG_WARN("inventory menu: %s: %s", kHousingLayout,
                        housing_layout.ok ? "empty" : housing_layout.error.c_str());
        }
    } else {
        TL_LOG_INFO(
            "inventory menu: '%s' not found - re-run scripts/extract_console_ui.py; "
            "gems cannot be socketed",
            kHousingLayout);
    }

    // 7d. The pet commands dialog (`CMessagePetCommandsXenon`): the same housing,
    // and `vfunc_4` loads MEDIA/UI/PETCOMMANDS_XENON.UILAYOUT into
    // its `parent` window (`+0x10`, which `CMessageHousingXenon::vfunc_4` finds
    // by name), shown, at its authored place.
    have_pet_commands_ = false;
    if (have_housing_) {
        int housing_parent = housing_root;
        for (size_t i = static_cast<size_t>(housing_root); i < nodes_.size(); ++i) {
            if (nodes_[i].widget == "housing/parent") housing_parent = static_cast<int>(i);
        }
        std::vector<uint8_t> pet_bytes;
        if (read_menu_resource(kPetCommandsLayout, group, pet_bytes) && !pet_bytes.empty()) {
            const tl::formats::UiLayout pet_layout =
                tl::formats::parse_ui_layout(pet_bytes.data(), pet_bytes.size());
            if (pet_layout.ok && !pet_layout.objects.empty()) {
                for (const tl::formats::UiObject& obj : pet_layout.objects) {
                    build(obj, housing_parent, "petcmd/", serial, false, false, 0.0f, 0.0f, true);
                }
                have_pet_commands_ = true;
            } else {
                TL_LOG_WARN("inventory menu: %s: %s", kPetCommandsLayout,
                            pet_layout.ok ? "empty" : pet_layout.error.c_str());
            }
        } else {
            TL_LOG_INFO(
                "inventory menu: '%s' not found - re-run scripts/extract_console_ui.py; "
                "Start opens no pet commands",
                kPetCommandsLayout);
        }

        // 7e. The replace-spell dialog (`CMessageSpellRemoveXenon`): the same
        // housing, and `vfunc_4` loads
        // MEDIA/UI/SPELLREMOVE_XENON.UILAYOUT into its `parent` window.
        have_spell_remove_ = false;
        std::vector<uint8_t> remove_bytes;
        if (read_menu_resource(kSpellRemoveLayout, group, remove_bytes) && !remove_bytes.empty()) {
            const tl::formats::UiLayout remove_layout =
                tl::formats::parse_ui_layout(remove_bytes.data(), remove_bytes.size());
            if (remove_layout.ok && !remove_layout.objects.empty()) {
                for (const tl::formats::UiObject& obj : remove_layout.objects) {
                    build(obj, housing_parent, "spellrm/", serial, false, false, 0.0f, 0.0f, true);
                }
                have_spell_remove_ = true;
            } else {
                TL_LOG_WARN("inventory menu: %s: %s", kSpellRemoveLayout,
                            remove_layout.ok ? "empty" : remove_layout.error.c_str());
            }
        } else {
            TL_LOG_INFO(
                "inventory menu: '%s' not found - re-run scripts/extract_console_ui.py; "
                "a full spell book cannot be changed",
                kSpellRemoveLayout);
        }
    }

    // 8. The journal's rows: `CJournalXenon`'s fill makes two
    // `ceguiWidget/StaticText` windows a row, label and value, inside the
    // layout's `journalwindow`. The look (`ceguiWidgets.looknfeel`) draws the
    // text in `Serif` with a copy in `DropTextColour` (FF000000) two pixels
    // right and down under it; the captions come from
    // `tl::core::journal_page_rows` when the page opens (`update()`).
    journal_rows_.clear();
    for (size_t r = 0; r < tl::core::kJournalStatisticCount; ++r) {
        JournalRow row;
        auto make = [&](const std::string& name, bool right, const Ogre::ColourValue& colour) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", name));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            area->setFontName(kFontName);
            area->setColour(colour);
            area->setAlignment(right ? Ogre::TextAreaOverlayElement::Right
                                     : Ogre::TextAreaOverlayElement::Left);
            text_root_->addChild(area);
            return area;
        };
        // Siblings at one depth draw in name order: each drop's name sorts
        // before its text's, as the layout's `/drop` + `/text` pairs do.
        const std::string base = "tl/inv/journal_" + std::to_string(r);
        const Ogre::ColourValue black(0.0f, 0.0f, 0.0f, 1.0f);
        row.label_drop = make(base + "/drop_label", false, black);
        row.label = make(base + "/label", false, Ogre::ColourValue::White);
        row.value_drop = make(base + "/drop_value", true, black);
        row.value = make(base + "/value", true, Ogre::ColourValue::White);
        journal_rows_.push_back(row);
    }

    // Add all created panels to their container and assign deterministic
    // ascending Z-orders by layer. The floating windows go into their own
    // container, which is a whole overlay above the text - see the header.
    for (Node& node : nodes_) {
        if (node.panel == nullptr) continue;
        (node.floating ? top_panel_root_ : panel_root_)->addChild(node.panel);
    }

    auto get_node_layer = [](const Node& n) -> int {
        if (n.image == "a_button" || n.image == "b_button" || n.image == "x_button" ||
            n.image == "y_button" || n.widget.find("buttonA") != std::string::npos ||
            n.widget.find("buttonB") != std::string::npos ||
            n.widget.find("buttonX") != std::string::npos ||
            n.widget.find("buttonY") != std::string::npos) {
            return 7;
        }
        if (n.widget.find("spendPoints/bg") != std::string::npos) {
            return 5;
        }
        // The selection frame's heading ("Buy"/"Sell") and its legend are
        // both `ALWAYS ON TOP` in the layout: over the rows they overlap.
        if (n.widget.find("buttontray") != std::string::npos ||
            n.widget.find("buyandsell") != std::string::npos ||
            n.widget.find("spendPoints") != std::string::npos ||
            n.widget.find("addPoint") != std::string::npos ||
            n.widget.find("removePoint") != std::string::npos) {
            return 6;
        }
        if (n.widget.find("StrHelp") != std::string::npos ||
            n.widget.find("DexHelp") != std::string::npos ||
            n.widget.find("MagicHelp") != std::string::npos ||
            n.widget.find("DefHelp") != std::string::npos ||
            n.widget.find("Help") != std::string::npos) {
            return 5;
        }
        if (n.widget.find("moveframe") != std::string::npos ||
            n.widget.find("pieceToMove") != std::string::npos) {
            return 4;
        }
        if (n.prefix == "master/") {
            // Master background black backdrop stays behind the columns
            if (n.image == "black") return 0;
            return 3;
        }
        if (n.prefix == "housing/") {
            // The item-select dialog's backdrop under its tray, and the close
            // button over it.
            return n.image == "black" ? 0 : 3;
        }
        if (n.prefix == "petcmd/" || n.prefix == "spellrm/") {
            // The pet commands and replace-spell dialogs, over the housing's
            // backdrop.
            return 3;
        }
        if (n.widget.find("CharacterViewport") != std::string::npos) {
            return 0;  // Behind TopFrame so TopFrame's frame borders overlay cleanly
        }
        if (n.widget.find("TopFrame") != std::string::npos ||
            n.widget.find("HiliteFrame") != std::string::npos) {
            return 1;
        }
        if (n.prefix == "char/" || n.prefix == "petpage/" || n.prefix == "inv/" ||
            n.prefix == "pet/" || n.prefix == "stats/" || n.prefix == "journal/" ||
            n.prefix == "quest/" || n.prefix == "vendor/" || n.prefix == "enchant/" ||
            n.prefix == "skills/" || n.prefix == "skillmap/" || n.prefix == "select/") {
            return 2;
        }
        return 0;
    };

    // Z within a container only orders that container's own children, so the
    // two are sorted separately.
    for (int pass = 0; pass < 2; ++pass) {
        const bool want_floating = pass == 1;
        Ogre::OverlayContainer* root = want_floating ? top_panel_root_ : panel_root_;
        std::vector<size_t> z_indices;
        z_indices.reserve(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].panel != nullptr && nodes_[i].floating == want_floating) {
                z_indices.push_back(i);
            }
        }
        std::stable_sort(z_indices.begin(), z_indices.end(), [&](size_t a, size_t b) {
            return get_node_layer(nodes_[a]) < get_node_layer(nodes_[b]);
        });
        // `Overlay::assignZOrders` hands an overlay's elements `zorder * 100`, so the 550 and 570
        // overlays own 55001..55999 and 57001..57999. Each panel wants a slot of its own: panels
        // sharing a z share a render-queue priority, where OGRE orders them by material.
        const Ogre::ushort base = static_cast<Ogre::ushort>(root->getZOrder());
        constexpr size_t kSpan = 990;
        const size_t count = z_indices.size();
        for (size_t k = 0; k < count; ++k) {
            // One z each while they fit; past that the range is shared out
            // evenly, which keeps the order although neighbours tie.
            const size_t step = count <= kSpan ? k : (k * (kSpan - 1)) / (count - 1);
            nodes_[z_indices[k]].panel->_notifyZOrder(static_cast<Ogre::ushort>(base + 1 + step));
        }
    }

    layout();
    hide();
    built_ = true;
    TL_LOG_INFO("menu: %zu window(s) built at %u x %u", nodes_.size(), width_, height_);
    return true;
}

// Releases everything `create()` may have taken, at whatever point it got to. Not gated on
// `built_`: a half-built menu still holds textures and materials, and `Renderer::restart()`
// destroys `Ogre::Root` before the old `Impl` goes, so a handle released later would call
// through a freed resource manager.
void InventoryMenu::destroy() {
    // Ogre::Root may already be gone if a caller destroys out of order; there is
    // nothing to unhook in that case, but the resource handles below still have
    // to be dropped while their managers exist, which is why this runs first.
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();

        for (JournalRow& row : journal_rows_) {
            for (Ogre::TextAreaOverlayElement* area :
                 {row.label_drop, row.label, row.value_drop, row.value}) {
                if (area == nullptr) continue;
                if (text_root_ != nullptr) text_root_->removeChild(area->getName());
                overlays.destroyOverlayElement(area);
            }
        }

        destroy_tooltips();

        for (std::vector<Ogre::TextAreaOverlayElement*>* pool :
             {&skill_name_pool_, &skill_desc_pool_, &skill_next_pool_}) {
            for (Ogre::TextAreaOverlayElement* area : *pool) {
                if (text_root_ != nullptr) text_root_->removeChild(area->getName());
                overlays.destroyOverlayElement(area);
            }
            pool->clear();
        }

        for (Node& node : nodes_) {
            Ogre::OverlayContainer* text_home = node.floating ? top_text_root_ : text_root_;
            Ogre::OverlayContainer* panel_home = node.floating ? top_panel_root_ : panel_root_;
            if (node.text != nullptr) {
                if (text_home != nullptr) text_home->removeChild(node.text->getName());
                overlays.destroyOverlayElement(node.text);
            }
            if (node.drop != nullptr) {
                if (text_home != nullptr) text_home->removeChild(node.drop->getName());
                overlays.destroyOverlayElement(node.drop);
            }
            if (node.panel != nullptr) {
                if (panel_home != nullptr) panel_home->removeChild(node.panel->getName());
                overlays.destroyOverlayElement(node.panel);
            }
        }

        if (text_root_ != nullptr) {
            if (text_overlay_ != nullptr) text_overlay_->remove2D(text_root_);
            overlays.destroyOverlayElement(text_root_);
        }
        if (panel_root_ != nullptr) {
            if (overlay_ != nullptr) overlay_->remove2D(panel_root_);
            overlays.destroyOverlayElement(panel_root_);
        }
        if (top_text_root_ != nullptr) {
            if (top_text_overlay_ != nullptr) top_text_overlay_->remove2D(top_text_root_);
            overlays.destroyOverlayElement(top_text_root_);
        }
        if (top_panel_root_ != nullptr) {
            if (top_overlay_ != nullptr) top_overlay_->remove2D(top_panel_root_);
            overlays.destroyOverlayElement(top_panel_root_);
        }
        if (top_text_overlay_ != nullptr) overlays.destroy(top_text_overlay_);
        if (top_overlay_ != nullptr) overlays.destroy(top_overlay_);
        if (text_overlay_ != nullptr) overlays.destroy(text_overlay_);
        if (overlay_ != nullptr) overlays.destroy(overlay_);
    }

    journal_rows_.clear();
    nodes_.clear();
    filtered_indices_.clear();
    text_root_ = nullptr;
    panel_root_ = nullptr;
    text_overlay_ = nullptr;
    overlay_ = nullptr;
    top_text_root_ = nullptr;
    top_panel_root_ = nullptr;
    top_text_overlay_ = nullptr;
    top_overlay_ = nullptr;

    rich_texts_.clear();
    alpha_materials_.clear();
    have_skills_ = false;
    sprites_.clear();
    materials_.clear();
    textures_.clear();
    sets_.clear();

    built_ = false;
    visible_ = false;
}

void InventoryMenu::build(const tl::formats::UiObject& object, int parent,
                          const std::string& prefix, size_t& serial, bool inherited_hidden,
                          bool inherited_always_on_top, float extra_off_x, float extra_off_y,
                          bool inherited_floating) {
    const std::string descriptor = object.descriptor();
    const std::string widget = object.string_or("WIDGET NAME", std::string());

    // The five windows that float over the rest of the screen, and everything under them (in
    // CEGUI each is a separate top-level window, so the data has no key for it); also the quest
    // tab's selection frame and its `Abandon Quest` tray, `ALWAYS ON TOP` over the next title;
    // and the skills tab's legend, which the console moves to the front each time it shows it
    // (`Window::moveToFront_impl`), over the panes after it.
    const bool floating = inherited_floating || widget == "StrHelp" || widget == "DexHelp" ||
                          widget == "MagicHelp" || widget == "DefHelp" || widget == "spendPoints" ||
                          widget == "HelpSkillDialog" ||
                          (prefix == "quest/" && widget == "moveframe");

    Node node;
    node.prefix = prefix;
    node.parent = parent;
    node.widget = widget.empty() ? "" : (prefix + widget);
    node.off_x = object.float_or("OFFSET X", 0.0f) + extra_off_x;
    node.off_y = object.float_or("OFFSET Y", 0.0f) + extra_off_y;
    node.wide =
        object.has("WIDTH") ? object.float_or("WIDTH", 0.0f) : default_extent(descriptor, true);
    node.tall =
        object.has("HEIGHT") ? object.float_or("HEIGHT", 0.0f) : default_extent(descriptor, false);
    node.scale_wide = object.float_or("SCALE WIDTH", 0.0f);
    node.scale_tall = object.float_or("SCALE HEIGHT", 0.0f);
    node.h_anchor = anchor_of(object.string_or("HORIZONTAL", "LEFT"));
    node.v_anchor = anchor_of(object.string_or("VERTICLE", "TOP"));
    node.h_text = anchor_of(object.string_or("HORIZONTAL ALIGN", "LEFT"));
    node.v_text = anchor_of(object.string_or("VERTICAL ALIGN", "TOP"));
    node.floating = floating;
    // A window's own `VISIBLE`, and only its own: a child of a hidden parent is
    // hidden by `is_node_hidden()` walking up to it, exactly as CEGUI hides a
    // subtree, and keeps its own flag for when the parent is shown again. The
    // skill-set rows need that - `SkillSet2` ships `VISIBLE:false` and the tab
    // swaps the two over.
    (void)inherited_hidden;
    node.hidden = !object.bool_or("VISIBLE", true);
    node.runtime_hidden = node.hidden;
    node.always_on_top = inherited_always_on_top || object.bool_or("ALWAYS ON TOP", false);
    // The identify window's text ships `ALPHA:2`, which CEGUI clamps.
    node.own_alpha = std::clamp(object.float_or("ALPHA", 1.0f), 0.0f, 1.0f);
    node.alpha = node.own_alpha * alpha_of(parent);

    auto& overlays = Ogre::OverlayManager::getSingleton();
    const std::string name = prefix + indexed("node_", serial);
    ++serial;

    if (descriptor == "Text" || descriptor == "Text Outlined") {
        node.points = font_points(object.string_or("FONT", "Serif"));
        Ogre::ColourValue colour = Ogre::ColourValue::White;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        const std::string raw_text = object.string_or("TEXT", std::string());

        if (tl::formats::parse_colour(object.string_or("COLOR", std::string()), r, g, b, a)) {
            colour = Ogre::ColourValue(r, g, b, a);
        } else if (raw_text.size() >= 10 && raw_text[0] == '|' &&
                   (raw_text[1] == 'c' || raw_text[1] == 'C')) {
            char hex_buf[9] = {0};
            for (int k = 0; k < 8; ++k) hex_buf[k] = raw_text[2 + k];
            unsigned long argb = std::strtoul(hex_buf, nullptr, 16);
            colour =
                Ogre::ColourValue(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                                  (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f);
        }
        colour.a *= node.alpha;

        std::string text_str = clean_ui_text(raw_text);
        if (object.bool_or("WORD WRAP", false)) {
            text_str = wrap_ui_text(text_str, 26);
        }

        auto make = [&](const std::string& element_name, const Ogre::ColourValue& tint) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", element_name));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            area->setFontName(kFontName);
            area->setColour(tint);
            if (node.h_text == 1) area->setAlignment(Ogre::TextAreaOverlayElement::Center);
            else if (node.h_text == 2) area->setAlignment(Ogre::TextAreaOverlayElement::Right);
            area->setCaption(text_str);
            (floating ? top_text_root_ : text_root_)->addChild(area);
            return area;
        };

        if (descriptor == "Text Outlined") {
            node.drop =
                make(name + "/drop", Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.75f * node.alpha));
        }
        node.text = make(name + "/text", colour);
        node.colour = colour;
    } else if (node.widget == "char/CharacterViewport" ||
               node.widget == "petpage/CharacterViewport") {
        auto* panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", name + "/char_vp"));
        panel->setMetricsMode(Ogre::GMM_RELATIVE);
        panel->setMaterialName("tl/inv/preview_material");
        panel->setUV(0.0f, 0.0f, 1.0f, 1.0f);
        node.panel = panel;
    } else {
        const std::string image = object.string_or("IMAGE", object.string_or("NORMAL", ""));
        // The skill-set rows' `SkillSlotN` / `MapSkillSlotN` are plain windows
        // the console puts a `StaticImage` in at run time (the
        // `gui_` windows), so they get a picture here too.
        const bool slot_window =
            (prefix == "skills/" || prefix == "skillmap/") &&
            (widget.rfind("SkillSlot", 0) == 0 || widget.rfind("MapSkillSlot", 0) == 0);
        if (!image.empty() || descriptor == "Image" || slot_window) {
            node.image = image;
            auto* panel = static_cast<Ogre::PanelOverlayElement*>(
                overlays.createOverlayElement("Panel", name));
            panel->setMetricsMode(Ogre::GMM_RELATIVE);

            const Sprite* hit = sprite(node.image);
            if (hit != nullptr && hit->texture) {
                panel->setMaterialName(material_for(*hit, node.alpha));
                const float tw = static_cast<float>(hit->texture->getWidth());
                const float th = static_cast<float>(hit->texture->getHeight());
                const float u0 = static_cast<float>(hit->rect.x) / tw;
                const float v0 = static_cast<float>(hit->rect.y) / th;
                const float u1 = u0 + static_cast<float>(hit->rect.width) / tw;
                const float v1 = v0 + static_cast<float>(hit->rect.height) / th;
                panel->setUV(u0, v0, u1, v1);
            } else {
                panel->setTransparent(true);
            }
            node.panel = panel;
        }
    }

    const int this_index = static_cast<int>(nodes_.size());
    nodes_.push_back(node);

    if (widget == "spendPoints") {
        Node bg_node;
        bg_node.prefix = prefix;
        bg_node.parent = this_index;
        bg_node.widget = prefix + "spendPoints/bg";
        bg_node.image = "black";
        bg_node.off_x = 4.0f;
        bg_node.off_y = 4.0f;
        bg_node.wide = std::max(0.0f, node.wide - 8.0f);
        bg_node.tall = std::max(0.0f, node.tall - 8.0f);
        bg_node.hidden = node.hidden;
        bg_node.runtime_hidden = node.runtime_hidden;
        bg_node.always_on_top = node.always_on_top;

        auto* bg_panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", name + "/bg"));
        bg_panel->setMetricsMode(Ogre::GMM_RELATIVE);
        const Sprite* hit = sprite("black");
        if (hit != nullptr && hit->texture) {
            bg_panel->setMaterialName(hit->material->getName());
            const float tw = static_cast<float>(hit->texture->getWidth());
            const float th = static_cast<float>(hit->texture->getHeight());
            const float u0 = static_cast<float>(hit->rect.x) / tw;
            const float v0 = static_cast<float>(hit->rect.y) / th;
            const float u1 = u0 + static_cast<float>(hit->rect.width) / tw;
            const float v1 = v0 + static_cast<float>(hit->rect.height) / th;
            bg_panel->setUV(u0, v0, u1, v1);
        } else {
            bg_panel->setTransparent(false);
        }
        bg_node.floating = floating;
        bg_node.panel = bg_panel;
        nodes_.push_back(bg_node);
    }

    // If this element specifies LineFrame look, insert the 9-slice frame behind its content
    if (object.string_or("LOOK N FEEL", "") == "ceguiWidget/LineFrame") {
        struct SliceDef {
            const char* image;
            float ox, oy, wide, tall;
        };
        // A frame sized by its parent (`SCALE WIDTH`/`SCALE HEIGHT` set over a parent of absolute
        // size, as the replace-spell dialog's `CEGUI Object` and the skill legend's) is CEGUI's
        // unified size, scale times the parent's plus the offset; any other keeps the fitted
        // sizes below.
        const Node* parent_node = parent >= 0 ? &nodes_[static_cast<size_t>(parent)] : nullptr;
        const bool by_parent = parent_node != nullptr && node.scale_wide > 0.0f &&
                               node.scale_tall > 0.0f && parent_node->scale_wide == 0.0f &&
                               parent_node->scale_tall == 0.0f;
        const float bw = by_parent            ? node.scale_wide * parent_node->wide + node.wide
                         : (node.wide > 0.0f) ? node.wide
                                              : 265.0f;
        float bh = by_parent            ? node.scale_tall * parent_node->tall + node.tall
                   : (node.tall > 0.0f) ? node.tall
                                        : 90.0f;
        if (node.widget.find("Help") != std::string::npos) {
            bh = 48.0f;
            nodes_[static_cast<size_t>(this_index)].tall = bh;
        }
        // The nine slices, fitted (no `ceguiWidget/LineFrame` look'n'feel reader here; each
        // sprite is a solid 22x22 in `widgets2`) and symmetric about the widget's own box.
        const float kTop = 21.0f;       // height of the three top slices
        const float kBottom = 22.0f;    // height of the three bottom slices
        const float kOverhang = 15.0f;  // how far the frame rises above the box
        const float mw = bw + 4.0f;
        const float mh = std::max(0.0f, bh - 2.0f);
        const float ty = -kOverhang;
        const float my = ty + kTop - 5.0f;
        const float by = bh + kOverhang - kBottom;
        const SliceDef kSlices[] = {
            {"frametopleft", -24.0f, ty, 22.0f, kTop},
            {"frametopedge", -2.0f, ty, mw, kTop},
            {"frametopright", bw + 2.0f, ty, 22.0f, kTop},
            {"frameleftedge", -24.0f, my, 22.0f, mh},
            {"framemiddle", -2.0f, my, mw, mh},
            {"framerightedge", bw + 2.0f, my, 22.0f, mh},
            {"framebottomleft", -24.0f, by, 22.0f, kBottom},
            {"framebottomedge", -2.0f, by, mw, kBottom},
            {"framebottomright", bw + 2.0f, by, 22.0f, kBottom},
        };

        for (size_t s = 0; s < std::size(kSlices); ++s) {
            const auto& def = kSlices[s];
            Node sn;
            sn.prefix = prefix;
            sn.parent = this_index;
            sn.widget = node.widget.empty() ? "" : (node.widget + "/slice_" + std::to_string(s));
            sn.image = def.image;
            sn.off_x = def.ox;
            sn.off_y = def.oy;
            sn.wide = def.wide;
            sn.tall = def.tall;
            sn.hidden = node.hidden;
            sn.runtime_hidden = node.runtime_hidden;
            sn.always_on_top = node.always_on_top;
            sn.floating = floating;

            auto* p = static_cast<Ogre::PanelOverlayElement*>(
                overlays.createOverlayElement("Panel", name + "/slice_" + std::to_string(s)));
            p->setMetricsMode(Ogre::GMM_RELATIVE);

            const Sprite* hit = sprite(sn.image);
            if (hit != nullptr && hit->texture) {
                p->setMaterialName(hit->material->getName());
                const float tw = static_cast<float>(hit->texture->getWidth());
                const float th = static_cast<float>(hit->texture->getHeight());
                const float u0 = static_cast<float>(hit->rect.x) / tw;
                const float v0 = static_cast<float>(hit->rect.y) / th;
                const float u1 = u0 + static_cast<float>(hit->rect.width) / tw;
                const float v1 = v0 + static_cast<float>(hit->rect.height) / th;
                p->setUV(u0, v0, u1, v1);
            } else {
                p->setTransparent(true);
            }
            sn.panel = p;
            nodes_.push_back(sn);
        }
    }

    std::vector<const tl::formats::UiObject*> children;
    children.reserve(object.children.size());
    for (const tl::formats::UiObject& child : object.children) children.push_back(&child);
    std::stable_sort(children.begin(), children.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });

    for (const tl::formats::UiObject* child : children) {
        build(*child, this_index, prefix, serial, node.hidden, node.always_on_top, 0.0f, 0.0f,
              floating);
    }
}

void InventoryMenu::layout() {
    scale_ = static_cast<float>(height_) / 768.0f;
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);

    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        const float parent_left =
            node.parent < 0 ? 0.0f : nodes_[static_cast<size_t>(node.parent)].left * across;
        const float parent_top =
            node.parent < 0 ? 0.0f : nodes_[static_cast<size_t>(node.parent)].top * down;
        const float parent_wide =
            node.parent < 0 ? across : nodes_[static_cast<size_t>(node.parent)].width * across;
        const float parent_tall =
            node.parent < 0 ? down : nodes_[static_cast<size_t>(node.parent)].height * down;

        float left = node.off_x * scale_;
        float top = node.off_y * scale_;
        const float wide = node.scale_wide * parent_wide + node.wide * scale_;
        const float tall = node.scale_tall * parent_tall + node.tall * scale_;

        if (node.h_anchor == 1) left += (parent_wide - wide) * 0.5f;
        else if (node.h_anchor == 2) left += parent_wide - wide;
        if (node.v_anchor == 1) top += (parent_tall - tall) * 0.5f;
        else if (node.v_anchor == 2) top += parent_tall - tall;

        node.left = (parent_left + left) / across;
        node.top = (parent_top + top) / down;
        node.width = wide / across;
        node.height = tall / down;

        const bool is_hidden = is_node_hidden(static_cast<int>(i));

        if (node.panel != nullptr) {
            if (is_hidden) {
                node.panel->hide();
            } else {
                node.panel->show();
                node.panel->setPosition(node.left, node.top);
                node.panel->setDimensions(node.width, node.height);
            }
        }

        if (node.text != nullptr) {
            if (is_hidden) {
                node.text->hide();
                if (node.drop != nullptr) node.drop->hide();
            } else {
                node.text->show();
                const float line = node.points * ui_points_to_pixels() * scale_;
                float x = node.left;
                if (node.h_text == 1) x = node.left + node.width * 0.5f;
                else if (node.h_text == 2) x = node.left + node.width;

                float y = node.top;
                if (node.v_text == 1) y = node.top + (tall - line) * 0.5f / down;
                else if (node.v_text == 2) y = node.top + (tall - line) / down;

                if (node.parent >= 0 && nodes_[static_cast<size_t>(node.parent)].widget.find(
                                            "Help") != std::string::npos) {
                    x += 16.0f * scale_ / across;
                    y -= 6.0f * scale_ / down;
                }

                const float nudge = (node.drop != nullptr) ? kLabelShadow * scale_ : 0.0f;
                set_char_height(node.text, line / down);
                node.text->setPosition(x, y);

                if (node.drop != nullptr) {
                    node.drop->show();
                    set_char_height(node.drop, line / down);
                    node.drop->setPosition(x + nudge / across, y + nudge / down);
                }
            }
        }
    }

    // The journal's rows: each window `kJournalRowWidth` x `kJournalRowHeight`
    // at `(0, kJournalRowPitch * row)` in `journalwindow`, all scaled; the text top-aligned, the
    // label from the left edge and the value to the right. The tooltip and the add/remove-point
    // panel float over them in `top_overlay_`.
    const int jw_idx = find_node_index_by_widget("journal/journalwindow");
    const bool rows_shown = visible_ && menu_mode_ == MenuMode::Stats && jw_idx >= 0;
    if (rows_shown) {
        const Node& jw = nodes_[static_cast<size_t>(jw_idx)];
        const float jw_left = jw.left * across;
        const float jw_top = jw.top * down;
        const float line = kFontSize * ui_points_to_pixels() * scale_;  // `Serif`
        const float drop = kLabelShadow * scale_;
        for (size_t r = 0; r < journal_rows_.size(); ++r) {
            JournalRow& row = journal_rows_[r];
            const float x = jw_left;
            const float right = jw_left + tl::core::kJournalRowWidth * scale_;
            const float y = jw_top + tl::core::kJournalRowPitch * static_cast<float>(r) * scale_;
            auto place = [&](Ogre::TextAreaOverlayElement* area, float px, float py) {
                set_char_height(area, line / down);
                area->setPosition(px / across, py / down);
                area->show();
            };
            place(row.label_drop, x + drop, y + drop);
            place(row.label, x, y);
            place(row.value_drop, right + drop, y + drop);
            place(row.value, right, y);
        }
    } else {
        hide_journal_rows();
    }
    lay_out_rich_texts();
}

void InventoryMenu::hide_journal_rows() {
    for (JournalRow& row : journal_rows_) {
        for (Ogre::TextAreaOverlayElement* area :
             {row.label_drop, row.label, row.value_drop, row.value}) {
            if (area != nullptr) area->hide();
        }
    }
}

// CJournalXenon's fill: the captions. Each text is one
// `|cFFC8C8C8 .. |u` run, drawn in the run's colour; the value window's own
// `TextColour` (1, 0.88, 0.24) colours nothing outside it.
void InventoryMenu::fill_journal(const tl::core::JournalStatistics& store, float play_time) {
    const auto rows = tl::core::journal_page_rows(store, play_time);
    auto caption = [](Ogre::TextAreaOverlayElement* text, Ogre::TextAreaOverlayElement* drop,
                      const std::string& marked) {
        if (text == nullptr) return;
        Ogre::ColourValue colour = Ogre::ColourValue::White;  // the look's `TextColour`
        std::string plain;
        for (const tl::core::MarkupRun& run : tl::core::markup_runs(marked)) {
            if (run.coloured && plain.empty())
                colour = Ogre::ColourValue(run.r, run.g, run.b, run.a);
            plain += run.text;
        }
        text->setColour(colour);
        text->setCaption(plain);
        if (drop != nullptr) drop->setCaption(plain);
    };
    for (size_t r = 0; r < journal_rows_.size() && r < rows.size(); ++r) {
        JournalRow& row = journal_rows_[r];
        caption(row.label, row.label_drop, rows[r].label);
        caption(row.value, row.value_drop, rows[r].value);
    }
}

bool InventoryMenu::is_node_hidden(int idx) const {
    while (idx >= 0 && idx < static_cast<int>(nodes_.size())) {
        const auto& n = nodes_[static_cast<size_t>(idx)];
        if (n.runtime_hidden) return true;
        idx = n.parent;
    }
    return false;
}

void InventoryMenu::set_uv(Node& node, float u0, float v0, float u1, float v1) const {
    if (node.panel != nullptr) {
        node.panel->setUV(u0, v0, u1, v1);
    }
}

InventoryMenu::Node* InventoryMenu::find_node_by_widget(const std::string& widget_name) {
    if (widget_name.empty()) return nullptr;
    for (Node& node : nodes_) {
        if (node.widget == widget_name) return &node;
    }
    return nullptr;
}

int InventoryMenu::find_node_index_by_widget(const std::string& widget_name) const {
    if (widget_name.empty()) return -1;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].widget == widget_name) return static_cast<int>(i);
    }
    return -1;
}

void InventoryMenu::set_widget_text(const std::string& widget_name, const std::string& text) {
    Node* n = find_node_by_widget(widget_name);
    if (!n) return;
    const std::string cleaned = clean_ui_text(text);
    if (n->text != nullptr) n->text->setCaption(cleaned);
    if (n->drop != nullptr) n->drop->setCaption(cleaned);
}

void InventoryMenu::set_widget_image(const std::string& widget_name,
                                     const std::string& image_name) {
    Node* n = find_node_by_widget(widget_name);
    if (!n || n->panel == nullptr) return;
    n->image = image_name;
    const Sprite* s = sprite(image_name);
    if (s != nullptr && s->texture) {
        n->panel->setTransparent(false);
        n->panel->setMaterialName(material_for(*s, n->alpha));
        const float tw = static_cast<float>(s->texture->getWidth());
        const float th = static_cast<float>(s->texture->getHeight());
        const float u0 = static_cast<float>(s->rect.x) / tw;
        const float v0 = static_cast<float>(s->rect.y) / th;
        const float u1 = u0 + static_cast<float>(s->rect.width) / tw;
        const float v1 = v0 + static_cast<float>(s->rect.height) / th;
        n->panel->setUV(u0, v0, u1, v1);
        n->panel->show();
    } else {
        n->panel->setTransparent(true);
    }
}

void InventoryMenu::set_children_visible(const std::string& widget_name, bool visible) {
    // By node rather than by name: not every child has a `WIDGET NAME` (the
    // rows' coin window has none). Shown again, each takes its authored
    // `VISIBLE` back.
    const int parent = find_node_index_by_widget(widget_name);
    if (parent < 0) return;
    std::vector<int> children;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].parent != parent) continue;
        nodes_[i].runtime_hidden = visible ? nodes_[i].hidden : true;
        children.push_back(static_cast<int>(i));
    }
    refresh_node_visibility(children);
}

void InventoryMenu::set_widget_visible(const std::string& widget_name, bool visible) {
    if (widget_name.empty()) return;

    std::vector<int> target_indices;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].widget == widget_name) {
            target_indices.push_back(static_cast<int>(i));
            nodes_[i].runtime_hidden = !visible;
        }
    }
    refresh_node_visibility(target_indices);
}

void InventoryMenu::refresh_node_visibility(const std::vector<int>& target_indices) {
    if (target_indices.empty()) return;

    for (size_t i = 0; i < nodes_.size(); ++i) {
        bool is_affected = false;
        int p = static_cast<int>(i);
        while (p >= 0 && p < static_cast<int>(nodes_.size())) {
            for (int t : target_indices) {
                if (p == t) {
                    is_affected = true;
                    break;
                }
            }
            if (is_affected) break;
            p = nodes_[p].parent;
        }
        if (!is_affected) continue;

        Node& n = nodes_[i];
        const bool hidden = is_node_hidden(static_cast<int>(i));
        if (n.panel != nullptr) {
            if (hidden) n.panel->hide();
            else n.panel->show();
        }
        if (n.text != nullptr) {
            if (hidden) n.text->hide();
            else n.text->show();
        }
        if (n.drop != nullptr) {
            if (hidden) n.drop->hide();
            else n.drop->show();
        }
    }
}

void InventoryMenu::set_widget_position(const std::string& widget_name, float off_x, float off_y) {
    Node* n = find_node_by_widget(widget_name);
    if (!n) return;
    n->off_x = off_x;
    n->off_y = off_y;
    layout();
}

void InventoryMenu::show() {
    if (!built_) return;
    visible_ = true;
    overlay_->show();
    text_overlay_->show();
    if (top_overlay_ != nullptr) top_overlay_->show();
    if (top_text_overlay_ != nullptr) top_text_overlay_->show();
    layout();
}

void InventoryMenu::open_vendor(VendorScreen screen) {
    if (!built_) return;
    const bool enchanter = screen == VendorScreen::Enchant || screen == VendorScreen::DestroyGems ||
                           screen == VendorScreen::RecoverGems || screen == VendorScreen::Retire ||
                           screen == VendorScreen::EnchantShrine;
    if (enchanter && !have_enchanter_) {
        TL_LOG_WARN("inventory menu: no %s, so the enchanter's screen cannot open",
                    kEnchanterLayout);
        return;
    }
    vendor_ = screen;
    menu_mode_ = MenuMode::Inventory;
    // `CMerchantMenuXenon::vfunc_7` on opening: the vendor's
    // tray takes the focus, unless it is a combiner's (then the bag does).
    // `CEnchantMenuXenon` has no vendor's tray: its two are the bag (`+0x10c`)
    // and the pet's (`+0x110`, `vfunc_6`), and the bag starts.
    focus_ = screen == VendorScreen::Combine || enchanter ? Tray::Bag : Tray::Vendor;
    // `vfunc_6` leaves `resultofenchant` alone; an enchanter's first refresh
    // empties it, but a shrine's page refreshes nothing (returns
    // without an NPC), so it opens on whatever line the window last held.
    if (screen != VendorScreen::EnchantShrine) enchant_result_.clear();
    enchant_allowed_ = true;
    enchant_pane_slot_ = -2;
    vendor_selected_ = 0;
    vendor_tab_pending_ = true;
    show();
}

// PORT: the vendor tray's stock and the cursor on its first row (the renderer's interface).
void InventoryMenu::set_vendor_stock(const tl::core::Inventory* stock, bool gambler) {
    vendor_stock_ = stock;
    vendor_gambler_ = gambler;
    vendor_selected_ = 0;
}

void InventoryMenu::set_trade_figures(int gold, const float* price_bonus) {
    trade_gold_ = gold;
    have_price_bonus_ = price_bonus != nullptr;
    price_bonus_ = price_bonus != nullptr ? *price_bonus : 0.0f;
}

std::string InventoryMenu::wrap_to_widget(const std::string& widget_name, const std::string& text) {
    const int idx = find_node_index_by_widget(widget_name);
    if (idx < 0) return text;
    const Node& node = nodes_[static_cast<size_t>(idx)];
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    const float line = node.points * ui_points_to_pixels() * scale_ / down;
    Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(kFontName);
    if (!font) return text;
    if (!font->isLoaded()) font->load();
    std::string out;
    const std::vector<std::string> lines = wrap_text(font, text, node.width, line, across, down);
    for (size_t l = 0; l < lines.size(); ++l) {
        if (l != 0) out += "\n";
        out += lines[l];
    }
    return out;
}

void InventoryMenu::set_enchant_result(const std::string& text, bool succeeded) {
    enchant_result_ = text;
    enchant_result_ok_ = succeeded;
}

void InventoryMenu::set_gem_result(const std::string& text) {
    if (vendor_ == VendorScreen::RecoverGems) recover_gems_result_ = text;
    if (vendor_ == VendorScreen::DestroyGems) destroy_gems_result_ = text;
}

void InventoryMenu::update_gem_pane(const tl::core::Inventory& inv) {
    // the function's tail for modes 3 and 2: `pressbuttongetsocket` (window
    // 11) or `pressbuttondestroysocket` (12) shown when the bag's selected item
    // (`+0x10c`'s, whichever tray has the focus) holds a gem.
    const int slot = selected_slot_index();
    const bool gems = slot >= 0 && inv.occupied(static_cast<size_t>(slot)) &&
                      tl::core::item_has_gems(inv.slots()[static_cast<size_t>(slot)].item);
    const bool destroy = vendor_ == VendorScreen::DestroyGems;
    set_widget_visible(
        destroy ? "enchant/pressbuttongetsocket" : "enchant/pressbuttondestroysocket", gems);
    // The lines wrote, in the colour the layout
    // authors (`FFFFFF00`; neither routine sets one). The windows author
    // `WORD WRAP`.
    const std::string widget = destroy ? "enchant/getsocketresult" : "enchant/destroysocketresult";
    set_widget_text(widget,
                    wrap_to_widget(widget, destroy ? destroy_gems_result_ : recover_gems_result_));
}

bool InventoryMenu::open_item_select() {
    if (!built_ || !have_housing_) return false;
    // > vfunc +0xc with 1 (`vfunc_3(1)`), which empties the
    // choice (`+0x30`); the tray starts at its first row.
    select_open_ = true;
    select_choice_ = -1;
    select_selected_ = 0;
    return true;
}

// PORT: the renderer's interface: the item select closed.
void InventoryMenu::close_item_select() { select_open_ = false; }

void InventoryMenu::update_pet_page(const tl::core::Pet& pet) {
    // the panel's refresh, for an owner that ISA 0x57 (the
    // panel's `+0xa0`, set). The pet's page has no title, level,
    // XP or fame windows, so only these of its fields are found.
    set_widget_text("petpage/characterName", pet.stats().display_name);
    // `HP`: `maxHP()`, red below the base (`+0x380`), white at it,
    // `FFc0c0ff` above; the pet's maximum carries no effects here, so it is the
    // base and white.
    set_widget_text("petpage/HP", std::to_string(static_cast<int>(pet.max_health())));
    set_widget_text_colour("petpage/HP", Ogre::ColourValue::White);
    // INVENTED: the pet's mana is not modelled (the HUD's bar is held full);
    // `Mana` keeps the layout's 0.
    // The four stats (-
    // `CCharacter::strength` and its siblings): the
    // base fields (`+0x42c`.. ) no pet's `unitInit` sets, so 0, plus the pet's
    // effects - its own manager and what it wears.
    const tl::core::CharacterProgression no_base;
    const tl::core::CharacterEffects pet_effects(&pet.effects(), &pet.bag());
    set_widget_text("petpage/strPts", std::to_string(no_base.strength(pet_effects)));
    set_widget_text("petpage/dexPts", std::to_string(no_base.dexterity(pet_effects)));
    set_widget_text("petpage/magicPts", std::to_string(no_base.magic(pet_effects)));
    set_widget_text("petpage/defPts", std::to_string(no_base.defense(pet_effects)));
    // The two spell rows: `+0x90` caches each slot's skill id, -1 at the start,
    // and a row is written only when the id changes - so the loader's "No Spell
    // Equipped" (`SpellText%d`) stays until a slot has held a
    // spell. A slot's skill is `getKnownSpell`, its id `+0x180`;
    // with one, `spellIcon%d` takes its `SKILL_ICON` (`+0x94`) and the text its
    // name (`+0xe8`), "\nRank: " and its invested level (`+0xa4`); without,
    // both are emptied.
    for (size_t slot = 0; slot < pet_page_spells_.size(); ++slot) {
        const tl::core::KnownSkill* spell = pet.skills().known_spell(slot);
        const int64_t id = spell != nullptr ? spell->guid() : -1;
        const std::string text = "petpage/SpellText" + std::to_string(slot);
        const std::string icon = "petpage/spellIcon" + std::to_string(slot);
        if (pet_page_spells_[slot] == -1 && id == -1) {
            set_widget_text(text, "No Spell Equipped");
            set_widget_image(icon, "");
        } else if (id != pet_page_spells_[slot]) {
            if (spell == nullptr) {
                set_widget_text(text, "");
                set_widget_image(icon, "");
            } else {
                set_widget_image(icon, spell->def != nullptr ? spell->def->icon : "");
                set_widget_text(text, (spell->def != nullptr ? spell->def->display_name : "") +
                                          "\nRank: " + std::to_string(spell->invested));
            }
        }
        pet_page_spells_[slot] = id;
    }
}

bool InventoryMenu::open_pet_commands() {
    if (!built_ || !have_pet_commands_) return false;
    // code 0xd: the dialog's `+0x2c` = the player's first pet,
    // its refresh (vtable +0x14, `vfunc_5`), then vtable +0xc with 1. Never
    // over the item-select dialog: both are `CGameUI`'s modal messages.
    select_open_ = false;
    pet_commands_open_ = true;
    return true;
}

void InventoryMenu::update_pet_commands() {
    // `CMessageHousingXenon::vfunc_4` hides the housing's
    // `closebutton` (`setVisible(false)`); the item select's
    // `vfunc_5` shows it again, this dialog never does - it has its own B row.
    set_widget_visible("housing/closebutton", false);
    // `vfunc_5`.
    const tl::core::PetCommandsView& view = pet_commands_view_;
    set_widget_image("petcmd/PetAggressive", view.aggressive_image);
    set_widget_image("petcmd/PetDefensive", view.defensive_image);
    set_widget_image("petcmd/PetPassive", view.passive_image);
    set_widget_text("petcmd/stancetext", view.stance_text);
    set_widget_visible("petcmd/sendtotown", view.send_visible);
}

std::string InventoryMenu::tray_caption(const tl::core::ItemStats& item, size_t slot,
                                        bool pet_tray) const {
    if (unit_types_ == nullptr) return "";
    tl::core::TrayCaptionContext owner;
    if (tray_caption_tests_) {
        owner = tray_caption_tests_(item, pet_tray);
    } else {
        owner.owner_is_player = !pet_tray;
        owner.owner_is_pet = pet_tray;
    }
    return tl::core::tray_a_caption(item, slot, *unit_types_, owner);
}

bool InventoryMenu::open_spell_remove() {
    if (!built_ || !have_spell_remove_) return false;
    // the dialog's `+0x2c` = the owner, its choice cleared,
    // its refresh (vtable +0x14), then vtable +0xc with 1 (`vfunc_3(1)`, which
    // sets the choice to 0 and refreshes again). One modal message at a time.
    select_open_ = false;
    pet_commands_open_ = false;
    spell_remove_open_ = true;
    return true;
}

void InventoryMenu::update_spell_remove() {
    // `CMessageHousingXenon::vfunc_4` hides the housing's `closebutton`; the
    // dialog has its own B row.
    set_widget_visible("housing/closebutton", false);
    // `vfunc_5`: the row fill for each slot, on the
    // eight windows its table holds (`spell0..3`, `spellimage0..3`).
    for (size_t i = 0; i < spell_remove_rows_.size(); ++i) {
        const tl::core::SpellRemoveRow& row = spell_remove_rows_[i];
        const std::string box = "spellrm/spell" + std::to_string(i);
        set_widget_visible(box, row.shown);
        if (!row.shown) continue;
        set_widget_image(box, row.box_image);
        set_widget_image("spellrm/spellimage" + std::to_string(i), row.icon_image);
    }
}

void InventoryMenu::confirm_item_select() {
    // `vfunc_2` code 5: `+0x30` = the tray's selected item (null
    // with nothing selected), then the default case: `vfunc_3(0)`.
    select_choice_ =
        select_selected_ >= 0 && select_selected_ < static_cast<int>(select_rows_.size())
            ? static_cast<int>(select_rows_[static_cast<size_t>(select_selected_)])
            : -1;
    close_item_select();
}

void InventoryMenu::item_select_step(int direction) {
    // the selection moves by one within the list.
    if (select_rows_.empty()) {
        select_selected_ = 0;
        return;
    }
    select_selected_ =
        std::clamp(select_selected_ + direction, 0, static_cast<int>(select_rows_.size()) - 1);
}

void InventoryMenu::item_select_step_tab(int direction) {
    // Code 0x13: `+0x128` - 1, at least 0 (a 6 would become 5); code 0x14:
    // (tray, 1). The dialog's tray is the only one, so there is no
    // other tray for the step to move on to; it stops at the last tab.
    select_tab_ = std::clamp(select_tab_ + direction, 0, 5);
}

void InventoryMenu::update_item_select(const tl::core::Inventory& inv,
                                       const tl::core::StatBook& graphs) {
    // for a tray whose `+0x180` is 2: every item of the owner
    // that `ISA(2)`, identified, with a free socket - equipped ones first, then
    // the rest. Here in slot order, which puts the equip locations first, as
    // the bag's rows do.
    select_rows_.clear();
    for (size_t i = 0; i < inv.slots().size(); ++i) {
        if (!inv.occupied(i)) continue;
        if (unit_types_ != nullptr && tl::core::item_takes_gem(inv.slots()[i].item, *unit_types_)) {
            select_rows_.push_back(i);
        }
    }
    if (select_rows_.empty()) {
        select_selected_ = 0;
    } else {
        select_selected_ =
            std::clamp(select_selected_, 0, static_cast<int>(select_rows_.size()) - 1);
    }

    // The tray as the bag draws itself, focused.
    set_widget_text("select/characterName", "My Bag");
    set_widget_image("select/TopFrame", "backroundbase_inventory_rollover");
    set_widget_image("select/HiliteFrame", "framea_rollover");
    static const char* const kTabs[6][2] = {
        {"select/inventory_filter_weapons", "tab_weapons"},
        {"select/inventory_filter_armor", "tab_armor"},
        {"select/inventory_filter_jewelry", "tab_jewelry"},
        {"select/inventory_filter_potions", "tab_potions"},
        {"select/inventory_filter_spells", "tab_spells"},
        {"select/inventory_filter_fish", "tab_fish"},
    };
    for (int tab = 0; tab < 6; ++tab) {
        set_widget_image(kTabs[tab][0],
                         std::string(kTabs[tab][1]) + (select_tab_ == tab ? "_rollover" : ""));
    }
    static const char* const kHi[6] = {"select/weaponHi", "select/armorHi",  "select/trinketHi",
                                       "select/potionHi", "select/scrollHi", "select/fishHi"};
    for (int tab = 0; tab < 6; ++tab) set_widget_visible(kHi[tab], select_tab_ == tab);
    set_widget_text("select/filtername", tl::core::inventory_filter_name(select_tab_));
    set_widget_visible("select/leftbumper", false);
    set_widget_visible("select/rightbumper", false);
    char size_buf[32];
    std::snprintf(size_buf, sizeof(size_buf), "%zu/%d", inv.size(),
                  tl::core::tray_count_capacity(tl::core::TrayOwner::Player));
    set_widget_text("select/inventorysize", size_buf);
    set_widget_text("select/playergoldtext", std::to_string(inv.gold()));
    set_widget_visible("select/buyandsell", false);
    set_widget_visible("select/petintown", false);
    set_widget_visible("select/transmutor", false);
    set_widget_visible("select/IDENTIFYWIN", false);

    const int select_scroll =
        tray_scroll("select/", select_tab_, select_selected_, select_rows_.size());
    fill_tray_rows("select/", inv, select_rows_, select_selected_, select_scroll, false);
    place_tray_frame("select/", select_selected_, select_scroll, select_rows_.size(), true);
    // for a tray whose `+0x180` is not 0: A "Insert",
    // nothing on X or Y.
    set_tray_legend("select/", "Insert", "", "");

    // `vfunc_5`: the close button's position, in its parent,
    // `(tray left + (tray width - its width) * 0.5, tray bottom - its height *
    // 0.125)`. The tray sits at (0, 0) of the same window.
    const Node* tray = find_node_by_widget("select/TopFrame");
    const Node* close = find_node_by_widget("housing/closebutton");
    if (tray != nullptr && close != nullptr) {
        set_widget_position("housing/closebutton", (tray->wide - close->wide) * 0.5f,
                            tray->tall - close->tall * 0.125f);
    }
}

void InventoryMenu::update_enchant_pane(const tl::core::Inventory& inv,
                                        const tl::core::StatBook& graphs) {
    if (vendor_ == VendorScreen::EnchantShrine) {
        // returns at once without an NPC (`+0xf0`): nothing on a
        // shrine's page is refreshed. What changes it after A:
        // the result line and its colour, and `pressbutton` shown per the
        // shrine's `+0x19c` - hidden once it is spent, and a spent shrine's
        // page cannot be opened again.
        set_widget_text("enchant/resultofenchant",
                        wrap_to_widget("enchant/resultofenchant", enchant_result_));
        set_widget_text_colour("enchant/resultofenchant",
                               enchant_result_ok_ ? Ogre::ColourValue(1.0f, 1.0f, 0.0f, 1.0f)
                                                  : Ogre::ColourValue(1.0f, 0.0f, 0.0f, 1.0f));
        if (!enchant_allowed_) set_widget_visible("enchant/pressbutton", false);
        return;
    }
    // The selected item (`+0x124`): the bag's, the only tray taking focus.
    const int slot = focus_ == Tray::Bag ? selected_slot_index() : -1;
    const tl::core::ItemStats* item = slot >= 0 && inv.occupied(static_cast<size_t>(slot))
                                          ? &inv.slots()[static_cast<size_t>(slot)].item
                                          : nullptr;
    // `vfunc_9`: a new selection refreshes the pane, and the
    // refresh empties `resultofenchant`; what wrote
    // there after an enchant lasts until then.
    if (item == nullptr || slot != enchant_pane_slot_) enchant_result_.clear();
    enchant_pane_slot_ = item != nullptr ? slot : -1;
    // The window authors `WORD WRAP`.
    set_widget_text("enchant/resultofenchant",
                    wrap_to_widget("enchant/resultofenchant", enchant_result_));
    set_widget_text_colour("enchant/resultofenchant",
                           enchant_result_ok_ ? Ogre::ColourValue(1.0f, 1.0f, 0.0f, 1.0f)
                                              : Ogre::ColourValue(1.0f, 0.0f, 0.0f, 1.0f));

    // for mode 0. Its red strings (`|cFFFF0000 ... |u`) are drawn
    // here as the caption's colour.
    const Ogre::ColourValue red(1.0f, 0.0f, 0.0f, 1.0f);
    set_widget_visible("enchant/disenchantChance", true);
    set_widget_visible("enchant/priceofenchantment", true);
    set_widget_text_colour("enchant/priceofenchantment", enchant_price_colour_);
    if (item == nullptr || enchant_rules_ == nullptr) {
        set_widget_text("enchant/priceofenchantment", "");
        set_widget_text("enchant/disenchantChance", "");
        set_widget_visible("enchant/pressbutton", false);
        return;
    }
    // `+0x318` (Linux `+0x344`) against `ENCHANTER_MAX_ENCHANTS`, signed.
    if (static_cast<int32_t>(item->enchant_count) >= enchant_rules_->enchanter_max_enchants) {
        set_widget_visible("enchant/pressbutton", false);
        set_widget_text("enchant/priceofenchantment", "Maximum enchantments reached");
        set_widget_text_colour("enchant/priceofenchantment", red);
        set_widget_text("enchant/disenchantChance", "");
        return;
    }
    if (unit_types_ == nullptr || !tl::core::item_can_enchant(*item, *unit_types_)) {
        set_widget_visible("enchant/pressbutton", false);
        set_widget_text("enchant/priceofenchantment", "Cannot enchant item");
        set_widget_text_colour("enchant/priceofenchantment", red);
        set_widget_text("enchant/disenchantChance", "");
        return;
    }
    const int price = tl::core::item_enchant_price(*item, graphs, *enchant_rules_);
    set_widget_text("enchant/priceofenchantment", std::to_string(price));
    if (price < trade_gold_) {
        set_widget_visible("enchant/pressbutton", true);
    } else {
        // Red, and `pressbutton` left as it was.
        set_widget_text_colour("enchant/priceofenchantment", red);
    }
    // "Disenchant chance" + ": " + the float through the console's stream
    // + "%".
    std::ostringstream chance;
    chance << "Disenchant chance: "
           << static_cast<double>(tl::core::enchanter_disenchant_chance(*item, *enchant_rules_))
           << "%";
    set_widget_text("enchant/disenchantChance", chance.str());
}

void InventoryMenu::update_vendor_column(const tl::core::StatBook& graphs) {
    const bool enchanter = enchanter_screen();
    if (enchanter) {
        const bool shrine = vendor_ == VendorScreen::EnchantShrine;
        // One pane of four, by the widget names the layout gives them; the
        // shrine's page (mode 1) is the enchanter's pane.
        set_widget_visible("enchant/enchant", vendor_ == VendorScreen::Enchant || shrine);
        set_widget_visible("enchant/getsocket", vendor_ == VendorScreen::DestroyGems);
        set_widget_visible("enchant/destroysocket", vendor_ == VendorScreen::RecoverGems);
        set_widget_visible("enchant/retire", vendor_ == VendorScreen::Retire);
        // `CEnchantMenuXenon::vfunc_6`: for an NPC the price line
        // (`sellprice`) shown and the shrine's own text hidden; without one the reverse. Its
        // `title` write lands on the Destroy Gems pane's window (the window table is filled
        // depth-first in layout order), so the enchanter's pane keeps "Enchanter".
        // The gem services' and retirement's result captions are editor placeholders and say
        // nothing.
        set_widget_visible("enchant/enchantshrine", shrine);
        set_widget_visible("enchant/sellprice", vendor_ == VendorScreen::Enchant);
        set_widget_visible("enchant/resultofenchant", vendor_ == VendorScreen::Enchant || shrine);
        set_widget_visible("enchant/getsocketresult", vendor_ == VendorScreen::DestroyGems);
        set_widget_visible("enchant/destroysocketresult", vendor_ == VendorScreen::RecoverGems);
        return;
    }

    const bool merchant = vendor_ == VendorScreen::Merchant;
    static const char* const kCaptions[] = {
        "", "Merchant", "Stash", "Shared Stash", "Transmuter", "", "", "", "", ""};
    set_widget_text("vendor/characterName", kCaptions[static_cast<int>(vendor_)]);

    // The merchant's own tab strip; the others keep the bag's.
    static const char* const kTabs[6][3] = {
        {"vendor/inventory_filter_weapons", "merchant_tab_weapons", "tab_weapons"},
        {"vendor/inventory_filter_armor", "merchant_tab_armor", "tab_armor"},
        {"vendor/inventory_filter_jewelry", "merchant_tab_jewelry", "tab_jewelry"},
        {"vendor/inventory_filter_potions", "merchant_tab_potions", "tab_potions"},
        {"vendor/inventory_filter_spells", "merchant_tab_gems", "tab_spells"},
        {"vendor/inventory_filter_fish", "merchant_tab_fish", "tab_fish"},
    };
    const bool focused = focus_ == Tray::Vendor;
    set_widget_image(
        "vendor/TopFrame",
        merchant ? (focused ? "backroundbase_merchant_rollover" : "backroundbase_merchant")
                 : (focused ? "backroundbase_inventory_rollover" : "backroundbase_inventory"));
    set_widget_image("vendor/HiliteFrame", focused ? "framea_rollover" : "framea");
    set_widget_text("vendor/filtername",
                    tl::core::inventory_filter_name(vendor_tab_, vendor_ == VendorScreen::Combine));
    static const char* const kHi[6] = {"vendor/weaponHi", "vendor/armorHi",  "vendor/trinketHi",
                                       "vendor/potionHi", "vendor/scrollHi", "vendor/fishHi"};
    // a combiner's tray has no tabs; the six and their
    // highlights are hidden, which is where the `transmutor` caption sits.
    const bool combiner_tray = vendor_ == VendorScreen::Combine;
    for (int t = 0; t < 6; ++t) {
        set_widget_image(kTabs[t][0], std::string(merchant ? kTabs[t][1] : kTabs[t][2]) +
                                          (t == vendor_tab_ ? "_rollover" : ""));
        set_widget_visible(kTabs[t][0], !combiner_tray);
        set_widget_visible(kHi[t], !combiner_tray && t == vendor_tab_);
    }
    // The bumper hints name the trays either side of the focused one; the
    // vendor's is the leftmost.
    set_widget_visible("vendor/leftbumper", false);
    set_widget_visible("vendor/rightbumper", focused);
    set_widget_visible("vendor/buyandsell", false);
    set_widget_visible("vendor/IDENTIFYWIN", false);
    set_widget_visible("vendor/playergoldwin", false);
    set_widget_visible("vendor/uparrow", false);
    set_widget_visible("vendor/downarrow", false);
    set_widget_visible("vendor/petintown", false);
    set_widget_visible("vendor/petreturntime2", false);
    // "Swap items to the Transmuter to create new items." belongs to the
    // combiner's screen and to no other.
    set_widget_visible("vendor/transmutor", vendor_ == VendorScreen::Combine);
    // A merchant's stock, the Transmuter's own inventory or a stash's, by the
    // same categoriser the bag uses, which lists everything
    // of a combiner's under tab 0.
    const bool combiner = vendor_ == VendorScreen::Combine;
    const bool stash = vendor_ == VendorScreen::Stash || vendor_ == VendorScreen::SharedStash;
    vendor_rows_.clear();
    const tl::core::Inventory* stock = merchant || combiner || stash ? vendor_stock_ : nullptr;
    const auto listed_under = [&](const tl::core::InventorySlot& held) {
        return unit_types_ != nullptr
                   ? tl::core::inventory_filter_of(held.item.unittype, *unit_types_, combiner)
                   : 0;
    };
    if (vendor_tab_pending_ && stock != nullptr) {
        // on opening: the first of the six tabs that lists
        // anything, else the tab the tray already had. The
        // console never asks for `MERCHANTTAB`, which the PC opens on
        // (`CMerchantMenu::setOpen`).
        vendor_tab_pending_ = false;
        for (int tab = 0; tab < tl::core::kInventoryFilterCount; ++tab) {
            bool any = false;
            for (const tl::core::InventorySlot& held : stock->slots()) {
                if (held.count > 0 && listed_under(held) == tab) any = true;
            }
            if (any) {
                vendor_tab_ = tab;
                break;
            }
        }
    }
    if (stock != nullptr) {
        for (size_t i = 0; i < stock->slots().size(); ++i) {
            const tl::core::InventorySlot& held = stock->slots()[i];
            if (held.count <= 0) continue;
            if (listed_under(held) == vendor_tab_) vendor_rows_.push_back(i);
        }
        char size_buf[32];
        if (combiner) {
            // a combiner's tray counts against 4.
            std::snprintf(size_buf, sizeof(size_buf), "%zu/4", vendor_rows_.size());
        } else if (stash) {
            // ... and a stash tray against 50, the shared stash's 42 slots
            // included.
            std::snprintf(size_buf, sizeof(size_buf), "%zu/%d", stock->size(),
                          tl::core::tray_count_capacity(tl::core::TrayOwner::Stash));
        } else {
            std::snprintf(size_buf, sizeof(size_buf), "%zu/%zu", stock->size(), stock->capacity());
        }
        set_widget_text("vendor/inventorysize", size_buf);
    } else {
        set_widget_text("vendor/inventorysize", "");
    }
    vendor_selected_ = vendor_rows_.empty() ? 0
                                            : std::clamp(vendor_selected_, 0,
                                                         static_cast<int>(vendor_rows_.size()) - 1);
    static const tl::core::Inventory kNothing(0, true);
    // prices by the item's owner: a merchant's buy price, and
    // for the Transmuter (not a Merchant) the sell price.
    const int vendor_scroll =
        tray_scroll("vendor/", vendor_tab_, vendor_selected_, vendor_rows_.size());
    fill_tray_rows("vendor/", stock != nullptr ? *stock : kNothing, vendor_rows_, vendor_selected_,
                   vendor_scroll, merchant, combiner ? tl::core::kCombineSlots.size() : 0);
    place_tray_frame("vendor/", vendor_selected_, vendor_scroll, vendor_rows_.size(),
                     focused && (merchant || combiner || stash));
    // on a combiner's tray, or a stash's: X
    // "Swap" and nothing on A or Y.
    if (focused && (combiner || stash)) set_tray_legend("vendor/", "", "Swap", "");
    if (focused && merchant) {
        // on a merchant's tray: X "Buy" and nothing on A or Y; the frame's
        // heading is "Buy".
        set_widget_visible("vendor/buyandsell", true);
        set_widget_text("vendor/buyandselltext", "Buy");
        set_tray_legend("vendor/", "", "Buy", "");
    }
}

void InventoryMenu::set_tray_legend(const std::string& prefix, const std::string& a,
                                    const std::string& x, const std::string& y) {
    set_widget_visible(prefix + "buttonB", false);
    const std::string captions[3][2] = {{"buttonA", a}, {"buttonX", x}, {"buttonY", y}};
    for (const auto& button : captions) {
        set_widget_text(prefix + button[0] + "text", button[1]);
        set_widget_visible(prefix + button[0], !button[1].empty());
    }
}

void InventoryMenu::set_widget_text_colour(const std::string& widget_name,
                                           const Ogre::ColourValue& colour) {
    Node* node = find_node_by_widget(widget_name);
    if (node == nullptr || node->text == nullptr) return;
    node->colour = colour;
    node->text->setColour(colour);
}

int InventoryMenu::tray_scroll(const std::string& prefix, int tab, int selected, size_t row_count) {
    // over the five trays (`+0x60`).
    constexpr uint32_t kRows = 5;
    const uint32_t count = static_cast<uint32_t>(row_count);
    if (count <= kRows) return 0;
    int& stored = tray_scrolls_[prefix + std::to_string(tab)];
    // Unsigned, as the original's: no selection (-1) is past the end.
    uint32_t at = static_cast<uint32_t>(selected);
    if (count <= at) at = count - 1;
    const uint32_t scroll = static_cast<uint32_t>(stored);
    if (scroll <= at) {
        if (kRows + scroll <= at) stored = static_cast<int>(at - kRows + 1);
        return stored;
    }
    stored = static_cast<int>(at);
    return stored;
}

void InventoryMenu::place_tray_frame(const std::string& prefix, int selected, int scroll,
                                     size_t row_count, bool shown) {
    // `moveframe` and the five `ItemTray`s are siblings inside `itemswindow`: the frame's place
    // over row n is the tray's own offset plus `moveframe`'s authored (-16, -18).
    const int tray_pos = selected - scroll;
    const bool have = shown && selected >= 0 && selected < static_cast<int>(row_count) &&
                      tray_pos >= 0 && tray_pos < 5;
    if (!have) {
        set_widget_visible(prefix + "moveframe", false);
        return;
    }
    constexpr float kMoveFrameOffX = -16.0f;
    constexpr float kMoveFrameOffY = -18.0f;
    float tray_x = 0.0f, tray_y = 92.0f * static_cast<float>(tray_pos);
    const int tray_idx = find_node_index_by_widget(prefix + "ItemTray" + std::to_string(tray_pos));
    if (tray_idx >= 0) {
        tray_x = nodes_[static_cast<size_t>(tray_idx)].off_x;
        tray_y = nodes_[static_cast<size_t>(tray_idx)].off_y;
    }
    set_widget_position(prefix + "moveframe", kMoveFrameOffX + tray_x, kMoveFrameOffY + tray_y);
    set_widget_visible(prefix + "moveframe", true);
}

void InventoryMenu::draw_row_sockets(const std::string& prefix, const std::string& s_tray,
                                     const tl::core::ItemStats& item) {
    // the row's socket windows in order, the first as many as
    // the item holds gems (`+0x39c`) `socket_full` (image 0x11), the next as
    // many as its sockets leave empty (`+0x394` - `+0x39c`) `socket_empty`
    // (0x10), and one past both shown only below the socket count (so never).
    const int gems = static_cast<int>(item.socketed.size());
    int empty = item.sockets - gems;
    for (int socket = 0; socket < 2; ++socket) {
        const std::string widget = prefix + "socket" + std::to_string(socket) + "_" + s_tray;
        if (socket < gems) {
            set_widget_image(widget, "socket_full");
            set_widget_visible(widget, true);
        } else if (empty > 0) {
            --empty;
            set_widget_image(widget, "socket_empty");
            set_widget_visible(widget, true);
        } else {
            set_widget_visible(widget, socket < item.sockets);
        }
    }
}

void InventoryMenu::fill_tray_rows(const std::string& prefix, const tl::core::Inventory& inv,
                                   const std::vector<size_t>& rows, int selected, int scroll,
                                   bool merchant_owned, size_t framed_rows) {
    const float* bonus = have_price_bonus_ ? &price_bonus_ : nullptr;
    for (int tray = 0; tray < 5; ++tray) {
        const std::string s_tray = std::to_string(tray);
        const std::string tray_widget = prefix + "ItemTray" + s_tray;
        const int item_idx = scroll + tray;
        // What an empty combiner's frame hid comes back first; the row's code
        // below hides again whatever the item does not use.
        set_children_visible(tray_widget, true);

        // Each of these is authored with a placeholder caption or image ("Damage and Armor",
        // `traycolor_green`, two `socket_empty`s, ...) that the original overwrites or hides per
        // item.
        set_widget_visible(prefix + "socket0_" + s_tray, false);
        set_widget_visible(prefix + "socket1_" + s_tray, false);
        set_widget_visible(prefix + "qualityarrow" + s_tray, false);
        set_widget_visible(prefix + "Quality" + s_tray, false);
        set_widget_visible(prefix + "Enchantments" + s_tray, false);
        set_widget_visible(prefix + "Requirment" + s_tray, false);

        if (item_idx >= static_cast<int>(rows.size())) {
            // a combiner's tray keeps the frame of each of
            // its four slots with nothing drawn in it.
            const bool framed = item_idx < static_cast<int>(framed_rows);
            set_widget_visible(tray_widget, framed);
            if (framed) {
                set_widget_image(tray_widget, "item_tray");
                set_children_visible(tray_widget, false);
                // (tray, 0, 0, row).
                set_widget_alpha(tray_widget, tl::core::tray_row_alpha(false, false));
                continue;
            }
            set_widget_visible(prefix + "Slot" + s_tray, false);
            set_widget_text(prefix + "SlotText" + s_tray, "");
            set_widget_text(prefix + "Equipped" + s_tray, "");
            set_widget_visible(prefix + "Equipped" + s_tray, false);
            set_widget_text(prefix + "Gold" + s_tray, "");
            set_widget_visible(prefix + "DamageArmor" + s_tray, false);
            continue;
        }
        const size_t slot_idx = rows[static_cast<size_t>(item_idx)];
        const auto& slot = inv.slots()[slot_idx];
        const bool equipped = !merchant_owned && tl::core::tray_shows_equipped(slot_idx);
        set_widget_visible(tray_widget, true);
        set_widget_image(tray_widget, equipped ? "item_tray_equipped" : "item_tray");

        // (`core/tray_row.h`): the square behind the icon, the
        // name's colour, the two lines under the name, the arrow and the foot
        // line. A gambler's tray is the merchant's own.
        tl::core::TrayRowContext row_context = row_context_;
        if (row_context.unit_types == nullptr) row_context.unit_types = unit_types_;
        row_context.owner_is_gambler = merchant_owned && vendor_gambler_;
        row_context.merchant_open = vendor_ == VendorScreen::Merchant;
        const tl::core::TrayRow row = tl::core::tray_row(slot.item, slot.count, row_context);
        const auto ogre_colour = [](const tl::core::LabelColour& c) {
            return Ogre::ColourValue(c.r, c.g, c.b, c.a);
        };

        // `getFullItemName(false)`; "???" on a gambler's tray.
        set_widget_text(prefix + "SlotText" + s_tray,
                        row.gambler_name ? std::string("???") : row_item_name(slot.item));
        set_widget_text_colour(prefix + "SlotText" + s_tray, ogre_colour(row.name_colour));

        if (!slot.item.icon.empty()) {
            set_widget_image(prefix + "Slot" + s_tray, slot.item.icon);
            set_widget_visible(prefix + "Slot" + s_tray, true);
        } else {
            set_widget_visible(prefix + "Slot" + s_tray, false);
        }

        // `traycolor_green` is only the editor's placeholder.
        set_widget_image(prefix + "QualityBk" + s_tray,
                         tl::core::tray_backdrop_image(row.backdrop));

        set_widget_text(prefix + "Equipped" + s_tray, equipped ? "Equipped" : "");
        set_widget_visible(prefix + "Equipped" + s_tray, equipped);

        set_widget_text(prefix + "Enchantments" + s_tray, row.enchantments);
        set_widget_visible(prefix + "Enchantments" + s_tray, !row.enchantments.empty());
        if (row.arrow != tl::core::TrayArrow::None) {
            set_widget_image(prefix + "qualityarrow" + s_tray,
                             tl::core::tray_arrow_image(row.arrow));
        }
        set_widget_visible(prefix + "qualityarrow" + s_tray,
                           row.arrow != tl::core::TrayArrow::None);
        set_widget_text(prefix + "Requirment" + s_tray, row.requirement);
        set_widget_text_colour(prefix + "Requirment" + s_tray, ogre_colour(row.requirement_colour));
        set_widget_visible(prefix + "Requirment" + s_tray, !row.requirement.empty());

        // The price: the item's owner a merchant, its buy
        // price, light red (`colour(a 1, r 1, g .5, b .5)`) when the player's
        // gold (`+0x3ac`) is short of it; anything else its sell price, and a
        // quest item none. Otherwise `FFffeb9a`.
        static const Ogre::ColourValue kPrice(0xff / 255.0f, 0xeb / 255.0f, 0x9a / 255.0f, 1.0f);
        std::string price_text;
        Ogre::ColourValue price_colour = kPrice;
        if (merchant_owned) {
            const int price = tl::core::item_buy_price_of(slot.item, slot.count, bonus);
            price_text = std::to_string(price);
            if (trade_gold_ < price) price_colour = Ogre::ColourValue(1.0f, 0.5f, 0.5f, 1.0f);
        } else if (unit_types_ == nullptr ||
                   !unit_types_->isa(slot.item.unittype, tl::core::unit_type::kQuestItem)) {
            price_text = std::to_string(tl::core::item_sell_price_of(slot.item, slot.count, bonus));
        }
        set_widget_text(prefix + "Gold" + s_tray, price_text);
        set_widget_text_colour(prefix + "Gold" + s_tray, price_colour);

        // The line under the name: "DPS: 24", "armor 4", a stack's "x3".
        set_widget_text(prefix + "DamageArmor" + s_tray, row.detail);
        set_widget_visible(prefix + "DamageArmor" + s_tray, !row.detail.empty());

        draw_row_sockets(prefix, s_tray, slot.item);

        // the row's `Alpha`, by the tray's own selection
        // whether or not the tray has the focus.
        set_widget_alpha(tray_widget, tl::core::tray_row_alpha(item_idx == selected, equipped));
    }
}

void InventoryMenu::hide() {
    if (!built_) return;
    visible_ = false;
    vendor_ = VendorScreen::None;
    select_open_ = false;
    pet_commands_open_ = false;
    spell_remove_open_ = false;
    vendor_stock_ = nullptr;
    vendor_rows_.clear();
    focus_ = Tray::Bag;
    overlay_->hide();
    text_overlay_->hide();
    if (top_overlay_ != nullptr) top_overlay_->hide();
    if (top_text_overlay_ != nullptr) top_text_overlay_->hide();
    hide_journal_rows();
    journal_filled_ = false;
    lay_out_rich_texts();
    tooltips_.clear();
    lay_out_tooltips();
}

void InventoryMenu::toggle() {
    if (visible_) hide();
    else show();
}

void InventoryMenu::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    if (built_) layout();
}

// The ring stepping below is `core/inventory_screen.h`'s. `MenuMode`/`current_tab_`'s numbering
// matches `tl::core::HousingTab`/`InventoryFilter`, so the cast is a relabel.
void InventoryMenu::set_menu_mode(MenuMode mode) { menu_mode_ = mode; }

void InventoryMenu::next_menu_tab() {
    menu_mode_ = static_cast<MenuMode>(
        tl::core::step_housing_tab(static_cast<tl::core::HousingTab>(menu_mode_), 1));
}

void InventoryMenu::prev_menu_tab() {
    menu_mode_ = static_cast<MenuMode>(
        tl::core::step_housing_tab(static_cast<tl::core::HousingTab>(menu_mode_), -1));
}

// PORT: a headless verification step: a bag slot selected at the next update.
void InventoryMenu::select_slot(size_t slot) { pending_select_slot_ = static_cast<int>(slot); }

bool InventoryMenu::touch_select_at(float x, float y) {
    if (!visible_ || menu_mode_ != MenuMode::Inventory || select_open_ || pet_commands_open_ ||
        spell_remove_open_) return false;

    const std::string* prefix = nullptr;
    const std::vector<size_t>* rows = nullptr;
    int selected = 0;
    int tab = 0;
    enum class Target { Bag, Pet, Vendor } target = Target::Bag;
    static const std::string bag_prefix = "inv/";
    static const std::string pet_prefix = "pet/";
    static const std::string vendor_prefix = "vendor/";
    if (vendor_ != VendorScreen::None) {
        prefix = &vendor_prefix;
        rows = &vendor_rows_;
        selected = vendor_selected_;
        tab = vendor_tab_;
        target = Target::Vendor;
    } else if (focus_ == Tray::Pet && pet_tray_available_) {
        prefix = &pet_prefix;
        rows = &pet_rows_;
        selected = pet_selected_;
        tab = pet_tab_;
        target = Target::Pet;
    } else {
        prefix = &bag_prefix;
        rows = &filtered_indices_;
        selected = selected_index_;
        tab = current_tab_;
    }
    if (prefix == nullptr || rows == nullptr || rows->empty()) return false;
    const int scroll = tray_scroll(*prefix, tab, selected, rows->size());
    for (int visible_row = 0; visible_row < 5; ++visible_row) {
        const int row = scroll + visible_row;
        if (row < 0 || static_cast<size_t>(row) >= rows->size()) continue;
        const Node* node = find_node_by_widget(*prefix + "ItemTray" + std::to_string(visible_row));
        if (node == nullptr || node->hidden) continue;
        const float left = node->left * static_cast<float>(width_);
        const float top = node->top * static_cast<float>(height_);
        const float right = left + node->width * static_cast<float>(width_);
        const float bottom = top + node->height * static_cast<float>(height_);
        if (x < left || x > right || y < top || y > bottom) continue;
        if (target == Target::Bag) {
            select_slot((*rows)[static_cast<size_t>(row)]);
        } else if (target == Target::Pet) {
            focus_ = Tray::Pet;
            pet_selected_ = row;
        } else {
            focus_ = Tray::Vendor;
            vendor_selected_ = row;
        }
        return true;
    }
    return false;
}

void InventoryMenu::set_current_tab(int tab) {
    current_tab_ = std::clamp(tab, 0, tl::core::kInventoryFilterCount - 1);
    selected_index_ = 0;
}

void InventoryMenu::next_tab() {
    if (menu_mode_ != MenuMode::Inventory) return;
    current_tab_ = static_cast<int>(
        tl::core::step_inventory_filter(static_cast<tl::core::InventoryFilter>(current_tab_), 1));
    selected_index_ = 0;
}

void InventoryMenu::prev_tab() {
    if (menu_mode_ != MenuMode::Inventory) return;
    current_tab_ = static_cast<int>(
        tl::core::step_inventory_filter(static_cast<tl::core::InventoryFilter>(current_tab_), -1));
    selected_index_ = 0;
}

namespace {

/// The two tray pictures `questmenu_xenon.layout` authors: `ItemTray0` ships with the
/// `_rollover` (lit) one and the other five with `_selected`.
constexpr const char* kTraySelected = "load_chartext_window_rollover";
constexpr const char* kTrayIdle = "load_chartext_window_selected";

}  // namespace

// PORT: the cursor moved in the tray that has the focus (`step_selected_index`,
// `step_selected_quest`, which hold the rules).
void InventoryMenu::select_next() {
    if (menu_mode_ == MenuMode::Inventory && vendor_ != VendorScreen::None &&
        focus_ == Tray::Vendor) {
        vendor_selected_ = tl::core::step_selected_index(vendor_selected_, 1, vendor_rows_.size());
    } else if (menu_mode_ == MenuMode::Inventory && pet_tray_focused()) {
        pet_selected_ = tl::core::step_selected_index(pet_selected_, 1, pet_rows_.size());
    } else if (menu_mode_ == MenuMode::Inventory) {
        selected_index_ =
            tl::core::step_selected_index(selected_index_, 1, filtered_indices_.size());
    } else if (menu_mode_ == MenuMode::Quest) {
        set_selected_quest(selected_quest_ + 1);
    }
}

// PORT: the cursor moved in the tray that has the focus (`step_selected_index`,
// `step_selected_quest`, which hold the rules).
void InventoryMenu::select_prev() {
    if (menu_mode_ == MenuMode::Inventory && vendor_ != VendorScreen::None &&
        focus_ == Tray::Vendor) {
        vendor_selected_ = tl::core::step_selected_index(vendor_selected_, -1, vendor_rows_.size());
    } else if (menu_mode_ == MenuMode::Inventory && pet_tray_focused()) {
        pet_selected_ = tl::core::step_selected_index(pet_selected_, -1, pet_rows_.size());
    } else if (menu_mode_ == MenuMode::Inventory) {
        selected_index_ =
            tl::core::step_selected_index(selected_index_, -1, filtered_indices_.size());
    } else if (menu_mode_ == MenuMode::Quest) {
        set_selected_quest(selected_quest_ - 1);
    }
}

// PORT: the journal's row set through `step_selected_quest`, which holds the rule.
void InventoryMenu::set_selected_quest(int index) {
    selected_quest_ = tl::core::step_selected_quest(index, 0, tl::core::kQuestLogSize);
}

// PORT: the stats page's view taken.
void InventoryMenu::set_stats_view(const tl::core::StatsPageView& view, int selected) {
    stats_view_ = view;
    stats_selected_ = selected;
}

// and the refresh it ends with, window by window
// (`CStatsXenon`'s 30 names, built from the table).
void InventoryMenu::show_stats_view() {
    static const char* const kPanels[4] = {"stats/strength", "stats/dexterity", "stats/magic",
                                           "stats/defense"};
    static const char* const kPoints[4] = {"stats/strPts", "stats/dexPts", "stats/magicPts",
                                           "stats/defPts"};
    static const char* const kDamage[4] = {"stats/strDMG", "stats/dexDMG", "stats/magicDMG",
                                           "stats/defArmor"};
    static const char* const kResistances[4] = {"stats/poisonRes", "stats/fireRes", "stats/iceRes",
                                                "stats/eletricRes"};
    static const char* const kAdded[4] = {"stats/strPtsAdded", "stats/dexPtsAdded",
                                          "stats/magicPtsAdded", "stats/defPtsAdded"};
    static const char* const kHelp[4] = {"stats/StrHelp", "stats/DexHelp", "stats/MagicHelp",
                                         "stats/DefHelp"};
    auto colour = [](uint32_t argb) {
        return Ogre::ColourValue(static_cast<float>((argb >> 16) & 0xff) / 255.0f,
                                 static_cast<float>((argb >> 8) & 0xff) / 255.0f,
                                 static_cast<float>(argb & 0xff) / 255.0f,
                                 static_cast<float>(argb >> 24) / 255.0f);
    };
    const tl::core::StatsPageView& v = stats_view_;
    for (int i = 0; i < 4; ++i) {
        // `statwindow_base_rollover` on the selection, else
        // `statwindow_base`.
        set_widget_image(kPanels[i],
                         i == stats_selected_ ? "statwindow_base_rollover" : "statwindow_base");
        set_widget_text(kPoints[i], v.points[i]);
        set_widget_text_colour(kPoints[i], colour(v.points_colour[i]));
        set_widget_text(kDamage[i], v.damage[i]);
        set_widget_text_colour(kDamage[i], colour(v.damage_colour[i]));
        set_widget_text(kResistances[i], v.resistances[i]);
        set_widget_text_colour(kResistances[i], colour(0xFFFFFFFFu));
        set_widget_text(kAdded[i], v.added[i]);
        set_widget_visible(kAdded[i], v.added_shown[i]);
        set_widget_visible(kHelp[i], v.help_shown[i]);
    }
    set_widget_text("stats/ptsToSpend", v.to_spend);
    set_widget_text_colour("stats/ptsToSpend", colour(0xFFFFFFFFu));
    // the frame over the selected panel, at the panel's place plus
    // the offset the build measured.
    if (const Node* panel = find_node_by_widget(kPanels[stats_selected_])) {
        set_widget_position("stats/pieceToMove", panel->off_x + stat_frame_dx_,
                            panel->off_y + stat_frame_dy_);
    }
    set_widget_alpha("stats/addPoint", v.add_alpha);
    set_widget_alpha("stats/removePoint", v.remove_alpha);
    set_widget_visible("stats/spendPoints", v.spend_shown);
}

// PORT: the bag slot under the cursor.
int InventoryMenu::selected_slot_index() const {
    if (filtered_indices_.empty() || selected_index_ < 0 ||
        selected_index_ >= static_cast<int>(filtered_indices_.size())) {
        return -1;
    }
    return static_cast<int>(filtered_indices_[static_cast<size_t>(selected_index_)]);
}

void InventoryMenu::update_quest_tab(const tl::core::QuestLog* quests) {
    // The rows as `CQuestMenuXenon` last built them: when
    // the tab opened, or when `refreshQuestMenu` asked (`refresh_quest_tab`).
    if (quest_rows_stale_) {
        quest_rows_.clear();
        if (quests != nullptr) {
            for (const tl::core::QuestEntry* entry : quests->journal_entries()) {
                quest_rows_.push_back(entry->name());
            }
        }
        quest_rows_stale_ = false;
    }
    std::vector<const tl::core::QuestEntry*> log;
    if (quests != nullptr) {
        for (const std::string& name : quest_rows_) {
            if (const tl::core::QuestEntry* entry = quests->find(name)) log.push_back(entry);
        }
    }

    // Six trays, filled in the order the quests were taken. An empty one is
    // hidden outright rather than drawn blank: the original's `updateLayout`
    // destroys the windows it has no quest for.
    if (selected_quest_ >= static_cast<int>(log.size())) {
        selected_quest_ = log.empty() ? 0 : static_cast<int>(log.size()) - 1;
    }

    selected_quest_name_.clear();
    selected_quest_abandonable_ = false;

    // `moveframe`, the gold surround on the chosen row, is authored at the top of the list and
    // moved; the trays are 89 units apart (-10, 79, 168, 257, 346, 435).
    set_widget_position("quest/moveframe", -16.0f,
                        -26.0f + 89.0f * static_cast<float>(selected_quest_));
    set_widget_visible("quest/moveframe", !log.empty());

    for (int i = 0; i < tl::core::kQuestLogSize; ++i) {
        const std::string tray = "quest/ItemTray" + std::to_string(i);
        const std::string slot = "quest/SlotText" + std::to_string(i);
        const std::string done = "quest/questcomplete" + std::to_string(i);

        if (i >= static_cast<int>(log.size())) {
            set_widget_visible(tray, false);
            set_widget_visible(slot, false);
            set_widget_visible(done, false);
            continue;
        }

        const tl::core::QuestEntry& entry = *log[static_cast<size_t>(i)];
        set_widget_visible(tray, true);
        set_widget_visible(slot, true);
        set_widget_image(tray, i == selected_quest_ ? kTraySelected : kTrayIdle);

        // `DISPLAYNAME` carries its own `|c` run, taken off here (no per-run colour).
        const std::string title = tl::core::strip_markup(
            entry.display_name().empty() ? entry.name() : entry.display_name());
        set_widget_text(slot, wrap_ui_text(title, 18));

        // "Quest Complete" once `CQuest::isComplete` would say so, which for a
        // `QUESTCONTROLLERCOMPLETES` quest is never and for one that asks for
        // nothing is as soon as its offer has been acknowledged.
        set_widget_visible(done, entry.objectives_complete());

        if (i == selected_quest_) {
            selected_quest_name_ = entry.name();
            selected_quest_abandonable_ =
                entry.definition != nullptr && entry.definition->can_abandon;
        }
    }

    const tl::core::QuestEntry* chosen = selected_quest_ < static_cast<int>(log.size())
                                             ? log[static_cast<size_t>(selected_quest_)]
                                             : nullptr;

    // --- the description pane -------------------------------------------
    if (chosen == nullptr) {
        set_widget_text("quest/questtitle", "");
        set_widget_text("quest/questdetails", "");
    } else {
        set_widget_text("quest/questtitle", tl::core::strip_markup(chosen->display_name().empty()
                                                                       ? chosen->name()
                                                                       : chosen->display_name()));

        // `getQuestDetails()` is `[DETAILS]`'s line with its `[TAG]`s
        // substituted; the window authors `WORD WRAP`, so it is wrapped here
        // against its own measured width rather than at a character count.
        set_widget_text(
            "quest/questdetails",
            wrap_to_widget("quest/questdetails", tl::core::strip_markup(chosen->details())));
    }

    // --- the reward row --------------------------------------------------
    //
    // Each box shown only when its figure is non-zero, as `CQuestDialogMenu::setNPC` does either
    // side of `getQuestRewardGold()` and its two siblings.
    const int gold = chosen != nullptr ? chosen->reward.gold : 0;
    const int xp = chosen != nullptr ? chosen->reward.xp : 0;
    const int fame = chosen != nullptr ? chosen->reward.fame : 0;

    set_widget_visible("quest/RewardGoldBox", gold > 0);
    set_widget_visible("quest/RewardGold", gold > 0);
    set_widget_text("quest/RewardGold", "GP:" + std::to_string(gold));
    set_widget_visible("quest/RewardXPBox", xp > 0);
    set_widget_visible("quest/RewardXP", xp > 0);
    set_widget_text("quest/RewardXP", "XP:" + std::to_string(xp));
    set_widget_visible("quest/RewardFameBox", fame > 0);
    set_widget_visible("quest/RewardFame", fame > 0);
    set_widget_text("quest/RewardFame", "Fame:" + std::to_string(fame));

    // The three item slots, as the offer window's: only the first is filled (`[REWARD]` carries
    // one `TREASURECLASS`; the others are for `[UNITREWARD]`), and an empty slot is hidden.
    const std::string reward_icon = chosen != nullptr ? chosen->reward_icon : std::string();
    const bool show_item = !reward_icon.empty() && sprite(reward_icon) != nullptr;
    for (int i = 1; i <= 3; ++i) {
        const std::string n = std::to_string(i);
        const bool on = (i == 1) && show_item;
        set_widget_visible("quest/ItemBkg" + n, on);
        set_widget_visible("quest/ItemSlot" + n, on);
        set_widget_visible("quest/EmptySocket" + n + "_0", false);
        set_widget_visible("quest/EmptySocket" + n + "_1", false);
    }
    set_widget_visible("quest/ItemHighlight", false);
    if (show_item) set_widget_image("quest/ItemSlot1", reward_icon);

    // `Y` on the tray tray means Abandon Quest, and only a `CANABANDON` quest
    // can be given up - the layout's `buttonB` is a `y_button` with that
    // caption. It goes away when nothing selected can be abandoned.
    set_widget_visible("quest/buttonB", selected_quest_abandonable_);
    set_widget_visible("quest/buttonBtext", selected_quest_abandonable_);
    set_widget_visible("quest/buttontray", selected_quest_abandonable_);

    // The abandon confirmation is its own modal and the layout authors it
    // `VISIBLE:false`; nothing here opens it yet, so it stays down.
    set_widget_visible("quest/QuestAbandonConfirm", false);
}

void InventoryMenu::update(const tl::core::Inventory& inv,
                           const tl::core::CharacterProgression& character,
                           tl::core::CharacterEffects effects, const tl::core::CombatSheet& sheet,
                           const tl::core::StatBook& graphs, const tl::core::Pet& pet,
                           float play_time, const tl::core::QuestLog* quests) {
    using tl::core::CharacterProgression;

    if (!built_) return;

    // --- 1. Master Frame Header & Footer ---
    if (menu_mode_ == MenuMode::Quest) {
        // The Quest tab's neighbours.
        set_widget_text("master/MenuLeft", "Skills");
        set_widget_text("master/MenuName", "Quest");
        set_widget_text("master/MenuRight", "Inventory");

        set_widget_visible("master/PetCommands", false);
        set_widget_visible("master/ToggleItemCompare", false);
    } else if (menu_mode_ == MenuMode::Inventory) {
        set_widget_text("master/MenuLeft", "Quest");
        set_widget_text("master/MenuName", "Inventory");
        set_widget_text("master/MenuRight", "Stats");

        set_widget_visible("master/PetCommands", true);
        set_widget_visible("master/ToggleItemCompare", true);
        set_widget_text("master/itemcomparetext",
                        item_compare_ ? "disable item compare" : "enable item compare");
    } else if (menu_mode_ == MenuMode::Skills) {
        // The ring's neighbours either side, as the other three tabs name theirs.
        set_widget_text("master/MenuLeft", "Stats");
        set_widget_text("master/MenuName", "Skills");
        set_widget_text("master/MenuRight", "Quest");

        set_widget_visible("master/PetCommands", false);
        set_widget_visible("master/ToggleItemCompare", false);
    } else {  // Stats
        set_widget_text("master/MenuLeft", "Inventory");
        set_widget_text("master/MenuName", "Stats");
        set_widget_text("master/MenuRight", "Skills");

        set_widget_visible("master/PetCommands", false);
        set_widget_visible("master/ToggleItemCompare", false);
    }

    set_widget_visible("master/lefttrigger", true);
    set_widget_visible("master/lefttriggerbutton", true);
    set_widget_visible("master/righttrigger", true);
    set_widget_visible("master/righttriggerbutton", true);
    set_widget_visible("master/BToClose", true);

    // A vendor's screen is not one of the ring's tabs: its title stands alone and the triggers
    // that would leave it are gone.
    if (vendor_ != VendorScreen::None) {
        static const char* const kTitles[] = {"", "Merchant", "Stash", "Shared Stash", "Transmuter",
                                              "Enchanter", "Destroy Gems", "Recover Gems",
                                              "Retirement",
                                              // A shrine's page is page type 6, as an
                                              // enchanter's is (`vfunc_6`).
                                              "Enchanter"};
        set_widget_text("master/MenuLeft", "");
        set_widget_text("master/MenuName", kTitles[static_cast<int>(vendor_)]);
        set_widget_text("master/MenuRight", "");
        set_widget_visible("master/lefttrigger", false);
        set_widget_visible("master/lefttriggerbutton", false);
        set_widget_visible("master/righttrigger", false);
        set_widget_visible("master/righttriggerbutton", false);
        set_widget_visible("master/PetCommands", false);
    }

    // --- 2. Left Column: Character Stats Sheet (`char/`) ---
    set_widget_text("char/characterName", character.name());
    set_widget_text("char/CharacterTitle", character.title());
    set_widget_text("char/Level", std::to_string(character.level()));
    set_widget_text("char/XP", "XP: " + std::to_string(character.experience()) + "/" +
                                   std::to_string(character.experience_gate()));
    set_widget_text("char/FameLevel", std::to_string(character.fame_level()));
    set_widget_text("char/Fame", "Fame: " + std::to_string(character.fame()) + "/" +
                                     std::to_string(character.fame_gate()));
    set_widget_text("char/HP", std::to_string(static_cast<int>(character.hp())));
    set_widget_text("char/Mana", std::to_string(static_cast<int>(character.mp())));

    set_widget_text("char/strPts", std::to_string(character.strength(effects)));
    set_widget_text("char/dexPts", std::to_string(character.dexterity(effects)));
    set_widget_text("char/magicPts", std::to_string(character.magic(effects)));
    set_widget_text("char/defPts", std::to_string(character.defense(effects)));

    // Show/hide columns based on menu mode
    const bool show_inv = (menu_mode_ == MenuMode::Inventory);
    // the character's panel with the bag's
    // focus, the pet's with the pet tray's.
    const bool pet_page_shown =
        have_pet_page_ && show_inv && pet_tray_focused() && pet.alive() && pet.in_world();
    if (pet_page_shown) update_pet_page(pet);
    const bool show_stats = (menu_mode_ == MenuMode::Stats);
    // Off the Stats page the journal is due a fill the next time it opens.
    if (!show_stats || !visible_) journal_filled_ = false;
    const bool show_quest = (menu_mode_ == MenuMode::Quest);
    const bool show_skills = (menu_mode_ == MenuMode::Skills);

    // Set visibility of inv/ and pet/ nodes. The quest tab takes the whole
    // container, so the character sheet goes away with the rest.
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        if (n.prefix == "inv/" || n.prefix == "pet/") {
            n.runtime_hidden = !show_inv;
        } else if (n.prefix == "stats/" || n.prefix == "journal/") {
            n.runtime_hidden = !show_stats;
        } else if (n.prefix == "quest/") {
            n.runtime_hidden = !show_quest;
        } else if (n.prefix == "skills/" || n.prefix == "skillmap/") {
            // Only the tab's roots: the windows under them keep the visibility
            // the tab gives each one (`update_skills_tab`).
            if (n.parent == container_idx_) {
                n.runtime_hidden = !show_skills || (n.prefix == "skillmap/" && !skill_map_open_);
            }
        } else if (n.prefix == "char/") {
            n.runtime_hidden =
                show_quest || show_skills || vendor_ != VendorScreen::None || pet_page_shown;
        } else if (n.prefix == "petpage/") {
            n.runtime_hidden = !pet_page_shown;
        } else if (n.prefix == "vendor/") {
            n.runtime_hidden =
                !show_inv || vendor_ == VendorScreen::None || vendor_ == VendorScreen::Enchant ||
                vendor_ == VendorScreen::DestroyGems || vendor_ == VendorScreen::RecoverGems ||
                vendor_ == VendorScreen::Retire || vendor_ == VendorScreen::EnchantShrine;
        } else if (n.prefix == "housing/") {
            n.runtime_hidden = !select_open_ && !pet_commands_open_ && !spell_remove_open_;
        } else if (n.prefix == "select/") {
            n.runtime_hidden = !select_open_;
        } else if (n.prefix == "petcmd/") {
            n.runtime_hidden = !pet_commands_open_;
        } else if (n.prefix == "spellrm/") {
            n.runtime_hidden = !spell_remove_open_;
        } else if (n.prefix == "enchant/") {
            n.runtime_hidden =
                !show_inv || vendor_ == VendorScreen::None || vendor_ == VendorScreen::Merchant ||
                vendor_ == VendorScreen::Stash || vendor_ == VendorScreen::SharedStash ||
                vendor_ == VendorScreen::Combine;
        }
    }
    if (show_inv && vendor_ != VendorScreen::None) update_vendor_column(graphs);
    if (select_open_) update_item_select(inv, graphs);
    if (pet_commands_open_) update_pet_commands();
    if (spell_remove_open_) update_spell_remove();

    if (show_quest && !quest_tab_was_open_) quest_rows_stale_ = true;
    quest_tab_was_open_ = show_quest;
    if (show_quest) update_quest_tab(quests);

    // -------------------------------------------------------------
    // Mode: Inventory
    // -------------------------------------------------------------
    if (show_inv) {
        // The six tabs: `tl::core::inventory_filter_of`, the console's own
        // categoriser. Without the hierarchy nothing can be
        // sorted, and every item shows under the first tab.
        if (pending_select_slot_ >= 0 && inv.occupied(static_cast<size_t>(pending_select_slot_)) &&
            unit_types_ != nullptr) {
            current_tab_ = tl::core::inventory_filter_of(
                inv.slots()[static_cast<size_t>(pending_select_slot_)].item.unittype, *unit_types_);
        }
        filtered_indices_.clear();
        for (size_t i = 0; i < inv.slots().size(); ++i) {
            const auto& s = inv.slots()[i];
            if (s.count <= 0) continue;
            const int tab = unit_types_ != nullptr
                                ? tl::core::inventory_filter_of(s.item.unittype, *unit_types_)
                                : 0;
            if (tab == current_tab_) filtered_indices_.push_back(i);
        }
        if (pending_select_slot_ >= 0) {
            for (size_t row = 0; row < filtered_indices_.size(); ++row) {
                if (filtered_indices_[row] == static_cast<size_t>(pending_select_slot_)) {
                    selected_index_ = static_cast<int>(row);
                }
            }
            pending_select_slot_ = -1;
        }

        if (filtered_indices_.empty()) {
            selected_index_ = 0;
        } else {
            selected_index_ =
                std::clamp(selected_index_, 0, static_cast<int>(filtered_indices_.size()) - 1);
        }
        if (vendor_ == VendorScreen::Enchant || vendor_ == VendorScreen::EnchantShrine) {
            update_enchant_pane(inv, graphs);
        } else if (vendor_ == VendorScreen::DestroyGems || vendor_ == VendorScreen::RecoverGems) {
            update_gem_pane(inv);
        }

        const bool merchant_screen = vendor_ == VendorScreen::Merchant;
        const bool bag_focused = focus_ == Tray::Bag;
        set_widget_text("inv/characterName", "My Bag");
        set_widget_image("inv/TopFrame", bag_focused ? "backroundbase_inventory_rollover"
                                                     : "backroundbase_inventory");
        set_widget_image("inv/HiliteFrame", bag_focused ? "framea_rollover" : "framea");

        // On the bag's own screen the tray that lost the focus to the pet's shows its tabs dim
        // and no `..Hi` arrow (dims an unfocused tray).
        const int lit_tab = vendor_ == VendorScreen::None && !bag_focused ? -1 : current_tab_;
        set_widget_image("inv/inventory_filter_weapons",
                         lit_tab == 0 ? "tab_weapons_rollover" : "tab_weapons");
        set_widget_image("inv/inventory_filter_armor",
                         lit_tab == 1 ? "tab_armor_rollover" : "tab_armor");
        set_widget_image("inv/inventory_filter_jewelry",
                         lit_tab == 2 ? "tab_jewelry_rollover" : "tab_jewelry");
        set_widget_image("inv/inventory_filter_potions",
                         lit_tab == 3 ? "tab_potions_rollover" : "tab_potions");
        set_widget_image("inv/inventory_filter_spells",
                         lit_tab == 4 ? "tab_spells_rollover" : "tab_spells");
        set_widget_image("inv/inventory_filter_fish",
                         lit_tab == 5 ? "tab_fish_rollover" : "tab_fish");

        if (vendor_ == VendorScreen::None) {
            // the focused tray shows its `rightbumper` (RB, the swap), the other
            // neither. INVENTED: the count test gating the unfocused tray's `leftbumper` (`+0x60 <
            // +0xd0`) is not read.
            set_widget_visible("inv/leftbumper", false);
            set_widget_visible("inv/rightbumper", bag_focused && pet_tray_available_);
        } else {
            // On a vendor's screen the bumpers move between trays; the bag's
            // neighbour is the vendor's, to its left.
            set_widget_visible("inv/leftbumper", bag_focused);
            set_widget_visible("inv/rightbumper", false);
        }

        set_widget_visible("inv/weaponHi", lit_tab == 0);
        set_widget_visible("inv/armorHi", lit_tab == 1);
        set_widget_visible("inv/trinketHi", lit_tab == 2);
        set_widget_visible("inv/potionHi", lit_tab == 3);
        set_widget_visible("inv/scrollHi", lit_tab == 4);
        set_widget_visible("inv/fishHi", lit_tab == 5);

        set_widget_text("inv/filtername", tl::core::inventory_filter_name(current_tab_));

        // the player's tray counts against 50.
        char size_buf[32];
        std::snprintf(size_buf, sizeof(size_buf), "%zu/%d", inv.size(),
                      tl::core::tray_count_capacity(tl::core::TrayOwner::Player));
        set_widget_text("inv/inventorysize", size_buf);

        set_widget_text("inv/playergoldtext", std::to_string(inv.gold()));

        set_widget_visible("inv/buyandsell", false);
        set_widget_visible("inv/petintown", false);
        set_widget_visible("inv/transmutor", false);
        set_widget_visible("inv/IDENTIFYWIN", false);

        const int bag_scroll =
            tray_scroll("inv/", current_tab_, selected_index_, filtered_indices_.size());
        fill_tray_rows("inv/", inv, filtered_indices_, selected_index_, bag_scroll, false);
        place_tray_frame("inv/", selected_index_, bag_scroll, filtered_indices_.size(),
                         bag_focused);
        const bool have_selection = bag_focused && selected_index_ >= 0 &&
                                    selected_index_ < static_cast<int>(filtered_indices_.size());
        if (have_selection) {
            // What A does to the selected item, the function's ordinary
            // branch (`core::tray_a_caption`), the player as the owner. The
            // red of "Identify(0)" is not drawn.
            const size_t slot_idx = filtered_indices_[static_cast<size_t>(selected_index_)];
            const auto& slot = inv.slots()[slot_idx];
            const std::string own_a = tray_caption(slot.item, slot_idx, /*pet_tray=*/false);
            set_widget_text("inv/buttonAtext", own_a);

            // Y (Drop) and X (Swap into the pet's bag) both refuse an equip-location slot, so
            // neither is offered.
            const bool equipped = tl::core::Inventory::is_equip_location(slot_idx);
            // for the bag (its owner is the player).
            const bool pet_swap =
                tl::core::pet_takes_items(/*tray_owner_is_player=*/true, pet.alive(), pet.state());
            set_widget_text("inv/buttonYtext", equipped ? "" : "Drop");
            if (merchant_screen) {
                // With a merchant's tray on the screen X sells whatever is
                // selected, equipped or not;
                // a quest item is refused, and says nothing.
                const bool quest =
                    unit_types_ != nullptr &&
                    unit_types_->isa(slot.item.unittype, tl::core::unit_type::kQuestItem);
                set_tray_legend("inv/", own_a, quest ? "" : "Sell", equipped ? "" : "Drop");
                set_widget_visible("inv/buyandsell", true);
                set_widget_text("inv/buyandselltext", "Sell");
            } else if (vendor_ == VendorScreen::Combine) {
                // on a combiner's page (type 5): X "Swap"
                // and nothing on A or Y, whichever tray; A does nothing there,
                // and Y's "Transmute" is the `transmutor`
                // caption over the Transmuter's tray.
                set_tray_legend("inv/", "", "Swap", "");
            } else if (vendor_ == VendorScreen::Enchant || vendor_ == VendorScreen::EnchantShrine) {
                // on an enchanter's or a shrine's page (modes
                // 0 and 1): an unidentified item keeps the tray's
                // own A caption; one `canEnchant` takes "Enchant"
                // while `CEnchantMenuXenon +0xf4` allows (null on an
                // enchanter's page, the shrine's `+0x19c` on a shrine's); any
                // other gets no buttons. X and Y say nothing there.
                const bool enchantable = enchant_allowed_ && unit_types_ != nullptr &&
                                         tl::core::item_can_enchant(slot.item, *unit_types_);
                set_tray_legend("inv/",
                                !slot.item.identified ? own_a
                                : enchantable         ? "Enchant"
                                                      : "",
                                "", "");
            } else if (vendor_ == VendorScreen::RecoverGems ||
                       vendor_ == VendorScreen::DestroyGems) {
                // on the gem services' pages (mode 2,
                // mode 3): A "Recover Gems" or "Destroy
                // Gems" for an item holding a gem,
                // nothing otherwise; X and Y say nothing.
                const bool gems = tl::core::item_has_gems(slot.item);
                set_tray_legend("inv/",
                                !gems                                  ? ""
                                : vendor_ == VendorScreen::RecoverGems ? "Recover Gems"
                                                                       : "Destroy Gems",
                                "", "");
            } else if (vendor_ == VendorScreen::Stash || vendor_ == VendorScreen::SharedStash) {
                // the function's ordinary branch, which a stash page does
                // not change: X "Swap" when passes for the first
                // pet, Y "Drop". X moves into the stash whatever
                // the caption.
                set_tray_legend("inv/", own_a, pet_swap ? "Swap" : "", equipped ? "" : "Drop");
            } else {
                // the function's ordinary branch, as on a stash page: X "Swap" (into the pet's
                // bag) with a pet, Y "Drop", and no B. DIVERGENCE: the port's X and Y refuse an
                // equipped item, so neither is offered for one.
                set_tray_legend("inv/", own_a, (equipped || !pet_swap) ? "" : "Swap",
                                equipped ? "" : "Drop");
            }
        }

        // The pet bag: the same layout built a third time, so what the middle column hides is
        // hidden here too, plus `transmutor`. `petintown` is the panel shown while the animal is
        // away, with the `petreturntime2` clock.
        const bool pet_here = pet.alive() && pet.in_world();
        const std::string pet_name = pet.stats().display_name.empty()
                                         ? std::string("Pet Bag")
                                         : pet.stats().display_name + "'s Bag";
        set_widget_text("pet/characterName", pet.alive() ? pet_name : "Pet Bag");
        // the player's first pet's tray counts against 50.
        char pet_size[32];
        std::snprintf(pet_size, sizeof(pet_size), "%zu/%d", pet.bag().size(),
                      tl::core::tray_count_capacity(tl::core::TrayOwner::FirstPet));
        set_widget_text("pet/inventorysize", pet_size);
        set_widget_visible("pet/moveframe", false);
        set_widget_visible("pet/leftbumper", false);
        set_widget_visible("pet/rightbumper", false);
        set_widget_visible("pet/transmutor", false);
        set_widget_visible("pet/buyandsell", false);
        set_widget_visible("pet/IDENTIFYWIN", false);
        set_widget_visible("pet/playergoldwin", false);
        set_widget_visible("pet/uparrow", false);
        set_widget_visible("pet/downarrow", false);

        // "Your pet has gone to town." and the clock under it (`petreturntime2`, authored hidden).
        set_widget_visible("pet/petintown", pet.alive() && !pet_here);
        if (pet.alive() && !pet_here) {
            set_widget_visible("pet/petreturntime2", true);
            set_widget_text("pet/petreturntime2", pet.town_clock());
        } else {
            set_widget_visible("pet/petreturntime2", false);
        }

        // The pet's tray, as any tray: its six tabs, the one it is on
        // lit; the slots of the pet's bag `inventory_filter_of` sorts under that
        // tab; the selection, its frame and its legend while it has the focus
        // (swaps the art and the frame). A bag that has gone shopping
        // lists nothing.
        pet_tray_available_ = pet_here;
        if (!pet_tray_available_ && focus_ == Tray::Pet) focus_ = Tray::Bag;
        const bool pet_focused = focus_ == Tray::Pet;
        set_widget_text("pet/filtername", tl::core::inventory_filter_name(pet_tab_));
        set_widget_image("pet/TopFrame", pet_focused ? "backroundbase_inventory_rollover"
                                                     : "backroundbase_inventory");
        set_widget_image("pet/HiliteFrame", pet_focused ? "framea_rollover" : "framea");
        static const char* const kPetTabs[6][2] = {
            {"pet/inventory_filter_weapons", "tab_weapons"},
            {"pet/inventory_filter_armor", "tab_armor"},
            {"pet/inventory_filter_jewelry", "tab_jewelry"},
            {"pet/inventory_filter_potions", "tab_potions"},
            {"pet/inventory_filter_spells", "tab_spells"},
            {"pet/inventory_filter_fish", "tab_fish"},
        };
        for (int tab = 0; tab < 6; ++tab) {
            set_widget_image(kPetTabs[tab][0],
                             std::string(kPetTabs[tab][1]) +
                                 (pet_focused && pet_tab_ == tab ? "_rollover" : ""));
        }
        static const char* const kPetHi[6] = {"pet/weaponHi", "pet/armorHi",  "pet/trinketHi",
                                              "pet/potionHi", "pet/scrollHi", "pet/fishHi"};
        for (int tab = 0; tab < 6; ++tab) {
            set_widget_visible(kPetHi[tab], pet_focused && pet_tab_ == tab);
        }
        set_widget_visible("pet/rightbumper", pet_focused);

        pet_rows_.clear();
        if (pet_here) {
            const auto& pet_slots = pet.bag().slots();
            for (size_t i = 0; i < pet_slots.size(); ++i) {
                if (pet_slots[i].count <= 0) continue;
                const int tab =
                    unit_types_ != nullptr
                        ? tl::core::inventory_filter_of(pet_slots[i].item.unittype, *unit_types_)
                        : 0;
                if (tab == pet_tab_) pet_rows_.push_back(i);
            }
        }
        pet_selected_ = pet_rows_.empty()
                            ? 0
                            : std::clamp(pet_selected_, 0, static_cast<int>(pet_rows_.size()) - 1);
        const int pet_scroll = tray_scroll("pet/", pet_tab_, pet_selected_, pet_rows_.size());
        fill_tray_rows("pet/", pet.bag(), pet_rows_, pet_selected_, pet_scroll, false);
        place_tray_frame("pet/", pet_selected_, pet_scroll, pet_rows_.size(), pet_focused);
        if (pet_focused && !pet_rows_.empty()) {
            // the function's ordinary branch on a tray the player does not
            // own: A for the pet as the owner (`core::tray_a_caption`), X
            // "Swap" (passes for any owner not ISA 0x1c), Y "Drop".
            const size_t slot_idx = pet_rows_[static_cast<size_t>(pet_selected_)];
            const bool equipped = tl::core::Inventory::is_equip_location(slot_idx);
            const std::string pet_a =
                tray_caption(pet.bag().slots()[slot_idx].item, slot_idx, /*pet_tray=*/true);
            set_tray_legend("pet/", pet_a, equipped ? "" : "Swap", equipped ? "" : "Drop");
        }

        hide_journal_rows();
    }

    // -------------------------------------------------------------
    // Mode: Stats
    // -------------------------------------------------------------
    if (show_stats) {
        show_stats_view();

        // The journal is filled as the page opens (`CStatsMenuXenon::setOpen`),
        // not every frame: the time played stays what it was
        // when the page came up.
        if (visible_ && !journal_filled_) {
            fill_journal(character.journal(), play_time);
            journal_filled_ = true;
        }
    }

    layout();
    lay_out_tooltips();
}

}  // namespace tl::render
