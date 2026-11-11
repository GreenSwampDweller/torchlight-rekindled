// A placed unit's skills: the [SKILL] blocks CBaseUnit::unitInit gives it and the scene's
// half of CCharacter::castSkill, events run as the unit's. What to cast and when is the
// level script's (LevelScript::select_offensive_skill).
#include "core/unit_skills_reader.h"
#include "render/renderer_impl.h"

namespace tl::render {

// PORT: the drawn unit of a machine's node (the port's scene bookkeeping).
Renderer::Impl::SceneUnit* Renderer::Impl::scene_unit_of(tl::core::LevelScript* machine,
                                                         int64_t node) {
    NodeMap* nodes = nodes_of(machine);
    if (nodes == nullptr) return nullptr;
    if (scene_units_changed) {
        scene_units_by_node.clear();
        for (auto& entry : scene_units) {
            scene_units_by_node[entry.second.node].push_back(&entry.second);
        }
        scene_units_changed = false;
    }
    const auto listed = scene_units_by_node.find(node);
    if (listed == scene_units_by_node.end()) return nullptr;
    const auto range = nodes->equal_range(node);
    for (SceneUnit* candidate : listed->second) {
        SceneUnit& unit = *candidate;
        if (unit.entity == nullptr) continue;
        const Ogre::SceneNode* parent = unit.entity->getParentSceneNode();
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == parent) return &unit;
        }
    }
    return nullptr;
}

namespace {

// The animations the mesh's own skeleton holds before anything is linked onto it, with their
// lengths.
void mesh_own_animations(const std::string& mesh, std::vector<std::string>& names,
                         std::vector<float>& lengths) {
    try {
        Ogre::MeshPtr model = Ogre::MeshManager::getSingleton().getByName(mesh, kGameGroup);
        if (!model || !model->hasSkeleton() || !model->getSkeleton()) return;
        const Ogre::SkeletonPtr& skeleton = model->getSkeleton();
        for (unsigned short i = 0; i < skeleton->getNumAnimations(); ++i) {
            names.push_back(skeleton->getAnimation(i)->getName());
            lengths.push_back(skeleton->getAnimation(i)->getLength());
        }
    } catch (const Ogre::Exception&) {}
}

}  // namespace

tl::core::AnimationCatalog Renderer::Impl::model_catalog(const std::string& mesh,
                                                         const std::string& dir, size_t* missing) {
    // CGenericModel::loadAnimations: the .ANIMATION
    // blocks in order, a missing file passed over, a duplicate skipped, the mesh's BIND
    // filed; each as long as its skeleton's first animation. INVENTED: filed by file stem
    // where the console uses the skeleton's own animation name.
    const tl::core::AnimationManifest& manifest = manifest_for(mesh);
    std::vector<tl::core::AnimationFileEntry> entries;
    std::vector<float> lengths;
    for (const std::string& stem : manifest.order()) {
        tl::core::AnimationFileEntry entry;
        entry.file = stem + ".skeleton";
        float length = 0.0f;
        const std::string path = archive_path_in_dir(stem + ".SKELETON", dir);
        if (!path.empty()) {
            try {
                Ogre::SkeletonPtr skeleton = Ogre::static_pointer_cast<Ogre::Skeleton>(
                    Ogre::SkeletonManager::getSingleton().load(path, kGameGroup));
                if (skeleton) {
                    entry.exists = true;
                    for (unsigned short i = 0; i < skeleton->getNumAnimations(); ++i) {
                        entry.skeleton_names.push_back(skeleton->getAnimation(i)->getName());
                    }
                    if (skeleton->getNumAnimations() > 0)
                        length = skeleton->getAnimation(0)->getLength();
                }
            } catch (const Ogre::Exception& e) {
                TL_LOG_WARN("skills: could not load '%s': %s", path.c_str(),
                            e.getDescription().c_str());
            }
        } else if (missing != nullptr) {
            ++*missing;
        }
        entries.push_back(entry);
        lengths.push_back(length);
    }
    // What the mesh's own skeleton holds before anything is linked onto it.
    std::vector<std::string> has;
    std::vector<float> has_lengths;
    mesh_own_animations(mesh, has, has_lengths);
    std::string model_stem = mesh;
    if (const size_t slash = model_stem.find_last_of("/\\"); slash != std::string::npos) {
        model_stem = model_stem.substr(slash + 1);
    }
    if (model_stem.size() >= 5) model_stem.resize(model_stem.size() - 5);
    const std::vector<tl::core::LoadedAnimation> list =
        tl::core::load_animation_list(entries, model_stem, has);

    // The set's first names are the mesh's own (one named `*` left out), as the list counts them.
    tl::core::AnimationCatalog catalog;
    for (size_t i = 0; i < has.size(); ++i) {
        if (has[i] != "*") catalog.emplace_back(has[i], has_lengths[i]);
    }
    for (size_t b = 0; b < list.size(); ++b) {
        if (list[b].index < 0) continue;
        const size_t at = static_cast<size_t>(list[b].index);
        if (catalog.size() <= at) catalog.resize(at + 1);
        catalog[at] = {manifest.order()[b], lengths[b]};
    }
    return catalog;
}

