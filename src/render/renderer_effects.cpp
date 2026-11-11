// The renderer's implementation: floaties, selection, skills and effects, missiles,
// spawning.
#include "render/renderer_impl.h"
#include "render/unit_opacity.h"

namespace tl::render {
namespace {
/// The voice key of the player's `LOWMANA_SOUND`, which `bankPlaying` asks after.
const std::string kLowManaVoice = "player/lowmana";
}  // namespace

/// One frame of a `FLOATYTEXT` line, over the unit's own position plus `kFloatyHeight`
/// (as `CQuestDialogMenu::setNPC`); off screen or behind the camera it is hidden.
// `CGameUI::addTextEvent`.
void Renderer::Impl::add_text_event(const tl::core::TextEvent& event) {
    if (!tl::core::text_events_shown(game_settings().floaty_numbers)) return;
    // `Ogre::Frustum::isVisible(Sphere(position, 0.5))` on the game camera.
    //
    if (camera != nullptr &&
        !camera->isVisible(
            Ogre::Sphere(Ogre::Vector3(event.x, event.y, event.z), tl::core::kTextEventRadius))) {
        return;
    }
    TL_LOG_INFO("text event: '%s' at (%.2f, %.2f, %.2f)", event.text.c_str(),
                static_cast<double>(event.x), static_cast<double>(event.y),
                static_cast<double>(event.z));
    text_events.push_back(event);
}

// `CCharacter::loadModel` makes the slot's `CParticle` once; each use moves it and
// calls `CParticle::Start`, which restarts the cache it holds.
// Placed on first use, and again once `Stop(true)` has taken the last one down.
void Renderer::Impl::start_character_particle(const tl::core::UnitStats& unit, int slot,
                                              const Ogre::Vector3& at, CharacterRef who) {
    if (slot < 0 || tl::core::character_particle::kCount <= slot) return;
    const std::string name =
        tl::core::character_particle_names(unit.particles)[static_cast<size_t>(slot)];
    if (name.empty()) return;
    // Its sounds were `World::start_slot_particle`'s.
    const bool was = slot_sounds_played;
    slot_sounds_played = true;
    struct Restore {
        bool& flag;
        bool was;
        ~Restore() { flag = was; }
    } restore{slot_sounds_played, was};
    // `CResourceManager::createParticle`: the name's file
    // in `media/particles/preload.dat`; an unlisted name makes nothing.
    const std::string file = preloaded_particle_file(name);
    if (file.empty()) return;
    SlotParticle& held = slot_particles[{who, slot}];
    if (!held.holders.empty() && held.file == file) {
        const Ogre::Vector3 moved = at - to_ogre(held.at);
        held.at = from_ogre(at);
        uint32_t cparticle = 0;
        for (Ogre::SceneNode* holder : held.holders) holder->translate(moved);
        for (ParticleSystem& system : holder_systems(held.holders)) {
            restart_particle_system(system);
            if (cparticle == 0) cparticle = system.cparticle;
        }
        if (cparticle != 0) cparticle_started(cparticle);
        TL_LOG_INFO("character particle: %s (slot %d) again at (%.2f, %.2f, %.2f)", name.c_str(),
                    slot, static_cast<double>(at.x), static_cast<double>(at.y),
                    static_cast<double>(at.z));
        return;
    }
    std::vector<Ogre::SceneNode*> holders =
        place_particle_layout(file, at, Ogre::Quaternion::IDENTITY, nullptr, ParticleStart::Cached);
    if (holders.empty()) {
        slot_particles.erase({who, slot});
        return;
    }
    TL_LOG_INFO("character particle: %s (slot %d) at (%.2f, %.2f, %.2f)", name.c_str(), slot,
                static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z));
    // A slot whose file changed (a transform) keeps its old layout to its timer, unheld.
    held.holders = holders;
    held.at = from_ogre(at);
    held.file = file;
    TransientParticles transient;
    transient.holders = std::move(holders);
    transient.self_timed = true;
    transient_particles.push_back(std::move(transient));
}

void Renderer::Impl::forget_slot_particles(const std::set<const Ogre::SceneNode*>& going) {
    for (auto it = slot_particles.begin(); it != slot_particles.end();) {
        const std::vector<Ogre::SceneNode*>& holders = it->second.holders;
        const bool gone =
            std::any_of(holders.begin(), holders.end(),
                        [&going](const Ogre::SceneNode* h) { return going.count(h); });
        it = gone ? slot_particles.erase(it) : std::next(it);
    }
}

// `CCharacter::updateEffects`, `fxShouldPlay` / `playFX`:
// each held effect's particle, made once 0.1 above the centre.
void Renderer::Impl::play_effect_fx(tl::core::EffectManager& effects, const Ogre::Vector3& centre,
                                    const std::string& who) {
    for (const std::string& name : effects.take_effect_fx()) {
        // `CResourceManager::createParticle`; an unlisted
        // name makes nothing. DIVERGENCE: asked once, not every update; nothing shows either way.
        const std::string file = preloaded_particle_file(name);
        if (file.empty()) continue;
        const Ogre::Vector3 at = centre + Ogre::Vector3::UNIT_Y * 0.1f;
        std::vector<Ogre::SceneNode*> holders = place_particle_layout(
            file, at, Ogre::Quaternion::IDENTITY, nullptr, ParticleStart::Cached);
        if (holders.empty()) continue;
        TL_LOG_INFO("effect particle: %s on %s at (%.2f, %.2f, %.2f)", name.c_str(), who.c_str(),
                    static_cast<double>(at.x), static_cast<double>(at.y),
                    static_cast<double>(at.z));
        TransientParticles transient;
        transient.holders = std::move(holders);
        transient.self_timed = true;
        transient_particles.push_back(std::move(transient));
    }
}

/// Advance and render all floating text items: dialog lines and combat events.
void Renderer::Impl::step_floaties(float delta) {
    std::vector<FloatyItem> items;

    // A floaty talk line: `CQuestDialogMenu::update`
    // closes the menu with `OK` on its next pass.
    if (talk_floaty_close.pending) {
        if (talk_floaty_close.fresh) {
            talk_floaty_close.fresh = false;
        } else {
            const TalkFloatyClose done = talk_floaty_close;
            talk_floaty_close = TalkFloatyClose{};
            talk_menu_closed(done.machine, done.node, 2, done.complete, done.quest);
        }
    }

    // 1. Dialog floaty line (if active)
    if (!floaty_line.empty()) {
        floaty_remaining -= delta;
        if (floaty_remaining <= 0.0f) {
            floaty_line.clear();
            floaty_unit.clear();
            floaty_remaining = 0.0f;
        } else {
            const std::string want = lowered(floaty_unit);
            const Ogre::Entity* speaker = nullptr;
            auto consider = [&](const Ogre::Entity* entity, const std::string& unit) {
                if (speaker != nullptr || entity == nullptr) return;
                if (lowered(unit).find(want) == std::string::npos) return;
                speaker = entity;
            };
            for (const auto& entry : scene_units) consider(entry.second.entity, entry.first);
            for (const auto& owner : spawned) {
                for (const SpawnedUnit& made : owner.second) consider(made.entity, made.unit);
            }
            const Ogre::SceneNode* node =
                speaker != nullptr ? speaker->getParentSceneNode() : nullptr;
            if (node != nullptr && camera != nullptr) {
                const Ogre::Vector3 head = node->_getDerivedPosition() +
                                           Ogre::Vector3(0.0f, tl::core::kFloatyHeight, 0.0f);
                const Ogre::Vector3 view = camera->getViewMatrix() * head;
                if (view.z < 0.0f) {
                    const Ogre::Vector3 clip = camera->getProjectionMatrix() * view;
                    const float at_x = clip.x * 0.5f + 0.5f;
                    const float at_y = -clip.y * 0.5f + 0.5f;
                    const float alpha = floaty_remaining / tl::core::kFloatySeconds;
                    FloatyItem item;
                    item.text = floaty_line;
                    item.x = at_x;
                    item.y = at_y;
                    item.alpha = alpha > 1.0f ? 1.0f : (alpha < 0.0f ? 0.0f : alpha);
                    item.colour = Ogre::ColourValue::White;
                    item.char_height = 0.030f * 1.25f;
                    items.push_back(item);
                }
            }
        }
    }

    // 2. `CGameUI::updateTextEvents`: spent events go back
    // to the pool.
    for (auto it = text_events.begin(); it != text_events.end();) {
        if (!tl::core::update_text_event(*it, delta)) {
            it = text_events.erase(it);
            continue;
        }
        if (camera != nullptr) {
            const Ogre::Vector3 view = camera->getViewMatrix() * Ogre::Vector3(it->x, it->y, it->z);
            if (view.z < 0.0f) {
                const Ogre::Vector3 clip = camera->getProjectionMatrix() * view;
                FloatyItem item;
                item.text = it->text;
                item.x = clip.x * 0.5f + 0.5f;
                item.y = -clip.y * 0.5f + 0.5f;
                item.alpha = it->opacity;
                item.colour = Ogre::ColourValue(it->red, it->green, it->blue, 1.0f);
                item.char_height = 0.030f * it->scale;
                items.push_back(item);
            }
        }
        ++it;
    }

    hud.set_floaties(items);
}

// CGameClient's per-frame target pick. Candidates:
// visible units on nodes the script still calls targetable; the rest is `core/targeting.h`.
bool Renderer::Impl::selected_unit(std::string& unit, bool& hostile, int64_t& node,
                                   Ogre::Entity*& picked_entity) {
    load_unit_types();
    std::vector<tl::core::TargetCandidate> candidates;
    struct Identity {
        const std::string* name;
        int64_t node;
        bool hostile;
        Ogre::Entity* entity;
    };
    std::vector<Identity> identities;

    const tl::core::Vec3 from = player.position();
    const float sight = tl::core::kTargetSightHeight;

    // `max(7.0, weapon range + 2.0)`.
    const float weapon_range =
        player_stats.weapon_attack.present ? player_stats.weapon_attack.attack.range : 0.0f;
    const float range = tl::core::target_range(weapon_range);
    // Nothing beyond this can be chosen, so no unit beyond it gets a line-of-sight ray
    // (the per-frame cost on a crowded floor).
    const float reach = std::max(range, tl::core::kTargetFallbackRadius);

    auto consider = [&](const Ogre::Entity* entity, const std::string& name, int64_t node_id) {
        if (entity == nullptr || !entity->isVisible() || name.empty()) return;
        // Asked of the machine that runs it (a room's boss is the room's).
        if (!node_targetable(node_id, entity->getParentSceneNode())) return;
        // Never a character whose master is the player (checks `+0x5A4`).
        if (node_is_player_pet(node_id)) return;
        const Ogre::SceneNode* at = entity->getParentSceneNode();
        if (at == nullptr) return;
        const Ogre::Vector3 where = at->_getDerivedPosition();
        const float ox = where.x - from.x;
        const float oz = where.z - from.z;
        if (ox * ox + oz * oz > (reach + 0.01f) * (reach + 0.01f)) return;

        tl::core::TargetCandidate candidate;
        candidate.x = where.x;
        candidate.y = where.y;
        candidate.z = where.z;
        // Not in the book: no readable `.DAT`, so no enemy.
        const tl::core::UnitStats* stats = unit_book.find(name);
        candidate.hostile = stats != nullptr && unit_hostile(node_id, *stats);

        // INVENTED: which harmless units may be pointed at; the original's team and type test
        // ('s, with 5) is not decoded. Someone to talk to counts,
        // a companion does not.
        if (!candidate.hostile) {
            namespace ut = tl::core::unit_type;
            const uint32_t type = unit_types.id_of(stats != nullptr ? stats->unittype : "");
            if (!unit_types.isa(type, ut::kInteractiveCharacter)) return;
        }
        // The ray has both ends lifted, over the floor rather than along it.
        tl::core::CollisionHit hit;
        candidate.in_sight =
            collision.empty() ||
            !collision.ray_collision(tl::core::Vec3{from.x, from.y + sight, from.z},
                                     tl::core::Vec3{where.x, where.y + sight, where.z}, true, hit);
        candidate.id = identities.size();
        candidates.push_back(candidate);
        identities.push_back(
            {&name, node_id, candidate.hostile, const_cast<Ogre::Entity*>(entity)});
    };

    for (const auto& entry : scene_units) {
        consider(entry.second.entity, entry.first, entry.second.node);
    }
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) consider(made.entity, made.unit, made.id);
    }
    if (candidates.empty()) return false;

    std::uint64_t chosen = 0;
    if (!tl::core::select_target(candidates.data(), candidates.size(), from.x, from.z,
                                 player.facing(), range, &chosen)) {
        return false;
    }
    const Identity& picked = identities[static_cast<std::size_t>(chosen)];
    unit = *picked.name;
    hostile = picked.hostile;
    node = picked.node;
    picked_entity = picked.entity;
    return true;
}

