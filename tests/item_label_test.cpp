// A floor item's name and particle: the label pass,
// CEquipment::setItemTextHighlighted,
// CEquipment::createParticles.
#include "core/item_label.h"
#include "test_support.h"

#include <cmath>
#include <map>

namespace {

using tl::core::ItemLabelFacts;
using tl::core::ItemLabelQuery;
using tl::core::ItemLabelTint;

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

tl::formats::AdmRecord text(const char* key, const char* value) {
    tl::formats::AdmRecord record;
    record.key = key;
    record.type = tl::formats::AdmType::String;
    record.text = value;
    return record;
}

}  // namespace

/// Quest before set before unique before the magical pair; `MAGIC` picks the
/// rare colour, a random enchant the green one.
TL_TEST(item_label_tint_follows_the_consoles_order) {
    ItemLabelFacts all;
    all.quest = all.set = all.unique = all.magical = all.magic = true;
    TL_CHECK(tl::core::item_label_tint(all) == ItemLabelTint::Quest);
    all.quest = false;
    TL_CHECK(tl::core::item_label_tint(all) == ItemLabelTint::Set);
    all.set = false;
    TL_CHECK(tl::core::item_label_tint(all) == ItemLabelTint::Unique);
    all.unique = false;
    TL_CHECK(tl::core::item_label_tint(all) == ItemLabelTint::Rare);
    all.magic = false;
    TL_CHECK(tl::core::item_label_tint(all) == ItemLabelTint::RandomEnchant);
    all.magical = false;
    TL_CHECK(tl::core::item_label_tint(all) == ItemLabelTint::Plain);
    // `RANDOMMAGIC SOCKETABLE` (0xa0) counts as magical here even when
    // `isMagical` says no.
    ItemLabelFacts socketable;
    socketable.random_magic_socketable = true;
    TL_CHECK(tl::core::item_label_tint(socketable) == ItemLabelTint::RandomEnchant);
}

/// The ten keys of `GLOBALS.DAT`, as the shipped file authors the quest pair,
/// and the plain pair written in the code.
TL_TEST(item_label_colours_read_from_globals) {
    tl::formats::AdmFile file;
    file.root.name = "GLOBALS";
    file.root.records.push_back(text("QUEST_COLOR", "FFFFCC3D"));
    file.root.records.push_back(text("QUEST_COLOR_UNSELECTED", "FFDBAE12"));
    const tl::core::ItemLabelColours colours = tl::core::read_item_label_colours(file);

    const tl::core::LabelColour lit =
        tl::core::item_label_colour(ItemLabelTint::Quest, true, colours);
    TL_CHECK(near(lit.a, 1.0f) && near(lit.r, 1.0f) && near(lit.g, 0xcc / 255.0f) &&
             near(lit.b, 0x3d / 255.0f));
    const tl::core::LabelColour dim =
        tl::core::item_label_colour(ItemLabelTint::Quest, false, colours);
    TL_CHECK(near(dim.r, 0xdb / 255.0f) && near(dim.g, 0xae / 255.0f) &&
             near(dim.b, 0x12 / 255.0f));

    // A key the file does not author: opaque black.
    const tl::core::LabelColour missing =
        tl::core::item_label_colour(ItemLabelTint::Set, true, colours);
    TL_CHECK(near(missing.a, 1.0f) && near(missing.r, 0.0f));

    const tl::core::LabelColour white =
        tl::core::item_label_colour(ItemLabelTint::Plain, true, colours);
    TL_CHECK(near(white.r, 1.0f) && near(white.g, 1.0f) && near(white.b, 1.0f));
    const tl::core::LabelColour grey =
        tl::core::item_label_colour(ItemLabelTint::Plain, false, colours);
    TL_CHECK(near(grey.a, 1.0f) && near(grey.r, 0.8f) && near(grey.g, 0.8f) && near(grey.b, 0.8f));
}

/// Highlighted: always, unless a container. Otherwise near (squared distance
/// under 20) and not breakable, or the show-items key against the toggle.
TL_TEST(item_label_shows_near_or_highlighted) {
    TL_CHECK(tl::core::kItemLabelNearSquared == 20.0f);
    ItemLabelQuery far;
    far.distance_squared = 400.0f;
    TL_CHECK(!tl::core::item_label_wanted(far));
    far.highlighted = true;
    TL_CHECK(tl::core::item_label_wanted(far));
    far.container = true;
    TL_CHECK(!tl::core::item_label_wanted(far));

    ItemLabelQuery near_one;
    near_one.distance_squared = 19.99f;
    TL_CHECK(tl::core::item_label_wanted(near_one));
    near_one.distance_squared = 20.0f;
    TL_CHECK(!tl::core::item_label_wanted(near_one));
    near_one.distance_squared = 1.0f;
    near_one.breakable = true;
    TL_CHECK(!tl::core::item_label_wanted(near_one));

    // The PC's key and setting: either one alone shows every label; both
    // together cancel out.
    ItemLabelQuery toggled;
    toggled.distance_squared = 400.0f;
    toggled.toggle_dropped_items = 1;
    TL_CHECK(tl::core::item_label_wanted(toggled));
    toggled.show_items_held = true;
    TL_CHECK(!tl::core::item_label_wanted(toggled));
    toggled.toggle_dropped_items = 0;
    TL_CHECK(tl::core::item_label_wanted(toggled));
}