void Renderer::Impl::step_unit_skills(float delta) {
    if (!(delta > 0.0f)) return;
    // The cast animations: the keys each frame steps past (`updateAnimation`),
    // then `updateSkillKeys`
    // and the groups they fire.
    for (size_t i = 0; i < unit_skill_playbacks.size();) {
        UnitSkillPlayback& p = unit_skill_playbacks[i];
        tl::core::LevelScript::UnitCasting* k = p.machine->unit_casting(p.node);
        SceneUnit* drawn = scene_unit_of(p.machine, p.node);
        if (k == nullptr || drawn == nullptr || p.machine->unit_is_dead(p.node)) {
            unit_skill_playbacks.erase(unit_skill_playbacks.begin() + static_cast<long>(i));
            continue;
        }
        const float from = p.time;
        float to = from + delta * p.speed;
        if (p.length > 0.0f && to > p.length) {
            to = p.held < 0.0f ? std::fmod(to, p.length) : p.length;
        }
        p.time = to;
        const tl::core::AnimationManifest& manifest = manifest_for(drawn->mesh);
        const std::vector<tl::core::FiredKey> keys =
            tl::core::keys_crossed(manifest.tags_for(p.animation), p.index, from, to);
        const std::string name = k->cast.skill;
        const tl::core::SkillRunState* state = k->runner.state(name);
        if (state != nullptr && !keys.empty()) {
            const std::vector<int> groups =
                tl::core::update_skill_keys(k->cast, state->animations, k->runner.is_running(name),
                                            !p.machine->unit_is_dead(p.node), keys);
            tl::core::LevelScript* const owner = p.machine;
            const int64_t node = p.node;
            for (int group : groups) {
                // The run's own target (`CSkill +0x40`) and point stand: the
                // group is fired with none of its own.
                tl::core::SkillCastContext context =
                    unit_cast_context(*owner, node, nullptr, 0, false);
                const UnitEventCaster saved = unit_event_caster;
                unit_event_caster = unit_event_caster_for(owner, node);
                const int applied = k->runner.trigger_event(
                    group, context, [this, owner, node](const std::string& n) {
                        return unit_skill_named(*owner, node, n);
                    });
                unit_event_caster = saved;
                TL_LOG_INFO(
                    "skills: %s's '%s' key at %.3fs fires %s, %d skill(s) applied",
                    owner->unit_of(node).c_str(), p.animation.c_str(), static_cast<double>(to),
                    group == tl::core::kSkillEventTrigger ? "EVENT_TRIGGER" : "EVENT_TRIGGER_TWO",
                    applied);
            }
        }
        // `CCharacter::updateAnimation`:
        // a looping cue lasts while the cast holds; once the flag
        // (`+0x267`) is down and no swing has time left, the gait takes the
        // model back.
        if (p.held < 0.0f && !k->cast.casting &&
            !(k->cast.animation_left > 0.0f && !k->cast.attack_ended)) {
            unit_skill_playbacks.erase(unit_skill_playbacks.begin() + static_cast<long>(i));
            continue;
        }
        // The cue's hold: then its queued loop, or nothing - the unit's gait
        // takes the model back.
        if (p.held >= 0.0f) {
            p.held -= delta;
            if (p.held <= 0.0f) {
                if (p.then_loop >= 0 &&
                    p.then_loop < static_cast<int>(unit_catalog(drawn->mesh)->size())) {
                    const auto& entry =
                        (*unit_catalog(drawn->mesh))[static_cast<size_t>(p.then_loop)];
                    Ogre::AnimationState* started = nullptr;
                    if (play_scene_animation(drawn->entity, drawn->mesh, entry.first, true,
                                             drawn->rate, true, &started, drawn->armed)) {
                        remember_state(drawn->entity, entry.first, started);
                        drawn->playing = entry.first;
                        drawn->playing_state = started;
                    }
                    p.animation = entry.first;
                    p.index = p.then_loop;
                    p.length = entry.second;
                    p.speed = 1.0f;
                    p.held = -1.0f;
                    p.time = 0.0f;
                    p.then_loop = -1;
                } else {
                    unit_skill_playbacks.erase(unit_skill_playbacks.begin() + static_cast<long>(i));
                    continue;
                }
            }
        }
        ++i;
    }

    // Every unit holding a skill.
    for (const MachineView& view : machines()) {
        tl::core::LevelScript* const owner = view.script;
        for (int64_t node : owner->casting_units()) step_unit_skill_manager(*owner, node, delta);
    }
}

