#include "core/pet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tl::core {

// PORT: the pet's AI state as the port's HUD and menus group them.
PetState pet_state_of_ai(int32_t ai_state) {
    switch (ai_state) {
        case 3:    // attack
        case 4:    // hunt
        case 0xd:  // skillAI
        case 0xe:  // performingSkillAI
        case 0xf:  // skillApproachAI
            return PetState::Attacking;
        case 10: return PetState::Fleeing;
        case 0x29: return PetState::Departing;
        case 0x2a: return PetState::InTown;
        default: return PetState::Following;
    }
}

// PORT: the pet record started for a unit placed in the level.
void Pet::spawn(const std::string& unit, const UnitStats& stats, const Vec3& at, float facing) {
    unit_ = unit;
    stats_ = stats;
    mirror_ = PetMirror{};
    mirror_.position = at;
    mirror_.facing = facing;
}

// PORT: the pet record emptied (a new game, or no pet).
void Pet::clear() {
    unit_.clear();
    stats_ = UnitStats{};
    stance_ = kDefaultStance;
    mirror_ = PetMirror{};
    bag_.clear();
    effects_ = std::make_shared<EffectManager>();
    skills_ = SkillBook{};
    level_ = 1;
    experience_ = 0;
}

// INVENTED: the stance after this one, aggressive -> defensive -> passive; the
// console's own order is not read for it.
void Pet::cycle_stance() {
    switch (stance_) {
        case PetStance::Aggressive: stance_ = PetStance::Defensive; break;
        case PetStance::Defensive: stance_ = PetStance::Passive; break;
        case PetStance::Passive: stance_ = PetStance::Aggressive; break;
    }
}

// PORT: the time left in town as `h:mm:ss`, for the HUD.
std::string Pet::town_clock() const {
    const int total = static_cast<int>(std::ceil(std::max(0.0f, mirror_.town_timer)));
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", total / 3600, (total / 60) % 60,
                  total % 60);
    return buffer;
}

}  // namespace tl::core
