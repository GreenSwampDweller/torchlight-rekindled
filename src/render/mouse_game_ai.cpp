// The mouse layer: the player's AI the clicks start, and the host's longer answers.
#include "render/mouse_game.h"

namespace tl::render {

namespace ai = tl::core::mouse_ai;

// --- The player's AI the clicks start ------------------------------------------------------

/// The player's reach to a point: `inAttackRange(0)` and `validLineOfSight`.
void Renderer::Impl::MouseGame::reach(const Vec3& at, float target_radius, bool& in_range,
                                      bool& sight) {
    g.load_unit_types();
    g.load_stat_graphs();
    const Vec3 me = g.player.position();
    // CCharacter::inAttackRange(0) with the attack in use.
    g.select_player_attack(0);
    const tl::core::HeldHands hands = g.player_held_hands();
    tl::core::AttackDescription attack =
        g.has_weapon ? tl::core::AttackDescription{}
                     : tl::core::own_attack(g.player_stats, g.player_stats.level,
                                            g.config.difficulty, /*player=*/true, g.stat_graphs);
    if (g.has_weapon) attack.range = g.weapon.range;
    const bool ranged = g.has_weapon && g.unit_types.isa(g.unit_types.id_of(g.weapon.unittype),
                                                         tl::core::unit_type::kRangedWeapon);
    tl::core::UnitStats body = g.player_stats;
    body.radius = g.player.body().radius;
    in_range =
        tl::core::in_attack_range(body, attack, me, at, target_radius, ranged,
                                  ranged ? g.player_total().effect_value(0x4e) : 0.0f, &hands);
    // CCharacter::validLineOfSight: 1.5 above both
    // feet, the rise held to a fifth of the length.
    const Vec3 from{me.x, me.y + 1.5f, me.z};
    Vec3 to{at.x, at.y + 1.5f, at.z};
    const float limit = std::sqrt(to.x * to.x + from.y * from.y + to.z * to.z) / 5.0f;
    float rise = to.y - from.y;
    if (limit <= rise) rise = limit;
    if (rise <= -limit) rise = -limit;
    to.y = rise + from.y;
    tl::core::CollisionHit hit;
    sight = g.collision.empty() || !g.collision.ray_collision(from, to, true, hit);
}

/// What the player's character is after: its target (`+0x340`), or the breakable that
/// is its target item (`+0x350`).
bool Renderer::Impl::MouseGame::target_point(Vec3& at, float& radius) const {
    if (g.player_target_node != 0) {
        tl::core::LevelScript* owner = g.script_owning(g.player_target_node);
        if (owner == nullptr || owner->unit_is_dead(g.player_target_node)) return false;
        if (!owner->unit_position(g.player_target_node, at)) return false;
        radius = owner->unit_radius(g.player_target_node);
        return true;
    }
    const Item* item = item_of(target_item_handle);
    if (item == nullptr || item->breakable == 0) return false;
    radius = 0.0f;
    return item_position(target_item_handle, at);
}

void Renderer::Impl::MouseGame::route_to(const Vec3& to) {
    const Vec3 me = g.player.position();
    std::vector<Vec3> route;
    if (g.nav.ready()) route = g.nav.find_path(me, Vec3{to.x, me.y, to.z}, g.passability);
    if (route.empty()) {
        if (no_route.x != to.x || no_route.z != to.z) {
            TL_LOG_INFO("mouse: no route from (%.2f, %.2f) to (%.2f, %.2f)%s",
                        static_cast<double>(me.x), static_cast<double>(me.z),
                        static_cast<double>(to.x), static_cast<double>(to.z),
                        g.nav.ready() ? "" : " (no path grid)");
            no_route = {to.x, 0.0f, to.z};
        }
        g.player.stop_following();
        return;
    }
    g.player.follow(std::move(route));
    route_end = {to.x, 0.0f, to.z};
    // `setDestination` zeroes `+0x278`.
    path_timer = 0.0f;
}

void Renderer::Impl::MouseGame::stop() {
    // `CCharacter::stopPathing`: `+0x278` is 10 when it was walking.
    if (g.player.following()) path_timer = 10.0f;
    g.player.stop_following();
}

void Renderer::Impl::MouseGame::give_up() {
    state = ai::kIdle;
    g.set_player_target(0);
    target_item_handle = 0;
}

void Renderer::Impl::MouseGame::swing_at_target() {
    const bool was = g.swing.active;
    // PORT: the turn the character's update makes towards its target, taken at once.
    Vec3 at;
    float radius = 0.0f;
    if (target_point(at, radius)) {
        const Vec3 me = g.player.position();
        g.player.face(std::atan2(at.x - me.x, at.z - me.z));
        g.player_turn_point = {at.x, 0.0f, at.z};
    }
    g.begin_swing(g.player_target_node);
    if (!was && g.swing.active) attack_started = true;
}

// CCharacter::attackAI for the player.
void Renderer::Impl::MouseGame::attack_ai(float delta) {
    Vec3 at;
    float radius = 0.0f;
    if (!target_point(at, radius)) return give_up();
    bool in_range = false;
    bool sight = false;
    reach(at, radius, in_range, sight);
    if (!in_range || !sight) {
        // A swing with more than 0.2 s left or a cast holding: nothing; else back to the hunt.
        if (performing_attack_loose() || g.player_performing_skill()) return;
        state = ai::kHunt;
        return;
    }
    stop();
    const Vec3 me = g.player.position();
    g.player.face(tl::core::turn_toward(g.player.facing(), at.x - me.x, at.z - me.z,
                                        g.player_stats.turn_rate, delta));
    if (!tl::core::facing_target(g.player.facing(), me, at, g.player_stats.attack_angle)) return;
    swing_at_target();
    state = ai::kIdle;
}

/// `followCharacter` when standing, or when the target is `moved` from where the route
/// was going; a breakable is walked to once. False when there is no way to it.
bool Renderer::Impl::MouseGame::follow_target(const Vec3& at, float moved) {
    if (!g.player.following()) {
        route_to(at);
    } else if (g.player_target_node != 0 && moved <= planar(at, route_end)) {
        route_to(at);
    }
    return g.player.following();
}

// CCharacter::huntAI for the player.
void Renderer::Impl::MouseGame::hunt(float delta) {
    Vec3 at;
    float radius = 0.0f;
    if (!target_point(at, radius)) return give_up();
    const bool hold = g.attack_button.hold;
    // Beyond `FOLLOW_RADIUS` (`+0x370`), not holding position: stopped and let go.
    if (g.player_stats.follow_radius <= planar(at, g.player.position()) && !hold) {
        stop();
        return give_up();
    }
    bool in_range = false;
    bool sight = false;
    reach(at, radius, in_range, sight);
    const bool reached = in_range && sight;
    if (!reached && !hold && 0.5f < path_timer && !follow_target(at, 1.0f)) return give_up();
    if (!reached && !hold) return;
    state = kAttackState;
    stop();
    attack_ai(delta);
}

const tl::core::KnownSkill* Renderer::Impl::MouseGame::skill(std::int64_t guid) const {
    if (guid == tl::core::kNoSkill) return nullptr;
    return g.player_skills.find_by_guid(guid);
}

/// A skill's target type at its current level.
int Renderer::Impl::MouseGame::target_type(const tl::core::KnownSkill& known) const {
    const tl::core::SkillProperty* property = known.property(known.effective);
    if (property != nullptr) return property->target_type;
    return known.def != nullptr ? known.def->base.target_type : 0;
}

// CCharacter::inSkillRange for the player: 0 in range,
// 1 nearer than `RANGEMIN`, 2 out of range or nothing to reach.
int Renderer::Impl::MouseGame::skill_reach(std::int64_t guid) const {
    const tl::core::KnownSkill* known = skill(guid);
    if (known == nullptr) return 2;
    if (g.player_target_node == 0 && item_of(target_item_handle) == nullptr) return 2;
    const int type = target_type(*known);
    if (type == 3 || type == 10 || type == 9) return 0;
    const tl::core::SkillProperty* property = known->property(known->effective);
    const tl::core::SkillProperty* fallback = known->def != nullptr ? &known->def->base : nullptr;
    const tl::core::SkillProperty* read = property != nullptr ? property : fallback;
    const float range = read != nullptr ? read->range : 0.0f;
    const float range_min = read != nullptr ? read->range_min : 0.0f;
    const float mine = std::max(0.0f, g.player.body().radius);
    Vec3 at;
    float radius = 0.0f;
    const bool have = target_point(at, radius);
    if (g.player_target_node == 0) {
        // The target item: its distance less its radius and the character's; in or out.
        if (!have) return 0;
        const float gap = (planar(at, g.player.position()) - tl::core::kBreakableRadius) - mine;
        return range < gap ? 2 : 0;
    }
    if (!have) return 2;
    const float gap = (planar(at, g.player.position()) - std::max(0.0f, radius)) - mine;
    if (range < gap) return 2;
    return gap < range_min ? 1 : 0;
}

bool Renderer::Impl::MouseGame::cast(std::int64_t guid) {
    const tl::core::KnownSkill* known = skill(guid);
    if (known == nullptr) return false;
    const bool was_active = !g.player_active_skill.empty();
    const std::string was_skill = g.player_active_skill.skill;
    if (aim.has_value()) g.pad_skill_target = *aim;
    const bool done = g.cast_player_skill(*known, true);
    g.pad_skill_target.reset();
    if (!g.player_active_skill.empty() &&
        (!was_active || was_skill != g.player_active_skill.skill)) {
        attack_started = true;
    }
    return done;
}

// CCharacter::skillApproachAI for the player.
void Renderer::Impl::MouseGame::skill_approach() {
    Vec3 at;
    float radius = 0.0f;
    if (!target_point(at, radius)) return give_up();
    // The skill asked for, or with none the current one.
    const tl::core::KnownSkill* known = skill(approach_skill);
    if (known == nullptr) known = g.player_skills.find_by_name(g.player_active_skill.skill);
    if (known == nullptr) return;
    const std::int64_t guid = known->guid();
    // `canCastCurrentSkill`: one cooling or not paid for ends the approach (state 2).
    const tl::core::SkillProperty* property = known->property(known->effective);
    const float cost = property != nullptr ? property->mana_cost : 0.0f;
    if (g.player_skill_runner.is_cooling(known->entry.name) || g.player_character.mp() < cost) {
        stop();
        state = ai::kIdle;
        return;
    }
    const bool hold = g.attack_button.hold;
    // Beyond `FOLLOW_RADIUS` (`+0x370`), not holding position: stopped and let go.
    if (g.player_stats.follow_radius <= planar(at, g.player.position()) && !hold) {
        stop();
        return give_up();
    }
    // Skills of target types 3, 10 and 9 need no line of sight.
    const int type = target_type(*known);
    bool sight = true;
    bool in_range = false;
    if (type != 3 && type != 10 && type != 9) reach(at, radius, in_range, sight);
    const int near = skill_reach(guid);
    if (near == 1) {
        // Nearer than the skill's minimum: a character that is no monster gives up.
        stop();
        state = ai::kIdle;
        g.set_player_target(0);
        return;
    }
    // `followCharacter` again once the target is 4 from where the route was going.
    // `CSkill::rollCancelChance` is not rolled: the port's player skills carry none.
    const bool reached = near == 0 && sight;
    if (!reached && !hold && 1.0f < path_timer && !follow_target(at, 4.0f)) return give_up();
    if ((skill_reach(guid) != 0 || !sight) && !hold) return;
    // In reach: state 0xd, stopped, and `skillAI` casts.
    state = kSkillState;
    stop();
    cast(guid);
}

void Renderer::Impl::MouseGame::step_ai(float delta) {
    // `+0x278` runs with the character's update, to 100.
    path_timer = std::min(100.0f, path_timer + delta);
    if (!g.player_control || g.subject_node == nullptr || g.player_death.dying) {
        state = ai::kIdle;
        return;
    }
    switch (state) {
        case kAttackState: attack_ai(delta); break;
        case ai::kHunt: hunt(delta); break;
        case ai::kSkillApproach: skill_approach(); break;
        case kSkillState:
            // `skillAI`: over when the skill is.
            if (!g.player_performing_skill()) state = ai::kIdle;
            break;
        default: break;
    }
}

// --- MouseHost, the longer answers ---------------------------------------------------------

int Renderer::Impl::MouseGame::ai_state() {
    if (g.fishing_state == tl::core::FishingState::Catch) return 0x22;
    if (g.player_item.kind != Kind::None) return ai::kGetItem;
    if (!g.pending_talk.empty()) return ai::kApproach;
    if (state == ai::kGetItem || state == ai::kApproach) state = ai::kIdle;
    return state;
}

void Renderer::Impl::MouseGame::set_ai_state(int wanted) {
    if (wanted == ai::kApproach) {
        // `approachAI` is the port's walk to somebody to talk to (`step_pending_talk`),
        // which holds them by name; the character's target is let go again.
        const std::string name = name_of(static_cast<Unit>(g.player_target_node));
        g.set_player_target(0);
        g.player_item = PlayerItemTarget{};
        if (!name.empty() && g.pending_talk != name) {
            g.pending_talk = name;
            g.set_talk_target(name);
            TL_LOG_INFO("mouse: the click walks to '%s'", name.c_str());
        }
        state = name.empty() ? ai::kIdle : wanted;
        return;
    }
    // Any other state ends the walk to somebody and to an item.
    if (!g.pending_talk.empty()) {
        g.pending_talk.clear();
        g.clear_talk_target();
    }
    g.player_item = PlayerItemTarget{};
    if (wanted == ai::kStop) {
        stop();
        wanted = ai::kIdle;
    }
    state = wanted;
}

void Renderer::Impl::MouseGame::set_destination(float x, float z) {
    if (g.attack_button.hold) {
        // Under the hold the character stops and attacks where it stands.
        const bool was = g.swing.active;
        g.set_player_destination_held(x, z);
        if (!was && g.swing.active) attack_started = true;
        return;
    }
    if (g.player_motion_held()) return;
    // The same point asked for again while the route to it is being walked.
    if (g.player.following() && std::fabs(route_end.x - x) < 0.05f &&
        std::fabs(route_end.z - z) < 0.05f) {
        return;
    }
    route_to({x, 0.0f, z});
}

void Renderer::Impl::MouseGame::fetch_item(Unit item, bool /*by_pet*/, Unit previous) {
    const Item* found = item_of(item);
    if (found == nullptr || found->target.kind == Kind::None) return;
    if (target_item_handle == item && g.player_item.kind != Kind::None) return;
    if (previous != item) stop();
    if (!g.pending_talk.empty()) {
        g.pending_talk.clear();
        g.clear_talk_target();
    }
    target_item_handle = item;
    g.player_item = found->target;
    state = ai::kGetItem;
    stop();
    TL_LOG_INFO("mouse: the click sets the target item");
}

}  // namespace tl::render