// Run the console's target pick for this frame and say what it chose; apart from
// drive_player() so headless runs (`--pad`) make the same decision.
bool Renderer::Impl::update_pad_selection(bool active) {
    std::string selected;
    bool hostile = false;
    int64_t node = 0;
    Ogre::Entity* entity = nullptr;
    const bool have = active && !script_dialog_open() && !script_player_disabled &&
                      selected_unit(selected, hostile, node, entity);

    // Light it up, and put the last one back (`CGameClient::update`,
    // `setHighlighted` on the pick and the dropped one).
    if (entity != pad_highlighted) {
        if (pad_highlighted != nullptr && entity_still_placed(pad_highlighted)) {
            highlight_entity(pad_highlighted, false, pad_highlighted_hostile, rim_light_texture);
        }
        // The red glow for a hostile one, a mimic's blue (outline_hostile()).
        const tl::core::UnitStats* picked = have ? unit_book.find(selected) : nullptr;
        const bool red = hostile && (picked == nullptr || outline_hostile(*picked));
        if (entity != nullptr) highlight_entity(entity, true, red, rim_light_texture);
        pad_highlighted = entity;
        pad_highlighted_hostile = red;
    }

    // Only when it changes, to keep the log to the decisions.
    if (selected != pad_selected) {
        if (have) {
            TL_LOG_INFO("pad target: '%s'%s", selected.c_str(), hostile ? " (hostile)" : "");
        } else if (!pad_selected.empty()) {
            TL_LOG_INFO("pad target: none");
        }
    }
    pad_selected = selected;
    pad_selected_hostile = hostile;
    pad_selected_node = node;
    return have;
}

// PORT: the inventory menu reads the skill texts' context the game half rebuilt.
void Renderer::Impl::view_skill_effects_changed() {
    inventory_menu.set_skill_effects(&skill_effects);
}

void Renderer::Impl::step_player_camera_opacity(float delta) {
    // `CGameClient::updateIngame`: `updateCameraOpacity` with the frame's
    // delta, or 0 when paused.
    if (subject_node == nullptr || camera == nullptr) return;
    const Ogre::Vector3 at = subject_node->_getDerivedPosition();
    const Ogre::Vector3 eye = camera->getDerivedPosition();
    const tl::core::OpacityStep step =
        tl::core::update_camera_opacity(player_opacity, eye.x - at.x, eye.y - at.y, eye.z - at.z,
                                        /*want=*/true, /*in_near_range=*/true, /*visible=*/true,
                                        /*death_flight=*/false, 0.0f, delta);
    player_opacity = step.opacity;
    if (step.written) tl::render::set_node_opacity(subject_node, player_opacity);
}

void Renderer::Impl::update_player_effects(float dt) {
    // A fresh or loaded inventory has not been handed its tables yet.
    if (player_inventory.effect_context().types == nullptr) refresh_inventory_effects();
    // `CCharacter::update`: `updateEffects`, then
    // `calculateEffectValues`, `deleteDeadEffects`; `updateAffixes`
    // from `CBaseUnit::update`. All take dt.
    if (dt <= 0.0f) return;
    player_effects.update_effects(dt);
    // `updateEffects`' `MISSILE REFLECT VISUAL` (0x3d) case: the reflector
    // strikes back with the holder's weapon (`+0x720` = share). INVENTED: the counter-blow
    // is not struck. Knock-back on the player: the push held to 10.
    player.set_push(tl::core::bound_outside_push(
        knock_back_push(player_effects, player_total().effect_value(0x35, 7), dt, "the player"),
        true));
    if (subject_node != nullptr) {
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        play_effect_fx(player_effects, subject_node->getPosition() + Ogre::Vector3::UNIT_Y * body,
                       "the player");
    }
    // `updateEffects`' conversions, as a unit's: the player's state is dead (6) once dead.
    convert_held_statuses(player_health.dead ? tl::core::ai_state::kDead : 0, &player_effects,
                          "the player");
    player_effects.update_affixes(dt);
    player_effects.calculate_effect_values();
    player_effects.delete_dead_effects();
    // Then `updateHP` and `updateMana`, off the totals just worked out.
    update_player_meters(dt);

    // `CCharacter::updateActiveSkill`, vtable +0x380,
    // right after `updateEffects`: upkeep and hold time.
    step_active_skill(dt);

    // `CSkillManager::update(float)`: cooldowns, and a
    // run whose minimum time is up fires `EVENT_END`.
    update_live_skill_events(dt);
    player_skill_runner.update(dt, player_cast_context(),
                               [this](const std::string& name) -> const tl::core::KnownSkill* {
                                   return player_skill(name);
                               });
    // CCharacter::update: `+0x708`, the procs' lockout.
    if (0.0f < player_proc_lockout) player_proc_lockout -= dt;
}

bool Renderer::Impl::use_player_item(size_t slot_index, bool from_tray) {
    // `CGameUI::useItem`: a living user and a
    // useable item.
    if (player_health.dead || !(player_health.current > 0.0f)) return false;
    if (slot_index >= player_inventory.slots().size()) return false;
    const tl::core::InventorySlot& slot = player_inventory.slots()[slot_index];
    if (slot.count <= 0) return false;
    load_unit_types();
    ensure_effect_data();
    const tl::core::ItemStats item = slot.item;
    const int stack = slot.count;
    if (!tl::core::item_is_useable(item, unit_types)) return false;
    // The Xbox's `useItem` sends a `PETONLY` item to the pet. INVENTED: not
    // modelled.
    if (unit_types.isa(item.unittype, tl::core::unit_type::kPetOnly) && pet.alive()) return false;
    // `CGameUI::performItemUse`: `TARGET_TYPE` 1 picks a
    // target with the cursor. INVENTED: a pad has no cursor; nothing happens.
    if (item.target_type != 0) return false;

    tl::core::ItemUseTarget target = player_use_target();
    if (!tl::core::can_use_on_target(item, target, unit_types, stat_graphs)) {
        if (from_tray && item.name == "Town Portal Scroll") {
            // The bag's press: an unusable `Town Portal
            // Scroll` queues `VOICECANTCASTHERE_SOUND`, 0.1 s on.
            play_voice(tl::core::SoundSlot::VoiceCantCastHere, 0.1f);
        } else {
            // `ERROR_SOUND` on the game UI's bank.
            play_ui(tl::core::InterfaceSound::Error);
        }
        TL_LOG_INFO("use: %s refused", item.display_name.c_str());
        return false;
    }
    int uses = item.uses;
    const tl::core::ItemUseResult used =
        tl::core::use_on_target(item, stack, uses, target, unit_types, stat_graphs);
    // CEquipment::useOnTarget: incrementJournalStatistic(12, 1)
    if (used.potion_counted) count_journal(tl::core::JournalStatistic::PotionsUsed);
    if (used.play_use_sound && subject_node != nullptr) {
        play_sound(item.use_sound, subject_node->getPosition());
    }
    // `incrementStackBy(-1)`, `removeEquipment` on the last use. INVENTED: a copy's
    // `+0x248` is not kept between uses (every shipped item has `USES:1`).
    if (used.remove_item) {
        player_inventory.remove_item(slot_index, stack);
    } else if (stack > 1) {
        player_inventory.remove_item(slot_index, 1);
    }
    TL_LOG_INFO("use: %s, %d effect(s) on the player, %d left", item.display_name.c_str(),
                used.applied, used.remove_item ? 0 : stack - 1);
    // `applyInstantEffect`'s `OPEN TOWN PORTAL` (before the stack loses its copy), then
    // `incrementJournalStatistic(0xd, 1)`.
    if (used.open_town_portal) {
        open_town_portal();
        count_journal(tl::core::JournalStatistic::PortalsUsed);
    }
    // ... and its `OPEN PORTAL`: a map scroll's portal.
    for (const std::string& dungeon : used.open_map_portals) open_map_portal(dungeon);
    if (used.maxima_changed) {
        refresh_player_maxima();
        TL_LOG_INFO("use: maximum health %.0f, mana %.0f (permanent +%d / +%.1f)",
                    static_cast<double>(player_health.max),
                    static_cast<double>(player_character.max_mp()), player_character.permanent_hp(),
                    static_cast<double>(player_character.permanent_mp()));
    }
    if (used.respec) respec_player();
    if (!used.skill.empty()) perform_unknown_skill(used.skill, used.skill_level);
    if (!used.learned_spell.empty()) {
        // `applyInstantEffect`: global slot 0x41, `VOICESPELLLEARNED_SOUND`, which
        // no class file authors (they spell it with two Ls): silent, as in the original.
        play_voice(tl::core::SoundSlot::VoiceSpellLearned, 0.1f);
        // `applyInstantEffect`.
        queue_tip(tl::core::ContextTip::Spell);
        player_skills.set_effect_managers(player_total(), nullptr);
        TL_LOG_INFO("use: learned the spell '%s'", used.learned_spell.c_str());
    }
    return true;
}

bool Renderer::Impl::identify_item_in(tl::core::Inventory& bag, size_t slot_index) {
    // the `Identify Scroll` copies (`CInventory::getEquipmentsOfUnitGuid`)
    // in the bag, then the pet's unless gone to town (0x29, 0x2a). INVENTED: the original
    // spends the first and clears the last; one stack is both here.
    tl::core::ItemStats* target = bag.item_in_slot(slot_index);
    if (target == nullptr) return false;
    tl::core::Inventory* holder = &player_inventory;
    int scroll = player_inventory.find_by_name("Identify Scroll");
    if (scroll < 0 && pet.alive() && pet.in_world()) {
        holder = &pet.bag();
        scroll = holder->find_by_name("Identify Scroll");
    }
    if (scroll < 0) {
        TL_LOG_INFO("identify: no Identify Scroll for '%s'", tl::core::item_name(*target).c_str());
        return false;
    }
    ensure_effect_data();
    const tl::core::ItemStats used_item = holder->slots()[static_cast<size_t>(scroll)].item;
    const int stack = holder->slots()[static_cast<size_t>(scroll)].count;
    int uses = used_item.uses;
    const tl::core::ItemUseResult used = tl::core::use_on_item(used_item, stack, uses, *target);
    if (used.applied == 0) {
        TL_LOG_INFO("identify: the scroll does nothing to '%s'",
                    tl::core::item_name(*target).c_str());
        return false;
    }
    // (`CItem::destroyItemText`: the copy reads under its full name from here.)
    if (used.play_use_sound && subject_node != nullptr) {
        play_sound(used_item.use_sound, subject_node->getPosition());
    }
    holder->remove_item(static_cast<size_t>(scroll), used.remove_item ? stack : 1);
    // The copy's effects count from now on, wherever it sits.
    refresh_inventory_effects();
    TL_LOG_INFO("identify: '%s' identified, %d scroll(s) left",
                tl::core::item_name(*target).c_str(), holder->count_by_name("Identify Scroll"));
    return true;
}

