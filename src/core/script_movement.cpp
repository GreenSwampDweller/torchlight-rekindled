// The level machine's characters walking: their movement and paths through the level; split from
// script_units.cpp and script.cpp.
#include "core/script.h"

#include "core/log.h"
#include "core/ogre_math.h"
#include "core/section_profile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tl::core {

namespace {

namespace type = tl::formats::cmp_type;
using namespace tl::core::logic;

// INVENTED: the ground distance the port's walk measures to say the player's
// destination is reached (a stand-in for `setDestination`'s search, below).
float distance_xz(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

// `CCharacter::stopPathing`.
void LevelScript::stop_pathing(Character& c) {
    if (c.pathing) c.since_route = 10.0f;
    c.pathing = false;
    c.next = c.motion.position;
}

// `CCharacter::setDestination(CLevel&, float, float)`.
//
bool LevelScript::set_destination(Character& c, const UnitStats& stats, float x, float z) {
    // No pathfinder, nothing moves (`+0x268 == 0`).
    if (nav_ == nullptr || passability_ == nullptr || !nav_->ready() || !passability_->ready()) {
        return false;
    }
    Object* o = find(c.node);
    const int32_t state = o != nullptr ? o->ai : ai_state::kIdle;
    if (state == 6 || state == 5 || state == 0xc ||
        (stats.movement.walking_speed == 0.0f && stats.movement.running_speed == 0.0f)) {
        if (c.pathing) c.since_route = 10.0f;
        c.pathing = false;
        c.next = c.motion.position;
        c.destination = {x, 0.0f, z};
        return false;
    }
    c.since_route = 0.0f;
    c.destination = {x, c.motion.position.y, z};

    const bool was_on_grid = c.on_grid;
    avoidance_remove(c);
    // `+0x69`: the object grid is ignored while the interface is in a cinematic.
    const bool cinematic = ui_in_cinematic();
    std::vector<Vec3> nodes;
    const Vec3 from = c.motion.position;
    bool found = false;
    timed(Section::Route, [&] {
        found = nav_->find_nodes(from, {x, 0.0f, z}, *passability_, cinematic, nodes, c.search_cap);
    });
    auto restore = [&]() {
        if (was_on_grid && state != 5 && state != 6) avoidance_add(c);
    };
    if (!found) {
        restore();
        return false;
    }

    // `reachedGoal` is "the current node has no next"; `advanceNextNode` steps.
    size_t cursor = 1;
    auto reached = [&]() { return nodes.size() < 2 || cursor + 1 >= nodes.size(); };
    if (!reached()) {
        c.route.clear();
        Vec3 last{};
        Vec3 node{};
        while (!reached()) {
            node = {nodes[cursor].x, 0.0f, nodes[cursor].z};
            bool added = false;
            if (c.route.empty()) {
                last = {from.x, 0.0f, from.z};
                c.route.add_point(last);
                added = true;
            } else if (!passability_->passable_between(last, node, false)) {
                last = node;
                c.route.add_point(last);
                added = true;
            }
            ++cursor;
            if (!reached() || added) continue;
            if (passability_->passable_between(last, c.destination, false) &&
                passability_->position_passable(c.destination)) {
                continue;
            }
            c.route.add_point(node);
        }
        if (!c.route.empty()) {
            if (!passability_->position_passable(c.destination)) {
                c.destination = c.route.back();
            } else if (passability_->passable_between(c.route.back(), c.destination, false)) {
                c.route.add_point({x, 0.0f, z});
            } else {
                c.destination = c.route.back();
            }
            c.pathing = true;
            const Vec3 first = c.route.point(0);
            c.next = {first.x, 0.0f, first.z};
        }
    }
    if (c.route.length() <= 0.0f) c.pathing = false;
    restore();
    return c.pathing;
}

// `CCharacter::updatePathing(float)`.
void LevelScript::update_pathing(Character& c, const UnitStats& stats, float delta) {
    if (!c.pathing) return;
    const SectionScope timed(Section::Pathing);
    Vec3 nearest;
    float along = 0.0f;
    c.route.find_nearest_point(c.motion.position, nearest, along);
    if (c.route.length() <= along) {
        stop_pathing(c);
        return;
    }
    const float ahead = std::min(along + 0.1f, c.route.length());
    c.next = c.route.position_at_distance(ahead);
    const float far_along = std::min(along + 0.3f, c.route.length());
    const Vec3 far_point = c.route.spline_position_at_distance(far_along);
    const float near_along = std::max(far_along - 0.3f, 0.0f);
    const Vec3 near_point = c.route.spline_position_at_distance(near_along);
    float dx = far_point.x - near_point.x;
    float dz = far_point.z - near_point.z;
    const float dy = far_point.y - near_point.y;
    const float span = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (1e-08f < span) {
        dx = dx / span;
        dz = dz / span;
    }
    c.facing = turn_toward(c.facing, dx, dz, stats.turn_rate, delta, nullptr, turn_factor(c));

    Vec3 check;
    float check_along = 0.0f;
    c.route.find_nearest_point(far_point, check, check_along);
    if (check_along <= along && c.route.length() < 1.0f) {
        stop_pathing(c);
    }
    float speed = character_speed(c, stats.movement.walking_speed);
    if (c.run && !(stats.movement.running_speed <= stats.movement.walking_speed)) {
        speed = character_speed(c, stats.movement.running_speed);
    }
    // Times `+0x25c`, and under AI flag 1 times it again and half
    // as much once more.
    const float factor = speed_multiplier(c, stats);
    speed = speed * factor;
    if (c.ai_flags.has(AIFlag::Berserk)) speed = speed * (factor * 1.5f);
    steer(c.motion, c.next.x - c.motion.position.x, c.next.z - c.motion.position.z,
          speed * c.motion.velocity_scale);
}

// `CCharacter::followCharacter`: the quarry's
// own circle comes off the grid while the route to where it stands is found.
void LevelScript::follow_character(Object& o, Character& c, const UnitRef& target,
                                   std::vector<ScriptEvent>& out) {
    if (!target.player()) {
        const Object* them = target_object(target);
        if (them == nullptr || them->ai == ai_state::kDying || them->ai == ai_state::kDead) return;
    }
    set_target(o, c, true, target, out);
    Vec3 at;
    if (!target_at(target, at)) return;
    LevelScript* owner = target.machine;
    Character* them = owner != nullptr ? owner->character(target.node) : nullptr;
    const bool was = them != nullptr && them->on_grid;
    if (them != nullptr) owner->avoidance_remove(*them);
    set_destination(c, stats_for(o.unit), at.x, at.z);
    if (them != nullptr && was) owner->avoidance_add(*them);
}

// PORT: the target item's place through the World's hook.
bool LevelScript::target_item_at(const Character& c, Vec3& at) const {
    return c.target_item != 0 && unit_skills_.item_position &&
           unit_skills_.item_position(c.target_item, at);
}

// `CCharacter::castSkill`: the target, else the target item.
void LevelScript::aim_at_target_item(const Character& c, SkillCaster& caster) const {
    Vec3 there;
    if (c.has_target || !target_item_at(c, there)) return;
    SkillTargetUnit item;
    item.position = there;
    item.is_item = true;
    caster.target_unit = item;
}

// `CCharacter::castSkill`: with no character found, `findItemWithinView` on the
// same frame and reach; `setTargetItem` refuses while dying or dead.
void LevelScript::find_target_item(const Object& o, Character& c, const UnitSkill& s, float angle,
                                   float reach) {
    if (c.has_target || !unit_skills_.item_within_view || o.ai == ai_state::kDying ||
        o.ai == ai_state::kDead) {
        return;
    }
    const ViewFrame frame = skill_frame(c);
    const int64_t item =
        unit_skills_.item_within_view(*this, o.id, frame.at, frame.forward, angle, reach);
    if (item == 0) return;
    c.target_item = item;
    TL_LOG_INFO("script: %s's '%s' finds breakable %lld in view", o.unit.c_str(),
                s.known.entry.name.c_str(), static_cast<long long>(item));
}

// `CCharacter::validLineOfSight(CLevel&, flags)`: true
// under `NO LINE OF SIGHT` or with no target and no target item. The ray goes from the centre
// 1.5 up to the target's centre, the rise held each way to a fifth of the world
// coordinates' length (`fmuls`/`fmadds`); a NaN takes the limit.
bool LevelScript::valid_line_of_sight(const Character& c) const {
    if (c.ai_flags.has(AIFlag::NoLineOfSight)) return true;
    const Vec3& self = c.motion.position;
    // the target item first, at the ray's own height.
    Vec3 centre;
    if (target_item_at(c, centre)) {
        centre.y = self.y;
    } else if (!c.has_target || !target_centre_at(c.target, centre)) {
        return true;
    }
    const Vec3 from{self.x + 0.0f, (0.0f + self.y) + 1.5f, self.z + 0.0f};
    Vec3 to{centre.x, (centre.y - self.y) + from.y, centre.z};
    const float rise = to.y - from.y;
    const float limit =
        std::sqrt(std::fma(from.y, from.y, std::fma(to.z, to.z, to.x * to.x))) * 0.2f;
    const float held = rise < limit ? rise : limit;
    to.y = (-limit < held ? held : -limit) + from.y;
    return sight_unobstructed(from, to);
}

// ---------------------------------------------------------------------------
// Pathing (`CPathController`, `CCharacter::updatePathFollowing`)
// ---------------------------------------------------------------------------

// `CPathController::setEnabled`.
void LevelScript::path_set_enabled(Object& o, bool on, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    if (o.enabled == on) return;
    o.enabled = on;
    broadcast(o.id, on ? kEnabled : kDisabled, quests, out);

    const bool player = o.path_group == "PLAYER" || o.unit == "PLAYER";
    if (!o.path_collected) {
        o.path_units = player ? std::vector<UnitRef>{UnitRef{}} : units_named(o.unit);
        o.path_collected = !o.path_units.empty();
    }
    for (const UnitRef& ref : o.path_units) {
        if (ref.player()) {
            // The player: `setPathToFollow`, `setAIState(2)`.
            if (on) {
                o.path_ended = false;
                if (o.path_to_player) o.position = frame_.point(player_at_);
                player_path_ = o.id;
                player_path_pct_ = -1.0f;
                player_path_forward_ = true;
                player_has_destination_ = false;
            } else if (player_path_ == o.id) {
                player_path_ = 0;
            }
            continue;
        }
        // The unit's own machine runs it, and its events are that machine's.
        LevelScript& runs = *ref.machine;
        std::vector<ScriptEvent>& sink = sink_for(ref.machine, out);
        Object* unit = runs.find(ref.node);
        if (unit == nullptr) continue;
        if (on) {
            o.path_ended = false;
            // A walk-to-player path moves itself onto the unit
            // (`setPosition(unit->getPosition(true))`), so it stands at the room's frame applied to
            // the unit's position.
            if (o.path_to_player) o.position = frame_.point(runs.position_of(*unit));
            // `setTarget` (none, or the player on a walk-to-player path),
            // `stopPathing`, `setTargetItem(0)` (a port monster holds no target
            // item), `setPathToFollow(this)`, `setAIState(2)`.
            if (Character* c = runs.character(ref.node)) {
                runs.set_target(*unit, *c, o.path_to_player, UnitRef{}, sink);
                runs.stop_pathing(*c);
            }
            runs.set_path_to_follow(*unit, o.id, this);
            runs.set_ai_state(*unit, ai_state::kIdle, quests, sink);
        } else if (unit->path == o.id && unit->path_machine == this) {
            runs.set_path_to_follow(*unit, 0, nullptr);
        }
        // `ENABLE ON START`: `CSceneNodeObject::setEnabled(true)` on the unit,
        // which only stores the flag (+0x82). It runs when the path is switched
        // off as well as on: the original's quirk, kept.
        if (o.path_enable_units && !unit->enabled) {
            unit->enabled = true;
            ScriptEvent e;
            e.action = ScriptAction::SetEnabled;
            e.node_id = unit->id;
            e.name = unit->name;
            e.on = true;
            sink.push_back(e);
        }
    }
}

// `CCharacter::setPathToFollowByName`: the first
// `Pathing` object in creation order whose `PATH NAME` equals it exactly; none clears.
void LevelScript::set_path_to_follow_by_name(Object& unit) {
    for (const Object& o : objects_) {
        if (o.type != type::kPathing || o.path_name != unit.path_name) continue;
        unit.path = o.id;
        unit.path_machine = this;
        return;
    }
    unit.path = 0;
    unit.path_machine = nullptr;
}

// `CMonster::setPathToFollow`, the same body as
// `CCharacter::setPathToFollow`: the cap restored, a
// path raising it to 800; route clock 10, percent -1.
void LevelScript::set_path_to_follow(Object& unit, int64_t path, LevelScript* machine) {
    Character* c = character(unit.id);
    if (c != nullptr) c->search_cap = c->saved_search_cap;
    unit.path = machine != nullptr ? path : 0;
    unit.path_machine = machine != nullptr && path != 0 ? machine : nullptr;
    if (unit.path_machine != nullptr && c != nullptr) {
        c->saved_search_cap = c->search_cap;
        c->search_cap = 800;
    }
    if (c != nullptr) c->since_route = 10.0f;
    unit.path_pct = -1.0f;
}

int32_t LevelScript::unit_search_cap(int64_t node) const {
    const Character* c = character(node);
    return c != nullptr ? c->search_cap : -1;
}

// `CPathController::getNextPointAtPercent`.
Vec3 LevelScript::path_point_at(const Object& path, float pct, bool forward,
                                float unit_radius) const {
    // `PATH TO PLAYER`: `CPathController::caculatePointInFrontOfPlayer`:
    // from the player towards the controller, twice the two radii out, at y 0.
    if (path.path_to_player) {
        float dx = path.position.x - player_at_.x;
        float dz = path.position.z - player_at_.z;
        const float length = std::sqrt(dx * dx + 0.0f + dz * dz);
        if (1e-08 < static_cast<double>(length)) {
            const float inv = 1.0f / length;
            dx = dx * inv;
            dz = dz * inv;
        }
        const float reach = player_radius_ + unit_radius;
        const float step = reach + reach;
        return Vec3{dx * step + player_at_.x, 0.0f, dz * step + player_at_.z};
    }
    if (path.points.empty()) return path.position;
    pct = pct <= 0.0f ? 0.0f : std::min(pct, 1.0f);
    auto fraction = [&](size_t i) {
        return path.path_total == 0.0f ? path.cumulative[i] : path.cumulative[i] / path.path_total;
    };
    if (!forward) {
        size_t i = path.points.size();
        do {
            --i;
        } while (i > 0 && pct < fraction(i));
        return path.points[i];
    }
    for (size_t i = 0; i < path.points.size(); ++i) {
        if (pct <= fraction(i)) return path.points[i];
    }
    return path.points.front();
}

// `CPathController::getClosestPctOfPath`. It
// measures the distance to every waypoint and then returns the first waypoint's
// fraction regardless, which is 0: the original's quirk, kept.
float LevelScript::path_closest_pct(const Object& path) const {
    if (path.path_to_player || path.points.size() == 1 || path.path_total == 0.0f) return 1.0f;
    return path.cumulative.front() / path.path_total;
}

// `CCharacter::updatePathFollowing(bool)`.
void LevelScript::path_follow(Object& unit, bool force, const Vec3& at, QuestState& quests,
                              std::vector<ScriptEvent>& out) {
    Character* c = character(unit.id);
    if (!force && !unit.enabled) {
        if (c != nullptr) stop_pathing(*c);
        unit.has_destination = false;
        return;
    }
    // The player's stand-in carries no machine: its path is this machine's.
    Object* path = path_of(unit);
    if (path == nullptr) return;

    bool set = false;
    if (unit.path_pct <= 0.0f) {
        unit.path_pct = unit.path_forward ? path_closest_pct(*path) : 0.95f;
        set = true;
    }
    // On the ground plane: both heights are zeroed before the test
    // (`movss %xmm2,0x54(%rsp)` with xmm2 = 0, and again).
    const float radius = c != nullptr ? c->motion.radius : player_radius_;
    const Vec3 point = path_point_at(*path, unit.path_pct, unit.path_forward, radius);
    const float dx = point.x - at.x;
    const float dz = point.z - at.z;
    if (!(2.0f < dx * dx + dz * dz)) {
        const float closest = path_closest_pct(*path);
        if (!unit.path_forward) {
            const float pct = std::min(closest, unit.path_pct);
            unit.path_pct = pct;
            if (0.0f <= pct) {
                unit.path_pct = pct - 0.05f;
                if (pct - 0.05f <= 0.05f) {
                    unit.path_pct = 0.0f;
                    if (unit.path_loops) unit.path_forward = true;
                }
                set = true;
            }
        } else {
            const float pct = std::max(closest, unit.path_pct);
            unit.path_pct = pct;
            if (!(1.0f < pct) || set) {
                unit.path_pct = pct + 0.05f;
                if (pct + 0.05f < 0.95f) {
                    set = true;
                } else {
                    unit.path_pct = 1.0f;
                    if (!unit.path_loops) {
                        set = true;
                        if (!path->path_ended) {
                            set = false;
                            path_reached(unit.path_machine, *path, quests, out);
                        }
                    } else {
                        unit.path_forward = false;
                        set = true;
                    }
                }
            }
        }
    }

    // on: a unit not moving takes a destination while its path runs;
    // otherwise only a step along the path gives it a new one.
    if (c != nullptr) {
        c->run = path->path_runs;
        if (!(!c->pathing && !path->path_ended) && !set) return;
        const Vec3 to = path_point_at(*path, unit.path_pct, unit.path_forward, radius);
        set_destination(*c, stats_for(unit.unit), to.x, to.z);
        if (!c->pathing && !path->path_ended) path_reached(unit.path_machine, *path, quests, out);
        return;
    }

    // The player (`CPlayer::update` runs this every frame): the port's walk
    // stands in for the player's `setDestination`.
    unit.running = path->path_runs;
    if (!(!unit.has_destination && !path->path_ended) && !set) return;

    // `setDestination` to the waypoint the percent names;
    // already in the goal's cell, the unit is not moving.
    unit.destination = path_point_at(*path, unit.path_pct, unit.path_forward, radius);
    unit.has_destination = distance_xz(unit.destination, at) > kNavCell * 0.5f;
    if (!unit.has_destination && !path->path_ended) {
        path->path_ended = true;
        broadcast(path->id, kEndOfPathReached, quests, out);
    }
}

}  // namespace tl::core
