// The renderer's implementation: script stepping, fidgets, pets, unit sounds, automap, the main loop.
// Split from renderer.cpp; the shared
// declarations are in renderer_impl.h.
#include "core/section_profile.h"
#include "render/gl_thread.h"
#include "render/renderer_impl.h"

#if defined(TL_PLATFORM_ANDROID)
#include "platform/android/android_glue.h"

#include <OgreAndroidEGLWindow.h>
#endif

namespace tl::render {
float Renderer::Impl::scene_animation_length(const std::string& unit,
                                             const std::string& animation) {
    if (names_the_bind_pose(animation)) return -1.0f;
    const auto units = scene_units.equal_range(unit);
    if (units.first == units.second) {
        // An `Animation Controller` on the player: the character's own file.
        if (unit == "PLAYER" && subject != nullptr &&
            !archive_path_in_dir(animation + ".SKELETON", subject_dir).empty() &&
            link_subject_animation(animation)) {
            const std::string here = animation_named(subject, animation);
            if (!here.empty()) return subject->getAnimationState(here)->getLength();
        }
        return -1.0f;
    }
    const SceneUnit& placed = units.first->second;
    if (placed.entity == nullptr || !placed.entity->hasSkeleton()) return -1.0f;

    const std::string dir = dir_of_asset(placed.mesh);
    std::string path = archive_path_in_dir(animation + ".SKELETON", dir);
    if (path.empty()) path = archive_animation_in_dir(dir, animation, placed.armed, 0);
    if (path.empty()) return -1.0f;
    try {
        const Ogre::SkeletonPtr source = Ogre::static_pointer_cast<Ogre::Skeleton>(
            Ogre::SkeletonManager::getSingleton().load(path, kGameGroup));
        if (!source || source->getNumAnimations() == 0) return 0.0f;
        return source->getAnimation(0)->getLength();
    } catch (const Ogre::Exception&) {
        return -1.0f;
    }
}

std::vector<std::pair<std::string, float>> Renderer::Impl::animation_catalog(
    const std::string& unit) {
    const auto units = scene_units.equal_range(unit);
    std::string mesh;
    if (units.first != units.second) {
        mesh = units.first->second.mesh;
    } else {
        for (const auto& owner : spawned) {
            for (const SpawnedUnit& made : owner.second) {
                if (made.unit == unit && made.entity != nullptr && made.entity->getMesh()) {
                    mesh = made.entity->getMesh()->getName();
                }
            }
        }
    }
    if (mesh.empty()) return {};
    const auto cached = animation_catalogs.find(mesh);
    if (cached != animation_catalogs.end()) return cached->second;
    std::vector<std::pair<std::string, float>> catalog;
    const std::string dir = dir_of_asset(mesh);
    for (const std::string& stem : manifest_for(mesh).order()) {
        float length = 0.0f;
        const std::string path = archive_path_in_dir(stem + ".SKELETON", dir);
        if (!path.empty()) {
            try {
                const Ogre::SkeletonPtr source = Ogre::static_pointer_cast<Ogre::Skeleton>(
                    Ogre::SkeletonManager::getSingleton().load(path, kGameGroup));
                if (source && source->getNumAnimations() > 0) {
                    length = source->getAnimation(0)->getLength();
                }
            } catch (const Ogre::Exception& e) {
                // Listed at length 0, as before; said once rather than silently.
                TL_LOG_WARN("animation catalog: %s: %s", path.c_str(), e.getDescription().c_str());
            }
        }
        catalog.emplace_back(stem, length);
    }
    animation_catalogs.emplace(mesh, catalog);
    return catalog;
}

// PORT: whether a machine's unit is inside the camera's view (OGRE culling boxes).
bool Renderer::Impl::unit_in_view(const tl::core::LevelScript& machine, int64_t node) {
    if (camera == nullptr) return true;
    NodeMap* nodes = nodes_of(const_cast<tl::core::LevelScript*>(&machine));
    if (nodes == nullptr) return true;
    const auto range = nodes->equal_range(node);
    bool bounded = false;
    for (auto it = range.first; it != range.second; ++it) {
        const Ogre::AxisAlignedBox box = culling_box(it->second, /*from_model=*/true);
        if (box.isNull()) continue;
        bounded = true;
        if (camera->isVisible(box)) return true;
    }
    return !bounded;
}

namespace {
/// The model a unit's node draws: an entity on the node, else on its first
/// child that has one.
Ogre::Entity* model_entity(Ogre::SceneNode* node) {
    for (Ogre::MovableObject* object : node->getAttachedObjects()) {
        if (auto* entity = dynamic_cast<Ogre::Entity*>(object)) return entity;
    }
    for (Ogre::Node* child : node->getChildren()) {
        auto* scene_child = dynamic_cast<Ogre::SceneNode*>(child);
        if (scene_child == nullptr) continue;
        for (Ogre::MovableObject* object : scene_child->getAttachedObjects()) {
            if (auto* entity = dynamic_cast<Ogre::Entity*>(object)) return entity;
        }
    }
    return nullptr;
}
}  // namespace

// The unit's culling box, as CCharacter::update and CItem::update
// hand Camera::isVisible. CCharacter::loadModel and
// CTriggerUnit::unitInit size it from the model (0.25
// wider each way, 1 taller, squared about the origin); others keep CBaseUnit::CBaseUnit's.
// CBaseUnit::updateCullingBounds
// boxes its corners through the transform. INVENTED: a character's box starts
// from the mesh's bounds (Mesh::getBounds), not its collision model's.
Ogre::AxisAlignedBox Renderer::Impl::culling_box(Ogre::SceneNode* node, bool from_model) const {
    if (node == nullptr) return Ogre::AxisAlignedBox();
    Ogre::Vector3 lo(-0.75f, 0.0f, -0.75f);
    Ogre::Vector3 hi(0.75f, 2.0f, 0.75f);
    Ogre::SceneNode* frame = node;
    if (from_model) {
        Ogre::Entity* entity = model_entity(node);
        if (entity == nullptr) return node->_getWorldAABB();
        if (entity->getParentSceneNode() != nullptr) frame = entity->getParentSceneNode();
        const Ogre::AxisAlignedBox& bounds = authored_mesh_bounds(entity);
        if (!bounds.isFinite()) return node->_getWorldAABB();
        const Ogre::Vector3 mn = bounds.getMinimum() - Ogre::Vector3(0.25f, 0.0f, 0.25f);
        const Ogre::Vector3 mx = bounds.getMaximum() + Ogre::Vector3(0.25f, 1.0f, 0.25f);
        const float r = std::max(std::max(-mn.x, mx.x), std::max(-mn.z, mx.z));
        lo = Ogre::Vector3(-r, mn.y, -r);
        hi = Ogre::Vector3(r, mx.y, r);
    }
    const Ogre::Affine3& to_world = frame->_getFullTransform();
    Ogre::AxisAlignedBox box;
    for (int corner = 0; corner < 8; ++corner) {
        const Ogre::Vector3 local((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                                  (corner & 4) ? hi.z : lo.z);
        box.merge(to_world * local);
    }
    return box;
}

// PORT: one linked machine's frame in step_script(): stepped while it runs, its characters
// while it waits.
void Renderer::Impl::step_linked_machine(NestedScript& machine, float delta, float speed,
                                         const tl::core::Defender& player_defense,
                                         const Ogre::Vector3& at) {
    const tl::core::Vec3 player_at{at.x, at.y, at.z};
    std::vector<tl::core::ScriptEvent> inner;
    machine.script.set_game_speed(speed);
    machine.script.set_player_defense(player_defense, player.body().radius);
    machine.script.set_player_effects(player_total());
    machine.script.set_player_hp(player_health.current, player_health.max);
    if (!machine.running) {
        // Its wires wait for a `Play`, but a character it placed is one of
        // the level's (`CLevel::updateCharacters`
        // walks them all).
        if (machine.script.has_characters()) {
            tell_player_motion(machine.script);
            machine.script.update_units(delta, player_at, quest_log.state(), inner);
            flush_logic_trace(machine.script);
            if (!inner.empty()) apply_script_events(inner, machine.nodes);
        }
        return;
    }
    tell_player_motion(machine.script);
    machine.script.update(delta, player_at, quest_log.state(), inner);
    flush_logic_trace(machine.script);

    // A machine with no wires, triggers, playback or characters stops being stepped
    // (a spent CAST SKILL ON DEATH layout); a Play on a Layout Link Timeline re-arms it.
    if (!machine.script.has_wires() && machine.script.trigger_count() == 0 &&
        !machine.script.timelines_playing() && !machine.script.has_characters() &&
        !machine.script.has_sound_nodes()) {
        machine.running = false;
    }

    if (inner.empty()) return;
    if (!machine.reported) {
        machine.reported = true;
        TL_LOG_INFO("script: linked machine '%s' is running (%zu event(s), %zu node(s))",
                    machine.owner_name.c_str(), inner.size(), machine.nodes.size());
    }
    apply_script_events(inner, machine.nodes);
}

void Renderer::Impl::step_script(float delta) {
    // `CGame::update` keeps the frame rate a
    // body's deletion asks.
    average_fps.add(delta);
    for (tl::core::LevelScript* machine : level_machines)
        machine->set_average_fps(average_fps.value());
    wake_formations();
    sync_unit_equipment();

    // `CCharacter::spawningAI`: out of the
    // entrance once its animation is no longer playing
    // (`animationPlayingSubstring`).
    for (auto it = spawning_units.begin(); it != spawning_units.end();) {
        const Ogre::AnimationState* entrance = it->second;
        if (entrance->getEnabled() && entrance->getTimePosition() < entrance->getLength()) {
            ++it;
            continue;
        }
        script.set_spawning(it->first, false);
        // And `setRenderBehind(RENDERBEHIND)`.
        for (const auto& [owner, units] : spawned) {
            for (const SpawnedUnit& unit : units) {
                if (unit.id != it->first || unit.entity == nullptr) continue;
                const tl::core::UnitStats stats = stats_for_unit(unit.unit);
                set_render_behind(unit.entity, stats.render_behind, outline_hostile(stats));
            }
        }
        it = spawning_units.erase(it);
    }

    // A generated floor's own script is empty (place_dungeon_floor()), but its rooms arm
    // their own machines (place_layout()), so no wires here is not "nothing to step".
    const bool any_nested_running =
        std::any_of(nested_scripts.begin(), nested_scripts.end(), [](const NestedScript& machine) {
            return machine.running || machine.script.has_characters();
        });
    if (script.wire_count() == 0 && script.trigger_count() == 0 && !script.has_characters() &&
        !script.has_sound_nodes() && !any_nested_running) {
        return;
    }

    // Where the triggers are tested against. Without player control there is no
    // character to walk into one, so the subject's own position stands in -
    // which is what makes `--dump-at` able to show a scripted scene at all.
    const Ogre::Vector3 at = player_control && subject_node ? subject_node->getPosition()
                             : subject_node                 ? subject_node->getPosition()
                                                            : Ogre::Vector3::ZERO;

    std::vector<tl::core::ScriptEvent> events;
    // What a monster's blow meets on the player: `CCharacter::AC()`, `maxHP()`,
    // `getBlockChance()` - for every machine, a room's units strike too.
    const tl::core::Defender player_defense = player_defender();
    script.set_player_defense(player_defense, player.body().radius);
    script.set_player_effects(player_total());
    script.set_player_hp(player_health.current, player_health.max);
    tell_player_motion(script);
    const float speed = game_speed.speed() * 1.0f;
    script.set_game_speed(speed);  // the level runs at the game's speed
    script.update(delta, tl::core::Vec3{at.x, at.y, at.z}, quest_log.state(), events);
    sound_listener = tl::core::Vec3{at.x, at.y, at.z};
    flush_logic_trace(script);
    // The player's position, as the gdb trace of the original samples it: every
    // 0.1 s of the script's clock, when it moved.
    if (logic_trace != nullptr && script.clock() >= logic_trace_next_sample) {
        logic_trace_next_sample = script.clock() + 0.1f;
        const float rx = std::round(at.x * 100.0f), ry = std::round(at.y * 100.0f),
                    rz = std::round(at.z * 100.0f);
        if (rx != logic_trace_last.x || ry != logic_trace_last.y || rz != logic_trace_last.z) {
            logic_trace_last = Ogre::Vector3(rx, ry, rz);
            std::fprintf(logic_trace, "%.3f P PLAYER %.2f %.2f %.2f",
                         static_cast<double>(script.clock()), static_cast<double>(at.x),
                         static_cast<double>(at.y), static_cast<double>(at.z));
            std::fputc(10, logic_trace);
        }
    }

    // Units back to their idle once a scripted one-shot has run.
    for (size_t i = 0; i < pending_idles.size();) {
        const PendingIdle pending = pending_idles[i];
        if (pending.state != nullptr && pending.state->getEnabled() &&
            pending.state->getTimePosition() + 1e-4f < pending.state->getLength()) {
            ++i;
            continue;
        }
        pending_idles.erase(pending_idles.begin() + static_cast<long>(i));
        const auto units = scene_units.equal_range(pending.unit);
        for (auto it = units.first; it != units.second; ++it) {
            if (it->second.node != pending.node || it->second.playing_state != pending.state) {
                continue;
            }
            // Already taken over by the animator's queue.
            if (pending.queued != nullptr && pending.queued->getEnabled()) {
                remember_state(it->second.entity, pending.animation, pending.queued);
                it->second.playing = pending.animation;
                it->second.playing_state = pending.queued;
                it->second.last_time = 0.0f;
                continue;
            }
            // The queued animation, or - where the model has none of it (a
            // unit with no `JUMP_LAND`) - the next one queued behind it.
            std::string animation = pending.animation;
            bool loops = pending.loops;
            size_t next = 0;
            while (true) {
                Ogre::AnimationState* idle = nullptr;
                if (play_scene_animation(it->second.entity, it->second.mesh, animation, loops,
                                         it->second.rate, /*optional=*/true, &idle,
                                         it->second.armed)) {
                    if (pending.state != nullptr) pending.state->setEnabled(false);
                    remember_state(it->second.entity, animation, idle);
                    it->second.playing = animation;
                    it->second.playing_state = idle;
                    it->second.last_time = 0.0f;
                    if (next < pending.after.size() && idle != nullptr) {
                        PendingIdle then;
                        then.node = pending.node;
                        then.unit = pending.unit;
                        then.state = idle;
                        then.animation = pending.after[next].first;
                        then.loops = pending.after[next].second;
                        then.after.assign(pending.after.begin() + static_cast<long>(next) + 1,
                                          pending.after.end());
                        pending_idles.push_back(then);
                    }
                    break;
                }
                if (next >= pending.after.size()) break;
                animation = pending.after[next].first;
                loops = pending.after[next].second;
                ++next;
            }
        }
    }

    // One-shot animations that have run out. Checked here rather than by the
    // machine, which has no idea how long an animation is.
    for (size_t i = 0; i < pending_animations.size();) {
        const PendingAnimation& pending = pending_animations[i];
        if (pending.state->getTimePosition() + 1e-4f < pending.state->getLength()) {
            ++i;
            continue;
        }
        script.animation_finished(pending.controller, quest_log.state(), events);
        pending_animations.erase(pending_animations.begin() + static_cast<long>(i));
    }

    // `CTriggerUnit::resetTrigger` at the end
    // of `unitInit`: the first state's layout, unless the prop has been fired
    // since it was placed.
    if (!pending_trigger_layouts.empty()) {
        const std::vector<std::pair<Ogre::Entity*, std::string>> waiting =
            std::move(pending_trigger_layouts);
        pending_trigger_layouts.clear();
        for (const auto& prop_of : waiting) {
            if (trigger_state_layouts.count(prop_of.first) != 0) continue;
            TriggerProp prop;
            if (trigger_prop(prop_of.second, prop)) {
                set_trigger_state_layout(prop_of.first, prop, 1);
            }
        }
    }

    // A trigger prop whose opening transition has run out: into the state's own
    // loop. Same reason as above - only the renderer knows when an animation is
    // over.
    for (size_t i = 0; i < pending_trigger_loops.size();) {
        const PendingTriggerLoop pending = pending_trigger_loops[i];
        if (pending.transition == nullptr ||
            pending.transition->getTimePosition() + 1e-4f < pending.transition->getLength()) {
            ++i;
            continue;
        }
        pending_trigger_loops.erase(pending_trigger_loops.begin() + static_cast<long>(i));
        const auto units = scene_units.equal_range(pending.unit);
        for (auto it = units.first; it != units.second; ++it) {
            if (it->second.node != pending.node) continue;
            Ogre::AnimationState* next = nullptr;
            if (!play_scene_animation(it->second.entity, it->second.mesh, pending.next,
                                      pending.loops, it->second.rate, /*optional=*/true, &next,
                                      it->second.armed)) {
                continue;
            }
            pending.transition->setEnabled(false);
            remember_state(it->second.entity, pending.next, next);
            it->second.playing = pending.next;
            it->second.playing_state = next;
            it->second.last_time = 0.0f;
            // A later step waits on *this* animation, not on the one that has
            // just finished: the chain is transition -> state -> loop.
            for (PendingTriggerLoop& later : pending_trigger_loops) {
                if (later.node == pending.node && later.transition == pending.transition) {
                    later.transition = next;
                }
            }
        }
    }

    if (!events.empty()) apply_script_events(events, scripted_nodes);

    // Every started linked machine, each against its own node map. By index: applying
    // events can link another layout and push_back invalidates iterators; one linked
    // during the pass is first stepped next frame.
    const size_t machines = nested_scripts.size();
    for (size_t index = 0; index < machines; ++index) {
        NestedScript& machine = nested_scripts[index];
        if (!machine.retired) step_linked_machine(machine, delta, speed, player_defense, at);
    }

    // A scripted camera has to be re-aimed every frame, and follow_subject() is
    // otherwise only reached through the player controller - so a cutscene
    // would hold its first frame's aim in any run without --play.
    if (scripted_camera.active) {
        // A `Camera Controller` is a layout descriptor: `updateLayouts`' time.
        scripted_camera.elapsed += delta * speed;
        follow_subject();
    }
}

/// Keep the character on the animation its state asks for (outside the run loop, so
/// headless runs animate too).
// PORT: the animation `wanted_player_animation` chooses put on the player's
// OGRE entities; a death or a scripted one-shot holds its last frame
// (`blendAnimation(DIE, ..., 1.0, -1)`).
void Renderer::Impl::follow_player_animation() {
    if (!player_control) return;
    const std::string wanted = wanted_player_animation();
    if (wanted == player_animation) return;

    // From the top of the new animation. Cross-fading between Idle and Run is
    // what this wants next and is a blend the animation code does not do yet; a
    // hard cut is honest about that.
    const bool applied = apply_subject_animation(wanted, 0.0f, /*quiet=*/true);
    // `blendAnimation(DIE, ..., 1.0, -1)` plays once and holds its last frame.
    const bool scripted_once = !player_death.dying && !scripted_player_animation.empty() &&
                               wanted == scripted_player_animation && !scripted_player_loops;
    if (applied && (player_death.dying || scripted_once)) {
        for (Ogre::Entity* entity : animated()) {
            const std::string here = animation_named(entity, wanted);
            if (!here.empty()) entity->getAnimationState(here)->setLoop(false);
        }
    }
    if (!applied) {
        // Say it once rather than every frame. A missing Run is the difference
        // between a character running and one sliding along in its idle pose,
        // and that is worth a log line.
        TL_LOG_WARN("no '%s' animation on the subject; keeping '%s'", wanted.c_str(),
                    player_animation.c_str());
    }
    player_animation = wanted;
}

bool Renderer::Impl::begin_spawn_animation(Ogre::Entity* entity, const std::string& mesh,
                                           const tl::core::UnitStats& stats) {
    if (entity == nullptr || !entity->hasSkeleton() || stats.spawn_anim.empty()) return false;

    // animationExistsSubstring() then findRandomAnimation(), as CCharacter::spawn() does:
    // the spawn class matches as a substring, a roll when several match.
    const std::string dir = dir_of_asset(mesh);
    if (dir.empty()) return false;
    const std::vector<std::string> stems = archive_animation_stems_matching(dir, stats.spawn_anim);
    if (stems.empty()) return false;

    const size_t which = std::min<size_t>(
        stems.size() - 1, static_cast<size_t>(tl::core::volatile_random().between(0.0f, 1.0f) *
                                              static_cast<float>(stems.size())));
    const std::string& stem = stems[which];

    Ogre::AnimationState* started = nullptr;
    if (!play_scene_animation(entity, mesh, stem, /*loops=*/false, 1.0f, /*optional=*/true,
                              &started, stats.armed) ||
        started == nullptr) {
        return false;
    }
    // Once, for the reason begin_fidget() spells out: a looping entrance never
    // reaches its end, so step_fidgets() waits for it for ever and the unit is
    // stuck half out of the chute.
    started->setLoop(false);
    remember_state(entity, stem, started);

    // Handed to the fidget table, whose per-frame loop puts the unit back on its idle.
    Fidget& f = fidgets[entity];
    f.state = started;
    f.playing = stem;
    f.since = 0.0f;
    TL_LOG_INFO("spawn: %s enters on '%s' (%.2fs)",
                stats.display_name.empty() ? stem.c_str() : stats.display_name.c_str(),
                stem.c_str(), static_cast<double>(started->getLength()));
    return true;
}

bool Renderer::Impl::begin_fidget(Fidget& fidget, Ogre::Entity* entity, const std::string& dir,
                                  bool is_subject, const std::string& mesh, bool armed) {
    if (!fidget.looked_up) {
        fidget.looked_up = true;
        fidget.available = archive_animation_stems_matching(dir, kFidgetWord);
    }
    if (fidget.available.empty()) return false;

    // `findRandomAnimation("FIDGET")` in the original.
    const size_t which =
        std::min<size_t>(fidget.available.size() - 1,
                         static_cast<size_t>(tl::core::volatile_random().between(0.0f, 1.0f) *
                                             static_cast<float>(fidget.available.size())));
    const std::string stem = fidget.available[which];

    if (is_subject) {
        fidget.back_to = playing.empty() ? std::string(kUnitIdleAnimation) : playing;
        // Linked onto the skeleton first (link_subject_animation() remembers); a file that
        // will not link or play is dropped, not retried.
        if (!link_subject_animation(stem) || !apply_subject_animation(stem, 0.0f, /*quiet=*/true)) {
            fidget.available.erase(fidget.available.begin() + static_cast<long>(which));
            return false;
        }
        // Once, not looping: `blendAnimation(idx, false, ...)`. Without this the
        // state wraps at its end and the switch back below is a frame late,
        // which reads as a twitch.
        for (Ogre::Entity* piece : animated()) {
            const std::string here = animation_named(piece, stem);
            if (!here.empty()) piece->getAnimationState(here)->setLoop(false);
        }
        const std::string here = animation_named(subject, stem);
        fidget.state = here.empty() ? nullptr : subject->getAnimationState(here);
        fidget.playing = stem;
        if (fidget.state != nullptr) {
            TL_LOG_INFO("fidget: the character plays '%s' (%.2fs)", stem.c_str(),
                        static_cast<double>(fidget.state->getLength()));
        }
        return fidget.state != nullptr;
    }

    // `CMonster::idleAI`: `blendAnimation(fidget, false,
    // 0.2, 1.0)`.
    Ogre::AnimationState* started = nullptr;
    if (!play_scene_animation(entity, mesh, stem, /*loops=*/false, 1.0f, /*optional=*/true,
                              &started, armed, nullptr, /*blend=*/0.2f) ||
        started == nullptr) {
        fidget.available.erase(fidget.available.begin() + static_cast<long>(which));
        return false;
    }
    // Forced, not trusted: a looping fidget never ends and step_fidgets() would wait for
    // ever.
    started->setLoop(false);
    remember_state(entity, stem, started);
    fidget.state = started;
    fidget.playing = stem;
    TL_LOG_INFO("fidget: %s plays '%s' (%.2fs)", mesh.c_str(), stem.c_str(),
                static_cast<double>(started->getLength()));
    return true;
}

/// One frame of the idle fidgets, for the subject and everyone the level placed (numbers:
/// Impl::Fidget). Called from advance(), so menus and the game all get it.
void Renderer::Impl::step_fidgets(float delta, float world_delta) {
    if (!(delta > 0.0f)) return;
    const float chance = 1.0f - std::exp(-kFidgetPerSecond * delta);

    // The subject: the character on the menu, on the creation screen, and the
    // one the player is steering.
    if (subject != nullptr && subject->hasSkeleton() && !subject_dir.empty()) {
        Fidget& f = fidgets[subject];
        f.since += delta;
        if (f.state != nullptr) {
            // Anything else taking the skeleton - breaking into a run, or a
            // swing - disables the state, and that ends the fidget rather than
            // leaving it to be waited on for ever.
            const bool still_ours = f.state->getEnabled();
            if (!still_ours || f.state->getTimePosition() + 1e-4f >= f.state->getLength()) {
                if (still_ours) {
                    const std::string back = player_control ? wanted_player_animation() : f.back_to;
                    apply_subject_animation(back, 0.0f, /*quiet=*/true);
                    if (player_control) player_animation = back;
                }
                f.state = nullptr;
                f.playing.clear();
                f.since = 0.0f;
            }
        } else {
            // Standing still and not mid-swing (idleAI's fidget needs no path and idle
            // AI); not dying (AI state 5), fishing (0x20..0x22) or casting (castSkill
            // AI state 0xe) - none reach idleAI.
            const bool still = !swing.active && !player_death.dying &&
                               !(player_control && player_performing_skill()) &&
                               fishing_state == tl::core::FishingState::Off &&
                               (!player_control || (!player.moving() && !player.following()));
            if (still && f.since > kFidgetCooldown &&
                tl::core::volatile_random().between(0.0f, 1.0f) < chance) {
                begin_fidget(f, subject, subject_dir, /*is_subject=*/true, std::string(),
                             /*armed=*/true);
            }
        }
    }

    // And everybody the level placed; hidden or busy units skip. The level's characters
    // and the pet run on the game's speed, the player on the frame's.
    //
    const float world_chance = 1.0f - std::exp(-kFidgetPerSecond * world_delta);
    for (auto& entry : scene_units) {
        SceneUnit& unit = entry.second;
        if (unit.entity == nullptr || !unit.entity->isVisible()) continue;

        // The dead do not fidget: CMonster::fidgetAI's first line (CCharacter::alive).
        // `targetable` is this engine's alive(): posed corpses and
        // LevelScript::unit_killed() both clear it.
        if (!node_targetable(unit.node, unit.entity->getParentSceneNode())) continue;

        Fidget& f = fidgets[unit.entity];
        f.since += world_delta;

        const std::string idle =
            unit.animation.empty() ? std::string(kUnitIdleAnimation) : unit.animation;

        if (f.state != nullptr) {
            const bool still_ours = f.state->getEnabled();
            if (still_ours && f.state->getTimePosition() + 1e-4f < f.state->getLength()) {
                continue;
            }
            // Something else took the skeleton (gait, swing, scripted animation): that ends
            // the fidget; do not put the resting animation back over it.
            if (!still_ours) {
                // The idle queued behind the fidget took over before it ended,
                // and the fidget's record was dropped under it: the unit is on
                // its idle again.
                if (f.queued != nullptr && f.queued->getEnabled() &&
                    lowered(unit.playing) == lowered(f.playing)) {
                    remember_state(unit.entity, idle, f.queued);
                    unit.playing = idle;
                    unit.playing_state = f.queued;
                    unit.last_time = 0.0f;
                }
                f.state = nullptr;
                f.queued = nullptr;
                f.playing.clear();
                f.since = 0.0f;
                continue;
            }
            Ogre::AnimationState* resumed = nullptr;
            if (play_scene_animation(unit.entity, unit.mesh, idle, unit.loops, unit.rate,
                                     /*optional=*/true, &resumed, unit.armed, nullptr,
                                     /*blend=*/0.2f)) {
                remember_state(unit.entity, idle, resumed);
                unit.playing = idle;
                unit.playing_state = resumed;
                unit.last_time = 0.0f;
            }
            f.state = nullptr;
            f.queued = nullptr;
            f.playing.clear();
            f.since = 0.0f;
            continue;
        }

        // A character a level machine runs fidgets from its own AI
        // (`LevelScript::idle_ai`, `CMonster::idleAI`), which
        // sends the animation; only the end of one already playing is looked
        // after here.
        if (script_owning(unit.node) != nullptr) continue;
        // Only from the animation the layout left it on. A unit swinging, dying
        // or walking a scripted path is not idle, and `playing` is what says so.
        if (lowered(unit.playing) != lowered(idle)) continue;
        if (!(f.since > kFidgetCooldown) ||
            tl::core::volatile_random().between(0.0f, 1.0f) >= world_chance) {
            continue;
        }

        const std::string dir = dir_of_asset(unit.mesh);
        if (dir.empty()) continue;
        if (!begin_fidget(f, unit.entity, dir, /*is_subject=*/false, unit.mesh, unit.armed)) {
            continue;
        }
        unit.playing = f.playing;
        unit.playing_state = f.state;
        unit.last_time = 0.0f;
        // `queueBlendAnimation("IDLE", true, 0.2, 1.0)`: the idle
        // waits behind the fidget and blends back in 0.2 s before it ends.
        f.queued = nullptr;
        Ogre::AnimationState* queued = nullptr;
        if (play_scene_animation(unit.entity, unit.mesh, idle, unit.loops, unit.rate,
                                 /*optional=*/true, &queued, unit.armed, nullptr,
                                 /*blend=*/0.2f, /*queue=*/true)) {
            f.queued = queued;
        }
    }
}

// ---------------------------------------------------------------- the pet --
// The player's animal, this side of core/pet.h: model, animation, sounds, minimap icon.

bool Renderer::Impl::spawn_pet(const std::string& unit) {
    if (unit.empty() || scene == nullptr) return false;
    // How battered it is survives a re-spawn: a level change is not a new
    // animal. The stance, the bag, the spells and the effects are `pet`'s and
    // stay as they are.
    const bool same_animal = pet.alive();
    const float was_fraction = pet.health_fraction();
    // And a pet away in town: the original carries the pet object itself
    // across a warp, and `CLevel::removeCharacter` leaves its AI state and
    // clock alone only then (`restore_unit_town_trip`).
    const int32_t was_state = pet.ai_state();
    const float was_town_timer = pet.town_seconds_left();
    // ... and a fish's transformation with it: `+0x688` and `+0x690`, which a
    // saved pet takes from its state (`CCharacter::applySaveState`).
    //
    const float was_transform_left = pet.transform_left();
    const std::string was_transformed_from = pet.transformed_from();
    destroy_pet_entity();

    if (!index_unit(unit)) {
        if (!pet_reported) {
            TL_LOG_WARN("pet: no unit called '%s' in the archive", unit.c_str());
            pet_reported = true;
        }
        return false;
    }
    const tl::core::UnitStats stats = stats_for_unit(unit);

    // Beside the character, and on the ground under that point. `ground_under()`
    // skips units, so the animal cannot end up standing on its owner - the same
    // trap the creation screen's cat fell into.
    Ogre::Vector3 at = subject_node != nullptr ? subject_node->getPosition() : Ogre::Vector3::ZERO;
    const float facing =
        subject_node != nullptr ? subject_node->getOrientation().getYaw().valueRadians() : 0.0f;
    at.x += std::cos(facing) * kPetSpawnOffset;
    at.z -= std::sin(facing) * kPetSpawnOffset;
    float surface = 0.0f;
    if (!collision.empty() && collision.ground_under(at.x, at.z, at.y + 8.0f, surface)) {
        at.y = surface;
    }

    // The model at the pet's own level (`+0x100`: `unitInit`'s, what its
    // master's awards have raised it to since, or `applySaveState`'s).
    const int level = std::max(1, pet.level());
    Ogre::Entity* entity =
        spawn_unit(unit, at, kPetNodeId, level, /*champion=*/false, /*spawn_anim=*/false,
                   Ogre::Quaternion(Ogre::Radian(facing), Ogre::Vector3::UNIT_Y));
    if (entity == nullptr) {
        TL_LOG_WARN("pet: '%s' will not load", unit.c_str());
        return false;
    }
    const SpawnedUnit& made = spawned[kPetNodeId].back();
    pet_entity = entity;
    pet_node = made.node;
    pet_node_id = made.id;
    pet_mesh.clear();
    for (const auto& entry : scene_units) {
        if (entry.second.entity == entity) pet_mesh = entry.second.mesh;
    }

    // A character of the level's own machine, the player's pet: `addPet` ->
    // `setMaster`, its stance (`+0x710`), its
    // effects (`+0x1b8`).
    script.add_spawned_unit(pet_node_id, unit, tl::core::Vec3{at.x, at.y, at.z},
                            /*hunts=*/false, facing);
    script.set_unit_level(pet_node_id, level);
    script.set_unit_experience(pet_node_id, pet.experience());
    std::vector<tl::core::ScriptEvent> events;
    script.make_player_pet(pet_node_id, quest_log.state(), events);
    if (!events.empty()) apply_script_events(events, scripted_nodes);
    script.set_unit_stance(pet_node_id,
                           static_cast<int32_t>(tl::core::pet_stance_number(pet.stance())));
    script.set_unit_effects(pet_node_id, pet.shared_effects());
    // ... and its inventory (`+0x490`), the other half of its effect totals.
    script.set_unit_inventory(pet_node_id, &pet.bag());
    // The unit it runs out into has to be known by then: in a process that
    // never placed it (a saved pet's), nothing else reads its record.
    if (!was_transformed_from.empty()) index_unit(was_transformed_from);
    script.restore_unit_transformation(pet_node_id, was_transform_left, was_transformed_from);
    if (was_transform_left != 0.0f) {
        TL_LOG_INFO("pet: still what a fish made it, %.0f s left (first a %s)",
                    static_cast<double>(was_transform_left), was_transformed_from.c_str());
    }
    if (same_animal && script.restore_unit_town_trip(pet_node_id, was_state, was_town_timer,
                                                     quest_log.state(), events)) {
        if (!events.empty()) apply_script_events(events, scripted_nodes);
        TL_LOG_INFO("pet: still away in town; back in %.0f s", static_cast<double>(was_town_timer));
    }

    // DIVERGENCE (asked for): the character's `+0x664` is set for the player's pet whatever
    // its unit authors (TROLL_PET authors no CANMATCHSPEED), so every pet keeps pace; a
    // Troll fought in the mine is unaffected.
    if (!stats.can_match_speed) {
        TL_LOG_INFO("pet: '%s' authors no CANMATCHSPEED; giving it one anyway so it can follow",
                    unit.c_str());
        script.set_unit_can_match_speed(pet_node_id, true);
    }

    float hp = 0.0f;
    float max = 0.0f;
    script.unit_health(pet_node_id, hp, max);
    if (same_animal) script.set_unit_hp(pet_node_id, std::max(1.0f, max * was_fraction));

    pet.spawn(unit, stats, tl::core::Vec3{at.x, at.y, at.z}, facing);
    refresh_pet_effects();
    give_pet_spells_to_script();
    sync_pet_mirror();
    pet_was_in_town = false;
    pet_last_health = pet.health();

    if (automap_initialized) {
        automap.set_pet_visible(true);
        // And where it is, now rather than on the first frame of the run loop:
        // a static dump never reaches `advance()`, so an icon left at the world
        // origin would be off the map in every photographed frame.
        automap.set_pet_position(at);
    }

    TL_LOG_INFO("pet: %s at (%.2f, %.2f, %.2f), %.0f health, level %d, %s", unit.c_str(),
                static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z),
                static_cast<double>(pet.max_health()), level,
                pet.stance() == tl::core::PetStance::Aggressive  ? "aggressive"
                : pet.stance() == tl::core::PetStance::Defensive ? "defensive"
                                                                 : "passive");
    return true;
}

void Renderer::Impl::pet_converted(const std::string& unit) {
    if (pet_node_id == 0) return;
    const tl::core::UnitStats stats = stats_for_unit(unit);
    // DIVERGENCE: every pet keeps pace (see `spawn_pet`); `unitInit` has just
    // read the new unit's own `CANMATCHSPEED` into `+0x664`.
    if (!stats.can_match_speed) script.set_unit_can_match_speed(pet_node_id, true);
    pet.become(unit, stats);
    // What a new level places is the unit it is now.
    config.pet = unit;
    refresh_pet_effects();
    sync_pet_mirror();
    pet_last_health = pet.health();
    // In town (0x2a) `CCharacter::update` keeps it out of sight
    // whatever `transformCharacter`'s two `setVisible` calls did.
    if (pet_node != nullptr && pet.state() == tl::core::PetState::InTown) {
        pet_node->setVisible(false);
    }
    TL_LOG_INFO("pet: now a %s (%.0f health%s)", unit.c_str(),
                static_cast<double>(pet.max_health()),
                pet.transform_left() == -999.0f ? ", for good"
                : pet.transform_left() > 0.0f   ? ", for a while"
                                                : "");
    refresh_game_hud();
    update_menu();
}

// PORT: a spawned unit's OGRE entity, node and held items destroyed.
void Renderer::Impl::destroy_spawned_unit(Ogre::Entity* entity, int64_t id) {
    forget_unit_entity(entity);
    if (id != 0) {
        for (auto it = scripted_nodes.begin(); it != scripted_nodes.end();) {
            it = it->first == id ? scripted_nodes.erase(it) : std::next(it);
        }
        pending_idles.erase(std::remove_if(pending_idles.begin(), pending_idles.end(),
                                           [id](const PendingIdle& p) { return p.node == id; }),
                            pending_idles.end());
        spawning_units.erase(id);
        unit_spawn_level.erase(id);
    }
    if (scene != nullptr && entity != nullptr) {
        // The items in its hands (`equip_unit_items`) hang off its bones.
        std::vector<Ogre::MovableObject*> held(entity->getAttachedObjects().begin(),
                                               entity->getAttachedObjects().end());
        entity->detachAllObjectsFromBone();
        for (Ogre::MovableObject* object : held) scene->destroyMovableObject(object);
        if (Ogre::SceneNode* node = entity->getParentSceneNode()) {
            node->detachAllObjects();
            scene->destroySceneNode(node);
        }
        scene->destroyEntity(entity);
    }
}

// PORT: an OGRE entity dropped from every list of the port's scene bookkeeping.
void Renderer::Impl::forget_unit_entity(Ogre::Entity* entity) {
    if (entity != nullptr) {
        // Every list `place_mesh()` and `spawn_unit()` put it in.
        fidgets.erase(entity);
        entity_animations.erase(entity);
        unit_meshes.erase(entity);
        paint_ordered.erase(entity);
        scene_animations.erase(
            std::remove_if(scene_animations.begin(), scene_animations.end(),
                           [entity](const SceneAnimation& a) { return a.entity == entity; }),
            scene_animations.end());
        layout_meshes.erase(std::remove(layout_meshes.begin(), layout_meshes.end(), entity),
                            layout_meshes.end());
        for (auto it = unit_states.begin(); it != unit_states.end();) {
            it = it->first.first == entity ? unit_states.erase(it) : std::next(it);
        }
        for (auto it = scene_units.begin(); it != scene_units.end();) {
            if (it->second.entity != entity) {
                ++it;
                continue;
            }
            // Its triggers' particles, some on nodes of their own.
            clear_unit_themes(it->second.themes);
            it = scene_units.erase(it);
            scene_units_changed = true;
        }
        for (auto& entry : spawned) {
            std::vector<SpawnedUnit>& owned = entry.second;
            owned.erase(
                std::remove_if(owned.begin(), owned.end(),
                               [entity](const SpawnedUnit& u) { return u.entity == entity; }),
                owned.end());
        }
        unit_icons.erase(std::remove_if(unit_icons.begin(), unit_icons.end(),
                                        [entity](const UnitIcon& i) { return i.entity == entity; }),
                         unit_icons.end());
        // Its animation particles now, not on the next frame's sweep in
        // step_unit_particle_keys(): the next entity may be given this address.
        for (size_t i = 0; i < unit_particle_triggers.size();) {
            UnitParticleTrigger& held = unit_particle_triggers[i];
            if (held.entity != entity) {
                ++i;
                continue;
            }
            destroy_particle_holders(held.trigger.holders);
            if (held.trigger.follower != nullptr && scene != nullptr) {
                scene->destroySceneNode(held.trigger.follower);
            }
            unit_particle_triggers.erase(unit_particle_triggers.begin() + static_cast<long>(i));
        }
        clear_unit_weapon_elements(entity);
        forget_unit_trails(entity);
    }
}

// INVENTED: the pet put 1.6 units (`kPetSpawnOffset`) to the player's side
// and onto the ground; the console's own placement of a pet brought along is
// not transcribed.
void Renderer::Impl::place_pet_beside_player() {
    if (!pet.alive() || !pet.in_world() || subject_node == nullptr || pet_node_id == 0) return;

    Ogre::Vector3 at = subject_node->getPosition();
    const float facing = subject_node->getOrientation().getYaw().valueRadians();
    at.x += std::cos(facing) * kPetSpawnOffset;
    at.z -= std::sin(facing) * kPetSpawnOffset;
    float surface = 0.0f;
    if (!collision.empty() && collision.ground_under(at.x, at.z, at.y + 8.0f, surface)) {
        at.y = surface;
    }
    script.move_unit(pet_node_id, tl::core::Vec3{at.x, at.y, at.z});
    if (pet_node != nullptr) pet_node->setPosition(at);
    sync_pet_mirror();
    if (automap_initialized) automap.set_pet_position(at);
    TL_LOG_INFO("pet: brought along to (%.2f, %.2f, %.2f)", static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z));
}