bool Renderer::Impl::wear_player_item(size_t slot_index) {
    // past the useable items.
    if (slot_index >= player_inventory.slots().size()) return false;
    const tl::core::InventorySlot& slot = player_inventory.slots()[slot_index];
    if (slot.count <= 0) return false;
    load_unit_types();
    if (!slot.item.identified) return identify_player_item(slot_index);

    const std::string name = tl::core::item_name(slot.item);
    const tl::core::RequirementReductions no_reductions{};
    const int level_required =
        tl::core::item_level_requirement(slot.item, &no_reductions, unit_types);
    const bool worn = slot_index < 12;
    const tl::core::Inventory::WearOutcome out =
        worn ? player_inventory.take_off_item(slot_index, unit_types)
             : player_inventory.wear_item(slot_index, unit_types, &player_character,
                                          &player_effects);
    // Sample 0x11 (`LAND_SOUND`) of each item's bank, at the player.
    if (subject_node != nullptr) {
        for (const tl::core::ItemStats& item : out.sounded) {
            play_sound(item.land_sound, subject_node->getPosition());
        }
    }
    // `CLevel::addItem` at the character, and the item's drop.
    for (const tl::core::InventorySlot& dropped : out.dropped) {
        drop_item_near_player(dropped.item, dropped.count);
    }
    if (out.equipped) {
        TL_LOG_INFO("equip: '%s' worn at location %d (%zu sound(s), %zu dropped)", name.c_str(),
                    out.location, out.sounded.size(), out.dropped.size());
    } else if (out.taken_off) {
        TL_LOG_INFO("equip: '%s' taken off%s", name.c_str(),
                    out.dropped.empty() ? "" : ", onto the floor: the bag is full");
    } else {
        TL_LOG_INFO("equip: '%s' is not worn (the character is level %d, the item asks for %d)",
                    name.c_str(), player_character.level(), level_required);
    }
    return out.equipped || out.taken_off;
}

void Renderer::Impl::activate_item_slot(int index) {
    // `CGameUI::activateItemSlot`: the slot's item GUID (`CPlayer +0x900`), the
    // bag's copy, and its slot as the one to use (`CGameUI +0x1678`, X360 `+0x1498`).
    if (index < 0 || index >= tl::core::kSkillFunctionSlots) return;
    const int64_t guid = player_skills.second_slots()[static_cast<size_t>(index)];
    if (guid == -1) return;
    const int slot = tl::core::equipment_of_guid(player_inventory, guid);
    if (slot < 0) return;
    load_unit_types();
    // `CGameUI::processIngameInput`, the requested slot:
    // a useable item (`CItem::isUseable`) goes to `useItem`.
    if (tl::core::item_is_useable(player_inventory.slots()[static_cast<size_t>(slot)].item,
                                  unit_types)) {
        if (use_player_item(static_cast<size_t>(slot))) {
            sync_equipped_weapon();
            update_menu();
            refresh_game_hud();
        }
        return;
    }
    // Any other: `equipEquipmentIntoFirstFreeLocation`, then if wearable (`canEquip`) what
    // `getComparisonItems` finds is taken out and the item put on (`Inventory::wear_item`,
    // the tray's). INVENTED: an item already worn is left
    // (`equipEquipmentIntoFirstFreeLocation`, unread).
    tl::core::Inventory::WearOutcome out;
    if (slot >= 12) {
        out = player_inventory.wear_item(static_cast<size_t>(slot), unit_types, &player_character,
                                         &player_effects);
    }
    if (subject_node != nullptr) {
        for (const tl::core::ItemStats& item : out.sounded) {
            play_sound(item.land_sound, subject_node->getPosition());
        }
    }
    for (const tl::core::InventorySlot& dropped : out.dropped) {
        drop_item_near_player(dropped.item, dropped.count);
    }
    if (!out.equipped) {
        // Nothing put on: sample 0x18 and `VOICEIMPOSSIBLE` (0x31) after 0.1 s.
        play_ui(tl::core::InterfaceSound::Error);
        play_voice(tl::core::SoundSlot::VoiceImpossible, 0.1f);
    }
    sync_equipped_weapon();
    update_menu();
    refresh_game_hud();
}

bool Renderer::Impl::cast_player_skill(const tl::core::KnownSkill& skill, bool report) {
    // `CCharacter::castSkill`, its entry tests: a swing
    // with more than 0.2 s left, or a cast still holding: true, nothing done.
    if (swing.active && (swing.length - swing.elapsed) / swing_rate() > 0.2f) return true;
    if (!player_active_skill.empty()) {
        const std::string& name = player_active_skill.skill;
        const tl::core::SkillRunState* active = player_skill_runner.state(name);
        const tl::core::SkillAnimations animations =
            active != nullptr ? active->animations : tl::core::SkillAnimations{};
        if (tl::core::still_casting(player_active_skill, animations,
                                    player_skill_runner.is_running(name))) {
            return true;
        }
    }

    build_player_catalog();
    tl::core::SkillCastContext context = player_cast_context();
    tl::core::SkillCaster caster = player_caster();
    // With no target and no target item, one in the skill's cone; the turn is the
    // character's `update`'s (`step_player_skill_turn`).
    find_player_skill_target(skill);
    aim_position_skill(skill, caster);
    // `castSkill` hands `startSkill` the target (kept, `CSkill +0x40`); with none,
    // `performSkill` hands the target item.
    if (player_target_node != 0) {
        for (tl::core::LevelScript* machine : level_machines) {
            tl::core::Vec3 at;
            if (!machine->unit_position(player_target_node, at)) continue;
            context.event_target.has_unit = true;
            context.event_target.unit = player_target_node;
            context.event_target.machine = machine;
            break;
        }
    } else if (player_skill_item != 0) {
        const int index = breakable_index(player_skill_item);
        if (index >= 0) {
            const BreakableWorld& b = breakables[static_cast<size_t>(index)];
            context.event_target.item = b.id;
            // `getSkillCanBeExecuted(skill, this, 3, +0x228, +0x350)` tests the
            // item as the target, at its node (no `+0x194` read for a breakable).
            tl::core::SkillTargetUnit item;
            const Ogre::Vector3 p = b.node->getPosition();
            item.position = {p.x, p.y, p.z};
            item.is_item = unit_types.isa(unit_types.id_of(stats_for_unit(b.unit).unittype), 2);
            caster.target_unit = item;
        }
    }
    float spent = 0.0f;
    const tl::core::SkillCastResult result =
        player_skill_runner.cast(skill, caster, context, &spent);
    if (result != tl::core::SkillCastResult::Cast) {
        // `castSkill` (`canCastCurrentSkill`):
        // short of mana, `LOWMANA_SOUND` unless already playing (`CSoundBank::bankPlaying`);
        // a player then queues the tip and `VOICEMANALOW_SOUND` 0.1 s on.
        if (result == tl::core::SkillCastResult::NotEnoughMana) {
            if (subject_node != nullptr && !tl::core::audio_voice_playing(kLowManaVoice)) {
                play_sound(player_stats.sounds[tl::core::SoundSlot::LowMana],
                           subject_node->getPosition(), 1.0f, 0.0f, kLowManaVoice);
            }
            queue_tip(tl::core::ContextTip::ManaLow);
            play_voice(tl::core::SoundSlot::VoiceManaLow, 0.1f);
        }
        if (report) {
            TL_LOG_WARN("skills: '%s' not cast: %s (level %u, %.0f mana)", skill.entry.name.c_str(),
                        tl::core::skill_cast_result_name(result), skill.effective,
                        static_cast<double>(caster.mana));
        }
        return false;
    }
    // CCharacter::castSkill: incrementJournalStatistic(8, 1)
    count_journal(tl::core::JournalStatistic::SkillsCast);
    // `CCharacter::modifyMana(-getManaCost())`.
    if (spent > 0.0f) modify_player_mana(-spent);
    // `castSkill`'s animation half: flags reset, `assignSkillAnimations`, rate, cue.
    tl::core::SkillRunState* running = player_skill_runner.state(skill.entry.name);
    if (running != nullptr && skill.def != nullptr) {
        const tl::core::SkillAnimationCue cue = tl::core::begin_skill_animation(
            player_active_skill, skill.entry.name, *skill.current_property(), caster, *running);
        play_skill_cue(cue);
    }
    TL_LOG_INFO(
        "skills: cast '%s' at level %u, %.0f mana spent (%.0f left), cooldown %.2fs, "
        "%zu affix(es) held, %zu effect(s)",
        skill.entry.name.c_str(), skill.effective, static_cast<double>(spent),
        static_cast<double>(player_character.mp()),
        static_cast<double>(running != nullptr ? running->cooldown_remaining : 0.0f),
        player_effects.affixes().size(), player_effects.all_effects().size());
    return true;
}

