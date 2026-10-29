// The gameplay half of the breakables: placing one, its avoidance on the map, breaking it.
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

void World::breakable_avoidance(BreakableWorld& b, bool add) {
    // `CBaseUnit::addToAvoidanceMap` / `removeFromAvoidanceMap`:
    // in a level, `OCCUPIESNODES`, not
    // takeable (ISA 0x1f) nor interactable (0x20); adding also wants `COLLIDEABLE` and
    // the model live. A breakable takes a circle of `+0x194 + 0.4`.
    if (!passability.ready() || b.node == nullptr) return;
    if (add) {
        if (b.state.on_grid || !b.state.collideable || !b.state.collision_live) return;
    } else if (!b.state.on_grid) {
        return;
    }
    const float r = tl::core::kBreakableRadius + 0.4f;
    const ogre::Vector3 p = view_position(b.node);
    const tl::core::Vec3 low{p.x + -r, p.y + 0.0f, p.z + -r};
    const tl::core::Vec3 high{p.x + r, p.y + 0.0f, p.z + r};
    if (add) {
        passability.increment_map_passability(low, high, r);
    } else {
        passability.decrement_map_passability(low, high, r);
    }
    TL_LOG_DEBUG("  map avoidance %c %s at (%a, %a) r %a", add ? '+' : '-', b.unit.c_str(),
                 static_cast<double>(p.x), static_cast<double>(p.z), static_cast<double>(r));
    b.state.on_grid = add;
}

bool World::place_breakable(const std::string& unit, const ogre::Vector3& at,
                            const ogre::Quaternion& facing, int64_t id, const ogre::Vector3* settle,
                            const float* drawn_scale) {
    if (!view_ready()) return false;
    ensure_prop_index();
    const auto found = prop_files.find(lowered(unit));
    if (found == prop_files.end()) return false;
    // The unit as the list holds it: the file over its `BASEFILE`
    // (`CUnitResourceList::parseChildFile`).
    const tl::formats::AdmFile file = read_unit_file(found->second);
    if (!file.ok) return false;

    BreakableWorld b;
    b.unit = unit;
    b.id = id;
    ensure_player_skill_files();
    b.data = tl::core::read_breakable(
        file, [this](const std::string& upper_name) -> std::optional<uint32_t> {
            const auto skill = player_skill_files.find(upper_name);
            if (skill == player_skill_files.end()) return std::nullopt;
            return static_cast<uint32_t>(skill->second->def.levels.size());
        });
    // `CBaseUnit::unitInit`, via `CItem::unitInit`: the
    // scale, drawn before `populate` turns it.
    const float scale = drawn_scale != nullptr
                            ? *drawn_scale
                            : tl::core::breakable_scale(b.data, tl::core::volatile_random());
    // Then its `[SKILL]`s, their effects rolled (the population's were, as it made them).
    if (drawn_scale == nullptr) {
        roll_prop_skills(unit);
        breakable_unit_loads(b.data);
    }

    // `createGenericModel` for the mesh; nothing is made without one.
    std::string mesh = b.data.mesh.empty() ? std::string() : resolve_archive_path(b.data.mesh);
    if (mesh.empty()) {
        TL_LOG_WARN("breakable '%s': no mesh '%s'", unit.c_str(), b.data.mesh.c_str());
        return false;
    }
    const size_t dot = mesh.find_last_of('.');
    if (dot != std::string::npos) mesh.resize(dot);
    tl::core::ItemStats model;
    model.mesh = mesh;
    model.display_name = unit;
    b.entity = create_item_entity(model);
    if (b.entity == nullptr) return false;
    view_cast_shadows(b.entity, b.data.shadows);
    tl::core::breakable_unit_init(b.state, b.data);

    // `CLevel::addItem`: the position, then
    // `setActiveInLevel(true)`: `snapToGround`, then
    // `addToAvoidanceMap` (vtable +0x270). No toss: a `CBreakable` is not a `CEquipment`.
    const ogre::Vector3 ground = snap_item_to_ground(at);
    b.node = view_place_entity(b.entity, ground, facing);
    view_set_scale(b.node, scale);
    // `addItem` sets the opacity to 1 (`updateOpacity(0, true)`); `+0x199` stays false
    // until the first `CItem::update` measures it.
    b.presence.opacity = 1.0f;
    {
        tl::core::ItemPresenceChange change;
        tl::core::update_item_opacity(b.presence, 0.0f, true, change);
        draw_item_presence(b.node, b.presence, change);
    }
    breakable_avoidance(b, true);
    // `spawnUnitByIndex` then moves it; the map grid keeps the cells it was
    // registered on, as the original's does.
    if (settle != nullptr) view_set_position(b.node, *settle);

    // `CMasterResourceManager::addCollisionModel(COLLISIONFILE)`, the model the
    // unit carries with it, in place where it ends up.
    const std::string collider = resolve_archive_path(b.data.collision);
    if (!collider.empty()) {
        view_add_node_collision(b.node, collider, id);
        collision.build();
        b.has_collision = collision.has_group(id);
        collision.set_group_blocks(id, b.data.block);
    }
    // As `unitInit` left it (live when `COLLIDEABLE`) until the first
    // `CBreakable::update` asks the range.
    if (b.has_collision) collision.set_group_enabled(id, b.state.collision_live);

    const ogre::Vector3 where = view_position(b.node);
    breakables.push_back(std::move(b));
    tl::core::Vec3 box_low{0.0f, 0.0f, 0.0f};
    tl::core::Vec3 box_high{0.0f, 0.0f, 0.0f};
    const bool boxed =
        breakables.back().has_collision && collision.group_bounds(id, box_low, box_high);
    TL_LOG_INFO("breakable: %s at (%.2f, %.2f, %.2f)%s, collision %.2f x %.2f x %.2f", unit.c_str(),
                static_cast<double>(where.x), static_cast<double>(where.y),
                static_cast<double>(where.z),
                breakables.back().has_collision ? "" : ", no collision model",
                static_cast<double>(boxed ? box_high.x - box_low.x : 0.0f),
                static_cast<double>(boxed ? box_high.y - box_low.y : 0.0f),
                static_cast<double>(boxed ? box_high.z - box_low.z : 0.0f));
    return true;
}