namespace {
// PORT: an attached particle trigger's node moved onto its bone, or with no
// bone onto the unit's own node - its place, its turn and its scale.
void follow_unit_node(Ogre::SceneNode* follower, const Ogre::SceneNode* model,
                      const Ogre::Bone* bone) {
    if (bone != nullptr) {
        follower->setPosition(model->_getFullTransform() * bone->_getDerivedPosition());
        follower->setOrientation(model->_getDerivedOrientation() * bone->_getDerivedOrientation());
        return;
    }
    follower->setPosition(model->_getDerivedPosition());
    follower->setOrientation(model->_getDerivedOrientation());
    follower->setScale(model->_getDerivedScale());
}

// The entity's bone of that name, or null when it has no skeleton or no such bone.
const Ogre::Bone* find_bone(const Ogre::Entity* entity, const std::string& name) {
    if (!entity->hasSkeleton() || !entity->getSkeleton()->hasBone(name)) return nullptr;
    return entity->getSkeleton()->getBone(name);
}
}  // namespace

/// Fire the trigger tags (`<mesh>.ANIMATION`) one step of an animation crossed. A bare
/// tag means the unit's own STEP_SOUND / ATTACK_SOUND (carried on UnitStats).
void Renderer::Impl::fire_animation_sounds(const std::string& mesh, const std::string& animation,
                                           const tl::core::UnitStats& unit, float was, float now,
                                           const Ogre::Vector3& at) {
    if (animation.empty()) return;
    const std::vector<tl::core::AnimationTag>& tags = manifest_for(mesh).tags_for(animation);
    if (tags.empty()) return;

    for (const tl::core::AnimationTag& tag : tags) {
        std::string sound;
        // CCharacter::loadModel files a PLAYSOUND key's sound into the key bank (+0x2a0)
        // (CGenericModel::addValueIndex, CGenericModel::findKey,
        // CGenericModel::getValueIndexes); the port plays it by name.
        if (tag.name == "PLAYSOUND") {
            sound = tag.sound;
        } else if (tag.name == "ATTACKSOUND") {
            sound = tag.sound.empty() ? unit.attack_sound() : tag.sound;
        } else if (tag.name == "FOOTSTEP") {
            sound = tag.sound.empty() ? unit.step_sound() : tag.sound;
        } else {
            continue;
        }
        if (sound.empty()) continue;

        const float when = tag.seconds();
        // Wrapped round the loop, or stepped past the frame this frame. The
        // same test `step_monster_swings()` uses for the `HIT` tag, because it
        // is the same question asked of the same clock.
        const bool passed = now < was ? (was < when || now >= when) : (was < when && now >= when);
        if (passed) play_sound(sound, at);
    }
}

