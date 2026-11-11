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
            start_slot_particle(victim, tl::core::character_particle::kIceExplode,
                                from_ogre(centre), character_ref(event.node_id, owner));
            start_death_shake("CRYSTALEXPLODE", 0.2f, 1.0f, from_ogre(centre));
            burst = true;
        } else if (event.burst && bursts_on_death(*owner, event.node_id, event.unit)) {
            // Exploded: the particle, the shake and the unit hidden are `World::kill_unit`'s,
            // after the death's procs and sample. Here only `clearAnimations`.
            if (unit.playing_state != nullptr) unit.playing_state->setEnabled(false);
            unit.playing.clear();
            unit.playing_state = nullptr;
            TL_LOG_INFO("script: %s bursts", event.name.c_str());
            return true;
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

    // The flight is laid by `World::kill_unit` at its point in `die`, and played on its
    // `DeathFlight` event (`play_death_flight`).
    return owner->death_would_fly(event.node_id);
}

// PORT: drawing, `die`'s `blendAnimation("DEATH_FLIGHT", ...)` for a flight the level laid.
void Renderer::Impl::play_death_flight(const tl::core::ScriptEvent& event, SceneUnit& unit) {
    Ogre::Vector3 feet = Ogre::Vector3::ZERO;
    if (!unit_node_position(event.node_id, feet)) return;
    const Ogre::Vector3 centre =
        feet + Ogre::Vector3(0.0f, stats_for_unit(event.unit).radius, 0.0f);
    const float length = scene_animation_length(event.unit, "DEATH_FLIGHT");
    if (length < 0.0f || !(0.0f < event.rate)) return;
    // The animation lasts as long as the flight.
    const float rate = length / event.rate;
    Ogre::AnimationState* started = nullptr;
    if (play_scene_animation(unit.entity, unit.mesh, "DEATH_FLIGHT", /*loops=*/false,
                             rate * unit.rate, /*optional=*/true, &started, unit.armed)) {
        remember_state(unit.entity, "DEATH_FLIGHT", started);
        unit.playing = "DEATH_FLIGHT";
        unit.playing_state = started;
        // `queueBlendAnimation("DEATH_LAND", false, 0, 1)` unless it falls.
        if (!event.loops) {
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
    const tl::core::Vec3 end = event.position;
    TL_LOG_INFO("script: %s is flung %.2f to (%.2f, %.2f, %.2f)%s", event.name.c_str(),
                static_cast<double>(event.force), static_cast<double>(end.x),
                static_cast<double>(end.y), static_cast<double>(end.z),
                event.loops ? ", into the void" : "");
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

// `rollAttack`'s `interrupt(target, true)` comes first.
void Renderer::Impl::strike_interrupt(const tl::core::ScriptEvent& event) {
    if (event.interrupts) {
        if (event.target_node == 0) {
            interrupt_player(true);
        } else if (tl::core::LevelScript* of = event.target_machine != nullptr
                                                   ? event.target_machine
                                                   : script_owning(event.target_node)) {
            std::vector<tl::core::ScriptEvent> events;
            of->interrupt_unit(event.target_node, quest_log.state(), events);
            if (NodeMap* nodes = nodes_of(of); nodes != nullptr && !events.empty()) {
                apply_script_events(events, *nodes);
            }
        }
    }
}

// `rollAttack`: `applyDamageEffects` on whoever was struck,
// for any damage, ahead of the transfers; at the attacker's level.
void Renderer::Impl::strike_damage_effects(const tl::core::ScriptEvent& event) {
    if (!event.blocked && 0.0f < event.amount) {
        int level = 1;
        tl::core::LevelScript* by = script_owning(event.node_id);
        if (by != nullptr) by->unit_level(event.node_id, level);
        tl::core::LevelScript* struck = nullptr;
        if (event.target_node != 0) {
            struck = event.target_machine != nullptr ? event.target_machine
                                                     : script_owning(event.target_node);
        }
        if (event.target_node == 0 || struck != nullptr) {
            apply_damage_status(struck, event.target_node, event.amount, event.by_type, event.parts,
                                event.first_type, event.critical, level,
                                tl::core::EffectSource{by != nullptr, by, event.node_id});
        }
    }
}

// `applyDamage`: "Glancing Blow!" over a
// struck one that is not the player's (`+0x5a4`); never for 0 damage.
void Renderer::Impl::strike_glancing(const tl::core::ScriptEvent& event) {
    tl::core::LevelScript* struck_by =
        event.target_machine != nullptr ? event.target_machine : script_owning(event.target_node);
    if (event.glancing && event.amount != 0.0f && struck_by != nullptr &&
        !struck_by->unit_is_player_pet(event.target_node)) {
        Ogre::Vector3 over = Ogre::Vector3::ZERO;
        if (unit_node_position(event.target_node, over)) {
            add_combat_text(tl::core::CombatText::GlancingBlow, over);
        }
    }
}

}  // namespace tl::render
