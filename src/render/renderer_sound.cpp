// The renderer's implementation: interaction, music, banners, sounds and voices, path blockers, unit index.
// Split from renderer.cpp; the shared
// declarations are in renderer_impl.h.
#include "core/unit_stats_reader.h"
#include "render/renderer_impl.h"

namespace tl::render {
// PORT: the drawn entity of a trigger's node (the port's scene bookkeeping).
Ogre::Entity* Renderer::Impl::trigger_entity(int64_t node, const std::string& unit,
                                             const NodeMap& nodes) const {
    const auto range = nodes.equal_range(node);
    const auto units = scene_units.equal_range(unit);
    for (auto it = units.first; it != units.second; ++it) {
        if (it->second.node != node || it->second.entity == nullptr) continue;
        const Ogre::SceneNode* where = it->second.entity->getParentSceneNode();
        for (auto n = range.first; n != range.second; ++n) {
            if (n->second == where) return it->second.entity;
        }
    }
    return nullptr;
}

// A on a targeted prop: CCharacter::getItemAI once in
// reach, in sight and faced. ENCHANTSHRINE (ISA 0x9d) puts the player in AI state 0x16;
// STATSHRINE (0x9e) and GEMSHRINE (0x9f) name 0x17 and 0x18; anything else is state 0 and
// CTriggerUnit::interact.
void Renderer::Impl::interact_targeted_trigger() {
    tl::core::LevelScript* owner = machine_script(trigger_targeted_machine);
    if (owner == nullptr) return;
    // A `FISHING HOLE` (ISA 0x7a) is not interacted with either: the
    // character's `startFishing` (vtable +0x338).
    for (const tl::core::LevelScript::UnitTriggerView& view : owner->unit_triggers()) {
        if (view.id != trigger_targeted_node || !is_fishing_hole(view.unit)) continue;
        start_fishing(Ogre::Vector3(view.at.x, view.at.y, view.at.z));
        return;
    }
    for (const tl::core::LevelScript::UnitTriggerView& view : owner->unit_triggers()) {
        if (view.id != trigger_targeted_node || !is_enchant_shrine(view.unit)) continue;
        open_enchant_shrine(trigger_targeted_machine, trigger_targeted_node);
        return;
    }
    std::vector<tl::core::ScriptEvent> events;
    owner->interact_unit_trigger(trigger_targeted_node, quest_log.state(), events);
    TL_LOG_INFO("pad prop: A on unit trigger %lld, %zu event(s)",
                static_cast<long long>(trigger_targeted_node), events.size());
    // A room's machine that had gone quiet has something to run again.
    if (trigger_targeted_machine >= 0) {
        nested_scripts[static_cast<size_t>(trigger_targeted_machine)].running = true;
    }
    apply_machine_events(trigger_targeted_machine, events);
}

/// The other half of `getItemAI`/`getItem`:
/// take whatever `update_item_selection` is pointing at. An item the pack has
/// no room for is left sitting rather than forced somewhere or lost.
void Renderer::Impl::pick_up_targeted_item() {
    if (item_targeted < 0 || static_cast<size_t>(item_targeted) >= dropped_items.size()) return;
    // `getItem`'s gold branch (ISA 0x22).
    if (dropped_items[static_cast<size_t>(item_targeted)].is_gold) {
        take_gold(static_cast<size_t>(item_targeted));
        return;
    }
    load_unit_types();
    DroppedWorldItem dropped = dropped_items[static_cast<size_t>(item_targeted)];
    // The item's quest (`+0x170` / `+0x178`), where a ground item holds it as
    // the tag it was placed with.
    if (dropped.item.quest_guid == -1 && !dropped.quest_tag.empty()) {
        dropped.item.quest_guid = quest_guid_of(dropped.quest_tag);
        dropped.item.quest_unit_id = dropped.quest_tag.objective;
    }
    // `CEquipment::addedToInventory` raises
    // the quest's event 0 from inside the pickup (`bag_item_added`), about the
    // item where it lay.
    bag_event_at = dropped.node->getPosition();
    bag_event_at_set = true;
    const int landed = player_inventory.pickup(dropped.item, dropped.count, unit_types);
    bag_event_at_set = false;
    if (landed < 0) {
        // `getItem`: no room anywhere, for a player.
        queue_tip(tl::core::ContextTip::InventoryFull);
        return;
    }

    TL_LOG_INFO("picked up %d x %s", dropped.count, dropped.item.display_name.c_str());
    // `getItem`: a player's tips for what it now carries.
    {
        const uint32_t type = unit_types.id_of(dropped.item.unittype);
        for (tl::core::ContextTip tip : tl::core::item_picked_up_tips(
                 unit_types.isa(type, tl::core::unit_type::kSocketable),
                 unit_types.isa(type, tl::core::unit_type::kSpell), dropped.item.identified)) {
            queue_tip(tip);
        }
    }
    // getItem: after a pickup, canEquip(item, false) and,
    // with a free allowed location and the requirements met,
    // equipEquipmentIntoFirstFreeLocation. One already in an equip location stays worn.
    if (landed >= static_cast<int>(tl::core::Inventory::kFixedSlotCount)) {
        const tl::core::ItemStats& item =
            player_inventory.slots()[static_cast<size_t>(landed)].item;
        if (player_inventory.can_equip(item, unit_types, &player_character, &player_effects) &&
            player_inventory.equip_into_first_free_location(static_cast<size_t>(landed), unit_types,
                                                            &player_character, &player_effects)) {
            TL_LOG_INFO("picked up %s and put it on", dropped.item.display_name.c_str());
            sync_equipped_weapon();
            refresh_skill_effects();
        }
    }
    // `CEquipment::playTakeSound`: slot 0x12 on the **item's own**
    // bank - its `TAKE_SOUND`, `StaffGet` for a staff - not the interface's.
    play_sound(dropped.item.take_sound, dropped.node->getPosition());
    // CLevel::removeItem(item, true) (inlined into getItem):
    // CBaseUnit::deactivateUnitInLevel and off the level's lists.
    remove_ground_item(dropped_items[static_cast<size_t>(item_targeted)]);
    dropped_items.erase(dropped_items.begin() + item_targeted);
    item_targeted = -1;
    // `CItem`'s unit state 3 reaches the spawner that released it.
    if (dropped.unit_id != 0) spawned_item_state(dropped.unit_id, 3);
    item_highlighted = nullptr;
    refresh_game_hud();
}

// `../music/town.ogg` is relative to `media/`, so the leading `../` steps out of the archive and
// into the install directory: a mod's own `music/` first (`CModFileFilter::getFilePath`,
// keys a mod's files by their path in its folder), then the game folder's, through
// the case-insensitive index (the data's music names are cased unlike the files).
namespace {
std::string music_file(const RendererConfig& config, const std::string& relative) {
    std::string tail = relative;
    while (tail.compare(0, 3, "../") == 0) tail.erase(0, 3);
    std::string from_mod = renderer_detail::mod_file_path(tail);
    if (!from_mod.empty()) return from_mod;
    if (config.assets != nullptr) {
        const std::string real = config.assets->resolve(tail);
        if (!real.empty()) tail = real;
    }
    std::string full = config.game_data;
    if (!full.empty() && full.back() != '/' && full.back() != '\\') full += '/';
    return full + tail;
}
}  // namespace

// What a `Music` node's CMusicObject (core/music_object.h) reaches: CSoundManager's music
// channel and the level's `MUSIC`.
tl::core::MusicHost Renderer::Impl::music_host(int64_t node) {
    tl::core::MusicHost host;
    // `getMusicFilePlaying`: the track on the channel, as `playMusic` names it.
    host.playing = [this]() {
        const bool silent = !audio_enabled || !tl::core::audio_available();
        if (music_track.empty()) return std::string();
        if (!silent && !tl::core::audio_is_channel_playing(tl::core::Channel::Music)) {
            return std::string();
        }
        return tl::core::music_track_key(music_track);
    };
    // playMusic(file, loops): stopMusic, then the track and its length (getSoundLength).
    // INVENTED: an already-reduced name is looked for under ../music/.
    host.play = [this](const std::string& file, bool loops) {
        std::string relative = file;
        if (relative.compare(0, 3, "../") != 0 && relative.find('/') == std::string::npos) {
            relative = "../music/" + relative;
        }
        play_music(std::string(), false, "");
        if (!play_music(relative, loops, "a Music node plays ") && music_track != relative) {
            return 0.0f;
        }
        if (loops) return 0.0f;
        const std::vector<uint8_t> bytes = read_file(music_file(config, relative));
        return bytes.empty() ? 0.0f : tl::formats::ogg_duration(bytes.data(), bytes.size());
    };
    host.stop = [this]() { play_music(std::string(), false, ""); };
    host.level_music = [this]() { return tl::core::music_track_key(level_music); };
    // The node's output, through the machine that holds it.
    host.output = [this, node](uint32_t output) {
        tl::core::LevelScript* owner = script_owning(node);
        if (owner == nullptr) return;
        std::vector<tl::core::ScriptEvent> events;
        owner->raise_output(node, output, quest_log.state(), events);
        if (!events.empty()) apply_script_events(events, scripted_nodes);
    };
    return host;
}

bool Renderer::Impl::play_music(const std::string& relative, bool loop, const char* because) {
    // A run with no audio device still resolves the file (not decode it), so headless runs
    // check the music path.
    const bool silent = !audio_enabled || !tl::core::audio_available();

    if (relative == music_track) return true;
    if (relative.empty()) {
        if (!silent) tl::core::audio_stop(tl::core::Channel::Music);
        music_track.clear();
        return true;
    }

    const std::string full = music_file(config, relative);

    if (silent) {
        // A track present counts as playing (getMusicFilePlaying), so a Music node runs its
        // course on a silent run too.
        if (is_file(full)) {
            TL_LOG_INFO("music (no audio device, not played): %s", full.c_str());
            music_track = relative;
        } else {
            TL_LOG_WARN("no music at '%s' (%s%s)", full.c_str(), because, relative.c_str());
        }
        return false;
    }

    std::vector<uint8_t> bytes = read_file(full);
    if (bytes.empty()) {
        // Not an error worth stopping for. The music sits outside the archive,
        // and on the console it is only there if it was deployed - see
        // the deploy step's music option.
        TL_LOG_WARN("no music at '%s' (%s%s)", full.c_str(), because, relative.c_str());
        return false;
    }

    TL_LOG_INFO("music: %s (%zu bytes, %s)", full.c_str(), bytes.size(), loop ? "looping" : "once");
    if (!tl::core::audio_play(tl::core::Channel::Music, std::move(bytes), relative, loop, 1.0f)) {
        return false;
    }
    music_track = relative;
    return true;
}

void Renderer::Impl::player_levelled_up() {
    // `CCharacter::levelUp` has already run,
    // in `award_experience`.
    character_level = player_character.level();
    // health and mana set to their new maxima.
    refresh_player_maxima();
    player_health.current = player_health.max;
    player_health.dead = false;

    // the particle, made once by `unitInit` and started
    // again on every level-up. This engine places it afresh instead, which
    // leaves the same one running: the previous one is taken down first.
    clear_level_up_particle();
    const std::string file = preloaded_particle_file("PLAYER_LEVELUP");
    if (!file.empty() && subject_node != nullptr) {
        level_up_particle_nodes = place_particle_layout(file, Ogre::Vector3::ZERO,
                                                        Ogre::Quaternion::IDENTITY, subject_node);
        TL_LOG_INFO("level up: %s on the player (%zu holder(s))", file.c_str(),
                    level_up_particle_nodes.size());
    }

    // CPlayer::levelUp plays the chime (slot 25 bound by CPlayer::unitInit)
    // and the narrator, banners between. The chime
    // only while the bank is not playing it (CSoundBank::bankPlaying(25)):
    // two levels at once ring once.
    if (!tl::core::audio_voice_playing(
            lowered(tl::core::interface_sound_name(tl::core::InterfaceSound::LevelUp)))) {
        play_ui(tl::core::InterfaceSound::LevelUp);
    }
    set_big_text(tl::core::level_up_banner(), tl::core::kLevelUpBannerSeconds);
    set_small_text(tl::core::level_up_caption(player_character.level()),
                   tl::core::kLevelUpBannerSeconds);
    play_voice(tl::core::SoundSlot::VoiceLevelUp);
    TL_LOG_INFO("level up: '%s' / '%s'", big_banner.text.c_str(), small_banner.text.c_str());
    // `CInventory::refreshEquipped()` before
    // the tip: every listener's `equipmentEquipped(nullptr)` - the character's
    // returns at once, the menus redraw.
    update_menu();
    //
    queue_tip(tl::core::ContextTip::LevelUp);
    // CInventory::forceRecalculationOfEquipmentStats() (inline in levelUp
    // on the console, after the new maxima).
    player_inventory.calculate_effect_values();
    refresh_game_hud();
}

void Renderer::Impl::refresh_game_hud() {
    if (!game_hud.ready()) return;

    tl::render::HudState state;
    state.health =
        player_health.max > 0.0f ? std::max(0.0f, player_health.current) / player_health.max : 1.0f;
    state.mana = player_character.max_mp() > 0.0f
                     ? std::max(0.0f, player_character.mp()) / player_character.max_mp()
                     : 1.0f;
    state.experience = player_character.experience_progress(stat_graphs);
    // `CLevel::getName`, written into the
    // `LevelName` window every frame by `CGameUI::updateIngameUI`.
    state.level_name = level_display_name;
    if (big_banner.alpha > 0.0f) {
        state.announce = big_banner.text;
        state.announce_alpha = big_banner.alpha;
    }
    if (small_banner.alpha > 0.0f) {
        state.sub_announce = small_banner.text;
        state.sub_announce_alpha = small_banner.alpha;
    }
    state.available_points = available_points.plus;
    state.available_points_glow = available_points.glow;

    // The panel belongs on screen while there is an animal in the level. A
    // character chooses a dog or a cat when it is created and never gives it
    // up, so the only thing that takes the panel away is a trip to town.
    state.pet = pet.alive() && pet.in_world();
    state.pet_health = pet.health_fraction();
    state.pet_fleeing = pet.state() == tl::core::PetState::Fleeing;
    state.pet_stance = pet.stance() == tl::core::PetStance::Aggressive  ? 0
                       : pet.stance() == tl::core::PetStance::Defensive ? 1
                                                                        : 2;

    // `CGameUI::updateSlots`: a slot holding
    // a skill the manager knows, with an icon, shows the skill; any other shows
    // its item half.
    state.skill_set = player_skills.skill_set();
    for (int slot = 0; slot < tl::core::kSkillFunctionSlots; ++slot) {
        const size_t i = static_cast<size_t>(slot);
        const tl::core::KnownSkill* skill = player_skills.find_by_guid(player_skills.slots()[i]);
        if (skill != nullptr && skill->def != nullptr && !skill->def->icon.empty()) {
            state.slots[i] = tl::core::skill_slot_view(
                skill->def->icon, player_skill_runner.cooling_time(skill->entry.name));
            continue;
        }
        const int64_t guid = player_skills.second_slots()[i];
        if (guid == -1) continue;
        const tl::core::ItemStats* item = items.find_by_guid(guid);
        state.slots[i] =
            tl::core::item_slot_view(item != nullptr ? item->icon : std::string(),
                                     tl::core::equipment_count_of_guid(player_inventory, guid));
    }

    // The pad's target in hud_xenon.layout's TargetHealth panel.
    if (!pad_selected.empty()) {
        state.target = true;
        const tl::core::UnitStats* stats = unit_book.find(pad_selected);
        state.target_name = stats != nullptr && !stats->display_name.empty()
                                ? tl::core::strip_markup(stats->display_name)
                                : pad_selected;
        // The character's own name (`+0x4c0`) where it was given one.
        const auto named = unit_names.find(pad_selected_node);
        if (named != unit_names.end()) state.target_name = tl::core::strip_markup(named->second);
        state.target_champion =
            (stats != nullptr && stats->is_champion) || spawn_marked_champion(pad_selected_node);
        state.target_level = effective_monster_level(pad_selected_node);
        if (stats != nullptr) state.target_description = stats->description;
        // A machine's character keeps its hit points in the machine
        // (LevelScript::unit_health); anything else in unit_health here.
        float current = 0.0f;
        float most = 0.0f;
        const tl::core::LevelScript* owner = script_owning(pad_selected_node);
        if (owner == nullptr || !owner->unit_health(pad_selected_node, current, most)) {
            const Health& health = health_of(pad_selected_node, pad_selected);
            current = health.current;
            most = health.max;
        }
        state.target_health = most > 0.0f ? std::max(0.0f, current) / most : 1.0f;
    }

    game_hud.set_state(state);
}

namespace {

std::string node_channel_key(size_t index) { return "node-channel/" + std::to_string(index); }

}  // namespace

// The mixer behind a `Sound` node's bank. The rules - when a channel starts,
// at what volume, when it stops - are the console's, in `core/sound_object`;
// these are its `CSoundManager` calls.
tl::core::SoundBankHost Renderer::Impl::node_sound_host() {
    tl::core::SoundBankHost host;
    // CSoundManager::playSound(file, volume, node):
    // nothing at no volume; else + randomBetweenVolatile(0, VOLUMEVARIATION), held to 1.
    host.play = [this](const std::string& file, float volume, float variation,
                       const tl::core::Vec3* at, bool loop) -> int32_t {
        if (!(volume > 0.0f)) return -1;
        const float gain =
            std::min(1.0f, volume + tl::core::volatile_random().between(0.0f, variation));
        size_t index = 0;
        for (; index < node_channels.size(); ++index) {
            if (!node_channel_sounding(index)) break;
        }
        if (index == node_channels.size()) return -1;
        NodeChannel& channel = node_channels[index];
        const std::string key = node_channel_key(index);
        tl::core::audio_stop_voice(key);
        channel = NodeChannel{};
        channel.positional = at != nullptr;
        if (at != nullptr) channel.at = Ogre::Vector3(at->x, at->y, at->z);
        channel.gain = gain;
        channel.pan = channel.positional ? node_channel_pan(channel.at) : 0.0f;
        if (!audio_enabled || !tl::core::audio_available()) {
            // A headless run resolves the file once and says so, as
            // `play_sound()` does: the only check a silent run can make.
            if (reported_sounds.insert(lowered(file)).second) {
                const std::string resolved = resolve_archive_path(file);
                if (resolved.empty()) {
                    TL_LOG_WARN("sound node (not played): '%s' is not in the archive",
                                file.c_str());
                    return -1;
                }
                TL_LOG_INFO("sound node (no audio device, not played): %s, volume %.2f%s",
                            resolved.c_str(), static_cast<double>(gain), loop ? ", looping" : "");
            }
            // DIVERGENCE (tooling): with no device the channel is held without a voice, so
            // the banks draw their volatile numbers as with sound.
            channel.used = true;
            channel.silent = true;
            channel.silent_loop = loop;
            return static_cast<int32_t>(index);
        }
        tl::core::SamplePtr sample = sample_for(file);
        if (!sample) return -1;
        if (!tl::core::audio_play_voice(std::move(sample), key, gain, channel.pan, loop)) return -1;
        channel.used = true;
        if (reported_sounds.insert(lowered(file)).second) {
            TL_LOG_INFO("sound node: %s on channel %zu (gain %.2f%s)", file.c_str(), index,
                        static_cast<double>(gain), loop ? ", looping" : "");
        }
        return static_cast<int32_t>(index);
    };
    // `CSoundManager::isChannelPlaying`.
    host.playing = [this](int32_t id) {
        if (id < 0 || static_cast<size_t>(id) >= node_channels.size()) return false;
        return node_channel_sounding(static_cast<size_t>(id));
    };
    // `CSoundManager::stopSound`.
    host.stop = [this](int32_t id) {
        if (id < 0 || static_cast<size_t>(id) >= node_channels.size()) return;
        tl::core::audio_stop_voice(node_channel_key(static_cast<size_t>(id)));
        node_channels[static_cast<size_t>(id)].used = false;
    };
    host.set_volume = [this](int32_t id, float volume) {
        if (id < 0 || static_cast<size_t>(id) >= node_channels.size()) return;
        NodeChannel& channel = node_channels[static_cast<size_t>(id)];
        if (!channel.used) return;
        channel.gain = volume;
        tl::core::audio_set_voice_mix(node_channel_key(static_cast<size_t>(id)), channel.gain,
                                      channel.pan);
    };
    host.set_paused = [this](int32_t id, bool paused) {
        if (id < 0 || static_cast<size_t>(id) >= node_channels.size()) return;
        if (!node_channels[static_cast<size_t>(id)].used) return;
        tl::core::audio_set_voice_paused(node_channel_key(static_cast<size_t>(id)), paused);
    };
    return host;
}

bool Renderer::Impl::node_channel_sounding(size_t index) const {
    const NodeChannel& channel = node_channels[index];
    if (!channel.used) return false;
    if (channel.silent) return channel.silent_loop;
    return tl::core::audio_voice_playing(node_channel_key(index));
}

// Which ear: where the node sits across the camera's right from the listener,
// the port's stand-in for FMOD's 3D panning (the mixer is the port's own).
float Renderer::Impl::node_channel_pan(const Ogre::Vector3& at) const {
    if (camera == nullptr) return 0.0f;
    const Ogre::Vector3 listener = subject_node != nullptr && player_control
                                       ? subject_node->getPosition()
                                       : camera->getDerivedPosition();
    Ogre::Vector3 towards = at - listener;
    const float away = towards.normalise();
    if (away <= 0.0f) return 0.0f;
    return std::max(-1.0f, std::min(1.0f, camera->getDerivedRight().dotProduct(towards)));
}

void Renderer::Impl::step_node_channels(float /*delta*/) {
    for (size_t index = 0; index < node_channels.size(); ++index) {
        NodeChannel& channel = node_channels[index];
        if (!channel.used || !channel.positional) continue;
        const float pan = node_channel_pan(channel.at);
        if (std::fabs(pan - channel.pan) < 1e-3f) continue;
        channel.pan = pan;
        tl::core::audio_set_voice_mix(node_channel_key(index), channel.gain, channel.pan);
    }
}

/// Play one authored sound definition, positioned in the level; every number is the
/// definition's. INVENTED: the rolloff shape - linear to silence at RADIUS, panned across
/// the camera's right vector.
bool Renderer::Impl::play_sound(const std::string& name, const Ogre::Vector3& at) {
    return play_sound(name, at, 1.0f, 0.0f, std::string());
}

bool Renderer::Impl::play_sound_drawn(const std::string& name, const std::string& recording,
                                      float variation, const Ogre::Vector3& at) {
    // PORT: the mix of a sample the machine drew (`play_unit_sample`).
    const SoundDefinition* definition = sound_by_name(name);
    if (definition == nullptr || definition->files.empty()) return false;
    if (!audio_enabled || !tl::core::audio_available()) return false;
    float gain = 0.0f;
    float pan = 0.0f;
    if (!positional_mix(definition->volume, definition->radius, at, gain, pan) || !(gain > 0.0f))
        return false;
    gain = std::min(1.0f, gain + variation);
    // The recording the bank's instance holds, as the machine drew it.
    tl::core::SamplePtr sample =
        sample_for(recording.empty() ? pick_recording(*definition) : recording);
    if (!sample) return false;
    return tl::core::audio_play_voice(std::move(sample), lowered(name), gain, pan);
}

bool Renderer::Impl::play_sound(const std::string& name, const Ogre::Vector3& at,
                                float volume_scale, float radius_if_none,
                                const std::string& voice_key, bool volume_given) {
    if (name.empty()) return false;
    const SoundDefinition* definition = sound_by_name(name);
    if (definition == nullptr) {
        // Worth one line: a tag naming a sound the tables have not got is a
        // misreading of the tag, not a missing file.
        if (reported_sounds.insert(lowered(name)).second) {
            TL_LOG_WARN("sound: nothing in media/sounds/ is called '%s'", name.c_str());
        }
        return false;
    }
    if (definition->files.empty()) return false;

    // CSoundBank::playSample: a chance rolls
    // randomIntegerBetweenVolatile(1, 100) and plays only above the roll (PLAYCHANCE:100 is
    // silent once in a hundred). Rolled with or without a device.
    tl::core::RunicRandom& rolls = tl::core::volatile_random();
    if (definition->play_chance >= 0 && definition->play_chance <= rolls.integer_between(1, 100)) {
        return false;
    }
    // Which of the bank slot's samples: `randomIntegerBetweenVolatile(0, count -
    // 1)` over the one a sound played by name stands in for. Which
    // recording of it sounds is the sound system's choice, not a draw.
    (void)rolls.integer_between(0, 0);
    const std::string& file = pick_recording(*definition);
    // `playSample`: a volume handed in replaces the definition's (`+0xac` x
    // the 3rd argument); none is the definition's (`+0x78`). `CSoundManager::playSound`:
    // nothing at no volume.
    const float volume = volume_given ? volume_scale : definition->volume * volume_scale;
    if (!(volume > 0.0f)) return false;

    // A run with no audio device still resolves the file, once, as play_music() does.
    if (!audio_enabled || !tl::core::audio_available()) {
        if (reported_sounds.insert(lowered(name)).second) {
            const std::string resolved = resolve_archive_path(definition->files.front());
            if (resolved.empty()) {
                TL_LOG_WARN("sound (not played): '%s' names '%s', which is not in the archive",
                            name.c_str(), definition->files.front().c_str());
            } else {
                TL_LOG_INFO(
                    "sound (no audio device, not played): %s -> %s (%zu recording(s), "
                    "volume %.2f, radius %.1f, play chance %d)",
                    name.c_str(), resolved.c_str(), definition->files.size(),
                    static_cast<double>(definition->volume),
                    static_cast<double>(definition->radius), definition->play_chance);
            }
        }
        return false;
    }
    // Started: `randomBetweenVolatile(0, VOLUMEVARIATION)` added
    // (once FMOD's `playSound` has succeeded), the sum held to 1.
    const float variation = rolls.between(0.0f, definition->volume_variation);

    float gain = 0.0f;
    float pan = 0.0f;
    // `CSoundBank::addSample`: the
    // definition's radius, or the bank's when it authors none.
    const float radius = definition->radius != 0.0f ? definition->radius : radius_if_none;
    // PORT: the falling off with distance, which the console's emitter does.
    const bool audible = positional_mix(volume, radius, at, gain, pan);
    if (!audible || !(gain > 0.0f)) return false;
    gain = std::min(1.0f, gain + variation);

    const std::string key = lowered(name);
    tl::core::SamplePtr sample = sample_for(file);
    if (!sample) return false;
    if (!voice_key.empty()) tl::core::audio_stop_voice(voice_key);
    const bool played = tl::core::audio_play_voice(std::move(sample),
                                                   voice_key.empty() ? key : voice_key, gain, pan);
    // Once per definition, so a log carries what a scene actually made a noise
    // about without carrying one line per footfall.
    if (played && reported_sounds.insert(key).second) {
        TL_LOG_INFO("sound: %s -> %s (%zu recording(s), gain %.2f, pan %+.2f)", name.c_str(),
                    file.c_str(), definition->files.size(), static_cast<double>(gain),
                    static_cast<double>(pan));
    }
    return played;
}

bool Renderer::Impl::positional_mix(float volume, float radius, const Ogre::Vector3& at,
                                    float& gain, float& pan) const {
    gain = volume;
    pan = 0.0f;
    if (radius > 0.0f) {
        // The listener is the character, not the camera: authored radii are a few units,
        // and the camera sits far further back.
        const Ogre::Vector3 listener =
            subject_node != nullptr && player_control
                ? subject_node->getPosition()
                : (camera != nullptr ? camera->getDerivedPosition() : Ogre::Vector3::ZERO);
        const float away = listener.distance(at);
        if (away >= radius) {
            gain = 0.0f;
            return false;
        }
        gain *= 1.0f - away / radius;
        // Which ear, though, is the camera's question: left and right are what
        // the viewer sees, not what the character faces.
        if (camera != nullptr) {
            const Ogre::Vector3 right = camera->getDerivedRight();
            pan = std::max(-1.0f, std::min(1.0f, right.dotProduct(at - listener) / radius));
        }
    }
    return gain > 0.0f;
}

bool Renderer::Impl::play_sound_2d(const std::string& name, const char* label) {
    if (name.empty()) return false;
    if (!audio_enabled || !tl::core::audio_available())
        return play_sound(name, Ogre::Vector3::ZERO);
    const SoundDefinition* definition = sound_by_name(name);
    if (definition == nullptr || definition->files.empty()) return false;
    const std::string key = lowered(name);
    // `CSoundBank::playSample` with no scene
    // node, then `CSoundManager::playSound`:
    // the same three rolls as `play_sound()`.
    tl::core::RunicRandom& rolls = tl::core::volatile_random();
    if (definition->play_chance >= 0 && definition->play_chance <= rolls.integer_between(1, 100)) {
        return false;
    }
    const std::vector<size_t> kept = tl::core::bank_recordings(*definition);
    const std::string& file = definition->files[kept[static_cast<size_t>(
        rolls.integer_between(0, static_cast<int>(kept.size()) - 1))]];
    if (!(definition->volume > 0.0f)) return false;
    const float gain =
        std::min(1.0f, definition->volume + rolls.between(0.0f, definition->volume_variation));
    tl::core::SamplePtr sample = sample_for(file);
    if (!sample) return false;
    const size_t frames = sample->frames;
    const int rate = sample->rate;
    const bool played = tl::core::audio_play_voice(std::move(sample), key, gain, 0.0f);
    // Once per definition, like `play_sound()`. A line that does not come out is
    // otherwise invisible, and a voice-over is the one sound where silence looks
    // exactly like the game being quiet on purpose.
    if (played && reported_sounds.insert(key).second) {
        TL_LOG_INFO("%s: %s -> %s (%.2f s, gain %.2f)", label, name.c_str(), file.c_str(),
                    static_cast<double>(frames) / static_cast<double>(rate > 0 ? rate : 44100),
                    static_cast<double>(gain));
    }
    return played;
}

// A Property Node as updatePathNodes sees it: TYPE 9 (Box) or 8 (Circle), its box from
// updateBoundingBox.
// RADIUS and BOX WIDTH set scale X, BOX LENGTH scale Z (else 1).
bool Renderer::Impl::path_blocker_of(const tl::core::LevelScript::NodeRecord& n,
                                     PathBlocker& b) const {
    const bool circle = n.kind == "Path Node Occupation Circle";
    if (!circle && n.kind != "Path Node Occupation Box") return false;
    b.circle = circle;
    b.centre = n.position;
    b.scale_x = n.box_width != 0.0f ? n.box_width : n.radius;
    const float scale_z = n.box_length != 0.0f ? n.box_length : 1.0f;
    const tl::core::NoSpawnRegion box =
        tl::core::property_node_bounds(n.position, n.right, n.up, n.forward, b.scale_x, scale_z);
    b.low = box.min;
    b.high = box.max;
    return true;
}

// The same for a node of the level's own file, laid before its script stands; world axes
// are the columns of node_orientation.
bool Renderer::Impl::path_blocker_of(const tl::formats::CmpNode& node, PathBlocker& b) const {
    if (node.type != tl::formats::cmp_type::kMarker) return false;
    namespace f = tl::formats::cmp_field::property_node;
    const tl::formats::CmpField* type = node.field(f::kType);
    const tl::formats::CmpField* where = node.field(f::kPosition);
    if (type == nullptr || where == nullptr || where->count() != 3) return false;
    const auto number = [&node](size_t index) {
        const tl::formats::CmpField* field = node.field(index);
        return field != nullptr && field->count() == 1 ? field->as_float() : 0.0f;
    };
    tl::core::LevelScript::NodeRecord n;
    n.node = node.id;
    n.kind = type->as_text();
    n.position = {where->as_float(0), where->as_float(1), where->as_float(2)};
    n.radius = number(f::kRadius);
    n.box_width = number(f::kBoxWidth);
    n.box_length = number(f::kBoxLength);
    const Ogre::Quaternion orientation = node_orientation(node);
    const Ogre::Vector3 x = orientation.xAxis();
    const Ogre::Vector3 y = orientation.yAxis();
    const Ogre::Vector3 z = orientation.zAxis();
    n.right = {x.x, x.y, x.z};
    n.up = {y.x, y.y, y.z};
    n.forward = {z.x, z.y, z.z};
    return path_blocker_of(n, b);
}

// PORT: a unit's position by name, from its OGRE bounding box (headless runs).
bool Renderer::Impl::unit_position(const std::string& name, Ogre::Vector3& at,
                                   std::string& found) const {
    const Ogre::Entity* unit = find_unit_entity(name, found);
    if (unit == nullptr) return false;
    at = unit->getWorldBoundingBox(true).getCenter();
    return true;
}

}  // namespace tl::render