void Renderer::Impl::handle_skills_tab(const tl::core::InputState& in, const SkillsTabPad& pad) {
    using tl::core::PadButton;
    using tl::core::SkillsTabCommand;
    const bool a = in.confirm && !confirm_held;
    const bool b = in.skip && !skip_held;
    const bool b_up = !in.skip && skip_held;
    bool changed = false;

    // The presses, as `GamepadButtonDown` hands them; the sticks' four
    // directions are buttons (codes 17-24), one per push.
    std::vector<PadButton> presses;
    if (pad.dpad_up) presses.push_back(PadButton::DPadUp);
    if (pad.dpad_down) presses.push_back(PadButton::DPadDown);
    if (a) presses.push_back(PadButton::A);
    if (b) presses.push_back(PadButton::B);
    if (pad.x) presses.push_back(PadButton::X);
    if (pad.y) presses.push_back(PadButton::Y);
    if (pad.rt) presses.push_back(PadButton::RightTrigger);
    if (pad.lt) presses.push_back(PadButton::LeftTrigger);
    if (pad.start) presses.push_back(PadButton::Start);
    {
        float sx = in.stick_x;
        float sy = in.stick_y;
        bool right_stick = false;
        if (std::abs(sx) < 0.5f && std::abs(sy) < 0.5f) {
            sx = in.look_x;
            sy = in.zoom;
            right_stick = true;
        }
        const bool pushed = std::abs(sx) > 0.5f || std::abs(sy) > 0.5f;
        if (pushed && !skill_stick_held) {
            PadButton direction;
            if (std::abs(sy) >= std::abs(sx)) {
                direction = sy > 0.0f ? PadButton::LeftStickUp : PadButton::LeftStickDown;
            } else {
                direction = sx > 0.0f ? PadButton::LeftStickRight : PadButton::LeftStickLeft;
            }
            if (right_stick) {
                direction = static_cast<PadButton>(static_cast<int>(direction) + 4);
            }
            presses.push_back(direction);
        }
        skill_stick_held = pushed;
    }

    for (PadButton button : presses) {
        if (!inventory_menu.visible() || inventory_menu.menu_mode() != MenuMode::Skills) break;
        const bool sheet = inventory_menu.skill_map_open();
        tl::core::KnownSkill* skill = inventory_menu.selected_skill(player_skills);
        const bool has_sheet = skill != nullptr && skill->effective != 0 && !skill->passive();
        const tl::core::SkillsTabStep step = tl::core::skills_tab_button_down(
            button, sheet, inventory_menu.skill_cursor().in_spells, has_sheet);
        switch (step.command) {
            case SkillsTabCommand::None: break;
            case SkillsTabCommand::SkillSet:
                // with the skills menu open: no gate.
                if (tl::core::switch_skill_set(player_skills, step.value, true) ==
                    tl::core::SkillSetSwitch::Switched) {
                    play_ui(tl::core::InterfaceSound::SkillAssign);
                    refresh_game_hud();
                }
                changed = true;
                break;
            case SkillsTabCommand::Learn:
                if (skill != nullptr &&
                    player_skills.learn(*skill, static_cast<uint32_t>(player_character.level()),
                                        player_character.unspent_skill_points_ref())) {
                    // Slot 0x1b, `POINTASSIGN`.
                    play_ui(tl::core::InterfaceSound::PointAssign);
                    TL_LOG_INFO("skills: '%s' to level %u, %d point(s) left",
                                skill->entry.name.c_str(), skill->invested,
                                player_character.unspent_skill_points());
                    // `CSkillManager::setSkillLevel` ends
                    // in `startPassiveSkills`: a passive
                    // point takes effect at once.
                    start_player_passives();
                    refresh_game_hud();
                    changed = true;
                }
                break;
            case SkillsTabCommand::OpenSheet:
                inventory_menu.set_skill_map_open(true);
                changed = true;
                break;
            case SkillsTabCommand::CloseSheet:
                inventory_menu.set_skill_map_open(false);
                changed = true;
                break;
            case SkillsTabCommand::SheetPress:
                // dims press frames the console sheet lacks: just the legend.
                changed = true;
                break;
            case SkillsTabCommand::CloseMenu: close_master_menu(); return;
            case SkillsTabCommand::NextTab: step_master_menu_tab(1); return;
            case SkillsTabCommand::PrevTab: step_master_menu_tab(-1); return;
            case SkillsTabCommand::Cursor:
                // with `NAVIGATE` (slot 0x43) on every step.
                switch (step.value) {
                    case 3: inventory_menu.skill_cursor_up(player_skills); break;
                    case 4: inventory_menu.skill_cursor_down(player_skills); break;
                    case 5: inventory_menu.skill_cursor_right(player_skills); break;
                    default: inventory_menu.skill_cursor_left(player_skills); break;
                }
                play_ui(tl::core::InterfaceSound::Navigate);
                changed = true;
                break;
        }
    }

    // The releases, as `GamepadButtonUp` hands them, in its order.
    const std::pair<bool, PadButton> releases[] = {
        {pad.y_up, PadButton::Y},
        {pad.rt_up, PadButton::RightTrigger},
        {b_up, PadButton::B},
        {pad.lt_up, PadButton::LeftTrigger},
    };
    for (const auto& [released, code] : releases) {
        if (!released) continue;
        if (!inventory_menu.visible() || inventory_menu.menu_mode() != MenuMode::Skills) break;
        const std::optional<tl::core::SkillButton> button =
            tl::core::skills_sheet_button_up(code, inventory_menu.skill_map_open());
        if (!button.has_value()) continue;
        tl::core::KnownSkill* skill = inventory_menu.selected_skill(player_skills);
        if (skill != nullptr && player_skills.map(*skill, *button, player_skills.skill_set())) {
            // slot 0x1e, `ASSIGNSKILL`; then the sheet closes.
            play_ui(tl::core::InterfaceSound::SkillAssign);
            TL_LOG_INFO("skills: '%s' mapped to function slot %d", skill->entry.name.c_str(),
                        tl::core::skill_button_slot(*button, player_skills.skill_set()));
            inventory_menu.set_skill_map_open(false);
            refresh_game_hud();
        }
        // Refused, the sheet stays and the legend is refreshed.
        changed = true;
    }
    if (changed) update_menu();
}

/// Resolve a `Unit Trigger`'s `MODEL` to the mesh, collision and states its unit file
/// authors: a `CTriggerUnit` (`getUnitModel()` / `getUnitCollisionModel()`;
/// `unitInit`, reads `MAXSTATES`
/// and `ANIMATION_STATE_n`). A unit with no `MESHFILE` draws nothing; `BASEFILE` is not
/// followed (props' base authors no mesh). Cached per unit name.
/// `CUnitTriggerDescriptor::GetTriggerModelStringByID`.
void Renderer::Impl::finish_trigger_unit(int64_t node, const std::string& unit,
                                         const TriggerProp& prop, bool by_player,
                                         const std::vector<const Ogre::SceneNode*>& mine,
                                         bool treasure, bool skill) {
    // Where the prop stands: the firing machine's copy of it, else nothing.
    Ogre::Vector3 at = Ogre::Vector3::ZERO;
    Ogre::Quaternion facing = Ogre::Quaternion::IDENTITY;
    bool found = false;
    const auto units = scene_units.equal_range(unit);
    for (auto it = units.first; it != units.second && !found; ++it) {
        if (it->second.node != node || it->second.entity == nullptr) continue;
        if (const Ogre::SceneNode* sn = it->second.entity->getParentSceneNode()) {
            if (!mine.empty() && std::find(mine.begin(), mine.end(), sn) == mine.end()) continue;
            at = sn->_getDerivedPosition();
            facing = sn->_getDerivedOrientation();
            found = true;
        }
    }
    if (!found) return;

    // `CTriggerUnit::trigger`, a one-shot reaching
    // `MAXSTATES`: `+0x1f0` cleared; a `[TREASURE]` class rolls
    // `randomIntegerBetweenVolatile(MINCOUNT, MAXCOUNT)` through `spawnTreasureClass`.
    //
    if (treasure && !prop.treasure_class.empty()) {
        // CTriggerUnit::trigger: incrementJournalStatistic(9, 1)
        if (by_player) count_journal(tl::core::JournalStatistic::ChestsOpened);
        const int32_t count =
            tl::core::volatile_random().integer_between(prop.treasure_min, prop.treasure_max);
        if (!loot_tables_built) {
            loot_tables_built = true;
            build_spawn_context(loot_tables, /*items_by_type=*/true);
        }
        const tl::core::SpawnContext& context = loot_tables.context;
        const tl::core::SpawnClass* treasure =
            context.get_spawn_class ? context.get_spawn_class(prop.treasure_class) : nullptr;
        if (treasure == nullptr) {
            TL_LOG_WARN("prop: %s names no spawn class '%s'", unit.c_str(),
                        prop.treasure_class.c_str());
        } else {
            tl::core::LootCharacter opener;
            const tl::core::LootCharacter* who = by_player ? loot_character(0, opener) : nullptr;
            const tl::core::LootPlacement place = loot_placement_at(from_ogre(at));
            const std::vector<tl::core::ItemStats> made = tl::core::create_units_at_position(
                *treasure, static_cast<uint32_t>(std::max(0, count)),
                who != nullptr ? &who->spawn : nullptr, nullptr, level_monster_level,
                tl::core::volatile_random(), tl::core::game_random(), context, loot_unit_maker(),
                &place);
            std::string names;
            for (const tl::core::ItemStats& item : made) {
                names += (names.empty() ? "" : ", ") + item.name;
            }
            TL_LOG_INFO("prop: %s opens on %zu item(s) from '%s' [%s]", unit.c_str(), made.size(),
                        prop.treasure_class.c_str(), names.c_str());
        }
    }

    // Then `CBaseUnit::selectRandomSkill`, executed at
    // the prop's position lowered by `+0x194`.
    if (!skill) return;
    const int32_t pick = tl::core::select_random_skill(static_cast<uint32_t>(prop.skills.size()),
                                                       tl::core::game_random());
    if (pick >= 0 && static_cast<size_t>(pick) < prop.skills.size()) {
        cast_prop_trap(unit, prop.skills[static_cast<size_t>(pick)],
                       prop.skill_levels[static_cast<size_t>(pick)],
                       at - Ogre::Vector3(0.0f, prop.collision_radius, 0.0f), facing, by_player,
                       node);
    }
}

// PORT: a unit whose body is a layout placed through `place_layout`.
bool Renderer::Impl::place_layout_bodied_unit(const std::string& unit, const Ogre::Vector3& at) {
    const std::string path = prop_layout_path(unit);
    if (path.empty()) return false;

    // Depth 1: its own node map, and with no owning link it arms its own machine.
    if (!place_layout(path, at, Ogre::Quaternion::IDENTITY, /*depth=*/1)) {
        return false;
    }
    TL_LOG_INFO("script: '%s' placed as a layout at (%.2f, %.2f, %.2f)", unit.c_str(),
                static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z));
    return true;
}

std::vector<Ogre::SceneNode*> Renderer::Impl::place_particle_layout(const std::string& reference,
                                                                    const Ogre::Vector3& at,
                                                                    const Ogre::Quaternion& facing,
                                                                    Ogre::SceneNode* parent,
                                                                    ParticleStart start) {
    std::vector<Ogre::SceneNode*> holders;
    if (reference.empty()) return holders;
    std::string path = reference;
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    const size_t before = particle_systems.size();
    const bool riding = parent != nullptr;
    // A kept `CParticle` of the file only where a new one would be no one's.
    const bool keepable =
        start == ParticleStart::Cached && particle_owner == 0 && node_sink == nullptr;
    bool placed = keepable && unpark_particle(path, at, facing, riding);
    if (!placed) {
        // Depth 1: a particle file, not a level.
        const bool outer_particle_file = placing_particle_file;
        placing_particle_file = true;
        cparticle_places_next_layout = start != ParticleStart::Live;
        placed = place_layout(path, riding ? Ogre::Vector3::ZERO : at,
                              riding ? Ogre::Quaternion::IDENTITY : facing, 1);
        cparticle_places_next_layout = false;
        placing_particle_file = outer_particle_file;
    }
    if (!placed) return holders;
    arm_particle_timer(before, particle_systems.size());
    claim_particle_holders(before, parent, holders);
    if (start != ParticleStart::Live) {
        // The `CParticle`'s node: the parent it rides, or `at`.
        const Ogre::Vector3 origin = riding ? parent->_getDerivedPosition() : at;
        const uint32_t id =
            register_cparticle(path, before, origin, [&holders](const ParticleSystem& system) {
                return std::find(holders.begin(), holders.end(), system.holder()) != holders.end();
            });
        if (keepable && id != 0) remember_parkable(id, path, holders, at, facing, riding);
        if (start == ParticleStart::Now || start == ParticleStart::Cached) {
            if (id != 0) {
                cparticle_started(id);
            } else if (const ParticleFileExtras* extras = particle_extras(path)) {
                play_particle_extras(*extras, origin, path);
            }
        }
    }
    return holders;
}

// PORT: drawing, the systems placed from `before` on that no list holds yet: theirs, each
// holder once, moved under `parent` when they ride it.
void Renderer::Impl::claim_particle_holders(size_t before, Ogre::SceneNode* parent,
                                            std::vector<Ogre::SceneNode*>& holders) {
    for (size_t i = before; i < particle_systems.size(); ++i) {
        ParticleSystem& system = particle_systems[i];
        Ogre::SceneNode* holder = system.holder();
        if (holder == nullptr || system.claimed) continue;
        system.claimed = true;
        if (std::find(holders.begin(), holders.end(), holder) != holders.end()) continue;
        if (parent != nullptr) {
            if (Ogre::Node* up = holder->getParent()) up->removeChild(holder);
            parent->addChild(holder);
        }
        holders.push_back(holder);
    }
}

// PORT: a `Cached` placement's file and each holder's pose in the placement's frame, for
// `unpark_particle`.
void Renderer::Impl::remember_parkable(uint32_t id, const std::string& file,
                                       const std::vector<Ogre::SceneNode*>& holders,
                                       const Ogre::Vector3& at, const Ogre::Quaternion& facing,
                                       bool riding) {
    PlacedParticle& placement = placed_particles[id];
    placement.file = file;
    placement.holders = holders;
    const Ogre::Vector3 frame_at = riding ? Ogre::Vector3::ZERO : at;
    const Ogre::Quaternion frame_back = riding ? Ogre::Quaternion::IDENTITY : facing.Inverse();
    for (const Ogre::SceneNode* holder : holders) {
        placement.frames.emplace_back(frame_back * (holder->getPosition() - frame_at),
                                      frame_back * holder->getOrientation());
    }
}

