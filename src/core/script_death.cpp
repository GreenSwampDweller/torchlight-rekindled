// A level character's death: `CCharacter::die`, its death animation and sample, the pets it
// takes with it. Split from script_units.cpp.
#include "core/script.h"

#include "core/character.h"
#include "core/death.h"
#include "core/hp_events.h"
#include "core/item_drop.h"
#include "core/log.h"

#include <string>
#include <vector>

namespace tl::core {
namespace {

using namespace tl::core::logic;

}  // namespace

// `CCharacter::die` for a level unit: its path
// and its place on the avoidance grid let go, the swing ended, then the Kill
// event the renderer plays and `unit_killed`.
void LevelScript::kill_character(Object& o, Character& c, QuestState& quests,
                                 std::vector<ScriptEvent>& out, const KillingBlow* blow, bool burst,
                                 bool by_player) {
    stop_pathing(c);
    avoidance_remove(c);
    c.swing_left = 0.0f;
    ScriptEvent e;
    e.action = ScriptAction::Kill;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    if (blow != nullptr && blow->has_direction) {
        e.has_direction = true;
        e.direction = blow->direction;
        e.force = blow->force;
    }
    c.has_killing_blow = e.has_direction;
    c.killing_direction = e.direction;
    c.killing_force = e.force;
    c.killed_by_player = by_player;
    e.burst = burst;
    e.on = by_player;
    if (burst) burst_deaths_.insert(o.id);
    out.push_back(e);
    if (!defer_death_sample_) {
        play_unit_sample(o, static_cast<int>(SoundSlot::Death), out);
        death_sampled_.insert(o.id);
    }
    unit_killed(o.id, quests, out);
}

bool LevelScript::death_would_fly(int64_t id) const {
    const Object* o = find(id);
    const Character* c = character(id);
    return o != nullptr && c != nullptr && c->has_killing_blow && 0.0f < c->killing_force &&
           unit_animation_exists(o->unit, "DEATH_FLIGHT");
}

bool LevelScript::fly_killed_unit(int64_t id, std::vector<ScriptEvent>& out) {
    if (!death_would_fly(id)) return false;
    Character& c = *character(id);
    DeathFlight flight;
    if (!start_death_flight(id, c.killing_direction, c.killing_force, &flight)) return false;
    const Object& o = *find(id);
    ScriptEvent e;
    e.action = ScriptAction::DeathFlight;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.position = flight.path.point(2);
    e.rate = flight.path.length() / flight.speed;
    e.loops = flight.falls;
    e.on = c.killed_by_player;
    e.force = c.killing_force;
    out.push_back(e);
    return true;
}

std::string LevelScript::death_animation(int64_t id) {
    const Object* o = find(id);
    if (o == nullptr) return std::string();
    std::string name;
    float length = 0.0f;
    find_random_animation(o->unit, "DIE", name, length);
    if (Character* c = character(id)) c->dying_left = name.empty() ? 0.0f : length;
    return name;
}

void LevelScript::play_death_sample(int64_t id, std::vector<ScriptEvent>& out) {
    if (death_sampled_.erase(id) != 0) return;
    if (const Object* o = find(id)) play_unit_sample(*o, static_cast<int>(SoundSlot::Death), out);
}

void LevelScript::play_hit_sample(int64_t id, int damage_type, std::vector<ScriptEvent>& out) {
    SoundSlot slot = SoundSlot::PhysicalHit;
    switch (damage_type) {
        case 2: slot = SoundSlot::FireHit; break;
        case 3: slot = SoundSlot::IceHit; break;
        case 4: slot = SoundSlot::ElectricHit; break;
        case 5: slot = SoundSlot::PoisonHit; break;
        default: break;
    }
    if (const Object* o = find(id)) play_unit_sample(*o, static_cast<int>(slot), out);
}

// `CCharacter::modifyHP` by `-HP`, then
// `CCharacter::die` with no attacker, direction or force.
void LevelScript::die_now(Object& o, Character& c, QuestState& quests,
                          std::vector<ScriptEvent>& out) {
    // `modifyHP(-HP)`, with `modifyHP`'s HP-threshold broadcast under the same
    // gates as in `damage_unit`, then `die` with no attacker, direction or force.
    const float before = c.combat.hp;
    const float delta = -before;
    if (delta < 0.0f && !o.spawned) {
        std::vector<uint32_t> fired;
        broadcast_unit_hp_events(static_cast<float>(c.combat.base_max_hp), before, c.min_hp, delta,
                                 o.fire_all_hp_events, fired);
        for (uint32_t ev : fired) {
            broadcast(o.id, ev, quests, out);
        }
        if (0.0f < c.min_hp) c.min_hp = 0.0f;
    }
    kill_character(o, c, quests, out);
}

// A layout's `KILL PETS` on a unit: each of its pets `CCharacter::modifyHP`
// by the master's `maxHP()`, then `die`.
void LevelScript::kill_pets(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    const Character* master = character(o.id);
    if (master == nullptr) return;
    // The master's list (`+0x648`), oldest first.
    std::vector<UnitRef> list;
    for (LevelScript* machine : level_machines()) {
        for (int64_t id : machine->pets_of(this, o.id)) list.push_back(UnitRef{machine, id});
    }
    // Each is `modifyHP(+maxHP())` - the master's maximum, not its own - then
    // `die` (vtable +0x330, no attacker). Quirk kept: `die` takes the pet out
    // of the very list being walked (`removePet` at its start) and the index
    // still steps on, so every other pet is passed over and lives.
    const float heal = static_cast<float>(character_max_hp(*master));
    for (size_t i = 0; i < list.size(); ++i) {
        LevelScript& runs = *list[i].machine;
        Object* po = runs.find(list[i].node);
        Character* pc = runs.character(list[i].node);
        if (po != nullptr && pc != nullptr && runs.dead_.count(po->id) == 0) {
            const float most = static_cast<float>(runs.character_max_hp(*pc));
            pc->combat.hp = std::min(pc->combat.hp + heal, most);
            runs.die_now(*po, *pc, quests, sink_for(list[i].machine, out));
        }
        list.erase(list.begin() + static_cast<long>(i));
    }
}

}  // namespace tl::core
