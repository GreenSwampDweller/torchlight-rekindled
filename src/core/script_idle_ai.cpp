// The ranged and circling idle AIs of `CMonster` (core/script.h).
#include "core/random.h"
#include "core/script.h"

namespace tl::core {

// `CMonster::idleAIRangedDefender(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_ranged_defender(Object& o, Character& c, QuestState& quests,
                                          std::vector<ScriptEvent>& out) {
    c.run = false;
    bool gated = false;
    UnitRef found;
    if (!idle_ai_sight(o, c, true, gated, found)) {
        // No one to act on: a defensive skill made ready, if none is.
        if (!gated && c.casting.active < 0) select_defensive_skill(c);
        return false;
    }
    set_target(o, c, true, found, out);
    if (idle_ai_open_attack(o, c, found, quests, out)) return true;
    select_defensive_skill(c);
    // `canCastCurrentSkill(null, false, false)`: of the target, and
    // `castSkill(-1)`.
    if (active_unit_skill(c) != nullptr && can_cast_current_skill(o, c, false, false, out)) {
        cast_skill(o, c, quests, out);
        return true;
    }
    if (9 < volatile_random().integer_between(0, 100)) return false;
    set_ai_state(o, ai_state::kFlee, quests, out);
    return true;
}

// `CMonster::idleAIRangedCaster(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_ranged_caster(Object& o, Character& c, float delta, QuestState& quests,
                                        std::vector<ScriptEvent>& out) {
    c.run = false;
    // Defensive on a 35% draw when one is held, offensive otherwise.
    const auto pick = [&]() {
        if (!has_defensive_skill(c) || 0x22 < volatile_random().integer_between(0, 100)) {
            select_offensive_skill(o, c, false, quests, out);
        } else {
            select_defensive_skill(c);
        }
    };
    bool gated = false;
    UnitRef found;
    if (!idle_ai_sight(o, c, false, gated, found)) {
        if (!gated && c.casting.active < 0) pick();
        return false;
    }
    set_target(o, c, true, found, out);
    if (idle_ai_open_attack(o, c, found, quests, out)) return true;
    pick();
    if (active_unit_skill(c) != nullptr && can_cast_current_skill_on(o, c, found, out)) {
        set_target(o, c, true, found, out);
        set_ai_state(o, ai_state::kSkill, quests, out);
        stop_pathing(c);
        monster_update_ai(o, c, delta, quests, out, true);
        return true;
    }
    if (9 < volatile_random().integer_between(0, 100)) return false;
    set_ai_state(o, ai_state::kFlee, quests, out);
    return true;
}

// `CMonster::idleAICircler(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_circler(Object& o, Character& c, float delta, QuestState& quests,
                                  std::vector<ScriptEvent>& out) {
    c.run = false;
    // Defensive on a 10% draw when one is held, offensive otherwise.
    const auto pick = [&]() {
        if (!has_defensive_skill(c) || 9 < volatile_random().integer_between(0, 100)) {
            select_offensive_skill(o, c, false, quests, out);
        } else {
            select_defensive_skill(c);
        }
    };
    bool gated = false;
    UnitRef found;
    if (!idle_ai_sight(o, c, false, gated, found)) {
        if (!gated && c.casting.active < 0) pick();
        return false;
    }
    set_target(o, c, true, found, out);
    if (idle_ai_open_attack(o, c, found, quests, out)) return true;
    pick();
    if (active_unit_skill(c) != nullptr && can_cast_current_skill_on(o, c, found, out)) {
        set_target(o, c, true, found, out);
        set_ai_state(o, ai_state::kSkill, quests, out);
        stop_pathing(c);
        monster_update_ai(o, c, delta, quests, out, true);
        return true;
    }
    // The hunt either way - and it stays the state when the draw
    // below fails and the handler answers that it did nothing.
    set_ai_state(o, ai_state::kHunt, quests, out);
    if (0x13 < volatile_random().integer_between(0, 100)) return false;
    // State 0xb (`getRangeAI`), to a point within 3 of the unit found: z drawn
    // first, then x.
    set_ai_state(o, 0xb, quests, out);
    Vec3 at;
    target_at(found, at);
    const float z = volatile_random().between(at.z - 3.0f, at.z + 3.0f);
    const float x = volatile_random().between(at.x - 3.0f, at.x + 3.0f);
    set_destination(c, stats_for(o.unit), x, z);
    return true;
}

}  // namespace tl::core