TL_TEST(item_label_text_drops_one_brace_span) {
    TL_CHECK_EQ(tl::core::item_label_text("Gleaming Ember"), std::string("Gleaming Ember"));
    TL_CHECK_EQ(tl::core::item_label_text("Axe {x} of Doom"), std::string("Axe  of Doom"));
    // Only the first span, and only with something inside it.
    TL_CHECK_EQ(tl::core::item_label_text("{a}{b}"), std::string("{b}"));
    TL_CHECK_EQ(tl::core::item_label_text("{}"), std::string("{}"));
    TL_CHECK_EQ(tl::core::item_label_text("no close {"), std::string("no close {"));
}

/// The label's size and placement: centred on the anchor, 26 inside the right and bottom,
/// never past the left or top.
TL_TEST(item_label_is_sized_and_kept_on_screen) {
    const tl::core::ItemLabelRect size = tl::core::item_label_size(60.0f, 15.0f, 1.0f);
    TL_CHECK(near(size.width, 64.0f) && near(size.height, 19.0f));
    const tl::core::ItemLabelRect scaled = tl::core::item_label_size(60.0f, 15.0f, 0.9375f);
    TL_CHECK(near(scaled.width, 64.0f - 2.25f) && near(scaled.height, 19.0f - 2.25f));

    const tl::core::ItemLabelRect mid =
        tl::core::place_item_label(size, 640.0f, 360.0f, 1280.0f, 720.0f);
    TL_CHECK(near(mid.x, 608.0f) && near(mid.y, 350.5f));
    const tl::core::ItemLabelRect right =
        tl::core::place_item_label(size, 1270.0f, 715.0f, 1280.0f, 720.0f);
    TL_CHECK(near(right.x, 1280.0f - 64.0f - 26.0f) && near(right.y, 720.0f - 19.0f - 26.0f));
    const tl::core::ItemLabelRect left =
        tl::core::place_item_label(size, 5.0f, 2.0f, 1280.0f, 720.0f);
    TL_CHECK(near(left.x, 0.0f) && near(left.y, 0.0f));
}

namespace {

// The HUD's frame: (-24, -9), grown by (48, 18).
constexpr tl::core::ItemLabelFrame kFrame{-24.0f, -9.0f, 48.0f, 18.0f};

bool frames_overlap(const tl::core::ItemLabelRect& a, const tl::core::ItemLabelRect& b) {
    const float a_left = a.x + kFrame.left, a_right = a_left + a.width + kFrame.wide;
    const float b_left = b.x + kFrame.left, b_right = b_left + b.width + kFrame.wide;
    const float a_top = a.y + kFrame.top, a_bottom = a_top + a.height + kFrame.tall;
    const float b_top = b.y + kFrame.top, b_bottom = b_top + b.height + kFrame.tall;
    return a_left < b_right && b_left < a_right && a_top < b_bottom && b_top < a_bottom;
}

}  // namespace

/// Stacked Labels: a label on another's spot goes just above it, the next just below the first
/// (the nearer free spot); the first keeps its own. Labels side by side are left alone.
TL_TEST(item_labels_stack_without_overlap) {
    const tl::core::ItemLabelRect size = tl::core::item_label_size(60.0f, 15.0f, 1.0f);
    const tl::core::ItemLabelRect spot =
        tl::core::place_item_label(size, 640.0f, 360.0f, 1280.0f, 720.0f);
    std::vector<tl::core::ItemLabelRect> pile{spot, spot, spot};
    tl::core::stack_item_labels(pile, kFrame, 2.0f, 720.0f);
    // A frame is 19 + 18 = 37 tall; 2 apart, a step is 39.
    TL_CHECK(near(pile[0].y, 350.5f));
    TL_CHECK(near(pile[1].y, 350.5f - 39.0f));
    TL_CHECK(near(pile[2].y, 350.5f + 39.0f));
    TL_CHECK(near(pile[1].x, spot.x) && near(pile[2].x, spot.x));

    tl::core::ItemLabelRect beside = spot;
    beside.x += 200.0f;
    std::vector<tl::core::ItemLabelRect> apart{spot, beside};
    tl::core::stack_item_labels(apart, kFrame, 2.0f, 720.0f);
    TL_CHECK(near(apart[0].y, spot.y) && near(apart[1].y, spot.y));
}

