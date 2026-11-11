// The renderer: light map, level rules, environment, layouts, dungeon floors.
#include "core/layout_text.h"
#include "core/level_memory.h"
#include "core/logic_names.h"
#include "render/renderer_impl.h"

namespace tl::render {
/// The dynamic light map: made once per scene as CGameClient::CGameClient
/// makes it under LIGHTING_ENABLED (ProjectorRTTLight on ProjectorCam,
/// ProjectorLightQuad at +-0.5 with RttLight, ProjectorLightMask with RttLightMask; the
/// corners reset each frame by CGameClient::updateMenu
/// and updateIngame). Re-textured per level as
/// CGameClient::refreshLighting: cleared to LIGHTMAP
/// (CLevelTemplateData +0x700), TORCHLIGHT (+0x6d0), LIGHTMASK (+0x6e8); constructor
/// defaults.
/// DIVERGENCE: the target is PF_BYTE_RGBA, not PF_R8G8B8 (the console allocates no other
/// manual format), so the pass takes its alpha from the surface.
void Renderer::Impl::setup_light_map(const tl::formats::AdmSection& rules) {
    if (config.no_torchlight) {
        TL_LOG_INFO("  light map left out (--no-torchlight)");
        return;
    }
    LightMap& map = light_map;
    if (!map.texture) {
        map.texture = Ogre::TextureManager::getSingleton().createManual(
            "tl/ProjectorRTTLight", Ogre::RGN_INTERNAL, Ogre::TEX_TYPE_2D, kLightMapResolution,
            kLightMapResolution, 0, Ogre::PF_BYTE_RGBA, Ogre::TU_RENDERTARGET);

        map.camera = scene->createCamera("tl/ProjectorCam");
        // The clip distances before the extents, where sets them
        // after: OGRE 14's setNearClipDistance() rescales extents already set.
        map.camera->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
        map.camera->setAspectRatio(1.0f);
        map.camera->setNearClipDistance(kLightMapNear);
        map.camera->setFarClipDistance(kLightMapFar);
        map.camera->setFrustumExtents(-kLightMapHalfExtent, kLightMapHalfExtent,
                                      kLightMapHalfExtent, -kLightMapHalfExtent);
        map.camera_node = scene->getRootSceneNode()->createChildSceneNode();
        map.camera_node->setPosition(0.0f, kLightMapHeight, 0.0f);
        map.camera_node->setOrientation(
            Ogre::Quaternion(Ogre::Degree(-90.0f), Ogre::Vector3::UNIT_X));
        map.camera_node->attachObject(map.camera);

        Ogre::RenderTarget* target = map.texture->getBuffer()->getRenderTarget();
        map.viewport = target->addViewport(map.camera);
        map.viewport->setShadowsEnabled(false);
        map.viewport->setClearEveryFrame(true);
        map.viewport->setBackgroundColour(Ogre::ColourValue(
            kLightMapDefaultClear, kLightMapDefaultClear, kLightMapDefaultClear, 1.0f));
        map.viewport->setOverlaysEnabled(false);
        map.viewport->setVisibilityMask(kVisibilityLightMap);
        map.viewport->setMaterialScheme(Ogre::MSN_SHADERGEN);
        target->setAutoUpdated(true);

        map.light_material = light_map_material("tl/RttLight", Ogre::SBT_REPLACE);
        set_light_map_texture(map.light_material, "media/sharedtextures/lantern.dds");
        map.mask_material = light_map_material("tl/RttLightMask", Ogre::SBT_TRANSPARENT_ALPHA);
        set_light_map_texture(map.mask_material, "media/sharedtextures/lightmask.dds");

        map.light_quad = light_map_quad(0.5f, map.light_material, Ogre::RENDER_QUEUE_BACKGROUND);
        map.light_quad->setVisibilityFlags(kVisibilityLightMap);
        // What the original's per-frame `setCorners` leaves (step_light_map): the same corners
        // and their own box at the origin, in place of the never-culled one it was made with.
        map.light_quad->setCorners(-0.5f, 0.5f, 0.5f, -0.5f);
        map.light_mask =
            light_map_quad(1.0f, map.mask_material, static_cast<Ogre::uint8>(kLightMapGroupLast));
        map.light_mask->setVisibilityFlags(kVisibilityLightMap);
        map.quad_node = scene->createSceneNode();
        map.quad_node->attachObject(map.light_quad.get());
        map.quad_node->attachObject(map.light_mask.get());

        map.listener = std::make_unique<LightMapTarget>(scene, map.quad_node, &g_view_renderables);
        target->addListener(map.listener.get());
    }

    // CLevelTemplateData::load:
    // LIGHTMAP RED/GREEN/BLUE are integers defaulting to 24; the constructor's white
    // is only a never-loaded template's.
    const auto channel = [&rules](const char* key) {
        return static_cast<float>(static_cast<int32_t>(rules.integer_or(key, 24))) / 255.0f;
    };
    const float r = channel("LIGHTMAP RED");
    const float g = channel("LIGHTMAP GREEN");
    const float b = channel("LIGHTMAP BLUE");
    map.viewport->setBackgroundColour(Ogre::ColourValue(r, g, b, 1.0f));
    const std::string torchlight =
        rules.string_or("TORCHLIGHT", "media/sharedtextures/lantern.dds");
    const std::string mask = rules.string_or("LIGHTMASK", "media/sharedtextures/lightmask.dds");
    set_light_map_texture(map.light_material, torchlight);
    set_light_map_texture(map.mask_material, mask);

    // CLevel::setProjectorPass: the room pieces'
    // materials; characters take none (CLevel::addCharacter). Items with LEVELLIGHTING do
    // through CLevel::addItem: MISSING.
    // Aimed over the player, at the level's start (CLevel +0x194).
    if (have_player_start) aim_light_map(to_ogre(player_start));

    std::set<const Ogre::Material*> seen;
    size_t lit = 0;
    for (Ogre::Entity* entity : room_piece_meshes) {
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            const Ogre::MaterialPtr& material = entity->getSubEntity(i)->getMaterial();
            if (!material || !seen.insert(material.get()).second) continue;
            add_light_pass(material);
            ++lit;
        }
    }
    TL_LOG_INFO(
        "  light map %ux%u: LIGHTMAP (%.0f, %.0f, %.0f), TORCHLIGHT %s, LIGHTMASK %s; "
        "%zu room piece material(s), %zu light pool(s)",
        kLightMapResolution, kLightMapResolution, static_cast<double>(r * 255.0f),
        static_cast<double>(g * 255.0f), static_cast<double>(b * 255.0f), torchlight.c_str(),
        mask.c_str(), lit, light_pool_meshes.size());
}

