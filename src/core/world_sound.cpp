// The gameplay half of render/renderer_sound.cpp (core/world.h).
#include "core/unit_stats_reader.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

// `CCharacter::attack` by the player on a
// mimic holding `MIMICIDLE`, without a ranged weapon: `reactToDamage(player,
// true)` and no blow (`step_mimic_attack` brings the player there).
void World::disturb_targeted_mimic() {
    tl::core::LevelScript* owner = machine_script(mimic_targeted_machine);
    if (owner == nullptr) return;
    std::vector<tl::core::ScriptEvent> events;
    const bool woke = owner->attack_idle_mimic(mimic_targeted_node, quest_log.state(), events);
    TL_LOG_INFO("pad prop: the attack wakes the mimic %lld%s",
                static_cast<long long>(mimic_targeted_node), woke ? "" : " (not lying in wait)");
    apply_machine_events(mimic_targeted_machine, events);
}

/// The `.ANIMATION` manifest beside one character mesh, read once; empty when none ships.
const tl::core::AnimationManifest& World::manifest_for(const std::string& mesh_path) {
    const auto known = manifests_by_mesh.find(mesh_path);
    if (known != manifests_by_mesh.end()) return *known->second;
    const std::string dir = dir_of_asset(mesh_path);
    std::string stem = mesh_path;
    const size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    // One per `.ANIMATION` file, as the console keeps a `CAnimationSet` per path: a folder can hold
    // several (`media/models/skeleton`: the plain, armored, unarmed and warrior skeletons').
    const std::string key = dir + "/" + tl::core::upper_ascii(stem);
    const auto found = manifests.find(key);
    if (found != manifests.end()) {
        manifests_by_mesh.emplace(mesh_path, &found->second);
        return found->second;
    }

    tl::core::AnimationManifest manifest;

    const std::string path = archive_path_in_dir(stem + ".ANIMATION.adm", dir);
    if (!path.empty()) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (!bytes.empty()) {
            const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
            if (file.ok) manifest = tl::core::AnimationManifest::from(file);
        }
    }
    if (manifest.empty()) {
        TL_LOG_INFO("  no animation manifest beside '%s'", mesh_path.c_str());
    } else {
        TL_LOG_INFO("  animation manifest for '%s': %zu tagged animation(s)", mesh_path.c_str(),
                    manifest.size());
    }
    const tl::core::AnimationManifest& made =
        manifests.emplace(key, std::move(manifest)).first->second;
    manifests_by_mesh.emplace(mesh_path, &made);
    return made;
}

void World::music_node_input(const tl::core::ScriptEvent& event) {
    // `CMusicObjectDescriptor::CreateObject` and the descriptor's
    // `setMusicFile` / `setLoops`, on the first input the node takes.
    auto made = music_objects.find(event.node_id);
    if (made == music_objects.end()) {
        tl::core::MusicObject music;
        music.set_file(event.text);
        music.loops = event.loops;
        made = music_objects.emplace(event.node_id, std::move(music)).first;
    }
    tl::core::MusicObject& music = made->second;
    const tl::core::MusicHost host = music_host(event.node_id);
    if (event.input == tl::core::logic::kStop) {
        tl::core::music_stop(music, host);
    } else if (event.input == tl::core::logic::kPlayLevelMusic) {
        tl::core::music_play_level(music, host);
    } else {
        tl::core::music_play(music, host);
    }
}

void World::step_music_objects(float delta) {
    for (auto& [node, music] : music_objects) {
        tl::core::music_update(music, music_host(node), delta);
    }
}

void World::set_big_text(const std::string& text, float seconds) {
    // `CGameUI::setBigText`: the text, the
    // hold, and alpha 1.0.
    big_banner.text = text;
    big_banner.left = seconds;
    big_banner.alpha = 1.0f;
}

void World::set_small_text(const std::string& text, float seconds) {
    // `CGameUI::setSmallText`, the same three
    // at `+0x1990`.
    small_banner.text = text;
    small_banner.left = seconds;
    small_banner.alpha = 1.0f;
}

void World::raise_arrival_banner() {
    // CGameClient::loadLevel: with a template and entry type other than
    // 4, setBigText(LEVELNAME, 1.0) then setSmallText("Floor N" or "", 1.0).
    // DIVERGENCE: entry type 4 is not told apart; every arrival raises it.
    if (!level_has_rules) return;
    constexpr float kArrivalSeconds = 1.0f;
    set_big_text(level_name_authored, kArrivalSeconds);
    set_small_text(tl::core::level_floor_caption(level_shows_floor, level_depth), kArrivalSeconds);
    TL_LOG_INFO("level banner: '%s' / '%s', level name '%s'", big_banner.text.c_str(),
                small_banner.text.c_str(), level_display_name.c_str());
}