bool Renderer::Impl::unpark_particle(const std::string& file, const Ogre::Vector3& at,
                                     const Ogre::Quaternion& facing, bool riding) {
    const auto found = parked_particles.find(file);
    if (found == parked_particles.end() || found->second.empty()) return false;
    ParkedParticle parked = std::move(found->second.back());
    found->second.pop_back();
    const Ogre::Vector3 frame_at = riding ? Ogre::Vector3::ZERO : at;
    const Ogre::Quaternion frame = riding ? Ogre::Quaternion::IDENTITY : facing;
    Ogre::SceneNode* root = scene->getRootSceneNode();
    for (size_t i = 0; i < parked.holders.size(); ++i) {
        Ogre::SceneNode* holder = parked.holders[i];
        parked_holders.erase(holder);
        holder->setPosition(frame_at + frame * parked.frames[i].first);
        holder->setOrientation(frame * parked.frames[i].second);
        root->addChild(holder);
    }
    // Each system as `place_torchlight_particle` leaves a new one.
    for (const ParticleSystem& kept : parked.systems) {
        ParticleSystem system;
        system.pu = kept.pu;
        system.node = kept.node;
        system.node_owner = &scripted_nodes;
        system.fixed_timeout = kept.fixed_timeout;
        system.finite_window = kept.finite_window;
        system.non_expiring = kept.non_expiring;
        if (!system.pu.is_light) hold_particle_with_the_world(system);
        start_torchlight_particle(system.pu);
        particle_systems.push_back(std::move(system));
        particle_index_add(particle_systems.size() - 1);
    }
    return true;
}

uint32_t Renderer::Impl::register_cparticle(
    const std::string& reference, size_t first, const Ogre::Vector3& origin,
    const std::function<bool(const ParticleSystem&)>& take) {
    uint32_t id = 0;
    for (size_t i = first; i < particle_systems.size(); ++i) {
        ParticleSystem& system = particle_systems[i];
        if (system.cparticle != 0 || !take(system)) continue;
        Ogre::SceneNode* holder = system.holder();
        if (holder == nullptr) continue;
        if (id == 0) {
            if (++next_placed_particle == 0) ++next_placed_particle;
            id = next_placed_particle;
            PlacedParticle placed;
            placed.extras = particle_extras(reference);
            placed.reference = reference;
            placed.from_holder = holder->_getFullTransform().inverse() * origin;
            placed_particles[id] = placed;
        }
        system.cparticle = id;
    }
    return id;
}

// `CLevel::updateVisibleParticles(Ogre::Camera*, const Ogre::Vector3&, bool)`,
// every frame with the camera's focus and `getIsPaused()`.
void Renderer::Impl::update_visible_particles(bool paused) {
    if (listed_particles == 0 || camera == nullptr) return;
    const Ogre::Viewport* const view = camera->getViewport();
    if (view == nullptr) return;
    const Ogre::Vector3 focus = game_camera.focus;
    // With a player, a pass only once the focus has moved 0.25, or on a paused frame.
    if (player_control) {
        if (!paused) {
            const float dx = focus.x - visible_particles_last.x;
            const float dy = focus.y - visible_particles_last.y;
            const float dz = focus.z - visible_particles_last.z;
            if (std::sqrt(std::fma(dx, dx, std::fma(dz, dz, dy * dy))) < 0.25f) return;
        }
        visible_particles_last = focus;
    }
    // Projection times view, and the camera's up, which `getScreenPosition` adds.
    const Ogre::Matrix4 clip = camera->getProjectionMatrix() * camera->getViewMatrix();
    const Ogre::Vector3 up = camera->getDerivedOrientation().yAxis();
    // `CGameUI::getWindowWidth` and `getAspectRatio`.
    const float width = static_cast<float>(view->getActualWidth());
    const float aspect = width / static_cast<float>(view->getActualHeight());
    // 45 with `NETBOOK MODE`, which the port's settings do not hold.
    const float depth_limit = 90.0f;

    size_t shown = 0;
    size_t hidden = 0;
    // Each `CParticle`'s first system, in one walk.
    std::unordered_map<uint32_t, const ParticleSystem*> firsts;
    for (const ParticleSystem& system : particle_systems) {
        if (system.cparticle != 0) firsts.emplace(system.cparticle, &system);
    }
    for (auto& [id, placed] : placed_particles) {
        if (!placed.listed) continue;
        // The link's `CParticle` (`+0x1f0`); a `NO CULLING` one (`+0x127`) never joined.
        // INVENTED: links are all enabled (the original skips disabled ones, vtable +0x48).
        const auto found = firsts.find(id);
        if (found == firsts.end()) continue;
        const ParticleSystem* first = found->second;
        const Ogre::Vector3 at = first->link_position;

        // `CGameUI::getScreenPosition(position, up, matrix)`:
        // one `up` above, through the matrix with its divide by w, X and Y over Z,
        // X to the window times the share no menu covers (`+0x14a0`).
        Ogre::Vector3 screen = clip * (at + up);
        if (screen.z != 0.0f) {
            const float inverse = 1.0f / screen.z;
            screen.x = screen.x * inverse;
            screen.y = screen.y * inverse;
            screen.z = screen.z * inverse;
        }
        const float half = width * 0.5f;
        const float across = std::fma(width, 0.5f, half * screen.x);

        const float dx = at.x - focus.x;
        const float dz = at.z - focus.z;
        const float away = std::sqrt(std::fma(dz, dz, dx * dx));

        const bool on_screen = !(across < width * -0.5f) && !(across > width * 1.5f) &&
                               !(screen.z < 0.0f) && screen.z <= depth_limit;
        const bool show = on_screen || away < aspect * 15.0f || paused;
        if (show == placed.visible) continue;
        placed.visible = show;
        ++(show ? shown : hidden);
        // `CLayout::setVisible`: the flag changed, so
        // `Start` or `Stop(false)`.
        for (ParticleSystem& system : particle_systems) {
            if (system.cparticle != id) continue;
            if (show) {
                restart_particle_system(system);
                // `CParticle::forceParticleUpdate(1.0)`, on a paused pass.
                if (paused) force_torchlight_particle_update(system.pu, 1.0f);
            } else {
                stop_particle_emission(system);
            }
        }
        if (show) cparticle_started(id);
    }
    if (shown + hidden > 0) {
        TL_LOG_INFO("particles: %zu link(s) shown, %zu hidden, of %zu the level lists", shown,
                    hidden, listed_particles);
    }
}

void Renderer::Impl::cparticle_started(uint32_t id) {
    const auto found = placed_particles.find(id);
    if (found == placed_particles.end() || found->second.extras == nullptr) return;
    const PlacedParticle& placed = found->second;
    for (const ParticleSystem& system : particle_systems) {
        if (system.cparticle != id) continue;
        Ogre::SceneNode* holder = system.holder();
        if (holder == nullptr) continue;
        play_particle_extras(*placed.extras, holder->_getFullTransform() * placed.from_holder,
                             placed.reference);
        return;
    }
}

void Renderer::Impl::play_particle_extras(const ParticleFileExtras& extras, const Ogre::Vector3& at,
                                          const std::string& reference) {
    // `CParticleCache::playSounds`: each sound in order on the preloader's bank,
    // radius the definition's or the bank's 10 (`CSoundBank`).
    // A character's slot particle: its sounds were `World::start_slot_particle`'s.
    const std::vector<ParticleFileExtras::Sound> none;
    for (const ParticleFileExtras::Sound& sound : slot_sounds_played ? none : extras.sounds) {
        // `getSoundDataObjectByGuid`, played by its name.
        const SoundDefinition* named = sound_by_guid(sound.guid);
        if (named == nullptr) continue;
        play_sound(named->name, at, sound.volume, 10.0f, std::string(),
                   /*volume_given=*/sound.volume != 0.0f);
    }
    // `CParticleCache::playCameraShakes`: `startCameraShake(position, false)` on
    // each. DIVERGENCE: the camera owns a copy where the original lends the cache's object
    // (plumbing: no shake here outlives the call).
    if (!extras.shakes.empty()) load_camera_shake_graphs();
    for (const tl::core::CameraShake& authored : extras.shakes) {
        tl::core::CameraShake shake;
        shake.clone_from(authored, camera_shake_graphs);
        shake.start(tl::core::Vec3f{at.x, at.y, at.z}, /*owned=*/true, camera_shakes);
        TL_LOG_INFO("camera: %s shakes '%s'%s", reference.c_str(), shake.name.c_str(),
                    shake.curve != nullptr ? "" : " (no curve; nothing moves)");
    }
}

void Renderer::Impl::forget_dead_cparticles() {
    // A placement leaves no trace of its own going: its systems do.
    if (placed_particles.size() < 256) return;
    // Sorted and kept between frames: a set made each frame was a node per system.
    std::vector<uint32_t>& live = live_cparticles;
    live.clear();
    for (const ParticleSystem& system : particle_systems) {
        if (system.cparticle != 0) live.push_back(system.cparticle);
    }
    std::sort(live.begin(), live.end());
    for (auto it = placed_particles.begin(); it != placed_particles.end();) {
        if (std::binary_search(live.begin(), live.end(), it->first)) {
            ++it;
            continue;
        }
        if (it->second.listed && listed_particles > 0) --listed_particles;
        it = placed_particles.erase(it);
    }
}

// `CTriggerUnit::setTriggerState`, its walk of `+0x2b0`:
// the current state's layout shown and started, the others hidden and stopped; `unitInit`
// parents each to the unit's node.
void Renderer::Impl::set_trigger_state_layout(Ogre::Entity* entity, const TriggerProp& prop,
                                              int state) {
    if (entity == nullptr || state < 1 || state > 2) return;
    Ogre::SceneNode* node = entity->getParentSceneNode();
    if (node == nullptr) return;
    TriggerStateLayout& held = trigger_state_layouts[entity];
    if (held.state == state) return;
    destroy_particle_holders(held.holders);
    held.holders.clear();
    held.state = state;
    const std::string& file = prop.state_layout[state - 1];
    if (file.empty()) return;
    // `unitInit` loads each as a kind-2 `CLayout` and, with no `Timeline`, as kind 1.
    const tl::formats::CmpScene* const parsed = cached_layout(file);
    const bool timed = parsed != nullptr && parsed->ok && !parsed->timelines.empty();
    const ParticleStart start =
        !timed && renderer_detail::caching_particles() ? ParticleStart::Now : ParticleStart::Live;
    held.holders =
        place_particle_layout(file, Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, node, start);
    // A prop the level has hidden shows its layout when it is shown itself.
    if (!entity->getVisible()) {
        for (Ogre::SceneNode* holder : held.holders) holder->setVisible(false);
    }
    TL_LOG_INFO("unit trigger: state %d layout %s, %zu system node(s)", state, file.c_str(),
                held.holders.size());
}

// PORT: drawing, the particle systems a placement adds after `particle_systems[mark]`.
size_t Renderer::Impl::view_particle_mark() const { return particle_systems.size(); }

// PORT: drawing, the systems placed since `mark` re-hung under `ride`, their place kept.
void Renderer::Impl::view_ride_particles(size_t mark, Ogre::SceneNode* ride) {
    const Ogre::Quaternion inverse = ride->_getDerivedOrientation().Inverse();
    const Ogre::Vector3 origin = ride->_getDerivedPosition();
    const Ogre::Vector3 scale = ride->_getDerivedScale();
    for (size_t i = mark; i < particle_systems.size(); ++i) {
        if (particle_systems[i].claimed) continue;
        Ogre::SceneNode* holder = particle_systems[i].holder();
        if (holder == nullptr || holder->getParent() == ride) continue;
        const Ogre::Vector3 world = holder->_getDerivedPosition();
        const Ogre::Quaternion turned = holder->_getDerivedOrientation();
        if (Ogre::Node* up = holder->getParent()) up->removeChild(holder);
        ride->addChild(holder);
        holder->setPosition((inverse * (world - origin)) / scale);
        holder->setOrientation(inverse * turned);
        holder->setScale(Ogre::Vector3::UNIT_SCALE / scale);
    }
}