/// `LIGHTPASS`: the light map projected onto one material. CLevel::setProjectorPass,
/// CGenericModel::setProjectorPass,
/// lighting-only branch; skipped for `nolight` materials.
/// With fewer than two
/// units the new one is appended, else it replaces the last, which moves to the end. The
/// unit: projected by ProjectorCam, clamped, LBX_MODULATE_X4.
void Renderer::Impl::add_light_pass(const Ogre::MaterialPtr& material) {
    if (!light_map.camera || material->getName().find("nolight") != std::string::npos) return;
    Ogre::Technique* technique = material->getBestTechnique();
    if (!technique || technique->getNumPasses() == 0) return;
    Ogre::Pass* pass = technique->getPass(0);

    // A shadow receiver (a name with `shadow`, CGenericModel::reInitialize)
    // under SHADOWS_ENABLED: CGenericModel::setProjectorPass
    // gives it the shadow map's unit (LBX_MODULATE_X2) and the light
    // map's (LBX_MODULATE_X4), appended or in place of the last, a copy of which goes last.
    if (shadow_map.camera && material->getName().find("shadow") != std::string::npos) {
        const unsigned short units = pass->getNumTextureUnitStates();
        for (unsigned short i = 0; i < units; ++i) {
            if (pass->getTextureUnitState(i)->getName() == "LIGHTPASS") return;
        }
        Ogre::TextureUnitState* shadow_unit = nullptr;
        Ogre::TextureUnitState* light_unit = nullptr;
        if (units < 2) {
            shadow_unit = pass->createTextureUnitState(shadow_map.texture->getName());
            light_unit = pass->createTextureUnitState(light_map.texture->getName());
        } else {
            const unsigned short last = static_cast<unsigned short>(units - 1);
            light_unit = pass->createTextureUnitState();
            Ogre::TextureUnitState* moved = pass->createTextureUnitState();
            *moved = *pass->getTextureUnitState(last);
            moved->setName("");
            shadow_unit = pass->getTextureUnitState(last);
            shadow_unit->setBlank();
            // DIVERGENCE (OGRE 14): as for the light map's unit below, the
            // reused unit drops the transform it had.
            shadow_unit->removeAllEffects();
            shadow_unit->setTextureTransform(Ogre::Matrix4::IDENTITY);
            shadow_unit->setTextureName(shadow_map.texture->getName());
            light_unit->setTextureName(light_map.texture->getName());
        }
        const auto project = [](Ogre::TextureUnitState* unit, Ogre::Frustum* frustum,
                                Ogre::LayerBlendOperationEx op) {
            unit->setProjectiveTexturing(true, frustum);
            unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
            unit->setTextureFiltering(Ogre::FO_POINT, Ogre::FO_LINEAR, Ogre::FO_NONE);
            unit->setColourOperationEx(op, Ogre::LBS_CURRENT, Ogre::LBS_TEXTURE);
            unit->setAlphaOperation(Ogre::LBX_SOURCE2, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
            unit->setName("LIGHTPASS");
        };
        project(shadow_unit, shadow_map.camera, Ogre::LBX_MODULATE_X2);
        project(light_unit, light_map.camera, Ogre::LBX_MODULATE_X4);
        return;
    }

    // The record's `+0x28` is the pass's own `LIGHTPASS` here: a material that
    // has one has had its pass. So `+0x2a`'s exclusion never has anything to
    // take off, and the count is every unit.
    const unsigned short counted = pass->getNumTextureUnitStates();
    for (unsigned short i = 0; i < counted; ++i) {
        if (pass->getTextureUnitState(i)->getName() == "LIGHTPASS") return;
    }

    Ogre::TextureUnitState* unit = nullptr;
    if (counted < 2) {
        unit = pass->createTextureUnitState(light_map.texture->getName());
    } else {
        const unsigned short at = static_cast<unsigned short>(counted - 1);
        Ogre::TextureUnitState* moved = pass->createTextureUnitState();
        *moved = *pass->getTextureUnitState(at);
        moved->setName("");
        unit = pass->getTextureUnitState(at);
        unit->setBlank();
        // DIVERGENCE (OGRE 14): the original's LIGHTPASS inherits the replaced unit's
        // texture transform; the RTSS aborts on a transform of a projected coordinate, so
        // it is dropped.
        unit->removeAllEffects();
        unit->setTextureTransform(Ogre::Matrix4::IDENTITY);
        unit->setTextureName(light_map.texture->getName());
    }
    unit->setProjectiveTexturing(true, light_map.camera);
    unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    unit->setTextureFiltering(Ogre::FO_POINT, Ogre::FO_LINEAR, Ogre::FO_NONE);
    unit->setColourOperationEx(Ogre::LBX_MODULATE_X4, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
    unit->setAlphaOperation(Ogre::LBX_SOURCE2, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
    unit->setName("LIGHTPASS");
}

/// The light map's per-frame half of CGameClient::updateMenu
/// and updateIngame: the unread flicker angle
/// (+0x2a8) still draws its volatile numbers every 16 ms, as the original does.
/// DIVERGENCE: at most once per port frame (the frame-rate exception).
void Renderer::Impl::step_light_map(float dt) {
    LightMap& map = light_map;
    if (!map.light_quad) return;
    const float frame = std::min(dt, 0.3f);
    map.flicker_clock += frame;
    if (map.flicker_clock > 0.016f) {
        map.flicker_clock = 0.0f;
        tl::core::RunicRandom& random = tl::core::volatile_random();
        map.flicker_angle += random.between(2.0f, 5.0f) * frame;
        if (random.between(0.0f, 10.0f) > 7.0f) map.flicker_angle += random.between(-0.2f, 0.2f);
        while (map.flicker_angle >= 6.2831855f) map.flicker_angle -= 6.2831855f;
    }
    // The original also sets the quad's corners again here each frame, to the same ones; what
    // that leaves is set once where the quad is made, since on GL each set maps the buffer.
}

/// `ProjectorCam` 100 above the camera's focus and `ProjectorCamShadow` 5
/// above it (`CCameraControl::updateGameCamera`, its
/// LIGHTING_ENABLED and SHADOWS_ENABLED branches; the menu's state 3 the same).
/// Every camera path of follow_subject() ends here, once a frame.
void Renderer::Impl::aim_light_map(const Ogre::Vector3& focus) {
    if (light_map.camera_node) {
        light_map.camera_node->setPosition(focus + Ogre::Vector3(0.0f, kLightMapHeight, 0.0f));
    }
    if (shadow_map.camera_node) {
        shadow_map.camera_node->setPosition(focus + Ogre::Vector3(0.0f, kShadowMapHeight, 0.0f));
    }
    // The player measured against the same point (update_player_shadow()).
    update_player_shadow(focus);
    // And every character's light (update_unit_lights()).
    update_unit_lights(focus);
}

/// The shadow map, made once per scene as CGameClient::CGameClient
/// makes it under SHADOWS_ENABLED (ProjectorRTT cleared to 0.5 grey,
/// ProjectorBackgroundQuad with RttBackground); set per level as
/// CGameClient::refreshLighting: SHADOWFADE
/// and ORTHOGRAPHICSHADOWS. Not drawn without the light map.
///
/// DIVERGENCE: the target is `PF_BYTE_RGBA`, for the light map's reason.
void Renderer::Impl::setup_shadow_map(const tl::formats::AdmSection& rules) {
    if (config.no_torchlight || config.no_shadows) {
        TL_LOG_INFO("  shadow map left out (%s)",
                    config.no_shadows ? "--no-shadows" : "--no-torchlight");
        return;
    }
    ShadowMap& map = shadow_map;
    if (!map.texture) {
        map.texture = Ogre::TextureManager::getSingleton().createManual(
            "tl/ProjectorRTT", Ogre::RGN_INTERNAL, Ogre::TEX_TYPE_2D, kShadowMapResolution,
            kShadowMapResolution, 0, Ogre::PF_BYTE_RGBA, Ogre::TU_RENDERTARGET);

        map.camera = scene->createCamera("tl/ProjectorCamShadow");
        map.camera->setAspectRatio(1.0f);
        map.camera->setNearClipDistance(kShadowMapNear);
        map.camera->setFarClipDistance(kShadowMapFar);
        map.camera->setFOVy(Ogre::Degree(kShadowMapFovDegrees));
        map.camera_node = scene->getRootSceneNode()->createChildSceneNode();
        map.camera_node->setPosition(0.0f, kShadowMapHeight, 0.0f);
        map.camera_node->setOrientation(
            Ogre::Quaternion(Ogre::Degree(-90.0f), Ogre::Vector3::UNIT_X));
        map.camera_node->attachObject(map.camera);

        Ogre::RenderTarget* target = map.texture->getBuffer()->getRenderTarget();
        map.viewport = target->addViewport(map.camera);
        map.viewport->setShadowsEnabled(false);
        map.viewport->setClearEveryFrame(true);
        map.viewport->setBackgroundColour(
            Ogre::ColourValue(kShadowMapClear, kShadowMapClear, kShadowMapClear, 1.0f));
        map.viewport->setOverlaysEnabled(false);
        map.viewport->setVisibilityMask(kVisibilityShadowMap);
        map.viewport->setMaterialScheme(Ogre::MSN_SHADERGEN);
        target->setAutoUpdated(true);

        map.background_material =
            light_map_material("tl/RttBackground", Ogre::SBT_TRANSPARENT_ALPHA);
        map.background = light_map_quad(1.0f, map.background_material,
                                        static_cast<Ogre::uint8>(kShadowFadeGroup));
        map.background->setVisibilityFlags(kVisibilityShadowMap);
        map.quad_node = scene->createSceneNode();
        map.quad_node->attachObject(map.background.get());

        map.listener = std::make_unique<ShadowMapTarget>(scene, map.quad_node, &g_view_renderables,
                                                         &gpu_skinning);
        target->addListener(map.listener.get());
    }

    set_light_map_texture(map.background_material,
                          rules.string_or("SHADOWFADE", "media/sharedtextures/shadowfade.dds"));
    // The clip distances before the extents, as setup_light_map() says why.
    const bool orthographic = rules.bool_or("ORTHOGRAPHICSHADOWS", false);
    if (orthographic) {
        map.camera->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
        map.camera->setFrustumExtents(-kShadowMapHalfExtent, kShadowMapHalfExtent,
                                      kShadowMapHalfExtent, -kShadowMapHalfExtent);
    } else {
        map.camera->resetFrustumExtents();
        map.camera->setProjectionType(Ogre::PT_PERSPECTIVE);
        map.camera->setFOVy(Ogre::Degree(kShadowMapFovDegrees));
    }
    TL_LOG_INFO("  shadow map %ux%u: %s, SHADOWFADE %s", kShadowMapResolution, kShadowMapResolution,
                orthographic ? "orthographic +-8" : "perspective 120",
                rules.string_or("SHADOWFADE", "media/sharedtextures/shadowfade.dds").c_str());
}

/// Before Root, as destroy_light_map().
void Renderer::Impl::destroy_shadow_map() {
    ShadowMap& map = shadow_map;
    if (!map.texture) return;
    Ogre::RenderTarget* target = map.texture->getBuffer()->getRenderTarget();
    if (map.listener) target->removeListener(map.listener.get());
    target->removeAllViewports();
    if (map.quad_node) {
        map.quad_node->detachAllObjects();
        if (Ogre::SceneNode* parent = map.quad_node->getParentSceneNode()) {
            parent->removeChild(map.quad_node);
        }
        scene->destroySceneNode(map.quad_node);
    }
    if (map.camera_node) map.camera_node->detachAllObjects();
    if (map.camera) scene->destroyCamera(map.camera);
    map.background.reset();
    map.listener.reset();
    map.background_material.reset();
    Ogre::TextureManager::getSingleton().remove(map.texture);
    map = ShadowMap{};
}

/// Before Root: the camera, the quads' node and the viewport belong to the
/// scene manager and the render target, and the target's texture to the
/// texture manager.
void Renderer::Impl::destroy_light_map() {
    LightMap& map = light_map;
    if (!map.texture) return;
    Ogre::RenderTarget* target = map.texture->getBuffer()->getRenderTarget();
    if (map.listener) target->removeListener(map.listener.get());
    target->removeAllViewports();
    if (map.quad_node) {
        map.quad_node->detachAllObjects();
        if (Ogre::SceneNode* parent = map.quad_node->getParentSceneNode()) {
            parent->removeChild(map.quad_node);
        }
        scene->destroySceneNode(map.quad_node);
    }
    if (map.camera_node) map.camera_node->detachAllObjects();
    if (map.camera) scene->destroyCamera(map.camera);
    map.light_quad.reset();
    map.light_mask.reset();
    map.listener.reset();
    Ogre::TextureManager::getSingleton().remove(map.texture);
    map = LightMap{};
}

void Renderer::Impl::apply_environment(const std::string& layout_path) {
    tl::formats::AdmFile rules;
    if (!read_rules(layout_path, rules, /*quiet=*/false)) return;

    // CAMERAMULT: the only camera figure the level data carries, a multiplier on a base
    // distance the data does not hold (see follow_subject()).
    camera_mult = rules.float_or("CAMERAMULT", 1.0f);
    // CGameClient::loadLevel: the camera's multiplier (+0x5c)
    // to CAMERAMULT (+0x98, load) and its dolly to CAMERAMULT * 28.5; no
    // template: 1 and 28.5.
    camera_dolly = kDefaultCameraDistance * camera_mult;
    // CGameClient::unloadCurrentLevel ends every shake
    // and speed modifier.
    //
    camera_shakes.clear();
    game_speed.clear();
    TL_LOG_INFO("  camera multiplier %.2f", static_cast<double>(camera_mult));

    // `ACTIVERANGEMOD` (`CLevelTemplateData::load`, `+0x98`), which
    // monster sight and chase ranges add on this level. Authored only by the
    // mine boss room and the Sunken hub (1000) and the fortress boss room (200).
    const float active_range_mod = rules.float_or("ACTIVERANGEMOD", 0.0f);
    level_active_range_mod = active_range_mod;
    for (tl::core::LevelScript* machine : level_machines) {
        machine->set_active_range_mod(active_range_mod);
    }
    // The active range:
    // INDOOR_UNIT_ACTIVE_RANGE under UNIT_LIGHT_FADE, else OUTDOOR_; CGameGlobals::reload
    // defaults 16 and 24; UNIT_NEAR_RANGE (24).
    {
        float indoor = 16.0f;
        float outdoor = 24.0f;
        float near_range = 24.0f;
        float shadow_range = 10.0f;
        float trigger_near = 40.0f;
        const std::vector<uint8_t> bytes = read_resource("media/GLOBALS.DAT.adm");
        if (!bytes.empty()) {
            const tl::formats::AdmFile globals = tl::formats::parse_adm(bytes.data(), bytes.size());
            if (globals.ok) {
                indoor = globals.float_or("INDOOR_UNIT_ACTIVE_RANGE", indoor);
                outdoor = globals.float_or("OUTDOOR_UNIT_ACTIVE_RANGE", outdoor);
                near_range = globals.float_or("UNIT_NEAR_RANGE", near_range);
                shadow_range = globals.float_or("UNIT_SHADOW_RANGE", shadow_range);
                trigger_near = globals.float_or("TRIGGER_NEAR_RANGE", trigger_near);
            }
        }
        // DIVERGENCE (2026-10-10): the ranges reach past the frame (growing with
        // CAMERAMULT), so units fade in before they come into view; the near range keeps
        // 8 beyond, as the near list is rebuilt once a second.
        {
            const float frame = 32.0f * std::max(1.0f, camera_mult);
            indoor = std::max(indoor, frame);
            outdoor = std::max(outdoor, frame);
            near_range = std::max(near_range, frame + 8.0f);
            trigger_near = std::max(trigger_near, near_range);
        }
        const bool fade = rules.bool_or("UNIT_LIGHT_FADE", true);
        level_unit_active_range = fade ? indoor : outdoor;
        level_unit_near_range = near_range;
        level_unit_shadow_range = shadow_range;
        level_light_fade = fade;
        // The light fade beside it (`CCharacter::update`):
        // UNIT_FADE_DISTANCE (`+0x8b4`, Linux `+0x764`, default 14.5) and
        // UNIT_FADE_START (`+0x8b8`, Linux `+0x768`, default 7), from the
        // template's defaults (`CLevelTemplateData::CLevelTemplateData`).
        level_unit_fade_distance = rules.float_or("UNIT_FADE_DISTANCE", 14.5f);
        level_unit_fade_start = rules.float_or("UNIT_FADE_START", 7.0f);
        level_indoor_range = indoor;
        level_outdoor_range = outdoor;
        level_trigger_near_range = trigger_near;
        for (tl::core::LevelScript* machine : level_machines) configure_unit_ranges(*machine);
        TL_LOG_INFO("  unit active range %.0f (%s)", static_cast<double>(fade ? indoor : outdoor),
                    fade ? "indoor" : "outdoor");
    }
    if (active_range_mod != 0.0f) {
        TL_LOG_INFO("  monster active range +%.0f", static_cast<double>(active_range_mod));
    }

    // Fill light: AMBIENT, which the game's shaders bind as ambient_light_colour.
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (rules.colour("AMBIENT", r, g, b)) {
        scene->setAmbientLight(Ogre::ColourValue(r, g, b));
        TL_LOG_INFO("  ambient light (%.2f, %.2f, %.2f)", r, g, b);
    }

    if (key_light && rules.colour("DIRECTIONAL", r, g, b)) {
        // CGameClient::refreshLighting: the key light is the template's
        // (+0x890) DIRECTIONAL times DIRECTIONAL INTENSITY, unclamped (lighting adds it
        // unclamped: patches/ogre/0015).
        const float intensity = rules.float_or("DIRECTIONAL INTENSITY", 1.0f);
        key_light->setDiffuseColour(Ogre::ColourValue(r * intensity, g * intensity, b * intensity));

        const Ogre::Vector3 from(rules.float_or("DIRECTIONAL X", 0.0f),
                                 rules.float_or("DIRECTIONAL Y", 1.0f),
                                 rules.float_or("DIRECTIONAL Z", 0.0f));
        if (!from.isZeroLength() && key_light_node) {
            // The data gives where the light is, so the direction it shines is
            // the way back towards the scene.
            key_light_node->setDirection(-from.normalisedCopy(), Ogre::Node::TS_WORLD);
        }
        TL_LOG_INFO("  directional (%.2f, %.2f, %.2f) x%.2f from (%.0f, %.0f, %.0f)", r, g, b,
                    static_cast<double>(intensity), from.x, from.y, from.z);
    }

    apply_scenery_fill(rules.root);
    setup_shadow_map(rules.root);
    setup_light_map(rules.root);

    // CGameClient::refreshLighting: cleared to the fog
    // colour, fog linear from FOG START to FOG END times 1.3 (1 in NETBOOK_MODE). Defaults:
    // 30, 42, 58; 24 and 50.
    {
        const float fog_r =
            static_cast<float>(static_cast<int32_t>(rules.integer_or("FOG RED", 30))) / 255.0f;
        const float fog_g =
            static_cast<float>(static_cast<int32_t>(rules.integer_or("FOG GREEN", 42))) / 255.0f;
        const float fog_b =
            static_cast<float>(static_cast<int32_t>(rules.integer_or("FOG BLUE", 58))) / 255.0f;
        const float start =
            static_cast<float>(static_cast<int32_t>(rules.integer_or("FOG START", 24)));
        const float end = static_cast<float>(static_cast<int32_t>(rules.integer_or("FOG END", 50)));
        const float scale = 1.3f;
        const Ogre::ColourValue fog(fog_r, fog_g, fog_b, 1.0f);
        scene->setFog(Ogre::FOG_LINEAR, fog, 0.0f, scale * start, end * scale);
        view_background = fog;
        if (window != nullptr) {
            for (unsigned short i = 0; i < window->getNumViewports(); ++i) {
                window->getViewport(i)->setBackgroundColour(fog);
            }
        }
        TL_LOG_INFO("  fog and background (%.2f, %.2f, %.2f) from %.1f to %.1f",
                    static_cast<double>(fog_r), static_cast<double>(fog_g),
                    static_cast<double>(fog_b), static_cast<double>(scale * start),
                    static_cast<double>(end * scale));
    }

    const std::string sky = rules.string_or("SKYBOX", "");
    if (!sky.empty() && !sky_node) {
        const std::string sky_path = resolve_archive_path(sky);
        if (sky_path.empty()) {
            TL_LOG_WARN("  skybox '%s' not in the archive", sky.c_str());
        } else {
            parse_sibling_material(sky_path);
            try {
                Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().load(sky_path, kGameGroup);
                const std::string dir = dir_of_asset(sky_path);
                auto& materials = Ogre::MaterialManager::getSingleton();
                for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
                    qualify_texture_paths(materials.getByName(sub->getMaterialName(), kGameGroup),
                                          dir, dir);
                }
                Ogre::Entity* entity = scene->createEntity(mesh);
                // A cached mesh hands its submeshes' *old* MaterialPtrs to a new entity.
                rebind_materials(entity);
                // Drawn before everything else and never written to the depth
                // buffer, so it stays behind the scene however large it is.
                entity->setRenderQueueGroup(Ogre::RENDER_QUEUE_SKIES_EARLY);
                entity->setCastShadows(false);
                sky_node = scene->getRootSceneNode()->createChildSceneNode();
                sky_node->attachObject(entity);
                TL_LOG_INFO("  skybox %s (radius %.1f)", sky_path.c_str(),
                            static_cast<double>(entity->getBoundingRadius()));
            } catch (const Ogre::Exception& e) {
                TL_LOG_WARN("  could not load skybox '%s': %s", sky_path.c_str(),
                            e.getDescription().c_str());
            }
        }
    }

    // CGameClient::loadLevel: WATER
    // (+0x6b0), POSTWATER (+0x6b8), MIDWATER (+0x6c0), generic models in queues 4, 0x59 and
    // 0x58, shown, at the origin.
    static const struct {
        const char* key;
        Ogre::uint8 queue;
    } kLevelModels[] = {{"WATER", 4}, {"POSTWATER", 0x59}, {"MIDWATER", 0x58}};
    for (const auto& model : kLevelModels) {
        const std::string wanted = rules.string_or(model.key, "");
        if (wanted.empty()) continue;
        const std::string path = resolve_archive_path(wanted);
        if (path.empty()) {
            TL_LOG_WARN("  %s '%s' not in the archive", model.key, wanted.c_str());
            continue;
        }
        parse_sibling_material(path);
        try {
            Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().load(path, kGameGroup);
            const std::string dir = dir_of_asset(path);
            auto& materials = Ogre::MaterialManager::getSingleton();
            for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
                qualify_texture_paths(materials.getByName(sub->getMaterialName(), kGameGroup), dir,
                                      dir);
            }
            Ogre::Entity* entity = scene->createEntity(mesh);
            rebind_materials(entity);
            entity->setRenderQueueGroup(static_cast<Ogre::uint8>(runic_queue(model.queue)));
            entity->setCastShadows(false);
            Ogre::SceneNode* node = scene->getRootSceneNode()->createChildSceneNode();
            node->setPosition(Ogre::Vector3::ZERO);
            node->attachObject(entity);
            // CLevel::setProjectorPass:
            // the light map on MIDWATER always, on WATER only with `LIGHT` in its file name;
            // CGenericModel::setAmbient of MATERIAL AMBIENT either way.
            const bool lit = model.queue == 0x58 ||
                             (model.queue == 4 &&
                              tl::core::upper_ascii(path).find("LIGHT") != std::string::npos);
            if (lit) {
                for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
                    const Ogre::MaterialPtr& material = entity->getSubEntity(i)->getMaterial();
                    if (material) add_light_pass(material);
                }
                std::set<Ogre::Material*> seen;
                set_model_ambient(entity, material_ambient, seen);
            }
            TL_LOG_INFO("  %s %s (queue %u%s)", model.key, path.c_str(),
                        static_cast<unsigned>(model.queue), lit ? ", light map" : "");
        } catch (const Ogre::Exception& e) {
            TL_LOG_WARN("  could not load %s '%s': %s", model.key, path.c_str(),
                        e.getDescription().c_str());
        }
    }
}

