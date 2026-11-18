#include "core/item_label.h"

#include "formats/cegui.h"

#include <cmath>
#include <cstdio>

namespace tl::core {

ItemLabelTint item_label_tint(const ItemLabelFacts& facts) {
    // `CEquipment::setItemTextHighlighted`. The base
    // `CItem` version is the same without the set test,
    // which a caller expresses by leaving `set` false.
    if (facts.quest) return ItemLabelTint::Quest;
    if (facts.set) return ItemLabelTint::Set;
    if (facts.unique) return ItemLabelTint::Unique;
    if (facts.magical || facts.random_magic_socketable) {
        return facts.magic ? ItemLabelTint::Rare : ItemLabelTint::RandomEnchant;
    }
    return ItemLabelTint::Plain;
}

namespace {

LabelColour colour_key(const tl::formats::AdmFile& globals, const char* key) {
    // An empty string - a key `reload` did not find - is what
    // `CEGUI::PropertyHelper::stringToColour` leaves at its 0xFF000000.
    LabelColour out{1.0f, 0.0f, 0.0f, 0.0f};
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    if (tl::formats::parse_colour(globals.string_or(key, ""), r, g, b, a)) {
        out = LabelColour{a, r, g, b};
    }
    return out;
}

}  // namespace

ItemLabelColours read_item_label_colours(const tl::formats::AdmFile& globals) {
    // `CGameGlobals::reload`, in its order.
    ItemLabelColours out;
    out.random_enchant = colour_key(globals, "RANDOMENCHANT_COLOR");
    out.rare = colour_key(globals, "RARE_COLOR");
    out.unique = colour_key(globals, "UNIQUE_COLOR");
    out.set = colour_key(globals, "SET_COLOR");
    out.quest = colour_key(globals, "QUEST_COLOR");
    out.random_enchant_unselected = colour_key(globals, "RANDOMENCHANT_COLOR_UNSELECTED");
    out.rare_unselected = colour_key(globals, "RARE_COLOR_UNSELECTED");
    out.unique_unselected = colour_key(globals, "UNIQUE_COLOR_UNSELECTED");
    out.set_unselected = colour_key(globals, "SET_COLOR_UNSELECTED");
    out.quest_unselected = colour_key(globals, "QUEST_COLOR_UNSELECTED");
    out.socket = colour_key(globals, "SOCKET_COLOR");
    return out;
}

// PORT: a colour as the eight hex digits of a `|c` markup run, `AARRGGBB`.
std::string label_colour_markup(const LabelColour& colour) {
    const auto byte = [](float v) {
        const int scaled = static_cast<int>(v * 255.0f + 0.5f);
        return scaled < 0 ? 0 : scaled > 255 ? 255 : scaled;
    };
    char text[16];
    std::snprintf(text, sizeof(text), "%02X%02X%02X%02X", byte(colour.a), byte(colour.r),
                  byte(colour.g), byte(colour.b));
    return text;
}

LabelColour item_label_colour(ItemLabelTint tint, bool highlighted,
                              const ItemLabelColours& colours) {
    // `setItemTextHighlighted`: the selected colours when highlighted, the unselected when not.
    switch (tint) {
        case ItemLabelTint::Quest: return highlighted ? colours.quest : colours.quest_unselected;
        case ItemLabelTint::Set: return highlighted ? colours.set : colours.set_unselected;
        case ItemLabelTint::Unique: return highlighted ? colours.unique : colours.unique_unselected;
        case ItemLabelTint::Rare: return highlighted ? colours.rare : colours.rare_unselected;
        case ItemLabelTint::RandomEnchant:
            return highlighted ? colours.random_enchant : colours.random_enchant_unselected;
        case ItemLabelTint::Plain: break;
    }
    // `CEGUI::colour` is alpha, red, green, blue: 1.0 then 0.8 x 3 (0x3f4ccccd).
    if (highlighted) return LabelColour{1.0f, 1.0f, 1.0f, 1.0f};
    return LabelColour{1.0f, 0.8f, 0.8f, 0.8f};
}

bool item_label_wanted(const ItemLabelQuery& query) {
    // the level's highlighted item has its name up unless it
    // is a container, which falls through to the rules for everything else.
    if (query.highlighted && !query.container) return true;
    const bool held = query.show_items_held;
    if (held != (query.toggle_dropped_items == 1)) return true;
    return query.distance_squared < kItemLabelNearSquared && !query.breakable;
}

// `getItemName()` with its first `{...}` span removed, as the label is built.
//
std::string item_label_text(const std::string& name) {
    std::string text = name;
    const size_t open = text.find('{');
    if (open == std::string::npos) return text;
    const size_t close = text.find('}', open);
    if (close == std::string::npos || !(open + 1 < close)) return text;
    text.erase(open, close - open + 1);
    return text;
}

// The label window's size: the text's extent and the line
// height, each padded, scaled by the setting.
ItemLabelRect item_label_size(float text_width, float line_height, float ratio) {
    // The two `UDim` offsets: `(ratio x 36 - 36) + extent + 4` across and
    // `(ratio x 36 - 36) + line height + 4` down.
    const float scaled = ratio * kItemLabelScaleBase - kItemLabelScaleBase;
    ItemLabelRect out;
    out.width = scaled + (text_width + kItemLabelPadding);
    out.height = scaled + line_height + kItemLabelPadding;
    return out;
}

// Where a label goes: centred on its anchor,
// then kept inside the screen's edges.
ItemLabelRect place_item_label(const ItemLabelRect& size, float at_x, float at_y,
                               float screen_width, float screen_height) {
    ItemLabelRect out = size;
    float x = -(size.width * 0.5f - at_x);
    float y = -(size.height * 0.5f - at_y);
    if (screen_width < (x + size.width) + kItemLabelEdge) {
        x = screen_width - (size.width + kItemLabelEdge);
    }
    if (screen_height < (y + size.height) + kItemLabelEdge) {
        y = screen_height - (size.height + kItemLabelEdge);
    }
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    out.x = x;
    out.y = y;
    return out;
}

void stack_item_labels(std::vector<ItemLabelRect>& rects, const ItemLabelFrame& frame, float gap,
                       float screen_height) {
    // Each label's frame as drawn: [left, right) x [top, bottom).
    struct Box {
        float left, right, top, bottom;
    };
    const auto box_of = [&](const ItemLabelRect& r, float y) {
        const float top = y + frame.top;
        return Box{r.x + frame.left, r.x + frame.left + r.width + frame.wide, top,
                   top + r.height + frame.tall};
    };
    const auto overlaps = [](const Box& a, const Box& b) {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    };
    std::vector<Box> placed;
    placed.reserve(rects.size());
    for (ItemLabelRect& rect : rects) {
        const auto free_at = [&](float y) {
            if (y < 0.0f || screen_height < y + rect.height + kItemLabelEdge) return false;
            const Box box = box_of(rect, y);
            for (const Box& other : placed) {
                if (overlaps(box, other)) return false;
            }
            return true;
        };
        float best = rect.y;
        if (!free_at(rect.y)) {
            // The spots worth trying: just above or just below a frame already placed. The
            // nearest free one wins; on a tie, the one above (labels climb, as items pile up).
            const float tall = rect.height + frame.tall;
            bool found = false;
            float best_distance = 0.0f;
            for (const Box& other : placed) {
                for (const float frame_top : {other.top - gap - tall, other.bottom + gap}) {
                    const float y = frame_top - frame.top;
                    if (!free_at(y)) continue;
                    const float distance = std::fabs(y - rect.y);
                    if (!found || distance < best_distance ||
                        (distance == best_distance && y < best)) {
                        best = y;
                        best_distance = distance;
                        found = true;
                    }
                }
            }
        }
        rect.y = best;
        placed.push_back(box_of(rect, rect.y));
    }
}

std::string drop_particle_name(const std::string& authored, bool quest, bool unique, bool magical) {
    // `createParticles`: `DROPPARTICLE` as `unitInit`
    // read it wins; the four fall-backs are.
    if (!authored.empty()) return authored;
    if (quest) return "QUEST_ITEM";
    if (unique) return "UNIQUE_WEAPON";
    if (magical) return "MAGIC_WEAPON";
    return "GENERIC_WEAPON";
}

std::string elemental_particle_for(int element, bool pistol) {
    // 2 fire, 3 ice, 4 electricity, 5 poison.
    switch (element) {
        case kDamageFire: return pistol ? "WEAPON_FIRE_PISTOL" : "WEAPON_FIRE";
        case kDamageIce: return pistol ? "WEAPON_ICE_PISTOL" : "WEAPON_ICE";
        case kDamageElectric: return pistol ? "WEAPON_ELECTRICITY_PISTOL" : "WEAPON_ELECTRICITY";
        case kDamagePoison: return pistol ? "WEAPON_POISON_PISTOL" : "WEAPON_POISON";
        default: return std::string();
    }
}

}  // namespace tl::core