// PORT: drawing, the systems placed since `mark` that no script of the layout claimed first:
// their holders, once each, and those holding a `Line` emitter.
void Renderer::Impl::view_claim_particles(size_t mark, std::vector<Ogre::SceneNode*>& holders,
                                          std::vector<Ogre::SceneNode*>& line_holders) {
    for (size_t i = mark; i < particle_systems.size(); ++i) {
        if (particle_systems[i].claimed) continue;
        Ogre::SceneNode* holder = particle_systems[i].holder();
        if (holder != nullptr) particle_systems[i].claimed = true;
        if (holder != nullptr &&
            std::find(holders.begin(), holders.end(), holder) == holders.end()) {
            holders.push_back(holder);
        }
        if (holder != nullptr && torchlight_particle_line_emitters(particle_systems[i].pu) > 0 &&
            std::find(line_holders.begin(), line_holders.end(), holder) == line_holders.end()) {
            line_holders.push_back(holder);
        }
    }
}

// PORT: drawing, whether a system hanging from one of `holders` still has particles.
bool Renderer::Impl::view_particles_alive(const std::vector<Ogre::SceneNode*>& holders) const {
    // A lookup of the systems' own index, brought up to date first if a destruction left it stale.
    const_cast<Impl*>(this)->ensure_particle_index();
    for (const Ogre::SceneNode* holder : holders) {
        const auto found = particle_index.by_holder.find(holder);
        if (found == particle_index.by_holder.end()) continue;
        for (size_t i : found->second) {
            if (torchlight_particle_has_particles(particle_systems[i].pu)) return true;
        }
    }
    return false;
}

// PORT: drawing, a skill event's systems stopped: the layout's own at once (every one when
// `loop`), a linked file's let die out.
void Renderer::Impl::view_stop_particles(const std::vector<Ogre::SceneNode*>& holders, bool loop) {
    for (ParticleSystem& system : holder_systems(holders)) {
        if (system.owner == 0 || loop) {
            if (!system.finished) {
                stop_torchlight_particle(system.pu);
                system.pu_dying = false;
                system.finished = true;
            }
        } else {
            stop_particle_emission(system);
        }
    }
}

// PORT: drawing, the systems hanging from `holders` stop emitting.
void Renderer::Impl::view_stop_emission(const std::vector<Ogre::SceneNode*>& holders) {
    for (ParticleSystem& system : holder_systems(holders)) {
        if (!system.finished) stop_particle_emission(system);
    }
}

// PORT: drawing, `holders` moved by `by` in the world.
void Renderer::Impl::view_translate(const std::vector<Ogre::SceneNode*>& holders,
                                    const Ogre::Vector3& by) {
    for (Ogre::SceneNode* holder : holders) {
        if (holder != nullptr) holder->translate(by, Ogre::Node::TS_WORLD);
    }
}

// PORT: drawing, each system of `line_holders` at `start`, its `Line` emitter's end at `end`.
void Renderer::Impl::view_stretch_lines(const std::vector<Ogre::SceneNode*>& line_holders,
                                        const Ogre::Vector3& start, const Ogre::Vector3& end) {
    for (ParticleSystem& system : holder_systems(line_holders)) {
        system.holder()->_setDerivedPosition(start);
        set_torchlight_particle_line_end(system.pu, end - start);
    }
}

namespace {

/// `CCharacter::getBones`: the bone each cached node
/// follows, by `gSKILL_BONE_ATTACHMENT_NAMES` index, each name tried in this order.
const Ogre::Bone* skill_attachment_bone(Ogre::Entity* entity, int attachment) {
    if (entity == nullptr || !entity->hasSkeleton()) return nullptr;
    std::vector<const char*> names;
    switch (attachment) {
        case tl::core::kSkillBoneHead: names = {"Bip01 Head"}; break;
        case tl::core::kSkillBoneRightHand: names = {"tag_righthand", "Bip01 R Hand"}; break;
        case tl::core::kSkillBoneLeftHand: names = {"tag_lefthand", "Bip01 L Hand"}; break;
        case tl::core::kSkillBoneRightShoulder: names = {"Bip01 R Clavicle"}; break;
        case tl::core::kSkillBoneLeftShoulder: names = {"Bip01 L Clavicle"}; break;
        default: return nullptr;
    }
    Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
    for (const char* name : names) {
        if (skeleton->hasBone(name)) return skeleton->getBone(name);
    }
    return nullptr;
}

}  // namespace

bool Renderer::Impl::skill_bone_world(const ViewUnit& unit, int attachment, Ogre::Vector3& out) {
    // `CSkillEvent::getBonePosition(unit, attachment, true)`.
    //
    Ogre::Entity* entity = nullptr;
    Ogre::Vector3 feet;
    if (unit.item != 0) {
        // A target item: its model and its node.
        const int index = breakable_index(unit.item);
        if (index < 0) return false;
        entity = breakables[static_cast<size_t>(index)].entity;
        feet = breakables[static_cast<size_t>(index)].node->getPosition();
    } else if (unit.player) {
        if (subject_node == nullptr) return false;
        entity = subject;
        feet = subject_node->getPosition();
    } else {
        if (unit.machine == nullptr) return false;
        tl::core::Vec3 at;
        if (!unit.machine->unit_position(unit.node, at)) return false;
        feet = Ogre::Vector3(at.x, at.y, at.z);
        if (SceneUnit* drawn = scene_unit_of(unit.machine, unit.node)) entity = drawn->entity;
    }
    // `CENTER`: half the mesh's height, at its scale, above the feet.
    if (attachment == tl::core::kSkillBoneCenter) {
        if (entity != nullptr && entity->getParentSceneNode() != nullptr) {
            const Ogre::AxisAlignedBox& box = authored_mesh_bounds(entity);
            const float scale = entity->getParentSceneNode()->_getDerivedScale().x;
            out = feet +
                  Ogre::Vector3(0.0f,
                                std::fabs((box.getMaximum().y - box.getMinimum().y) * 0.5f) * scale,
                                0.0f);
        } else {
            out = feet;
        }
        return true;
    }
    // A hand, the head or a shoulder: the bone's world place (`updateBonePositions`).
    // No such bone: the zero vector, not the feet.
    if (attachment >= tl::core::kSkillBoneHead && attachment <= tl::core::kSkillBoneLeftShoulder) {
        const Ogre::Bone* bone = skill_attachment_bone(entity, attachment);
        if (bone == nullptr || entity == nullptr || entity->getParentNode() == nullptr) {
            out = Ogre::Vector3::ZERO;
            return true;
        }
        out = entity->getParentNode()->_getFullTransform() * bone->_getDerivedPosition();
        return true;
    }
    // None, or `POSITION`: the feet.
    out = feet;
    return true;
}

// PORT: a drawn unit's facing read off its OGRE node.
bool Renderer::Impl::skill_unit_forward(const ViewUnit& unit, Ogre::Vector3& out) {
    const Ogre::Node* node = nullptr;
    if (unit.player) {
        node = subject_node;
    } else if (unit.machine != nullptr) {
        if (SceneUnit* drawn = scene_unit_of(unit.machine, unit.node)) {
            if (drawn->entity != nullptr) node = drawn->entity->getParentSceneNode();
        }
    }
    if (node == nullptr) return false;
    out = node->_getDerivedOrientation() * Ogre::Vector3::UNIT_Z;
    return true;
}

namespace renderer_detail {
/// The level's characters a missile can meet, gathered across every machine.
}  // namespace renderer_detail

// PORT: each unit's drawn entity, the last of the placed units with its id, else the last of
// the spawned ones, found in one walk of both for all of a sweep's units; then each swept.
void Renderer::Impl::unit_collider_sweeps(const std::vector<int64_t>& ids,
                                          const tl::core::Vec3& from, const tl::core::Vec3& to,
                                          float radius, std::vector<float>& t,
                                          std::vector<char>& drawn) {
    std::unordered_map<int64_t, size_t> wanted;
    wanted.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) wanted.emplace(ids[i], i);
    std::vector<const Ogre::Entity*> placed(ids.size(), nullptr);
    std::vector<const Ogre::Entity*> made_ones(ids.size(), nullptr);
    for (const auto& entry : scene_units) {
        if (entry.second.entity == nullptr) continue;
        const auto at = wanted.find(entry.second.node);
        if (at != wanted.end()) placed[at->second] = entry.second.entity;
    }
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) {
            if (made.entity == nullptr) continue;
            const auto at = wanted.find(made.id);
            if (at != wanted.end()) made_ones[at->second] = made.entity;
        }
    }
    t.assign(ids.size(), 2.0f);
    drawn.assign(ids.size(), 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        const Ogre::Entity* entity = placed[i] != nullptr ? placed[i] : made_ones[i];
        drawn[i] = unit_collider_sweep(entity, from, to, radius, t[i]) ? 1 : 0;
    }
}

// `CBaseUnit::sphereCollision(start, end, radius, ...)`.
bool Renderer::Impl::unit_collider_sweep(const Ogre::Entity* entity, const tl::core::Vec3& from,
                                         const tl::core::Vec3& to, float radius, float& t) {
    t = 2.0f;
    if (entity == nullptr || !entity->isVisible() || entity->getParentSceneNode() == nullptr) {
        return false;
    }
    // `getUnitCollisionModel` (vtable +0x1e8, `+0x4c8`): `CCharacter::loadModel`
    // reads `COLLISIONFILE`, else `media/models/collider.mesh`.
    // INVENTED: the 26 units with their own `collider` / `colliderbig` get it too.
    if (!unit_collider_read) {
        unit_collider_read = true;
        const CollisionModel& model = collision_model("media/models/collider.mesh");
        size_t faces = 0;
        for (const CollisionModel::Submesh& sub : model.submeshes) {
            for (size_t i = 0; i + 2 < sub.corners.size(); i += 3) {
                const auto& a = sub.points[sub.corners[i]];
                const auto& b = sub.points[sub.corners[i + 1]];
                const auto& c = sub.points[sub.corners[i + 2]];
                unit_collider.add_triangle({a.x, a.y, a.z}, {b.x, b.y, b.z}, {c.x, c.y, c.z});
                ++faces;
            }
        }
        unit_collider.build();
        if (model.has_bounds) {
            TL_LOG_INFO(
                "units: collider.mesh has %zu face(s), (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)",
                faces, static_cast<double>(model.low.x), static_cast<double>(model.low.y),
                static_cast<double>(model.low.z), static_cast<double>(model.high.x),
                static_cast<double>(model.high.y), static_cast<double>(model.high.z));
        } else {
            TL_LOG_WARN("units: media/models/collider.mesh gave %zu face(s)", faces);
        }
    }
    if (unit_collider.empty()) return false;

    // The unit's bounds first; then both ends into the unit's frame: less its
    // `getPosition(true)`, through the inverse of its node's orientation, no scale.
    const Ogre::SceneNode* node = entity->getParentSceneNode();
    const Ogre::Vector3 at = node->_getDerivedPosition();
    const Ogre::Quaternion inverse = node->_getDerivedOrientation().Inverse();
    const Ogre::Vector3 a = inverse * (Ogre::Vector3(from.x, from.y, from.z) - at);
    const Ogre::Vector3 b = inverse * (Ogre::Vector3(to.x, to.y, to.z) - at);
    const tl::core::Vec3 start{a.x, a.y, a.z};
    const tl::core::Vec3 end{b.x, b.y, b.z};
    // `CCollisionList::sphereCollision` over the model's faces (`+0x38`); a box that meets none
    // of them sorts nothing, and nothing is met.
    const tl::core::Vec3 box_low{std::min(a.x, b.x) - radius, std::min(a.y, b.y) - radius,
                                 std::min(a.z, b.z) - radius};
    const tl::core::Vec3 box_high{std::max(a.x, b.x) + radius, std::max(a.y, b.y) + radius,
                                  std::max(a.z, b.z) + radius};
    if (!unit_collider.may_sort_any(box_low, box_high)) return true;
    unit_collider.sort_for_collision(box_low, box_high);
    tl::core::CollisionHit hit;
    if (!unit_collider.pre_sorted_sphere_collision(start, end, radius, /*objects=*/false, hit)) {
        return true;
    }
    const tl::core::Vec3 d = end - start;
    const tl::core::Vec3 m = hit.centre - start;
    const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    t = len2 > 0.0f ? std::sqrt((m.x * m.x + m.y * m.y + m.z * m.z) / len2) : 0.0f;
    return true;
}

