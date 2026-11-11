// The authored stat curves under `media/graphs/stats/` (`CGraph`, `CGraphManager`). A unit's
// `MAXHP`, `MINDAMAGE`, `ARMOR` are percentages of a curve indexed by level.
// No file I/O: the caller hands in the parsed `.adm` section.
#pragma once

#include "formats/adm.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tl::core {

/// Curve interpolation type matching EGRAPH_TYPES in the original binary.
enum class GraphType : uint32_t {
    Linear = 0,  ///< EGRAPH_TYPES 0 (IT_LINEAR in ParticleUniverse::DynamicAttributeCurved)
    Curved = 1,  ///< EGRAPH_TYPES 1 (IT_SPLINE in ParticleUniverse::DynamicAttributeCurved)
};

/// One authored curve (`CGraph`): one line, or two (MINY/MAXY).
/// Evaluated by `CGraph::getValue`.
class StatGraph {
public:
    struct Point {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Line {
        GraphType type = GraphType::Linear;
        std::vector<Point> points;
        std::vector<float> tangents;  // for Catmull-Rom spline (IT_SPLINE)
        float range = 0.0f;
    };

    // CGraph::CGraph
    StatGraph();
    explicit StatGraph(GraphType type, std::string name = "", uint32_t line_limit = 1);

    // No destructor declared: it would suppress the implicit
    // move, and every `std::move` of a graph would copy its lines.

    // CGraph::loadGraph / CGraph::loadGraphLine: a `[LINE]` record.
    static StatGraph from(const tl::formats::AdmSection& section);

    // CGraph::clear: empties one line's points; every other field survives.
    void clear(unsigned int line_idx = 0);

    // CGraph::addGraphLine
    void add_line(GraphType type);

    // CGraph::addValue: also updates max_x and last_y.
    void add_point(float x, float y, unsigned int line_idx = 0);

    // `CGraph::processPoints`
    void process_points();

    // CGraph::getControlPoints: the most points of any line.
    size_t size() const;
    size_t size(unsigned int line_idx) const;

    // `CGraph::getValue`: line 0 at x,
    // INFER_PASSED_END extrapolating past the largest X.
    float at(float x) const;

    bool empty() const;
    // PORT: an accessor of the port's own bookkeeping.
    const std::string& name() const { return name_; }
    GraphType type() const { return type_; }
    bool infer_past_end() const { return infer_past_end_; }
    void set_infer_past_end(bool infer) { infer_past_end_ = infer; }
    void set_slope(unsigned int line_idx, float slope) {
        (line_idx != 0 ? slope_1_ : slope_0_) = slope;
    }
    float slope(unsigned int line_idx = 0) const { return (line_idx != 0) ? slope_1_ : slope_0_; }
    float max_x() const { return max_x_; }
    float last_y(unsigned int line_idx = 0) const {
        return (line_idx != 0) ? last_y_1_ : last_y_0_;
    }
    size_t num_lines() const { return lines_.size(); }
    const std::vector<Point>& points(unsigned int line_idx = 0) const;
    const std::vector<Line>& lines() const { return lines_; }

private:
    std::string name_;
    GraphType type_ = GraphType::Linear;
    std::vector<Line> lines_;
    uint32_t line_limit_ = 1;
    bool dirty_ = false;
    bool infer_past_end_ = false;
    float slope_0_ = 0.0f;
    float slope_1_ = 0.0f;
    float last_y_0_ = 0.0f;
    float last_y_1_ = 0.0f;
    float max_x_ = 0.0f;
};

/// The loaded curves by name and by file name, case-blind (`CGraphManager`).
class StatBook {
public:
    // CGraphManager::CGraphManager
    StatBook() = default;

    // No destructor declared, so the book stays movable.

    // CGraphManager::loadGraph: keyed by uppercase name and uppercase path.
    void add(const std::string& name, StatGraph graph);
    void add_by_filename(const std::string& filename, StatGraph graph);

    // CGraphManager::getGraph: by name only.
    const StatGraph* find(const std::string& name) const;

    // CGraphManager::getGraphByFileName
    const StatGraph* find_by_filename(const std::string& filename) const;