std::string World::preloaded_particle_file(const std::string& name) {
    if (!preloaded_particles_read) {
        preloaded_particles_read = true;
        // CParticlePreloader::CParticlePreloader(CResourceSettings*):
        // the list is the `Preload Particle` setting, else preload.dat.
        const std::string path = resolve_archive_path("media/particles/preload.dat.adm");
        const std::vector<uint8_t> bytes =
            path.empty() ? std::vector<uint8_t>{} : read_resource(path);
        if (!bytes.empty()) {
            const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
            for (const tl::formats::AdmSection& particle : file.root.children) {
                if (particle.name != "PARTICLE") continue;
                const std::string key = tl::core::upper_ascii(particle.string_or("NAME", ""));
                // The first of a name is kept, as the preloader's own lookup finds it.
                if (!key.empty()) preloaded_particles.emplace(key, particle.string_or("FILE", ""));
            }
        }
        TL_LOG_INFO("particles: %zu named in media/particles/preload.dat",
                    preloaded_particles.size());
    }
    const auto found = preloaded_particles.find(tl::core::upper_ascii(name));
    return found == preloaded_particles.end() ? std::string() : found->second;
}

void World::start_level_audio(const std::string& layout_path) {
    // No early return for a silent run: play_music() still resolves the track.

    // --- Music, from the rules file beside the layout (rules_path_for()). ---------------
    const std::string rules_path = rules_path_for(layout_path);

    std::string music;
    std::string level_name;
    // A level with no template has an empty name (`getName`'s first branch).
    level_has_rules = false;
    level_name_authored.clear();
    level_shows_floor = true;
    level_display_name.clear();
    if (!rules_path.empty()) {
        const std::vector<uint8_t> bytes = read_resource(rules_path);
        if (!bytes.empty()) {
            const tl::formats::AdmFile rules = tl::formats::parse_adm(bytes.data(), bytes.size());
            if (rules.ok) {
                music = rules.string_or("MUSIC", "");
                level_name = rules.string_or("NAME", "");
                // The HUD's level name: CLevel::getName
                // over LEVELNAME (+0x770, default Dungeon) and SHOWFLOORNUMBER (+0x76c).
                level_has_rules = true;
                level_name_authored =
                    tl::core::strip_markup(rules.string_or("LEVELNAME", "Dungeon"));
                level_shows_floor = rules.bool_or("SHOWFLOORNUMBER", true);
                level_display_name = tl::core::level_display_name(level_name_authored,
                                                                  level_shows_floor, level_depth);
            }
        }
    }

    // A main-menu level's music is in the engine: CGameClient::loadMenuLevel calls
    // CSoundManager::stopMusic() then plays `../music/Title.ogg`, looping.
    // Recognised by the rules' NAME (MainMenu*) or a layout under MAINMENUS/.
    const bool menu_level =
        level_name.compare(0, 8, "MainMenu") == 0 ||
        tl::core::upper_ascii(layout_path).find("/MAINMENUS/") != std::string::npos;
    if (menu_level) {
        // `CGameClient::loadMenuLevel` raises
        // no banner, and no HUD is drawn over a backdrop.
        level_has_rules = false;
        level_display_name.clear();
    }
    if (music.empty() && menu_level) {
        music = kMenuMusic;
        TL_LOG_INFO("main-menu level '%s': music is the engine's own '%s'", level_name.c_str(),
                    music.c_str());
    }

    // Remembered whole, so `Play Level Music` on a `Music` node has something
    // to go back to after a level has swapped in a fight track.
    level_music = music;
    music_objects.clear();
    // The rules carry no loop flag because a level's theme always does; a
    // `Music` node's own `LOOPS` is for the scripted tracks, which are a
    // different thing.
    play_music(music, /*loop=*/true, "the level authors MUSIC:");
    // The level's `Sound` nodes are its machines' (`core/sound_object.h`),
    // started by their scene's activation and played through
    // `node_sound_host()`.
}

void World::ensure_sound_index() {
    if (sound_files_loaded) return;
    sound_files_loaded = true;

    std::vector<std::string> names = archive_paths_under("media/sounds/", ".adm");
    // Then the console's own UI.DAT, after the install's (a name both define keeps the
    // install's): it adds Navigate and the rest only the console has.
    for (const std::string& path : archive_paths_under("xbox/media/sounds/", ".adm")) {
        names.push_back(path);
    }
    // `CSoundBankDataInformation::reload`
    // and `parseSoundData`: each listed
    // file's groups, in order.
    for (const std::string& path : names) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
        if (!file.ok) continue;
        sound_info.parse_file(file.root);
    }
    TL_LOG_INFO("sounds: %zu definition(s) from %zu table(s)", sound_info.size(), names.size());
}