/// At the top of the screen there is no room above: the second label goes below.
TL_TEST(item_labels_stack_inside_the_screen) {
    const tl::core::ItemLabelRect size = tl::core::item_label_size(60.0f, 15.0f, 1.0f);
    const tl::core::ItemLabelRect top =
        tl::core::place_item_label(size, 640.0f, 2.0f, 1280.0f, 720.0f);
    std::vector<tl::core::ItemLabelRect> pile{top, top};
    tl::core::stack_item_labels(pile, kFrame, 2.0f, 720.0f);
    TL_CHECK(near(pile[0].y, 0.0f));
    TL_CHECK(near(pile[1].y, 39.0f));
}

/// A heap of loot - labels of several widths, anchors a few pixels apart - ends with no two
/// frames overlapping, every label still near its item.
TL_TEST(item_labels_stack_a_heap) {
    std::vector<tl::core::ItemLabelRect> heap;
    // 14: a column of 39-pixel steps around the middle of a 720-line screen holds 17.
    for (int i = 0; i < 14; ++i) {
        const tl::core::ItemLabelRect size =
            tl::core::item_label_size(40.0f + 13.0f * static_cast<float>(i % 5), 15.0f, 1.0f);
        heap.push_back(tl::core::place_item_label(size, 600.0f + 7.0f * static_cast<float>(i % 4),
                                                  360.0f + 5.0f * static_cast<float>(i % 3),
                                                  1280.0f, 720.0f));
    }
    const std::vector<tl::core::ItemLabelRect> before = heap;
    tl::core::stack_item_labels(heap, kFrame, 2.0f, 720.0f);
    for (size_t i = 0; i < heap.size(); ++i) {
        TL_CHECK(near(heap[i].x, before[i].x));
        TL_CHECK(std::fabs(heap[i].y - before[i].y) < 20.0f * 39.0f);
        for (size_t j = i + 1; j < heap.size(); ++j) TL_CHECK(!frames_overlap(heap[i], heap[j]));
    }
}

TL_TEST(drop_particle_by_kind) {
    TL_CHECK_EQ(tl::core::drop_particle_name("GLYPH_PARTICLE", true, true, true),
                std::string("GLYPH_PARTICLE"));
    TL_CHECK_EQ(tl::core::drop_particle_name("", true, true, true), std::string("QUEST_ITEM"));
    TL_CHECK_EQ(tl::core::drop_particle_name("", false, true, true), std::string("UNIQUE_WEAPON"));
    TL_CHECK_EQ(tl::core::drop_particle_name("", false, false, true), std::string("MAGIC_WEAPON"));
    TL_CHECK_EQ(tl::core::drop_particle_name("", false, false, false),
                std::string("GENERIC_WEAPON"));
}

/// Electricity is asked first and wins ties; a later element must be strictly
/// larger. No positive bonus, no particle.
TL_TEST(elemental_particle_by_largest_bonus) {
    std::map<int, int> bonus;
    auto of = [&](int type) {
        const auto found = bonus.find(type);
        return found == bonus.end() ? 0 : found->second;
    };
    TL_CHECK(tl::core::elemental_particle_name(false, of).empty());
    bonus[tl::core::kDamageElectric] = 5;
    bonus[tl::core::kDamageFire] = 5;
    TL_CHECK_EQ(tl::core::elemental_particle_name(false, of), std::string("WEAPON_ELECTRICITY"));
    bonus[tl::core::kDamageIce] = 6;
    TL_CHECK_EQ(tl::core::elemental_particle_name(true, of), std::string("WEAPON_ICE_PISTOL"));
    bonus[tl::core::kDamagePoison] = 7;
    TL_CHECK_EQ(tl::core::elemental_particle_name(false, of), std::string("WEAPON_POISON"));
    bonus.clear();
    bonus[tl::core::kDamageFire] = 1;
    TL_CHECK_EQ(tl::core::elemental_particle_name(false, of), std::string("WEAPON_FIRE"));
    // A negative electric bonus is not a start: fire still needs to be above 0.
    bonus[tl::core::kDamageElectric] = -3;
    TL_CHECK_EQ(tl::core::elemental_particle_name(false, of), std::string("WEAPON_FIRE"));
}