// Every unit holding a skill: `updateActiveSkill`, then the
// manager's `update` - cooldowns, runs, `EVENT_END`.
void Renderer::Impl::step_unit_skill_manager(tl::core::LevelScript& machine, int64_t node,
                                             float delta) {
    tl::core::LevelScript* const owner = &machine;
    tl::core::LevelScript::UnitCasting* k = owner->unit_casting(node);
    if (k == nullptr) return;
    // DIVERGENCE: `CCharacter::unitInit`
    // fires these as the unit is
    // made; here it is the first frame the unit's skills are stepped,
    // when its model is in the scene for the events to use.
    if (!k->create_fired) fire_unit_skills_on_create(*owner, node);
    const std::string name = k->cast.skill;
    const tl::core::SkillRunState* state = k->runner.state(name);
    int64_t upkeep = 0;
    if (const tl::core::KnownSkill* known = unit_skill_named(*owner, node, name)) {
        if (state != nullptr) {
            if (const tl::core::SkillProperty* property = known->property(state->level)) {
                upkeep = static_cast<int64_t>(property->mana_cost_ot);
            }
        }
    }
    const tl::core::ActiveSkillUpdate update = tl::core::update_active_skill(
        k->cast, state != nullptr ? state->animations : tl::core::SkillAnimations{},
        k->runner.is_running(name), upkeep, k->mana, delta);
    k->mana = std::max(0.0f, k->mana - update.mana_spent);
    if (update.hide_trails) {
        if (const SceneUnit* drawn = scene_unit_of(owner, node)) {
            const auto trails = unit_trails.find(drawn->entity);
            if (trails != unit_trails.end()) trails->second.trails.hide_hands();
        }
    }
    const auto lookup = [this, owner, node](const std::string& n) {
        return unit_skill_named(*owner, node, n);
    };
    if (!update.stop_all && !k->runner.any_running()) {
        // Nothing of it runs: the update only counts its cooldowns down.
        prepare_unit_cast(*owner, node);
        k->runner.update(delta, tl::core::SkillCastContext{}, lookup);
        return;
    }
    tl::core::SkillCastContext context = unit_cast_context(*owner, node, nullptr, 0, false);
    const UnitEventCaster saved = unit_event_caster;
    unit_event_caster = unit_event_caster_for(owner, node);
    // `updateActiveSkill`:
    // `stopAllSkills(false, false, false)` when the upkeep runs dry.
    if (update.stop_all) k->runner.stop_all(context, lookup, false, false);
    k->runner.update(delta, context, lookup);
    unit_event_caster = saved;
}

}  // namespace tl::render