/// The process's `CDungeonManager`, built the first time anything asks, with
/// what the original does once at start on either side of it:
/// `CMasterResourceManager`'s constructor makes it over `media/dungeons/`,
/// and its constructor reloads it at once.
tl::core::DungeonManager& loaded_dungeon_manager() {
    tl::core::DungeonManager& manager = tl::core::dungeon_manager();
    if (manager.loaded) return manager;
    // `CGame::CGame` comes first: `srand(time(0))`.
    tl::core::seed_crt_from_time();
    // `getFileList(L"MEDIA/DUNGEONS/", list, L"*.dat", ...)`: in pak mode
    // `getFilesInDirectory` lists the archive's `*.dat.*` entries in the zip's
    // own order and cuts the compiled extension; `CDungeon::loadDungeon` reads
    // each. A file that is no dungeon still makes a `CDungeon` and takes a salt.
    std::vector<tl::core::DungeonDef> definitions;
    for (const std::string& path : archive_paths_in_listing_order("media/dungeons/", ".dat.adm")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        tl::core::DungeonDef definition;
        if (!bytes.empty()) {
            definition =
                tl::core::parse_dungeon_def(tl::formats::parse_adm(bytes.data(), bytes.size()));
        }
        definitions.push_back(std::move(definition));
    }
    manager.reload(std::move(definitions), tl::core::game_random(), tl::core::volatile_random());
    // And `CGameClient::CGameClient` after it: the game seed.
    tl::core::game_seed().seed =
        tl::core::roll_game_seed(tl::core::kLevelSeedSetting, tl::core::volatile_random());
    TL_LOG_INFO("dungeons: %zu loaded, each with its own salt", manager.dungeons.size());
    return manager;
}