// PORT: drawing, a node of its own for a missile, at `at`.
Ogre::SceneNode* Renderer::Impl::view_create_node(const Ogre::Vector3& at) const {
    return scene->getRootSceneNode()->createChildSceneNode(at);
}

// PORT: drawing, a missile's node at `at`, turned along `forward`.
void Renderer::Impl::view_aim_node(Ogre::SceneNode* node, const Ogre::Vector3& at,
                                   const Ogre::Vector3& forward) {
    node->setPosition(at);
    node->setDirection(forward, Ogre::Node::TS_WORLD, Ogre::Vector3::UNIT_Z);
}

// PORT: drawing, a node and what hangs from it gone.
void Renderer::Impl::view_destroy_node(Ogre::SceneNode* node) { scene->destroySceneNode(node); }

// PORT: drawing, a held item's model: its hand node's place and the model's box height; false
// when it is not drawn.
bool Renderer::Impl::view_held_item(const Ogre::Entity* held, Ogre::Vector3& hand,
                                    float& height) const {
    if (held == nullptr || held->getParentNode() == nullptr) return false;
    hand = held->getParentNode()->_getDerivedPosition();
    const Ogre::AxisAlignedBox& box = held->getBoundingBox();
    height = box.isFinite() ? box.getMaximum().y - box.getMinimum().y : 0.0f;
    return true;
}

void Renderer::Impl::convert_unit_body(tl::core::LevelScript* machine, int64_t node,
                                       const std::string& unit) {
    SceneUnit* drawn = scene_unit_of(machine, node);
    if (drawn == nullptr || drawn->entity == nullptr) {
        TL_LOG_WARN("transform: no model for node %lld to change", static_cast<long long>(node));
        return;
    }
    Ogre::Entity* old_entity = drawn->entity;
    Ogre::SceneNode* holder = old_entity->getParentSceneNode();
    if (holder == nullptr) return;
    const std::string mesh_path = unit_mesh_path(unit);
    if (mesh_path.empty()) return;

    // `unitInit`'s model, made on its own node and moved onto the character's, so the node
    // every map holds by the character's id stays.
    MeshPlacement placement;
    placement.mesh = mesh_path;
    placement.position = from_ogre(holder->getPosition());
    placement.orientation = from_ogre(holder->getOrientation());
    placement.is_unit = true;
    Ogre::Entity* entity = place_mesh(placement);
    if (entity == nullptr) return;
    if (Ogre::SceneNode* made = entity->getParentSceneNode()) {
        made->detachObject(entity);
        scene->destroySceneNode(made);
    }

    // `deleteUnitSpecificData`: the old model and every
    // list it was in; the spawner's list keeps the character.
    std::vector<int64_t> owners;
    for (const auto& entry : spawned) {
        for (const SpawnedUnit& made : entry.second) {
            if (made.entity == old_entity) owners.push_back(entry.first);
        }
    }
    const bool was_pet = old_entity == pet_entity;
    forget_unit_entity(old_entity);
    {
        std::vector<Ogre::MovableObject*> held(old_entity->getAttachedObjects().begin(),
                                               old_entity->getAttachedObjects().end());
        old_entity->detachAllObjectsFromBone();
        for (Ogre::MovableObject* object : held) scene->destroyMovableObject(object);
        holder->detachObject(old_entity);
        scene->destroyEntity(old_entity);
    }
    holder->attachObject(entity);
    const float scale = machine != nullptr ? machine->unit_scale(node) : 0.0f;
    const float drawn_scale = scale > 0.0f ? scale : stats_for_unit(unit).scale;
    holder->setScale(drawn_scale, drawn_scale, drawn_scale);

    const tl::core::UnitStats stats = stats_for_unit(unit);
    Ogre::AnimationState* started = nullptr;
    play_scene_animation(entity, mesh_path, kUnitIdleAnimation, /*loops=*/true, 1.0f,
                         /*optional=*/true, &started, stats.armed);
    remember_state(entity, kUnitIdleAnimation, started);
    scene_units.emplace(unit, SceneUnit{entity, mesh_path, 1.0f, node, kUnitIdleAnimation, true,
                                        kUnitIdleAnimation, started, 0.0f, stats.armed});
    scene_units_changed = true;
    for (int64_t owner : owners) spawned[owner].push_back(SpawnedUnit{holder, entity, unit, node});
    std::string right_hand;
    std::string left_hand;
    if (machine != nullptr) machine->unit_hands(node, right_hand, left_hand);
    equip_unit_items(entity, unit, right_hand, left_hand);
    apply_render_always(entity, stats.render_always);
    apply_rim_light(entity, rim_light_texture);
    // `CCharacter::loadModel`: `setRenderBehind(+0x4a1)`, `RENDERBEHIND`.
    set_render_behind(entity, stats.render_behind, outline_hostile(stats));
    if (was_pet) {
        pet_entity = entity;
        pet_mesh = mesh_path;
    }

    // `CParticle::Start` on the new model's particle 8, `TRANSFORM`.
    tl::core::Vec3 feet;
    if (machine != nullptr && machine->unit_position(node, feet)) {
        start_slot_particle(stats, tl::core::character_particle::kTransform,
                            ogre::Vector3(feet.x, feet.y + machine->unit_radius(node), feet.z),
                            character_ref(node, machine));
    }
    TL_LOG_INFO("transform: node %lld drawn as %s (%s)", static_cast<long long>(node), unit.c_str(),
                mesh_path.c_str());
}

// PORT: a spawned unit's model made and stood in the scene: its idle, its held items, its entrance
// (core/world.h). Null when the mesh does not load.
Ogre::Entity* Renderer::Impl::view_spawn_unit_model(const std::string& unit,
                                                    const std::string& mesh_path, int64_t id,
                                                    const ogre::Vector3& at,
                                                    const ogre::Quaternion& facing,
                                                    float placed_scale, bool spawn_anim) {
    MeshPlacement placement;
    placement.mesh = mesh_path;
    placement.node_id = id;
    placement.position = at;
    placement.orientation = facing;
    placement.is_unit = true;
    placement.scale = ogre::Vector3(placed_scale, placed_scale, placed_scale);

    Ogre::Entity* entity = place_mesh(placement);
    if (entity == nullptr) return nullptr;

    const bool armed = stats_for_unit(unit).armed;
    Ogre::AnimationState* started = nullptr;
    play_scene_animation(entity, mesh_path, kUnitIdleAnimation, /*loops=*/true, 1.0f,
                         /*optional=*/true, &started, armed);
    remember_state(entity, kUnitIdleAnimation, started);

    const auto placed =
        scene_units.emplace(unit, SceneUnit{entity, mesh_path, 1.0f, id, kUnitIdleAnimation, true,
                                            kUnitIdleAnimation, started, 0.0f, armed});
    scene_units_changed = true;
    {
        // The authored hands; a rolled one goes on later (`sync_unit_equipment`).
        const tl::core::UnitStats held = stats_for_unit(unit);
        equip_unit_items(entity, unit, held.right_hand, held.left_hand);
        apply_render_always(entity, held.render_always);
        // `CCharacter::loadModel`: `setRenderBehind` with `RENDERBEHIND`;
        // an entrance turns it off below.
        set_render_behind(entity, held.render_behind, outline_hostile(held));
    }
    apply_rim_light(entity, rim_light_texture);

    // And its entrance, over the idle (`SceneUnit::animation`, which step_fidgets() returns
    // to). Its sound is the animation's own `PLAYSOUND` key (step_unit_sounds()).
    if (spawn_anim && begin_spawn_animation(entity, mesh_path, stats_for_unit(unit))) {
        const Fidget& f = fidgets[entity];
        placed->second.playing = f.playing;
        placed->second.playing_state = f.state;
        placed->second.last_time = 0.0f;
    }
    return entity;
}

// PORT: the script's unit scale put on the unit's OGRE node.
void Renderer::Impl::apply_unit_scale(int64_t node) {
    const float scale = script.unit_scale(node);
    if (!(scale > 0.0f)) return;
    for (const auto& entry : spawned) {
        for (const SpawnedUnit& made : entry.second) {
            if (made.id != node || made.node == nullptr) continue;
            made.node->setScale(scale, scale, scale);
            return;
        }
    }
}

// `CUnitSpawner::createParticleSystemAt`: the layout at this
// index used again, a new index loads one (the caller has snapped the point to the ground).
// INVENTED: a used layout is replaced, not restarted.
void Renderer::Impl::view_spawner_particles(int64_t spawner, const std::string& file,
                                            const tl::core::ogre::Vector3& at,
                                            const tl::core::ogre::Quaternion& facing,
                                            uint32_t index) {
    std::map<uint32_t, std::vector<Ogre::SceneNode*>>& layouts = spawner_particles[spawner];
    const auto used = layouts.find(index);
    if (used != layouts.end()) destroy_particle_holders(used->second);
    // A kind-1 `CLayout`: a `CParticle` once particles are cached.
    layouts[index] = place_particle_layout(
        file, at, facing, nullptr,
        renderer_detail::caching_particles() ? ParticleStart::Now : ParticleStart::Live);
}

bool Renderer::Impl::view_begin_spawn_entrance(int64_t id, Ogre::Entity* entity) {
    const auto entrance = fidgets.find(entity);
    if (entrance == fidgets.end() || entrance->second.state == nullptr) return false;
    spawning_units[id] = entrance->second.state;
    // `CCharacter::spawn`: `setRenderBehind(false)` for the entrance;
    // `spawningAI` puts `RENDERBEHIND` back.
    if (const RenderBehind* behind = render_behind_of(entity)) {
        set_render_behind(entity, false, behind->hostile);
    }
    return true;
}

/// `CGenericModel::setAmbient`: every material the model
/// draws with (`+0x1f0`) takes `colour` as ambient and diffuse (`Ogre::Material::setAmbient`
/// and `setDiffuse`, every pass). `seen` keeps a shared material to one write.
void Renderer::Impl::set_model_ambient(Ogre::Entity* entity, const Ogre::ColourValue& colour,
                                       std::set<Ogre::Material*>& seen) {
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        const Ogre::MaterialPtr& mat = entity->getSubEntity(i)->getMaterial();
        if (!mat || !seen.insert(mat.get()).second) continue;
        mat->setAmbient(colour);
        mat->setDiffuse(colour);
    }
}

/// The level's `MATERIAL AMBIENT`, on its room pieces: `CLevelTemplateData::load`
/// reads it (92 each by default, times 1/255 by `fmuls`) and
/// `CLevel::setProjectorPass` hands it to `setAmbient` on
/// every `Room Piece`; MIDWATER and `LIGHT` water too
/// (apply_environment()). MISSING: `LEVELLIGHTING` items (`CLevel::addItem`).
///
void Renderer::Impl::apply_scenery_fill(const tl::formats::AdmSection& rules) {
    const auto channel = [&rules](const char* key) {
        return static_cast<float>(static_cast<int32_t>(rules.integer_or(key, 92))) *
               0.003921568859368563f;
    };
    material_ambient =
        Ogre::ColourValue(channel("MATERIAL AMBIENT RED"), channel("MATERIAL AMBIENT GREEN"),
                          channel("MATERIAL AMBIENT BLUE"), 1.0f);
    std::set<Ogre::Material*> seen;
    for (Ogre::Entity* entity : room_piece_meshes)
        set_model_ambient(entity, material_ambient, seen);
    TL_LOG_INFO("  material ambient (%.3f, %.3f, %.3f) on %zu room piece material(s)",
                static_cast<double>(material_ambient.r), static_cast<double>(material_ambient.g),
                static_cast<double>(material_ambient.b), seen.size());
}