const World::SoundDefinition* World::sound_by_name(const std::string& name) {
    if (name.empty()) return nullptr;
    ensure_sound_index();
    // `getSoundDataObject(name)`; every name is stored upper-cased.
    return sound_info.find(tl::core::upper_ascii(name));
}

const World::SoundDefinition* World::sound_by_guid(const std::string& guid) {
    if (guid.empty()) return nullptr;
    ensure_sound_index();
    // `getSoundDataObjectByGuid`. The GUID is an `INTEGER64` in the sound
    // tables and a `STRING` in the layouts that reference it, so it arrives
    // here as its decimal spelling.
    char* end = nullptr;
    const long long value = std::strtoll(guid.c_str(), &end, 10);
    if (end == guid.c_str() || *end != 0) return nullptr;
    return sound_info.find_by_guid(static_cast<int64_t>(value));
}

/// How long a `SOUND` definition plays, in seconds (its first recording): how long the
/// line stays up and the scene waits.
float World::sound_length(const std::string& name) {
    const SoundDefinition* definition = sound_by_name(name);
    if (definition == nullptr) return 0.0f;

    const std::string path = resolve_archive_path(definition->files.front());
    if (path.empty()) return 0.0f;
    const std::vector<uint8_t> bytes = read_resource(path);
    if (bytes.empty()) return 0.0f;
    return tl::formats::ogg_duration(bytes.data(), bytes.size());
}

tl::core::SamplePtr World::sample_for(const std::string& archive_path) {
    const auto found = samples.find(archive_path);
    if (found != samples.end()) return found->second;

    tl::core::SamplePtr decoded;
    const std::string resolved = resolve_archive_path(archive_path);
    if (!resolved.empty()) {
        const std::vector<uint8_t> bytes = read_resource(resolved);
        if (!bytes.empty()) {
            // A quarter of the one-shots, every voice line among them, are Ogg: the
            // extension picks the decoder.
            if (lowered(resolved).size() > 4 &&
                lowered(resolved).compare(lowered(resolved).size() - 4, 4, ".ogg") == 0) {
                decoded = tl::core::audio_decode_ogg(bytes, archive_path);
            } else {
                const tl::formats::WavSound wav =
                    tl::formats::parse_wav(bytes.data(), bytes.size());
                if (wav.ok) {
                    auto sample = std::make_shared<tl::core::Sample>();
                    sample->samples = wav.samples;
                    sample->channels = wav.channels;
                    sample->rate = wav.sample_rate;
                    sample->frames = wav.frames;
                    decoded = std::move(sample);
                } else {
                    TL_LOG_WARN("sound: could not decode '%s': %s", archive_path.c_str(),
                                wav.error.c_str());
                }
            }
        }
    }
    // A null is cached too: a file the archive has not got must not be looked
    // for again every time a paw lands.
    samples.emplace(archive_path, decoded);
    return decoded;
}

const std::string& World::pick_recording(const SoundDefinition& definition) {
    // PORT: the sound system's choice of recording.
    if (definition.files.size() < 2) return definition.files.front();
    std::uniform_int_distribution<size_t> any(0, definition.files.size() - 1);
    return definition.files[any(recording_picker)];
}

// `CQuestDialog::playDialogSound`: the
// dialog's `SOUND` in slot 0x25 of a bank of its own, played with no position.
// `CQuestDialogMenu::setNPC` stops the
// outgoing dialog's recording before it plays the new one.
void World::play_dialog_sound(const std::string& name) {
    stop_dialog_sound();
    if (name.empty()) return;
    if (play_sound_2d(name, "dialog")) dialog_sound_key = lowered(name);
}

// `CQuestDialog::stopDialogSound` ->
// `CSoundBank::stop(0x25)`.
void World::stop_dialog_sound() {
    if (dialog_sound_key.empty()) return;
    tl::core::audio_stop_voice(dialog_sound_key);
    dialog_sound_key.clear();
}

const char* World::voice_or_none(tl::core::SoundSlot slot) const {
    const std::string& name = player_stats.sounds[slot];
    return name.empty() ? "-" : name.c_str();
}

bool World::play_ui(tl::core::InterfaceSound sound) {
    const char* name = tl::core::interface_sound_name(sound);
    // Non-positional, like every other interface sound: `CGameUI` and the menus
    // pass a null scene node to `playSample`, so the listener's distance never
    // enters into it.
    return play_sound_2d(name, "ui");
}