/// The file-system questions a `Dungeon` and its templates ask, answered from
/// the mounted archive, and `CDungeonManager::getDungeonByName` from the
/// process's manager.
tl::core::DungeonFiles archive_dungeon_files() {
    // The level template, as CLevelTemplateData::load builds it, its file listings as
    // CFileSystem::getFilesInDirectory names files:
    // upper-cased, the compiled extension cut.
    tl::core::DungeonFiles dungeon_files;
    tl::core::LevelTemplateFiles& template_files = dungeon_files.layouts;
    template_files.exists = [](const std::string& layout) {
        return !renderer_detail::resolve_archive_path(layout + ".cmp").empty();
    };
    template_files.list_layouts = [](const std::string& folder) {
        std::vector<std::string> listed;
        for (const std::string& path :
             renderer_detail::archive_paths_under(folder, ".layout.cmp")) {
            std::string name = path;
            for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            name.resize(name.size() - 4);
            listed.push_back(std::move(name));
        }
        return listed;
    };
    dungeon_files.read_ruleset = [](const std::string& ruleset) {
        const std::string path = renderer_detail::resolve_archive_path(ruleset + ".adm");
        if (path.empty()) return tl::formats::AdmFile{};
        const std::vector<uint8_t> bytes = renderer_detail::read_resource(path);
        if (bytes.empty()) return tl::formats::AdmFile{};
        return tl::formats::parse_adm(bytes.data(), bytes.size());
    };
    dungeon_files.dungeon_by_name = [](const std::string& upper_name) {
        return tl::render::loaded_dungeon_manager().dungeon_by_name(upper_name);
    };
    return dungeon_files;
}