int World::breakable_index(int64_t id) const {
    for (size_t i = 0; i < breakables.size(); ++i) {
        if (breakables[i].id == id && breakables[i].node != nullptr) return static_cast<int>(i);
    }
    return -1;
}

bool World::break_breakable(size_t index, bool by_player) {
    if (index >= breakables.size() || breakables[index].node == nullptr) return false;
    const ogre::Vector3 position = view_position(breakables[index].node);
    const ogre::Quaternion facing = view_orientation(breakables[index].node);
    const ogre::Vector3 up = facing * ogre::Vector3::UNIT_Y;
    const bool have_breaker = by_player && subject_node != nullptr;
    const ogre::Vector3 breaker_at = have_breaker ? view_position(subject_node) : position;
    const tl::core::Vec3 breaker{breaker_at.x, breaker_at.y, breaker_at.z};

    // `CBreakable::interact(CCharacter*)`.
    BreakableWorld& b = breakables[index];
    const tl::core::BreakableInteraction out = tl::core::breakable_interact(
        b.state, /*in_level=*/true, {position.x, position.y, position.z}, {up.x, up.y, up.z},
        have_breaker ? &breaker : nullptr, static_cast<uint32_t>(b.data.skills.size()),
        tl::core::game_random());
    if (!out.broke) return false;
    // `+0x1f0` cleared, then `removeFromAvoidanceMap` (vtable +0x268).
    breakable_avoidance(b, false);

    // The skill, cast with the breakable's own collision out and put back.
    const std::string unit = b.unit;
    const int64_t id = b.id;
    const bool has_collision = b.has_collision;
    const tl::core::BreakableData data = b.data;
    if (out.skill >= 0 && static_cast<size_t>(out.skill) < data.skills.size()) {
        if (has_collision) collision.set_group_enabled(id, false);
        const std::string& name = data.skills[static_cast<size_t>(out.skill)];
        cast_prop_trap(unit, name, data.skill_levels[static_cast<size_t>(out.skill)],
                       ogre::Vector3(out.skill_at.x, out.skill_at.y, out.skill_at.z), facing,
                       have_breaker);
        if (has_collision) collision.set_group_enabled(id, true);
    }

    // The particle, `+0.1` along the up vector, started.
    if (!data.interact_particle.empty()) {
        std::string particle = preloaded_particle_file(data.interact_particle);
        if (particle.empty()) particle = data.interact_particle;
        std::vector<Ogre::SceneNode*> holders = place_particle_layout(
            particle, ogre::Vector3(out.particle_at.x, out.particle_at.y, out.particle_at.z),
            ogre::Quaternion::IDENTITY, nullptr);
        if (holders.empty()) {
            TL_LOG_WARN("breakable: %s has no particle '%s'", unit.c_str(),
                        data.interact_particle.c_str());
        } else {
            transient_particles.push_back(TransientParticles{std::move(holders), 0.0f});
        }
    }
    // Sample 0 of its own bank, `INTERACT_SOUND`.
    play_sound(data.interact_sound, position);

    // `CLevel::rollTreasure(breaker, null, position)`.
    if (!loot_tables_built) {
        loot_tables_built = true;
        build_spawn_context(loot_tables, /*items_by_type=*/true);
    }
    const tl::core::SpawnContext& spawn_context = loot_tables.context;
    tl::core::LootCharacter breaker_loot;
    const tl::core::LootCharacter* broken_by =
        loot_character(have_breaker ? 0 : tl::core::LevelScript::kNoAttacker, breaker_loot);
    const tl::core::LootPlacement place = loot_placement_at(position);
    const tl::core::SpawnClass* barrel =
        spawn_context.get_spawn_class ? spawn_context.get_spawn_class("BARREL_TREASURE") : nullptr;
    const tl::core::LootDrop drop = tl::core::roll_treasure(
        barrel, level_monster_level, broken_by, nullptr, tl::core::volatile_random(),
        tl::core::game_random(), spawn_context, loot_unit_maker(), &place);
    int gold = 0;
    for (int pile : drop.gold) gold += pile;

    // `setVisible(false, true)`, the collision model out, `+0x218`; found again, as the
    // skill's layout may have grown `breakables`.
    for (BreakableWorld& again : breakables) {
        if (again.id != id) continue;
        tl::core::ItemPresenceChange change;
        again.presence.used = again.state.used;
        tl::core::set_item_visible_now(again.presence, false, change);
        draw_item_presence(again.node, again.presence, change);
        if (again.has_collision) collision.set_group_enabled(id, again.state.collision_live);
    }
    TL_LOG_INFO("breakable: %s broken%s, %zu item(s) and %zu pile(s) of gold (%d)", unit.c_str(),
                have_breaker ? " by the player" : "", drop.items.size(), drop.gold.size(), gold);

    // `CItem::interact`: `questEventFire(2)` (the player
    // only), then, for a character, unit state 2 to its spawner.
    {
        tl::core::QuestEventUnit broken =
            quest_event_unit(unit, stats_for_unit(unit).unittype, position, {});
        quest_event(quest_log.unit_interacted(broken, have_breaker));
    }
    if (have_breaker) spawned_item_state(id, 2);
    return true;
}