bool World::play_voice(tl::core::SoundSlot slot, float delay, float volume) {
    const std::string& name = player_stats.sounds[slot];
    if (name.empty()) {
        // Logged once: six voice slots are unauthored and VOICESPELLLEARNED_SOUND is
        // misspelled in the data (core/sound_bank.h).
        const int index = static_cast<int>(slot);
        if (reported_voices.insert(index).second) {
            TL_LOG_INFO("voice: the class authors no '%s'; nothing to say",
                        tl::core::kSoundSlotKeys[static_cast<size_t>(index)]);
        }
        return false;
    }
    return queue_global_sample(slot, name, volume, delay);
}

// CSoundBank::queueGlobalSample(slot, volume, delay). The
// gate on the sound system (manager +0x375/+0x374) is the mixer's: a silent run still
// queues.
bool World::queue_global_sample(tl::core::SoundSlot slot, const std::string& name, float volume,
                                float delay) {
    if (name.empty()) return false;
    const SoundDefinition* definition = sound_by_name(name);
    if (definition == nullptr) return false;
    tl::core::RunicRandom& rolls = tl::core::volatile_random();
    // The slot's chance (X360 +0x7c, Linux +0x90): none (-1) always plays;
    // else a 1..100 roll must come out below it.
    if (definition->play_chance >= 0 && definition->play_chance <= rolls.integer_between(1, 100)) {
        return false;
    }
    // A volume of 0 is the slot's own (X360 +0x6c, Linux +0x78); any other is
    // scaled by the bank's (X360 +0x90, Linux +0xac; 1).
    const float level = volume == 0.0f ? definition->volume : volume * 1.0f;
    // No samples, nothing; else one drawn on the volatile stream,
    // before `queueSound` decides whether to keep it.
    if (definition->files.empty()) return false;
    // Among the recordings the bank kept (`bank_recordings`).
    const std::vector<size_t> kept = tl::core::bank_recordings(*definition);
    const std::string& file = definition->files[kept[static_cast<size_t>(
        rolls.integer_between(0, static_cast<int>(kept.size()) - 1))]];
    tl::core::VoiceQueue::Entry entry;
    entry.slot = static_cast<int>(slot);
    entry.sample = name;
    entry.file = file;
    entry.volume = level;
    entry.delay = delay;
    const bool playing = tl::core::audio_voice_playing(voice_queue.channel());
    if (!voice_queue.queue(std::move(entry), playing)) {
        TL_LOG_INFO("voice: %s not queued (%s)", name.c_str(),
                    voice_queue.playing_slot() == static_cast<int>(slot) && playing
                        ? "it is the one sounding"
                        : "already queued");
        return false;
    }
    return true;
}

// `CSoundManager::update(listener, position, delta)`,
// its queue half: one line at a time, each once
// the one before has finished and its own delay has run.
void World::step_voices(float delta) {
    const bool playing = tl::core::audio_voice_playing(voice_queue.channel());
    tl::core::VoiceQueue::Entry entry;
    if (!voice_queue.update(delta, playing, entry)) return;
    // stopSound on the old channel, then playSound(sample, no node, volume),
    // volume variation capped at 1.
    if (!voice_queue.channel().empty()) tl::core::audio_stop_voice(voice_queue.channel());
    voice_queue.set_channel(std::string());
    const SoundDefinition* definition = sound_by_name(entry.sample);
    if (definition == nullptr || !(entry.volume > 0.0f)) return;
    tl::core::RunicRandom& rolls = tl::core::volatile_random();
    const float gain =
        std::min(1.0f, entry.volume + rolls.between(0.0f, definition->volume_variation));
    const std::string key = "voice:" + lowered(entry.sample);
    bool played = false;
    if (audio_enabled && tl::core::audio_available()) {
        tl::core::SamplePtr sample = sample_for(entry.file);
        if (sample) played = tl::core::audio_play_voice(std::move(sample), key, gain, 0.0f);
    }
    if (played) voice_queue.set_channel(key);
    TL_LOG_INFO("voice: %s -> %s (gain %.2f)%s", entry.sample.c_str(), entry.file.c_str(),
                static_cast<double>(gain), played ? "" : ", silent");
}

// CLevel::incrementMapPassability(AxisAlignedBox, float)
// on, decrementMapPassability off: radius 0 for a box,
// getScaleX() for a circle. The route's grid and NavGrid::blocked are refreshed.
size_t World::set_path_blocker_cells(const PathBlocker& b, bool on) {
    // `+0x50`/`+0x54` (the grid's size): no grid, no cells.
    if (!passability.ready()) return 0;
    const float radius = b.circle ? b.scale_x : 0.0f;
    if (on) {
        passability.increment_map_passability(b.low, b.high, radius);
    } else {
        passability.decrement_map_passability(b.low, b.high, radius);
    }
    // The record first, while the route's copy still has the cells as they were.
    const size_t changed = nav.mark_blocker(b.low, b.high, on);
    nav.refresh(passability, b.low, b.high);
    return changed;
}