std::string Renderer::menu_layout_for(const std::string& dungeon_name, int32_t depth) {
    const tl::core::DungeonFiles files = archive_dungeon_files();

    // `CGameClient::loadMenuLevel`:
    // `setCurrentDungeon(name)`, `resetGameSeed(depth)`, then
    // `getLevelTemplateDataForDepth(depth)`. With no template,
    // `mainmenu_townrules.dat`.
    std::string rules = "media/layouts/mainmenus/mainmenu_townrules.dat";
    tl::core::RunicRandom& rng = tl::core::game_random();
    tl::core::GameSeed& seed = tl::core::game_seed();
    seed.current_dungeon = loaded_dungeon_manager().dungeon_by_name(dungeon_name);
    tl::core::reset_game_seed(seed, depth, rng);
    if (tl::core::Dungeon* dungeon = seed.current_dungeon) {
        const tl::core::LevelTemplate* found = dungeon->get_level_template_for_depth(
            static_cast<uint32_t>(std::max(0, depth)), files, rng);
        // A template's `MAINMENURULES` (X360 +0x824) is taken as it stands, empty or not.
        if (found != nullptr) rules = found->main_menu_rules;
    }

    // `CLevel::loadRoomLayout(rules, 0, 3)`: the rules' own template, and the
    // one room piece its single chunk holds.
    tl::core::LevelTemplate backdrop;
    backdrop.load(rules, files.read_ruleset(rules), files.layouts, rng);
    const tl::core::LevelLayout* layout = tl::core::choose_room_layout(
        backdrop, rng, tl::core::level_seed(tl::core::kLevelSeedSetting, seed, depth));
    std::string chosen;
    if (layout != nullptr && !layout->pieces.empty() &&
        layout->pieces[0] < backdrop.chunks.size()) {
        chosen = backdrop.chunks[layout->pieces[0]].path;
    }
    if (impl_ && !chosen.empty()) impl_->backdrop_rules[Impl::backdrop_key(chosen)] = rules;
    TL_LOG_INFO("main menu: '%s' depth %d -> %s -> %s", dungeon_name.c_str(), depth, rules.c_str(),
                chosen.empty() ? "(nothing)" : chosen.c_str());
    return chosen;
}

