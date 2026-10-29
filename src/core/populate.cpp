#include "core/populate.h"

#include "core/log.h"

#include <cmath>

namespace tl::core {

namespace {

/// `randomOpenPosition`'s reach, and the no-spawn test's half-width.
constexpr float kOpenRadius = 3.0f;
constexpr float kNoSpawnRadius = 1.0f;
/// The height every drawn point is given.
constexpr float kDrawHeight = 27.5f;
/// Tries at a place for one unit: the counter runs to 0x31.
constexpr unsigned kLastTry = 0x31;
/// Marker picks before the unit is placed in the section instead.
constexpr unsigned kMarkerPicks = 10;
/// Half the length of the lines a blocking item must leave passable.
constexpr float kClearance = 2.0f;
constexpr float kDegreesToRadians = 0.017453292f;

/// `populateFormations`' numbers: the height of a drawn point (`0x41f40000`),
/// the no-spawn test's half-width, the room between two
/// formations and the tries, which run the counter from 1 to 0x33.
constexpr float kFormationHeight = 30.5f;
constexpr float kFormationNoSpawn = 12.0f;
constexpr float kFormationSpacing = 20.0f;
constexpr int kFormationLastTry = 0x33;

/// The gold phase's.
constexpr float kGoldHeight = 7.5f;
constexpr float kGoldOpenRadius = 3.0f;
constexpr float kGoldNoSpawn = 0.5f;

// PORT: whether a point moved.
bool moved(const Vec3& from, const Vec3& to) {
    return to.x != from.x || to.y != from.y || to.z != from.z;
}

/// `TArrayList<AxisAlignedBox>`'s out-of-range read of element 0.
const PopulateSection& section_at(const std::vector<PopulateSection>& sections, uint32_t index) {
    return index < sections.size() ? sections[index] : sections.front();
}

}  // namespace

// `CLevel::populateSectionOfLevel`.
int populate_section_of_level(const PopulateSection& section, const std::string& spawn_class,
                              bool champion, bool use_markers, const PopulateWorld& world) {
    if (!world.roll || world.volatile_rolls == nullptr) return 0;
    const std::vector<std::string> units = world.roll(spawn_class);
    RunicRandom& rolls = *world.volatile_rolls;

    int characters = 0;
    int items = 0;
    for (const std::string& unit : units) {
        Vec3 at{0.0f, 0.0f, 0.0f};
        bool found = false;
        PopulatedFacing facing;
        // The counter the marker picks start is the one the section's tries
        // go on with.
        unsigned tries = 0;
        std::vector<SpawnMarker>* markers = world.markers;
        if (use_markers && markers != nullptr && !markers->empty()) {
            while (true) {
                const int32_t count = static_cast<int32_t>(markers->size());
                const uint32_t index = static_cast<uint32_t>(rolls.integer_between(1, count) - 1);
                // `TArrayList`'s out-of-range read of element 0.
                SpawnMarker& marker =
                    index < markers->size() ? (*markers)[index] : markers->front();
                if (marker.taken == 0) {
                    at = marker.position;
                    ++marker.taken;
                    facing.from_marker = true;
                    facing.orientation = marker.orientation;
                    found = true;
                    break;
                }
                if (++tries == kMarkerPicks) break;
            }
        }
        while (!found) {
            const float z = rolls.between(section.first.z, section.second.z);
            const float x = rolls.between(section.first.x, section.second.x);
            const Vec3 drawn{x, kDrawHeight, z};
            const Vec3 open =
                world.random_open_position ? world.random_open_position(drawn, kOpenRadius) : drawn;
            at = open;
            // Unchanged in every component: nowhere open within reach.
            if (moved(drawn, open) &&
                !(world.in_no_spawn_region && world.in_no_spawn_region(open, kNoSpawnRadius))) {
                found = true;
                break;
            }
            if (kLastTry < ++tries) break;
        }
        if (!found) continue;

        // `CResourceManager::createUnit`, then the turn: a marker's own, or
        // `MATH::rotateY((0, 0, 1), degrees * pi / 180)` drawn whatever was made.
        const PopulatedUnit made = world.create ? world.create(unit) : PopulatedUnit{};
        if (!facing.from_marker) facing.yaw = rolls.between(0.0f, 360.0f) * kDegreesToRadians;
        if (made.kind == PopulatedKind::Character) {
            if (world.add_character && world.add_character(made, unit, at, facing, champion)) {
                ++characters;
            }
            continue;
        }
        if (made.kind != PopulatedKind::Item) continue;
        if (made.blocks) {
            const auto clear = [&world](const Vec3& from, const Vec3& to) {
                return !world.passable_between || world.passable_between(from, to);
            };
            const bool open =
                clear({at.x - kClearance, at.y, at.z}, {at.x + kClearance, at.y, at.z}) &&
                clear({at.x, at.y, at.z - kClearance}, {at.x, at.y, at.z + kClearance}) &&
                clear({at.x + kClearance, at.y, at.z - kClearance},
                      {at.x - kClearance, at.y, at.z + kClearance}) &&
                clear({at.x - kClearance, at.y, at.z - kClearance},
                      {at.x + kClearance, at.y, at.z + kClearance});
            // Made and thrown away: the place stays empty and nothing is counted.
            if (!open) {
                if (world.discard) world.discard(made);
                continue;
            }
        }
        if (world.add_item && world.add_item(made, unit, at, facing)) ++items;
    }
    return characters + items;
}

// The unit phase loops of `CLevel::populate`:
// monsters, champions, NPCs, creeps,
// props.
int populate_phase(const std::vector<PopulateSection>& sections, Randomizer& sections_pick,
                   RunicRandom& g_rand, const std::string& spawn_class, uint32_t wanted,
                   bool champion, bool use_markers, bool empty_counts, const PopulateWorld& world) {
    if (wanted == 0 || sections.empty()) return 0;
    int placed = 0;
    uint32_t counted = 0;
    int empty_run = 0;
    do {
        const uint32_t index = static_cast<uint32_t>(sections_pick.get_random(g_rand));
        TL_LOG_DEBUG("  populate: section %u of %zu", index, sections.size());
        int made = populate_section_of_level(section_at(sections, index), spawn_class, champion,
                                             use_markers, world);
        placed += made;
        if (empty_counts) {
            if (made < 1) made = 1;
        } else if (made < 1 && 10 < ++empty_run) {
            made = 1;
            empty_run = 0;
        }
        counted += static_cast<uint32_t>(made);
    } while (counted < wanted);
    return placed;
}

// `CLevel::populateFormations`.
uint32_t populate_formations(const std::vector<PopulateSection>& sections,
                             Randomizer& sections_pick, const std::vector<FormationChoice>& choices,
                             const std::function<uint32_t()>& wanted, std::vector<Vec3>& existing,
                             RunicRandom& g_rand, const FormationWorld& world) {
    if (world.volatile_rolls == nullptr) return 0;
    RunicRandom& rolls = *world.volatile_rolls;
    Randomizer files(RandomizerType::kNormal);
    for (size_t i = 0; i < choices.size(); ++i) {
        // An empty `FILE` leaves at once (0x95de... ->) with 0.
        if (choices[i].file.empty()) return 0;
        files.add_choice(static_cast<int32_t>(i), 0 < choices[i].weight ? choices[i].weight : 1);
    }
    const uint32_t count = wanted ? wanted() : 0;
    if (count == 0) return 0;
    for (uint32_t made = 0; made < count; ++made) {
        const PopulateSection& box =
            sections.empty()
                ? PopulateSection{}
                : section_at(sections, static_cast<uint32_t>(sections_pick.get_random(g_rand)));
        const uint32_t pick = static_cast<uint32_t>(files.get_random(g_rand));
        static const std::string kNoFile;
        const std::string& file = choices.empty()         ? kNoFile
                                  : pick < choices.size() ? choices[pick].file
                                                          : choices.front().file;
        // A first point, drawn (z, then x) and never used.
        rolls.between(box.first.z, box.second.z);
        rolls.between(box.first.x, box.second.x);
        for (int tries = 1;; ++tries) {
            const float z = rolls.between(box.first.z, box.second.z);
            const float x = rolls.between(box.first.x, box.second.x);
            const Vec3 drawn{x, kFormationHeight, z};
            const Vec3 open =
                world.random_open_position ? world.random_open_position(drawn, kOpenRadius) : drawn;
            bool fits = moved(drawn, open) && !(world.in_no_spawn_region &&
                                                world.in_no_spawn_region(open, kFormationNoSpawn));
            if (fits) {
                for (const Vec3& other : existing) {
                    const float dx = other.x - open.x;
                    const float dy = other.y - open.y;
                    const float dz = other.z - open.z;
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) < kFormationSpacing) {
                        fits = false;
                        break;
                    }
                }
            }
            if (fits) {
                if (world.add_formation) world.add_formation(file, open);
                existing.push_back(open);
                break;
            }
            if (tries + 1 == kFormationLastTry) break;
        }
    }
    return count;
}