// `CBreakable::unitInit`, after `CItem::unitInit`: an
// `INTERACT_PARTICLE` longer than 8 with a path separator in it is loaded through the preloader
// (`CParticlePreloader::LoadParticle`), then `INTERACT_SOUND` is added as sample 0
// (`CSoundBank::addSample`, `g_Rand` left at its low word).
void World::breakable_unit_loads(const tl::core::BreakableData& data) {
    const std::string& particle = data.interact_particle;
    if (particle.size() > 8 && particle.find_first_of("/\\") != std::string::npos) {
        const std::string path = layout_archive_path(particle);
        if (!path.empty()) load_particle_file(path);
    }
    ensure_sound_index();
    if (!data.interact_sound.empty() && sound_info.find(data.interact_sound) != nullptr) {
        tl::core::game_random().set_seed(
            static_cast<int32_t>(static_cast<uint32_t>(tl::core::game_random().state())));
    }
}

void World::remove_breakable(BreakableWorld& b) {
    // `CCharacter::notifyOfDeletion(CItem*)`: a character whose
    // target item it was lets it go.
    if (player_skill_item == b.id) player_skill_item = 0;
    breakable_avoidance(b, false);
    if (b.has_collision) collision.set_group_enabled(b.id, false);
    if (b.node != nullptr) view_destroy_node(b.node);
    if (b.entity != nullptr) view_destroy_entity(b.entity);
    b.node = nullptr;
    b.entity = nullptr;
}

}  // namespace tl::core