/// Whether the character is in shot, logged when not: checked at hand-over and at each
/// dumped frame.
bool subject_in_shot(const Ogre::Camera* camera, const Ogre::Entity* subject) {
    if (!camera || !subject || !subject->getParentSceneNode()) return true;
    const Ogre::AxisAlignedBox box = subject->getWorldBoundingBox(true);
    if (box.isNull()) return true;
    return camera->isVisible(box);
}

bool Renderer::enable_player_control() {
    Impl& m = *impl_;
    if (!m.subject || !m.subject_node) {
        TL_LOG_ERROR("nothing to control; load a character first");
        return false;
    }
    if (!m.camera_node) {
        TL_LOG_ERROR("no camera to follow with");
        return false;
    }

    m.player_control = true;
    m.load_player_stats();

    tl::core::Body body;
    // `media/units/players/base.dat` authors no COLLISION_RADIUS, so the player
    // is `CCharacter::unitInit`'s default sphere.
    body.radius = tl::core::kDefaultCollisionRadius;

    // The character's own height, from the mesh's bounds.
    const Ogre::AxisAlignedBox& bounds = m.subject->getMesh()->getBounds();
    if (!bounds.isNull())
        body.height = std::max(0.5f, bounds.getMaximum().y - bounds.getMinimum().y);
    m.player.set_body(body);

    // Stood on the collision: CCharacter::dropToGround,
    // the ray +100 to -100 about the unit.
    Ogre::Vector3 at = m.subject_node->getPosition();
    float surface = at.y;
    if (!m.collision.empty() && m.collision.ground_under(at.x, at.z, at.y + 100.0f, surface)) {
        at.y = surface;
        m.subject_node->setPosition(at);
    }
    m.player.place({at.x, at.y, at.z}, m.subject_node->getOrientation().getYaw().valueRadians());

    // The camera keeps the layout's framing, scaled by CAMERAMULT (follow_subject()). The
    // yaw axis is pinned, or lookAt accumulates roll.
    m.camera_node->setFixedYawAxis(true, Ogre::Vector3::UNIT_Y);

    if (m.layout_has_camera) {
        // The scene authored its own framing. Keep both halves of it: the offset
        // the camera sits at, and the point it looks at, each relative to the
        // character, so the whole framing travels rather than just the position.
        const Ogre::Vector3 eye = m.camera_node->getPosition();
        const Ogre::Vector3 forward = m.layout_camera_target - eye;
        if (forward.squaredLength() < 1e-6f) {
            TL_LOG_ERROR("the camera is standing on its own target; cannot derive a heading");
            return false;
        }
        m.camera_yaw = std::atan2(forward.x, forward.z);
        m.camera_offset = (eye - at) * m.camera_mult;
        m.camera_look_offset = m.layout_camera_target - at;
        TL_LOG_INFO("  camera framing from the layout's own Camera/Camera Target");
    } else {
        m.camera_offset = default_camera_offset(m.camera_mult);
        m.camera_yaw = std::atan2(-m.camera_offset.x, -m.camera_offset.z);

        // `CCameraControl::updateGameCamera` aims at the
        // focus itself; a captured town frame's view matrix agrees.
        m.camera_look_offset = Ogre::Vector3::ZERO;

        TL_LOG_INFO(
            "  this layout authors no camera; using the engine default "
            "(heading %.1f deg, distance %.1f x CAMERAMULT %.2f) "
            "from CCameraControl::updateGameCamera.",
            static_cast<double>(Ogre::Radian(m.camera_yaw).valueDegrees()),
            static_cast<double>(kDefaultCameraDistance), static_cast<double>(m.camera_mult));
    }
    // CGame::createCamera: `PlayerCam` clips at 1 and 90
    // (45 under the PC's netbook mode, which the console does not have). The near plane sets
    // how far a particle's `DEPTH BIAS` reaches towards the eye.
    m.camera->setNearClipDistance(kPlayerCamNear);
    m.camera->setFarClipDistance(kPlayerCamFar);
    m.camera_follows = true;
    m.follow_subject();

    // What the character's own unit file says it is holding, and what it
    // permanently wears. Before the log line below, so a run's opening lines
    // say what it is armed with.
    m.equip_starting_weapon();
    m.equip_carried_particles();

    // And the animal that came with it. Here rather than at load time because a
    // pet needs three things that only exist by now: somebody to stand beside,
    // a level to stand on, and a navigation grid to follow them across.
    if (!m.config.pet.empty()) m.spawn_pet(m.config.pet);

    // The player is a character the level took in like any other, so it
    // carries the same sheen (`CLevel::addCharacter`).
    //
    apply_rim_light(m.subject, m.rim_light_texture);
    // The player's model draws behind (`CCharacter::loadModel`:
    // `RENDERBEHIND`, which no player file authors), its weapon and
    // worn pieces with it; and the outline is drawn now the game is in play
    // (`CGame::renderableQueued` refuses it in game state 0).
    m.set_subject_render_behind(true, false);
    g_view_renderables.behind_in_play = true;
    if (m.config.invulnerable) m.god_mode = true;
    TL_LOG_INFO("player control on: body radius %.2f, height %.2f", body.radius, body.height);
    TL_LOG_INFO("  camera offset (%.2f, %.2f, %.2f), heading %.1f deg, CAMERAMULT %.2f",
                m.camera_offset.x, m.camera_offset.y, m.camera_offset.z,
                m.camera_yaw * 180.0f / 3.14159265358979323846f, m.camera_mult);
    if (m.collision.empty()) {
        TL_LOG_WARN("  walking with no collision geometry loaded");
    }
    if (!subject_in_shot(m.camera, m.subject)) {
        TL_LOG_ERROR(
            "  the character is NOT in the camera's frustum - the framing is wrong, "
            "and the frame will not show what you expect");
    }
    if (m.automap_initialized && m.config.minimap) {
        m.automap.show();
    }
    return true;
}

}  // namespace tl::render

