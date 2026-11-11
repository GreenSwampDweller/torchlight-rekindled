#include "render/pc_stats.h"

#include <vector>

namespace tl::render {

namespace {

constexpr const char* kLayout = "media/UI/statsmenu.layout";

/// The four attributes in the stats page's order, and each one's windows.
constexpr const char* kAttribute[4] = {"Strength", "Dexterity", "Magic", "Defense"};
constexpr const char* kDamage[4] = {"Melee Damage", "Ranged Damage", "Magic Damage",
                                    "Damage Absorb"};
constexpr const char* kSpend[4] = {"SpendStrength", "SpendDexterity", "SpendMagic", "SpendDefense"};
constexpr const char* kReclaim[4] = {"ReclaimStrength", "ReclaimDexterity", "ReclaimMagic",
                                     "ReclaimDefense"};
constexpr const char* kHousing[4] = {"PointHousing1", "PointHousing2", "PointHousing3",
                                     "PointHousing4"};
/// The stats page lists poison, fire, ice and lightning (damage types 5, 2, 3, 4).
constexpr const char* kResist[4] = {"Poison Resist", "Fire Resist", "Ice Resist",
                                    "Lightning Resist"};

}  // namespace

bool PcStats::create(const std::string& group, unsigned width, unsigned height) {
    // The pane's background is its model; without it the texts still stand.
    pane_.create(group, "character", /*right=*/false, width, height, "tl/pcstats", 545);
    screen_.use_imagesets({"media/UI/hud.imageset", "media/UI/ui2.imageset",
                           "media/UI/uiicons.imageset", "media/UI/statsmenu_ui.imageset"});
    if (!screen_.create_pc(group, width, height, kLayout, "tl/pcstats", 546)) {
        pane_.destroy();
        return false;
    }
    // The layout's captions are its editor's placeholders; fame is not carried.
    for (const char* text : {"CharacterName", "CharacterTitle", "FameLevel", "Fame"}) {
        screen_.set_caption(text, std::string());
    }
    screen_.set_hidden("FameBarSub", true);
    screen_.refresh();
    return true;
}

void PcStats::destroy() {
    screen_.destroy();
    pane_.destroy();
}

void PcStats::set_visible(bool on) {
    if (!screen_.ready() || on == screen_.visible()) return;
    if (on) {
        pane_.show();
        screen_.show();
    } else {
        screen_.hide();
        pane_.hide();
    }
}

void PcStats::resize(unsigned width, unsigned height) {
    pane_.resize(width, height);
    screen_.resize(width, height);
}

// The sheet's windows, by their names in the layout.
void PcStats::set(const tl::core::StatsPageView& view, const Figures& figures) {
    if (!screen_.ready()) return;
    view_ = view;
    figures_ = figures;
    have_ = true;
    // INVENTED: the wording of the name, level, experience and pool captions (the PC's
    // format strings are not read).
    screen_.set_caption("CharacterName", figures.name);
    screen_.set_caption("Level", std::to_string(figures.level));
    screen_.set_caption("XP", figures.experience);
    screen_.set_caption("HP", figures.health);
    screen_.set_caption("Mana", figures.mana);
    screen_.set_caption("Stat Points", "Points Remaining: " + view.to_spend);
    // A plus while points are left to place, a minus on an attribute with points pending.
    const bool can_add = view.add_alpha >= 1.0f;
    for (int i = 0; i < 4; ++i) {
        screen_.set_caption(kAttribute[i], view.points[i]);
        screen_.set_caption(kDamage[i], view.damage[i]);
        screen_.set_caption(kResist[i], view.resistances[i]);
        screen_.set_hidden(kSpend[i], !can_add);
        screen_.set_hidden(kReclaim[i], !view.added_shown[i]);
        screen_.set_hidden(kHousing[i], !can_add && !view.added_shown[i]);
    }
    screen_.refresh();
    screen_.set_fill("ExperienceBarSub", figures.experience_fill, /*upward=*/false);
}

std::string PcStats::point(float x, float y) {
    std::string over = screen_.window_at(x, y);
    if (over == lit_) return over;
    if (!lit_.empty()) screen_.set_hover(lit_, false);
    if (!over.empty()) screen_.set_hover(over, true);
    lit_ = over;
    if (have_) set(view_, figures_);
    return over;
}

bool PcStats::blocks(float x, float y) const {
    return screen_.visible() && screen_.window_contains("Blocker", x, y);
}

}  // namespace tl::render
