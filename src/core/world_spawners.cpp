// The level's `Unit Spawner`s (core/unit_spawner.h) as `CUnitSpawner` sees the level: where a
// release lands, what it makes, and the outputs back to the spawner's machine.
#include "core/world_spawners.h"

#include <cmath>

namespace tl::core {

namespace {

ogre::Quaternion quaternion_of(const tl::core::SpawnAxes& axes) {
    ogre::Quaternion q;
    q.FromAxes(ogre::Vector3(axes.x.x, axes.x.y, axes.x.z),
               ogre::Vector3(axes.y.x, axes.y.y, axes.y.z),
               ogre::Vector3(axes.z.x, axes.z.y, axes.z.z));
    return q;
}

}  // namespace

// PORT: `CLevel +0x1a8`, the level's monster level, for the spawner host.
int32_t World::SpawnerHost::level_monster_level() const { return world_.level_monster_level; }

tl::core::Vec3 World::SpawnerHost::random_open_position(const tl::core::Vec3& at, float radius,
                                                        bool avoid_no_spawn) {
    return tl::core::random_open_position(
        at, 0.0f, radius,
        [this, avoid_no_spawn](const tl::core::Vec3& p) {
            return !world_.passability.ready() ||
                   (world_.passability.map_passable(p) && world_.passability.position_passable(p) &&
                    !(avoid_no_spawn &&
                      tl::core::is_in_no_spawn_region(world_.no_spawn_regions, p, 0.0f)));
        },
        tl::core::volatile_random());
}

bool World::SpawnerHost::ray_collision(const tl::core::Vec3& from, const tl::core::Vec3& to,
                                       tl::core::Vec3& normal) {
    tl::core::CollisionHit hit;
    if (!world_.collision.ray_collision(from, to, /*objects=*/true, hit)) return false;
    normal = hit.normal;
    return true;
}

// `Ogre::Math::RangeRandom`: `rand()` scaled by 1/32767, one fused step.
float World::SpawnerHost::range_random(float lo, float hi) {
    const float drawn = static_cast<float>(tl::core::crt_random().next());
    return std::fma(drawn * (hi - lo), 3.0518509e-05f, lo);
}

tl::core::RunicRandom& World::SpawnerHost::volatile_random() { return tl::core::volatile_random(); }

void World::SpawnerHost::create_particles(const std::string& file, const tl::core::Vec3& at,
                                          const tl::core::SpawnAxes& axes, uint32_t index) {
    // `CUnitSpawner::createParticleSystemAt`: nothing when the snap misses.
    tl::core::Vec3 snapped = at;
    if (!world_.collision.snap_to_valid_ground(snapped)) return;
    world_.view_spawner_particles(node_, file, ogre::Vector3(snapped.x, snapped.y, snapped.z),
                                  quaternion_of(axes), index);
}

void World::SpawnerHost::fire_missile(const std::string& resource, const tl::core::Vec3& at,
                                      const tl::core::SpawnAxes& axes) {
    // `fireMissile` is handed the orientation; the missile flies along its z.
    world_.fire_spawner_missile(node_, resource, ogre::Vector3(at.x, at.y, at.z),
                                ogre::Vector3(axes.z.x, axes.z.y, axes.z.z));
}

// `CSpawnClassParser::getSpawnClassByName` and
// `CSpawnClass::rollSpawnClass` with `(level, 0xffffffff,
// 0, -1, 0, 0)` on `g_Rand`, as `CUnitSpawner::spawnUnitByIndex` rolls.
tl::core::SpawnClassPick World::SpawnerHost::roll_spawn_class(const std::string& name,
                                                              int32_t level) {
    if (!world_.loot_tables_built) {
        world_.loot_tables_built = true;
        world_.build_spawn_context(world_.loot_tables, /*items_by_type=*/true);
    }
    const tl::core::SpawnContext& context = world_.loot_tables.context;
    tl::core::SpawnClassPick pick;
    const tl::core::SpawnClass* table =
        context.get_spawn_class ? context.get_spawn_class(name) : nullptr;
    if (table == nullptr) return pick;
    pick.found_class = true;
    std::vector<std::string> units;
    std::vector<bool> enchant;
    table->roll_spawn_class(units, &enchant, nullptr, nullptr, level, 0xffffffffu, 0, -1, 0, 0,
                            tl::core::game_random(), context);
    if (!units.empty()) {
        pick.unit = units[0];
        pick.enchant = !enchant.empty() && enchant[0];
    }
    return pick;
}

tl::core::SpawnedUnitRef World::SpawnerHost::create_unit(const tl::core::SpawnRelease& release,
                                                         const std::string& name, bool enchant) {
    using tl::core::SpawnGroup;
    using tl::core::SpawnSettle;
    const ogre::Quaternion facing = quaternion_of(release.axes);
    const ogre::Vector3 at(release.position.x, release.position.y, release.position.z);
    const ogre::Vector3 spawner(release.spawner_position.x, release.spawner_position.y,
                                release.spawner_position.z);

    // `CResourceManager::createUnit`: an item by name for `Items`, or a table's pick.
    world_.ensure_item_index();
    const tl::core::ItemStats* data = nullptr;
    if (release.group == SpawnGroup::Items || release.group == SpawnGroup::SpawnClass) {
        data = world_.items.find(name);
    }
    if (data != nullptr) {
        // `createItem(data, false, true, false)`, then `enchant(true)` on the table's flag.
        const tl::core::ItemStats made = world_.create_item(*data, false, true, enchant);
        const size_t before = world_.dropped_items.size();
        if (made.is_gold()) {
            world_.place_gold(made.gold, at, &made, /*drop_sound=*/release.item_drop);
        } else if (release.item_drop) {
            // `addItem(item, p, true)`: `CEquipment::drop` tosses it from there.
            world_.drop_item(made, 1, at);
        } else {
            world_.place_ground_item(made, 1, at, facing, tl::core::QuestUnitTag{});
        }
        if (world_.dropped_items.size() == before) return {};
        WorldState::DroppedWorldItem& placed = world_.dropped_items.back();
        if (release.settle != SpawnSettle::AsAdded && placed.node != nullptr) {
            world_.view_set_position(placed.node,
                                     release.settle == SpawnSettle::AtSpawner ? spawner : at);
            world_.view_set_orientation(placed.node, facing);
            placed.rest = facing;
        }
        const int64_t id = world_.next_spawned_id();
        placed.unit_id = id;
        world_.spawner_units[id] = WorldState::SpawnerUnit{machine_, node_};
        TL_LOG_INFO("spawner: '%s' releases %s", release.resource.c_str(),
                    made.display_name.c_str());
        // Every item but gold is a `CEquipment`.
        return {tl::core::SpawnedKind::Item, id, !made.is_gold()};
    }
    if (release.group != SpawnGroup::Monsters && release.group != SpawnGroup::Props &&
        release.group != SpawnGroup::SpawnClass) {
        TL_LOG_WARN("spawner: no %s called '%s'", release.group_upper.c_str(), name.c_str());
        return {};
    }

    // A `CBreakable` is an item: `addItem(b, p, item drop)`,
    // which tosses only a `CEquipment`; then the spawner settles it.
    if (world_.is_breakable_unit(name)) {
        const int64_t id = world_.next_spawned_id();
        const ogre::Vector3* settle = release.settle == SpawnSettle::AtSpawner    ? &spawner
                                      : release.settle == SpawnSettle::AtPosition ? &at
                                                                                  : nullptr;
        if (!world_.place_breakable(name, at, facing, id, settle)) return {};
        world_.spawner_units[id] = WorldState::SpawnerUnit{machine_, node_};
        TL_LOG_INFO("spawner: '%s' releases %s", release.resource.c_str(), name.c_str());
        return {tl::core::SpawnedKind::Item, id, false};
    }

    // `CResourceManager::createUnit` makes the unit first - `unitInit` rolls its
    // scale, effects, level and default equipment on `g_RandVolatile` - and only then
    // `CLevel::addCharacter(c, p, false)` grounds it
    // (`CCharacter::dropToGround(4.0)`, whose search draws from the same
    // generator) and the spawner settles it. The order matters whenever the ground ray searches.
    world_.index_unit(name);  // the unit's `COLLISION_RADIUS` (`createUnit` has read it by now)
    const tl::core::LevelScript::UnitCreation creation = world_.script.roll_unit_creation(name);
    const auto settled = [&](const ogre::Vector3& grounded) {
        if (release.settle == SpawnSettle::AtPosition) return at;
        if (release.settle == SpawnSettle::AtSpawner) return spawner;
        return grounded;
    };

    const int level = release.level > 0 ? release.level : 0;
    if (world_.spawn_unit(name, at, node_, level, /*champion=*/false, release.play_spawn_anim,
                          facing) == nullptr) {
        const ogre::Vector3 place =
            settled(world_.character_to_ground(at, world_.stats_for_unit(name).radius));
        if (!world_.view_place_layout_bodied_unit(name, place)) {
            TL_LOG_WARN("spawner: '%s' resolved to no unit", name.c_str());
            return {};
        }
        const int64_t id = world_.next_spawned_id();
        world_.spawner_units[id] = WorldState::SpawnerUnit{machine_, node_};
        return {tl::core::SpawnedKind::Item, id, false};
    }
    // `CLevel::addCharacter`'s order: the unit at the drawn point
    // (`setPosition(p)`, its own effects already applied by `unitInit`), `CCharacter::createPathfinder`
    // (`MINION_SPAWNCLASS` brings its minions round that point, each made,
    // turned, searched, added, grounded and taken as a pet), then `dropToGround`; the
    // `TARGET PLAYER` hunt and the spawner's `LEVEL` follow in `spawnUnitByIndex`'s tail. Each
    // step draws from `g_RandVolatile`.
    const WorldState::SpawnedUnit made =
        world_.spawned[node_].back();  // a copy: the minions grow the list
    const ogre::Vector3 forward = facing * ogre::Vector3::UNIT_Z;
    world_.script.add_spawned_unit(made.id, name, tl::core::Vec3{at.x, at.y, at.z},
                                   /*hunts=*/false, std::atan2(forward.x, forward.z), &creation);
    world_.script.mark_dropped(made.id);
    world_.spawner_units[made.id] = WorldState::SpawnerUnit{machine_, node_};
    // `CSkillManager::fireSkillsOnCreate` from `CCharacter::unitInit`: the
    // unit's `EVENT_UNIT_CREATE` skills run as it is made - a running skill keeps the unit active
    // wherever it stands (`calculateActiveRange`), which `CCharacter::spawn`'s entrance below asks.
    // The unit is the level script's (`add_spawned_unit` above), not the spawner node's machine's.
    world_.fire_unit_skills_on_create(world_.script, made.id);
    const int minion_level = level > 0 ? level : world_.level_monster_level;
    world_.place_unit_minions_at(tl::core::Vec3{at.x, at.y, at.z}, node_,
                                 world_.roll_unit_minions(name, minion_level), made.id);
    const ogre::Vector3 place =
        settled(world_.character_to_ground(at, world_.stats_for_unit(name).radius));
    if (place != at) {
        world_.script.move_unit(made.id, tl::core::Vec3{place.x, place.y, place.z});
        world_.view_set_position(made.node, place);
    }
    // `CCharacter::spawn` (from `spawnUnitByIndex` +0x910, right after
    // `addCharacter`): `calculateActiveRange` first, and only for a visible unit in active range
    // the entrance - AI state 12, its animation, then `playSample(19)` on the unit's bank, its
    // `SPAWN_SOUND`, the slot's chance and the recording drawn on `g_RandVolatile` - before the hunt.
    std::vector<tl::core::ScriptEvent> range_events;
    const bool entrance =
        release.play_spawn_anim && world_.script.spawn_in_active_range(made.id, range_events);
    if (!range_events.empty()) {
        if (NodeMap* nodes = world_.nodes_of(machine_))
            world_.apply_script_events(range_events, *nodes);
    }
    if (entrance) {
        world_.play_bank_slot(world_.stats_for_unit(name), tl::core::SoundSlot::Spawn, place);
    }
    if (release.target_player) world_.script.hunt_player(made.id);
    world_.apply_spawn_level(made.id);
    if (!release.give_loot) world_.no_loot_units.insert(made.id);
    if (!release.give_xp) world_.no_xp_units.insert(made.id);
    // `DESTROY BODY` (`CUnitSpawner::spawnUnitByIndex`): `DESUMMON ON DEATH`
    // for 999999 seconds, value 1, dynamic, saved (+0x2b), before `addCharacter`.
    if (release.destroy_body) {
        if (tl::core::EffectManager* effects = world_.script.unit_effects(made.id)) {
            tl::core::Effect desummon =
                tl::core::value_effect(tl::core::kEffectTypeDesummonOnDeath, 999999.0f, 1.0f,
                                       tl::core::EffectActivation::Dynamic);
            desummon.save = true;
            effects->add_effect(desummon);
        }
    }
    // Without `PLAY SPAWN ANIM`, or out of active range, its AI state 4 is the machine's own
    // starting state.
    if (entrance && world_.view_begin_spawn_entrance(made.id, made.entity)) {
        world_.script.set_spawning(made.id, true);
    }
    world_.ensure_prop_index();
    const bool prop = world_.prop_files.count(lowered(name)) != 0;
    TL_LOG_INFO("spawner: '%s' releases %s at (%.2f, %.2f, %.2f)%s", release.resource.c_str(),
                name.c_str(), static_cast<double>(place.x), static_cast<double>(place.y),
                static_cast<double>(place.z), entrance ? " with its entrance" : "");
    return {prop ? tl::core::SpawnedKind::Item : tl::core::SpawnedKind::Character, made.id, false};
}

// The spawn listeners (`CUnitSpawner +0x218`): a skill event's (`CSkillEvent::addListeners`)
// is told of each character released
// (`unitSpawnedFromUnitSpawner`).
void World::SpawnerHost::unit_spawned(const tl::core::SpawnedUnitRef& unit) {
    // `create_unit` put it in the level's own machine.
    if (unit.kind != tl::core::SpawnedKind::Character) return;
    const auto found = world_.skill_spawners.find(node_);
    if (found == world_.skill_spawners.end()) return;
    if (world_.script.unit_of(unit.id).empty()) return;
    world_.skill_unit_spawned(found->second.source, world_.script, unit.id);
}

// A released unit still exists: the spawner's `TSafePointer` is not cleared.
// PORT: answered from the port's list of what the spawner made.
// `setResourceSpawn`: the data group's `SPAWNER` (default empty)
// against empty. The port's unit book is keyed by name, not by the spawner's group.
bool World::SpawnerHost::resource_names_spawner(const std::string& resource) {
    world_.index_unit(resource);
    return !world_.stats_for_unit(resource).spawner_model.empty();
}

bool World::SpawnerHost::unit_exists(int64_t id) const {
    const auto found = world_.spawned.find(node_);
    if (found == world_.spawned.end()) return false;
    for (const WorldState::SpawnedUnit& unit : found->second) {
        if (unit.id == id) return true;
    }
    return false;
}

bool World::SpawnerHost::character_alive(int64_t id) const {
    return !world_.script.unit_is_dead(id);
}

// `CCharacter::die` (vtable +0x330) with no one to blame, on a released unit
// (`CUnitSpawner::destroyUnits`).
void World::SpawnerHost::kill_character(int64_t id) {
    if (world_.script.unit_is_dead(id)) return;
    const auto found = world_.spawned.find(node_);
    if (found == world_.spawned.end()) return;
    for (const WorldState::SpawnedUnit& unit : found->second) {
        if (unit.id != id) continue;
        const std::string name = unit.unit;
        world_.kill_unit(id, name, /*fall=*/true, tl::core::LevelScript::kNoAttacker);
        return;
    }
}

void World::SpawnerHost::hide_character(int64_t id, bool hide) {
    // `setEnabled(false)`: the machine forgets it (no thinking, not hunted); still alive.
    const auto found = world_.spawned.find(node_);
    if (found == world_.spawned.end()) return;
    for (const WorldState::SpawnedUnit& unit : found->second) {
        if (unit.id != id) continue;
        if (unit.node != nullptr) world_.view_set_visible({unit.node}, !hide);
        if (hide) world_.script.remove_spawned_unit(id);
        return;
    }
}

void World::SpawnerHost::run_particles(bool run) {
    // `CLayout::stop(false)`: nothing more released, each layout goes once dead.
    // INVENTED: `CLayout::start` again is not modelled.
    if (run) return;
    const auto found = world_.spawner_particles.find(node_);
    if (found == world_.spawner_particles.end()) return;
    for (auto& layout : found->second) {
        world_.transient_particles.push_back(WorldState::TransientParticles{layout.second, 0.0f});
    }
    world_.spawner_particles.erase(found);
}

void World::SpawnerHost::deliver() {
    if (outputs_.empty() || machine_ == nullptr) return;
    std::vector<tl::core::ScriptEvent> events;
    for (uint32_t raised : outputs_) {
        TL_LOG_INFO("spawner: %lld raises '%s'", static_cast<long long>(node_),
                    tl::core::logic::output_name(raised));
        machine_->raise_output(node_, raised, world_.quest_log.state(), events);
    }
    outputs_.clear();
    if (events.empty()) return;
    if (NodeMap* nodes = world_.nodes_of(machine_)) world_.apply_script_events(events, *nodes);
}

// PORT: a layout input handed to the spawner node's `UnitSpawner::input`.
void World::spawner_input(const tl::core::ScriptEvent& event) {
    if (event.spawner == nullptr) return;
    SpawnerHost host(*this, event.machine, event.node_id);
    event.spawner->input(event.input, host);
    TL_LOG_INFO("script: '%s' %s (%s '%s'): %u of %u out", event.name.c_str(),
                tl::core::logic::input_name(event.input), event.spawner->spec().group.c_str(),
                event.spawner->spec().resource.c_str(), event.spawner->released(),
                event.spawner->total());
    host.deliver();
}

// Every spawner's `CUnitSpawner::update`, once a frame. INVENTED: all stepped
// here, not among the level's units in their order.
void World::step_spawners(float delta) {
    if (!(delta > 0.0f)) return;
    for (MachineView& view : machines()) {
        for (const auto& entry : view.script->unit_spawners()) {
            SpawnerHost host(*this, view.script, entry.first);
            entry.second->update(delta, host);
            host.deliver();
        }
    }
}

// `CGameClient::loadLevel` ends with `CLevel::updateLayouts`
// at 0.001 s: every descriptor once, spawners among them.
// INVENTED: the spawners are stepped after the machines' descriptors, as step_spawners does, and
// the client's `+0xfbc` update is not carried.
void World::settle_loaded_level() {
    constexpr float kLoadStep = 0.001f;
    const tl::core::Vec3 at{player.position().x, player.position().y, player.position().z};
    for (MachineView& view : machines()) {
        std::vector<tl::core::ScriptEvent> events;
        view.script->update_layouts(kLoadStep, at, quest_log.state(), events);
        if (!events.empty() && view.nodes != nullptr) apply_script_events(events, *view.nodes);
    }
    step_spawners(kLoadStep);
}

void World::spawned_item_state(int64_t id, int state) {
    const auto found = spawner_units.find(id);
    if (found == spawner_units.end()) return;
    const SpawnerUnit owner = found->second;
    spawner_units.erase(found);
    tl::core::UnitSpawner* spawner =
        owner.machine != nullptr ? owner.machine->unit_spawner(owner.spawner) : nullptr;
    if (spawner == nullptr) return;
    SpawnerHost host(*this, owner.machine, owner.spawner);
    spawner->unit_state_change(id, state, host);
    host.deliver();
}

}  // namespace tl::core