namespace tl::render {

// PORT: an automap tile the layout places (core/world.h).
void Renderer::Impl::view_automap_add_tile(int type, const ogre::Vector3& at,
                                           const ogre::Vector3& dir, bool a, bool b) {
    automap.add_tile(type, to_ogre(at), to_ogre(dir), a, b);
}

// PORT: the player's marker on the automap where the layout puts it (core/world.h).
void Renderer::Impl::view_automap_player(const ogre::Vector3& at) {
    automap.set_player_position(to_ogre(at), Ogre::Quaternion::IDENTITY);
    automap.update(to_ogre(at), 0.0f);
}

// PORT: the camera, the sky and the subject for a top-level layout (core/world.h).
void Renderer::Impl::view_place_viewer(const tl::formats::CmpScene& layout,
                                       const ogre::Vector3& origin) {
    aim_camera(layout, to_ogre(origin));
    // The sky has no position of its own; it follows the viewer.
    if (sky_node && camera_node) sky_node->setPosition(camera_node->getPosition());

    // Player Start is where the game puts the character, so it is where the
    // subject belongs - rather than the origin, which in this scene is tens
    // of units away from anything.
    if (const tl::formats::CmpNode* start = find_spawn(layout)) {
        stand_subject_at(*start, to_ogre(origin));
        if (subject_node != nullptr) {
            const Ogre::Vector3 f = subject_node->getOrientation() * Ogre::Vector3::UNIT_Z;
            player_start_yaw = std::atan2(f.x, f.z);
        }
    }
}

}  // namespace tl::render