namespace renderer_detail {
/// A screen quad of the light map: `Ogre::Rectangle2D` with texture coordinates, bounds
/// +-100000 as makes them (never culled).
std::unique_ptr<Ogre::Rectangle2D> light_map_quad(float half, const Ogre::MaterialPtr& material,
                                                  Ogre::uint8 group) {
    auto quad = std::make_unique<Ogre::Rectangle2D>(true);
    quad->setCorners(-half, half, half, -half);
    quad->setBoundingBox(Ogre::AxisAlignedBox(Ogre::Vector3(-100000.0f), Ogre::Vector3(100000.0f)));
    quad->setMaterial(material);
    quad->setRenderQueueGroup(group);
    return quad;
}

/// `RttLight` and `RttLightMask`: one pass,
/// unlit, fog off, a single texture, no depth check or write, and the scene
/// blend given.
Ogre::MaterialPtr light_map_material(const std::string& name, Ogre::SceneBlendType blend) {
    auto& materials = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr material = materials.getByName(name, Ogre::RGN_INTERNAL);
    if (!material) material = materials.create(name, Ogre::RGN_INTERNAL);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setFog(true, Ogre::FOG_NONE);
    pass->removeAllTextureUnitStates();
    pass->createTextureUnitState();
    pass->setDepthWriteEnabled(false);
    pass->setDepthCheckEnabled(false);
    material->setSceneBlending(blend);
    return material;
}

bool ShadowCasters::renderableQueued(Ogre::Renderable* rend, Ogre::uint8 group, Ogre::ushort,
                                     Ogre::Technique** technique, Ogre::RenderQueue*) {
    if (group == kShadowFadeGroup) return true;
    auto* sub = dynamic_cast<Ogre::SubEntity*>(rend);
    if (sub == nullptr || technique == nullptr) return false;
    Ogre::Entity* entity = sub->getParent();
    if (!entity->getCastShadows()) return false;
    const float opacity = entity_opacity(entity);
    if (!(opacity > 0.0f)) return false;
    // `floorf(opacity * 12)` less one, 0 when not above 0.
    int level = static_cast<int>(std::floor(opacity * 12.0f)) - 1;
    if (level <= 0) level = 0;
    if (level >= kShadowLevels) level = kShadowLevels - 1;
    unsigned bones = 0;
    unsigned weights = 0;
    const bool skinned = gpu_ != nullptr && gpu_->gpu_skin(rend, bones, weights);
    const Ogre::MaterialPtr mat = material(level, skinned ? bones : 0u, skinned ? weights : 0u);
    if (!mat) return false;
    Ogre::Technique* best = mat->getBestTechnique(0, rend);
    if (best == nullptr && mat->getCompilationRequired()) {
        // The shader generator rewrote every shader since: touched so it compiles
        // (compile_waiting_material()).
        mat->touch();
        best = mat->getBestTechnique(0, rend);
    }
    if (best == nullptr) return false;
    *technique = best;
    return true;
}

/// `CGame::createMaterials`: `RenderShadow<i>`,
/// ambient and diffuse black, emissive 0.5 less a 24th a level (`fnmsubs`), no depth
/// check or write, no lights (`+0xce`, `setMaxSimultaneousLights(0)`).
Ogre::MaterialPtr ShadowCasters::material(int level, unsigned bones, unsigned weights) {
    const auto key = std::make_tuple(level, bones, weights);
    const auto found = materials_.find(key);
    if (found != materials_.end()) return found->second;
    std::string name = "tl/RenderShadow" + std::to_string(level);
    if (weights > 0) name += "/skin" + std::to_string(bones) + "x" + std::to_string(weights);
    auto& manager = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = manager.getByName(name, Ogre::RGN_INTERNAL);
    if (!mat) {
        mat = manager.create(name, Ogre::RGN_INTERNAL);
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        const float glow = std::fma(-static_cast<float>(level), 0.0416666679f, 0.5f);
        pass->setAmbient(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
        pass->setDiffuse(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
        pass->setSelfIllumination(glow, glow, glow);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setMaxSimultaneousLights(0);
        mat->load();
        // Its shaders made now (the scheme resolver does not build internal materials); a
        // skinned twin's by GpuSkinning::skin_material().
        auto* generator = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        if (weights == 0 && generator != nullptr &&
            generator->createShaderBasedTechnique(*mat, Ogre::MSN_DEFAULT, Ogre::MSN_SHADERGEN)) {
            generator->validateMaterial(Ogre::MSN_SHADERGEN, *mat);
        }
    }
    if (weights > 0 && (gpu_ == nullptr || !gpu_->skin_material(mat, bones, weights))) {
        // No twin to skin this one: refused, as the bind pose would be wrong.
        mat.reset();
    }
    materials_[key] = mat;
    return mat;
}

namespace {
/// The key `set_render_behind` keeps an entity's state under.
const char* const kRenderBehindKey = "tl_render_behind";
}  // namespace

void set_render_behind(Ogre::Entity* entity, bool on, bool hostile) {
    if (entity == nullptr) return;
    entity->getUserObjectBindings().setUserAny(kRenderBehindKey,
                                               Ogre::Any(RenderBehind{on, hostile}));
    // `CCharacter::setRenderBehind`: queue 0x32 on, 0x58 off (0x5b with
    // `+0x216`, never set here).
    entity->setRenderQueueGroup(static_cast<Ogre::uint8>(
        on ? static_cast<unsigned>(Ogre::RENDER_QUEUE_MAIN) : kCharacterGroup));
    // And each equipped item's model (`CEquipment::setRenderBehind`).
    for (Ogre::MovableObject* attached : entity->getAttachedObjects()) {
        if (auto* held = dynamic_cast<Ogre::Entity*>(attached))
            set_render_behind(held, on, hostile);
    }
}

const RenderBehind* render_behind_of(const Ogre::Entity* entity) {
    if (entity == nullptr) return nullptr;
    const Ogre::Any& held = entity->getUserObjectBindings().getUserAny(kRenderBehindKey);
    return held.has_value() ? Ogre::any_cast<RenderBehind>(&held) : nullptr;
}

bool render_behind_swap(Ogre::Renderable* rend, Ogre::ushort priority, Ogre::Technique** technique,
                        Ogre::RenderQueue* queue, RenderBehindMaterials& materials,
                        GpuSkinning* gpu) {
    auto* sub = dynamic_cast<Ogre::SubEntity*>(rend);
    if (sub == nullptr || technique == nullptr || *technique == nullptr ||
        (*technique)->getNumPasses() == 0) {
        return false;
    }
    Ogre::Entity* entity = sub->getParent();
    const RenderBehind* state = render_behind_of(entity);
    // The record's on flag and its opacity above 0.
    if (state == nullptr || !state->on || !(entity_opacity(entity) > 0.0f)) return false;
    // The record's `[0]`: `setLightOverride` sets it while the unit's
    // light^4 is 0.4 or less: none in the dark.
    if (entity_outline_hidden(entity)) return false;
    unsigned bones = 0;
    unsigned weights = 0;
    const bool skinned = gpu != nullptr && gpu->gpu_skin(rend, bones, weights);
    const Ogre::MaterialPtr mat =
        materials.material(state->hostile, skinned ? bones : 0u, skinned ? weights : 0u);
    if (!mat) return false;
    Ogre::Technique* best = mat->getBestTechnique(0, rend);
    if (best == nullptr && mat->getCompilationRequired()) {
        mat->touch();
        best = mat->getBestTechnique(0, rend);
    }
    if (best == nullptr) return false;
    // The body on 0x58 with its technique, then the render-behind
    // technique in its place (`+0x3c`'s).
    queue->getQueueGroup(static_cast<Ogre::uint8>(kCharacterGroup))
        ->addRenderable(rend, *technique, priority);
    *technique = best;
    return true;
}

/// `CGenericModel::reInitialize`:
/// `rbmat_N`, ambient and diffuse black, no depth write, no fog, `outlineblue.dds`
/// (`outlinered.dds` for a hostile unit, `setRenderBehindTexture`,
/// `CCharacter::setAlignment`) as a curved environment map, depth test `GREATER`, blended
/// `SBT_ADD`; skinned via GpuSkinning::skin_material. Self-illumination 1, scaled by the
/// renderable's light.
Ogre::MaterialPtr RenderBehindMaterials::material(bool hostile, unsigned bones, unsigned weights) {
    const auto key = std::make_tuple(hostile, bones, weights);
    const auto found = materials_.find(key);
    if (found != materials_.end()) return found->second;
    std::string name = std::string("tl/RenderBehind/") + (hostile ? "red" : "blue");
    if (weights > 0) name += "/skin" + std::to_string(bones) + "x" + std::to_string(weights);
    auto& manager = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = manager.getByName(name, Ogre::RGN_INTERNAL);
    if (!mat) {
        mat = manager.create(name, Ogre::RGN_INTERNAL);
        Ogre::Technique* technique = mat->getTechnique(0);
        Ogre::Pass* pass = technique->getPass(0);
        pass->setAmbient(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
        pass->setDiffuse(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
        pass->setSelfIllumination(1.0f, 1.0f, 1.0f);
        pass->setFog(true, Ogre::FOG_NONE);
        pass->setDepthWriteEnabled(false);
        pass->setDepthFunction(Ogre::CMPF_GREATER);
        pass->setSceneBlending(Ogre::SBT_ADD);
        const std::string path =
            resolve_archive_path(hostile ? "media/sharedtextures/outlinered.dds"
                                         : "media/sharedtextures/outlineblue.dds");
        Ogre::TextureUnitState* unit = pass->createTextureUnitState();
        if (!path.empty()) unit->setTextureName(path);
        unit->setEnvironmentMap(true, Ogre::TextureUnitState::ENV_CURVED);
        mat->load();
        auto* generator = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        if (weights == 0 && generator != nullptr &&
            generator->createShaderBasedTechnique(*mat, Ogre::MSN_DEFAULT, Ogre::MSN_SHADERGEN)) {
            generator->validateMaterial(Ogre::MSN_SHADERGEN, *mat);
        }
    }
    if (weights > 0 && (gpu_ == nullptr || !gpu_->skin_material(mat, bones, weights))) {
        mat.reset();
    }
    materials_[key] = mat;
    return mat;
}

/// `TextureUnitState::setTextureName(name, TEX_TYPE_2D)` on a light map material, from the
/// rules' archive path (`CGameClient::refreshLighting`); a
/// missing path leaves it.
void set_light_map_texture(const Ogre::MaterialPtr& material, const std::string& reference) {
    const std::string path = resolve_archive_path(reference);
    if (path.empty()) {
        TL_LOG_WARN("  light map texture '%s' is not in the archive", reference.c_str());
        return;
    }
    Ogre::TextureUnitState* unit = material->getTechnique(0)->getPass(0)->getTextureUnitState(0);
    unit->setTextureName(path);
    unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
}

}  // namespace renderer_detail
}  // namespace tl::render

namespace tl::render {

// PORT: a mesh's material scripts parsed before it is drawn (core/world.h).
void Renderer::Impl::view_prepare_mesh(const std::string& mesh_path) {
    parse_sibling_material(mesh_path);
}

}  // namespace tl::render