// `CLevel::populate`, the gold phase.
int populate_gold(const std::vector<PopulateSection>& sections, Randomizer& sections_pick,
                  uint32_t wanted, RunicRandom& g_rand, const GoldWorld& world) {
    if (world.volatile_rolls == nullptr || sections.empty()) return 0;
    RunicRandom& rolls = *world.volatile_rolls;
    int piles = 0;
    for (uint32_t i = 0; i < wanted; ++i) {
        const PopulateSection& box =
            section_at(sections, static_cast<uint32_t>(sections_pick.get_random(g_rand)));
        const float z = rolls.between(box.first.z, box.second.z);
        const float x = rolls.between(box.first.x, box.second.x);
        const Vec3 drawn{x, kGoldHeight, z};
        const Vec3 open = world.random_open_item_position
                              ? world.random_open_item_position(drawn, kGoldOpenRadius)
                              : drawn;
        if (!moved(drawn, open)) continue;
        if (world.in_no_spawn_region && world.in_no_spawn_region(open, kGoldNoSpawn)) continue;
        const int32_t amount = rolls.integer_between(2, 0xc);
        if (world.add_gold) world.add_gold(amount, open);
        ++piles;
    }
    return piles;
}
// `CLevel::populate`'s room union: a new corner below the
// first (`<=`) or above the second keeps the old one only when it is not past it.
void LevelRoomBox::add(const PopulateSection& section) {
    if (state == 0) {
        state = 1;
        low = section.first;
        high = section.second;
        return;
    }
    low = {low.x <= section.first.x ? low.x : section.first.x,
           low.y <= section.first.y ? low.y : section.first.y,
           low.z <= section.first.z ? low.z : section.first.z};
    high = {section.second.x <= high.x ? high.x : section.second.x,
            section.second.y <= high.y ? high.y : section.second.y,
            section.second.z <= high.z ? high.z : section.second.z};
}

// `CLevel::getRoomIndexThatPositionIsIn`.
int32_t room_index_that_position_is_in(const std::vector<LevelRoomBox>& rooms,
                                       const Vec3& position) {
    for (size_t i = 0; i < rooms.size(); ++i) {
        const LevelRoomBox& r = rooms[i];
        if (r.state == 0) continue;
        if (r.state == 2 ||
            (r.low.x <= position.x && position.x <= r.high.x && r.low.y <= position.y &&
             position.y <= r.high.y && r.low.z <= position.z && position.z <= r.high.z)) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// `CLevel::getItemByOriginalGuid`.
int find_item_by_original_guid(const std::vector<OriginalGuidUnit>& items,
                               const std::vector<LevelRoomBox>& rooms, int64_t guid, int32_t room,
                               int64_t hierarchy) {
    int last = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        const OriginalGuidUnit& item = items[i];
        if (item.guid != guid) continue;
        if (room != -1 && room != room_index_that_position_is_in(rooms, item.position)) continue;
        if (hierarchy == 0 || item.hierarchy == hierarchy) return static_cast<int>(i);
        last = static_cast<int>(i);
    }
    return last;
}

}  // namespace tl::core