// Every blocker that is on comes off the map, its record with it: a level reset
// lays them again from the script (`apply_path_blockers`).
void World::lift_path_blockers() {
    for (const int64_t id : level_path_blockers_on) {
        const tl::formats::CmpNode* node = script_layout.node_by_id(id);
        PathBlocker b;
        if (node != nullptr && path_blocker_of(*node, b)) set_path_blocker_cells(b, false);
    }
    level_path_blockers_on.clear();
    for (auto& entry : machine_path_blockers) {
        if (!entry.second.applied) continue;
        set_path_blocker_cells(entry.second.box, false);
        entry.second.applied = false;
    }
    nav.clear_blockers();
}

void World::register_door_avoidance() {
    // CTriggerUnit::setActiveInLevel(true): first time
    // the state set to the first, shown, CItem::snapToGround and
    // CBaseUnit::addToAvoidanceMap, whose INTERACTABLE
    // branch probes the trigger's collision into the map grid. INVENTED: no snap for a
    // layout's prop.
    const std::vector<int64_t> nodes = std::move(door_trigger_nodes);
    door_trigger_nodes.clear();
    if (!passability.ready()) return;

    size_t added = 0;
    for (const int64_t node_id : nodes) {
        // Each placed copy has its own group (instance_key()); a group probed
        // twice would be marked twice, which opening it (a single decrement)
        // never takes back.
        if (door_avoidance_added.count(node_id) != 0) continue;
        if (!collision.has_group(node_id)) continue;
        tl::core::Vec3 low, high;
        if (!collision.group_bounds(node_id, low, high)) continue;
        passability.increment_map_passability_collision(low, high, collision, node_id);
        door_avoidance_added.insert(node_id);
        ++added;
    }
    if (added > 0) {
        TL_LOG_INFO("  passability: %zu door/chest/trigger collision model(s) mark the map grid",
                    added);
    }
}

/// Load the authored numbers (WALKINGSPEED, RUNNINGSPEED and the rest) of every unit this
/// layout places, indexed by NAME from media/units/monsters/ once.
void World::load_unit_stats(const tl::formats::CmpScene& layout, const std::set<int64_t>& gated) {
    unit_book.clear();

    std::set<std::string> wanted;
    for (const tl::formats::CmpNode& node : layout.nodes) {
        if (node.type != tl::formats::cmp_type::kMonster) continue;
        if (gated.count(node.id) != 0) continue;
        const tl::formats::CmpField* field = node.field(tl::formats::cmp_field::monster::kMonster);
        if (field == nullptr) continue;
        const std::string unit = field->as_text();
        if (!unit.empty()) wanted.insert(lowered(unit));
    }

    // Before the early return: a level whose monsters all come from a Unit Spawner places
    // none, and still needs the unit book.
    script.set_units(&unit_book);
    wire_script(script);
    if (wanted.empty()) return;

    ensure_monster_index();

    // Each unit is read over its `BASEFILE` chain (`read_unit_file`).
    size_t known = 0, missing = 0;
    for (const std::string& unit : wanted) {
        if (index_unit(unit)) {
            ++known;
        } else {
            ++missing;
        }
    }

    script.set_units(&unit_book);
    wire_script(script);
    TL_LOG_INFO("  units: %zu of %zu placed unit(s) have their own .DAT%s", known, wanted.size(),
                missing > 0 ? " (the rest walk at the player's speed)" : "");
}

// CEquipment::calculateCombatStats: the right hand's
// weapon swings with its own attack (class, RANGE, STRIKERANGE, SPEED / 100,
// AI_ATTACKCOOLDOWN), which CCharacter::selectAttack takes over the unit's ATTACK.
tl::core::UnitStats::WeaponAttack World::weapon_attack_of(const std::string& item_name,
                                                          tl::core::Hand hand) {
    if (item_name.empty()) return {};
    ensure_item_index();
    const tl::core::ItemStats* item = items.find(item_name);
    if (item == nullptr) return {};
    load_unit_types();
    return tl::core::weapon_attack_of(*item, hand, unit_types);
}

