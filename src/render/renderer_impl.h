// The shared declarations of the renderer's implementation files (renderer*.cpp).
#pragma once

#include "render/renderer.h"

#include "core/layout_helpers.h"
#include "core/layout_maths.h"
#include "core/saved_orientation.h"
#include "core/world.h"

#include "core/attack_button.h"
#include "core/camera_shake.h"
#include "core/character.h"
#include "core/character_sheet.h"
#include "core/cinematics.h"
#include "core/combine.h"
#include "core/hotbar.h"
#include "core/howto_menu.h"
#include "core/inventory.h"
#include "core/inventory_screen.h"
#include "core/item_drop.h"
#include "core/item_use.h"
#include "core/merchant.h"
#include "core/model_animation.h"
#include "core/stats_page.h"
#include "render/automap.h"
#include "render/cinematic_menu.h"
#include "render/death_screen.h"
#include "render/display_gamma.h"
#include "render/game_hud.h"
#include "render/gl_thread.h"
#include "render/gpu_skinning.h"
#include "render/gpu_timer.h"
#include "render/hud.h"
#include "render/inventory_menu.h"
#include "render/layout_screen.h"
#include "render/menu.h"
#include "render/particle_systems.h"
#include "render/particle_universe.h"
#include "render/particle_wrappers.h"
#include "render/quest_dialog.h"
#include "render/render_system_choice.h"
#include "render/renderer_archive.h"
#include "render/touch_camera.h"
#include "render/touch_overlay.h"
#include "render/unit_opacity.h"

#include "core/active_range.h"
#include "core/affix_book.h"
#include "core/animation_manifest.h"
#include "core/asset_path.h"
#include "core/audio.h"
#include "core/average_fps.h"
#include "core/breakable.h"
#include "core/character_effects.h"
#include "core/collision.h"
#include "core/context_tips.h"
#include "core/death.h"
#include "core/dungeon.h"
#include "core/enchanting.h"
#include "core/fishing.h"
#include "core/game_speed.h"
#include "core/game_ui.h"
#include "core/gamepad.h"
#include "core/input.h"
#include "core/item_effects.h"
#include "core/item_label.h"
#include "core/item_name.h"
#include "core/item_roll.h"
#include "core/item_save.h"
#include "core/item_sets.h"
#include "core/light_override.h"
#include "core/log.h"
#include "core/missile.h"
#include "core/music_object.h"
#include "core/navigation.h"
#include "core/no_spawn_region.h"
#include "core/passability.h"
#include "core/pet.h"
#include "core/pet_commands.h"
#include "core/platform.h"
#include "core/player.h"
#include "core/populate.h"
#include "core/quest.h"
#include "core/random_groups.h"
#include "core/retirement.h"
#include "core/script.h"
#include "core/settings_menu.h"
#include "core/skill_animation.h"
#include "core/skill_cast.h"
#include "core/skill_text.h"
#include "core/skills.h"
#include "core/skills_tab.h"
#include "core/sound_data.h"
#include "core/spawn_class.h"
#include "core/spell_remove.h"
#include "core/stats.h"
#include "core/targeting.h"
#include "core/text.h"
#include "core/text_event.h"
#include "core/town_portal.h"
#include "core/tray_moves.h"
#include "core/tray_press.h"
#include "core/unit_observers.h"
#include "core/unit_resource.h"
#include "core/unit_themes.h"
#include "core/unit_types.h"
#include "core/wardrobe.h"
#include "core/warp.h"
#include "core/waypoints.h"
#include "core/weapon_procs.h"
#include "core/weapon_trail.h"
#include "core/weapons.h"
#include "formats/adm.h"
#include "formats/cmp.h"
#include "formats/ogg.h"
#include "formats/wav.h"

#include <array>
#include <fstream>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreBillboard.h>
#include <OgreBillboardSet.h>
#include <OgreBone.h>
#include <OgreCamera.h>
#include <OgreCodec.h>
#include <OgreControllerManager.h>
#include <OgreDataStream.h>
#include <OgreEntity.h>
#include <OgreFrameListener.h>
#include <OgreFrustum.h>
#include <OgreGpuProgramManager.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreImage.h>
#include <OgreKeyFrame.h>
#include <OgreLogManager.h>
#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgreMatrix3.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreMeshSerializer.h>
#include <OgrePass.h>
#include <OgreRectangle2D.h>
#include <OgreRenderQueueListener.h>
#include <OgreRenderTargetListener.h>
#include <OgreRenderTexture.h>
#include <OgreRenderWindow.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>
#include <OgreSkeletonManager.h>
#include <OgreStaticGeometry.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreTagPoint.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>
#include <OgreTimer.h>
#include <OgreViewport.h>

#include <OgreShaderGenerator.h>

// Static plugin registration: no plugins.cfg and no dlopen (the Switch has no dynamic
// loader). Only the render system differs: the console's OGRE has GLES2 alone, unless
// built with TL_SWITCH_GL3PLUS (render/render_system_choice.h).
#if defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_ANDROID) || defined(TL_PLATFORM_IOS)
#include <OgreGLES2Plugin.h>
#if defined(TL_SWITCH_GL3PLUS)
#include <OgreGL3PlusPlugin.h>
#endif
#else
#include <OgreGL3PlusPlugin.h>
#include <OgreGLPlugin.h>
#endif
#include <OgreSTBICodec.h>

#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef TL_OGRE_MEDIA_DIR
#define TL_OGRE_MEDIA_DIR ""
#endif

