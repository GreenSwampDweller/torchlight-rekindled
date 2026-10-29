// The renderer's implementation: data tables, swings and damage, loot, breakables.
#include "core/item_data.h"
#include "core/unit_resource.h"
#include "formats/unit_data.h"
#include "render/renderer_impl.h"

namespace tl::render {

// `CCharacter::die`, after the death sound: the burst
// or the flight (0x8395xx).
bool Renderer::Impl::die_burst_or_flight(const tl::core::ScriptEvent& event, SceneUnit& unit,
                                         NodeMap& nodes) {
    tl::core::LevelScript* owner = script_owning(event.node_id);
    if (owner == nullptr) return false;
    const tl::core::UnitStats victim = stats_for_unit(event.unit);
    Ogre::Vector3 feet = Ogre::Vector3::ZERO;
    if (!unit_node_position(event.node_id, feet)) return false;
    // `getPosition(true)`: the centre, a radius above the feet.
    const Ogre::Vector3 centre = feet + Ogre::Vector3(0.0f, victim.radius, 0.0f);

    // The burst: not the player (`ISA 0x1c`) and not a champion (`+0x702`); a unit
    // without an effect manager holds no affix.
    const bool champion = victim.is_champion || spawn_marked_champion(event.node_id);
    if (!champion) {
        const tl::core::EffectManager* effects = owner->unit_effects(event.node_id);
        const bool frozen = effects != nullptr && effects->has_affix("DAMAGEEFFECTFROZEN");
        bool burst = false;
        if (frozen) {
            // Shattered: `ICEEXPLODE` (slot 9) at the centre, `CRYSTALEXPLODE`
            // for 0.2 seconds at the magnitude's default.
            start_character_particle(victim, tl::core::character_particle::kIceExplode, centre,
                                     character_ref(event.node_id, owner));
            start_death_shake("CRYSTALEXPLODE", 0.2f, 1.0f, from_ogre(centre));
            burst = true;
        } else if (event.burst) {
            // Exploded, only with `PARTICLE_EXPLODE` (slot 10) and blood shown; the
            // magnitude `randomBetweenVolatile(0.5, 0.75)` is drawn after the particle.
            const auto names = tl::core::character_particle_names(victim.particles);
            if (!names[tl::core::character_particle::kExplode].empty() &&
                game_settings().show_blood) {
                start_character_particle(victim, tl::core::character_particle::kExplode, centre,
                                         character_ref(event.node_id, owner));
                const float magnitude = tl::core::volatile_random().between(0.5f, 0.75f);
                start_death_shake("DEATHEXPLODE", 0.3f, magnitude, from_ogre(centre));
                burst = true;
            }
        }
        if (burst) {
            // `setVisible(false, true)`, `clearAnimations`: gone at once, no death animation.
            std::vector<tl::core::ScriptEvent> hidden;
            owner->set_unit_hidden(event.node_id, true, /*immediate=*/true, hidden);
            if (!hidden.empty()) apply_script_events(hidden, nodes);
            if (unit.playing_state != nullptr) unit.playing_state->setEnabled(false);
            unit.playing.clear();
            unit.playing_state = nullptr;
            TL_LOG_INFO("script: %s %s", event.name.c_str(), frozen ? "shatters" : "bursts");
            return true;
        }
    }

    // The flight: a blow with a direction and a force, and a model with a
    // `DEATH_FLIGHT` animation.
    if (!event.has_direction || !(0.0f < event.force)) return false;
    const float length = scene_animation_length(event.unit, "DEATH_FLIGHT");
    if (length < 0.0f) return false;
    tl::core::DeathFlight flight;
    if (!owner->start_death_flight(event.node_id, event.direction, event.force, &flight)) {
        return false;
    }
    // `blendAnimation("DEATH_FLIGHT", ...)`: the animation lasts as long as the flight.
    const float rate = length / (flight.path.length() / flight.speed);
    Ogre::AnimationState* started = nullptr;
    if (play_scene_animation(unit.entity, unit.mesh, "DEATH_FLIGHT", /*loops=*/false,
                             rate * unit.rate, /*optional=*/true, &started, unit.armed)) {
        remember_state(unit.entity, "DEATH_FLIGHT", started);
        unit.playing = "DEATH_FLIGHT";
        unit.playing_state = started;
        // `queueBlendAnimation("DEATH_LAND", false, 0, 1)` unless it falls.
        if (!flight.falls) {
            PendingIdle land;
            land.node = event.node_id;
            land.unit = event.unit;
            land.state = started;
            land.animation = "DEATH_LAND";
            land.loops = false;
            pending_idles.push_back(land);
        }
    }
    // The player's blow shakes the camera: `KILL`, 0.2 seconds at 0.5.
    if (event.on) start_death_shake("KILL", 0.2f, 0.5f, from_ogre(centre));
    const tl::core::Vec3 end = flight.path.point(2);
    TL_LOG_INFO("script: %s is flung %.2f to (%.2f, %.2f, %.2f)%s", event.name.c_str(),
                static_cast<double>(event.force), static_cast<double>(end.x),
                static_cast<double>(end.y), static_cast<double>(end.z),
                flight.falls ? ", into the void" : "");
    return true;
}

/// Start a swing, unless one is already running.
// PORT: drawing, how long an entity's animation of that name runs; -1 when it has none.
float Renderer::Impl::view_animation_length(Ogre::Entity* entity, const std::string& name) const {
    Ogre::AnimationState* state = nullptr;
    if (Ogre::AnimationStateSet* states = entity->getAllAnimationStates()) {
        for (const auto& entry : states->getAnimationStates()) {
            if (lowered(entry.first) == lowered(name)) state = entry.second;
        }
    }
    return state == nullptr ? -1.0f : state->getLength();
}

// PORT: drawing, a trail key of the player's own animation (keys 7 and 8).
void Renderer::Impl::player_weapon_trail_key(const tl::core::AnimationTag& tag) {
    weapon_trail_key(player_trails, subject, tag, player_trail_character());
}

// PORT: drawing, the player's ranged weapon's tracer.
void Renderer::Impl::player_shoot_tracer(const tl::core::Vec3& from, const tl::core::Vec3& to) {
    player_trails.trails.shoot_tracer(from, to, false);
}

// PORT: drawing, the player's hands' trails off.
void Renderer::Impl::player_hide_hand_trails() { player_trails.trails.hide_hands(); }

// PORT: drawing, a placed unit's fall in the animation its death chose; hidden without one.
bool Renderer::Impl::view_unit_falls(int64_t node, const std::string& animation) {
    bool fell = false;
    for (auto& entry : scene_units) {
        if (entry.second.node != node) continue;
        // INVENTED: with no machine's choice, `Die`, then `Die1`.
        const std::vector<std::string> stems = animation.empty()
                                                   ? std::vector<std::string>{"Die", "Die1"}
                                                   : std::vector<std::string>{animation};
        for (const std::string& stem : stems) {
            Ogre::AnimationState* started = nullptr;
            if (!play_scene_animation(entry.second.entity, entry.second.mesh, stem,
                                      /*loops=*/false, entry.second.rate,
                                      /*optional=*/true, &started, entry.second.armed)) {
                continue;
            }
            remember_state(entry.second.entity, stem, started);
            entry.second.playing = stem;
            entry.second.playing_state = started;
            entry.second.last_time = 0.0f;
            fell = true;
            break;
        }
        if (!fell) entry.second.entity->setVisible(false);
        break;
    }
    return fell;
}

// PORT: drawing, whether there is a scene to put things in.
bool Renderer::Impl::view_ready() const { return scene != nullptr; }

// PORT: drawing, an entity's shadow on or off.
void Renderer::Impl::view_cast_shadows(Ogre::Entity* entity, bool on) {
    entity->setCastShadows(on);
}

// PORT: a collision model put where a node stands, in a group of its own.
void Renderer::Impl::view_add_node_collision(Ogre::SceneNode* node, const std::string& path,
                                             int64_t group) {
    node->_update(true, false);
    add_collision_mesh(path, node->_getFullTransform(), group);
}

}  // namespace tl::render