// CCharacter::createDefaultEquipment:
// loadAndEquipSpawnClass for SPAWNRIGHTHAND (0) then
// SPAWNLEFTHAND (1): one rollSpawnClass at level -1, flags 0x10, seed 0xffffffff (g_Rand
// as it stands), the first unit equipped.
void World::roll_unit_equipment(const std::string& unit,
                                tl::core::LevelScript::UnitCreation& made) {
    const tl::core::UnitStats stats = stats_for_unit(unit);
    if (!stats.spawn_right_hand.empty()) {
        const std::string rolled =
            resolve_spawn_resource("Spawn Class", stats.spawn_right_hand,
                                   /*depth=*/0, /*level=*/-1, /*flags=*/0x10);
        if (!rolled.empty()) {
            made.right_hand = rolled;
            made.weapon_attack = weapon_attack_of(rolled);
        }
    }
    if (!stats.spawn_left_hand.empty()) {
        const std::string rolled =
            resolve_spawn_resource("Spawn Class", stats.spawn_left_hand,
                                   /*depth=*/0, /*level=*/-1, /*flags=*/0x10);
        if (!rolled.empty()) {
            made.left_hand = rolled;
            made.left_weapon_attack = weapon_attack_of(rolled, tl::core::Hand::Left);
        }
    }
}

tl::core::LevelScript::UnitEquipment World::unit_equipment_hooks() {
    ensure_item_index();
    load_unit_types();
    tl::core::LevelScript::UnitEquipment hooks;
    hooks.items = &items;
    // `CResourceManager::createEquipment(name, false)`:
    // `createItem(name, false)`.
    hooks.create = [this](const tl::core::ItemStats& data, bool random_enchant) {
        return create_item(data, false, random_enchant);
    };
    // `loadAndEquipSpawnClass`: level -1, seed -1, flags
    // 0x10, on `g_Rand`.
    hooks.roll = [this](const std::string& spawn_class, std::vector<std::string>& units,
                        std::vector<bool>& enchant) {
        SpawnTables tables;
        build_spawn_context(tables, /*items_by_type=*/false);
        const std::optional<tl::core::SpawnClass> loaded =
            load_spawn_class(spawn_class, tables.context);
        if (!loaded) return;
        loaded->roll_spawn_class(units, &enchant, nullptr, nullptr, /*level=*/-1, 0xffffffffu,
                                 /*level_bonus=*/0, /*rarity_override=*/-1, /*magic_find=*/0,
                                 /*flags=*/0x10, tl::core::game_random(), tables.context);
    };
    hooks.enchant = [this](tl::core::ItemStats& item) {
        tl::core::enchant_item(item, true, item_roll_context());
    };
    hooks.weapon_attack = [this](const std::string& item, tl::core::Hand hand) {
        return weapon_attack_of(item, hand);
    };
    return hooks;
}

bool World::index_unit(const std::string& name) {
    const std::string unit = lowered(name);
    if (unit_book.find(unit) != nullptr) return true;

    ensure_monster_index();
    {
        const auto found = monster_files.find(unit);
        if (found == monster_files.end()) return false;

        // The unit's own data, merged over its `BASEFILE` chain
        // (`CUnitResourceList::parseChildFile`).
        const tl::formats::AdmFile file = read_unit_file(found->second);
        tl::core::UnitStatsSources sources;
        sources.unit_types = &unit_types;
        ensure_effect_data();
        sources.effect_types = &effect_types;
        sources.skill = [this](const std::string& skill) -> const tl::core::AttackSkill* {
            ensure_skill_index();
            const auto entry = skills.find(skill);
            return entry == skills.end() ? nullptr : &entry->second;
        };
        sources.item_unittype = [this](const std::string& item) -> std::optional<std::string> {
            ensure_item_index();
            const tl::core::ItemStats* found_item = items.find(item);
            if (found_item == nullptr) return std::nullopt;
            return found_item->unittype;
        };
        sources.has_animation = [this](const std::string& dir, const std::string& animation,
                                       bool armed) {
            return !archive_animation_in_dir(dir, animation, armed, 0).empty();
        };
        sources.weapon_attack = [this](const std::string& item, tl::core::Hand hand) {
            return weapon_attack_of(item, hand);
        };
        tl::core::UnitStats stats = tl::core::read_unit_stats(file, unit, sources);
        // PORT: the player's pet is the renderer's, not a level machine's
        // character, so `setMaster`'s side for it (the
        // master's alignment, `GOOD`) is carried on its unit data: a `PET`
        // with no `ALIGNMENT` of its own stands on the player's side.
        if (file.ok && file.root.find("ALIGNMENT") == nullptr && lowered(stats.unittype) == "pet") {
            stats.alignment = tl::core::UnitStats::Alignment::Friendly;
        }
        unit_book.add(unit, stats);
        std::string swings;
        for (const tl::core::AttackSkill& attack : stats.attacks) {
            swings += swings.empty() ? ", attacks with " : "/";
            swings += attack.animation;
        }
        TL_LOG_INFO(
            "    %-24s walk %.2f run %.2f, radius %.2f, sight %.1f, reach %.2f, "
            "hp %.0f-%.0f%%, dmg %.0f-%.0f%%, %s%s%s%s",
            unit.c_str(), static_cast<double>(stats.movement.walking_speed),
            static_cast<double>(stats.movement.running_speed), static_cast<double>(stats.radius),
            static_cast<double>(stats.sight_radius), static_cast<double>(stats.reach),
            static_cast<double>(stats.min_health), static_cast<double>(stats.max_health),
            static_cast<double>(stats.min_damage), static_cast<double>(stats.max_damage),
            stats.hostile() ? "hostile" : (stats.friendly() ? "friendly" : "neutral"),
            stats.armed ? ", armed" : "", stats.collideable ? "" : ", walk through",
            swings.c_str());
    }
    return true;
}

