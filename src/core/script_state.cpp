// A level's script remembered and put back: the `CLogicNodeState` records
// `CPlayer::storeLevelSavedState` takes of a room
// layout's logic nodes, `CGameClient::restoreLevelState`
// putting them back, and the character side of both.
#include "core/log.h"
#include "core/script.h"
#include "formats/float_convert.h"

#include <cmath>
#include <cstring>
#include <iterator>

namespace tl::core {

namespace {

namespace type = tl::formats::cmp_type;
using tl::formats::LogicNodeState;

constexpr uint32_t kTeleport = 41;
constexpr uint32_t kOutputIncrementor = 43;

// PORT: the save format's vector.
tl::formats::SaveVec3 to_save(const Vec3& v) { return {v.x, v.y, v.z}; }
// PORT: the save format's vector.
Vec3 from_save(const tl::formats::SaveVec3& v) { return {v.x, v.y, v.z}; }

// PORT: a saved float's bits read back.
float bits_to_float(int32_t bits) {
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

int32_t float_to_bits(float value) {
    int32_t out;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

// What `storeLevelSavedState` stores, per descriptor, in its order.
struct Descriptor {
    const char* name;
    uint32_t type;
};
constexpr Descriptor kStoreOrder[] = {
    {"Player Sphere Trigger", type::kPlayerSphereTrigger},
    {"Player Box Trigger", type::kPlayerBoxTrigger},
    {"Property Node", type::kMarker},
    {"Group", type::kGroup},
    {"Timeline", type::kTimeline},
    {"Counter", type::kCounter},
    {"Timer", type::kTimer},
    {"Output Incrementor", kOutputIncrementor},
    {"Teleport", kTeleport},
    {"Animation Controller", type::kAnimationController},
    {"Puzzle Input", 46},
    {"Unit Spawner", type::kUnitSpawner},
};

// ... and the order `restoreLevelState` walks them in.
constexpr Descriptor kRestoreOrder[] = {
    {"Timeline", type::kTimeline},
    {"Player Sphere Trigger", type::kPlayerSphereTrigger},
    {"Player Box Trigger", type::kPlayerBoxTrigger},
    {"Property Node", type::kMarker},
    {"Group", type::kGroup},
    {"Animation Controller", type::kAnimationController},
    {"Counter", type::kCounter},
    {"Timer", type::kTimer},
    {"Output Incrementor", kOutputIncrementor},
    {"Teleport", kTeleport},
    {"Puzzle Input", 46},
    {"Unit Spawner", type::kUnitSpawner},
};

}  // namespace

// CPlayer::storeLevelSavedState, the
// per-layout descriptor loop.
int LevelScript::store_descriptors() { return static_cast<int>(std::size(kStoreOrder)); }

void LevelScript::store_logic_states(uint32_t scene, int64_t hierarchy,
                                     std::vector<LogicNodeState>& out, int only,
                                     int64_t top_parent) const {
    for (int k = 0; k < store_descriptors(); ++k) {
        if (only >= 0 && k != only) continue;
        const Descriptor& descriptor = kStoreOrder[k];
        for (const Object& o : objects_) {
            if (o.type != descriptor.type) continue;
            LogicNodeState s;
            s.descriptor = descriptor.name;
            s.scene = scene;
            s.object_id = o.id;
            // A root of the file (parent -1) hangs from what loaded it: the Layout Link for a linked copy.
            // A Timer, Timeline or Output Incrementor passes over Groups to the first ancestor that is not
            // one, as the console stores it (how its loader comes to it is not read).
            int64_t parent = o.parent;
            if (o.type == type::kTimer || o.type == type::kTimeline ||
                o.type == kOutputIncrementor) {
                const Object* up = find(parent);
                for (int depth = 0; up != nullptr && up->type == type::kGroup && depth < 64;
                     ++depth) {
                    parent = up->parent;
                    up = find(parent);
                }
            }
            s.parent_id = parent != 0 && parent != -1 ? parent : top_parent;
            s.hierarchy = hierarchy;
            switch (o.type) {
                case type::kPlayerSphereTrigger:
                case type::kPlayerBoxTrigger:
                    s.flags[0] = o.enabled;
                    s.flags[1] = o.inside;            // +0x100
                    s.flags[2] = o.triggered_once;    // +0x101
                    s.flags[3] = o.deactivated_once;  // +0x102
                    s.position = to_save(own_position(o));
                    break;
                case type::kMarker:
                    s.flags[0] = o.enabled;
                    s.position = to_save(own_position(o));
                    break;
                case type::kGroup:
                    s.flags[0] = o.enabled;
                    s.flags[1] = o.visible;  // +0x81
                    s.position = to_save(own_position(o));
                    break;
                case type::kTimeline:
                    // the percent through, 1 with no duration.
                    s.f20 = 0.0f < o.tl_duration ? o.tl_time / o.tl_duration : 1.0f;
                    s.flags[0] = o.tl_playing;             // +0xa1
                    s.f24 = o.tl_backwards ? 1.0f : 0.0f;  // +0xa0
                    s.flags[4] = o.enabled;                // !+0xa2
                    break;
                case type::kCounter:
                    s.f20 = static_cast<float>(o.counter_value);  // +0x60
                    s.flags[0] = o.enabled;                       // +0x68
                    break;
                case type::kTimer:
                    s.i1c = o.timer_loops_left;  // +0x68
                    s.f20 = o.timer_time;        // +0x58
                    s.flags[0] = o.enabled;      // +0x6c
                    break;
                case kOutputIncrementor:
                    // `storeLevelSavedState`: the count (+0x5c),
                    // the loops left (+0x64) as a float, the flag (+0x68).
                    s.i1c = o.counter_value;
                    s.f20 = static_cast<float>(o.incr_loops_left);
                    s.flags[0] = o.enabled;
                    break;
                case kTeleport: s.flags[0] = o.enabled; break;
                case type::kAnimationController:
                    s.flags[0] = o.anim_playing;          // +0x91
                    s.flags[4] = o.anim_loops;            // +0x92
                    s.i18 = float_to_bits(o.anim_force);  // +0xa4
                    break;
                case 46:
                    // `CPuzzleRandomizer` +0x68 (enabled).
                    // Only enabled is saved: a restored puzzle keeps its build-time
                    // shuffle and loses its progress.
                    s.flags[0] = o.enabled;
                    break;
                case type::kUnitSpawner:
                    if (o.spawner) {
                        s.flags[0] = o.spawner->releasing();                // +0x1c1
                        s.f20 = static_cast<float>(o.spawner->released());  // +0x18c
                        s.f24 = o.spawner->elapsed();                       // +0x188
                    }
                    break;
                default: break;
            }
            out.push_back(std::move(s));
        }
    }
}

// CGameClient::restoreLevelState, the
// per-layout loop.
void LevelScript::restore_logic_states(uint32_t scene, int64_t hierarchy,
                                       const std::vector<LogicNodeState>& states,
                                       QuestState& quests, std::vector<ScriptEvent>& out) {
    auto matching = [&](const Object& o, const char* descriptor) -> const LogicNodeState* {
        for (const LogicNodeState& s : states) {
            if (s.scene != scene || s.object_id != o.id || s.hierarchy != hierarchy) continue;
            if (s.descriptor != descriptor) continue;
            return &s;
        }
        return nullptr;
    };

    for (const Descriptor& descriptor : kRestoreOrder) {
        for (size_t i = 0; i < objects_.size(); ++i) {
            if (objects_[i].type != descriptor.type) continue;
            const LogicNodeState* s = matching(objects_[i], descriptor.name);
            if (s == nullptr) continue;
            Object& o = objects_[i];
            switch (o.type) {
                case type::kTimeline:
                    if (s->flags[4] == 0) {
                        // `CTimeline::Stop`.
                        if (!o.tl_backwards) {
                            if (0.0f < o.tl_time) timeline_stop_to(o, false, quests, out);
                        } else if (o.tl_time <= 1.0f && o.tl_time != 1.0f) {
                            timeline_stop_to(o, true, quests, out);
                        }
                    } else {
                        o.tl_playing = true;  // `SetEnabled(true)` writes +0xa1
                        o.tl_backwards = s->f24 == 1.0f;
                        if (0.0f < s->f20 && o.tl_set_properties_on_load && !o.tl_backwards) {
                            // `Play(true)`, ignored while already playing
                            // forwards; then `UpdateTimeline(+0x20 * duration)`.
                            timeline_play(o, quests, out);
                            timeline_update(o, s->f20 * o.tl_duration, quests, out);
                        }
                    }
                    o.enabled = s->flags[4] != 0;
                    o.tl_playing = s->flags[0] != 0;
                    TL_LOG_DEBUG(
                        "  timeline %lld put back: playing %d enabled %d percent %a backwards %a"
                        " -> time %a of %a step %a",
                        static_cast<long long>(o.id), s->flags[0], s->flags[4],
                        static_cast<double>(s->f20), static_cast<double>(s->f24),
                        static_cast<double>(o.tl_time), static_cast<double>(o.tl_duration),
                        static_cast<double>(o.tl_step));
                    break;
                case type::kPlayerSphereTrigger:
                case type::kPlayerBoxTrigger:
                    o.enabled = s->flags[0] != 0;
                    o.inside = s->flags[1] != 0;
                    o.triggered_once = s->flags[2] != 0;
                    o.deactivated_once = s->flags[3] != 0;
                    o.position = position_from_own(o, from_save(s->position));
                    break;
                case type::kMarker: {
                    o.position = position_from_own(o, from_save(s->position));
                    const bool on = s->flags[0] != 0;
                    if (o.enabled != on) {
                        o.enabled = on;
                        ScriptEvent e;
                        e.action = ScriptAction::SetEnabled;
                        e.node_id = o.id;
                        e.name = o.name;
                        e.on = on;
                        out.push_back(e);
                    }
                    break;
                }
                case type::kGroup: {
                    o.enabled = s->flags[0] != 0;
                    const bool visible = s->flags[1] != 0;
                    if (o.visible != visible) {
                        o.visible = visible;
                        ScriptEvent e;
                        e.action = ScriptAction::SetVisible;
                        e.node_id = o.id;
                        e.name = o.name;
                        e.on = visible;
                        out.push_back(e);
                    }
                    o.position = position_from_own(o, from_save(s->position));
                    break;
                }
                case type::kAnimationController:
                    if (s->flags[0] != 0) {
                        const size_t first = out.size();
                        animation_play(o, s->flags[4] != 0, quests, out);
                        // `CAnimationPlayer::setAnimationDuration(float)`;
                        // unless the controller
                        // loops, every unit's model is stepped on by it at once
                        // (`CGenericModel::updateAnimation(d, true)`).
                        o.anim_remaining = bits_to_float(s->i18);
                        if (!o.anim_loops) {
                            for (size_t i = first; i < out.size(); ++i) {
                                if (out[i].action == ScriptAction::Animation && out[i].on) {
                                    out[i].advance = o.anim_remaining;
                                }
                            }
                        }
                    }
                    break;
                case type::kCounter:
                    o.enabled = s->flags[0] != 0;
                    o.counter_value = tl::formats::x86_float_to_int32(s->f20);  // from a save file
                    break;
                case type::kTimer:
                    timer_set_enabled(o, s->flags[0] != 0, quests, out);
                    o.timer_loops_left = s->i1c;
                    o.timer_time = s->f20;
                    timer_reset(o);
                    break;
                case kOutputIncrementor:
                    // `COutputIncrementor::setEnabled` broadcasts.
                    o.enabled = s->flags[0] != 0;
                    broadcast(o.id, o.enabled ? logic::kEnabled : logic::kDisabled, quests, out);
                    // `restoreLevelState`: the loops left
                    // truncated from the float, then the count.
                    o.incr_loops_left = static_cast<int32_t>(s->f20);
                    o.counter_value = s->i1c;
                    break;
                case kTeleport: o.enabled = s->flags[0] != 0; break;
                case 46:
                    // `CPuzzleRandomizer` +0x68 (enabled).
                    // Only enabled is saved: a restored puzzle keeps its build-time
                    // shuffle and loses its progress.
                    o.enabled = s->flags[0] != 0;
                    break;
                case type::kUnitSpawner:
                    if (o.spawner) {
                        o.spawner->restore_state(
                            s->flags[0] != 0, static_cast<uint32_t>(static_cast<int64_t>(s->f20)),
                            s->f24);
                    }
                    break;
                default: break;
            }
        }
    }
}

std::vector<LevelScript::CharacterRecord> LevelScript::living_characters() const {
    std::vector<CharacterRecord> out;
    for (const Object& o : objects_) {
        if (o.type != type::kMonster) continue;
        const Character* c = character(o.id);
        if (c == nullptr) continue;
        // A pet goes with the player (`CPlayer::storeLevelSavedState`'s
        // first pass skips a unit with a master).
        if (c->master_player) continue;
        // `CCharacter::alive` and `+0x190` (pending delete).
        if (dead_.count(o.id) != 0 || o.ai == ai_state::kDying || o.ai == ai_state::kDead) continue;
        CharacterRecord record;
        record.node = o.id;
        record.unit = o.unit;
        record.from_layout = !o.spawned;
        record.position = feet_of(*c);
        record.facing = c->facing;
        record.hp = c->combat.hp;
        record.level = c->combat.level;
        record.champion_name = c->champion_name;
        if (c->has_master) record.master = c->master.node;
        record.equipment = true;
        record.right_hand = c->right_hand;
        record.left_hand = c->left_hand;
        record.weapon_attack = c->weapon_attack;
        record.left_weapon_attack = c->left_weapon_attack;
        out.push_back(std::move(record));
    }
    return out;
}

// `CGameClient::restoreLevelState`, the
// character side: `setLevel` (vtable +0x318) before the hit points, then the
// place, facing and held items the record kept.
bool LevelScript::apply_character_record(const CharacterRecord& record,
                                         std::vector<ScriptEvent>& out) {
    Object* o = find(record.node);
    Character* c = character(record.node);
    if (o == nullptr || c == nullptr) return false;
    // `vtable +0x318` is `setLevel`, before the hit points are written back.
    if (record.level != c->combat.level) set_unit_level(record.node, record.level);
    c->motion.position = {record.position.x, record.position.y + c->motion.radius,
                          record.position.z};
    c->facing = record.facing;
    c->combat.hp = record.hp;
    // Its saved inventory replaces what it was made holding.
    if (record.equipment) {
        c->right_hand = record.right_hand;
        c->left_hand = record.left_hand;
        c->weapon_attack = record.weapon_attack;
        c->left_weapon_attack = record.left_weapon_attack;
        c->attack_left = false;
    }
    o->position = record.position;
    // `CCharacter::applySaveState` moves it with `setPosition` alone:
    // the avoidance map is not touched until its first update.
    ScriptEvent e;
    e.action = ScriptAction::MoveUnit;
    e.node_id = o->id;
    e.unit = o->unit;
    e.position = record.position;
    e.facing = record.facing;
    e.turned = true;
    out.push_back(e);
    c->sent_at = record.position;
    c->sent_facing = record.facing;
    return true;
}

// `CGameClient::restoreLevelState`: a placed
// monster the remembered level does not list is gone (it had died or been
// removed): hidden, untargetable and dead.
std::vector<int64_t> LevelScript::delete_placed_monsters_except(const std::set<int64_t>& kept,
                                                                std::vector<ScriptEvent>& out) {
    std::vector<int64_t> deleted;
    for (const Object& o : objects_) {
        // Only what a `Monster` descriptor placed: a spawned unit is no descriptor's.
        if (o.type != type::kMonster || o.unit.empty() || o.spawned) continue;
        if (kept.count(o.id) != 0) continue;
        const Character* c = character(o.id);
        if (c == nullptr) continue;
        deleted.push_back(o.id);
    }
    for (int64_t id : deleted) {
        Object* o = find(id);
        ScriptEvent e;
        e.action = ScriptAction::SetVisible;
        e.node_id = id;
        e.name = o != nullptr ? o->name : std::string();
        e.on = false;
        out.push_back(e);
        if (o != nullptr) {
            o->visible = false;
            o->targetable = false;
        }
        if (Character* c = character(id)) avoidance_remove(*c);
        forget_character(id);
        dead_.insert(id);
    }
    return deleted;
}

// PORT: the machine's `Warper` nodes, for the renderer.
std::vector<LevelScript::WarperRecord> LevelScript::warpers() const {
    std::vector<WarperRecord> out;
    for (const Object& o : objects_) {
        if (o.type != type::kWarper) continue;
        WarperRecord w;
        w.node = o.id;
        w.position = o.position;
        w.forward = o.forward;
        w.dungeon = o.dungeon;
        w.warp_name = o.warp_name;
        w.level_delta = o.level_delta;
        w.level_absolute = o.level_absolute;
        out.push_back(std::move(w));
    }
    return out;
}

// PORT: the machine's nodes in the file's order.
std::vector<int64_t> LevelScript::node_order() const {
    std::vector<int64_t> out;
    out.reserve(objects_.size());
    for (const Object& o : objects_) out.push_back(o.id);
    return out;
}

// The axes of the node `o` hangs from: its parent's own, or the file's for a parent that reads
// none (a Group) and for a node of the file's top.
void LevelScript::parent_axes(const Object& o, const Object*& parent, Vec3& r, Vec3& u,
                              Vec3& f) const {
    parent = o.parent != 0 ? find(o.parent) : nullptr;
    const bool own = parent != nullptr && (parent->forward.x * parent->forward.x +
                                           parent->forward.y * parent->forward.y +
                                           parent->forward.z * parent->forward.z) > 0.0f;
    r = own ? parent->right : frame_.direction({1.0f, 0.0f, 0.0f});
    u = own ? parent->up : frame_.direction({0.0f, 1.0f, 0.0f});
    f = own ? parent->forward : frame_.direction({0.0f, 0.0f, 1.0f});
}

// `CPositionableObject::getPosition(false)` (r5 0): the node's own +0x70,
// relative to the node it hangs from (a chest in a Group at z 2.16 keeps 38.16 for the
// file's 40.32); a node of the file's top, relative to the file's frame.
Vec3 LevelScript::own_position(const Object& o) const {
    const Object* parent = nullptr;
    Vec3 r, u, f;
    parent_axes(o, parent, r, u, f);
    const Vec3 d = o.position - (parent != nullptr ? parent->position : frame_.origin);
    const auto dot = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    return {dot(d, r), dot(d, u), dot(d, f)};
}

// The level's place of a node whose own position (`own_position`) is `own`.
Vec3 LevelScript::position_from_own(const Object& o, const Vec3& own) const {
    const Object* parent = nullptr;
    Vec3 r, u, f;
    parent_axes(o, parent, r, u, f);
    return (parent != nullptr ? parent->position : frame_.origin) + r * own.x + u * own.y +
           f * own.z;
}

// PORT: the machine's nodes of a kind, for the renderer.
std::vector<LevelScript::NodeRecord> LevelScript::nodes_of_type(uint32_t node_type) const {
    std::vector<NodeRecord> out;
    for (const Object& o : objects_) {
        if (o.type != node_type) continue;
        NodeRecord n;
        n.node = o.id;
        n.unit = o.unit;
        n.name = o.name;
        n.position = o.position;
        n.local_position = own_position(o);
        n.parent = o.parent;
        n.enabled = o.enabled;
        if (o.type == type::kMarker) {
            n.kind = o.resource;
            n.radius = o.marker_radius;
            n.box_width = o.marker_width;
            n.box_length = o.marker_length;
            n.right = o.right;
            n.up = o.up;
            n.forward = o.forward;
        }
        out.push_back(std::move(n));
    }
    return out;
}

// INVENTED: on arrival every `Unit Trigger` within reach of the player (2.0,
// `trigger_update`'s) counts as already entered, so arriving fires none; the
// console's arrival is not read for this.
void LevelScript::arrive_at(const Vec3& player) {
    if (!proximity_unit_triggers_) return;
    for (Object& o : objects_) {
        if (o.type != type::kUnitTrigger) continue;
        // The reach `trigger_update` tests a Unit Trigger with.
        constexpr float kUnitTriggerReach = 2.0f;
        const Vec3 d = player - o.position;
        o.inside = d.x * d.x + d.y * d.y + d.z * d.z <= kUnitTriggerReach * kUnitTriggerReach;
    }
}

}  // namespace tl::core
