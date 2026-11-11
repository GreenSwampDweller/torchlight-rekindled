#include "core/level_memory.h"

#include <cmath>

namespace tl::core {

namespace {

// `CPlayer::updateStoredLevels`: a stored level's dungeon
// and depth.
bool is_level(const tl::formats::LevelSaveState& state, const std::string& dungeon, int32_t depth) {
    return state.depth == depth && state.dungeon == dungeon;
}

// `CPlayer::updateStoredLevels`: the quest test both its
// passes make for one state, a quest with details bound to its dungeon and depth.
bool quest_bound(const tl::formats::LevelSaveState& state,
                 const std::vector<QuestLevelClaim>& quests) {
    bool bound = false;
    for (const QuestLevelClaim& quest : quests) {
        if (!quest.has_details) continue;
        if (upper_ascii(quest.dungeon) == state.dungeon && quest.depth == state.depth) bound = true;
    }
    return bound;
}

}  // namespace

// PORT: ASCII case folding, the case-blind compare of names in the port's narrow strings.
std::string upper_ascii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// CPlayer::hasLevelSavedState
bool has_level_saved_state(const TravelRecord& travel, const std::string& dungeon, int32_t depth) {
    for (const tl::formats::LevelSaveState& state : travel.levels) {
        if (is_level(state, dungeon, depth)) return true;
    }
    return false;
}

// CPlayer::getLevelSavedState
tl::formats::LevelSaveState* get_level_saved_state(TravelRecord& travel, const std::string& dungeon,
                                                   int32_t depth) {
    for (tl::formats::LevelSaveState& state : travel.levels) {
        if (is_level(state, dungeon, depth)) return &state;
    }
    return nullptr;
}

// CPlayer::removeLevelSavedState
void remove_level_saved_state(TravelRecord& travel, uint32_t index) {
    if (travel.levels.empty()) return;
    if (static_cast<int32_t>(index) < static_cast<int32_t>(travel.levels.size())) {
        travel.levels[index] = std::move(travel.levels.back());
    }
    travel.levels.pop_back();
}

// CPlayer::hasDungeonHistory
bool has_dungeon_history(const TravelRecord& travel, const std::string& dungeon) {
    const std::string name = upper_ascii(dungeon);
    for (const tl::formats::LevelSaveState& state : travel.levels) {
        if (state.dungeon == name) return true;
    }
    return false;
}

// CPlayer::getMaxDepth
int32_t get_max_depth(const TravelRecord& travel, const std::string& dungeon) {
    const std::string name = upper_ascii(dungeon);
    for (const tl::formats::DungeonTrackerSave& tracker : travel.trackers) {
        if (tracker.name == name) return tracker.deepest;
    }
    return -1;
}

// CPlayer::getDungeonRank
int32_t get_dungeon_rank(const TravelRecord& travel, const std::string& dungeon) {
    // The name as given, case and all: unlike getMaxDepth, no StringUpper first.
    for (const tl::formats::DungeonTrackerSave& tracker : travel.trackers) {
        if (tracker.name == dungeon) return tracker.player_level;
    }
    return 0;
}

// CPlayer::clearDungeonHistory
void clear_dungeon_history(TravelRecord& travel, const std::string& dungeon) {
    const std::string name = upper_ascii(dungeon);
    for (tl::formats::LevelSaveState& state : travel.levels) {
        if (state.dungeon == name) state.time = -1.0f;
    }
}

// CPlayer::updateDungeonTracking
void update_dungeon_tracking(TravelRecord& travel, const std::string& dungeon, int32_t depth,
                             int32_t player_level, const int32_t* salt) {
    const std::string name = upper_ascii(dungeon);
    for (tl::formats::DungeonTrackerSave& tracker : travel.trackers) {
        if (tracker.name != name) continue;
        if (tracker.deepest <= depth) tracker.deepest = depth;
        if (salt == nullptr) return;
        if (tracker.salt != *salt) tracker.player_level = player_level;
        tracker.salt = *salt;
        return;
    }
    tl::formats::DungeonTrackerSave added;
    added.name = name;
    added.player_level = player_level;
    added.salt = salt != nullptr ? *salt : 0;
    added.deepest = depth;
    travel.trackers.push_back(std::move(added));
}

// CPlayer::storeLevelSavedState, up to the
// new state's time.
tl::formats::LevelSaveState& begin_store_level_saved_state(TravelRecord& travel,
                                                           const std::string& dungeon,
                                                           int32_t depth, bool is_town,
                                                           bool dont_store, int32_t player_level,
                                                           const int32_t* salt) {
    update_dungeon_tracking(travel, dungeon, depth, player_level, salt);
    if (travel.deepest_depth <= depth) travel.deepest_depth = depth;
    if (travel.deepest_depth_max < travel.deepest_depth) {
        travel.deepest_depth_max = travel.deepest_depth;
    }

    float kept_time = -1.0f;
    if (has_level_saved_state(travel, dungeon, depth)) {
        for (uint32_t i = 0; i < travel.levels.size(); ++i) {
            if (is_level(travel.levels[i], dungeon, depth)) {
                kept_time = travel.levels[i].time;
                remove_level_saved_state(travel, i);
                break;
            }
        }
    }

    tl::formats::LevelSaveState state;
    state.depth = depth;
    state.dungeon = dungeon;
    state.is_town = is_town ? 1 : 0;
    // a `DONTSTORE` template marks the state -1; otherwise a revisited
    // level keeps its first visit's time (`!= -1.0` and NaN both keep it).
    if (dont_store) {
        state.time = -1.0f;
    } else if (kept_time != -1.0f || std::isnan(kept_time)) {
        state.time = kept_time;
    } else {
        state.time = travel.play_time;
    }
    travel.levels.push_back(std::move(state));
    return travel.levels.back();
}

// CPlayer::updateStoredLevels
void update_stored_levels(TravelRecord& travel, const std::string& dungeon, int32_t depth,
                          const std::vector<QuestLevelClaim>& quests,
                          const LevelMemoryRules& rules) {
    auto is_last_dungeon = [&](const tl::formats::LevelSaveState& state) {
        return travel.last_dungeon_depth == state.depth && state.dungeon == travel.last_dungeon;
    };
    auto is_portal_level = [&](const tl::formats::LevelSaveState& state) {
        return travel.portal_depth == state.depth && state.dungeon == travel.portal_dungeon &&
               travel.portal_open;
    };

    // Over the limit: forget the oldest level nothing holds on to, once.
    if (rules.max_level_memory < static_cast<int32_t>(travel.levels.size())) {
        int32_t oldest = -1;
        float oldest_time = 1e6f;
        for (uint32_t i = 0; i < travel.levels.size(); ++i) {
            const tl::formats::LevelSaveState& state = travel.levels[i];
            if (state.holds_quest_item != 0 || quest_bound(state, quests) || state.is_town != 0) {
                continue;
            }
            if (is_level(state, dungeon, depth)) continue;
            if (is_last_dungeon(state)) continue;
            if (is_portal_level(state)) continue;
            if (state.time < oldest_time) {
                oldest = static_cast<int32_t>(i);
                oldest_time = state.time;
            }
        }
        if (oldest != -1) remove_level_saved_state(travel, static_cast<uint32_t>(oldest));
    }

    // Expiry.
    for (int32_t i = 0; i < static_cast<int32_t>(travel.levels.size()); ++i) {
        const tl::formats::LevelSaveState& state = travel.levels[static_cast<size_t>(i)];
        if (state.holds_quest_item != 0) continue;
        const bool last_with_time =
            is_last_dungeon(state) && (state.time != -1.0f || std::isnan(state.time));
        const bool fresh =
            travel.play_time <= static_cast<float>(rules.level_respawn_time) + state.time;
        if (fresh && (state.time != -1.0f || std::isnan(state.time))) continue;
        if (is_level(state, dungeon, depth)) continue;
        if (last_with_time) continue;
        if (is_portal_level(state)) continue;
        if (quest_bound(state, quests) || state.is_town != 0) continue;
        remove_level_saved_state(travel, static_cast<uint32_t>(i));
        --i;
    }
}

}  // namespace tl::core