int64_t World::instance_key(int64_t node, uint32_t scene, int64_t hierarchy) {
    if (scene == 0 && hierarchy == 0) return node;
    uint64_t k = static_cast<uint64_t>(hierarchy) * 0x9e3779b97f4a7c15ull;
    k ^= (static_cast<uint64_t>(scene) + 1u) * 0xc2b2ae3d27d4eb4full;
    return static_cast<int64_t>(static_cast<uint64_t>(node) ^ k);
}

int64_t World::instance_key(int64_t node, const NodeMap& nodes) const {
    for (const NestedScript& machine : nested_scripts) {
        if (&machine.nodes == &nodes) return instance_key(node, machine.scene, machine.hierarchy);
    }
    return node;
}

int64_t World::instance_key(int64_t node, int machine) const {
    if (machine >= 0 && static_cast<size_t>(machine) < nested_scripts.size()) {
        const NestedScript& m = nested_scripts[static_cast<size_t>(machine)];
        return instance_key(node, m.scene, m.hierarchy);
    }
    return node;
}

void World::clear_level_up_particle() { destroy_particle_holders(level_up_particle_nodes); }

// CPropertyNode::updatePathNodes, once its cells are in:
// killOrMoveBaseUnits, machine by machine, over the
// node's own box (+0x120).
void World::clear_path_blocker_area(const PathBlocker& b) {
    tl::core::LevelScript::OccupationArea area;
    area.centre = b.centre;
    area.min = b.low;
    area.max = b.high;
    area.circle = b.circle;
    area.scale_x = b.scale_x;
    // A copy of the list: the events can link a layout, which adds a machine.
    const std::vector<MachineView> views = machines();
    for (const MachineView& view : views) {
        std::vector<tl::core::ScriptEvent> events;
        std::vector<int64_t> killed;
        std::vector<std::pair<int64_t, std::string>> names;
        view.script->set_defer_death_sample(true);
        view.script->kill_or_move_base_units(area, quest_log.state(), events, killed);
        view.script->set_defer_death_sample(false);
        for (const int64_t id : killed) names.emplace_back(id, view.script->unit_of(id));
        if (view.nodes != nullptr && !events.empty()) apply_script_events(events, *view.nodes);
        for (const auto& dead : names) {
            TL_LOG_INFO("script: %s is caught in a pathing blocker and dies", dead.second.c_str());
            kill_unit(dead.first, dead.second, /*fall=*/false, tl::core::LevelScript::kNoAttacker);
        }
    }
}

void World::apply_path_blocker(int64_t node_id, bool on, const NodeMap* nodes, bool live) {
    if (!nav.ready()) return;
    const tl::formats::CmpNode* node = script_layout.node_by_id(node_id);
    if (node == nullptr) {
        // A linked layout's, or a placed room's.
        const auto found =
            machine_path_blockers.find(nodes != nullptr ? instance_key(node_id, *nodes) : node_id);
        if (found == machine_path_blockers.end()) return;
        MachinePathBlocker& b = found->second;
        // `+0x150`: a second `Enable` changes nothing.
        if (b.applied == on) return;
        b.applied = on;
        const size_t changed = set_path_blocker_cells(b.box, on);
        TL_LOG_INFO("script: pathing blocker %lld %s, %zu cell(s) changed",
                    static_cast<long long>(node_id), on ? "on" : "off", changed);
        if (on && live) clear_path_blocker_area(b.box);
        return;
    }
    PathBlocker b;
    if (!path_blocker_of(*node, b)) return;
    // `+0x150`, as for a linked layout's: a timeline's `ENABLED` key sets the
    // flag every frame it plays, and only a change moves cells.
    const bool applied = level_path_blockers_on.count(node_id) != 0;
    if (applied == on) return;
    if (on) {
        level_path_blockers_on.insert(node_id);
    } else {
        level_path_blockers_on.erase(node_id);
    }
    const size_t changed = set_path_blocker_cells(b, on);
    TL_LOG_INFO("script: pathing blocker '%s' %s, %zu cell(s) changed", node->name.c_str(),
                on ? "on" : "off", changed);
    if (on && live) clear_path_blocker_area(b);
}