namespace tl::render {
namespace renderer_detail {}
using namespace renderer_detail;
namespace ogre = tl::core::ogre;

// PORT: the game's maths (core/ogre_types.h) and OGRE's, one to the other.
inline Ogre::Vector3 to_ogre(const ogre::Vector3& v) { return Ogre::Vector3(v.x, v.y, v.z); }
inline Ogre::Quaternion to_ogre(const ogre::Quaternion& q) {
    return Ogre::Quaternion(q.w, q.x, q.y, q.z);
}
inline ogre::Vector3 from_ogre(const Ogre::Vector3& v) { return ogre::Vector3(v.x, v.y, v.z); }
inline ogre::Quaternion from_ogre(const Ogre::Quaternion& q) {
    return ogre::Quaternion(q.w, q.x, q.y, q.z);
}
// PORT: a save record's orientation (core/saved_orientation.h) in OGRE's maths.
inline void save_orientation(const Ogre::Quaternion& turn, std::array<uint8_t, 64>& out) {
    tl::core::save_orientation(from_ogre(turn), out);
}
inline Ogre::Quaternion saved_orientation(const std::array<uint8_t, 64>& in) {
    return to_ogre(tl::core::saved_orientation(in));
}
namespace renderer_detail {
class RenderBehindMaterials;  // the render-behind outline's materials, below

using tl::core::kDefaultRimLight;

/// The game's own data, kept out of the default group so its scripts are not parsed
/// (mount_resources()).
constexpr const char* kGameGroup = "Torchlight";

/// Where place_mesh()'s increasing paint order for opaque layout meshes stops:
/// every later one shares this priority.
constexpr Ogre::ushort kLayoutDrawOrderCeiling = 90;

/// Path tests via stat() rather than std::filesystem: console paths carry a device prefix
/// ("sdmc:/..."), which std::filesystem::path is not specified for.
inline bool is_dir(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool is_file(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// Join two path components with '/', which every platform here accepts.
inline std::string join_path(const std::string& base, const std::string& leaf) {
    if (base.empty()) return leaf;
    const char back = base.back();
    return (back == '/' || back == '\\') ? base + leaf : base + "/" + leaf;
}

/// Forwards OGRE's log through ours, so the console's file sink captures it too.
class OgreLogBridge : public Ogre::LogListener {
public:
    void messageLogged(const Ogre::String& message, Ogre::LogMessageLevel lml, bool /*maskDebug*/,
                       const Ogre::String& /*logName*/, bool& /*skipThisMessage*/) override {
        // OGRE indexes resources by basename and Torchlight reuses filenames across
        // directories: these are expected, and would bury the real errors.
        if (message.find("because it already exists in resource group") != Ogre::String::npos) {
            ++duplicate_names;
            return;
        }

        tl::core::LogLevel level = tl::core::LogLevel::Info;
        switch (lml) {
            case Ogre::LML_TRIVIAL: level = tl::core::LogLevel::Debug; break;
            case Ogre::LML_NORMAL: level = tl::core::LogLevel::Info; break;
            case Ogre::LML_WARNING: level = tl::core::LogLevel::Warning; break;
            case Ogre::LML_CRITICAL: level = tl::core::LogLevel::Error; break;
        }
        tl::core::log_write(level, "OGRE", 0, message);
    }

    size_t duplicate_names = 0;
};

/// Generates a shader technique whenever a material has none for the RTSS scheme: the
/// game's materials are all fixed-function, which neither GL3Plus nor GLES2 has. Mirrors
/// OgreBites::SGTechniqueResolverListener (the Bites component is not built).
class ShaderResolver : public Ogre::MaterialManager::Listener {
public:
    ShaderResolver(Ogre::RTShader::ShaderGenerator* generator, GpuSkinning* skinning)
        : generator_(generator), skinning_(skinning) {}

    Ogre::Technique* handleSchemeNotFound(unsigned short /*schemeIndex*/,
                                          const Ogre::String& scheme_name, Ogre::Material* original,
                                          unsigned short /*lodIndex*/,
                                          const Ogre::Renderable* /*rend*/) override {
        if (!generator_->hasRenderState(scheme_name)) return nullptr;

        if (!generator_->createShaderBasedTechnique(*original, Ogre::MSN_DEFAULT, scheme_name)) {
            // Never silently: a null here draws the fixed-function technique, which stops
            // the frame, so it gets the blank white's. `createShaderBasedTechnique` refuses
            // when a redeclared material left an entry built from another source technique
            // (parse_material_script()).
            TL_LOG_WARN("no shader technique for material '%s' (group '%s'); it draws blank white",
                        original->getName().c_str(), original->getGroup().c_str());
            return blank_technique(scheme_name, original);
        }
        if (skinning_ != nullptr) skinning_->regenerating(*original);
        generator_->validateMaterial(scheme_name, *original);

        for (auto* technique : original->getTechniques()) {
            if (technique->getSchemeName() == scheme_name) return technique;
        }
        // The generator answered for the name and put nothing on this object:
        // its entry is another `Material` of the same name.
        TL_LOG_WARN(
            "the shader technique of material '%s' (group '%s') is another object's; it "
            "draws blank white",
            original->getName().c_str(), original->getGroup().c_str());
        return blank_technique(scheme_name, original);
    }

    bool afterIlluminationPassesCreated(Ogre::Technique* technique) override {
        if (!generator_->hasRenderState(technique->getSchemeName())) return false;
        Ogre::Material* material = technique->getParent();
        generator_->validateMaterialIlluminationPasses(technique->getSchemeName(),
                                                       material->getName(), material->getGroup());
        return true;
    }

    bool beforeIlluminationPassesCleared(Ogre::Technique* technique) override {
        if (!generator_->hasRenderState(technique->getSchemeName())) return false;
        Ogre::Material* material = technique->getParent();
        generator_->invalidateMaterialIlluminationPasses(technique->getSchemeName(),
                                                         material->getName(), material->getGroup());
        return true;
    }

private:
    /// OGRE's unlit blank white, with shaders: what a material with no shader technique
    /// draws with (its fixed-function one would throw).
    Ogre::Technique* blank_technique(const Ogre::String& scheme_name,
                                     const Ogre::Material* original) {
        const Ogre::MaterialPtr blank =
            Ogre::MaterialManager::getSingleton().getDefaultMaterial(false);
        if (!blank || blank.get() == original) return nullptr;
        blank->load();
        for (int attempt = 0; attempt < 2; ++attempt) {
            for (auto* technique : blank->getTechniques()) {
                if (technique->getSchemeName() == scheme_name) return technique;
            }
            if (!generator_->createShaderBasedTechnique(*blank, Ogre::MSN_DEFAULT, scheme_name))
                break;
            generator_->validateMaterial(scheme_name, *blank);
        }
        return nullptr;
    }

    Ogre::RTShader::ShaderGenerator* generator_;
    GpuSkinning* skinning_;
};

}  // namespace renderer_detail

using tl::core::MeshPlacement;

using tl::core::kUnitIdleAnimation;

using tl::core::ParticleStart;

using tl::core::kMissileParticleStopSeconds;

struct Renderer::Impl : tl::core::World {
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool add_map_portal(const ogre::Vector3& at, const std::string& warp, bool enabled) override {
        const auto r = add_map_portal(to_ogre(at), warp, enabled);
        return r;
    }
    using World::place_breakable;
    // PORT: `World::place_breakable` (core/world.h) in OGRE's maths, for the renderer's callers.
    bool place_breakable(const std::string& unit, const Ogre::Vector3& at,
                         const Ogre::Quaternion& facing, int64_t id, const Ogre::Vector3* settle,
                         const float* drawn_scale = nullptr) {
        const ogre::Vector3 settle_c = settle != nullptr ? from_ogre(*settle) : ogre::Vector3::ZERO;
        return World::place_breakable(unit, from_ogre(at), from_ogre(facing), id,
                                      settle != nullptr ? &settle_c : nullptr, drawn_scale);
    }
    using World::cast_prop_trap;
    // PORT: `World::cast_prop_trap` (core/world.h) in OGRE's maths, for the renderer's callers.
    void cast_prop_trap(const std::string& unit, const std::string& name, uint32_t level,
                        const Ogre::Vector3& skill_at, const Ogre::Quaternion& facing,
                        bool by_player) {
        World::cast_prop_trap(unit, name, level, from_ogre(skill_at), from_ogre(facing), by_player);
    }
    using World::drop_item;
    // PORT: `World::drop_item` (core/world.h) in OGRE's maths, for the renderer's callers.
    void drop_item(const tl::core::ItemStats& item, int count, const Ogre::Vector3& at) {
        World::drop_item(item, count, from_ogre(at));
    }
    using World::dropped_to_ground;
    // PORT: `World::dropped_to_ground` (core/world.h) in OGRE's maths, for the renderer's callers.
    Ogre::Vector3 dropped_to_ground(const Ogre::Vector3& at) const {
        const Ogre::Vector3 r = to_ogre(World::dropped_to_ground(from_ogre(at)));
        return r;
    }
    using World::place_gold;
    // PORT: `World::place_gold` (core/world.h) in OGRE's maths, for the renderer's callers.
    void place_gold(const tl::core::GoldPile& pile, const Ogre::Vector3& at,
                    const tl::core::ItemStats* item = nullptr, bool drop_sound = true) {
        World::place_gold(pile, from_ogre(at), item, drop_sound);
    }
    using World::place_ground_item;
    // PORT: `World::place_ground_item` (core/world.h) in OGRE's maths, for the renderer's callers.
    void place_ground_item(const tl::core::ItemStats& item, int count, const Ogre::Vector3& at,
                           const Ogre::Quaternion& orientation, const tl::core::QuestUnitTag& tag) {
        World::place_ground_item(item, count, from_ogre(at), from_ogre(orientation), tag);
    }
    using World::snap_item_to_ground;
    // PORT: `World::snap_item_to_ground` (core/world.h) in OGRE's maths, for the renderer's callers.
    Ogre::Vector3 snap_item_to_ground(const Ogre::Vector3& at) {
        const Ogre::Vector3 r = to_ogre(World::snap_item_to_ground(from_ogre(at)));
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    tl::core::QuestEventUnit quest_event_unit(const std::string& name, const std::string& unittype,
                                              const ogre::Vector3& at,
                                              const tl::core::QuestUnitTag& tag) override {
        const auto r = quest_event_unit(name, unittype, to_ogre(at), tag);
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    Ogre::SceneNode* view_place_entity(Ogre::Entity* entity, const ogre::Vector3& at,
                                       const ogre::Quaternion& turn) override {
        auto* const r = view_place_entity(entity, to_ogre(at), to_ogre(turn));
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    std::vector<Ogre::SceneNode*> place_particle_layout(const std::string& reference,
                                                        const ogre::Vector3& at,
                                                        const ogre::Quaternion& facing,
                                                        Ogre::SceneNode* parent,
                                                        ParticleStart start) override {
        const auto r =
            place_particle_layout(reference, to_ogre(at), to_ogre(facing), parent, start);
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    void view_aim_node(Ogre::SceneNode* node, const ogre::Vector3& at,
                       const ogre::Vector3& forward) override {
        view_aim_node(node, to_ogre(at), to_ogre(forward));
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    Ogre::SceneNode* view_create_node(const ogre::Vector3& at) override {
        auto* const r = view_create_node(to_ogre(at));
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool view_held_item(const Ogre::Entity* held, ogre::Vector3& hand,
                        float& height) const override {
        Ogre::Vector3 hand_o = to_ogre(hand);
        const auto r = view_held_item(held, hand_o, height);
        hand = from_ogre(hand_o);
        return r;
    }
    using World::add_combat_text;
    // PORT: `World::add_combat_text` (core/world.h) in OGRE's maths, for the renderer's callers.
    void add_combat_text(tl::core::CombatText what, const Ogre::Vector3& unit_at, int amount = 0) {
        World::add_combat_text(what, from_ogre(unit_at), amount);
    }
    using World::loot_placement_at;
    // PORT: `World::loot_placement_at` (core/world.h) in OGRE's maths, for the renderer's callers.
    tl::core::LootPlacement loot_placement_at(const Ogre::Vector3& at) {
        const auto r = World::loot_placement_at(from_ogre(at));
        return r;
    }
    using World::play_death_particle;
    // PORT: `World::play_death_particle` (core/world.h) in OGRE's maths, for the renderer's callers.
    void play_death_particle(const tl::core::UnitStats& unit, const Ogre::Vector3& at,
                             CharacterRef who) {
        World::play_death_particle(unit, from_ogre(at), who);
    }
    using World::play_hit_particle;
    // PORT: `World::play_hit_particle` (core/world.h) in OGRE's maths, for the renderer's callers.
    void play_hit_particle(const tl::core::UnitStats& victim, const Ogre::Vector3& from,
                           const Ogre::Vector3& at, CharacterRef who) {
        World::play_hit_particle(victim, from_ogre(from), from_ogre(at), who);
    }
    using World::play_strike_particle;
    // PORT: `World::play_strike_particle` (core/world.h) in OGRE's maths, for the renderer's callers.
    void play_strike_particle(const tl::core::UnitStats& attacker, const Ogre::Vector3& from,
                              const Ogre::Vector3& at, float radius, bool critical, bool blocked,
                              CharacterRef who) {
        World::play_strike_particle(attacker, from_ogre(from), from_ogre(at), radius, critical,
                                    blocked, who);
    }
    using World::show_absorbed;
    // PORT: `World::show_absorbed` (core/world.h) in OGRE's maths, for the renderer's callers.
    void show_absorbed(const Ogre::Vector3& at, const tl::core::UnitStats& stats) {
        World::show_absorbed(from_ogre(at), stats);
    }
    using World::start_death_shake;
    // PORT: `World::start_death_shake` (core/world.h) in OGRE's maths, for the renderer's callers.
    void start_death_shake(const char* name, float duration, float magnitude,
                           const Ogre::Vector3& centre) {
        World::start_death_shake(name, duration, magnitude, from_ogre(centre));
    }
    using World::unit_node_position;
    // PORT: `World::unit_node_position` (core/world.h) in OGRE's maths, for the renderer's callers.
    bool unit_node_position(int64_t node, Ogre::Vector3& at) {
        ogre::Vector3 at_c = from_ogre(at);
        const auto r = World::unit_node_position(node, at_c);
        at = to_ogre(at_c);
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool skill_bone_world(const ViewUnit& unit, int attachment, ogre::Vector3& out) override {
        Ogre::Vector3 out_o = to_ogre(out);
        const auto r = skill_bone_world(unit, attachment, out_o);
        out = from_ogre(out_o);
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool skill_unit_forward(const ViewUnit& unit, ogre::Vector3& out) override {
        Ogre::Vector3 out_o = to_ogre(out);
        const auto r = skill_unit_forward(unit, out_o);
        out = from_ogre(out_o);
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    void start_character_particle(const tl::core::UnitStats& unit, int slot,
                                  const ogre::Vector3& at, CharacterRef who) override {
        start_character_particle(unit, slot, to_ogre(at), who);
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    void view_stretch_lines(const std::vector<Ogre::SceneNode*>& line_holders,
                            const ogre::Vector3& start, const ogre::Vector3& end) override {
        view_stretch_lines(line_holders, to_ogre(start), to_ogre(end));
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    void view_translate(const std::vector<Ogre::SceneNode*>& holders,
                        const ogre::Vector3& by) override {
        view_translate(holders, to_ogre(by));
    }
    // PORT: the scene node hooks (core/world.h).
    ogre::Vector3 view_position(const Ogre::SceneNode* node) const override {
        return from_ogre(node->getPosition());
    }
    ogre::Quaternion view_orientation(const Ogre::SceneNode* node) const override {
        return from_ogre(node->getOrientation());
    }
    ogre::Vector3 view_derived_position(const Ogre::SceneNode* node) const override {
        return from_ogre(node->_getDerivedPosition());
    }
    ogre::Quaternion view_derived_orientation(const Ogre::SceneNode* node) const override {
        return from_ogre(node->_getDerivedOrientation());
    }
    void view_set_position(Ogre::SceneNode* node, const ogre::Vector3& at) override {
        node->setPosition(to_ogre(at));
    }
    void view_set_orientation(Ogre::SceneNode* node, const ogre::Quaternion& turn) override {
        node->setOrientation(to_ogre(turn));
    }
    Ogre::SceneNode* view_parent_node(const Ogre::Entity* entity) const override {
        return entity->getParentSceneNode();
    }
    tl::core::DungeonFiles archive_dungeon_files() const override;
    // PORT: the hook (core/world.h) in OGRE's maths.
    void play_effect_fx(tl::core::EffectManager& effects, const ogre::Vector3& centre,
                        const std::string& who) override {
        play_effect_fx(effects, to_ogre(centre), who);
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool play_sound(const std::string& name, const ogre::Vector3& at) override {
        const auto r = play_sound(name, to_ogre(at));
        return r;
    }
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool play_sound(const std::string& name, const ogre::Vector3& at, float volume_scale,
                    float radius_if_none, const std::string& voice_key,
                    bool volume_given) override {
        const auto r =
            play_sound(name, to_ogre(at), volume_scale, radius_if_none, voice_key, volume_given);
        return r;
    }
    // PORT: the archive hooks (core/world.h), answered by the archive the renderer mounts.
    std::vector<std::string> archive_paths_in_listing_order(const std::string& dir,
                                                            const char* suffix) const override;
    std::string resolve_archive_path_quiet(const std::string& reference) const override;
    std::string class_unit_path(const std::string& class_name, bool* console) const override;
    std::string archive_animation_in_dir(const std::string& dir, const std::string& state,
                                         bool armed, int variation) const override;
    std::string archive_path_in_dir(const std::string& basename,
                                    const std::string& dir) const override;
    std::string archive_path_unique(const std::string& basename) const override;
    std::vector<std::string> archive_paths_under(const std::string& dir,
                                                 const char* suffix) const override;
    std::vector<std::string> data_paths_under(const std::string& dir) const override;
    std::vector<std::string> data_paths_in_listing_order(const std::string& dir) const override;
    bool console_game_data() const override;
    std::string dir_of_asset(const std::string& name) const override;
    tl::core::DungeonManager& loaded_dungeon_manager() const override;
    std::vector<uint8_t> read_resource(const std::string& path) const override;
    std::string resolve_archive_path(const std::string& reference) const override;

    Ogre::LogManager* log_manager = nullptr;
    std::unique_ptr<OgreLogBridge> log_bridge;

    std::unique_ptr<Ogre::Root> root;
#if defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_ANDROID) || defined(TL_PLATFORM_IOS)
    // One of the two per Root, never both: each makes its own EGL display.
    std::unique_ptr<Ogre::GLES2Plugin> gles2_plugin;
#if defined(TL_SWITCH_GL3PLUS)
    std::unique_ptr<Ogre::GL3PlusPlugin> gl3plus_plugin;
#endif
#else
    std::unique_ptr<Ogre::GL3PlusPlugin> gl3plus_plugin;
    std::unique_ptr<Ogre::GLPlugin> gl_plugin;
#endif

    /// ParticleUniverse, up while Root is. See `render/particle_universe.h`.
    ParticleUniverseHost particle_universe;

    Ogre::RenderWindow* window = nullptr;
    /// Android: the EGL surface let go in the background and remade, the context kept.
    void watch_window_surface();
    void fit_camera_to_window() const;  ///< to the window's first viewport, the world's
    /// config.txt's `fps_limit`: the rest of the frame slept out, after the present.
    void pace_frame();
    std::chrono::steady_clock::time_point next_frame;  ///< when the next frame may end
    /// The desktop window as made, not as asked for: clamped to the screen, or made the
    /// monitor's size by a window manager's fullscreen. `config`'s size becomes its size.
    void adopt_window_size();
    Ogre::SceneManager* scene = nullptr;
    /// See `TransparentsWithScenery`.
    std::unique_ptr<Ogre::RenderQueueListener> transparents_listener;
    Ogre::Camera* camera = nullptr;
    Ogre::SceneNode* camera_node = nullptr;
    Ogre::Light* key_light = nullptr;
    Ogre::SceneNode* key_light_node = nullptr;
    Ogre::SceneNode* sky_node = nullptr;
    /// The template's `LEVELPARTICLE` layout (`CGameClient +0x1080`), put on the player
    /// every frame (`advance`).
    Ogre::SceneNode* level_particle_node = nullptr;
    /// The holders of the systems only the view draws (`hold_particle_with_the_world`).
    Ogre::SceneNode* particle_holder_node = nullptr;
    void hold_particle_with_the_world(const ParticleSystem& system);

    /// Everything a layout placed, for the ground query below.
    std::vector<Ogre::Entity*> layout_meshes;

    /// Which of `layout_meshes` are units. See `MeshPlacement::is_unit`.
    std::set<Ogre::Entity*> unit_meshes;

    /// `stgeom_`, the room pieces' main batch (bake_static_scenery()); `stgeompre_` and
    /// `stgeompost_` are the scene manager's.
    Ogre::StaticGeometry* static_scenery = nullptr;
    /// The room pieces bake_static_scenery() may batch: bakeable, with the
    /// render phase their tileset gives them.
    struct BakeablePiece {
        Ogre::Entity* entity = nullptr;
        int render_phase = 0;
    };
    std::vector<BakeablePiece> bakeable_room_pieces;
    /// Numbers the sets' names, which a scene manager keeps unique.
    unsigned baked_sets = 0;

    /// The room pieces placed, whose materials take the light map's
    /// `LIGHTPASS` (see MeshPlacement::room_piece and add_light_pass()).
    std::vector<Ogre::Entity*> room_piece_meshes;

    /// The layout `Light`s' copies (MeshPlacement::light_density): light map only (`CLevel::bake`'s
    /// `stgeomLight_`; entities here).
    std::vector<Ogre::Entity*> light_pool_meshes;

    /// The layout's particle systems; see ParticleSystem.
    std::vector<ParticleSystem> particle_systems;

    ParticleIndex particle_index;
    void particle_index_add(size_t i);
    void particle_index_remove(size_t i);
    /// The systems one of `keys` (by link) or `wrapper_keys` (wrappers by node) names, in
    /// `particle_systems`' order, each once; then the lookups script events make.
    ParticleRefs indexed_particles(std::initializer_list<ParticleIndex::Key> keys,
                                   std::initializer_list<ParticleIndex::Key> wrapper_keys);
    /// The systems hanging from `holders`, in `particle_systems`' order.
    ParticleRefs holder_systems(const std::vector<Ogre::SceneNode*>& holders);
    /// `particle_index` brought up to date after a destruction made it stale.
    void ensure_particle_index();
    ParticleRefs map_wrappers(const NodeMap& nodes);
    ParticleRefs link_systems(const NodeMap& nodes, int64_t node);
    ParticleRefs link_or_free_systems(const NodeMap& nodes, int64_t node);
    ParticleRefs node_wrappers(const NodeMap& nodes, int64_t node);
    ParticleRefs shown_systems(const NodeMap& nodes, int64_t node);
    size_t show_node_particles(const tl::core::ScriptEvent& event, NodeMap& nodes);

    /// Everything in the scene with an animation running: the level's NPCs and animated
    /// props. Apart from animated() (the subject), which runs at the player's pace.
    struct SceneAnimation {
        Ogre::Entity* entity = nullptr;
        float rate = 1.0f;
        /// A `Room Piece`'s model: `CRoomPiece::updateAnimation`
        /// runs it only while the camera sees
        /// its box (`room_piece_in_view`).
        bool room_piece = false;
    };
    /// `CRoomPiece::updateAnimation`'s test: position plus model box
    /// (`CGenericModel +0x1d4..+0x1e8`) inside the frustum (`Ogre::Frustum::isVisible`).
    ///
    bool room_piece_in_view(const Ogre::Entity* entity) const;
    std::vector<SceneAnimation> scene_animations;

    /// The track of `state` on `entity`'s animator, added on first use.
    unsigned entity_track(EntityAnimation& animation, Ogre::AnimationState* state);
    /// Copy the animator's tracks onto the states, where they changed.
    void write_entity_animation(EntityAnimation& animation) override;

    /// The window an NPC offers a quest in (`questdialogmenu_xenon.layout`): a
    /// `CQuestDialogMenu`, apart from a `FLOATYTEXT` line's `CTextEvent`.
    QuestDialogScreen quest_dialog;

    /// See `render/death_screen.h`.
    DeathScreen death_screen;
    /// The fishing ring (`media/UI/fishing_xenon.layout`, `CFishingMenu`, `CGameUI +0x3e4`),
    /// shown in AI state 0x21. `CFishingMenu::CFishingMenu` (X360 inline in
    /// `CGameUI::create`) starts shown (`+0x30`, X360 `+0x18`);
    /// `createMenus` builds it.
    LayoutScreen fishing_screen;
    /// Load Character (`media/UI/characterload.uilayout`, `CContinueGameMenu`),
    /// built when the screen is first asked for and drawn from
    /// `tl::core::ContinueMenu` by `show_load_character`.
    LayoutScreen character_load_screen;
    void show_load_character(const tl::core::ContinueMenu& state, const std::string& shown_file);
    /// `highlight`'s authored corner; `CContinueGameMenu +0x188` keeps it less
    /// `Player1`'s, which `show_load_character` takes off.
    float load_highlight_x = 0.0f, load_highlight_y = 0.0f;
    /// The modal message (`media/UI/modalmenu_xenon.layout`, `CModalMenu`, `CGameUI +0x3cc`)
    /// as `CGameUI::openModalDialog` opens it: a title, a line and `Ok`.
    /// `CModalMenu::CModalMenu`; no callbacks here.
    LayoutScreen modal_screen;

    /// `CQuestDialog::giveOrRemoveItems`: `[TAKEITEM]`s,
    /// then `[GIVEITEM]`s; false when already handed over (`+0x77`).
    bool hand_dialog_items(const tl::core::QuestDefinition& quest,
                           const tl::core::DialogLine& line) override;
    /// A floaty line the player's talk showed: `CQuestDialogMenu::update`
    /// calls `onClick(10)` on its next pass
    /// (`fresh` until then).
    struct TalkFloatyClose {
        bool pending = false;
        bool fresh = false;
        int machine = kNoMachine;
        int64_t node = 0;
        bool complete = false;
        std::string quest;
    };
    TalkFloatyClose talk_floaty_close;
    /// The placed or spawned unit called `unit` nearest the player, or 0.
    int64_t nearest_unit_node(const std::string& unit) const;

    /// Build the offer window for `unit` and show it; false when it has nothing to say or
    /// the console interface data is not mounted.
    bool open_quest_offer(const std::string& unit);
    /// `CCharacter::approachAI`'s choice for the unit reached, and what
    /// `CGameUI::updateIngameUI` opens for it.
    void interact_with(const std::string& unit);
    void open_vendor_screen(const std::string& unit, VendorScreen screen);
    void close_quest_offer() override;
    bool open_script_dialog(const tl::core::ScriptEvent& event, const std::string& text);
    /// The quest window is showing a line a `Quest Controller` opened, and
    /// answering it answers the script.
    bool script_dialog_window = false;

    /// `CQuestDialog::cleanUp` for a completed quest's
    /// lines: the pets they made leave the player.
    void release_quest_pets(const tl::core::QuestCompletion& done) override;

    /// `CQuestRewards::rewardPlayer` for one completion.
    void reward_player(const tl::core::QuestCompletion& done) override;

    /// The template's UNIT_FADE_DISTANCE and UNIT_FADE_START: where a unit's
    /// light falls to 0 and where it starts to (update_unit_lights()).
    float level_unit_fade_distance = 14.5f;
    float level_unit_fade_start = 7.0f;
    /// `KSETTINGS_F_AVERAGE_FPS`, kept as `CGame::update`
    /// keeps it.
    tl::core::AverageFps average_fps;
    /// The camera test `CCharacter::update` makes on the
    /// culling bounds (`culling_box`); no bounds counts as in view.
    bool unit_in_view(const tl::core::LevelScript& machine, int64_t node) override;
    /// A unit's culling bounds in the world (`CBaseUnit +0x1c0`, a
    /// `CCullingBounds`), sized from its model or the constructor's default.
    Ogre::AxisAlignedBox culling_box(Ogre::SceneNode* node, bool from_model) const;

    // The town portal (`core/town_portal.h`, renderer_portal.cpp).
    /// `CSoundObjectDescriptor::descriptorSceneActivated`
    /// for a portal's layout: `START ON ACTIVATE` sounds play.
    void start_portal_sounds(OpenPortal& portal);
    /// The held portal sounds' gain and pan, from where the listener stands.
    void step_portal_sounds();
    /// The gain and pan `play_sound` gives a definition heard at `at`; false
    /// when it is out of earshot.
    bool positional_mix(float volume, float radius, const Ogre::Vector3& at, float& gain,
                        float& pan) const;
    int portal_targeted = -1;
    /// `warpLevels("Town", -99)` from a portal used (`CTriggerUnit::trigger`).
    bool portal_warp_pending = false;
    tl::core::PortalLevel portal_level() const override;
    void delete_open_portals() override;
    void create_portals() override;
    /// `CCharacter::applyInstantEffect`'s `OPEN PORTAL` on the player:
    /// `CPlayer::openMapPortal`.
    void open_map_portal(const std::string& dungeon) override;
    /// `CResourceManager::createUnit(L"PROPS", L"MAP PORTAL")`, `CLevel::addItem` and its
    /// `WARP` effect: from `openMapPortal` and from a stored level state.
    bool add_map_portal(const Ogre::Vector3& at, const std::string& warp, bool enabled);
    /// What `setEnabled(false)` takes away from a portal prop: its particles
    /// and held sounds.
    void disable_portal(OpenPortal& portal);
    /// The level's portal props as `core/town_portal.h`'s two walks read them,
    /// one entry per `open_portals` entry and in its order.
    std::vector<tl::core::TemporaryPortal> temporary_portals() const;
    /// A on the targeted portal: `CTriggerUnit::trigger`.
    ///
    void use_targeted_portal();

    /// The Xbox `waypointmenu_xenon.layout`, and what `CWaypointMenu::setOpen`
    /// put in it.
    LayoutScreen waypoint_screen;
    /// `CGameUI::toggleWaypointMenu` ->
    /// `CWaypointMenu::setOpen`.
    void toggle_waypoint_menu();
    void open_waypoint_menu();
    void close_waypoint_menu() override;
    void refresh_waypoint_menu() override;
    // The Pause Menu and Options (`core/settings_menu.h`, renderer_options.cpp).
    /// `COptionsMenu` on the Xbox `options_Xenon.layout`: Start in the game.
    LayoutScreen pause_screen;
    bool pause_stick_held = false;
    /// `CSettingsMenuXenon` on `settingsmenu_xenon.layout`.
    LayoutScreen settings_screen;
    bool settings_stick_held = false;
    bool settings_dpad_held = false;
    bool settings_y_held = false;
    /// Y on Options: Help once the close has been carried out.
    bool help_after_settings = false;
    /// Help, `CHowToMenu` (`CGameUI +0x18b8`) on the Xbox `howto_xenon.layout`
    /// (`core/howto_menu.h`).
    LayoutScreen howto_screen;
    void create_options_screens();
    /// `CGameUI::toggleOptions`, opening.
    void open_pause_menu();
    void close_pause_menu() override;
    void refresh_pause_menu();
    /// The pad on the Pause Menu: the tab list, A is `onClick` on the row,
    /// Start (and B) close it.
    void drive_pause_menu(const tl::core::InputState& in, bool start_edge);
    /// `CGameUI::toggleSettings`, opening.
    void open_settings_menu();
    void refresh_settings_menu();
    void drive_settings_menu(const tl::core::InputState& in);
    /// `CSettingsMenuXenon::vfunc_3` once a frame: carries a close out, writes
    /// the file after Done and brings the Pause Menu back.
    void step_settings_menu();
    /// `CHowToMenu::setOpen`, and what its close leads to.
    void set_help_open(bool open) override;

    // Context tips (`core/context_tips.h`, renderer_tips.cpp).
    /// `CTipMenu` (`CGameUI +0x550`) on the Xbox `tipmenu_xenon.layout`.
    LayoutScreen tip_screen;
    /// `CGameUI +0x1960`.
    tl::core::TipQueue tip_queue;
    /// `CTipMenu +0x30`; `CTipMenu::CTipMenu`.
    bool tip_open = false;
    /// `CGameUI +0x1798`, the seven left-side menus `CGameUI::create` lists:
    /// `CInventoryMenuXenon`, `CSkillMenuXenon`, `CQuestMenuXenon`,
    /// `CMerchantMenuXenon`, `CEnchantMenuXenon`, `CStashMenuXenon`, `CStatsMenuXenon`; the
    /// port's master menu is all of them.
    bool full_screen_menu_up() const override { return inventory_menu.visible(); }
    /// Which of them, for the log.
    const char* modal_dialog_name() const;
    bool level_was_paused = false;
    /// The player's target is an interactive character (`ISA 0x83`) through its
    /// conversation: paused, `CGameClient::updateIngame`
    /// keeps the particles beside it running. A quest controller's window too
    /// (`CQuestController::interactWithUnit` -> `CGameUI::forceInteractiveDialog`
    /// `CCharacter::setTarget`).
    bool talking_to_unit() const {
        return talk_target_machine != kNoMachine ||
               (quest_dialog.visible() &&
                (!script_dialog_window || conversation_speaker() != nullptr));
    }
    /// That, or the quest dialog having its NPC: what `updateIngame` still
    /// updates while paused - the unit, and the player beside it.
    bool conversation_open() const {
        return talk_target_machine != kNoMachine || quest_dialog.visible() || script_dialog_open();
    }
    /// The unit the open quest window is with, or null.
    Ogre::Entity* conversation_speaker() const;
    /// `updateIngame`'s paused branch: the speaker's and the player's poses.
    void step_conversation_poses(float delta, float player_rate);
    void create_tip_screen();
    /// `CGameUI::queueTip`.
    void queue_tip(tl::core::ContextTip tip) override;
    /// The queue's turn in `CGameUI::updateIngameUI`, once a frame.
    void step_tips();
    /// A button on the open tip, one of the `kMenuPad*`
    /// codes: B closes, Y turns tips off.
    void press_tip_menu(int button);
    /// `CGameUI::modalDialogOpenPartial`: a window of the
    /// modal list (`CGameUI +0x1948`) is open. On the console also `CGameUI::modalDialogOpen`:
    /// no console window is ever still closing.
    ///
    bool modal_dialog_open() const override;
    /// The console screens' tips: the bag, the merchant and the stash as they
    /// open, the stats and skills tabs while points are unspent.
    void queue_menu_tips();
    bool tip_bag_was_open = false;

    /// One-shot animations a script controller started, so the machine is told when one
    /// ends (`Animation Stopped`): only the renderer knows how long one runs.
    struct PendingAnimation {
        int64_t controller = 0;
        Ogre::AnimationState* state = nullptr;
    };
    std::vector<PendingAnimation> pending_animations;

    /// A `Unit Trigger` prop part-way through its opening transition:
    /// `CTriggerUnit::setTriggerState` blends it once, keeps the unit active for
    /// its length plus 0.3 s (`+0x1e0`), then queues the state's own animation.
    struct PendingTriggerLoop {
        Ogre::AnimationState* transition = nullptr;
        int64_t node = 0;
        std::string unit;
        std::string next;
        bool loops = false;
    };
    std::vector<PendingTriggerLoop> pending_trigger_loops;

    /// `CTriggerUnit +0x2b0`: each state's layout (`LAYOUT_STATE_n`, `unitInit`)
    /// on the unit's node; `setTriggerState`
    /// shows the current one. DIVERGENCE: only the current one exists.
    struct TriggerStateLayout {
        int state = 0;
        std::vector<Ogre::SceneNode*> holders;
    };
    std::map<Ogre::Entity*, TriggerStateLayout> trigger_state_layouts;

    /// The cinematic state `update_npc_icons` last saw (`gCinematicActive`).
    bool npc_icons_cinematic = false;
    void view_skill_effects_changed() override;
    /// `CGameUI::useItem` on the player's bag slot;
    /// `from_tray` speaks a `Town Portal Scroll` refusal.
    bool use_player_item(size_t slot, bool from_tray = false) override;
    /// The console's default A on any other bag row: identify, take off,
    /// or put on in place of what is in the way. True when anything changed.
    bool wear_player_item(size_t slot) override;
    /// The same on the copy at `slot` of `bag` (the pet's, on its tray).
    bool identify_item_in(tl::core::Inventory& bag, size_t slot) override;

    // ------------------------------------------- the pet's items and spells --
    // renderer_pet_items.cpp: A on the pet's tray, the learn and its
    // replace dialog (`CMessageSpellRemoveXenon`), the pet's effects and
    // regeneration (`CCharacter::updateEffects`, `updateHP`).

    /// What asks of a tray's owner about `item`.
    /// `CGameUI +0x14c4`: item compare, toggled by the D-pad's left and right.
    /// Off, details come 0.65 s after the selection settles
    /// (`+0x1e4`); on, at once with the worn items beside.
    bool item_compare = false;
    bool item_compare_held = false;
    float item_tip_timer = 0.0f;
    /// The tray's `+0x1e8`: the item the timer runs for, as tray * 1000 + slot,
    /// -1 for none.
    int item_tip_key = -1;
    bool item_tip_shown = false;
    void toggle_item_compare();
    void step_item_tooltips(float dt);
    void show_item_tooltips();
    tl::core::TrayCaptionContext tray_caption_context(const tl::core::ItemStats& item,
                                                      bool pet_tray);
    /// A on the pet's tray: with the pet as the owner.
    void pet_tray_press();
    /// `canUse(item, user, pet)` and `useOnTarget(item, user, pet)`
    /// (Linux `CEquipment::canUseOnTarget`, `useOnTarget`) on
    /// the first pet: a spell learned, an instant `TRANSFORM` (`LevelScript::transform_unit`).
    /// False when `canUse` refuses; `used` is the caller's to spend by.
    bool use_item_on_pet(const tl::core::ItemStats& item, int stack, bool by_player,
                         tl::core::ItemUseResult& used) override;
    /// The pet's half of a `Transform` event: `convertCharacter`
    /// re-made it as `unit`.
    void pet_converted(const std::string& unit);
    /// the function's equipment branch on the pet's tray.
    bool wear_pet_item(size_t slot);
    /// The replace dialog's state (`CMessageSpellRemoveXenon`): whose book, the
    /// scroll's slot in that owner's bag, the choice (`+0x38`).
    bool spell_remove_pet = false;
    size_t spell_remove_slot = 0;
    int spell_remove_choice = 0;
    bool spell_remove_stick_x_held = false;
    bool open_spell_remove(bool pet_owner, size_t slot) override;
    /// `vfunc_5` for the owner as it is now.
    void refresh_spell_remove();
    /// `vfunc_2` on the pad, and the tray's poll once it is down:
    /// the chosen slot unlearned and the scroll tried again.
    void handle_spell_remove(const tl::core::InputState& in, bool x_edge, bool y_edge,
                             bool start_edge, bool back_edge, bool other_edge);

    /// `CGameUI::activateItemSlot` and the use it leads to.
    void activate_item_slot(int index) override;
    /// `CCharacter::castSkill` for the player (`--cast` and
    /// the hotbar). `report` logs a refusal; a held button does not.
    bool cast_player_skill(const tl::core::KnownSkill& skill, bool report) override;
    /// The skills tab's description column as text beside its PNG, for regression checks.
    void write_skill_text_sidecar(const std::string& png_path);
    /// `CCharacter::update`'s effect calls, in order:
    /// affix durations, effect ages, the sweep.
    void update_player_effects(float dt);
    /// `CCharacter::updateCameraOpacity` on the player (`tl::core::update_camera_opacity`):
    /// `delta` is the frame's, or 0 while the game is paused.
    void step_player_camera_opacity(float delta);
    /// The player's `+0x528` (X360 `+0x508`): 1 until the camera comes within 6.
    float player_opacity = 1.0f;
    /// The buttons the skills tab hears this frame: pressed (`GamepadButtonDown`)
    /// and, for the four a skill goes on, released (`GamepadButtonUp`).
    struct SkillsTabPad {
        bool dpad_up = false;
        bool dpad_down = false;
        bool lt = false;
        bool rt = false;
        bool y = false;
        bool x = false;
        bool start = false;
        bool lt_up = false;
        bool rt_up = false;
        bool y_up = false;
    };
    /// The pad on the skills tab: `CSkillMenuXenon`'s handler and
    /// its map sheet's; `core/skills_tab.*` decides.
    void handle_skills_tab(const tl::core::InputState& in, const SkillsTabPad& pad);
    bool skill_stick_held = false;
    /// The master menu closed as its housing's `setOpen(false)`: `InventoryClose` (0x42)
    /// except the skills tab (`core::housing_plays_close_sound`); Stats points placed.
    void close_master_menu();
    /// LT / RT on the master menu: the housing's close, then
    /// the next one's open (`InventoryOpen`, 0x16).
    void step_master_menu_tab(int direction);

    /// The letterbox bars and the dialogue box a script shows (render/hud.h).
    Hud hud;

    /// The game screen's interface (orbs, skill slots, experience bar) from
    /// `hud_xenon.layout`; `hud` is the script's.
    GameHud game_hud;

    /// What the last creation screen was left holding (`creation_names()`).
    std::string created_player_name;
    std::string created_pet_name;

    /// The console's stash, `CPlayer +0x9f4`.
    tl::core::Inventory player_stash = tl::core::make_player_stash();
    /// `CSharedStash::loadSharedStash` / `saveSharedStash`,
    /// the file in `directory`.
    void load_shared_stash(const std::string& directory);
    void save_shared_stash(const std::string& directory);

    /// The Xbox 360 Menu (`media/UI/inventorymenu_xenon.layout` etc.).
    InventoryMenu inventory_menu;

    /// Input edge detection flags for menu navigation.
    bool inventory_button_held = false;
    bool inventory_stick_x_held = false;
    bool death_stick_held = false;
    bool inventory_stick_y_held = false;
    bool menu_prev_held = false;
    bool menu_next_held = false;
    bool tab_prev_held = false;
    bool tab_next_held = false;
    bool action_y_held = false;
    /// `in.attack` (X) read as the inventory screen's own X while it is open.
    bool action_x_held = false;
    bool pet_stance_held = false;
    bool pet_town_held = false;
    /// Hotbar buttons already down when the hotbar got the pad back (e.g. the B that closed
    /// a menu), ignored until let go. `hotbar_had_pad`: the hotbar read the pad last frame.
    std::array<bool, 7> hotbar_wait_release{};
    bool hotbar_had_pad = false;

    /// Whether the next main menu offers `ContinueLast`.
    bool continue_available = false;

    std::string chosen_dungeon;
    /// The floor that goes with it: the picked global floor number.
    int chosen_dungeon_floor = 1;
    /// The biome the Choose Level list returned `ChooseLevelBiome` for (`run_main_menu()`).
    std::string chosen_biome;

    /// Spawned units playing their entrance, held out of their AI until it stops
    /// (`LevelScript::set_spawning`).
    std::map<int64_t, Ogre::AnimationState*> spawning_units;
    static constexpr float kFormationWakeDistance = 25.0f;  ///< `distance < 25.0`
    void wake_formations();

    /// The Xbox `+` over the bag and its glow (`core/game_ui.h`).
    tl::core::AvailablePointsIcon available_points;

    /// `CPlayer::levelUp` after `CCharacter::levelUp`:
    /// `PLAYER_LEVELUP` particle, chime and narrator, the banners for 2.0 seconds, full
    /// health and mana.
    void player_levelled_up() override;

    /// Fill the HUD in from what the engine knows (`HudState`).
    void refresh_game_hud() override;

    /// The main menu, built from the game's own interface files. Empty until
    /// run_main_menu() puts one up.
    MainMenu menu;

    /// The loading screen, from `media/UI/loading.layout`.
    LoadingScreen loading;

    /// The character creation screen, from `media/UI/charactercreate.layout`.
    CharacterCreate create_screen;

    /// Paint the loading screen and leave it on the display: one frame, as loading is
    /// synchronous and what was presented last stays.
    void paint_loading_screen() {
        if (config.headless || window == nullptr || root == nullptr) return;
        if (!loading.show(kGameGroup, config.width, config.height)) return;
        // Hidden while painted: the outgoing level's line must not show through.
        hud.set_line(std::string(), std::string());
        root->renderOneFrame();
        // After the frame: the boost lowers the GPU clock.
        tl::core::set_loading_boost(true);
    }

    /// The line on screen, for the debug overlay and for the frame dumps.
    std::string dialog_line;
    std::string dialog_speaker;

    /// A `FLOATYTEXT` line and who says it, while it fades (a `CTextEvent`, apart from the
    /// box). This countdown drives the fade only; the machine's decides when it is over.
    std::string floaty_line;
    std::string floaty_unit;
    float floaty_remaining = 0.0f;

    /// The `CTextEvent`s in the air (`CGameUI +0x1468`), oldest first (core/text_event.h).
    std::vector<tl::core::TextEvent> text_events;

    /// Who each hunting unit is going for, by attacker node: 0 the player, else the
    /// target's node (from `Hunt` events, for `step_monster_swings()`).
    std::map<int64_t, int64_t> hunt_target;

    /// The `Pathing` node walking the player, or 0 (only the renderer knows it arrived).
    int64_t scripted_walk_path = 0;

    /// `--logic-trace`: every wire the level script delivers, in the trace line format.
    std::FILE* logic_trace = nullptr;
    // PORT: whether --logic-trace is writing (core/world.h).
    bool logic_trace_on() const override { return logic_trace != nullptr; }
    float logic_trace_next_sample = 0.0f;
    Ogre::Vector3 logic_trace_last{1e9f, 1e9f, 1e9f};

    /// The script has the player disabled (`CSceneNodeObject::setEnabled(false)`): only a
    /// scripted path moves the character.
    bool script_player_disabled = false;

    /// Units a script put on a one-shot, back to idle when it has run
    /// (`CCharacter::setAIPlayAnimation`'s `queueBlendAnimation("IDLE")`; also `die`'s
    /// `DEATH_LAND` behind a flight).
    struct PendingIdle {
        int64_t node = 0;
        std::string unit;
        Ogre::AnimationState* state = nullptr;
        std::string animation = kUnitIdleAnimation;
        bool loops = true;
        /// What is queued behind `animation` in turn, each with whether it loops.
        std::vector<std::pair<std::string, bool>> after;
        /// `animation` queued on the animator (`queueBlendAnimation("IDLE", true, 0.2,
        /// speed)`, starting 0.2 s before the end).
        Ogre::AnimationState* queued = nullptr;
    };
    std::vector<PendingIdle> pending_idles;

    /// The unit the player pressed A on and is walking to, or empty:
    /// `CGameUI::updateIngameUI` opens its talk at state 0x1e, after the walk; dropped if
    /// the walk fails.
    std::string pending_talk;

    /// The stash whose lid is up while its screen is (`CStashMenu::setOpen`).
    std::string stash_opened_unit;
    void animate_stash(const std::string& unit, bool open);
    /// Where a frame's time goes, summed over the fps line's window (us).
    unsigned long long prof_select_us = 0;
    unsigned long long prof_drive_us = 0;
    unsigned long long prof_advance_us = 0;
    unsigned long long prof_render_us = 0;
    /// The part of `prof_render_us` before the swap (OGRE walking the scene, GL calls); the
    /// rest is the swap, so the two split CPU from GPU.
    unsigned long long prof_submit_us = 0;
    /// The level's machines - the monsters' AI, paths and fights - inside
    /// `prof_advance_us` (step_script()).
    unsigned long long prof_script_us = 0;
    /// GpuSkinning::step(), inside `prof_submit_us`.
    unsigned long long prof_skin_us = 0;
    /// Frames a headless run has stepped since its last `sections:` lines.
    unsigned headless_profile_frames = 0;
    unsigned headless_profile_windows = 0;
    /// The update's sections (core/section_profile.h) and OGRE's phases, per frame over
    /// `frames`, to the log when `print`; then both start over.
    void log_sections(unsigned frames, bool print);
    void print_sections(unsigned frames);
    void step_headless_sections();
    /// The skinned units' poses for the frame about to be drawn, on the job pool
    /// (renderer_poses.cpp); OGRE's own pass then finds them done.
    void evaluate_poses();
    std::vector<Ogre::Entity*> pose_jobs;
    /// Entities evaluate_poses() posed since the last timing window.
    unsigned long long posed_entities = 0;
    void step_near_poses(float level_delta, float rate);
    void count_player_footsteps(float was, float now);
    void step_linked_machine(NestedScript& machine, float delta, float speed,
                             const tl::core::Defender& player_defense, const Ogre::Vector3& at);

    /// `prof_submit_us` split at OGRE's seams, summed over every viewport and the window:
    /// `controllers` (frame start to the first scene-graph update: particles, animations),
    /// `scene_graph`, `cull` (also skeletons and particle geometry) and `draw` (GL calls);
    /// `cull` and `draw` are also kept per viewport.
    struct FramePhases final : Ogre::SceneManager::Listener, Ogre::RenderQueueListener {
        enum Target { kView = 0, kTexture, kMinimap, kTargets };
        Ogre::Timer* timer = nullptr;
        const Ogre::RenderTarget* window = nullptr;
        /// The scene listened to, for the viewport being drawn (the minimap's has no
        /// preFindVisibleObjects, Automap::attach).
        const Ogre::SceneManager* scene = nullptr;
        unsigned long long frame_start = 0;
        unsigned long long mark = 0;
        bool first = false;
        int target = kView;
        unsigned long long controllers = 0;
        unsigned long long scene_graph = 0;
        unsigned long long cull = 0;
        unsigned long long draw = 0;
        unsigned long long poses = 0;  // evaluate_poses(), before the frame
        unsigned long long cull_by[kTargets] = {};
        unsigned long long draw_by[kTargets] = {};

        unsigned long long now() const { return timer != nullptr ? timer->getMicroseconds() : 0; }
        void start_frame() {
            frame_start = now();
            first = true;
        }
        void reset() {
            controllers = scene_graph = cull = draw = poses = 0;
            for (int i = 0; i < kTargets; ++i) cull_by[i] = draw_by[i] = 0;
        }
        void preUpdateSceneGraph(Ogre::SceneManager*, Ogre::Camera*) override {
            mark = now();
            if (first) controllers += mark - frame_start;
            first = false;
        }
        void postUpdateSceneGraph(Ogre::SceneManager*, Ogre::Camera*) override {
            scene_graph += now() - mark;
        }
        int classify(const Ogre::Viewport* viewport) const {
            return viewport == nullptr || viewport->getTarget() != window ? kTexture
                   : viewport->getZOrder() != 0                           ? kMinimap
                                                                          : kView;
        }
        void preFindVisibleObjects(Ogre::SceneManager*, Ogre::SceneManager::IlluminationRenderStage,
                                   Ogre::Viewport* viewport) override {
            target = classify(viewport);
            mark = now();
        }
        void postFindVisibleObjects(Ogre::SceneManager*,
                                    Ogre::SceneManager::IlluminationRenderStage,
                                    Ogre::Viewport*) override {
            const unsigned long long spent = now() - mark;
            cull += spent;
            cull_by[target] += spent;
        }
        void preRenderQueues() override {
            if (scene != nullptr) target = classify(scene->getCurrentViewport());
            mark = now();
        }
        void postRenderQueues() override {
            const unsigned long long spent = now() - mark;
            draw += spent;
            draw_by[target] += spent;
        }
    };
    FramePhases frame_phases;

    /// Two jobs on OGRE's frame events: `frameStarted` ends the loading boost
    /// (paint_loading_screen()), and `frameRenderingQueued` (Root::_updateAllRenderTargets)
    /// splits `prof_submit_us` from the swap. It also counts frames for
    /// `step_stats_overlay`, in every loop.
    struct FrameClock final : Ogre::FrameListener {
        Ogre::Timer* timer = nullptr;
        Impl* owner = nullptr;
        unsigned long long queued_us = 0;
        bool frameStarted(const Ogre::FrameEvent&) override {
            tl::core::set_loading_boost(false);
            if (owner != nullptr) owner->step_stats_overlay();
            if (owner != nullptr) owner->touch_overlay.step();
            if (owner != nullptr) owner->evaluate_poses();
            if (owner != nullptr) owner->frame_phases.start_frame();
            if (owner != nullptr) {
                owner->gpu_timer.frame_started(
                    owner->scene,
                    {owner->window, owner->light_map_target(), owner->shadow_map_target()});
            }
            return true;
        }
        bool frameRenderingQueued(const Ogre::FrameEvent&) override {
            if (owner != nullptr) owner->gpu_timer.frame_queued();
            // Every draw of the frame is issued: materials can change now consistently.
            if (owner != nullptr && timer != nullptr) {
                const unsigned long long s0 = timer->getMicroseconds();
                owner->gpu_skinning.step(owner->scene);
                owner->prof_skin_us += timer->getMicroseconds() - s0;
            }
            if (timer != nullptr) queued_us = timer->getMicroseconds();
            return true;
        }
        bool frameEnded(const Ogre::FrameEvent&) override {
            if (owner != nullptr) owner->pace_frame();
            return true;
        }
    };
    FrameClock frame_clock;
    TouchOverlay touch_overlay;

    int unit_machine(const std::string& unit, int64_t& node) const override;
    /// `CQuestDialogMenu::updateDialogTags`: the line's
    /// `<LOOKAT=NAME>` made the speaker's target.
    void update_dialog_tags(const std::string& unit, const std::string& text);

    /// The confirm button was down last frame: a dialog goes on the press, not the hold.
    bool confirm_held = false;
    bool attack_held = false;  ///< the pad's Attack (X), for its press edge
    /// The X button's share of the in-game dispatch.
    ///
    void step_attack_button(float delta, bool down, bool pressed, float move_x, float move_y);
    /// `step_attack_button` ran this frame (the pad's dispatch); `advance`
    /// clears it, and steps the button itself for `auto_attack` without it.
    bool attack_button_stepped = false;

    /// The title's main menu is running (`run_main_menu`): Options' close test
    /// (`+0x1404`), out of a game the Pause Menu does not come back.
    bool title_menu_running = false;
    int debug_menu_row = 0;
    bool quit_held = false;
    bool debug_stick_held = false;
    void open_debug_menu();
    void close_debug_menu();
    void refresh_debug_menu();
    /// The menu's pad: up and down, A, and B or + to close.
    void drive_debug_menu(const tl::core::InputState& in);

    /// The port's performance line (`Hud::set_stats`): frames over half-second windows,
    /// and the window's size; toggled from the debug menu.
    bool stats_overlay_on = false;
    unsigned stats_frames = 0;
    unsigned long long stats_window_us = 0;
    void set_stats_overlay(bool on);
    void step_stats_overlay();

    /// Which path each unit is walked by, so the log can say where a path picked it up.
    std::map<int64_t, std::string> unit_path;

    /// Definitions already reported on, so each is said once.
    std::set<std::string> reported_sounds;

    /// Play a sound definition by name, positioned in the world (authored files, `VOLUME`,
    /// `VOLUMEVARIATION`, `PLAYCHANCE`, `RADIUS`). False when not played.
    bool play_sound(const std::string& name, const Ogre::Vector3& at);
    /// The same for a layout's `Sound` node (`CSoundObject::play`):
    /// its `VOLUME` scales the definition's, its `RADIUS` fills in
    /// (`CSoundBank::addSample`), and its last sound stops first.
    /// `volume_given`: `CSoundBank::playSample` handed a
    /// volume, standing in for the definition's (as `CParticleCache::playSounds` does).
    bool play_sound(const std::string& name, const Ogre::Vector3& at, float volume_scale,
                    float radius_if_none, const std::string& voice_key, bool volume_given = false);
    /// A sample whose draws the level machine made (`LevelScript::play_unit_sample`).
    bool play_sound_drawn(const std::string& name, const std::string& recording, float variation,
                          const Ogre::Vector3& at);

    /// ... and without a position, for something that happens to the player.
    bool play_sound_2d(const std::string& name, const char* label = "voice") override;

    /// The channels a layout's `Sound` nodes play on (core/sound_object.h): `CSoundManager`'s
    /// 64 (`FMOD::System::getChannel`), each a mixer voice; a positional one keeps its spot.
    struct NodeChannel {
        bool used = false;
        bool positional = false;
        Ogre::Vector3 at = Ogre::Vector3::ZERO;
        float gain = 0.0f;
        float pan = 0.0f;
        /// Held with no voice, a run with no audio device: sounding while it
        /// loops, over at once otherwise.
        bool silent = false;
        bool silent_loop = false;
    };
    std::array<NodeChannel, 64> node_channels;
    bool node_channel_sounding(size_t index) const;
    /// What a machine's sound banks ask of the mixer.
    tl::core::SoundBankHost node_sound_host() override;
    /// The pan of each positional node channel, kept to the listener and the
    /// camera every frame.
    void step_node_channels(float delta);
    float node_channel_pan(const Ogre::Vector3& at) const;

    /// Start (or stop, for an empty name) a track the data names.
    bool play_music(const std::string& relative, bool loop, const char* because) override;

    /// What is on each channel, so a re-load does not restart the right track.
    std::string music_track;

    /// What a node's `CMusicObject` asks of the sound manager and its level.
    tl::core::MusicHost music_host(int64_t node) override;

    /// The scripted camera, when a `Camera Controller` has taken it: the controller moves
    /// the point looked at, and the camera keeps its offset (as follow_subject()). `follow`
    /// is the node whose position is that focus.
    struct ScriptedCamera {
        bool active = false;
        Ogre::Vector3 position = Ogre::Vector3::ZERO;
        int64_t follow = 0;
        bool have_follow_position = false;
        Ogre::Vector3 follow_position = Ogre::Vector3::ZERO;

        /// The pan over `PAN TIME`.
        Ogre::Vector3 from = Ogre::Vector3::ZERO;
        float elapsed = 0.0f;
        float pan_time = 0.0f;
        float ease_at = 1.0f;
    };
    ScriptedCamera scripted_camera;

    /// `CGameUI +0x178c`, the interactive menus open (`setInteractiveMenuVisible`):
    /// the bars (`CInteractiveMenu`, `+0x570`) are up while above
    /// 0. Counted by `INTERACTIVE UI` and the quest dialog (`CQuestDialogMenu::setOpen`).
    ///
    int interactive_menus = 0;
    void set_interactive_menu_visible(bool on) override;
    /// The player's `+0x70d`, the mesh hidden on purpose (`setMeshVisible`):
    /// `HIDE PLAYER`.
    bool player_mesh_hidden = false;
    /// `CCharacter::setMeshVisible(visible, true)`
    /// on the player.
    void set_player_mesh_visible(bool visible);
    /// State 6, `CLOSE INGAME UI`: `CGameUI::closeLeft`, `closeRight`,
    /// `closeMenus` and `closeAll`.
    void close_ingame_ui() override;
    /// What `CCameraController::startCamera` closes:
    /// `CGameUI::closeAll` and `CGameUI::hideModalDialogs`.
    void close_for_camera();

    /// The dynamic light map (setup_light_map()): `ProjectorRTTLight` (`CGameClient +0x210`),
    /// its `ProjectorCam` (`CCameraControl +0x28`), and the quads `ProjectorLightQuad`
    /// (`+0x220`, `+0x230`) and `ProjectorLightMask` (`+0x228`, `+0x238`), in the scene only
    /// while it renders (`preRenderTargetUpdate`). Torn down before Root.
    struct LightMap {
        Ogre::TexturePtr texture;
        Ogre::Viewport* viewport = nullptr;
        Ogre::Camera* camera = nullptr;
        Ogre::SceneNode* camera_node = nullptr;
        Ogre::SceneNode* quad_node = nullptr;
        std::unique_ptr<Ogre::Rectangle2D> light_quad;
        std::unique_ptr<Ogre::Rectangle2D> light_mask;
        Ogre::MaterialPtr light_material;
        Ogre::MaterialPtr mask_material;
        std::unique_ptr<Ogre::RenderTargetListener> listener;
        /// `CGameClient +0x2a8` and the frame clock `gTorchlightUpdate`; see
        /// step_light_map().
        float flicker_angle = 0.0f;
        float flicker_clock = 0.0f;
    } light_map;

    /// The shadow map (setup_shadow_map()).
    struct ShadowMap {
        Ogre::TexturePtr texture;
        Ogre::Viewport* viewport = nullptr;
        Ogre::Camera* camera = nullptr;
        Ogre::SceneNode* camera_node = nullptr;
        Ogre::SceneNode* quad_node = nullptr;
        std::unique_ptr<Ogre::Rectangle2D> background;
        Ogre::MaterialPtr background_material;
        std::unique_ptr<Ogre::RenderTargetListener> listener;
    } shadow_map;

    /// One placed `CParticle`: its extras, and where it stands from its first system's
    /// holder (ParticleSystem::cparticle).
    struct PlacedParticle {
        const ParticleFileExtras* extras = nullptr;
        std::string reference;
        Ogre::Vector3 from_holder = Ogre::Vector3::ZERO;
        /// On the level's list (`CLevel +0x260`, X360 `+0x1bc`) that `CLevel::findParticles`
        /// fills: start-on-load links with no `NO CULLING`
        /// technique, which `update_visible_particles` shows and hides.
        bool listed = false;
        /// `CSceneNodeObject +0x81` of the link (X360 `+0x6d`): shown.
        bool visible = true;
        /// Placed by `place_particle_layout`: its file, its holders in placement order and each
        /// one's pose in the placement's frame, so its systems can be kept for the next.
        std::string file;
        std::vector<Ogre::SceneNode*> holders;
        std::vector<std::pair<Ogre::Vector3, Ogre::Quaternion>> frames;
    };
    std::map<uint32_t, PlacedParticle> placed_particles;
    /// PORT: a `CParticle`'s systems once it is taken down, stopped and out of the scene, kept
    /// by file for its next placement, as the original's preloader keeps its `CParticleCache`s
    /// (its getParticleCache and returnParticleCache: the last returned is the next taken).
    struct ParkedParticle {
        std::vector<Ogre::SceneNode*> holders;
        std::vector<std::pair<Ogre::Vector3, Ogre::Quaternion>> frames;
        std::vector<ParticleSystem> systems;
    };
    std::map<std::string, std::vector<ParkedParticle>> parked_particles;
    std::unordered_set<const Ogre::SceneNode*> parked_holders;
    /// A kept `CParticle` of `file` put back at `at` (or on `parent`), its systems listed again
    /// and started from `particle_systems`' end; false when none is kept.
    bool unpark_particle(const std::string& file, const Ogre::Vector3& at,
                         const Ogre::Quaternion& facing, bool riding);
    void claim_particle_holders(size_t before, Ogre::SceneNode* parent,
                                std::vector<Ogre::SceneNode*>& holders);
    void remember_parkable(uint32_t id, const std::string& file,
                           const std::vector<Ogre::SceneNode*>& holders, const Ogre::Vector3& at,
                           const Ogre::Quaternion& facing, bool riding);
    std::map<uint32_t, bool> parkable_placements(const std::set<const Ogre::SceneNode*>& going);
    void park_placements(std::map<uint32_t, ParkedParticle>& parking);
    void forget_particle_nodes(const std::set<const Ogre::SceneNode*>& going);
    void destroy_all_particle_systems();

    uint32_t next_placed_particle = 0;
    /// How many placements are `listed`.
    size_t listed_particles = 0;
    /// `updateVisibleParticles`' static `mLastPos`: the camera focus the last
    /// pass was made for.
    Ogre::Vector3 visible_particles_last{-9999.0f, -9999.0f, -999.0f};
    /// `CLevel::updateVisibleParticles(Ogre::Camera*, const Ogre::Vector3&, bool)`:
    /// a listed link is shown while across the screen's width
    /// (half a screen spare), within aspect times 15 of the focus, or paused (CLayout::setVisible).
    void update_visible_particles(bool paused);
    /// Make `particle_systems[first, end)` that `take` accepts one `CParticle` over
    /// `reference` at `origin`; 0 when none was taken.
    uint32_t register_cparticle(const std::string& reference, size_t first,
                                const Ogre::Vector3& origin,
                                const std::function<bool(const ParticleSystem&)>& take);
    /// `CParticle::Start` besides the systems:
    /// `CParticleCache::playSounds` and `playCameraShakes`.
    void cparticle_started(uint32_t id);
    void play_particle_extras(const ParticleFileExtras& extras, const Ogre::Vector3& at,
                              const std::string& reference);
    /// Drop the placements none of whose systems is left.
    void forget_dead_cparticles();
    std::vector<uint32_t> live_cparticles;  ///< forget_dead_cparticles()'s, kept for its storage

    /// Increasing paint order for layout meshes; see place_mesh().
    unsigned layout_draw_order = 1;
    /// Blended passes on alpha-less textures that place_mesh() made solid.
    size_t blends_drawn_solid = 0;
    /// Cut-out meshes place_mesh() queued after the other depth-writing blends.
    size_t cut_outs_drawn_last = 0;
    /// What a texture's alpha holds (`formats::dds_alpha_census`), by name:
    /// 255 throughout or absent, and the share an alpha rejection removes.
    struct TextureAlpha {
        bool opaque = false;
        float clear_share = 0.0f;
    };
    std::unordered_map<std::string, TextureAlpha> texture_alphas;
    const TextureAlpha& texture_alpha(Ogre::TextureUnitState* unit);
    /// The layout meshes given an order of their own below that counter's
    /// ceiling; bake_static_scenery() leaves them out.
    std::set<const Ogre::Entity*> paint_ordered;

    /// Brightness and contrast, drawn over the finished frame.
    DisplayGamma display_gamma;

    /// The in-game minimap and automap system.
    Automap automap;
    bool map_toggle_held = false;

    Ogre::Vector3 camera_offset = Ogre::Vector3::ZERO;
    bool camera_follows = false;

    /// `CCameraControl`'s state across frames (`updateGameCamera`):
    /// smoothed focus (+0x4c), smoothed offset (+0x3c), `gbWasInZoomIn`.
    struct GameCamera {
        Ogre::Vector3 focus = Ogre::Vector3::ZERO;
        Ogre::Vector3 offset = Ogre::Vector3::ZERO;
        bool was_zoomed_in = false;
        bool have_focus = false;
        bool close_up = false;  ///< the state last logged
        TouchCamera touch;
    };
    GameCamera game_camera;
    /// The right stick's zoom, -1 to 1 (`InputState::zoom`), and the dolly (`CCameraControl +0x38`,
    /// X360 `+0x18`), reset each level. `CCameraControl::CCameraControl`
    /// (X360 inline in `CGame::createCamera`): dolly 28.5, the rest zero.
    float camera_zoom_input = 0.0f;
    /// The frame's delta for the camera (`step_player`); 0 for any other re-aim.
    float camera_delta = 0.0f;
    /// The unit talked to in the quest window (target in AI state 0x1f), and the dialog's
    /// `CAMERAMODX/Y/Z` (`CQuestDialog +0x88`).
    std::string talk_unit;
    Ogre::Vector3 talk_camera_mod = Ogre::Vector3::ZERO;
    /// The same for a script's line (`CGameUI::forceInteractiveDialog`):
    /// its target character, 0 for a prop's line.
    int64_t script_talk_node = 0;
    Ogre::Vector3 script_talk_camera_mod = Ogre::Vector3::ZERO;

    /// Whether the loaded layout aimed the camera itself (only the main-menu layouts).
    bool layout_has_camera = false;

    /// Where that authored camera was aimed, in world space.
    Ogre::Vector3 layout_camera_target = Ogre::Vector3::ZERO;

    /// What the follow camera looks at, relative to the subject's feet.
    Ogre::Vector3 camera_look_offset = Ogre::Vector3::ZERO;

    /// The quit input was pressed (`+`, ESC); `run()` ends its loop, and `main.cpp`'s
    /// `play()` returns to the main menu (`Renderer::quit_to_menu_requested()`).
    bool quit_requested = false;

    // ---------------------------------------------------------------- combat --

    /// The left hand's item when `weapon_entity` is the right's (one of
    /// `worn_attachments`), for its weapon trail.
    Ogre::Entity* worn_left_hand = nullptr;
    /// `CCharacter::attack`: the hand
    /// a swing is taken with when one is ranged and the other not.
    void choose_player_attack_hand() override;
    /// The other equipped items `CEquipment::attachToGivenLocation` puts on the character
    /// (core/equipment_attach.h), rebuilt by `sync_worn_attachments`.
    std::vector<Ogre::Entity*> worn_attachments;

    /// The hand nodes, `CCharacter +0x2e8` (right) and `+0x2f8` (left): children of the
    /// unit's node scaled by `WEAPON_SCALE` (`CCharacter::getBones`),
    /// kept on `tag_righthand` / `tag_lefthand` by `updateBonePositions`.
    /// Made when a weapon first wants one.
    Ogre::SceneNode* hand_nodes[2] = {nullptr, nullptr};
    int weapon_element_hand = 0;
    /// `CEquipment::createParticles` + `attachToGivenLocation`
    /// for the weapon just put in a hand.
    void attach_weapon_element(bool right);

    /// The shadow and light maps brought up to date for a frame dump.
    void update_light_maps(const std::string& png_path);

    int death_selected = 0;

    /// The lowest speed of the slow-down under way, for the log.
    float game_speed_lowest = 1.0f;

    /// The attack button held with the stick at rest for the whole scene
    /// (`step_attack_button`), to verify a fight headlessly (Renderer::set_auto_attack()).
    bool auto_attack = false;

    /// `--pad`: step the console's target selector in a headless run.
    bool headless_targeting = false;

    /// What the console's selector picked and last frame's, so a change is logged once.

    std::string pad_selected;

    bool pad_selected_hostile = false;

    int64_t pad_selected_node = 0;

    /// The entity lit up and whether as hostile; checked against the scene before clearing.

    Ogre::Entity* pad_highlighted = nullptr;

    bool pad_highlighted_hostile = false;

    /// The second pass's other pick: a `Unit Trigger` by machine and node
    /// (`trigger_targeted_machine` `kNoMachine` for none).
    int trigger_targeted_machine = -2;
    int64_t trigger_targeted_node = 0;
    void interact_targeted_trigger();
    /// The selection came from the console's second pick (`+0x1c4`).
    bool item_second_pick = false;
    /// The target item as A sets it (`CCharacter::setTargetItem`, `+0x350`, AI state 7):
    /// what `getItemAI` walks to and uses (`step_item_ai`).
    struct PlayerItemTarget {
        /// `Mimic`: a sleeping mimic as the target (`+0x340`) for `attackAI`.
        enum class Kind { None, Dropped, Trigger, Portal, Mimic };
        Kind kind = Kind::None;
        Ogre::SceneNode* dropped = nullptr;  ///< the floor item's node
        int machine = kNoMachine;            ///< a `Unit Trigger`'s (or mimic's) machine and node
        int64_t node = 0;
        std::string portal;  ///< an open portal's unit
        Ogre::Vector3 portal_at = Ogre::Vector3::ZERO;
    };
    PlayerItemTarget player_item;
    /// A on the current item selection: `setTargetItem` with what it points at.
    void target_selected_item();
    /// `CCharacter::getItemAI` for the player, each frame.
    void step_item_ai(float delta);
    /// `CCharacter::attackAI` on a sleeping mimic.
    void step_mimic_attack(float delta);
    /// `CGameUI::updateIngameUI` for AI state 0x16.
    void open_enchant_shrine(int machine, int64_t node);
    /// The prop a trigger draws, if it draws one.
    Ogre::Entity* trigger_entity(int64_t node, const std::string& unit, const NodeMap& nodes) const;

    float fishing_animation_rate = 1.0f;
    float fishing_animation_clock = 0.0f;
    /// `CPlayer +0x828`, `+0x830`, `+0x840`/`+0x848`: the rod on the right
    /// hand, the fish on the hook, the line.
    Ogre::Entity* fishing_rod = nullptr;
    Ogre::Entity* hooked_fish = nullptr;
    Ogre::AnimationState* hooked_fish_idle = nullptr;
    Ogre::ManualObject* fishing_line = nullptr;
    Ogre::SceneNode* fishing_line_node = nullptr;
    /// Where the hole is: the lure never sinks below it (`updateFishingLine`).
    Ogre::Vector3 fishing_hole_at = Ogre::Vector3::ZERO;
    /// `CPlayer::startFishing` at the hole
    /// `at`.
    void start_fishing(const Ogre::Vector3& at);
    /// `setAIState(2)` out of any fishing state (`CPlayer::setAIState` hides the rod).
    void stop_fishing() override;
    /// Play a fishing animation (`blendAnimation` / `queueBlendAnimation`).
    void play_fishing_animation(const std::string& name, float rate, bool once,
                                const std::string& then) override;
    /// `animationPlayingSubstring` for the one-shot fishing animation.
    bool fishing_animation_playing(const std::string& name) const override;
    /// `CPlayer::catchFish`.
    void catch_fish() override;
    /// `CPlayer::updateAnimation`'s fishing half and
    /// `updateFishingLine`: rod, line and hooked fish.
    void update_fishing_gear();
    void build_fishing_gear();
    void destroy_fishing_gear();
    /// The ring's size and colour (`CFishingMenu::update`).
    ///
    void refresh_fishing_screen();
    /// `CGameUI::openModalDialog`.
    void open_modal(const std::string& title, const std::string& text) override;

    /// A standing unit's fidgets: `CMonster::idleAI` under `animationExistsSubstring("FIDGET")`:
    /// while idle, `randomIntegerBetweenVolatile(0, 1000) <= 9` per AI tick, not within
    /// `kFidgetCooldown` (`+0x284`), one `findRandomAnimation("FIDGET")` played once with
    /// `IDLE` queued. DIVERGENCE (frame rate): the per-tick roll is a rate per second at
    /// 60 Hz, applied as `1 - exp(-rate * delta)`.
    struct Fidget {
        /// Seconds since this unit last fidgeted - the original's `+0x284`.
        float since = 0.0f;
        /// The state a fidget is playing on, or null when none is.
        Ogre::AnimationState* state = nullptr;
        /// What to go back to when it runs out.
        std::string back_to;
        /// What is playing, so a scene unit can be put back on its own idle.
        std::string playing;
        /// A scene unit's idle queued behind the fidget (`queueBlendAnimation("IDLE", true,
        /// 0.2, 1)`), taking over 0.2 s before it ends.
        Ogre::AnimationState* queued = nullptr;
        /// The mesh's `.SKELETON`s named "fidget", asked once; empty until then.
        std::vector<std::string> available;
        bool looked_up = false;
    };

    /// Keyed by entity, and cleared whenever an entity is torn down (`clear_subject()`).
    std::map<Ogre::Entity*, Fidget> fidgets;

    /// Link one `.SKELETON` beside the character's mesh onto it and what it wears
    /// (Renderer::link_animation()).
    bool link_subject_animation(const std::string& name) override;

    /// Play one of them, exclusively, on the character and what it wears
    /// (Renderer::apply_animation()); `quiet` for the per-frame caller.
    bool apply_subject_animation(const std::string& name, float time, bool quiet);

    /// Put the character on whatever wanted_player_animation() asks for.
    void follow_player_animation() override;

    bool scripted_player_loops = false;

    /// `die`'s branches before the fall: the burst or the death flight.
    /// True when one took the death (no `DIE` animation).
    bool die_burst_or_flight(const tl::core::ScriptEvent& event, SceneUnit& unit, NodeMap& nodes);

    /// Destroy the subject and everything hanging off it.
    void clear_subject();

    bool equip_starting_weapon();
    /// `location` is where the inventory holds it (0 the right hand, 1 the
    /// left); -1 puts a bow in the left and anything else in the right.
    bool equip_weapon_item(const tl::core::ItemStats& item, int location = -1) override;
    /// Hang `right` and `left` (item names, empty for none) on a unit's
    /// `tag_righthand`/`tag_lefthand` bones (its `[EQUIPMENT]`).
    void equip_unit_items(Ogre::Entity* entity, const std::string& unit, const std::string& right,
                          const std::string& left);
    /// Hang on each drawn character the hands its roll gave it, once made.
    void sync_unit_equipment();
    void unequip_weapon() override;
    /// `CEquipment::equipped` for every location but the held weapon's.
    void sync_worn_attachments(int weapon_location) override;
    void clear_worn_attachments();

    /// The console's floor labels for the frame: landed items and usable
    /// `Unit Trigger`s on screen (core/item_label.h), handed to the HUD.
    void step_ground_labels();

    /// `CCharacter::unitThemeUpdated` on a
    /// drawn unit: its theme particles made, placed and started, or stopped.
    void unit_theme_updated(UnitThemeState& state, Ogre::Entity* entity, uint32_t unit_type,
                            const tl::core::UnitTheme* theme, bool removed);
    /// What `ThemeParticleTriggers` asks of a drawn unit; `light_map` marks a theme's
    /// particles.
    tl::core::ThemeParticleUnit trigger_unit(UnitThemeState& state, Ogre::Entity* entity,
                                             uint32_t unit_type, const std::string& label,
                                             bool light_map);
    /// `configureParticleTrigger`, the node
    /// half, for the triggers just made, and their offsets (+0x28).
    void configure_trigger_nodes(UnitThemeState& state, Ogre::Entity* entity);
    /// `CCharacter::loadModel`, the triggers
    /// of the model's base skeleton, once per drawn unit.
    void load_model_triggers(UnitThemeState& state, Ogre::Entity* entity, const std::string& mesh,
                             uint32_t unit_type);
    /// `CParticle::Start` / `Stop` / the park for steps `setVisible`
    /// and `die` hand back.
    bool cparticle_looping(const UnitThemeState& state, int particle) const;
    void apply_trigger_steps(UnitThemeState& state,
                             const std::vector<tl::core::ThemeParticleStep>& steps);
    /// `CCharacter::updateBonePositions`, the
    /// triggers' half.
    void update_theme_particle_positions(UnitThemeState& state, Ogre::Entity* entity);
    /// Each frame: `CBaseUnit::update`'s theme half for the player and every
    /// placed unit, then their theme particles moved onto their bones.
    void step_unit_themes();
    /// What step_unit_themes() reads of the machines a frame: each placed unit's effect
    /// manager's themes, and the machines that can answer for a unit.
    struct UnitThemeSources {
        std::map<const SceneUnit*, const std::vector<const tl::core::UnitTheme*>*> effect_themes;
        std::vector<const tl::core::LevelScript*> machines;
    };
    UnitThemeSources unit_theme_sources();
    /// `CLevel::updateNPCIcons` over the
    /// placed units.
    void update_npc_icons() override;
    /// `CGameUI::getUIIsInCinematic`, the
    /// part the port has.
    bool ui_in_cinematic() const override;
    /// Take a unit's theme particles down and forget its themes.
    void clear_unit_themes(UnitThemeState& state);

    /// `CLevel::findItemWithinView` from the player: the
    /// first breakable within `max_angle` and `max_distance` in clear sight, or -1.
    int breakable_within_view(float max_angle, float max_distance) const override;

    /// The point `CLevel::update` runs the items from: a
    /// scripted camera's, else the player's.
    ogre::Vector3 item_view_point() const override;
    /// `Camera::isVisible` on the item's culling bounds (`+0x1c0`).
    bool item_in_camera(Ogre::SceneNode* node, bool from_model = false) const override;
    /// A step's changes drawn on the item's node.
    void draw_item_presence(Ogre::SceneNode* node, const tl::core::ItemPresence& p,
                            const tl::core::ItemPresenceChange& change) override;
    /// `CLevel::updateItems` over the near list: each
    /// `update` (`CBreakable::update`, floor items,
    /// `CTriggerUnit::update`); out-of-range broken ones deleted.
    // PORT: drawing, what the item code asks of the scene (renderer_items.cpp).
    void view_restart_trigger_layout(int64_t node, const NodeMap* nodes) override;
    Ogre::SceneNode* view_place_entity(Ogre::Entity* entity, const Ogre::Vector3& at,
                                       const Ogre::Quaternion& turn) const;
    void view_set_scale(Ogre::SceneNode* node, float scale) override;
    void view_set_visible(const std::vector<Ogre::SceneNode*>& holders, bool on) override;
    void view_destroy_entity(Ogre::Entity* entity) override;
    ogre::Vector3 view_landed_size(Ogre::SceneNode* node, Ogre::Entity* entity) override;
    std::string view_entity_mesh(const Ogre::Entity* entity) const override;
    void view_hide_death_screen() override;
    void view_show_minimap() override;

    /// The merchant whose screen is open.
    int64_t trading_with = 0;
    /// X on a vendor's screen: buy from the merchant's tray,
    /// sell from the bag.
    void trade_selected();

    /// A on the bag on an enchanter's or a shrine's screen;
    /// true when the press is the tray's own A instead (an unidentified item).
    bool enchant_selected();
    /// A on the bag on a gem service's page: (mode 2, gems
    /// back, item destroyed) or (mode 3, gems destroyed).
    void gem_service_selected();
    /// A on the retirement page (mode 4): the item becomes the heirloom and
    /// the character retires.
    void retire_selected();
    /// `CGameClient +0x3900` / `+0x3908`: the heirloom for the next new character, taken by
    /// `Renderer::take_heirloom` before the rebuild.
    tl::core::HeirloomHandOff heirloom_handoff;
    /// The retirement's `requestSetGameState(0, 2)`: New Game in retirement mode.
    bool retirement_requested = false;
    /// A on a gem in the bag: the item-select
    /// dialog, for a player with a pet. Returns false when it did not open.
    bool open_gem_socketing() override;
    /// The dialog's input (`CMessageItemSelectXenon::vfunc_2`); on closing the
    /// tray's poll sets the gem into the chosen item.
    void handle_item_select(const tl::core::InputState& in);
    void socket_selected_gem();
    bool item_select_stick_y_held = false;
    bool item_select_stick_x_held = false;
    /// Start on the inventory tab (code 0xd): the pet commands
    /// dialog (`core/pet_commands.h`). False when it did not open.
    bool open_pet_commands();
    /// The dialog's codes this frame: Y (8), Start (0xd), Back (0xe), and `other` (codes 1..4
    /// and 9..12, which close it).
    struct PetCommandEdges {
        bool y = false;
        bool start = false;
        bool back = false;
        bool other = false;
    };
    /// The dialog's input while it is up (`CMessagePetCommandsXenon::vfunc_2`).
    ///
    void handle_pet_commands(const tl::core::InputState& in, const PetCommandEdges& edges);
    /// `vfunc_5` for the pet as it is now.
    void refresh_pet_commands();
    bool pet_commands_stick_x_held = false;
    /// X on a Transmuter's or a stash's screen: bag to their slots, their
    /// tray back.
    void swap_selected();
    /// X on the bag's screen: the stack to the other tray
    /// (`core::move_to_inventory`); into the pet's only when allows.
    void swap_bag_and_pet();
    /// Y on the pet's tray: the stack onto the level at the pet's feet.
    void drop_selected_pet_item();
    /// The inventory the vendor tray shows on a Transmuter's or a stash's
    /// page, which X moves into and out of; null on any other.
    tl::core::Inventory* housing_inventory_for(VendorScreen screen);
    tl::core::Inventory* housing_inventory_of(VendorScreen screen);

    /// The item's own mesh as an entity, its materials bound; null when it names none.
    Ogre::Entity* create_item_entity(const tl::core::ItemStats& item) override;

    /// A unit as a quest event sees it (`questEventFire`'s third argument): its
    /// name, its `UNITTYPE` for `ISA`, where it stands and what it carries.
    tl::core::QuestEventUnit quest_event_unit(const std::string& name, const std::string& unittype,
                                              const Ogre::Vector3& at,
                                              const tl::core::QuestUnitTag& tag);
    void bag_item_added(tl::core::ItemStats& item) override;
    void bag_item_removed(const tl::core::ItemStats& item) override;
    /// Where an event's item is: where it lay (`bag_event_at`), else the player.
    Ogre::Vector3 bag_event_position() const;
    bool bag_event_at_set = false;
    Ogre::Vector3 bag_event_at = Ogre::Vector3::ZERO;
    /// Carry out what completed quests take back (`QuestLog::take_item_removals`):
    /// the player's bag, then the pet's.
    void apply_quest_item_removals() override;
    /// A pickup or a kill reported to the quests, and the banner for each one it finished.
    void quest_event(const std::vector<const tl::core::QuestDefinition*>& finished) override;
    /// `CCharacter::getItemAI`'s targeting half: which
    /// `dropped_items` entry is in reach; sets `item_targeted`/`item_highlighted` only.
    bool update_item_selection(bool active);
    /// The other half of `getItemAI`/`getItem`: takes whatever
    /// `update_item_selection` is currently pointing at.
    void pick_up_targeted_item();

    /// Release the particle systems the character always carries: the manifest's keys on
    /// the base skeleton (e.g. the gauntlet glow). Animation `SPAWNPARTICLE` keys are the
    /// swing's.
    size_t equip_carried_particles();

    /// A `SPAWNPARTICLE` key on a player animation: a `CParticleAnimationTrigger`, made at
    /// model load in the original (`CCharacter::loadModel`,
    /// `configureParticleTrigger`), here on first firing.
    struct ParticleTrigger {
        std::string animation;
        size_t tag = 0;
        std::string bone;
        Ogre::Vector3 offset = Ogre::Vector3::ZERO;
        bool attaches = true;
        /// `SPAWNPARTICLE_STOP_ON_DEATH` (+0x20).
        bool stop_on_death = false;
        /// The key's `UNITTHEME` (+0x50), upper-cased; empty for none.
        std::string unit_theme;
        /// The node that follows the bone (+0x38), for a trigger that attaches.
        Ogre::SceneNode* follower = nullptr;
        /// What the layout placed, and where each holder sits inside it.
        std::vector<Ogre::SceneNode*> holders;
        std::vector<Ogre::Vector3> inside;
    };
    std::vector<ParticleTrigger> particle_triggers;
    /// The player's unit themes, upper-cased (`CCharacter::unitThemeUpdated`).
    ///
    std::set<std::string> player_theme_names;

    /// A `SPAWNPARTICLE` key fired: `CCharacter::updateAnimation`,
    /// keyframe types 4 and 5.
    void fire_particle_trigger(const std::string& animation, size_t index,
                               const tl::core::AnimationTag& tag) override;
    /// Keep every attached trigger's node on its bone: `CCharacter::
    /// updateBonePositions`.
    void update_particle_triggers();
    void clear_particle_triggers();

    /// The same on a placed unit: `CCharacter::updateAnimation`
    /// types 4, 5, 0xc (`REMOVEPARTICLES`) and 0xd (`REMOVEANIMATIONPARTICLES`).
    struct UnitParticleTrigger {
        Ogre::Entity* entity = nullptr;
        std::string layout;  ///< the key's `LAYOUT`, upper-cased, slashes forward
        ParticleTrigger trigger;
    };
    std::vector<UnitParticleTrigger> unit_particle_triggers;
    /// Every placed unit's particle keys this frame, and attached triggers on their bones
    /// (`updateBonePositions`).
    void step_unit_particle_keys();

    /// A placed unit's weapon element: `CEquipment::createParticles`
    /// under the hand's node (`attachToGivenLocation`),
    /// kept on the bone by `updateBonePositions`.
    struct UnitWeaponElement {
        Ogre::Entity* entity = nullptr;
        std::string bone;
        Ogre::SceneNode* hand = nullptr;
        std::vector<Ogre::SceneNode*> holders;
    };
    std::vector<UnitWeaponElement> unit_weapon_elements;

    // ---------------------------------------------------------- weapon trails --

    /// An `Ogre::Node` as `CWeaponTrail::update` asks of it:
    /// a weapon's tag point, or a skeleton bone.
    struct OgreTrailNode : tl::core::WeaponTrailNode {
        Ogre::Node* node = nullptr;
        tl::core::Vec3 derived_position() override {
            const Ogre::Vector3 p = node->_getDerivedPosition();
            return {p.x, p.y, p.z};
        }
        tl::core::Vec3 position() override {
            const Ogre::Vector3 p = node->getPosition();
            return {p.x, p.y, p.z};
        }
        void translate_local(const tl::core::Vec3& d) override {
            node->translate(Ogre::Vector3(d.x, d.y, d.z), Ogre::Node::TS_LOCAL);
        }
        void set_position(const tl::core::Vec3& p) override {
            node->setPosition(Ogre::Vector3(p.x, p.y, p.z));
        }
    };
    /// A character's `CWeaponTrail`s (`CCharacter +0x698..+0x6a8`) and the `ManualObject` each
    /// draws into (`CWeaponTrail +0x28`, under its own root node, `+0x40`).
    struct DrawnTrails {
        tl::core::CharacterWeaponTrails trails;
        OgreTrailNode hands[2];  ///< the tag points of slots 0 and 1's items
        OgreTrailNode bones[2];  ///< a key's bone, for the right and left trails
        struct Drawn {
            Ogre::ManualObject* object = nullptr;
            Ogre::SceneNode* node = nullptr;
            std::string material;
        };
        Drawn drawn[3];  ///< right, left, tracer
    };
    DrawnTrails player_trails;
    /// A placed unit's, by its entity; made by its first trail key.
    std::map<Ogre::Entity*, DrawnTrails> unit_trails;

    /// The character as `CCharacter`'s trail code reads it.
    tl::core::TrailCharacter player_trail_character();
    tl::core::TrailCharacter unit_trail_character(Ogre::Entity* entity, DrawnTrails& drawn,
                                                  bool good, int64_t node = 0);
    /// `updateAnimation`, keys 7 and 8, for
    /// `tag` crossed on `entity`'s animation.
    void weapon_trail_key(DrawnTrails& drawn, Ogre::Entity* entity,
                          const tl::core::AnimationTag& tag, const tl::core::TrailCharacter& who);
    /// The frame's `CWeaponTrail::update`s, before the frame's keys.
    void step_weapon_trails(float player_dt, float world_dt);
    /// Each trail's strip onto its `ManualObject`, once the keys are done.
    void draw_weapon_trails();
    void draw_character_trails(DrawnTrails& drawn);
    void destroy_character_trails(DrawnTrails& drawn);
    /// A placed unit's trails and held items, when its entity goes.
    void forget_unit_trails(Ogre::Entity* entity);
    /// `CGame::createMaterials`'s four trail materials, or a
    /// key's `LAYOUT` made into one (`updateAnimation`), by name.
    void ensure_trail_material(const std::string& name);
    void attach_unit_weapon_element(Ogre::Entity* entity, const tl::core::ItemStats& item,
                                    const std::string& bone, float scale);
    /// `entity`'s, or every one whose unit has left the scene when null.
    void clear_unit_weapon_elements(Ogre::Entity* entity);

    // PORT: drawing, what the swing, a death and a breakable ask of the scene
    // (renderer_combat.cpp).
    float view_animation_length(Ogre::Entity* entity, const std::string& name) const override;
    void player_weapon_trail_key(const tl::core::AnimationTag& tag) override;
    void player_shoot_tracer(const tl::core::Vec3& from, const tl::core::Vec3& to) override;
    void player_hide_hand_trails() override;
    bool view_unit_falls(int64_t node) override;
    bool view_ready() const override;
    void view_cast_shadows(Ogre::Entity* entity, bool on) override;
    void view_add_node_collision(Ogre::SceneNode* node, const std::string& path,
                                 int64_t group) override;

    /// Advance every hunting monster's swing and land those.
    void step_monster_swings(float delta);

    /// A unit's animations in its manifest's order with their lengths; see
    /// `LevelScript::set_animation_catalog`.
    std::vector<std::pair<std::string, float>> animation_catalog(const std::string& unit) override;
    std::map<std::string, std::vector<std::pair<std::string, float>>> animation_catalogs;

    bool save_character(const std::string& path, bool force) override;
    void step_player_death(float delta);
    void open_death_screen();
    void refresh_death_screen();

    /// How long the last load_layout() took, in seconds.
    float load_seconds = 0.0f;

    tl::core::RandomTrace rng_trace;
    /// Takes the trace off `g_Rand`, which outlives this renderer.
    struct RngTraceGuard {
        ~RngTraceGuard() { tl::core::game_random().set_trace(nullptr); }
    } rng_trace_guard;

    /// `CWardrobe` (core/wardrobe.h): the class's outfit (`wear_base_outfit`), each slot's
    /// entity, and what the slots last held; `wardrobe_ready` once the outfit is read.
    tl::core::WardrobeBase wardrobe_base;
    std::string wardrobe_class;
    std::string wardrobe_stem;
    bool wardrobe_ready = false;
    std::array<Ogre::Entity*, tl::core::kWardrobeSlotCount> worn_pieces{};
    std::string worn_signature;

    /// The main menu's debug wardrobe editor (render/renderer_wardrobe.cpp), the engine's
    /// own tool. A look is one way a slot can be drawn, with its first item and a count.
    struct WardrobeLook {
        const tl::core::ItemStats* item = nullptr;  ///< null: the class's own / an empty hand
        std::string mesh;
        std::string texture;
        int sharing = 1;
    };
    /// One row: a wardrobe slot (`tl::core::WardrobeSlot`), or -1 for the weapon.
    struct WardrobeRow {
        std::string label;
        int slot = -1;
        std::vector<WardrobeLook> looks;
        int index = 0;
    };
    struct WardrobeEditor {
        bool open = false;
        std::vector<WardrobeRow> rows;
        /// What the menu had before, put back on the way out.
        std::string previous_class;
        tl::core::Inventory saved_inventory;
        bool had_weapon = false;
        tl::core::ItemStats saved_weapon;
        Ogre::Vector3 camera_position = Ogre::Vector3::ZERO;
        Ogre::Quaternion camera_orientation = Ogre::Quaternion::IDENTITY;
        Ogre::Quaternion subject_orientation = Ogre::Quaternion::IDENTITY;
        /// The camera's distance from the character, as a share of the
        /// backdrop's own (1 = where the menu put it).
        float zoom = 1.0f;
        std::string dress_class = "Alchemist";  ///< the class it dresses
    };
    WardrobeEditor wardrobe_editor;

    /// The main menu's debug bestiary (render/renderer_bestiary.cpp), the engine's own tool:
    /// every unit under `media/units/monsters/`, placed in turn where the menu's pet stands.
    struct BestiaryUnit {
        std::string name;    ///< `NAME`, what `spawn_unit()` takes
        std::string file;    ///< its `.DAT.adm`
        std::string folder;  ///< the file's directory under `media/units/monsters/`
    };
    struct Bestiary {
        bool open = false;
        /// Ordered by file path, so a folder's units are together.
        std::vector<BestiaryUnit> units;
        /// Each folder's first index into `units`, in order.
        std::vector<int> folders;
        int unit = 0;
        /// The `.SKELETON` stems beside the unit's mesh other than its own
        /// skeleton; the screen's 0 is the unit's idle as placed.
        std::vector<std::string> animations;
        int animation = 0;
        /// What stands there now: null when the unit could not be placed, and
        /// `status` says why.
        Ogre::Entity* entity = nullptr;
        int64_t id = 0;
        std::string mesh;
        std::string status;
        /// The spot, and the turn the right stick has added to its facing.
        Ogre::Vector3 where = Ogre::Vector3::ZERO;
        Ogre::Quaternion facing = Ogre::Quaternion::IDENTITY;
        float turn_degrees = 0.0f;
        /// The camera's distance as a share of the one that frames the whole
        /// unit (1).
        float zoom = 1.0f;
        /// What the menu had before, put back on the way out.
        Ogre::Vector3 camera_position = Ogre::Vector3::ZERO;
        Ogre::Quaternion camera_orientation = Ogre::Quaternion::IDENTITY;
        std::vector<Ogre::SceneNode*> hidden_pets;
    };
    Bestiary bestiary;
    void place_bestiary_unit();
    void play_bestiary_animation();
    void place_bestiary_camera();
    std::string bestiary_text() const;
    void refresh_bestiary_screen();
    std::vector<WardrobeLook> wardrobe_armour_looks(int slot);
    std::vector<WardrobeLook> wardrobe_weapon_looks();
    int wardrobe_weapon_location(const tl::core::ItemStats& item);
    void wear_wardrobe_row(const WardrobeRow& row);
    std::string wardrobe_editor_text() const;
    void redress_wardrobe_editor();
    void place_wardrobe_camera();
    /// Load a wardrobe mesh onto the subject (materials, skeleton, linked animations), pushed
    /// on `equipment`; null when it does not load.
    Ogre::Entity* attach_worn_mesh(const std::string& mesh_path);
    /// `set_render_behind()` on the subject and its wardrobe pieces (on its node, not its
    /// bones, so the entity's own walk misses them).
    void set_subject_render_behind(bool on, bool hostile);
    /// `CWardrobe::update(CInventory*)` for the player: five
    /// slots from outfit and worn items, the skin composited, each slot's submeshes; nothing
    /// when the slots are unchanged, unless `force`.
    void dress_player(bool force = false) override;

    /// Archive paths of the animation skeletons linked so far, for pieces equipped later.
    std::vector<std::string> linked_paths;

    /// Re-parse the subject's material scripts even if cached: set only around a creation
    /// class swap, as every class declares `Material_#1/<slot>` (g_parse_counts).
    bool force_subject_materials = false;

    // -------------------------------------------------------------- the pet --

    /// Last frame's town flag and health, for the hide and the low-health line.
    bool pet_was_in_town = false;
    float pet_last_health = 0.0f;

    /// The pet has been reported missing (once).
    bool pet_reported = false;

    /// Put the chosen animal beside the character, a character of the level script's; false
    /// when the unit or mesh is missing.
    bool spawn_pet(const std::string& unit);

    /// Stand the animal beside the character, wherever it now is (`place_player_at()`).
    void place_pet_beside_player() override;

    /// Take a unit `spawn_unit()` placed out of every list and destroy its model, held items,
    /// node and particles; `id` from `spawn_unit()` (0: unknown). The script is not told.
    void destroy_spawned_unit(Ogre::Entity* entity, int64_t id) override;
    /// Every per-entity list `place_mesh()` and `spawn_unit()` used, emptied of the entity.
    void forget_unit_entity(Ogre::Entity* entity);
    /// The scene's half of `CCharacter::convertCharacter`:
    /// `deleteUnitSpecificData`, `unitInit`'s model for
    /// `unit` on the same node, then the `TRANSFORM` particle.
    void convert_unit_body(tl::core::LevelScript* machine, int64_t node, const std::string& unit);

    /// One frame of the pet: script mirror, the town hide, the voices and the icon.
    void step_pet();

    /// Fire the trigger tags this frame's step of `entity`'s animation passed (footfalls,
    /// swings, barks); `unit` supplies a bare tag's sound (`UnitStats::attack_sound`).
    void fire_animation_sounds(const std::string& mesh, const std::string& animation,
                               const tl::core::UnitStats& unit, float was, float now,
                               const Ogre::Vector3& at);

    /// Play the trigger tags every animated unit crossed this frame.
    void step_unit_sounds();

    /// The pets that stood on the backdrop's `Point of Interest`, by unit name: kept hidden,
    /// not destroyed, as `scene_units`, `scene_animations` and `spawned` hold them.
    std::map<std::string, Ogre::Entity*> menu_pets;

    /// Where a main-menu backdrop's `Point of Interest` stands the pet and its turn; false
    /// without one.
    bool menu_pet_spot(Ogre::Vector3& where, Ogre::Quaternion& facing);

    /// The animation enabled and where it was parked, for equip() to match.
    std::string playing;
    float playing_time = 0.0f;

    Ogre::RTShader::ShaderGenerator* shader_generator = nullptr;
    std::unique_ptr<ShaderResolver> shader_resolver;
    GpuSkinning gpu_skinning;
    /// The GPU's own time over a sampled frame and its passes, for the fps line.
    GpuTimer gpu_timer;
    /// The light map's and the shadow map's render targets, or null.
    Ogre::RenderTarget* light_map_target() const {
        return light_map.texture ? light_map.texture->getBuffer()->getRenderTarget() : nullptr;
    }
    Ogre::RenderTarget* shadow_map_target() const {
        return shadow_map.texture ? shadow_map.texture->getBuffer()->getRenderTarget() : nullptr;
    }
    /// The characters' render-behind outline (set_render_behind()).
    std::unique_ptr<renderer_detail::RenderBehindMaterials> render_behind_materials;

    // Character preview for console inventory
    Ogre::TexturePtr preview_texture;
    Ogre::Camera* preview_camera = nullptr;
    Ogre::SceneNode* preview_camera_node = nullptr;
    Ogre::SceneNode* preview_node = nullptr;
    Ogre::Light* preview_light = nullptr;
    Ogre::SceneNode* preview_light_node = nullptr;
    Ogre::Entity* preview_subject = nullptr;
    Ogre::Entity* preview_weapon = nullptr;
    std::vector<Ogre::Entity*> preview_equipment;
    float preview_idle_time = 0.0f;

    void init_character_preview();
    void update_character_preview(float delta) override;
    void shutdown_character_preview();
    void update_menu() override;
    /// The Stats column (`CStatsXenon`): pending points, the selection.
    tl::core::StatsPage stats_page;
    /// `CStatsMenuXenon::vfunc_7` (`setOpen`): opens with the Stats tab,
    /// placing its pending points when it closes.
    void sync_stats_page();

    bool mount_resources();
    void setup_scene();
    void advance(float delta);
    using World::place_layout;
    // PORT: place_layout in OGRE's maths, for the renderer's callers.
    bool place_layout(const std::string& reference, const Ogre::Vector3& origin,
                      const Ogre::Quaternion& basis, int depth, uint32_t load_seed_arg = 0) {
        return place_layout(reference, from_ogre(origin), from_ogre(basis), depth, load_seed_arg);
    }

    using World::add_collision_mesh;
    // PORT: add_collision_mesh with OGRE's matrix, for the renderer's callers.
    void add_collision_mesh(const std::string& path, const Ogre::Matrix4& to_world,
                            int64_t group = 0) {
        ogre::Matrix4 m;
        for (size_t r = 0; r < 4; ++r)
            for (size_t c = 0; c < 4; ++c) m[r][c] = to_world[r][c];
        add_collision_mesh(path, m, group, nullptr);
    }

    /// `CGameUI::addTextEvent`: nothing with "Show Damage
    /// Info" off or out of the camera's sight.
    void add_text_event(const tl::core::TextEvent& event) override;
    /// One of a character's own particles (`CCharacter +0x4a8`) put at `at`
    /// and started. Nothing where the slot is null.
    void start_character_particle(const tl::core::UnitStats& unit, int slot,
                                  const Ogre::Vector3& at, CharacterRef who);
    /// The slot particles whose holders are among `going` forgotten (they are destroyed).
    void forget_slot_particles(const std::set<const Ogre::SceneNode*>& going);
    /// `CEffect::playFX`: each asking effect's `PARTICLE_FX` 0.1 above `centre`,
    /// started. `who` is for the log.
    void play_effect_fx(tl::core::EffectManager& effects, const Ogre::Vector3& centre,
                        const std::string& who);
    void step_floaties(float delta);
    void step_pending_talk();

    /// The HUD and the minimap, out of the way of a scene or a conversation.
    void step_hud_visibility();
    bool hud_hidden_for_scene = false;
    bool controller_connected = false;
    bool hud_hidden_game_hud = false;
    bool hud_hidden_automap = false;

    /// Write the frame just drawn to the file a pad script's `shot=` asked for,
    /// if it asked (`tl::core::take_scripted_shot`).
    void save_scripted_shot();
    /// Beside a shot: the material, technique and passes each dropped item in
    /// view is drawn with.
    void log_dropped_item_materials();
    /// Beside a shot: each unit in view, its height over the ground and what
    /// it is playing.
    void log_shot_units();

    /// What the buttons act on: the unit the Xbox build's target selector picks.
    ///
    bool selected_unit(std::string& unit, bool& hostile, int64_t& node,
                       Ogre::Entity*& picked_entity);

    /// Run that pick for this frame, remember it and log it when it changes.
    /// `active` false clears it.
    bool update_pad_selection(bool active);

    /// Where a placed unit stands, by lower-cased substring (`ratlin`).
    bool unit_position(const std::string& name, Ogre::Vector3& at, std::string& found) const;

    bool path_blocker_of(const tl::core::LevelScript::NodeRecord& n, PathBlocker& b) const override;
    bool path_blocker_of(const tl::formats::CmpNode& node, PathBlocker& b) const override;

    /// Put one unit into the scene at a point, remembered under `owner`; null when the unit or
    /// its mesh is missing. `level`: the rolled monster level (0: none, `character_level`).
    /// `champion`: resolved through `CHAMPIONSPAWNCLASS`, given `CCharacter::makeChampion`'s
    /// stats (`apply_champion_curves()`).
    using World::spawn_unit;
    // PORT: spawn_unit in OGRE's maths, for the renderer's callers.
    Ogre::Entity* spawn_unit(const std::string& unit, const Ogre::Vector3& at, int64_t owner,
                             int level = 0, bool champion = false, bool spawn_anim = false,
                             const Ogre::Quaternion& facing = Ogre::Quaternion::IDENTITY) {
        return World::spawn_unit(unit, from_ogre(at), owner, level, champion, spawn_anim,
                                 from_ogre(facing));
    }
    Ogre::Entity* view_spawn_unit_model(const std::string& unit, const std::string& mesh_path,
                                        int64_t id, const ogre::Vector3& at,
                                        const ogre::Quaternion& facing, float placed_scale,
                                        bool spawn_anim) override;
    /// The node of a unit the script made takes its drawn scale (`LevelScript::unit_scale`).
    void apply_unit_scale(int64_t node);

    /// Play a released unit's entrance, if it has one and the spawner asked
    /// (`UnitStats::spawn_anim`); whether anything started.
    bool begin_spawn_animation(Ogre::Entity* entity, const std::string& mesh,
                               const tl::core::UnitStats& stats);

    /// A `Unit Spawner` as the level sees it (core/unit_spawner.h): the renderer's half of
    /// `CUnitSpawner` for one spawner; its outputs go back to its machine.
    class SpawnerHost : public tl::core::UnitSpawnerHost {
    public:
        SpawnerHost(Impl& impl, tl::core::LevelScript* machine, int64_t node)
            : impl_(impl), machine_(machine), node_(node) {}
        int32_t level_monster_level() const override;
        tl::core::Vec3 random_open_position(const tl::core::Vec3& at, float radius,
                                            bool avoid_no_spawn) override;
        bool ray_collision(const tl::core::Vec3& from, const tl::core::Vec3& to,
                           tl::core::Vec3& normal) override;
        float range_random(float lo, float hi) override;
        tl::core::RunicRandom& volatile_random() override;
        void create_particles(const std::string& file, const tl::core::Vec3& at,
                              const tl::core::SpawnAxes& axes, uint32_t index) override;
        void fire_missile(const std::string& resource, const tl::core::Vec3& at,
                          const tl::core::SpawnAxes& axes) override;
        tl::core::SpawnClassPick roll_spawn_class(const std::string& name, int32_t level) override;
        tl::core::SpawnedUnitRef create_unit(const tl::core::SpawnRelease& release,
                                             const std::string& name, bool enchant) override;
        bool unit_exists(int64_t id) const override;
        bool character_alive(int64_t id) const override;
        void kill_character(int64_t id) override;
        void hide_character(int64_t id, bool hide) override;
        void run_particles(bool run) override;
        void output(uint32_t event) override { outputs_.push_back(event); }
        void unit_spawned(const tl::core::SpawnedUnitRef& unit) override;

        /// Hand the outputs raised so far to the spawner's machine.
        void deliver();

    private:
        Impl& impl_;
        tl::core::LevelScript* machine_;
        int64_t node_;
        std::vector<uint32_t> outputs_;
    };

    /// An input reaching a spawner (`CUnitSpawnerDescriptor::InputLogicEvent`).
    void spawner_input(const tl::core::ScriptEvent& event);
    /// `CUnitSpawnerDescriptor::update` for every machine's spawners.
    void step_spawners(float delta);
    /// `CUnitSpawner::unitStateChange` for a released item:
    /// 3 when picked up.
    void spawned_item_state(int64_t id, int state) override;

    /// Place a unit whose body is a particle layout, not a mesh (`LAYOUTFILE` and no
    /// `MESHFILE`, e.g. the phase beast's portal), which spawn_unit() cannot place.
    bool place_layout_bodied_unit(const std::string& unit, const Ogre::Vector3& at);

    /// The layout half of `CTriggerUnit::setTriggerState`;
    /// `state` is 1 or 2.
    void set_trigger_state_layout(Ogre::Entity* entity, const TriggerProp& prop, int state);
    /// `CTriggerUnit::trigger` at a one-shot's last state:
    /// treasure and trap. `mine`: the firing machine's nodes (only its copy opens); empty,
    /// the first copy found.
    void finish_trigger_unit(int64_t node, const std::string& unit, const TriggerProp& prop,
                             bool by_player, const std::vector<const Ogre::SceneNode*>& mine,
                             bool treasure, bool skill);

    /// `CGenericModel +0x1cc`'s list for a mesh as `loadAnimations` builds it from the
    /// `.ANIMATION` file (`tl::core::load_animation_list`); `missing` counts absent files.
    tl::core::AnimationCatalog model_catalog(const std::string& mesh, const std::string& dir,
                                             size_t* missing = nullptr) override;
    /// Every placed unit's cast: its animation's keys, `updateActiveSkill` and
    /// its manager's `update`.
    void step_unit_skills(float delta);
    void step_unit_skill_manager(tl::core::LevelScript& machine, int64_t node, float delta);
    /// The entity a machine's unit is drawn with, or null.
    SceneUnit* scene_unit_of(tl::core::LevelScript* machine, int64_t node) override;
    // PORT: drawing, a skill event's particle systems (renderer_effects.cpp).
    size_t view_particle_mark() const override;
    void view_ride_particles(size_t mark, Ogre::SceneNode* ride) override;
    void view_claim_particles(size_t mark, std::vector<Ogre::SceneNode*>& holders,
                              std::vector<Ogre::SceneNode*>& line_holders) override;
    bool view_particles_alive(const std::vector<Ogre::SceneNode*>& holders) const override;
    void view_stop_particles(const std::vector<Ogre::SceneNode*>& holders, bool loop) override;
    void view_stop_emission(const std::vector<Ogre::SceneNode*>& holders) override;
    void view_translate(const std::vector<Ogre::SceneNode*>& holders, const Ogre::Vector3& by);
    void view_stretch_lines(const std::vector<Ogre::SceneNode*>& line_holders,
                            const Ogre::Vector3& start, const Ogre::Vector3& end);
    /// `CSkillEvent::getBonePosition(unit, attachment, true)`
    /// in the world; false when the unit is gone.
    bool skill_bone_world(const ViewUnit& unit, int attachment, Ogre::Vector3& out);
    /// The unit's facing on the ground (vtable +0x130). False when it is gone.
    bool skill_unit_forward(const ViewUnit& unit, Ogre::Vector3& out);

    /// `CCharacter::doWeaponSkill(weapon)`: true when a
    /// skill started, which ends the blow.
    bool player_weapon_skill(const ProcTarget& target) override;

    // PORT: drawing, a missile's node and particles, a held item's model (renderer_effects.cpp).
    Ogre::SceneNode* view_create_node(const Ogre::Vector3& at) const;
    void view_aim_node(Ogre::SceneNode* node, const Ogre::Vector3& at,
                       const Ogre::Vector3& forward);
    void view_destroy_node(Ogre::SceneNode* node) override;
    void view_transients_alive(const std::vector<const std::vector<Ogre::SceneNode*>*>& lists,
                               const std::vector<char>& stop, const std::vector<char>& by_updating,
                               std::vector<char>& alive) override;
    bool view_held_item(const Ogre::Entity* held, Ogre::Vector3& hand, float& height) const;
    /// A particle layout placed at `at` (or riding `parent`), and its systems' nodes.
    std::vector<Ogre::SceneNode*> place_particle_layout(const std::string& reference,
                                                        const Ogre::Vector3& at,
                                                        const Ogre::Quaternion& facing,
                                                        Ogre::SceneNode* parent,
                                                        ParticleStart start = ParticleStart::Now);
    void destroy_particle_holders(std::vector<Ogre::SceneNode*>& holders) override;
    /// `CBaseUnit::sphereCollision` for a drawn unit: the
    /// first `t` in [0, 1] its collision model is touched, above 1 if not; false if not drawn.
    bool unit_collider_sweep(const Ogre::Entity* entity, const tl::core::Vec3& from,
                             const tl::core::Vec3& to, float radius, float& t);
    void unit_collider_sweeps(const std::vector<int64_t>& ids, const tl::core::Vec3& from,
                              const tl::core::Vec3& to, float radius, std::vector<float>& t,
                              std::vector<char>& drawn) override;
    /// A character's collision model (`CCharacter +0x4c8`): `media/models/collider.mesh`,
    /// what `loadModel` gives units with no `COLLISIONFILE`.
    tl::core::CollisionWorld unit_collider;
    bool unit_collider_read = false;

    void follow_subject();
    /// `CCameraControl::updateGameCamera` for the engine's default framing.
    void update_game_camera(const Ogre::Vector3& player_at);
    void tray_bumpers(bool vendor, bool rb, bool lb);
    void create_interface();
    void step_player(float delta, const tl::core::InputState& in) override;
    void drive_player(float delta);
    Ogre::Entity* place_mesh(const MeshPlacement& placement);
    /// `CLevel::bake`'s room-piece batching, once the level and its
    /// remembered state are in place.
    void bake_static_scenery() override;
    void view_prepare_mesh(const std::string& mesh_path) override;

    /// Carry out what a level script decided, and log it; `nodes` is the map the events
    /// address.
    void apply_script_events(const std::vector<tl::core::ScriptEvent>& events,
                             NodeMap& nodes) override;

    /// One frame of the level script, from wherever the character is.
    void step_script(float delta);

    // The level remembered and put back.

    /// A `Cinematic` node's `[CINEMATIC]`, opened by the run loop.
    std::string pending_layout_cinematic;

    // PORT: the automap's revealed cells into a level state and back (renderer_session.cpp).
    void view_store_automap(tl::formats::LevelSaveState& state) override;
    void view_restore_automap(const tl::formats::LevelSaveState& state) override;

    /// Put the level back as it loaded: nothing hidden, no camera held, triggers unspent.
    void reset_script();
    /// `CParticle::Start`'s timer over
    /// `particle_systems[first, last)`: largest `LIFE TIME`, else largest finite emitter
    /// window, none if any never expires; plus 0.1.
    void arm_particle_timer(size_t first, size_t last);
    bool place_particle(const tl::formats::CmpScene& layout, const tl::formats::CmpNode& node,
                        const Ogre::Vector3& position, const Ogre::Quaternion& orientation);
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool place_particle(const tl::formats::CmpScene& layout, const tl::formats::CmpNode& node,
                        const ogre::Vector3& position,
                        const ogre::Quaternion& orientation) override {
        return place_particle(layout, node, to_ogre(position), to_ogre(orientation));
    }
    void view_automap_add_tile(int type, const ogre::Vector3& at, const ogre::Vector3& dir, bool a,
                               bool b) override;
    void view_automap_player(const ogre::Vector3& at) override;
    void view_place_viewer(const tl::formats::CmpScene& layout,
                           const ogre::Vector3& origin) override;
    void view_place_meshes(const std::vector<MeshPlacement>& placements, size_t& animated_props,
                           size_t& idled) override;
    /// One of `view_place_meshes`' placements: its entity, its animation and, a unit's, its
    /// scene entry.
    void view_place_one_mesh(const MeshPlacement& from_layout, size_t& animated_props,
                             size_t& idled);
    /// A placed unit's scene entry, hands, render flags and rim light.
    void dress_placed_unit(Ogre::Entity* entity, const MeshPlacement& placement,
                           Ogre::AnimationState* started, bool armed);
    void view_own_trigger_particles(size_t before, int64_t node, const NodeMap& sink) override;
    void view_settle_link_particles(size_t systems_before, int64_t owner,
                                    const std::string& reference, const ogre::Vector3& origin,
                                    const ogre::Quaternion& basis,
                                    tl::core::LinkParticleSettle what) override;
    /// `view_settle_link_particles`' parts: its systems learn their link; they become one
    /// `CParticle`; they stop.
    void link_particle_systems(size_t systems_before, int64_t owner, const Ogre::Vector3& origin,
                               const Ogre::Quaternion& basis);
    void register_link_cparticle(size_t systems_before, int64_t owner, const std::string& reference,
                                 const Ogre::Vector3& origin, bool stopped);
    void stop_particle_systems(size_t systems_before);
    bool view_caching_particles() const override;
    unsigned long view_milliseconds() const override;
    void view_automap_create() override;
    size_t view_level_particle_node(size_t systems_before) override;
    void view_stand_subject(const ogre::Vector3& at, const ogre::Vector3& facing) override;
    void logic_trace_text(const char* text) override;
    /// One `Particle` node as Torchlight's ParticleUniverse system, started as
    /// `CParticle::Start` starts it.
    bool place_torchlight_particle(const tl::formats::CmpScene& layout,
                                   const tl::formats::CmpNode& node, const Ogre::Vector3& position,
                                   const Ogre::Quaternion& orientation);
    /// `CParticle::updateLevelObject` for a system.
    void step_torchlight_particle(ParticleSystem& system, float delta);
    void step_particles(float delta);

    /// One frame of the idle fidgets: see `Impl::Fidget`.
    void step_fidgets(float delta, float world_delta);

    /// Start a fidget on one entity, or say there is nothing to start.
    bool begin_fidget(Fidget& fidget, Ogre::Entity* entity, const std::string& dir, bool is_subject,
                      const std::string& mesh, bool armed);
    /// `started`: the state enabled, the only way to switch that animation off later.
    /// `played`: the animation class actually started (RUN/WALK may substitute).
    /// `blend`: 0 plays outright (`CGenericModel::playAnimation`), else cross-fades
    /// (`blendAnimation`). `queue`: waits behind what plays (`queueBlendAnimation`).
    bool play_scene_animation(Ogre::Entity* entity, const std::string& mesh_path,
                              const std::string& animation, bool loops, float rate,
                              bool optional = false, Ogre::AnimationState** started = nullptr,
                              bool armed = true, std::string* played = nullptr, float blend = 0.0f,
                              bool queue = false) override;

    /// How long a placed unit's animation runs (`CGenericModel::getAnimationLengthSeconds`);
    /// negative when missing, 0 for a pose.
    float scene_animation_length(const std::string& unit, const std::string& animation) override;

    /// Append what a machine traced to `--logic-trace`.
    void flush_logic_trace(tl::core::LevelScript& machine) override {
        if (logic_trace == nullptr) return;
        for (const std::string& line : machine.take_trace()) {
            std::fputs(line.c_str(), logic_trace);
            std::fputc('\n', logic_trace);
        }
        std::fflush(logic_trace);
    }
    void aim_camera(const tl::formats::CmpScene& scene, const Ogre::Vector3& origin);
    bool ground_under(const Ogre::Vector3& from, float& surface_y) const;
    // PORT: the hook (core/world.h) in OGRE's maths.
    bool ground_under(const ogre::Vector3& from, float& surface_y) const override {
        return ground_under(to_ogre(from), surface_y);
    }
    void stand_subject_at(const tl::formats::CmpNode& start, const Ogre::Vector3& origin);
    /// The layout is a main-menu backdrop (`/MAINMENUS/`), loaded via
    /// `CGameClient::loadMenuLevel`: `CLevel::bake` adds its `Light`s to
    /// `stgeomLight_`, but only `CGameClient::loadLevel`'s pass
    /// builds it, so a menu's layout lights never reach the light map.
    bool loading_menu_level = false;
    void apply_environment(const std::string& layout_path) override;
    void set_model_ambient(Ogre::Entity* entity, const Ogre::ColourValue& colour,
                           std::set<Ogre::Material*>& seen);
    void apply_scenery_fill(const tl::formats::AdmSection& rules);
    /// The level template's `MATERIAL AMBIENT` (`CLevelTemplateData +0x880`),
    /// as apply_scenery_fill() read it.
    Ogre::ColourValue material_ambient = Ogre::ColourValue(0.36078432f, 0.36078432f, 0.36078432f);
    void setup_shadow_map(const tl::formats::AdmSection& rules);
    /// Logs what the scene holds (nodes, entities, particle systems...), with
    /// `when` after "scene" in the line.
    void log_scene_census(const char* when) const override;
    /// The end of a timing window: glthread's costs, the shaders made, the GPU
    /// waits (renderer_diagnostics.cpp).
    static void log_driver_costs(bool print, unsigned frames);
    void update_player_shadow(const Ogre::Vector3& focus);
    /// Every character's and loose item's light by distance from `focus` (`CCharacter::update`
    /// `CItem::update`, `CGenericModel::setLightOverride`),
    /// once a frame.
    void update_unit_lights(const Ogre::Vector3& focus);
    /// `COLLISION_RADIUS` by unit name, for the fade's sphere centre.
    std::unordered_map<std::string, float> unit_radius_cache;
    void destroy_shadow_map();
    void setup_light_map(const tl::formats::AdmSection& rules);
    void add_light_pass(const Ogre::MaterialPtr& material);
    void step_light_map(float dt);
    void aim_light_map(const Ogre::Vector3& focus);
    void destroy_light_map();
    bool setup_shader_generator();
    /// The program binaries `config.shader_cache` holds, into OGRE and back out before it
    /// goes (RendererConfig::shader_cache).
    void load_shader_cache();
    void save_shader_cache();
    /// Where those go: `config.shader_cache`, or a file of its own while the console runs
    /// GL3Plus (RTSS writes different GLSL per render system).
    std::string shader_cache_file;
    void frame_camera_on(Ogre::Entity* entity);
    void apply_scheme(Ogre::Viewport* viewport) const;
    /// What the view is cleared to: the port's grey for a bare model, else the level's fog
    /// (`CGameClient::refreshLighting`).
    Ogre::ColourValue view_background{0.15f, 0.16f, 0.20f};
    void update_automap_icons() override;
    /// `CCharacter::updateMotion`'s icon move and `setMeshVisible`'s visibility, per frame.
    void step_automap_icons();
    /// A unit's automap icon (`CCharacter +0x728`), and whether its mesh was
    /// visible the last time it was looked at.
    struct UnitIcon {
        Ogre::SceneNode* node = nullptr;
        Ogre::Entity* entity = nullptr;
        Ogre::Billboard* billboard = nullptr;
        bool shown = true;
        std::string unit;
        Ogre::Vector3 placed = Ogre::Vector3::ZERO;
        bool reported_moving = false;
    };
    std::vector<UnitIcon> unit_icons;
    size_t last_unit_icon_count = static_cast<size_t>(-1);
};
namespace renderer_detail {
/// Declared here because mounting the archive invalidates the caches.
void forget_archive_index();
/// Every material script in the archive read once and its declared names filed.
void index_material_scripts();

void forget_parsed_materials();

/// Parses the material script beside a mesh (same basename), not the whole archive's.
std::string dir_of_asset(const std::string& name);

std::vector<std::string> declared_materials(const std::string& text);
std::string qualify_material_names(const std::string& text,
                                   const std::vector<std::pair<std::string, std::string>>& renames);
std::string strip_material_blocks(const std::string& text, const std::vector<std::string>& drop);
std::string deduplicate_material_script(const std::string& text, size_t& dropped);
std::string complete_colour_op_ex(const std::string& text, size_t& fixed);
extern std::set<std::string> g_parsed_materials;
/// How often one script may be parsed before the log warns (a cache being thrashed).
constexpr size_t kNoisyParseCount = 8;

extern std::map<std::string, size_t> g_parse_counts;
extern std::map<std::string, std::string> g_material_home;
extern std::map<std::string, std::string> g_material_alias;
extern std::map<std::string, std::string> g_material_script;
void forget_parsed_materials();
// Defined below, next to the archive index it reads.
size_t material_script_rank(const std::string& script);

extern Ogre::SceneManager* g_material_scene;
// Defined below, next to the other things that know about materials.
size_t rebind_materials(Ogre::Entity* entity);
/// `CGenericModel::reInitialize`'s fully emissive passes drawn unlit (renderer.cpp).
void unlight_full_emissive(Ogre::Entity* entity);

size_t rebind_scene_materials(Ogre::SceneManager* scene);

void apply_qualified_materials(Ogre::Entity* entity, const std::string& dir);

// Defined below, next to the other things that know about the wardrobe's naming.
std::string slot_of_material(const std::string& material_name);

void qualify_texture_paths(const Ogre::MaterialPtr& material, const std::string& dir,
                           const std::string& fallback_dir);

std::string dir_of_asset(const std::string& name);

std::string fill_empty_texture_units(const std::string& text, const std::string& script,
                                     size_t& filled);

void parse_material_script(const std::string& script, std::vector<std::string>* redefined = nullptr,
                           bool force = false);
/// The file of the spawn class whose `NAME` is `name` (case aside), or empty.
std::vector<std::string> underscore_words(const std::string& name);
std::string wardrobe_texture_for(const std::string& mesh_name, const std::string& dir,
                                 const std::string& slot, const std::string& set_word);
size_t rebind_materials(Ogre::Entity* entity);
Ogre::TextureUnitState* rim_unit(Ogre::Pass* pass, const std::string& texture);
void apply_rim_light(Ogre::Entity* entity, const std::string& texture);
void apply_render_always(Ogre::Entity* entity, bool render_always);
/// A mesh's bounds as shipped: apply_render_always() replaces a
/// `RENDERALWAYS` mesh's own with the original's never-culled box.
const Ogre::AxisAlignedBox& authored_mesh_bounds(const Ogre::Entity* entity);
void highlight_entity(Ogre::Entity* entity, bool on, bool hostile, const std::string& rim_texture);
void apply_qualified_materials(Ogre::Entity* entity, const std::string& dir);
size_t rebind_scene_materials(Ogre::SceneManager* scene);
size_t report_stale_materials(Ogre::SceneManager* scene, const char* when);
void repoint_materials(const std::vector<Ogre::Entity*>& entities,
                       const std::vector<std::string>& redefined, const std::string& body_dir);
void parse_sibling_material(const std::string& mesh_name,
                            std::vector<std::string>* redefined = nullptr, bool force = false);
std::string archive_dir_of(const std::string& resource_name);
std::string archive_path_in_dir(const std::string& basename, const std::string& dir);
std::string animation_state_of(const std::string& stem, bool& armed, bool& has_armed_suffix,
                               int& variation);
std::string archive_animation_in_dir(const std::string& dir, const std::string& state, bool armed,
                                     int variation);
std::vector<std::string> archive_animation_stems_matching(const std::string& dir,
                                                          const std::string& substring);
std::string class_unit_path(const std::string& class_name, bool* console = nullptr);
std::string archive_path_unique(const std::string& basename);
/// Find the file a material's texture name refers to: by the mesh, then the wearer, then
/// anywhere unambiguous, then with the archive's extensions; never an ambiguous match.
std::string resolve_archive_path(const std::string& reference);
/// The same lookup without the warning, to ask whether a name is a full archive path.
std::string resolve_archive_path_quiet(const std::string& reference);

std::string resolve_texture(const std::string& name, const std::string& dir,
                            const std::string& fallback_dir);
void qualify_animated_frames(Ogre::TextureUnitState* unit, const std::string& dir,
                             const std::string& fallback_dir);
void qualify_texture_paths(const Ogre::MaterialPtr& material, const std::string& dir,
                           const std::string& fallback_dir);
std::string dir_of_asset(const std::string& name);
std::string animation_named(const Ogre::Entity* entity, const std::string& wanted);
std::string slot_of_material(const std::string& material_name);
std::string fill_empty_texture_units(const std::string& text, const std::string& script,
                                     size_t& filled);
void ensure_skeleton(const Ogre::MeshPtr& mesh, const std::string& mesh_name,
                     const std::string& dir);
/// The pet a new character starts with unless the creation screen says otherwise
/// (`charactercreate.layout` lists `Dog` first).
constexpr const char* kMenuPetUnit = "Dog";

/// Where a class's idle is parked on the creation screen (as `--animation-time`).
constexpr float kMenuIdleTime = 1.1f;

using tl::core::kMenuMusic;

std::vector<uint8_t> read_file(const std::string& path);
void log_animation_states(const Ogre::Entity* entity);
using tl::core::names_the_bind_pose;
bool skeleton_has_tracks(const Ogre::SkeletonPtr& skeleton);
size_t pose_from_skeleton(Ogre::Entity* entity, const Ogre::SkeletonPtr& pose);
size_t merge_shared_animations(const Ogre::SkeletonPtr& master, const Ogre::Skeleton* source,
                               const Ogre::Skeleton::BoneHandleMap& bones,
                               const Ogre::StringVector& wanted);
bool link_path_onto(Ogre::Entity* entity, const std::string& path);

/// Draws the transparent scenery OGRE 14 moves to `RENDER_QUEUE_TRANSPARENTS` (95) as the
/// last thing in the main group, where Torchlight's OGRE 1.6 drew it (particles sit at 51
/// to 90; `CParticleTechWrapper::setAsLight` 0x5a + sort index). Group 95 is drawn just
/// before the first group after the main one, then skipped.
class TransparentsWithScenery : public Ogre::RenderQueueListener {
public:
    explicit TransparentsWithScenery(Ogre::SceneManager* scene) : scene_(scene) {}

    void preRenderQueues() override { drawn_ = false; }

    void renderQueueStarted(Ogre::uint8 id, const Ogre::String&, bool& skip) override {
        if (id == Ogre::RENDER_QUEUE_TRANSPARENTS) {
            skip = drawn_;
            return;
        }
        if (drawn_ || id <= Ogre::RENDER_QUEUE_MAIN || id > Ogre::RENDER_QUEUE_TRANSPARENTS) return;
        drawn_ = true;
        const auto& groups = scene_->getRenderQueue()->_getQueueGroups();
        if (Ogre::RenderQueueGroup* group = groups[Ogre::RENDER_QUEUE_TRANSPARENTS].get()) {
            if (scene_->isRenderQueueToBeProcessed(Ogre::RENDER_QUEUE_TRANSPARENTS)) {
                scene_->_renderQueueGroupObjects(group,
                                                 Ogre::QueuedRenderableCollection::OM_PASS_GROUP);
            }
        }
    }

private:
    Ogre::SceneManager* scene_;
    bool drawn_ = false;
};

/// What the view queues (installed by setup_scene()). The light map's groups 95-99 are
/// never drawn in the view: `CGame::renderableQueued` (installed by
/// `CGame::preViewportUpdate`; removed) refuses
/// them (`CGenericModel::setRenderToLightMap`;
/// `CParticleTechWrapper::setAsLight`; `CGameClient::renderableQueued`,
/// `preRenderTargetUpdate`); here at
/// kLightMapGroupFirst..kLightMapGroupLast (kVisibilityLightMap). And the merged scenery
/// is re-queued at `kLayoutDrawOrderCeiling` (90) so depth ties fall as before the merge.
/// The original's queues: group 0x58 (characters; `CCharacter::setRenderBehind`, 50 with
/// the body re-queued) after the main group, then Runic's 0x5a up. Below 0x58 they are
/// OGRE's numbers; from 0x58 on they follow the main group, capped below the overlay.
constexpr unsigned runic_queue(unsigned queue) {
    return queue < 0x58 ? queue
                        : (queue - 0x58 + Ogre::RENDER_QUEUE_MAIN + 1 < 90
                               ? queue - 0x58 + Ogre::RENDER_QUEUE_MAIN + 1
                               : 90u);
}

constexpr unsigned kCharacterGroup = runic_queue(0x58);

/// Where Runic's group 0x5a lands; the rest follow at the same offsets.
constexpr unsigned kParticleGroupBase = runic_queue(0x5a);

constexpr unsigned kLightMapGroupFirst = kParticleGroupBase + 5;

constexpr unsigned kLightMapGroupLast = kParticleGroupBase + 9;

/// What the light map draws: things with its visibility flag. `preRenderTargetUpdate`
/// masks to 2, carried only by the plate and mask quads,
/// the `stgeomLight_` lights (`CLevel::bake`) and a
/// `setRenderToLightMap(true)` model; everything else
/// carries 1 (`CGame::chooseSceneManager`), particles included. The port's 2 is
/// the automap's, so here it is 4.
constexpr std::uint32_t kVisibilityLightMap = 0x4;

/// A renderable whose material has no compiled technique is queued with OGRE's blank white
/// (`RenderQueue::addRenderable`) and stays on it, as the queue only `prepare()`s it. The
/// shader generator rewriting its shaders (`SGScheme::synchronizeWithLightSettings`,
/// `Material::removeTechnique`) leaves a prepared material so; it is compiled here.
inline void compile_waiting_material(Ogre::Renderable* rend, Ogre::Technique** technique) {
    if (technique == nullptr || *technique == nullptr) return;
    const Ogre::MaterialPtr& held = rend->getMaterial();
    if (!held || (*technique)->getParent() == held.get()) return;
    if (!held->getCompilationRequired() || !held->getSupportedTechniques().empty()) return;
    held->touch();
    if (Ogre::Technique* own = rend->getTechnique()) {
        *technique = own;
        TL_LOG_INFO("material '%s' was left with no technique compiled; compiled again",
                    held->getName().c_str());
    }
}

/// `CCharacter::setRenderBehind(bool)`: a model drawn
/// through what stands in front of it (`CGenericModel::setRenderBehind`);
/// queue 50 on, 0x58 off; equipped items follow
/// (`CEquipment::setRenderBehind`). `hostile`: `CCharacter::setAlignment`'s
/// `outlinered.dds` over `outlineblue.dds`.
struct RenderBehind {
    bool on = false;
    bool hostile = false;
};
void set_render_behind(Ogre::Entity* entity, bool on, bool hostile);
/// The state `set_render_behind` left on an entity; null when it never ran.
const RenderBehind* render_behind_of(const Ogre::Entity* entity);

/// The render-behind materials by colour and skinning (`CGenericModel::reInitialize`'s
/// `rbmat_N`).
class RenderBehindMaterials {
public:
    explicit RenderBehindMaterials(GpuSkinning* gpu) : gpu_(gpu) {}
    Ogre::MaterialPtr material(bool hostile, unsigned bones, unsigned weights);

private:
    GpuSkinning* gpu_;
    std::map<std::tuple<bool, unsigned, unsigned>, Ogre::MaterialPtr> materials_;
};

/// `CGame::renderableQueued`, its render-behind half: a
/// seen sub-entity is queued again on 0x58 (the body) and drawn here additive with depth
/// test `GREATER`. True when it swapped `technique`.
bool render_behind_swap(Ogre::Renderable* rend, Ogre::ushort priority, Ogre::Technique** technique,
                        Ogre::RenderQueue* queue, RenderBehindMaterials& materials,
                        GpuSkinning* gpu);

class ViewRenderables : public Ogre::RenderQueue::RenderableListener {
public:
    bool renderableQueued(Ogre::Renderable* rend, Ogre::uint8 group, Ogre::ushort priority,
                          Ogre::Technique** technique, Ogre::RenderQueue* queue) override {
        compile_waiting_material(rend, technique);
        if (group >= kLightMapGroupFirst && group <= kLightMapGroupLast) return false;
        if (gpu != nullptr) gpu->guard(rend, technique);
        // In play only: `CGameClient +0x38d0` (the game state) 0 refuses it.
        if (behind != nullptr && behind_in_play && group == Ogre::RENDER_QUEUE_MAIN &&
            render_behind_swap(rend, priority, technique, queue, *behind, gpu)) {
            return true;
        }
        if (priority == kLayoutDrawOrderCeiling || opaque == nullptr) return true;
        auto* bucket = dynamic_cast<Ogre::StaticGeometry::GeometryBucket*>(rend);
        if (bucket == nullptr ||
            bucket->getParent()->getParent()->getParent()->getParent() != opaque) {
            return true;
        }
        queue->addRenderable(rend, group, kLayoutDrawOrderCeiling);
        return false;
    }

    /// The level's merge; nothing else is re-queued.
    const Ogre::StaticGeometry* opaque = nullptr;
    /// No skinning shader on what the CPU skins (GpuSkinning::guard).
    GpuSkinning* gpu = nullptr;
    /// The render-behind outline's materials, and whether the game is in play.
    RenderBehindMaterials* behind = nullptr;
    bool behind_in_play = false;
};

extern ViewRenderables g_view_renderables;
bool load_archive_image(const std::string& path, Ogre::Image& out);
}  // namespace renderer_detail
Ogre::Quaternion orientation_from_basis(const Ogre::Vector3& forward, const Ogre::Vector3& right,
                                        const Ogre::Vector3& up);
Ogre::Quaternion node_orientation(const tl::formats::CmpNode& node);
Ogre::Vector3 layout_scale(const tl::formats::CmpNode& node, float unit_scale);
Ogre::MeshPtr load_layout_mesh(const std::string& mesh_path);
Ogre::AnimationState* state_named_any_of(Ogre::AnimationStateSet* states,
                                         const std::vector<std::string>& wanted);
namespace renderer_detail {
/// `g_bCachParticles`: set by the first level a template loads
/// (`CLevel::loadRoomLayout`,
/// `CLayout::setCacheingParticlesForLevel`), never cleared, for the process.
bool caching_particles();
void set_caching_particles();
void restart_particle_system(ParticleSystem& system);
void stop_particle_emission(ParticleSystem& system);
using tl::core::kDefaultCameraDistance;

Ogre::Vector3 default_camera_offset(float camera_mult);
using tl::core::find_spawn;
using tl::core::log_memory;
/// The material name of every submesh as the serializer reads it, in order: OGRE 14 keeps
/// no unresolvable name (`SubMesh::getMaterialName`), and collision materials
/// (`tl::core::collision_material`) resolve nowhere (`MeshSerializerImpl::readSubMesh`).
struct SubmeshMaterialNames final : Ogre::MeshSerializerListener {
    std::vector<std::string> names;
    void processMaterialName(Ogre::Mesh*, Ogre::String* name) override { names.push_back(*name); }
    void processSkeletonName(Ogre::Mesh*, Ogre::String*) override {}
    void processMeshCompleted(Ogre::Mesh*) override {}
};

using tl::core::log_passability;
using tl::core::write_passability_dump;
void for_each_triangle(
    const Ogre::MeshPtr& mesh, const Ogre::Matrix4& to_world,
    const std::function<void(const Ogre::Vector3&, const Ogre::Vector3&, const Ogre::Vector3&)>& fn,
    const std::function<bool(const std::string&)>& accept_material = nullptr);
}  // namespace renderer_detail
using tl::core::kSpawnedNodeBase;

using tl::core::kMissilePlayer;

namespace renderer_detail {
using tl::core::spawn_data_of;
Ogre::Quaternion quaternion_from_axes(const tl::core::SpawnAxes& axes);
/// `ProjectorCam` (`CGameClient::CGameClient`): orthographic,
/// `setFrustumExtents(-24, 24, 24, -24)`, looking straight down, kept 100 above the focus
/// (`CCameraControl::updateGameCamera`).
constexpr float kLightMapHalfExtent = 24.0f;

constexpr float kLightMapNear = 1.0f;

constexpr float kLightMapFar = 300.0f;

constexpr float kLightMapHeight = 100.0f;

/// `ProjectorRTTLight` is `KSETTINGS_SHADOW_RESOLUTION` square;
/// the high preset's 512 (`CSettings::assignDefault`).
constexpr unsigned kLightMapResolution = 512;

/// The light map's clear colour until a level's rules set LIGHTMAP
/// (0x3dc0c0c1, 24/255).
constexpr float kLightMapDefaultClear = 24.0f / 255.0f;

/// What the light map queues: `CGameClient::renderableQueued`
/// with the shadow flag (`+0x2b0`) clear takes groups 0, 95-99 and 101.
class LightMapRenderables : public Ogre::RenderQueue::RenderableListener {
public:
    bool renderableQueued(Ogre::Renderable* rend, Ogre::uint8 group, Ogre::ushort,
                          Ogre::Technique** technique, Ogre::RenderQueue*) override {
        compile_waiting_material(rend, technique);
        return group == Ogre::RENDER_QUEUE_BACKGROUND ||
               (group >= kLightMapGroupFirst && group <= kLightMapGroupLast) ||
               group == kLightMapGroupLast + 2;
    }
};

/// `CGameClient::preRenderTargetUpdate` / `postRenderTargetUpdate`
/// for the light map: its listener in for the update, the quads joined meanwhile; the mask
/// is the viewport's (kVisibilityLightMap). `CLevel::setNonLightStaticGeometryVisible`
/// has nothing to hide.
class LightMapTarget : public Ogre::RenderTargetListener {
public:
    LightMapTarget(Ogre::SceneManager* scene, Ogre::SceneNode* quads,
                   Ogre::RenderQueue::RenderableListener* view)
        : scene_(scene), quads_(quads), view_(view) {}

    // PORT: drawing, the light map's render target refreshed.
    void preRenderTargetUpdate(const Ogre::RenderTargetEvent&) override {
        scene_->getRenderQueue()->setRenderableListener(&renderables_);
        if (!quads_->getParentSceneNode()) scene_->getRootSceneNode()->addChild(quads_);
    }

    void postRenderTargetUpdate(const Ogre::RenderTargetEvent&) override {
        scene_->getRenderQueue()->setRenderableListener(view_);
        if (Ogre::SceneNode* parent = quads_->getParentSceneNode()) parent->removeChild(quads_);
    }

private:
    Ogre::SceneManager* scene_;
    Ogre::SceneNode* quads_;
    Ogre::RenderQueue::RenderableListener* view_;
    LightMapRenderables renderables_;
};

/// `ProjectorCamShadow` (`CGameClient::CGameClient`): FOVy
/// 120, looking down; `CGameClient::refreshLighting`
/// makes it orthographic (`setFrustumExtents(-8, 8, 8, -8)`) for ORTHOGRAPHICSHADOWS
/// (`CLevelTemplateData +0x8b0`). Kept 5 above the focus
/// (`CCameraControl::updateGameCamera`).
constexpr float kShadowMapHalfExtent = 8.0f;

constexpr float kShadowMapFovDegrees = 120.0f;

constexpr float kShadowMapNear = 1.0f;

constexpr float kShadowMapFar = 150.0f;

constexpr float kShadowMapHeight = 5.0f;

/// `ProjectorRTT` is `SHADOW_RESOLUTION` square, cleared to grey 0.5:
/// the receivers double the map, so the clear leaves a surface as it was.
constexpr unsigned kShadowMapResolution = kLightMapResolution;

constexpr float kShadowMapClear = 0.5f;

/// `ProjectorBackgroundQuad` is on group 94 (`setRenderQueueGroup(0x5e)`), after the
/// casters here.
constexpr unsigned kShadowFadeGroup = kParticleGroupBase + 4;

/// `RenderShadow<i>` and its skinned twins (`CGame::createMaterials`):
/// twelve of each.
constexpr int kShadowLevels = 12;

/// What the shadow map queues: `CGameClient::renderableQueued`
/// with the shadow flag (X360 `+0x220`, Linux `+0x2b0`) set: a casting (`+0x1`),
/// visible (`+0x134`) sub-entity drawn with `RenderShadow<floor(opacity * 12) - 1>`, or its
/// skinned twin for its weight count (`+0x12c`), made per bone and weight count.
class ShadowCasters : public Ogre::RenderQueue::RenderableListener {
public:
    explicit ShadowCasters(GpuSkinning* gpu) : gpu_(gpu) {}
    bool renderableQueued(Ogre::Renderable* rend, Ogre::uint8 group, Ogre::ushort,
                          Ogre::Technique** technique, Ogre::RenderQueue*) override;

private:
    Ogre::MaterialPtr material(int level, unsigned bones, unsigned weights);

    GpuSkinning* gpu_;
    std::map<std::tuple<int, unsigned, unsigned>, Ogre::MaterialPtr> materials_;
};

/// `CGameClient::preRenderTargetUpdate` / `postRenderTargetUpdate`
/// for the shadow map (kVisibilityShadowMap).
class ShadowMapTarget : public Ogre::RenderTargetListener {
public:
    ShadowMapTarget(Ogre::SceneManager* scene, Ogre::SceneNode* quad,
                    Ogre::RenderQueue::RenderableListener* view, GpuSkinning* gpu)
        : scene_(scene), quad_(quad), view_(view), casters_(gpu) {}

    // PORT: drawing, the shadow map's render target refreshed.
    void preRenderTargetUpdate(const Ogre::RenderTargetEvent&) override {
        scene_->getRenderQueue()->setRenderableListener(&casters_);
        if (!quad_->getParentSceneNode()) scene_->getRootSceneNode()->addChild(quad_);
    }

    void postRenderTargetUpdate(const Ogre::RenderTargetEvent&) override {
        scene_->getRenderQueue()->setRenderableListener(view_);
        if (Ogre::SceneNode* parent = quad_->getParentSceneNode()) parent->removeChild(quad_);
    }

private:
    Ogre::SceneManager* scene_;
    Ogre::SceneNode* quad_;
    Ogre::RenderQueue::RenderableListener* view_;
    ShadowCasters casters_;
};

std::unique_ptr<Ogre::Rectangle2D> light_map_quad(float half, const Ogre::MaterialPtr& material,
                                                  Ogre::uint8 group);
Ogre::MaterialPtr light_map_material(const std::string& name, Ogre::SceneBlendType blend);
void set_light_map_texture(const Ogre::MaterialPtr& material, const std::string& reference);
}  // namespace renderer_detail

tl::core::DungeonManager& loaded_dungeon_manager();
tl::core::DungeonFiles archive_dungeon_files();
bool subject_in_shot(const Ogre::Camera* camera, const Ogre::Entity* subject);
namespace renderer_detail {
std::string biome_key_from_ruleset(const std::string& ruleset);
std::string biome_display_name(const std::string& key);
using tl::core::orientation_bytes;
using tl::core::yaw_of_bytes;
tl::formats::QuestManagerSave quests_to_save(
    const tl::core::QuestLog& log, const std::function<int64_t(const std::string&)>& unit_guid_of);
std::string save_directory_of(const std::string& path);
extern bool keep_loading_screen;
extern bool keep_game_hud;
extern bool keep_inventory;
Ogre::MeshPtr load_gear_mesh(const std::string& path, const std::string& fallback_dir);
Ogre::Vector3 line_point(const Ogre::Vector3 (&p)[4], float t);
}  // namespace renderer_detail
using tl::core::kQuestBannerSeconds;

/// How fast it then fades, in alpha per second (.rodata, `CGameUI::updateIngameUI`),
/// after the hold.
constexpr float kBannerFadePerSecond = 2.0f;

/// The fidget cooldown: `CMonster::idleAI` tests `+0x284` against 10.0.
constexpr float kFidgetCooldown = 10.0f;

/// How often a unit that may fidget does, per second: ten in 1001 per AI tick at 60 Hz.
/// DIVERGENCE (frame rate): a rate, not a per-frame roll.
constexpr float kFidgetPerSecond = 60.0f * 10.0f / 1001.0f;

/// The word every fidget file carries, matched as a substring
/// (archive_animation_stems_matching()).
constexpr const char* kFidgetWord = "fidget";

/// The owner the pet's model is filed under in `spawned`, apart from every
/// spawner's: the model itself gets a spawned unit's id (`pet_node_id`).
constexpr int64_t kPetNodeId = 0x5350000000000000LL - 1;

/// How far to the character's side a new pet is put: the two authored radii.
constexpr float kPetSpawnOffset = 1.6f;

}  // namespace tl::render