    /// The named curve's value at `x`, or `fallback` when nothing loaded it.
    float value(const std::string& name, float x, float fallback = 0.0f) const;

    size_t size() const { return by_name_.size(); }
    const std::map<std::string, StatGraph>& all() const { return by_name_; }

    // CGraphManager::clear
    void clear();

    // CGraphManager reload re-read from disk; here it only clears.
    void reload();

    // The original's singleton is not kept: the book is passed explicitly.

private:
    std::map<std::string, StatGraph> by_name_;
    std::map<std::string, StatGraph> by_filename_;
};

/// The monster curves, which no `.DAT` names: the kind of monster picks them.
inline constexpr const char* kMonsterHealthGraph = "HEALTH_MONSTER_BYLEVEL";
inline constexpr const char* kMonsterDamageGraph = "DAMAGE_MONSTER";
inline constexpr const char* kMonsterArmorGraph = "ARMOR_MONSTER_BYLEVEL";
inline constexpr const char* kWeaponDamageGraph = "BASE_WEAPON_DAMAGE";

/// The curves `CCharacter::makeChampion` swaps a champion
/// onto, with difficulty variants (`difficulty_suffix()`).
inline constexpr const char* kChampionMonsterHealthGraph = "HEALTH_CHAMPIONMONSTER_BYLEVEL";
inline constexpr const char* kChampionMonsterDamageGraph = "DAMAGE_CHAMPIONMONSTER";
inline constexpr const char* kChampionMonsterArmorGraph = "ARMOR_CHAMPIONMONSTER_BYLEVEL";

/// The player-side curves, indexed by the player's level. `ARMOR_PLAYER_BYLEVEL_FORSET` is
/// indexed by the item's level (`CEquipment::setGraphAC`).
inline constexpr const char* kPlayerArmorGraph = "ARMOR_PLAYER_BYLEVEL_FORSET";
inline constexpr const char* kExperienceGateGraph = "EXPERIENCEGATE";
inline constexpr const char* kFameGateGraph = "FAMEGATE";
inline constexpr const char* kStatPointsPerLevelGraph = "STAT_POINTS_PER_LEVEL";
inline constexpr const char* kSkillPointsPerLevelGraph = "SKILL_POINTS_PER_LEVEL";
inline constexpr const char* kSkillPointsPerFameLevelGraph = "SKILL_POINTS_PER_FAME_LEVEL";
inline constexpr const char* kExperienceMonsterGraph = "EXPERIENCE_MONSTER";
inline constexpr const char* kExperienceChampionMonsterGraph = "EXPERIENCE_CHAMPIONMONSTER";
inline constexpr const char* kGoldDropGraph = "GOLDDROP";

/// What a quest pays in fame before its percentage: the champion curve, as
/// `CQuestRewards::calculateRewards` reads it (there is no quest fame curve).
inline constexpr const char* kFameChampionMonsterGraph = "FAME_CHAMPIONMONSTER";

/// The curve-name suffix a difficulty picks, as `CQuestRewards::calculateRewards` switches.
/// Out of range gives none: the original's fall-through arm looks up no curve at all.
inline const char* difficulty_suffix(int difficulty) {
    switch (difficulty) {
        case 0: return "_EASY";
        case 2: return "_HARD";
        case 3: return "_VERYHARD";
        default: return "";
    }
}

/// Sell-price curves by quality, indexed by the item's level; the item's `VALUE` is a
/// percentage of them (`CEquipment::recalculatePrice`).
inline constexpr const char* kSellPriceNormalGraph = "PRICE_PLAYERSELL_NORMAL";
inline constexpr const char* kSellPriceMagicGraph = "PRICE_PLAYERSELL_MAGIC";
inline constexpr const char* kSellPriceUniqueGraph = "PRICE_PLAYERSELL_UNIQUE";

/// The minion curves: the fallback for a pet whose `.DAT` names none.
inline constexpr const char* kMinionHealthGraph = "HEALTH_MINION_BYLEVEL";
inline constexpr const char* kMinionDamageGraph = "DAMAGE_MINION_BYLEVEL";

}  // namespace tl::core
