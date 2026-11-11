#include "render/pc_hud.h"

#include "core/log.h"

#include <cstdio>
#include <vector>

namespace tl::render {

namespace {

constexpr const char* kLayout = "media/UI/bottomhud.layout";
constexpr const char* kPetLayout = "media/UI/pethud.layout";

/// `SkillSlot3`, `SkillText10`: a window of the numbered row.
std::string numbered(const char* stem, int number) {
    char name[32];
    std::snprintf(name, sizeof name, "%s%d", stem, number);
    return name;
}

}  // namespace

bool PcHud::create(const std::string& group, unsigned width, unsigned height) {
    // The PC's own sheets, named here so they load beside a console pak's too, and the
    // item icons the slots show.
    std::vector<std::string> sets = {"media/UI/hud.imageset", "media/UI/ui2.imageset",
                                     "media/UI/uiicons.imageset",
                                     "media/UI/itemicons/itemicons.imageset"};
    for (int sheet = 1; sheet <= 16; ++sheet) {
        sets.push_back("media/UI/itemicons/itemicons" + std::to_string(sheet) + ".imageset");
    }
    screen_.use_imagesets(sets);
    pet_.use_imagesets(std::move(sets));
    // Under the console's menus (the inventory at 600) and over the game.
    if (!screen_.create_pc(group, width, height, kLayout, "tl/pchud", 540)) return false;
    // The layout's captions are its editor's placeholders (`1`): blank until the game says.
    for (const char* text : {"TargetName", "TargetLevel", "TargetDescription", "LevelName",
                             "LargeMessageText", "SmallMessageText"}) {
        screen_.set_caption(text, std::string());
    }
    screen_.refresh();
    // The pet's panel is the same data's; without it the bar still stands.
    if (pet_.create_pc(group, width, height, kPetLayout, "tl/pcpet", 541)) {
        pet_.set_caption("PetName", std::string());
        pet_.refresh();
    }
    return true;
}

void PcHud::set_visible(bool on) {
    if (!screen_.ready()) return;
    if (on != screen_.visible()) {
        if (on) {
            screen_.show();
        } else {
            screen_.hide();
        }
    }
    // The pet's panel only while there is an animal in the level.
    const bool pet = on && pet_.ready() && have_state_ && shown_.pet;
    if (pet == pet_.visible()) return;
    if (pet) {
        pet_.show();
    } else {
        pet_.hide();
    }
}

void PcHud::set_extras(bool stat_points, bool skill_points, const std::string& right_skill_icon) {
    stat_points_ = stat_points;
    skill_points_ = skill_points;
    right_skill_icon_ = right_skill_icon;
}

// The HUD's windows, by their names in the layout.
void PcHud::set_state(const HudState& state) {
    if (!screen_.ready()) return;
    shown_ = state;
    have_state_ = true;
    screen_.set_caption("LevelName", state.level_name);
    screen_.set_caption("LargeMessageText", state.announce_alpha > 0.0f ? state.announce : "");
    screen_.set_caption("SmallMessageText",
                        state.sub_announce_alpha > 0.0f ? state.sub_announce : "");

    // The target's panel, up only while the pointer is over somebody.
    for (const char* window : {"TargetHealth", "TargetName", "TargetLevel", "TargetDescription"}) {
        screen_.set_hidden(window, !state.target);
    }
    if (state.target) {
        screen_.set_caption("TargetName", state.target_name);
        screen_.set_text_colour("TargetName", state.target_champion
                                                  ? Ogre::ColourValue(1.0f, 0.88f, 0.24f)
                                                  : Ogre::ColourValue::White);
        screen_.set_caption("TargetLevel", state.target_level > 0
                                               ? "Level " + std::to_string(state.target_level)
                                               : std::string());
        screen_.set_caption("TargetDescription", state.target_description);
    }

    // The ten slots: the picture, greyed while it cools or is run out, and the caption.
    for (int slot = 0; slot < tl::core::kSkillFunctionSlots; ++slot) {
        const tl::core::HotbarSlotView& view = state.slots[static_cast<size_t>(slot)];
        const std::string picture = numbered("SkillSlot", slot + 1);
        screen_.set_image(picture, view.icon);
        screen_.set_tint(
            picture, view.dimmed ? Ogre::ColourValue(0.6f, 0.6f, 0.6f) : Ogre::ColourValue::White);
        screen_.set_caption(numbered("SkillText", slot + 1), view.text);
    }

    screen_.set_hidden("StatsUp", !stat_points_);
    screen_.set_hidden("SkillUp", !skill_points_);
    screen_.set_image("PlayerSkillRight", right_skill_icon_);

    screen_.refresh();
    // The bars, on the windows as they were just laid out.
    screen_.set_fill("PlayerHealthBarSub", state.health, /*upward=*/true);
    screen_.set_fill("PlayerManaBarSub", state.mana, /*upward=*/true);
    screen_.set_fill("ExperienceBarSub", state.experience, /*upward=*/false);
    if (state.target) screen_.set_fill("TargetHealthBarSub", state.target_health, false);

    if (!pet_.ready()) return;
    pet_.set_hidden("FleeingText", !state.pet_fleeing);
    // The chosen stance is the one drawn selected.
    pet_.set_hover("PetAggressive", state.pet_stance == 0);
    pet_.set_hover("PetDefensive", state.pet_stance == 1);
    pet_.set_hover("PetPassive", state.pet_stance == 2);
    pet_.refresh();
    pet_.set_fill("PetHealth", state.pet_health, false);
    pet_.set_fill("PetMana", state.pet_mana, false);
}

// CGameUI::mouseOverPanel, the rectangles that always stand.
bool PcHud::blocks(float x, float y) const {
    if (!screen_.visible()) return false;
    if (screen_.window_contains("BottomHudBlocker", x, y)) return true;
    if (screen_.window_contains("BottomHudBlocker2", x, y)) return true;
    return pet_.visible() && screen_.window_contains("PetHudBlocker", x, y);
}

std::string PcHud::point(float x, float y) {
    // The pet's panel first: it lights nothing under the pointer (its stances are lit by
    // which is chosen).
    std::string pets = pet_.window_at(x, y);
    if (!pets.empty()) return pets;
    const std::string over = screen_.window_at(x, y);
    if (over == lit_) return over;
    if (!lit_.empty()) screen_.set_hover(lit_, false);
    if (!over.empty()) screen_.set_hover(over, true);
    lit_ = over;
    // A relayout resets the bars' cut: put the state back.
    if (have_state_) set_state(shown_);
    return over;
}

}  // namespace tl::render