// updatePathNodes for a linked layout's nodes, placed by their machine's frame
// (NodeRecord::right and the rest are world).
void World::apply_machine_path_blockers() {
    machine_path_blockers.clear();
    if (!nav.ready()) return;
    size_t blockers = 0;
    for (const MachineView& view : machines()) {
        if (view.script == &script) continue;
        for (const tl::core::LevelScript::NodeRecord& n :
             view.script->nodes_of_type(tl::formats::cmp_type::kMarker)) {
            MachinePathBlocker b;
            if (!path_blocker_of(n, b.box)) continue;
            b.applied = n.enabled;
            machine_path_blockers[instance_key(n.node, view.scene, view.hierarchy)] = b;
            TL_LOG_INFO("  pathing: blocker %lld, box (%.2f, %.2f)-(%.2f, %.2f)%s",
                        static_cast<long long>(n.node), static_cast<double>(b.box.low.x),
                        static_cast<double>(b.box.low.z), static_cast<double>(b.box.high.x),
                        static_cast<double>(b.box.high.z), n.enabled ? "" : " (disabled)");
            if (!n.enabled) continue;
            if (!path_blockers_laid) set_path_blocker_cells(b.box, true);
            ++blockers;
        }
    }
    if (blockers > 0) {
        TL_LOG_INFO(
            "  pathing: %zu blocker(s) of linked layouts on, %zu of %zu walkable cell(s) covered",
            blockers, nav.blocked_count(), nav.walkable_count());
    }
}

void World::apply_path_blockers(const tl::formats::CmpScene& layout,
                                const std::set<int64_t>& gated) {
    if (!nav.ready()) return;
    // Whatever is still on comes off first, so a cell is never counted twice.
    for (const int64_t id : level_path_blockers_on) {
        const tl::formats::CmpNode* node = layout.node_by_id(id);
        PathBlocker b;
        if (node != nullptr && path_blocker_of(*node, b)) set_path_blocker_cells(b, false);
    }
    level_path_blockers_on.clear();
    namespace f = tl::formats::cmp_field::property_node;
    size_t blockers = 0;
    for (const tl::formats::CmpNode& node : layout.nodes) {
        if (node.type != tl::formats::cmp_type::kMarker) continue;
        if (gated.count(node.id) != 0) continue;
        const tl::formats::CmpField* type = node.field(f::kType);
        if (type == nullptr) continue;
        const std::string kind = type->as_text();
        if (kind != "Path Node Occupation Box" && kind != "Path Node Occupation Circle") {
            continue;
        }
        // `ENABLED` defaults to true: 717 of the archive's 1,366 property nodes
        // author it and both of the town's blockers say so outright.
        const tl::formats::CmpField* enabled = node.field(f::kEnabled);
        if (enabled != nullptr && enabled->count() == 1 && !enabled->as_bool()) continue;
        apply_path_blocker(node.id, true);
        ++blockers;
    }
    if (blockers > 0) {
        TL_LOG_INFO("  pathing: %zu blocker(s) on, %zu of %zu walkable cell(s) covered", blockers,
                    nav.blocked_count(), nav.walkable_count());
    }
}

// PORT: a drawn unit by (part of) its name, for the headless switches.
Ogre::Entity* World::find_unit_entity(const std::string& name, std::string& found) const {
    const std::string want = lowered(name);
    Ogre::Entity* best = nullptr;
    const std::string* best_name = nullptr;
    bool best_exact = false;
    // The first unit whose name contains `name`, unless one is called exactly
    // that ("stash" is the Stash, not the Shared Stash placed before it).
    auto consider = [&](Ogre::Entity* entity, const std::string& unit) {
        if (best_exact || entity == nullptr) return;
        const std::string have = lowered(unit);
        if (have.find(want) == std::string::npos) return;
        const bool exact = have == want;
        if (best != nullptr && !exact) return;
        best = entity;
        best_name = &unit;
        best_exact = exact;
    };
    for (const auto& entry : scene_units) consider(entry.second.entity, entry.first);
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) consider(made.entity, made.unit);
    }
    if (best == nullptr) return nullptr;
    found = *best_name;
    return best;
}

}  // namespace tl::core
