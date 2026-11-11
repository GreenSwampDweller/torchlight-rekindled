// The `[STAT]` watchers' view of a level character (`CAIManager`); split from script_units.cpp.
#include "core/script.h"

#include <cmath>
#include <string>
#include <vector>

namespace tl::core {

// What a `[STAT]` watcher of `CAIManager::initalize` reads of
// character `my_id`.
AIStatWatcherHooks LevelScript::stat_watcher_hooks(int64_t my_id) {
    AIStatWatcherHooks watcher_hooks;
    watcher_hooks.owner_id = [my_id]() { return my_id; };
    watcher_hooks.hp = [this, my_id]() -> int32_t {
        if (const Character* self = character(my_id)) {
            return static_cast<int32_t>(std::floor(self->combat.hp));
        }
        return 0;
    };
    watcher_hooks.max_hp = [this, my_id]() -> int32_t {
        if (const Character* self = character(my_id)) {
            return character_max_hp(*self);
        }
        return 1;
    };
    // INVENTED: monsters have no mana model in the port today.
    //
    watcher_hooks.mana = []() -> int32_t { return 0; };
    watcher_hooks.max_mana = []() -> int32_t { return 1; };
    watcher_hooks.alive = [this, my_id]() -> bool {
        if (const Character* self = character(my_id)) {
            return self->combat.hp > 0.0f && dead_.count(my_id) == 0;
        }
        return false;
    };
    watcher_hooks.has_level = [this]() -> bool { return !level_machines().empty(); };
    watcher_hooks.formation_units = [this, my_id]() -> std::vector<int64_t> {
        if (const Character* self = character(my_id)) {
            return self->ai_flags.formation_units();
        }
        return {};
    };
    watcher_hooks.find_flags_by_id = [this](int64_t id) -> AIFlags* {
        for (LevelScript* m : level_machines()) {
            if (Character* other = m->character(id)) {
                return &other->ai_flags;
            }
        }
        return nullptr;
    };
    watcher_hooks.active_units_count = [this, my_id](uint32_t unittype, uint32_t alignment,
                                                     float radius, bool include_living,
                                                     bool include_dead) -> float {
        if (const Character* self = character(my_id)) {
            std::vector<int64_t> out;
            get_active_characters_at_position(self->motion.position, unittype, alignment, radius,
                                              include_living, include_dead, out);
            return static_cast<float>(out.size());
        }
        return 0.0f;
    };
    // `getTargets`' area cases walk the level's character list (`level_machines()` in order)
    // with the distance and, for `AREAUNITTYPES`, `ISA` only.
    watcher_hooks.area_targets = [this, my_id](float radius,
                                               uint32_t unittype) -> std::vector<int64_t> {
        if (const Character* self = character(my_id)) {
            std::vector<int64_t> out;
            get_active_characters_at_position(self->motion.position, unittype, 3, radius, true,
                                              true, out);
            return out;
        }
        return {};
    };
    watcher_hooks.area_formation_targets = [this, my_id](float radius) -> std::vector<int64_t> {
        std::vector<int64_t> out;
        if (const Character* self = character(my_id)) {
            const Vec3& my_pos = self->motion.position;
            for (int64_t peer_id : self->ai_flags.formation_units()) {
                for (LevelScript* m : level_machines()) {
                    if (const Character* peer = m->character(peer_id)) {
                        const float dx = my_pos.x - peer->motion.position.x;
                        const float dy = my_pos.y - peer->motion.position.y;
                        const float dz = my_pos.z - peer->motion.position.z;
                        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) {
                            out.push_back(peer_id);
                        }
                        break;
                    }
                }
            }
        }
        return out;
    };
    // CAIManager::addAISkill
    watcher_hooks.add_ai_skill = [this, my_id](AISkill* skill, float duration) {
        if (Character* self = character(my_id)) self->ai_skills.add(skill, duration);
    };
    // CAIManager::hasAISkill
    watcher_hooks.has_ai_skill = [this, my_id](AISkill* skill) -> bool {
        if (Character* self = character(my_id)) return self->ai_skills.has(skill);
        return false;
    };
    // CAIManager::removeAISkill
    watcher_hooks.remove_ai_skill = [this, my_id](AISkill* skill) {
        if (Character* self = character(my_id)) self->ai_skills.remove(skill);
    };
    // INVENTED: CCharacter::setAIPlayAnimation
    // is not transcribed; animation hook
    // is empty.
    watcher_hooks.play_animation = [](int64_t, const std::string&) {};
    return watcher_hooks;
}

}  // namespace tl::core