namespace tl::render {

// PORT: the meshes a layout places, made and stood in the scene (core/world.h).
void Renderer::Impl::view_place_meshes(const std::vector<MeshPlacement>& placements,
                                       size_t& animated_props, size_t& idled) {
    // Every material script first, then every entity: a material may be defined by a
    // script of a later node.
    for (const auto& placement : placements) {
        parse_sibling_material(placement.mesh);
    }

    // Collision meshes' own material scripts are not parsed: a collision mesh is
    // never drawn, and the submesh material names that decide its faces come
    // from the serializer itself (see add_collision_mesh()).
    for (const auto& placement_from_layout : placements) {
        view_place_one_mesh(placement_from_layout, animated_props, idled);
    }
}

// PORT: one of view_place_meshes()' placements.
void Renderer::Impl::view_place_one_mesh(const MeshPlacement& from_layout, size_t& animated_props,
                                         size_t& idled) {
    MeshPlacement placement = from_layout;
    Ogre::Entity* entity = place_mesh(placement);

    // CRoomPiece::setMeshFilePath: a piece whose model has
    // any animation plays IDLE, looped, at 1.0. Gated on hasSkeleton(), so the lookup is rare.
    if (entity != nullptr && placement.room_piece && placement.animation.empty() &&
        entity->hasSkeleton()) {
        placement.animation = kUnitIdleAnimation;
        placement.idle_fallback = true;
        placement.loops = true;
        placement.animation_rate = 1.0f;
    }

    const bool armed = placement.unit.empty() ? true : stats_for_unit(placement.unit).armed;
    Ogre::AnimationState* started = nullptr;
    if (entity && !placement.animation.empty() &&
        play_scene_animation(entity, placement.mesh, placement.animation, placement.loops,
                             placement.animation_rate, placement.idle_fallback, &started, armed)) {
        remember_state(entity, placement.animation, started);
        ++animated_props;
        if (placement.idle_fallback) ++idled;
        if (placement.room_piece) {
            for (SceneAnimation& animation : scene_animations) {
                if (animation.entity == entity) animation.room_piece = true;
            }
        }
    }
    if (entity && !placement.unit.empty()) dress_placed_unit(entity, placement, started, armed);
}

// PORT: a placed unit's scene entry and looks.
void Renderer::Impl::dress_placed_unit(Ogre::Entity* entity, const MeshPlacement& placement,
                                       Ogre::AnimationState* started, bool armed) {
    scene_units.emplace(
        placement.unit,
        SceneUnit{entity, placement.mesh, placement.animation_rate, placement.node_id,
                  placement.animation, placement.loops, placement.animation, started, 0.0f, armed});
    scene_units_changed = true;
    {
        const tl::core::UnitStats held = stats_for_unit(placement.unit);
        equip_unit_items(entity, placement.unit, held.right_hand, held.left_hand);
        apply_render_always(entity, held.render_always);
        // `CCharacter::loadModel`: `setRenderBehind`
        // with its `RENDERBEHIND`.
        set_render_behind(entity, held.render_behind, outline_hostile(held));
    }
    apply_rim_light(entity, rim_light_texture);
    if (!placement.trigger_resting_layout.empty()) {
        trigger_layout_props.emplace_back(entity, placement.unit);
        pending_trigger_layouts.emplace_back(entity, placement.unit);
    }
}

// PORT: a Unit Trigger's own layout's systems are the trigger's (core/world.h).
void Renderer::Impl::view_own_trigger_particles(size_t before, int64_t node, const NodeMap& sink) {
    for (size_t i = before; i < particle_systems.size(); ++i) {
        particle_index_remove(i);
        particle_systems[i].owner = node;
        particle_systems[i].owner_nodes = &sink;
        particle_systems[i].trigger_own_layout = true;
        particle_index_add(i);
    }
}

// PORT: a link's particle file once placed: its systems' link, its `CParticle` (core/world.h).
void Renderer::Impl::view_settle_link_particles(size_t systems_before, int64_t owner,
                                                const std::string& reference,
                                                const ogre::Vector3& origin,
                                                const ogre::Quaternion& basis,
                                                tl::core::LinkParticleSettle what) {
    const Ogre::Vector3 origin_o = to_ogre(origin);
    const Ogre::Quaternion basis_o = to_ogre(basis);
    if (what.particle) arm_particle_timer(systems_before, particle_systems.size());
    if (what.particle) link_particle_systems(systems_before, owner, origin_o, basis_o);
    if (what.cparticle) {
        register_link_cparticle(systems_before, owner, reference, origin_o, what.stopped);
    }
    if (what.stopped) stop_particle_systems(systems_before);
}

// PORT: a link's own systems (an inner link's have their own owner) learn which file's events
// name the link, and where they stand from it.
void Renderer::Impl::link_particle_systems(size_t systems_before, int64_t owner,
                                           const Ogre::Vector3& origin,
                                           const Ogre::Quaternion& basis) {
    const Ogre::Quaternion inverse = basis.Inverse();
    for (size_t i = systems_before; i < particle_systems.size(); ++i) {
        ParticleSystem& system = particle_systems[i];
        if (system.owner != owner || system.claimed) continue;
        Ogre::SceneNode* holder = system.holder();
        if (holder == nullptr) continue;
        particle_index_remove(i);
        system.owner_nodes = node_sink != nullptr ? node_sink : &scripted_nodes;
        particle_index_add(i);
        system.link_position = origin;
        system.link_orientation = basis;
        system.offset_from_link = inverse * (holder->getPosition() - origin);
        system.turn_from_link = inverse * holder->getOrientation();
    }
}

// PORT: a link's systems as one `CParticle`.
void Renderer::Impl::register_link_cparticle(size_t systems_before, int64_t owner,
                                             const std::string& reference,
                                             const Ogre::Vector3& origin, bool stopped) {
    const int64_t link_owner = owner;
    const uint32_t id = register_cparticle(reference, systems_before, origin,
                                           [link_owner](const ParticleSystem& system) {
                                               return system.owner == link_owner && !system.claimed;
                                           });
    if (id == 0 || stopped) return;
    // `CLayoutDescriptor::descriptorSceneActivated`: `start`
    // when `START ON LOAD`, which is the `CParticle`'s `Start`.
    cparticle_started(id);
    // `CLevel::findParticles`: a
    // link that starts on load joins the level's list unless one of
    // its `Particle` nodes has a `NO CULLING` technique.
    bool no_cull = false;
    for (size_t i = systems_before; i < particle_systems.size(); ++i) {
        const ParticleSystem& system = particle_systems[i];
        if (system.cparticle != id) continue;
        if (torchlight_particle_no_cull(system.pu)) no_cull = true;
    }
    PlacedParticle& placed = placed_particles[id];
    if (!no_cull && !placed.listed) {
        placed.listed = true;
        ++listed_particles;
    }
}

// PORT: a link's systems stopped.
void Renderer::Impl::stop_particle_systems(size_t systems_before) {
    for (size_t i = systems_before; i < particle_systems.size(); ++i) {
        ParticleSystem& system = particle_systems[i];
        // `CLayoutDescriptor::descriptorSceneActivated` stops it.
        if (system.pu.system != nullptr && !system.claimed) {
            stop_torchlight_particle(system.pu);
            system.finished = true;
        }
    }
}

// PORT: whether particles are cached yet (core/world.h).
bool Renderer::Impl::view_caching_particles() const { return renderer_detail::caching_particles(); }

// PORT: OGRE's clock, for the log's timings (core/world.h).
unsigned long Renderer::Impl::view_milliseconds() const {
    return root && root->getTimer() ? root->getTimer()->getMilliseconds() : 0;
}

}  // namespace tl::render

namespace tl::render {

// PORT: the automap drawn from the grid just baked; a town's whole (core/world.h).
void Renderer::Impl::view_automap_create() { automap.create(passability, level_is_town); }

// PORT: a line of --logic-trace's file, when one is being written (core/world.h).
void Renderer::Impl::logic_trace_text(const char* text) {
    if (logic_trace != nullptr) std::fputs(text, logic_trace);
}

}  // namespace tl::render

namespace tl::render {

// PORT: the level particle's systems hung on one node that follows the player (core/world.h).
size_t Renderer::Impl::view_level_particle_node(size_t systems_before) {
    level_particle_node = scene->getRootSceneNode()->createChildSceneNode();
    size_t riding = 0;
    for (size_t i = systems_before; i < particle_systems.size(); ++i) {
        Ogre::SceneNode* holder = particle_systems[i].holder();
        if (holder == nullptr || holder->getParent() == level_particle_node) continue;
        if (Ogre::Node* up = holder->getParent()) up->removeChild(holder);
        level_particle_node->addChild(holder);
        ++riding;
    }
    return riding;
}

}  // namespace tl::render

namespace tl::render {

// PORT: the subject stood at the dungeon's entrance, facing in (core/world.h).
void Renderer::Impl::view_stand_subject(const ogre::Vector3& at, const ogre::Vector3& facing) {
    if (subject_node == nullptr) return;
    subject_node->setPosition(to_ogre(at));
    if (!facing.isZeroLength()) {
        subject_node->setOrientation(Ogre::Vector3::UNIT_Z.getRotationTo(to_ogre(facing)));
    }
}

}  // namespace tl::render