/// Every animated unit the level placed, once per frame (the pet's own from step_pet()).
// PORT: the animation sounds of each drawn unit, fired as its OGRE animation crosses their keys.
void Renderer::Impl::step_unit_sounds() {
    if (!audio_enabled || !player_control) return;
    for (auto& entry : scene_units) {
        SceneUnit& unit = entry.second;
        if (unit.entity == nullptr || unit.playing_state == nullptr) continue;
        if (!unit.playing_state->getEnabled()) continue;
        const Ogre::SceneNode* at = unit.entity->getParentSceneNode();
        if (at == nullptr || !unit.entity->isVisible()) continue;

        const float now = unit.playing_state->getTimePosition();
        // A new animation starts before its first key: the Ember Colossus's
        // `die` read as a wrap of its attack and played all four of its
        // sounds on the first frame.
        const float was = unit.sound_animation == unit.playing ? unit.last_sound_time : -1.0f;
        unit.sound_animation = unit.playing;
        unit.last_sound_time = now;
        if (now == was) continue;
        fire_animation_sounds(unit.mesh, unit.playing, stats_for_unit(entry.first), was, now,
                              at->_getDerivedPosition());
    }
}

void Renderer::Impl::step_unit_particle_keys() {
    // A trigger whose unit has left the scene goes with it.
    for (size_t i = 0; i < unit_particle_triggers.size();) {
        UnitParticleTrigger& held = unit_particle_triggers[i];
        const bool live =
            std::any_of(scene_units.begin(), scene_units.end(),
                        [&held](const auto& e) { return e.second.entity == held.entity; });
        if (live) {
            ++i;
            continue;
        }
        destroy_particle_holders(held.trigger.holders);
        if (held.trigger.follower != nullptr) scene->destroySceneNode(held.trigger.follower);
        unit_particle_triggers.erase(unit_particle_triggers.begin() + static_cast<long>(i));
    }
    clear_unit_weapon_elements(nullptr);

    const auto stop = [this](UnitParticleTrigger& held) {
        // `CParticle::Stop`: no more is emitted, and what is out dies away.
        if (held.trigger.holders.empty()) return;
        TransientParticles leaving;
        leaving.holders = std::move(held.trigger.holders);
        transient_particles.push_back(std::move(leaving));
        held.trigger.holders.clear();
        held.trigger.inside.clear();
    };
    const auto clean = [](std::string text) {
        for (char& c : text) {
            if (c == '\\') c = '/';
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return text;
    };

    // The mesh keys crossed this frame, applied once the walk is over.
    struct MeshKey {
        int64_t node;
        Ogre::Entity* entity;
        bool hides;
        bool at_once;
    };
    std::vector<MeshKey> mesh_keys;
    for (auto& entry : scene_units) {
        SceneUnit& unit = entry.second;
        if (unit.entity == nullptr || unit.playing_state == nullptr) continue;
        if (!unit.playing_state->getEnabled()) continue;
        Ogre::SceneNode* model = unit.entity->getParentSceneNode();
        if (model == nullptr) continue;

        const float now = unit.playing_state->getTimePosition();
        // A new animation starts before its first key.
        const float was = unit.key_animation == unit.playing ? unit.key_time : -1.0f;
        unit.key_animation = unit.playing;
        unit.key_time = now;
        if (now == was) continue;
        const std::vector<tl::core::AnimationTag>& tags =
            manifest_for(unit.mesh).tags_for(unit.playing);
        for (size_t index = 0; index < tags.size(); ++index) {
            const tl::core::AnimationTag& tag = tags[index];
            // CCharacter::updateAnimation, keys
            // 0x16..0x19: HIDEMESH/FADEOUTMESH set +0x70d and hide, SHOWMESH/FADEINMESH show;
            // the FADE pair through updateOpacity.
            const bool hides = tag.name == "HIDEMESH" || tag.name == "FADEOUTMESH";
            const bool shows = tag.name == "SHOWMESH" || tag.name == "FADEINMESH";
            if (hides || shows) {
                const float at = tag.seconds();
                if (now < was ? (was < at || now >= at) : (was < at && now >= at)) {
                    mesh_keys.push_back({unit.node, unit.entity, hides,
                                         tag.name == "HIDEMESH" || tag.name == "SHOWMESH"});
                }
                continue;
            }
            // Keys 7 and 8, `SHOWWEAPONTRAIL` and `HIDEWEAPONTRAIL`.
            if (tag.name == "SHOWWEAPONTRAIL" || tag.name == "HIDEWEAPONTRAIL") {
                const float at = tag.seconds();
                if (now < was ? (was < at || now >= at) : (was < at && now >= at)) {
                    DrawnTrails& drawn = unit_trails[unit.entity];
                    const bool good = unit_side(unit.node, stats_for_unit(entry.first)) ==
                                      tl::core::UnitStats::Alignment::Friendly;
                    const tl::core::TrailCharacter who =
                        unit_trail_character(unit.entity, drawn, good, unit.node);
                    // INVENTED: `selectAttack`
                    // is asked here, at the
                    // key, for the original's attack and range questions the
                    // port does not have (see `step_weapon_trails`).
                    drawn.trails.select_attack(who, true);
                    weapon_trail_key(drawn, unit.entity, tag, who);
                }
                continue;
            }
            // Keys 10 and 11 on an item: CItem::updateAnimation(float):
            // ENABLECOLLISION sets +0x18d, +0x19c and addToAvoidanceMap,
            // DISABLECOLLISION clears +0x18d and removeFromAvoidanceMap.
            if (tag.name == "ENABLECOLLISION" || tag.name == "DISABLECOLLISION") {
                const float at = tag.seconds();
                if (now < was ? (was < at || now >= at) : (was < at && now >= at)) {
                    // The machine whose node map holds this copy's scene node
                    // (a prop is no character, so `script_owning` misses it).
                    NodeMap* nodes = nullptr;
                    for (const MachineView& view : machines()) {
                        if (view.nodes == nullptr) continue;
                        const auto held = view.nodes->equal_range(unit.node);
                        for (auto n = held.first; n != held.second && nodes == nullptr; ++n) {
                            if (n->second == model) nodes = view.nodes;
                        }
                        if (nodes != nullptr) break;
                    }
                    TriggerProp prop;
                    if (nodes != nullptr && trigger_prop(entry.first, prop)) {
                        set_trigger_collidable(instance_key(unit.node, *nodes),
                                               tag.name == "ENABLECOLLISION");
                        TL_LOG_INFO(
                            "unit trigger '%s': %s at its key", entry.first.c_str(),
                            tag.name == "ENABLECOLLISION" ? "collidable" : "not collidable");
                    }
                }
                continue;
            }
            // Keys 0x12 and 0x13, `DAMPVELOCITY` and `UNDAMPVELOCITY`: the
            // character's velocity scale (`+0x258`) to 0 - its velocity with
            // it - or back to 1 (`LevelScript::damp_unit_velocity`).
            if (tag.name == "DAMPVELOCITY" || tag.name == "UNDAMPVELOCITY") {
                const float at = tag.seconds();
                if (now < was ? (was < at || now >= at) : (was < at && now >= at)) {
                    if (tl::core::LevelScript* owner = script_owning(unit.node)) {
                        owner->damp_unit_velocity(unit.node, tag.name == "DAMPVELOCITY");
                    }
                }
                continue;
            }
            const bool spawns =
                tag.name == "SPAWNPARTICLE" || tag.name == "SPAWNPARTICLE_STOP_ON_DEATH";
            const bool removes = tag.name == "REMOVEPARTICLES";
            const bool removes_own = tag.name == "REMOVEANIMATIONPARTICLES";
            if (!spawns && !removes && !removes_own) continue;
            const float when = tag.seconds();
            const bool passed =
                now < was ? (was < when || now >= when) : (was < when && now >= when);
            if (!passed) continue;

            if (removes || removes_own) {
                const std::string layout = clean(tag.layout);
                for (UnitParticleTrigger& held : unit_particle_triggers) {
                    if (held.entity != unit.entity) continue;
                    if (removes_own ? held.trigger.animation != unit.playing
                                    : !layout.empty() && held.layout != layout) {
                        continue;
                    }
                    stop(held);
                }
                continue;
            }
            // INVENTED: a key with a `UNITTHEME` runs only while the unit has
            // the theme (`unitThemeUpdated`); a
            // placed unit's themes are not asked here, so such a key is left
            // alone.
            if (tag.layout.empty() || !tag.unit_theme.empty()) continue;

            const Ogre::Bone* bone = nullptr;
            if (unit.entity->hasSkeleton() && !tag.bone.empty() &&
                unit.entity->getSkeleton()->hasBone(tag.bone)) {
                bone = unit.entity->getSkeleton()->getBone(tag.bone);
            }
            UnitParticleTrigger* held = nullptr;
            for (UnitParticleTrigger& candidate : unit_particle_triggers) {
                if (candidate.entity == unit.entity &&
                    candidate.trigger.animation == unit.playing && candidate.trigger.tag == index) {
                    held = &candidate;
                }
            }
            if (held == nullptr) {
                UnitParticleTrigger made;
                made.entity = unit.entity;
                made.layout = clean(tag.layout);
                made.trigger.animation = unit.playing;
                made.trigger.tag = index;
                made.trigger.bone = tag.bone;
                made.trigger.offset = Ogre::Vector3(tag.offset_x, tag.offset_y, tag.offset_z);
                made.trigger.attaches = tag.attaches;
                made.trigger.stop_on_death = tag.name == "SPAWNPARTICLE_STOP_ON_DEATH";
                // configureParticleTrigger: an attached
                // trigger's particle follows the bone, or with none the unit's node, turning
                // with the unit. The scene's own node, moved each frame.
                if (made.trigger.attaches) {
                    made.trigger.follower = scene->getRootSceneNode()->createChildSceneNode();
                }
                unit_particle_triggers.push_back(std::move(made));
                held = &unit_particle_triggers.back();
            }
            ParticleTrigger& trigger = held->trigger;
            // Stopped, or never placed: the layout afresh (`CParticle::Start`).
            destroy_particle_holders(trigger.holders);
            trigger.inside.clear();
            std::string reference = tag.layout;
            for (char& c : reference) {
                if (c == '\\') c = '/';
            }
            Ogre::Vector3 at = trigger.offset;
            if (trigger.follower != nullptr) {
                follow_unit_node(trigger.follower, model, bone);
            } else {
                at = model->_getFullTransform() *
                     ((bone != nullptr ? bone->_getDerivedPosition() : Ogre::Vector3::ZERO) +
                      trigger.offset);
            }
            trigger.holders =
                place_particle_layout(reference, at, Ogre::Quaternion::IDENTITY, trigger.follower);
            TL_LOG_INFO("animation particle: %s's '%s' key %zu releases %s%s (%zu system node(s))",
                        unit.mesh.c_str(), unit.playing.c_str(), index, reference.c_str(),
                        trigger.follower != nullptr ? ", attached" : "", trigger.holders.size());
        }
    }

    // `updateBonePositions`: each attached
    // trigger's node at the model's position plus the bone's, with the bone's
    // turn.
    for (UnitParticleTrigger& held : unit_particle_triggers) {
        ParticleTrigger& trigger = held.trigger;
        if (trigger.follower == nullptr) continue;
        const Ogre::SceneNode* model = held.entity->getParentSceneNode();
        if (model == nullptr) continue;
        const Ogre::Bone* bone = find_bone(held.entity, trigger.bone);
        follow_unit_node(trigger.follower, model, bone);
    }
    // ... and each weapon element's hand node, inside the unit's node.
    for (UnitWeaponElement& held : unit_weapon_elements) {
        const Ogre::Bone* bone = held.entity->getSkeleton()->getBone(held.bone);
        held.hand->setPosition(bone->_getDerivedPosition());
        held.hand->setOrientation(bone->_getDerivedOrientation());
    }

    for (const MeshKey& key : mesh_keys) {
        const int64_t node = key.node;
        const bool hide = key.hides;
        tl::core::LevelScript* owner = script_owning(node);
        if (owner == nullptr) continue;
        std::vector<tl::core::ScriptEvent> events;
        if (!owner->set_unit_hidden(node, hide, key.at_once, events)) continue;
        // `CCharacter::setVisible(false)`
        // puts every weapon trail off and out of sight.
        if (hide) {
            if (const auto trails = unit_trails.find(key.entity); trails != unit_trails.end()) {
                trails->second.trails.hide_all();
            }
        }
        TL_LOG_INFO("animation key: %s %s", owner->unit_of(node).c_str(),
                    hide ? "hidden" : "shown");
        if (NodeMap* nodes = nodes_of(owner); nodes != nullptr && !events.empty()) {
            apply_script_events(events, *nodes);
        }
    }
}

void Renderer::Impl::step_pet() {
    if (!pet.alive() || !player_control) return;
    if (pet_node_id == 0) return;
    script.set_level_is_town(in_town());
    sync_pet_mirror();

    // Back from town: the master's voice
    // (0x2c, VOICEPETRETURNED_SOUND), what it sold and the dialog; the sum by
    // item_sell_price().
    for (const int64_t id : script.take_units_back_from_town()) {
        if (id != pet_node_id) continue;
        play_voice(tl::core::SoundSlot::VoicePetReturned, 0.1f);
        load_unit_types();
        int sold = 0;
        const int gold = pet.sell_bag(
            [&](const tl::core::ItemStats& item, int count) {
                return tl::core::item_sell_price(item, stat_graphs, count);
            },
            // `CBaseUnit::getIsQuestUnit`.
            [&](const tl::core::ItemStats& item) {
                return unit_types.isa(item.unittype, tl::core::unit_type::kQuestItem) ||
                       item.quest_guid != -1;
            },
            sold);
        if (gold > 0) {
            player_inventory.add_gold(gold);
            // CCharacter::giveGold: incrementJournalStatistic(1, amount)
            count_journal(tl::core::JournalStatistic::GoldGathered, gold);
        }
        // The dialog, titled `Pet`: on a town's first
        // level (`CLevel +0x1a4` 0 and the template's `IS_TOWN`) the pet
        // "meets you in town", anywhere else it "returns from town"; with
        // anything sold the line carries the sum.
        const bool met_in_town = level_depth == 0 && in_town();
        std::string text;
        if (sold > 0) {
            text = std::string(met_in_town ? "Your pet meets you in town, and brings you"
                                           : "Your pet returns from town, and brings you") +
                   "\n" + std::to_string(gold) + " " + "Gold from the sale of items.";
        } else {
            text = met_in_town ? "Your pet meets you in town." : "Your pet returns from town.";
        }
        open_modal("Pet", text);
        TL_LOG_INFO(
            "pet: back from town at (%.1f, %.1f) with %d gold from %d item(s); dialog "
            "'%s'",
            static_cast<double>(pet.position().x), static_cast<double>(pet.position().z), gold,
            sold, text.c_str());
        refresh_game_hud();
        update_menu();
    }

    // Hidden while in town (`CCharacter::update`, `setVisible(false)`).
    const bool away = pet.state() == tl::core::PetState::InTown;
    if (away != pet_was_in_town) {
        pet_was_in_town = away;
        if (pet_node != nullptr) pet_node->setVisible(!away);
        if (automap_initialized) automap.set_pet_visible(!away);
        if (away) TL_LOG_INFO("pet: gone to town; back in %s", pet.town_clock().c_str());
    }

    // `CCharacter::modifyHP` plays the master's voice when the
    // pet's health comes down to one - what puts it to flight.
    if (pet_last_health > 1.0f && pet.health() <= 1.0f) {
        play_voice(tl::core::SoundSlot::VoicePetFled);
        TL_LOG_INFO("pet: at its last hit point (%.0f of %.0f); it runs",
                    static_cast<double>(pet.health()), static_cast<double>(pet.max_health()));
    }
    pet_last_health = pet.health();

    if (automap_initialized && !away) {
        const tl::core::Vec3 at = pet.position();
        automap.set_pet_position(Ogre::Vector3(at.x, at.y, at.z));
    }
}

// CLevel::updateAutomapIcons: enabled characters not
// pending deletion, the icon of their first type that has one, facing (1, 0, -1), kept on
// the unit (+0x728).
void Renderer::Impl::update_automap_icons() {
    if (!automap_initialized) return;
    automap.clear_npc_icons();
    unit_icons.clear();
    load_unit_types();

    std::vector<MachineView> views = machines();
    const auto enabled = [&](int64_t node) {
        for (const MachineView& view : views) {
            if (!view.script->node_enabled(node) || view.script->unit_is_dead(node)) return false;
        }
        return true;
    };

    const Ogre::Vector3 dir(1.0f, 0.0f, -1.0f);
    for (const auto& entry : scene_units) {
        const SceneUnit& su = entry.second;
        if (!su.entity || !su.entity->getParentSceneNode()) continue;
        if (!enabled(su.node)) continue;

        const uint32_t type = unit_types.id_of(stats_for_unit(entry.first).unittype);
        namespace ut = tl::core::unit_type;
        int icon_tile = -1;
        if (unit_types.isa(type, ut::kGambler)) {
            icon_tile = 0x1f;
        } else if (unit_types.isa(type, ut::kEnchanter)) {
            icon_tile = 0x1d;
        } else if (unit_types.isa(type, ut::kCombiner)) {
            icon_tile = 0x20;
        } else if (unit_types.isa(type, ut::kSocketableDestroyer)) {
            icon_tile = 0x21;
        } else if (unit_types.isa(type, ut::kSocketableRecoverer)) {
            icon_tile = 0x1e;
        } else if (unit_types.isa(type, ut::kMerchant)) {
            icon_tile = 0x19;
        } else if (unit_types.isa(type, ut::kInteractiveCharacter)) {
            icon_tile = npc_quest_icon(entry.first, type);
        }
        if (icon_tile == -1) continue;

        Ogre::SceneNode* node = su.entity->getParentSceneNode();
        UnitIcon icon;
        icon.node = node;
        icon.entity = su.entity;
        icon.billboard = automap.add_tile(icon_tile, node->_getDerivedPosition(), dir, true, true);
        icon.shown = true;
        icon.unit = entry.first;
        icon.placed = node->_getDerivedPosition();
        unit_icons.push_back(icon);
    }
    if (unit_icons.size() != last_unit_icon_count) {
        last_unit_icon_count = unit_icons.size();
        TL_LOG_INFO("automap: %zu unit icon(s)", unit_icons.size());
    }
    // The second half: enabled CTriggerUnit items not marked for deletion that
    // are a TownPortal (0x2b) or WaypointNode (0xab), tile 0x22, not kept on the item (it
    // stays until clearNPCIcons). CPlayer::createPortals' portals are deleted at once here.
    size_t portal_icons = 0;
    for (const OpenPortal& portal : open_portals) {
        const uint32_t type = unit_types.id_of(portal.unittype);
        if (!unit_types.isa(type, tl::core::unit_type::kTownPortal) &&
            !unit_types.isa(type, tl::core::unit_type::kWaypointNode)) {
            continue;
        }
        automap.add_tile(0x22, to_ogre(portal.at), dir, true, true);
        ++portal_icons;
    }
    if (portal_icons != 0) TL_LOG_INFO("automap: %zu portal icon(s)", portal_icons);
    step_automap_icons();
}

void Renderer::Impl::step_automap_icons() {
    if (!automap_initialized) return;
    for (UnitIcon& icon : unit_icons) {
        if (icon.billboard == nullptr || icon.node == nullptr) continue;
        // `CCharacter::updateMotion`: the
        // icon follows the unit's position every frame.
        const Ogre::Vector3 now = icon.node->_getDerivedPosition();
        icon.billboard->setPosition(now);
        if (!icon.reported_moving && icon.placed.distance(now) > 1.0f) {
            icon.reported_moving = true;
            TL_LOG_INFO("automap: %s's icon follows it to (%.2f, %.2f)", icon.unit.c_str(),
                        static_cast<double>(now.x), static_cast<double>(now.z));
        }
        // CCharacter::setMeshVisible ->
        // CLevel::setNPCAutomapBillboardVisible.
        // DIVERGENCE: the change is noticed here, as units are hidden in many places.
        const bool shown = icon.entity != nullptr && icon.entity->isVisible();
        if (shown != icon.shown) {
            icon.shown = shown;
            automap.set_npc_icon_visible(icon.billboard, shown);
        }
    }
}

Ogre::Entity* Renderer::Impl::conversation_speaker() const {
    if (!quest_dialog.visible()) return nullptr;
    // `CGameUI::getQuestDialogNPC`: who a script's window was opened for, or
    // the unit A was pressed on.
    const std::string& unit = script_dialog_window ? offered_by : talk_unit;
    if (unit.empty()) return nullptr;
    std::string found;
    return find_unit_entity(unit, found);
}

// CGameClient::updateIngame, the paused
// branch (0x58ee.. on): the player's target when it is an interactive
// character, and the quest dialog's NPC, have their `update` and
// `updateAnimation`; so has the player beside either.
void Renderer::Impl::step_conversation_poses(float delta, float player_rate) {
    const auto step = [this, delta](Ogre::Entity* entity, float rate) {
        if (entity == nullptr) return;
        const bool modelled = step_entity_animation(entity, delta);
        Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
        if (states == nullptr) return;
        for (const auto& entry : states->getAnimationStates()) {
            if (!entry.second->getEnabled()) continue;
            if (modelled && animator_owns(entity, entry.second)) continue;
            entry.second->addTime(delta * rate);
        }
    };
    // The NPC's `CCharacter::update` and `updateAnimation`:
    // its gait is chosen again, so one whose path ended into its own line
    // stands rather than runs in place under the box.
    if (quest_dialog.visible()) {
        const std::string& unit = script_dialog_window ? offered_by : talk_unit;
        for (const MachineView& view : machines()) {
            std::vector<tl::core::ScriptEvent> events;
            const bool had =
                view.script->update_paused_speaker(unit, delta, quest_log.state(), events);
            if (!events.empty()) apply_script_events(events, *view.nodes);
            if (had) break;
        }
    }
    if (Ogre::Entity* speaker = conversation_speaker()) {
        float rate = 1.0f;
        for (const SceneAnimation& animation : scene_animations) {
            if (animation.entity == speaker) rate = animation.rate;
        }
        step(speaker, rate);
    }
    for (Ogre::Entity* entity : animated()) step(entity, player_rate);
}

// CRoomPiece::updateAnimation(float), the test before
// CGenericModel::updateAnimation: the piece's position plus the mesh bounds, unturned and
// unscaled, against the game's camera (+0x4c8 of the global).
bool Renderer::Impl::room_piece_in_view(const Ogre::Entity* entity) const {
    if (camera == nullptr || entity == nullptr || entity->getParentSceneNode() == nullptr) {
        return true;
    }
    const Ogre::AxisAlignedBox& own = entity->getMesh()->getBounds();
    if (own.isNull() || own.isInfinite()) return true;
    const Ogre::Vector3 at = entity->getParentSceneNode()->_getDerivedPosition();
    const Ogre::AxisAlignedBox box(at + own.getMinimum(), at + own.getMaximum());
    return camera->isVisible(box);
}

// `CLevel::updateCharacterAnimation(dt, speed, player)`:
// of the near characters the player's
// `updateAnimation` takes the level's time as it is, every other one's
// that time times the game speed, and the near items' the same.
void Renderer::Impl::step_near_poses(float level_delta, float rate) {
    // The player's own state, whose `FOOTSTEP` crossings are counted below.
    const std::string player_state = player_control && subject != nullptr && subject->getMesh()
                                         ? animation_named(subject, player_animation)
                                         : std::string();
    for (Ogre::Entity* entity : animated()) {
        Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
        if (states == nullptr) continue;
        for (const auto& entry : states->getAnimationStates()) {
            if (!entry.second->getEnabled()) continue;
            const float was = entry.second->getTimePosition();
            entry.second->addTime(level_delta * rate);
            const float now = entry.second->getTimePosition();
            if (entity == subject && !player_state.empty() && entry.first == player_state &&
                was != now) {
                count_player_footsteps(was, now);
            }
        }
    }
}

// CCharacter::updateAnimation: every `FOOTSTEP` event the animation fires counts
// statistic 3, before and regardless of the step sound (bank `+0x298`). The crossing
// test is fire_animation_sounds()'s.
void Renderer::Impl::count_player_footsteps(float was, float now) {
    for (const tl::core::AnimationTag& tag :
         manifest_for(subject->getMesh()->getName()).tags_for(player_animation)) {
        if (tag.name != "FOOTSTEP") continue;
        const float when = tag.seconds();
        const bool passed = now < was ? (was < when || now >= when) : (was < when && now >= when);
        if (passed) count_journal(tl::core::JournalStatistic::StepsTaken);
    }
}

void Renderer::Impl::advance(float delta) {
    const tl::core::SectionScope whole(tl::core::Section::Update);
    step_headless_sections();
    step_cinematic_music(delta);
    step_music_objects(delta);
    // `CGameClient::updateIngame`: the particle falloff is measured
    // from the player, written every frame.
    if (subject_node) {
        particle_universe.set_falloff_position(subject_node->_getDerivedPosition());
    }
    // `CGameClient::updateIngame`: particle systems stand
    // still while the game is paused, unless the player's target is an
    // interactive character (`ISA 0x83`, the port's talk target).
    particle_universe.set_game_paused(game_paused() && !talking_to_unit());
    // The level's script stands still with the rest of the level
    // (`LevelScript::level_paused`): its units, its timers and its timelines.
    {
        const bool modal = game_paused();
        for (const MachineView& view : machines()) view.script->set_modal_open(modal);
        if (modal != level_was_paused) {
            level_was_paused = modal;
            if (modal) {
                TL_LOG_INFO("pause: the level stands still under %s", modal_dialog_name());
            } else {
                TL_LOG_INFO("pause: the level runs again");
            }
        }
    }
    // The same: the game speed follows the frame's own
    // time, paused or not, and the particle systems run at it times the
    // client's speed multiplier (`+0x38c0`, 1 unless the developer keys in
    // `processIngameInput` change it; the port has none).
    load_game_speed();
    game_speed.calculate(delta);
    const float speed = game_speed.speed() * 1.0f;
    particle_universe.set_game_speed(speed);
    if (game_speed.size() != 0) {
        game_speed_lowest = std::min(game_speed_lowest, speed);
    } else if (game_speed_lowest < 1.0f) {
        TL_LOG_INFO("game speed: back to 1 (lowest %.3f)", static_cast<double>(game_speed_lowest));
        game_speed_lowest = 1.0f;
    }
    // updateIngame: level time is the frame's, capped at 0.3 s times
    // the speed; CLevel::update runs the player on it and
    // the rest on it
    // times the speed. DIVERGENCE: the original splits it into steps of <= 0.016 s.
    const float level_delta = speed * 0.3f < delta ? speed * 0.3f : delta;
    const float world_delta = level_delta * speed;
    step_light_map(delta);
    // A quest finished by a script, or by another quest, is paid and cleans up
    // too (`completeQuest` always rewards); complete_quest() does its own.
    pay_quest_completions();
    deliver_quest_notices();

    if (automap_initialized && subject_node) {
        automap.set_player_position(subject_node->getPosition(), subject_node->getOrientation());
        step_automap_icons();
        automap.update(subject_node->getPosition(), delta);
    }
    // CGameUI::updateIngameUI, per banner: the timer runs down first; only once it
    // has expired does the alpha fall by 2.0 a second, clamped at zero.
    for (Banner* banner : {&big_banner, &small_banner}) {
        if (banner->left > 0.0f) {
            banner->left = std::max(0.0f, banner->left - delta);
        } else if (banner->alpha > 0.0f) {
            banner->alpha = std::max(0.0f, banner->alpha - kBannerFadePerSecond * delta);
        }
    }
    // The context tips: `CPlayer::update` and the screens queue
    // theirs, then the queue takes its turn (`CGameUI::updateIngameUI`).
    // Here and not with the pad, so a run that polls no pad has
    // them too.
    queue_player_update_tips();
    queue_menu_tips();
    queue_pet_bag_tip();
    step_tips();
    // The Xbox `+` over the bag, from the player's unspent
    // stat and skill points.
    available_points.update(delta, player_character.unspent_stat_points(),
                            player_character.unspent_skill_points());
    refresh_game_hud();

    // The held attack button when no pad was read (headless), before the animation is
    // chosen; let go while the character follows a route.
    if (auto_attack && player_control && !attack_button_stepped) {
        const bool down = !player.following();
        step_attack_button(delta, down, down && !attack_held, 0.0f, 0.0f);
        attack_held = down;
    }
    attack_button_stepped = false;

    // Before anything is stepped: a swing that started this frame has to be on
    // the skeleton before the skeleton is advanced, or its first frame is the
    // previous animation's.
    step_player_death(level_delta);
    follow_player_animation();

    // Every enabled state, walking the full map (addTime() must not mutate the enabled
    // list). A gait plays at speed / RUNNINGSPEED so feet do not skate; a swing at
    // ATTACKSPEED.
    float rate = 1.0f;
    if (player_control) {
        rate = fishing_state != tl::core::FishingState::Off ? fishing_animation_rate
               : skill_playback.active                      ? skill_playback.speed
               : swing.active                               ? swing_rate()
                                                            : player.animation_rate();
    }

    // CGameClient::getIsPaused: under a modal menu
    // no level updates. DIVERGENCE
    // (a play report): the original still animates the player and first pet
    // (CGenericModel::updateAnimation); here every pose holds but a conversation's.
    const bool paused = game_paused();
    // `updateIngame` walks the level's particle links on a paused frame too
    // (`CLevel::updateVisibleParticles` with `getIsPaused()`), where every one
    // is shown; an unpaused frame's walk is below, after the poses.
    if (paused) update_visible_particles(true);
    // `CCharacter::update` adds each frame to
    // +0x388, the player's play time, while the level updates.
    if (!paused && player_control && config.travel != nullptr)
        config.travel->play_time += level_delta;
    if (!paused) {
        std::optional<tl::core::SectionScope> section(std::in_place, tl::core::Section::Animation);
        step_near_poses(level_delta, rate);
        // The fish on the hook plays its `IDLE` while it shows
        // (`CPlayer::updateAnimation`).
        if (hooked_fish_idle != nullptr && hooked_fish != nullptr && hooked_fish->isVisible()) {
            hooked_fish_idle->addTime(level_delta);
        }
        // The rod, the line and the fish, on the poses just stepped.
        update_fishing_gear();
        refresh_fishing_screen();

        // Everything the level animates, NPCs and props, each at its node's SPEED. One
        // started by play_scene_animation() goes through its animator
        // (CGenericModel::updateAnimation). Layout props by their descriptors:
        // CGenericModelDescriptor::update and
        // CRoomPieceDescriptor::update, which calls
        // CRoomPiece::updateAnimation after a camera test.
        for (const SceneAnimation& animation : scene_animations) {
            if (animation.room_piece && !room_piece_in_view(animation.entity)) continue;
            const bool modelled = step_entity_animation(animation.entity, world_delta);
            if (Ogre::AnimationStateSet* states = animation.entity->getAllAnimationStates()) {
                for (const auto& entry : states->getAnimationStates()) {
                    if (!entry.second->getEnabled()) continue;
                    if (modelled && animator_owns(animation.entity, entry.second)) continue;
                    entry.second->addTime(world_delta * animation.rate);
                }
            }
        }

        // `CCharacter::updateAnimation`
        // updates the weapon trails after the pose and before the keys the pose
        // crossed.
        step_weapon_trails(level_delta, world_delta);

        // After the poses have moved, so a fidget that has run out is noticed on
        // the frame it ends rather than the one after, and before the script runs,
        // which is what may take a unit off its idle.
        step_fidgets(level_delta, world_delta);

        // ... and, on the same clock, whatever those poses just walked past: a
        // footfall, a swing, the bark on a dog's fidget.
        section.emplace(tl::core::Section::UnitKeys);
        step_unit_sounds();
        step_unit_particle_keys();
        step_breakable_shapes();
        step_node_channels(level_delta);
        step_portal_sounds();

        section.emplace(tl::core::Section::Particles);
        update_particle_triggers();
        update_visible_particles(false);
        step_particles(world_delta);

        // Whatever a spawner still owes: a chute lets its units out one at a time
        // over the DURATION it authors, not all in the frame the trigger fired.
        section.emplace(tl::core::Section::Spawners);
        step_spawners(world_delta);
        section.emplace(tl::core::Section::Missiles);
        step_missiles(world_delta);

        // The fight, after the poses have moved and before the script runs: a blow
        // lands on a frame of an animation, so the animation has to have reached it.
        section.emplace(tl::core::Section::Fight);
        step_swing(level_delta);
        step_skill_animation(level_delta);
        update_worn_skill_managers(level_delta);
        step_monster_swings(world_delta);

        // The pet after the monsters' swings, so their bites resolve in a fixed order.
        step_pet();
    } else if (conversation_open()) {
        step_conversation_poses(level_delta, rate);
    }

    // The player's effects age before the script reads totals; not while paused
    // (CLevel::update runs them). updateCameraOpacity is in play only,
    // never in updateMenu.
    if (player_control) step_player_camera_opacity(paused ? 0.0f : level_delta);
    if (!paused) {
        const tl::core::SectionScope timed(tl::core::Section::EffectsSkills);
        update_player_effects(level_delta);
        // And everyone else's (`CCharacter::update` runs the same for a monster).
        update_unit_effects(level_delta);
        // Their casts: the animation's keys, `updateActiveSkill`, the manager's
        // `update` (`CCharacter::update`, as for the
        // player).
        step_unit_skills(level_delta);
        // Every key and cast of the frame is in: the weapon trails' strips
        // onto their objects.
        draw_weapon_trails();
    }

    // Anything the character or the narrator was queued to say. After the pet,
    // because the pet is what queues most of it.
    step_voices(delta);

    // The level's own script: its triggers, its timers and its timelines. After
    // the animations and the particles, because what it does is move things and
    // take the camera, and both should land on the state this frame ends in.
    {
        const tl::core::SectionScope timed(tl::core::Section::Script);
        const unsigned long long s0 = root->getTimer()->getMicroseconds();
        step_script(level_delta);
        prof_script_us += root->getTimer()->getMicroseconds() - s0;
    }

    // Outside step_script(), which returns early on a level that wires nothing: floaty
    // lines and A-started conversations are not wires.
    step_floaties(delta);
    // `CInteractiveMenu::update`, on the interface's time.
    hud.update_bars(delta);
    step_pending_talk();
    if (!paused) step_item_ai(world_delta);
    step_hud_visibility();
    // `CLevel::update`: `updateItems` after
    // the characters, on the level's time; not while the level is paused.
    if (!paused) step_level_items(world_delta);
    step_merchants(world_delta);
    step_unit_themes();
    // `CGameClient::updateIngame` calls
    // `autoPickupGold` each frame.
    auto_pickup_gold();
    step_ground_labels();
    // `updateIngame`: the `LEVELPARTICLE` layout
    // (`+0x1080`) is set to the player's position.
    if (level_particle_node != nullptr && subject_node != nullptr) {
        level_particle_node->setPosition(subject_node->_getDerivedPosition());
    }

    // The turntable is a backdrop's flourish and is wrong for a character the
    // player is steering: it would spin the model against the way it is walking.
    if (!player_control && subject_node && config.turntable_degrees_per_second != 0.0f) {
        subject_node->yaw(
            Ogre::Radian(Ogre::Degree(config.turntable_degrees_per_second * delta).valueRadians()));
    }
}

void Renderer::run() {
    Impl& m = *impl_;
    if (m.config.headless) return;
    m.loading.hide();

    // The HUD only when someone holds the controller (not --mesh or the backdrops).
    if (m.player_control) {
        m.refresh_game_hud();
        m.game_hud.show();
        if (m.automap_initialized && m.config.minimap) {
            m.automap.show();
        }
    }

    // Root's own timer rather than a platform clock: it exists on both targets
    // and needs no new platform glue to work on the console.
    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last = timer->getMilliseconds();

    unsigned long window_started = last;
    unsigned frames = 0;
    unsigned windows = 0;
    const unsigned long started = last;

    while (!m.window->isClosed() && tl::core::should_keep_running() && !m.quit_requested) {
        tl::core::pump_events();

        const unsigned long now = timer->getMilliseconds();
        if (m.config.bench_seconds > 0.0f &&
            static_cast<float>(now - started) >= m.config.bench_seconds * 1000.0f) {
            TL_LOG_INFO("bench: %.0f s played, quitting",
                        static_cast<double>(m.config.bench_seconds));
            break;
        }
        // CGame::frameStarted: CGame::update takes the
        // frame's time held to 0.1 s.
        const float delta = std::min(static_cast<float>(now - last) * 0.001f, 0.1f);
        last = now;

        // Movement before animation, so the pose advances by the rate the speed
        // this frame implies rather than by the previous frame's.
        const unsigned long long t0 = timer->getMicroseconds();
        m.drive_player(delta);
        const unsigned long long t1 = timer->getMicroseconds();
        m.advance(delta);
        const unsigned long long t2 = timer->getMicroseconds();
        m.prof_drive_us += t1 - t0;
        m.prof_advance_us += t2 - t1;

        // A layout's Cinematic node: setCinematicOpen, up until its
        // button, then event 2.
        // The level stands still under it.
        if (!m.pending_layout_cinematic.empty()) {
            const std::string name = std::move(m.pending_layout_cinematic);
            m.pending_layout_cinematic.clear();
            const MenuAction shown = run_cinematic(std::string(), name);
            for (const Impl::MachineView& view : m.machines()) view.script->cinematic_menu_event(2);
            if (shown == MenuAction::Quit) break;
            last = timer->getMilliseconds();
        }

        // A floor's stairs are Unit Triggers wired Triggered -> Activate Warper; the warp
        // arrives as the script's Warp event.

        // A render-time fault (a material without a shader technique) is caught: on the
        // Switch an uncaught one just ends the process with no log line.
        bool rendered = false;
        try {
            const unsigned long long r0 = timer->getMicroseconds();
            rendered = m.root->renderOneFrame();
            const unsigned long long r1 = timer->getMicroseconds();
            m.prof_render_us += r1 - r0;
            m.log_long_frame(t1 - t0, t2 - t1, r1 - r0, r1 - t0);
            const unsigned long long queued = m.frame_clock.queued_us;
            if (queued >= r0 && queued <= r1) m.prof_submit_us += queued - r0;
            m.save_scripted_shot();
        } catch (const Ogre::Exception& e) {
            TL_LOG_ERROR("render failed, ending the run: %s", e.getFullDescription().c_str());
            break;
        }
        if (!rendered) break;
        if (m.dungeon_transition_pending) break;

        // Frame rate, averaged over five-second windows; the first (shader compilation,
        // uploads) skipped.
        ++frames;
        if (now - window_started >= 5000) {
            const float seconds = static_cast<float>(now - window_started) * 0.001f;
            // Taken at every window's end, the first included, so each figure
            // covers exactly the window it is printed with.
            float busy[tl::core::kCoreCount] = {};
            const bool have_busy = tl::core::core_busy(busy);
            double gpu_ms = 0.0;
            double gpu_pass_ms[GpuTimer::kPasses] = {};
            unsigned gpu_samples = 0;
            static double group_ms[GpuTimer::kGroups];
            static double group_batches[GpuTimer::kGroups];
            double gap_ms[3] = {};
            m.gpu_timer.take_view_groups(group_ms, gap_ms);
            const bool have_gpu = m.gpu_timer.take_mean(gpu_ms, gpu_pass_ms, gpu_samples);
            double pass_batches[GpuTimer::kPasses] = {};
            double interface_batches = 0.0;
            const bool have_batches =
                m.gpu_timer.take_batches(pass_batches, interface_batches, group_batches);
            if (windows > 0) {
                // The sampler's samples from here on belong to this level.
                char mark[64];
                std::snprintf(mark, sizeof mark, "%s depth %d window %u",
                              m.level_dungeon.empty() ? "layout" : m.level_dungeon.c_str(),
                              m.level_depth, windows);
                tl::core::profiler_mark(mark);
                TL_LOG_INFO("%.1f fps over %.1f s (%u frames)",
                            static_cast<double>(static_cast<float>(frames) / seconds),
                            static_cast<double>(seconds), frames);
                m.log_scene_census("");

                // Per frame, in milliseconds: the input and target selection,
                // the selector alone (inside it), the world update, and the draw;
                // then what the last frame drew, so a slow draw can be told
                // apart as too many batches or too many pixels.
                const double n = frames > 0 ? static_cast<double>(frames) : 1.0;
                const Ogre::RenderTarget::FrameStats& drawn = m.window->getStatistics();
                TL_LOG_INFO(
                    "  per frame: input %.2f ms (selector %.2f), update %.2f ms, render %.2f ms "
                    "(%zu batches, %zu triangles)",
                    static_cast<double>(m.prof_drive_us) / n / 1000.0,
                    static_cast<double>(m.prof_select_us) / n / 1000.0,
                    static_cast<double>(m.prof_advance_us) / n / 1000.0,
                    static_cast<double>(m.prof_render_us) / n / 1000.0, drawn.batchCount,
                    drawn.triangleCount);
                // Submit is the CPU issuing batches; present is the GPU or vsync.
                TL_LOG_INFO("  render split: submit %.2f ms, present %.2f ms",
                            static_cast<double>(m.prof_submit_us) / n / 1000.0,
                            static_cast<double>(m.prof_render_us - m.prof_submit_us) / n / 1000.0);
                // Which part of core 0's frame is worth another core: the
                // monsters' AI inside the update, and the submit by OGRE's own
                // phases (FramePhases).
                const auto ms = [n](unsigned long long us) {
                    return static_cast<double>(us) / n / 1000.0;
                };
                TL_LOG_INFO(
                    "  core 0: ai %.2f ms of the update; submit: particles+controllers %.2f, "
                    "scene graph %.2f, cull+animate %.2f, draw calls %.2f ms",
                    ms(m.prof_script_us), ms(m.frame_phases.controllers),
                    ms(m.frame_phases.scene_graph), ms(m.frame_phases.cull),
                    ms(m.frame_phases.draw));
                // The same two phases by what was drawn, and the skinning
                // hand-over's own cost with what it has on the GPU right now.
                using Phases = Impl::FramePhases;
                const Phases& fp = m.frame_phases;
                TL_LOG_INFO(
                    "  cull+animate / draw calls: view %.2f / %.2f, render textures %.2f / %.2f, "
                    "minimap %.2f / %.2f ms; gpu skinning step %.2f ms, %zu of %zu skinned "
                    "entities on the GPU",
                    ms(fp.cull_by[Phases::kView]), ms(fp.draw_by[Phases::kView]),
                    ms(fp.cull_by[Phases::kTexture]), ms(fp.draw_by[Phases::kTexture]),
                    ms(fp.cull_by[Phases::kMinimap]), ms(fp.draw_by[Phases::kMinimap]),
                    ms(m.prof_skin_us), m.gpu_skinning.on_gpu(), m.gpu_skinning.skinned());
                // Core load, clocks and the GPU's time over a sampled frame; console only,
                // each part left out when the platform cannot say.
                unsigned cpu_mhz = 0;
                unsigned gpu_mhz = 0;
                unsigned memory_mhz = 0;
                const bool have_clocks = tl::core::clock_rates(cpu_mhz, gpu_mhz, memory_mhz);
                if (have_busy || have_clocks || have_gpu) {
                    char line[256];
                    int used = std::snprintf(line, sizeof line, "  machine:");
                    const auto room = [&]() {
                        return used < static_cast<int>(sizeof line)
                                   ? sizeof line - static_cast<size_t>(used)
                                   : 0;
                    };
                    if (have_busy) {
                        used += std::snprintf(line + used, room(),
                                              " cores busy %.0f%% / %.0f%% / %.0f%%;",
                                              static_cast<double>(busy[0] * 100.0f),
                                              static_cast<double>(busy[1] * 100.0f),
                                              static_cast<double>(busy[2] * 100.0f));
                    }
                    if (have_clocks && room() > 0) {
                        used += std::snprintf(line + used, room(),
                                              " clocks cpu %u, gpu %u, memory %u MHz;", cpu_mhz,
                                              gpu_mhz, memory_mhz);
                    }
                    if (have_gpu && room() > 0) {
                        used +=
                            std::snprintf(line + used, room(), " gpu %.2f ms a frame (%u sampled)",
                                          gpu_ms, gpu_samples);
                    }
                    TL_LOG_INFO("%s", line);
                }
                // The GPU's time by pass, over the sampled frames.
                if (have_gpu) {
                    char passes[256];
                    int at = std::snprintf(passes, sizeof passes, "  gpu by pass:");
                    for (int p = 0; p < GpuTimer::kPasses && at < static_cast<int>(sizeof passes);
                         ++p) {
                        at += std::snprintf(passes + at, sizeof passes - static_cast<size_t>(at),
                                            " %s %.2f ms;", GpuTimer::pass_name(p), gpu_pass_ms[p]);
                    }
                    TL_LOG_INFO("%s", passes);
                }
                // The batches a frame by pass; the interface's are inside the view's.
                if (have_batches) {
                    char line[256];
                    int at = std::snprintf(line, sizeof line, "  batches by pass:");
                    for (int p = 0; p < GpuTimer::kPasses && at < static_cast<int>(sizeof line);
                         ++p) {
                        at += std::snprintf(line + at, sizeof line - static_cast<size_t>(at),
                                            " %s %.0f;", GpuTimer::pass_name(p), pass_batches[p]);
                    }
                    if (at < static_cast<int>(sizeof line)) {
                        std::snprintf(line + at, sizeof line - static_cast<size_t>(at),
                                      " of the view, interface %.0f", interface_batches);
                    }
                    TL_LOG_INFO("%s", line);
                }
                // The view by render queue group: GPU ms (sampled) and batches.
                if (have_batches || have_gpu) {
                    char line[512];
                    int at = std::snprintf(line, sizeof line,
                                           "  view by queue group (gpu ms / batches): before %.2f, "
                                           "between %.2f, after %.2f;",
                                           gap_ms[0], gap_ms[1], gap_ms[2]);
                    for (int g = 0; g < GpuTimer::kGroups && at < static_cast<int>(sizeof line);
                         ++g) {
                        if (group_ms[g] < 0.005 && group_batches[g] < 0.5) continue;
                        at += std::snprintf(line + at, sizeof line - static_cast<size_t>(at),
                                            " q%d %.2f/%.0f;", g, group_ms[g], group_batches[g]);
                    }
                    TL_LOG_INFO("%s", line);
                }
            }
            Impl::log_driver_costs(windows > 0, frames);
            m.prof_drive_us = m.prof_select_us = m.prof_advance_us = m.prof_render_us = 0;
            m.prof_submit_us = m.prof_script_us = m.prof_skin_us = 0;
            m.log_sections(frames, windows > 0);
            ++windows;
            frames = 0;
            window_started = now;
        }
    }
}

namespace {

// OGRE's EGLWindow::destroy() leaves an Android window's EGL surface in place -
// releasing it is AndroidEGLWindow::_notifySurfaceDestroyed()'s - and the
// activity's window takes one surface at a time, so without this the next Root
// (New Game's restart) cannot make its own. Nothing to do on the other targets.
// With the context preserved (platform_window_params) only the surface goes
// here; the context goes with the window, as Root is released.
void release_window_surface(Ogre::RenderWindow* window) {
#if defined(TL_PLATFORM_ANDROID)
    tl::core::android::set_surface_hooks({});
    if (window != nullptr) static_cast<Ogre::AndroidEGLWindow*>(window)->_notifySurfaceDestroyed();
#else
    (void)window;
#endif
}

}  // namespace

void Renderer::shutdown() {
    Impl& m = *impl_;
    if (!m.root) return;

    // `CSoundObjectDescriptor::descriptorSceneDeactivated`
    // as the level goes: every `Sound` node disabled. What the
    // nodes broadcast goes nowhere; the level is not run again.
    if (m.script_ready) {
        std::vector<tl::core::ScriptEvent> dropped;
        m.script.level_deactivated(m.quest_log.state(), dropped);
        // `CLayoutDescriptor::descriptorSceneDeactivated`: each
        // `Layout` node's own scene `CEditorScene::setActivated(false)` (vtable
        // +0x218) - the layouts the level placed, deactivated in turn.
        for (Impl::NestedScript& nested : m.nested_scripts) {
            nested.script.level_deactivated(m.quest_log.state(), dropped);
        }
    }

    // The mixer first, and before anything else at all: it runs on its own
    // thread and holds decoders over buffers this object owns.
    tl::core::audio_shutdown();

    // Then the interface. Both hold elements owned by the `OverlayManager`, and
    // the HUD also holds an `OverlaySystem` registered with the scene manager;
    // all of it is gone by the time Impl's own members are released below. The
    // menu goes first because its overlay was created after the HUD's.
    m.loading.destroy();
    m.shutdown_character_preview();
    m.quest_dialog.destroy();
    m.death_screen.destroy();
    m.fishing_screen.destroy();
    m.character_load_screen.destroy();
    m.modal_screen.destroy();
    m.waypoint_screen.destroy();
    m.pause_screen.destroy();
    m.settings_screen.destroy();
    m.howto_screen.destroy();
    m.tip_screen.destroy();
    m.inventory_menu.destroy();
    m.menu.destroy();
    m.game_hud.destroy();
    m.hud.destroy();
    m.automap.shutdown();
    m.display_gamma.destroy();

    // Before Root: the light map's camera and quads hang off scene nodes the
    // scene manager is about to destroy, and Impl's own members are released
    // after root.reset() below.
    m.destroy_light_map();
    m.destroy_shadow_map();

    // ParticleUniverse's systems are MovableObjects of the scene manager that
    // root.reset() destroys, and its factories are registered with Root; so it
    // goes before either. Impl's own member would otherwise stop it after both.
    m.destroy_all_particle_systems();
    m.particle_universe.stop();

    // Its materials while the MaterialManager is still there.
    g_view_renderables.gpu = nullptr;
    g_view_renderables.behind = nullptr;
    g_view_renderables.behind_in_play = false;
    m.render_behind_materials.reset();
    m.gpu_skinning.stop();
    m.gpu_timer.stop(m.scene, {m.window, m.light_map_target(), m.shadow_map_target()});

    // While the programs it holds still exist.
    m.save_shader_cache();
    if (m.shader_resolver) {
        Ogre::MaterialManager::getSingleton().removeListener(m.shader_resolver.get());
        m.shader_resolver.reset();
    }
    if (m.shader_generator) {
        Ogre::RTShader::ShaderGenerator::destroy();
        m.shader_generator = nullptr;
    }

    Ogre::STBIImageCodec::shutdown();
    m.root->removeFrameListener(&m.frame_clock);
    m.frame_clock.timer = nullptr;
    release_window_surface(m.window);
    m.root.reset();
    // Nothing is loading once the renderer is gone.
    tl::core::set_loading_boost(false);
#if defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_ANDROID) || defined(TL_PLATFORM_IOS)
    m.gles2_plugin.reset();
#if defined(TL_SWITCH_GL3PLUS)
    m.gl3plus_plugin.reset();
#endif
#else
    m.gl3plus_plugin.reset();
    m.gl_plugin.reset();
#endif

    // Root's destructor does not own the LogManager we created for it.
    delete m.log_manager;
    m.log_manager = nullptr;
    m.log_bridge.reset();
}

void Renderer::toggle_automap() { impl_->automap.cycle_mode(); }

void Renderer::automap_zoom_in() { impl_->automap.zoom_in(); }

void Renderer::automap_zoom_out() { impl_->automap.zoom_out(); }

}  // namespace tl::render
