#include "render/particle_wrappers.h"

#include "core/log.h"

#if defined(TL_HAVE_PARTICLEUNIVERSE)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

#include <OgreCamera.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>

#include "ParticleAffectors/ParticleUniverseAlignAffector.h"
#include "ParticleAffectors/ParticleUniverseBoxCollider.h"
#include "ParticleAffectors/ParticleUniverseColourAffector.h"
#include "ParticleAffectors/ParticleUniverseGeometryRotator.h"
#include "ParticleAffectors/ParticleUniverseGravityAffector.h"
#include "ParticleAffectors/ParticleUniverseJetAffector.h"
#include "ParticleAffectors/ParticleUniverseLineAffector.h"
#include "ParticleAffectors/ParticleUniverseLinearForceAffector.h"
#include "ParticleAffectors/ParticleUniversePlaneCollider.h"
#include "ParticleAffectors/ParticleUniverseScaleAffector.h"
#include "ParticleAffectors/ParticleUniverseSineForceAffector.h"
#include "ParticleAffectors/ParticleUniverseSphereCollider.h"
#include "ParticleAffectors/ParticleUniverseTextureAnimator.h"
#include "ParticleAffectors/ParticleUniverseTextureRotator.h"
#include "ParticleAffectors/ParticleUniverseVortexAffector.h"
#include "ParticleEmitters/ParticleUniverseBoxEmitter.h"
#include "ParticleEmitters/ParticleUniverseCircleEmitter.h"
#include "ParticleEmitters/ParticleUniverseLineEmitter.h"
#include "ParticleEmitters/ParticleUniverseSphereSurfaceEmitter.h"
#include "ParticleRenderers/ParticleUniverseBillboardRenderer.h"
#include "ParticleRenderers/ParticleUniverseEntityRenderer.h"
#include "ParticleRenderers/ParticleUniverseRibbonTrailRenderer.h"
#include "ParticleUniverseSystem.h"
#include "ParticleUniverseSystemManager.h"
#include "formats/cmp.h"
#include "render/depth_buffer.h"

namespace tl::render {

namespace {

namespace PU = ParticleUniverse;
using tl::formats::CmpField;
using tl::formats::CmpNode;
using tl::formats::CmpScene;
namespace cmp_field = tl::formats::cmp_field;
namespace cmp_type = tl::formats::cmp_type;

// ---------------------------------------------------------------------------
// The editor's enumerations, as the compiled layouts spell them. A node stores
// the text; the loader turns it into the setter's number with the descriptor's
// `...IDByString` interpreter.

/// `gPARTICLE_RENDER_TYPE_NAMES`, `CParticleTechWrapper::setRenderType`.
///
constexpr const char* kRenderTypes[] = {"Billboard",
                                        "Billboard Up",
                                        "Billboard Forward",
                                        "Billboard Up Camera",
                                        "Billboard Forward Camera",
                                        "Billboard Self",
                                        "Billboard Common",
                                        "Billboard Shape",
                                        "Box",
                                        "Sphere",
                                        "Entity",
                                        "EntityWorld",
                                        "RibbonTrail"};
constexpr unsigned kRenderTypeEntity = 10;
constexpr unsigned kRenderTypeEntityWorld = 11;
constexpr unsigned kRenderTypeRibbonTrail = 12;

/// `gPARTICLE_MATERIAL_TYPES` (`CParticleTechWrapper::setParticleRenderStyle`).
constexpr const char* kRenderStyles[] = {"Alpha", "Normal", "Additive", "Modulate"};
constexpr unsigned kStyleAlpha = 0;
constexpr unsigned kStyleAdditive = 2;
constexpr unsigned kStyleModulate = 3;

/// `gEMITTER_TYPES` (`CParticleEmitterWrapper::setEmitterType`):
/// also the ParticleUniverse emitter names.
constexpr const char* kEmitterTypes[] = {"Point", "Box", "Circle", "Line", "SphereSurface"};
constexpr int kEmitterBox = 1;
constexpr int kEmitterCircle = 2;
constexpr int kEmitterLine = 3;
constexpr int kEmitterSphereSurface = 4;

/// `gPARTICLE_BILLBOARD_ORIGIN_POSITIONS`: OGRE's nine origins, from Top Left.
constexpr const char* kOrigins[] = {"Top Left",    "Top Center",    "Top Right",
                                    "Center Left", "Center",        "Center Right",
                                    "Bottom Left", "Bottom Center", "Bottom Right"};

/// `gPARTICLE_AFFECTOR_FORCE_APPLICATION_TYPES`
/// (`CForceAffectorDescriptor::GetForceApplicationIDByString`).
constexpr const char* kForceApplications[] = {"Average", "Add"};

/// `gPARTICLE_COLLISION_TYPE` (`CColliderDescriptor::GetParticleCollisionTypeIDByString`
/// 0 when not found): the library's CT_NONE, CT_BOUNCE, CT_FLOW.
constexpr const char* kCollisionTypes[] = {"Stop", "Bounce", "Flow"};

/// `gPARTICLE_INTERSECTION_TYPE` (`GetParticleIntersectionTypeIDByString`): the
/// library's IT_POINT, IT_BOX.
constexpr const char* kIntersectionTypes[] = {"Fast", "Box"};

template <size_t N>
int index_of(const char* const (&names)[N], const std::string& text, int fallback) {
    for (size_t i = 0; i < N; ++i) {
        if (text == names[i]) return static_cast<int>(i);
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Reading a node's fields. A field the node does not carry keeps the
// constructor's value, as a shorter node written by an older editor does.

bool read_float(const CmpNode& node, size_t index, float& out) {
    const CmpField* f = node.field(index);
    if (f == nullptr || f->count() != 1) return false;
    out = f->as_float();
    return true;
}

bool read_uint(const CmpNode& node, size_t index, uint32_t& out) {
    const CmpField* f = node.field(index);
    if (f == nullptr || f->count() != 1) return false;
    out = f->as_uint();
    return true;
}

bool read_bool(const CmpNode& node, size_t index, bool& out) {
    const CmpField* f = node.field(index);
    if (f == nullptr || f->count() != 1) return false;
    out = f->as_bool();
    return true;
}

bool read_vector(const CmpNode& node, size_t index, Ogre::Vector3& out) {
    const CmpField* f = node.field(index);
    if (f == nullptr || f->count() != 3) return false;
    out = Ogre::Vector3(f->as_float(0), f->as_float(1), f->as_float(2));
    return true;
}

bool read_text(const CmpNode& node, size_t index, std::string& out) {
    const CmpField* f = node.field(index);
    if (f == nullptr) return false;
    out = f->as_text();
    return true;
}

std::vector<float> read_floats(const CmpNode& node, size_t index) {
    std::vector<float> values;
    if (const CmpField* f = node.field(index)) {
        values.reserve(f->count());
        for (size_t i = 0; i < f->count(); ++i) values.push_back(f->as_float(i));
    }
    return values;
}

/// `getDynPropFromArray`: the first value
/// picks the kind - 0 fixed, 1 random, 2/3 a (time, value) curve (linear /
/// spline), 4/5 Torchlight's random curve of (time, min, max) - and anything
/// malformed is a fixed 0.
PU::DynamicAttribute* dynamic_from_array(const float* values, size_t count) {
    const auto fixed = [](float value) {
        auto* attribute = PU_NEW_T(PU::DynamicAttributeFixed, Ogre::MEMCATEGORY_SCENE_OBJECTS)();
        attribute->setValue(value);
        return attribute;
    };
    if (count == 0) return fixed(0.0f);
    const int kind = static_cast<int>(values[0]);
    const int rest = static_cast<int>(count) - 1;
    switch (kind) {
        case 0: return fixed(count > 1 ? values[1] : 0.0f);
        case 1:
            if (rest > 1) {
                auto* attribute =
                    PU_NEW_T(PU::DynamicAttributeRandom, Ogre::MEMCATEGORY_SCENE_OBJECTS)();
                attribute->setMinMax(values[1], values[2]);
                return attribute;
            }
            break;
        case 2:
        case 3:
            if (rest > 1 && (rest & 1) == 0) {
                auto* attribute =
                    PU_NEW_T(PU::DynamicAttributeCurved, Ogre::MEMCATEGORY_SCENE_OBJECTS)(
                        kind != 2 ? PU::IT_SPLINE : PU::IT_LINEAR);
                for (int i = 0; i < rest; i += 2)
                    attribute->addControlPoint(values[i + 1], values[i + 2]);
                attribute->processControlPoints();
                return attribute;
            }
            break;
        case 4:
        case 5:
            if (rest > 1 && rest == (rest / 3) * 3) {
                auto* attribute =
                    PU_NEW_T(PU::DynamicAttributeRandomCurved, Ogre::MEMCATEGORY_SCENE_OBJECTS)(
                        kind != 4 ? PU::IT_SPLINE : PU::IT_LINEAR);
                for (int i = 0; i < rest; i += 3) {
                    attribute->addControlPoint(values[i + 1], values[i + 2], values[i + 3]);
                }
                attribute->processControlPoints();
                return attribute;
            }
            break;
        default: break;
    }
    return fixed(0.0f);
}

PU::DynamicAttribute* dynamic_from_array(const std::vector<float>& values) {
    return dynamic_from_array(values.data(), values.size());
}

/// A dynamic property field, or nullptr when the node does not carry it.
PU::DynamicAttribute* read_dynamic(const CmpNode& node, size_t index) {
    const CmpField* f = node.field(index);
    if (f == nullptr) return nullptr;
    return dynamic_from_array(read_floats(node, index));
}

// ---------------------------------------------------------------------------
// CParticleTechWrapper

/// The wrapper's own state (`CParticleTechWrapper`), with the constructor's values.
///
struct TechWrapper {
    PU::ParticleSystem* system = nullptr;
    PU::ParticleTechnique* technique = nullptr;
    unsigned render_type = 13;                   // +0x148, none yet
    unsigned render_queue = 0x5a;                // +0x14c
    unsigned rotation_type = 0;                  // +0x150, Geometry
    unsigned origin = 4;                         // +0x154, Center
    float ribbon_length = 10.0f;                 // +0x158
    float ribbon_width = 1.0f;                   // +0x15c
    unsigned ribbon_segments = 3;                // +0x160
    bool ribbon_fade = false;                    // +0x164
    bool ribbon_local = false;                   // +0x165
    int flipbook_width = 1;                      // +0x168
    int flipbook_height = 1;                     // +0x16c
    float u_scroll = 0.0f;                       // +0x170
    float v_scroll = 0.0f;                       // +0x174
    bool sorts = false;                          // +0x178
    PU::BillboardRenderer* billboard = nullptr;  // +0x180
    PU::EntityRenderer* entity = nullptr;        // +0x198
    PU::RibbonTrailRenderer* ribbon = nullptr;   // +0x1a0
    // +0x1a8, the entity renderer's `CGenericModel`, made by `setModelPath`: here
    // its path (+0x110) and the mesh it loaded, by the name the port loaded it
    // under (`ParticleBuildContext::load_mesh`).
    bool has_model = false;
    std::string model_path;
    std::string model_mesh;
    std::string texture;                         // +0x1b0
    std::string mesh_path;                       // +0x1b8
    unsigned render_style = 1;                   // +0x1c8, Normal
    bool depth_check = true;                     // +0x1cc
    bool is_light = false;                       // +0x1cd
    unsigned sort_index = 0;                     // +0x1d0
    float depth_bias = 0.0f;                     // +0x1d4
    std::vector<PU::ParticleEmitter*> emitters;  // +0x1d8, the emitter wrappers' emitters
    // +0x1f0, the affector wrappers' affectors (`CAffectorWrapper +0x110`), by
    // node id.
    std::vector<std::pair<int64_t, PU::ParticleAffector*>> affectors;
    bool enabled = true;  // +0x82
    const ParticleBuildContext* context = nullptr;
    Ogre::SceneNode* node = nullptr;

    // `CParticleTechWrapper::addEmitter`: an emitter not on
    // the list (`+0x1d8`) is appended, and the technique (`+0x140`) is handed it either way -
    // twice for one already listed, as the original does. The editor's stop and restart, and the
    // emitter wrapper's scene node, are left out.
    void add_emitter(PU::ParticleEmitter* emitter) {
        if (emitter == nullptr) return;
        if (std::find(emitters.begin(), emitters.end(), emitter) == emitters.end()) {
            emitters.push_back(emitter);
        }
        if (technique != nullptr) technique->addEmitter(emitter);
    }
    // `CParticleTechWrapper::removeEmitter`:
    // off the list, the last one taking its place, and out of the technique
    // when it is this one's.
    void remove_emitter(PU::ParticleEmitter* emitter) {
        if (emitter == nullptr) return;
        const auto it = std::find(emitters.begin(), emitters.end(), emitter);
        if (it != emitters.end()) {
            *it = emitters.back();
            emitters.pop_back();
        }
        if (technique != nullptr && emitter->getParentTechnique() != nullptr &&
            emitter->getParentTechnique() == technique) {
            technique->removeEmitter(emitter);
        }
    }
    // `CParticleTechWrapper::addAffector`: one
    // already listed (`+0x1f0`) changes nothing; else appended and handed to
    // the technique. The editor's stop and restart are left out.
    void add_affector(int64_t node, PU::ParticleAffector* affector) {
        if (affector == nullptr) return;
        for (const auto& entry : affectors) {
            if (entry.second == affector) return;
        }
        affectors.emplace_back(node, affector);
        if (technique != nullptr) technique->addAffector(affector);
    }
    // `CParticleTechWrapper::removeAffector`:
    // off the list, the last one taking its place, and out of the technique
    // when it is this one's.
    void remove_affector(PU::ParticleAffector* affector) {
        if (affector == nullptr) return;
        for (auto& entry : affectors) {
            if (entry.second != affector) continue;
            entry = affectors.back();
            affectors.pop_back();
            break;
        }
        if (technique != nullptr && affector->getParentTechnique() != nullptr &&
            affector->getParentTechnique() == technique) {
            technique->removeAffector(affector);
        }
    }

    // `CParticleTechWrapper::setRenderQueue`: clamped to 100, handed
    // to the renderer - drawn in the port's own queue layout, with the original's
    // number kept beside it for the technique's cull radius.
    void set_render_queue(unsigned queue) {
        render_queue = std::min(queue, 100u);
        if (PU::ParticleRenderer* renderer = technique->getRenderer()) {
            const unsigned drawn =
                context->map_render_queue ? context->map_render_queue(render_queue) : render_queue;
            renderer->setRenderQueueGroup(static_cast<Ogre::uint8>(drawn));
            renderer->setTorchlightRenderQueueGroup(static_cast<Ogre::uint8>(render_queue));
        }
    }

    // `CParticleTechWrapper::setSortIndex` and `setAsLight`:
    // 90 + the index, 95 + it for a light.
    void set_sort_index(unsigned index) {
        sort_index = index;
        set_render_queue(index + (is_light ? 0x5f : 0x5a));
    }
    void set_as_light(bool light) {
        is_light = light;
        set_render_queue(sort_index + (light ? 0x5f : 0x5a));
    }

    // `CParticleTechWrapper::setSorts`.
    void set_sorts(bool value) {
        sorts = value;
        if (PU::ParticleRenderer* renderer = technique->getRenderer()) renderer->setSorted(value);
    }

    // `CParticleTechWrapper::setFlipbookWidth`,
    // `CParticleTechWrapper::setFlipbookHeight`.
    void set_flipbook_width(int width) {
        flipbook_width = width;
        technique->setFlipbookWidthHeight(width, flipbook_height);
    }
    void set_flipbook_height(int height) {
        flipbook_height = height;
        technique->setFlipbookWidthHeight(flipbook_width, height);
    }

    // `CParticleTechWrapper::setBillboardRotationType`,
    // `CParticleTechWrapper::setBillboardOrigin`.
    void set_rotation_type(unsigned type) {
        rotation_type = type;
        if (billboard) {
            billboard->setBillboardRotationType(type == 1 ? Ogre::BBR_TEXCOORD : Ogre::BBR_VERTEX);
        }
    }
    void set_origin(unsigned value) {
        origin = value;
        if (billboard) billboard->setBillboardOrigin(static_cast<Ogre::BillboardOrigin>(value));
    }

    // Stored, and applied to a ribbon renderer: `CParticleTechWrapper::setRibbonEndFade`,
    // colours of 0 when fading, 1 when not;
    // `CParticleTechWrapper::setRibbonLocalTrail`,
    // `CParticleTechWrapper::setRibbonLength`, `CParticleTechWrapper::setRibbonWidth`,
    // `CParticleTechWrapper::setRibbonMaxChains` (at most 100).
    void set_ribbon_fade(bool fade) {
        ribbon_fade = fade;
        if (!ribbon) return;
        const Ogre::ColourValue end =
            fade ? Ogre::ColourValue(0, 0, 0, 0) : Ogre::ColourValue(1, 1, 1, 1);
        ribbon->setStartColour(end);
        ribbon->setEndColour(end);
    }
    void set_ribbon_local(bool local) {
        ribbon_local = local;
        if (ribbon) ribbon->setTrailLocal(local);
    }
    void set_ribbon_length(float length) {
        ribbon_length = length;
        if (ribbon) ribbon->setTrailLength(length);
    }
    void set_ribbon_width(float width) {
        ribbon_width = width;
        if (ribbon) ribbon->setTrailWidth(width);
    }
    void set_ribbon_segments(unsigned segments) {
        ribbon_segments = std::min(segments, 100u);
        if (ribbon) ribbon->setMaxChainElements(ribbon_segments);
    }

    /// `CParticleTechWrapper::setRenderType`.
    /// Box (8) and Sphere (9) keep the renderer as made (the wrapper only
    /// keeps a pointer to it, `+0x188` / `+0x190`).
    void set_render_type(unsigned type) {
        if (type > 12) return;
        if (!emitters.empty()) technique->setKeepLocal(emitters.front()->isKeepLocal());
        if (render_type != type) {
            billboard = nullptr;
            entity = nullptr;
            ribbon = nullptr;
            // Types 0-7 are all ParticleUniverse's billboard renderer, and
            // EntityWorld is its entity renderer.
            const char* name = type < 8                         ? kRenderTypes[0]
                               : type == kRenderTypeEntityWorld ? kRenderTypes[kRenderTypeEntity]
                                                                : kRenderTypes[type];
            technique->setRenderer(name);
        }
        render_type = type;
        if (type == kRenderTypeRibbonTrail) {
            ribbon = static_cast<PU::RibbonTrailRenderer*>(technique->getRenderer());
            ribbon->setInitialColour(Ogre::ColourValue(1, 1, 1, 1));
            ribbon->setColourChange(Ogre::ColourValue(0, 0, 0, 0));
            ribbon->setUseVertexColours(true);
            set_ribbon_fade(ribbon_fade);
            set_ribbon_local(ribbon_local);
            set_flipbook_width(flipbook_width);
            set_flipbook_height(flipbook_height);
            technique->setDefaultHeight(technique->getDefaultHeight());
            technique->setDefaultWidth(technique->getDefaultWidth());
            set_ribbon_length(ribbon_length);
            set_ribbon_width(ribbon_width);
            set_ribbon_segments(ribbon_segments);
        } else if (type == kRenderTypeEntity || type == kRenderTypeEntityWorld) {
            // Nothing until `setModelPath` has made the model. The model loads the
            // mesh when its path is not the one wanted, and is hidden (the port
            // makes no model to hide); the renderer is given the loaded mesh's
            // cleaned path. Entity turns each particle toward its direction of
            // travel, EntityWorld gives it the particle's own orientation.
            if (has_model) {
                if (model_path != mesh_path) {
                    model_path = mesh_path;
                    model_mesh = context->load_mesh ? context->load_mesh(mesh_path) : std::string();
                }
                entity = static_cast<PU::EntityRenderer*>(technique->getRenderer());
                entity->setEntityOrientationType(type == kRenderTypeEntity
                                                     ? PU::EntityRenderer::ENT_ORIENTED_SELF
                                                     : PU::EntityRenderer::ENT_ORIENTED_SHAPE);
                entity->setMeshName(model_mesh);
            }
        } else if (type < 8) {
            billboard = static_cast<PU::BillboardRenderer*>(technique->getRenderer());
            set_rotation_type(rotation_type);
            set_origin(origin);
            switch (type) {
                case 1:  // Billboard Up
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_TL_UP);
                    billboard->setCommonDirection(Ogre::Vector3::UNIT_Y);
                    billboard->setCommonUpVector(Ogre::Vector3::UNIT_Z);
                    break;
                case 2:  // Billboard Forward: the node's own forward and up
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_TL_FORWARD);
                    billboard->setCommonDirection(node->getOrientation() * Ogre::Vector3::UNIT_Z);
                    billboard->setCommonUpVector(node->getOrientation() * Ogre::Vector3::UNIT_Y);
                    break;
                case 3:  // Billboard Up Camera
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_TL_UP_CAMERA);
                    billboard->setCommonDirection(Ogre::Vector3::UNIT_Z);
                    billboard->setCommonUpVector(Ogre::Vector3::UNIT_Y);
                    break;
                case 4:  // Billboard Forward Camera
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_TL_FORWARD_CAMERA);
                    billboard->setCommonUpVector(Ogre::Vector3::UNIT_Y);
                    billboard->setCommonDirection(Ogre::Vector3::UNIT_Z);
                    break;
                case 5:  // Billboard Self
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_ORIENTED_SELF);
                    break;
                case 7:  // Billboard Shape
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_ORIENTED_SHAPE);
                    break;
                case 6:  // Billboard Common
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_ORIENTED_COMMON);
                    break;
                default:  // Billboard
                    billboard->setBillboardType(PU::BillboardRenderer::BBT_POINT);
                    billboard->setCommonDirection(Ogre::Vector3::ZERO);
                    billboard->setCommonUpVector(Ogre::Vector3::ZERO);
                    break;
            }
        }
        if (type != kRenderTypeRibbonTrail) {
            set_flipbook_width(flipbook_width);
            set_flipbook_height(flipbook_height);
            technique->setDefaultHeight(technique->getDefaultHeight());
            technique->setDefaultWidth(technique->getDefaultWidth());
        }
        technique->notifyCurrentCamera(context->camera);
    }

    /// `CParticleTechWrapper::setModelPath`:
    /// the path stored; the first non-empty one makes the model and applies the
    /// render type again. A later path is stored and waits for the next
    /// `setRenderType`.
    void set_model_path(const std::string& path) {
        mesh_path = path;
        if (has_model || path.empty()) return;
        has_model = true;
        set_render_type(render_type);
    }

    /// `CParticleTechWrapper::createTextureMaterial`:
    /// one material per texture, style,
    /// depth check and scroll rates, shared by name. Unlit, unfogged
    /// (`setFog(true, FOG_NONE, White, 0.001, 0, 1)`), never writing depth;
    /// `Additive` blends `one one`, `Alpha` by the texture's alpha, `Modulate`
    /// multiplies the scene (`zero src_colour`), `Normal` replaces.
    void create_texture_material() {
        std::string path;
        if (!texture.empty() && context->resolve_texture) path = context->resolve_texture(texture);
        set_render_queue(render_queue);
        set_sorts(sorts);

        std::string name = "tl/pu/";
        name += path.empty() ? std::string("untextured") : path;
        name += "/";
        name += kRenderStyles[std::min(render_style, 3u)];
        name += depth_check ? "DEPTH_ON" : "DEPTH_OFF";
        // The bias is the pass's own: a particle authoring `DEPTH BIAS` (the
        // lanterns' `Glow camera`, 500) must not lend it to - or take none
        // from - another on the same texture and style.
        if (depth_bias != 0.0f) name += "B" + std::to_string(depth_bias);
        if (u_scroll != 0.0f) name += "U" + std::to_string(u_scroll);
        if (v_scroll != 0.0f) name += "V" + std::to_string(v_scroll);

        auto& materials = Ogre::MaterialManager::getSingleton();
        Ogre::MaterialPtr material = materials.getByName(name, context->resource_group);
        if (material) {
            technique->setMaterialName(name);
            Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
            if (pass->getNumTextureUnitStates() > 0) {
                pass->getTextureUnitState(0)->setScrollAnimation(u_scroll, v_scroll);
            }
            return;
        }
        material = materials.create(name, context->resource_group);
        Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        // `D3D9RenderSystem::_setDepthBias`: `constantBias * -3.99999999e-06`
        // of the [0, 1] depth range; GL counts in the depth buffer's units, negated.
        pass->setDepthBias(depth_bias * 3.99999999e-06f * depth_buffer_units(), 0.0f);
        material->setFog(true, Ogre::FOG_NONE, Ogre::ColourValue::White, 0.001f, 0.0f, 1.0f);
        if (!path.empty()) pass->createTextureUnitState(path);
        pass->setDepthWriteEnabled(false);
        pass->setDepthCheckEnabled(depth_check);
        // The fixed-function pipeline OGRE 1.6 drew with fed an unlit pass its
        // vertex colour; OGRE 14's generated shaders read it only when asked.
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        if (render_style == kStyleAdditive) {
            material->setSceneBlending(Ogre::SBT_MODULATE);
            material->setSceneBlending(Ogre::SBF_ONE, Ogre::SBF_ONE);
        } else if (render_style == kStyleModulate) {
            material->setSceneBlending(Ogre::SBF_ZERO, Ogre::SBF_SOURCE_COLOUR);
            // The particle's colour is added to the texture, not multiplied
            // into it (`LBX_ADD`, `LBS_CURRENT`, `LBS_TEXTURE`), so
            // the texture's white margin stays white and leaves the scene
            // alone; only its dark body takes the tint.
            if (pass->getNumTextureUnitStates() > 0) {
                pass->getTextureUnitState(0)->setColourOperationEx(Ogre::LBX_ADD, Ogre::LBS_CURRENT,
                                                                   Ogre::LBS_TEXTURE);
            }
        } else if (render_style == kStyleAlpha) {
            material->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        }
        if (pass->getNumTextureUnitStates() > 0) {
            pass->getTextureUnitState(0)->setScrollAnimation(u_scroll, v_scroll);
        }
        technique->setMaterialName(name);
    }
};

// ---------------------------------------------------------------------------
// CParticleEmitterWrapper

/// The emitter wrapper's own state, with the constructor's values
/// (`CParticleEmitterWrapper::CParticleEmitterWrapper`).
struct EmitterWrapper {
    PU::ParticleEmitter* emitter = nullptr;                                  // +0x110
    TechWrapper* parent = nullptr;                                           // +0x108
    float radius = 1.0f;                                                     // +0x118
    float min_line_increment = 0.0f;                                         // +0x11c
    float max_line_increment = 0.0f;                                         // +0x120
    float max_line_deviation = 0.0f;                                         // +0x124
    int type = 0;                                                            // +0x128, Point
    float box_width = 1.0f, box_height = 1.0f, box_depth = 1.0f;             // +0x12c..+0x134
    bool enabled = true;                                                     // +0x138
    bool circle_fixed = true;                                                // +0x139
    Ogre::Vector3 line_end = Ogre::Vector3::ZERO;                            // +0x13c
    std::vector<float> min_radius, max_radius, circle_degrees, circle_step;  // +0x148..+0x160
    Ogre::Vector3 position = Ogre::Vector3::ZERO;

    // Each shape setter keeps its value and hands it to an emitter of its type:
    // `CParticleEmitterWrapper::setRadius`, SphereSurface (4);
    // `CParticleEmitterWrapper::setBoxWidth`, Box (1);
    // `CParticleEmitterWrapper::setBoxHeight`, Box (1);
    // `CParticleEmitterWrapper::setBoxDepth`, Box (1);
    // `CParticleEmitterWrapper::setPositionEnd`, Line (3);
    // `CParticleEmitterWrapper::setMinLineIncrement`, Line (3);
    // `CParticleEmitterWrapper::setMaxLineIncrement`, Line (3);
    // `CParticleEmitterWrapper::setMaxLineDeviation`, Line (3);
    // `CParticleEmitterWrapper::setCircleFixedSize`, Circle (2).
    void set_radius(float value) {
        radius = value;
        if (type == kEmitterSphereSurface)
            static_cast<PU::SphereSurfaceEmitter*>(emitter)->setRadius(value);
    }
    void set_box_width(float value) {
        box_width = value;
        if (type == kEmitterBox) static_cast<PU::BoxEmitter*>(emitter)->setWidth(value);
    }
    void set_box_height(float value) {
        box_height = value;
        if (type == kEmitterBox) static_cast<PU::BoxEmitter*>(emitter)->setHeight(value);
    }
    void set_box_depth(float value) {
        box_depth = value;
        if (type == kEmitterBox) static_cast<PU::BoxEmitter*>(emitter)->setDepth(value);
    }
    void set_line_end(const Ogre::Vector3& end) {
        line_end = end;
        if (type == kEmitterLine) static_cast<PU::LineEmitter*>(emitter)->setEnd(end);
    }
    void set_min_line_increment(float value) {
        min_line_increment = value;
        if (type == kEmitterLine) static_cast<PU::LineEmitter*>(emitter)->setMinIncrement(value);
    }
    void set_max_line_increment(float value) {
        max_line_increment = value;
        if (type == kEmitterLine) static_cast<PU::LineEmitter*>(emitter)->setMaxIncrement(value);
    }
    void set_max_line_deviation(float value) {
        max_line_deviation = value;
        if (type == kEmitterLine) static_cast<PU::LineEmitter*>(emitter)->setMaxDeviation(value);
    }
    void set_circle_fixed(bool fixed) {
        circle_fixed = fixed;
        if (type == kEmitterCircle) static_cast<PU::CircleEmitter*>(emitter)->setFixedRadius(fixed);
    }
    // `CParticleEmitterWrapper::setMinRadius`,
    // `CParticleEmitterWrapper::setMaxRadius`,
    // `CParticleEmitterWrapper::setCircleDegrees`,
    // `CParticleEmitterWrapper::setCircleStep`: kept across a
    // type change (`+0x148..+0x160`) and applied to a Circle emitter (type 2).
    void set_min_radius(const std::vector<float>& values) {
        min_radius = values;
        if (type == kEmitterCircle)
            static_cast<PU::CircleEmitter*>(emitter)->setMinRadius(dynamic_from_array(values));
    }
    void set_max_radius(const std::vector<float>& values) {
        max_radius = values;
        if (type == kEmitterCircle)
            static_cast<PU::CircleEmitter*>(emitter)->setMaxRadius(dynamic_from_array(values));
    }
    void set_circle_degrees(const std::vector<float>& values) {
        circle_degrees = values;
        if (type == kEmitterCircle)
            static_cast<PU::CircleEmitter*>(emitter)->setCircleAngle(dynamic_from_array(values));
    }
    void set_circle_step(const std::vector<float>& values) {
        circle_step = values;
        if (type == kEmitterCircle)
            static_cast<PU::CircleEmitter*>(emitter)->setStep(dynamic_from_array(values));
    }

    // `CParticleEmitterWrapper::setParentTechniqueWrapper`:
    // off the old wrapper's list and out of
    // the emitter's technique, then onto the new one's - whose `addEmitter`
    // hands the technique the emitter - and into its technique if that did not.
    void set_parent_technique_wrapper(TechWrapper* wrapper) {
        if (parent != nullptr) parent->remove_emitter(emitter);
        if (PU::ParticleTechnique* own = emitter->getParentTechnique()) own->removeEmitter(emitter);
        parent = wrapper;
        if (wrapper == nullptr) return;
        wrapper->add_emitter(emitter);
        if (emitter->getParentTechnique() != wrapper->technique) {
            wrapper->technique->addEmitter(emitter);
        }
    }

    // `CParticleEmitterWrapper::setParentGuid`:
    // detached, then attached to the first `CParticleTechWrapper` up its chain
    // of parents - the `Particle` node `collect_parts` gathered it under,
    // `found` here.
    void set_parent_guid(TechWrapper* found) {
        if (parent != nullptr) parent->remove_emitter(emitter);
        set_parent_technique_wrapper(nullptr);
        if (found != nullptr) set_parent_technique_wrapper(found);
    }

    // `CParticleEmitterWrapper::setKeepLocal`: the emitter's and the
    // technique's.
    void set_keep_local(bool keep) {
        emitter->setKeepLocal(keep);
        if (parent) parent->technique->setKeepLocal(keep);
    }

    // `CParticleEmitterWrapper::setEnabled`.
    void set_enabled(bool value) {
        enabled = value;
        emitter->setEmissionStopped(!value);
    }

    /// `CParticleEmitterWrapper::setEmitterType`:
    /// a new emitter of the type takes the
    /// old one's attributes and its place, then the shape values are applied
    /// again.
    void set_emitter_type(int new_type) {
        if (type == new_type) return;
        type = new_type;
        auto* manager = PU::ParticleSystemManager::getSingletonPtr();
        PU::ParticleEmitter* replacement = manager->createEmitter(kEmitterTypes[new_type]);
        TechWrapper* const owner = parent;
        set_parent_technique_wrapper(nullptr);
        // The new emitter keeps its own type string (+0x128) across the copy, which would
        // stamp the old one's on it; `CSkillEvent::startEvent` finds line emitters by it.
        const Ogre::String kind = replacement->getEmitterType();
        emitter->copyAttributesTo(replacement);
        replacement->setEmitterType(kind);
        manager->destroyEmitter(emitter);
        emitter = replacement;
        set_parent_technique_wrapper(owner);

        set_radius(radius);
        emitter->position = position;
        set_min_line_increment(min_line_increment);
        set_max_line_increment(max_line_increment);
        set_max_line_deviation(max_line_deviation);
        set_line_end(line_end);
        set_box_depth(box_depth);
        set_box_height(box_height);
        set_box_width(box_width);
        set_circle_fixed(circle_fixed);
        set_min_radius(std::vector<float>(min_radius));
        set_max_radius(std::vector<float>(max_radius));
        set_circle_degrees(std::vector<float>(circle_degrees));
        set_circle_step(std::vector<float>(circle_step));
    }
};

/// `CParticleEmitterWrapperDescriptor::CreateObject` makes an
/// `Emitter` node's wrapper: `CParticleEmitterWrapper::CParticleEmitterWrapper`.
///
void construct_emitter(EmitterWrapper& wrapper, TechWrapper& parent) {
    auto* manager = PU::ParticleSystemManager::getSingletonPtr();
    wrapper.emitter = manager->createEmitter("Point");
    PU::ParticleEmitter* emitter = wrapper.emitter;
    emitter->setNumberOfLoops(-1);
    emitter->setParticleDirection(Ogre::Vector3(0.0f, 1.0f, 0.0f));
    const float rate[] = {0.0f, 15.0f};
    const float one[] = {1.0f, 1.0f, 1.0f};
    const float angle[] = {0.0f, 45.0f};
    const float velocity[] = {0.0f, 15.0f};
    const float life[] = {1.0f, 5.0f, 10.0f};
    const float duration[] = {0.0f, 1.0f};
    emitter->setDynEmissionRate(dynamic_from_array(rate, 2));
    emitter->setDynParticleWidth(dynamic_from_array(one, 3));
    emitter->setDynParticleHeight(dynamic_from_array(one, 3));
    emitter->setDynParticleDepth(dynamic_from_array(one, 3));
    emitter->setDynAngle(dynamic_from_array(angle, 2));
    emitter->setDynVelocity(dynamic_from_array(velocity, 2));
    emitter->setDynTotalTimeToLive(dynamic_from_array(life, 3));
    emitter->setDynDuration(dynamic_from_array(duration, 2));
    wrapper.set_min_radius({0.0f, 3.0f});
    wrapper.set_max_radius({0.0f, 3.0f});
    wrapper.set_circle_degrees({0.0f, 0.0f});
    wrapper.set_circle_step({0.0f, 0.1f});
    wrapper.enabled = true;
    emitter->setEmissionStopped(false);
    // `setParentGuid` then attaches it to its technique before its properties
    // are read (`CDescriptor::loadObjectFromBinaryFile`).
    //
    wrapper.set_parent_guid(&parent);
}

/// An `Emitter` node's properties, in `CParticleEmitterWrapperDescriptor`'s
/// registration order.
void read_emitter(EmitterWrapper& wrapper, const CmpNode& node) {
    namespace f = cmp_field::emitter;
    const auto emitter = [&]() { return wrapper.emitter; };
    Ogre::Vector3 v;
    float x = 0.0f;
    bool b = false;
    uint32_t u = 0;
    std::string text;

    // POSITION: `CParticleEmitterWrapper::positionUpdated`, the node's local position.
    if (read_vector(node, f::kPosition, v)) {
        wrapper.position = v;
        emitter()->position = v;
    }
    if (read_text(node, f::kTypeOfEmitter, text)) {
        wrapper.set_emitter_type(index_of(kEmitterTypes, text, 0));
    }
    if (read_vector(node, f::kDirection, v)) emitter()->setParticleDirection(v);
    if (read_bool(node, f::kFixed, b)) emitter()->setUnifiedScale(b);
    if (auto* d = read_dynamic(node, f::kScaleOnLaunch)) emitter()->setDynScaleOnLaunch(d);
    if (auto* d = read_dynamic(node, f::kWidth)) emitter()->setDynParticleWidth(d);
    if (auto* d = read_dynamic(node, f::kHeight)) emitter()->setDynParticleHeight(d);
    if (auto* d = read_dynamic(node, f::kDepth)) emitter()->setDynParticleDepth(d);
    if (auto* d = read_dynamic(node, f::kAngle)) emitter()->setDynAngle(d);
    if (auto* d = read_dynamic(node, f::kVelocity)) emitter()->setDynVelocity(d);
    if (auto* d = read_dynamic(node, f::kMass)) emitter()->setDynParticleMass(d);
    if (read_bool(node, f::kKeepLocal, b)) wrapper.set_keep_local(b);
    if (read_bool(node, f::kEnabled, b)) wrapper.set_enabled(b);
    if (read_bool(node, f::kNoExpiration, b)) emitter()->setNonExpiring(b);
    if (read_bool(node, f::kDelayFirst, b)) emitter()->setDelayFirst(b);
    if (auto* d = read_dynamic(node, f::kDelay)) emitter()->setDynRepeatDelay(d);
    if (auto* d = read_dynamic(node, f::kParticleLife)) emitter()->setDynTotalTimeToLive(d);
    if (auto* d = read_dynamic(node, f::kEmitDuration)) emitter()->setDynDuration(d);
    if (auto* d = read_dynamic(node, f::kEmitRate)) emitter()->setDynEmissionRate(d);
    if (read_uint(node, f::kNumLoops, u)) emitter()->setNumberOfLoops(static_cast<int32_t>(u));
    if (read_bool(node, f::kReleaseOverDistance, b)) emitter()->setEmitOverDistance(b);
    if (auto* d = read_dynamic(node, f::kChanceToEmit)) emitter()->setDynChanceToEmit(d);
    if (read_float(node, f::kMinLineIncrement, x)) wrapper.set_min_line_increment(x);
    if (read_float(node, f::kMaxLineIncrement, x)) wrapper.set_max_line_increment(x);
    if (read_float(node, f::kMaxLineDeviation, x)) wrapper.set_max_line_deviation(x);
    if (read_vector(node, f::kEndPosition, v)) wrapper.set_line_end(v);
    if (read_float(node, f::kBoxWidth, x)) wrapper.set_box_width(x);
    if (read_float(node, f::kBoxHeight, x)) wrapper.set_box_height(x);
    if (read_float(node, f::kBoxDepth, x)) wrapper.set_box_depth(x);
    if (read_float(node, f::kRadius, x)) wrapper.set_radius(x);
    if (node.field(f::kMinRadius)) wrapper.set_min_radius(read_floats(node, f::kMinRadius));
    if (node.field(f::kMaxRadius)) wrapper.set_max_radius(read_floats(node, f::kMaxRadius));
    if (node.field(f::kStep)) wrapper.set_circle_step(read_floats(node, f::kStep));
    if (read_bool(node, f::kCircleFixed, b)) wrapper.set_circle_fixed(b);
}

// ---------------------------------------------------------------------------
// The affector wrappers (`CAffectorWrapper` and its subclasses).

/// `CAffectorWrapper::setEnabled`: the wrapper's `+0x119`, and the
/// affector's own switch (`ParticleAffector +0x110`) set to its negation. The
/// Particle's enabled flag under the affector is not touched.
void set_affector_enabled(PU::ParticleAffector* affector, bool enabled) {
    affector->setTorchlightDisabled(!enabled);
}

/// `CAffectorWrapper::CAffectorWrapper` and
/// the parts every affector node shares: POSITION
/// (`CAffectorWrapper::positionUpdated`, - the
/// node's local position when positioning is on; off, the affector's and the
/// wrapper's own position are both zero) and ENABLE (`setEnabled`;
/// the constructor leaves it on).
PU::ParticleAffector* construct_affector(TechWrapper& parent, const CmpNode& node, const char* type,
                                         bool positioning) {
    auto* manager = PU::ParticleSystemManager::getSingletonPtr();
    PU::ParticleAffector* affector = manager->createAffector(type);
    set_affector_enabled(affector, true);
    // `CAffectorWrapper::setParentTechniqueWrapper`
    // (its Xbox twin is not paired), from its
    // `setParentGuid`: off a former wrapper's list (none yet), onto this one's.
    parent.add_affector(node.id, affector);
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    if (positioning) read_vector(node, 0, position);
    affector->position = position;
    bool enabled = true;
    if (read_bool(node, 1, enabled)) set_affector_enabled(affector, enabled);
    return affector;
}

/// Build one affector node; false when it is not one of the transcribed kinds.
bool build_affector(TechWrapper& parent, const CmpNode& node) {
    switch (node.type) {
        case cmp_type::kColorOverTime: {
            // `CColorAffectorDescriptor::CreateObject`:
            // `CColorWrapper::CColorWrapper`, white at 0, then
            // `CColorWrapper::setColors` with the authored (time, r, g, b, a)
            // runs. DIVERGENCE: the original reads a trailing partial group past the array; the
            // port stops at the last whole group.
            auto* colour =
                static_cast<PU::ColourAffector*>(construct_affector(parent, node, "Colour", true));
            colour->addColour(0.0f, Ogre::ColourValue(1, 1, 1, 1));
            const std::vector<float> keys =
                read_floats(node, cmp_field::color_over_time::kColorOverTime);
            if (node.field(cmp_field::color_over_time::kColorOverTime) != nullptr) {
                colour->clearColourMap();
                for (size_t i = 0; i + 4 < keys.size(); i += 5) {
                    colour->addColour(keys[i], Ogre::ColourValue(keys[i + 1], keys[i + 2],
                                                                 keys[i + 3], keys[i + 4]));
                }
            }
            return true;
        }
        case cmp_type::kScaleOverTime: {
            // `CScaleAffectorDescriptor::CreateObject`:
            // `CScaleWrapper::CScaleWrapper`: X, Y, Z fixed 1.
            namespace f = cmp_field::scale_over_time;
            auto* scale =
                static_cast<PU::ScaleAffector*>(construct_affector(parent, node, "Scale", true));
            const float one[] = {0.0f, 1.0f};
            scale->setDynScaleX(dynamic_from_array(one, 2));
            scale->setDynScaleY(dynamic_from_array(one, 2));
            scale->setDynScaleZ(dynamic_from_array(one, 2));
            bool fixed = false;
            if (read_bool(node, f::kFixed, fixed)) scale->setUnifiedScale(fixed);
            if (auto* d = read_dynamic(node, f::kX)) scale->setDynScaleX(d);
            if (auto* d = read_dynamic(node, f::kY)) scale->setDynScaleY(d);
            if (auto* d = read_dynamic(node, f::kZ)) scale->setDynScaleZ(d);
            return true;
        }
        case cmp_type::kTextureRotate: {
            // `CTextureRotateDescriptor::CreateObject`, its
            // constructor inline on the console: `CTextureRotateWrapper::CTextureRotateWrapper`:
            // the affector's own defaults.
            namespace f = cmp_field::texture_rotate;
            auto* rotator = static_cast<PU::TextureRotator*>(
                construct_affector(parent, node, "TextureRotator", true));
            bool own = false;
            if (auto* d = read_dynamic(node, f::kRotationSpeed)) rotator->setRotationSpeed(d);
            if (auto* d = read_dynamic(node, f::kStartingRotation)) rotator->setRotation(d);
            if (read_bool(node, f::kUseOwnRotation, own)) rotator->setUseOwnRotationSpeed(own);
            return true;
        }
        case cmp_type::kTextureAnimation: {
            // `CTextureAnimateDescriptor::CreateObject`, its
            // constructor inline on the console: `CTextureAnimateWrapper::CTextureAnimateWrapper`:
            // the affector's own defaults.
            namespace f = cmp_field::texture_animation;
            auto* animator = static_cast<PU::TextureAnimator*>(
                construct_affector(parent, node, "TextureAnimator", true));
            bool flag = false;
            if (auto* d = read_dynamic(node, f::kAnimationSpeed)) animator->setAnimationSpeed(d);
            if (read_bool(node, f::kUseOwnAnimation, flag)) animator->setUseOwnAnimationSpeed(flag);
            if (read_bool(node, f::kUseRandomStartingFrame, flag))
                animator->setUseRandomStartingFrame(flag);
            return true;
        }
        case cmp_type::kGravityWell: {
            // `CGravityAffectorDescriptor::CreateObject`, its
            // constructor inline on the console: `CGravityWrapper::CGravityWrapper`:
            // the affector's own defaults.
            auto* gravity = static_cast<PU::GravityAffector*>(
                construct_affector(parent, node, "Gravity", true));
            float value = 0.0f;
            if (read_float(node, cmp_field::gravity_well::kGravity, value))
                gravity->setGravity(value);
            return true;
        }
        case cmp_type::kVortex: {
            // `CVortexAffectorDescriptor::CreateObject`, its
            // constructor inline on the console: `CVortexWrapper::CVortexWrapper`:
            // about +Y until authored.
            namespace f = cmp_field::vortex;
            auto* vortex =
                static_cast<PU::VortexAffector*>(construct_affector(parent, node, "Vortex", true));
            vortex->setRotationVector(Ogre::Vector3(0.0f, 1.0f, 0.0f));
            Ogre::Vector3 axis;
            if (read_vector(node, f::kDirection, axis)) vortex->setRotationVector(axis);
            if (auto* d = read_dynamic(node, f::kRate)) vortex->setRotationSpeed(d);
            return true;
        }
        case cmp_type::kSineForce: {
            // `CSineForceAffectorDescriptor::CreateObject`, its
            // constructor inline on the console: `CSineForceWrapper::CSineForceWrapper`
            // on `CForceWrapper::CForceWrapper`: no
            // force, `Add` (1 into the affector's `+0xc8`), positioning off.
            namespace f = cmp_field::sine_force;
            auto* sine = static_cast<PU::SineForceAffector*>(
                construct_affector(parent, node, "SineForce", false));
            sine->setForceVector(Ogre::Vector3::ZERO);
            sine->setForceApplication(PU::BaseForceAffector::FA_ADD);
            std::string text;
            Ogre::Vector3 force;
            float value = 0.0f;
            if (read_text(node, f::kApplication, text)) {
                sine->setForceApplication(index_of(kForceApplications, text, 0) == 1
                                              ? PU::BaseForceAffector::FA_ADD
                                              : PU::BaseForceAffector::FA_AVERAGE);
            }
            if (read_vector(node, f::kForces, force)) sine->setForceVector(force);
            if (read_float(node, f::kMin, value)) sine->setFrequencyMin(value);
            if (read_float(node, f::kMax, value)) sine->setFrequencyMax(value);
            return true;
        }
        case cmp_type::kLinearForce: {
            // `CLinearForceAffectorDescriptor::CreateObject`, its
            // constructor inline on the console: `CLinearForceWrapper::CLinearForceWrapper`
            // on `CForceWrapper`: no force, `Add`, positioning off.
            namespace f = cmp_field::linear_force;
            auto* force = static_cast<PU::LinearForceAffector*>(
                construct_affector(parent, node, "LinearForce", false));
            force->setForceVector(Ogre::Vector3::ZERO);
            force->setForceApplication(PU::BaseForceAffector::FA_ADD);
            std::string text;
            Ogre::Vector3 vector;
            if (read_text(node, f::kApplication, text)) {
                force->setForceApplication(index_of(kForceApplications, text, 0) == 1
                                               ? PU::BaseForceAffector::FA_ADD
                                               : PU::BaseForceAffector::FA_AVERAGE);
            }
            if (read_vector(node, f::kForces, vector)) force->setForceVector(vector);
            return true;
        }
        case cmp_type::kJet: {
            // `CJetAffectorDescriptor::CreateObject`, its
            // constructor inline on the console: `CJetWrapper::CJetWrapper`, the
            // affector's defaults; `ACCELERATION` through
            // `CJetWrapper::setDynamicPropAcceleration`.
            auto* jet =
                static_cast<PU::JetAffector*>(construct_affector(parent, node, "Jet", true));
            if (auto* d = read_dynamic(node, cmp_field::jet::kAcceleration))
                jet->setDynAcceleration(d);
            return true;
        }
        case cmp_type::kAlign: {
            // `CAlignDescriptor::CreateObject`, its constructor
            // inline on the console: `CAlignWrapper::CAlignWrapper`; `TO
            // VELOCITY` straight to the affector (`CAlignDescriptor::Set_setAlignByVelocity`),
            // `STRETCH` through `setResize` (`Set_setAllowStretch`).
            namespace f = cmp_field::align;
            auto* align =
                static_cast<PU::AlignAffector*>(construct_affector(parent, node, "Align", true));
            bool flag = false;
            if (read_bool(node, f::kToVelocity, flag)) align->setAlignByVelocity(flag);
            if (read_bool(node, f::kStretch, flag)) align->setResize(flag);
            return true;
        }
        case cmp_type::kGeometryRotator: {
            // `CGeometryRotateDescriptor::CreateObject`, its
            // constructor inline on the console: `CGeometryRotateWrapper::CGeometryRotateWrapper`,
            // the affector's defaults; `ROTATION SPEED` through
            // `CGeometryRotateWrapper::setDynamicPropRotationSpeed`.
            namespace f = cmp_field::geometry_rotator;
            auto* rotator = static_cast<PU::GeometryRotator*>(
                construct_affector(parent, node, "GeometryRotator", true));
            bool own = false;
            Ogre::Vector3 axis;
            if (auto* d = read_dynamic(node, f::kRotationSpeed)) rotator->setRotationSpeed(d);
            if (read_bool(node, f::kUseOwnRotation, own)) rotator->setUseOwnRotationSpeed(own);
            if (read_vector(node, f::kAxis, axis)) rotator->setRotationAxis(axis);
            return true;
        }
        case cmp_type::kPlaneCollision:
        case cmp_type::kSphereCollision:
        case cmp_type::kBoxCollision: {
            // `CPlaneColliderDescriptor::CreateObject`,
            // `CSphereColliderDescriptor::CreateObject`,
            // `CBoxColliderDescriptor::CreateObject`, each
            // constructor inline on the console: `CPlaneColliderWrapper::CPlaneColliderWrapper`,
            // `CSphereColliderWrapper::CSphereColliderWrapper`,
            // `CBoxColliderWrapper`, on `CColliderWrapper::CColliderWrapper`:
            // positioning on, the collider's defaults. The plane's
            // normal (0, 1, 0) goes over in `CPlaneColliderWrapper::updatePlane`, which
            // `CPlaneColliderWrapper::positionUpdated`, NORMAL and SHOW PLANE call;
            // `CSphereColliderWrapper::updateCircle` does nothing.
            namespace f = cmp_field::collider;
            const bool plane = node.type == cmp_type::kPlaneCollision;
            const bool box = node.type == cmp_type::kBoxCollision;
            auto* collider =
                static_cast<PU::BaseCollider*>(construct_affector(parent, node,
                                                                  plane ? "PlaneCollider"
                                                                  : box ? "BoxCollider"
                                                                        : "SphereCollider",
                                                                  true));
            if (box) {
                // The box wrapper's constructor: 5 x 2.5 x 5 before the node's
                // own. `CBoxColliderWrapper::updateAlignedBox`, which each
                // dimension's and SHOW BOX's setter calls, does nothing.
                auto* box_collider = static_cast<PU::BoxCollider*>(collider);
                box_collider->setWidth(5.0f);
                box_collider->setHeight(2.5f);
                box_collider->setDepth(5.0f);
            }
            std::string text;
            float value = 0.0f;
            if (read_text(node, f::kType, text)) {
                collider->setCollisionType(static_cast<PU::BaseCollider::CollisionType>(
                    index_of(kCollisionTypes, text, 0)));
            }
            if (read_text(node, f::kIntersectionType, text)) {
                collider->setIntersectionType(static_cast<PU::BaseCollider::IntersectionType>(
                    index_of(kIntersectionTypes, text, 0)));
            }
            if (read_float(node, f::kFriction, value)) collider->setFriction(value);
            if (read_float(node, f::kBouncyness, value)) collider->setBouncyness(value);
            if (plane) {
                Ogre::Vector3 normal(0.0f, 1.0f, 0.0f);
                read_vector(node, f::kNormal, normal);
                static_cast<PU::PlaneCollider*>(collider)->setNormal(normal);
            } else if (box) {
                auto* box_collider = static_cast<PU::BoxCollider*>(collider);
                if (read_float(node, f::kWidth, value)) box_collider->setWidth(value);
                if (read_float(node, f::kHeight, value)) box_collider->setHeight(value);
                if (read_float(node, f::kDepth, value)) box_collider->setDepth(value);
            } else if (read_float(node, f::kRadius, value)) {
                static_cast<PU::SphereCollider*>(collider)->setRadius(value);
            }
            return true;
        }
        case cmp_type::kLine: {
            // `CLineAffectorDescriptor::CreateObject`, its
            // constructor inline on the console: `CLineWrapper::CLineWrapper` on
            // `CAffectorWrapper`: positioning off, the affector's defaults
            // (`LineAffectorFactory::createAffector`), each field
            // straight to its setter.
            namespace f = cmp_field::line;
            auto* line =
                static_cast<PU::LineAffector*>(construct_affector(parent, node, "Line", false));
            float value = 0.0f;
            Ogre::Vector3 end;
            if (read_float(node, f::kDeviation, value)) line->setMaxDeviation(value);
            if (read_float(node, f::kTimeStep, value)) line->setTimeStep(value);
            if (read_float(node, f::kDrift, value)) line->setDrift(value);
            if (read_vector(node, f::kEndPosition, end)) line->setEnd(end);
            return true;
        }
        default: return false;
    }
}

bool is_affector(uint32_t type) {
    switch (type) {
        case cmp_type::kColorOverTime:
        case cmp_type::kScaleOverTime:
        case cmp_type::kTextureRotate:
        case cmp_type::kTextureAnimation:
        case cmp_type::kGravityWell:
        case cmp_type::kVortex:
        case cmp_type::kSineForce:
        case cmp_type::kLinearForce:
        case cmp_type::kAlign:
        case cmp_type::kJet:
        case cmp_type::kGeometryRotator:
        case cmp_type::kPlaneCollision:
        case cmp_type::kSphereCollision:
        case cmp_type::kBoxCollision:
        case cmp_type::kLine: return true;
        default: return false;
    }
}

/// A Particle's emitters and affectors, in the order the loader builds them:
/// children depth first, in file order. An affector under an emitter belongs to
/// the nearest technique above it (`CAffectorWrapper::setParentGuid`).
///
void collect_parts(const CmpScene& layout, const CmpNode& node, std::vector<const CmpNode*>& out) {
    for (const CmpNode* child : layout.children_of(node.id)) {
        if (child->type == cmp_type::kParticle) continue;  // a technique of its own
        out.push_back(child);
        collect_parts(layout, *child, out);
    }
}

}  // namespace

bool can_build_torchlight_particle(const CmpScene& layout, const CmpNode& node, std::string* why) {
    const auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    std::string text;
    if (read_text(node, cmp_field::particle::kRenderType, text)) {
        if (index_of(kRenderTypes, text, -1) < 0) return fail("render type " + text);
    }
    std::vector<const CmpNode*> parts;
    collect_parts(layout, node, parts);
    for (const CmpNode* part : parts) {
        if (part->type == cmp_type::kEmitter) {
            if (read_text(*part, cmp_field::emitter::kTypeOfEmitter, text) &&
                index_of(kEmitterTypes, text, -1) < 0) {
                return fail("emitter " + text);
            }
            continue;
        }
        if (!is_affector(part->type)) return fail("child '" + part->name + "'");
        switch (part->type) {
            case cmp_type::kColorOverTime:
            case cmp_type::kScaleOverTime:
            case cmp_type::kTextureRotate:
            case cmp_type::kTextureAnimation:
            case cmp_type::kGravityWell:
            case cmp_type::kVortex:
            case cmp_type::kSineForce:
            case cmp_type::kLinearForce:
            case cmp_type::kJet:
            case cmp_type::kAlign:
            case cmp_type::kGeometryRotator:
            case cmp_type::kPlaneCollision:
            case cmp_type::kSphereCollision:
            case cmp_type::kBoxCollision:
            case cmp_type::kLine: break;
            default: return fail("affector '" + part->name + "'");
        }
    }
    return true;
}

bool build_torchlight_particle(const ParticleBuildContext& context, const CmpScene& layout,
                               const CmpNode& node, const Ogre::Vector3& position,
                               const Ogre::Quaternion& orientation, TorchlightParticle& out) {
    namespace f = cmp_field::particle;
    auto* manager = PU::ParticleSystemManager::getSingletonPtr();
    if (manager == nullptr || context.scene == nullptr) return false;

    TechWrapper tech;
    tech.context = &context;
    tech.node = context.scene->getRootSceneNode()->createChildSceneNode(position, orientation);

    // `CParticleWrapper::CParticleWrapper`, then
    // `CParticleWrapper::createParticleSystem`: a system named
    // "part..." with no bounds updating of its own, techniques local, on the node, prepared,
    // handed the player's camera.
    static unsigned serial = 0;
    tech.system =
        manager->createParticleSystem("tl/pu/part" + std::to_string(serial++), context.scene);
    tech.system->setQueryFlags(1);
    if (!(tech.system->getFixedTimeout() > 0.0f)) tech.system->setBoundsAutoUpdated(false, 0.25f);
    tech.system->setKeepLocal(true);
    tech.node->attachObject(tech.system);
    tech.system->prepare();
    tech.system->setEnabled(true);
    tech.system->notifyCurrentCamera(context.camera);

    // `CParticleTechWrapperDescriptor::CreateObject`
    // makes a `Particle` node's wrapper:
    // `CParticleTechWrapper::CParticleTechWrapper`:
    // a technique at the origin, 0.5 x 0.5 x
    // 1 by default, a visual quota of 50, a velocity cap of 9999, enabled, a
    // billboard renderer and queue 90.
    tech.technique = manager->createTechnique();
    tech.technique->position = Ogre::Vector3::ZERO;
    tech.technique->setDefaultDepth(1.0f);
    tech.technique->setDefaultHeight(0.5f);
    tech.technique->setDefaultWidth(0.5f);
    tech.technique->setVisualParticleQuota(50);
    tech.technique->setMaxVelocity(9999.0f);
    tech.technique->setEnabled(true);
    tech.set_render_type(0);
    tech.set_render_queue(0x5a);
    tech.system->addTechnique(tech.technique);
    tech.set_render_type(0);

    // The node's properties in `CParticleTechWrapperDescriptor`'s registration
    // order. POSITION and the basis
    // are the placement.
    std::string text;
    float x = 0.0f;
    bool b = false;
    uint32_t u = 0;
    if (read_text(node, f::kRenderType, text))
        tech.set_render_type(index_of(kRenderTypes, text, 0));
    if (read_float(node, f::kVisualScale, x)) tech.system->setScale(Ogre::Vector3(x, x, x));
    if (read_float(node, f::kTimeScale, x)) tech.system->setScaleTime(x);
    if (read_float(node, f::kVelocityScale, x)) tech.system->setScaleVelocity(x);
    if (read_float(node, f::kLifeTime, x)) tech.system->setFixedTimeout(x);
    if (read_bool(node, f::kWorldOrientation, b)) tech.technique->setWorldOrientation(b);
    if (read_bool(node, f::kNoCulling, b)) tech.technique->setNoCull(b);
    if (read_bool(node, f::kIsLight, b)) tech.set_as_light(b);
    if (read_bool(node, f::kVisible, b)) tech.enabled = b;
    if (read_bool(node, f::kSorts, b)) tech.set_sorts(b);
    // `SORT INDEX` is the text "0"-"4" (`GetSortIndexIDByString`); above
    // 4 it is 0.
    if (read_text(node, f::kSortIndex, text)) {
        int index = std::atoi(text.c_str());
        if (index < 0 || index > 4) index = 0;
        tech.set_sort_index(static_cast<unsigned>(index));
    }
    if (read_float(node, f::kDepthBias, x)) tech.depth_bias = x;
    if (read_float(node, f::kMaxVelocity, x)) tech.technique->setMaxVelocity(x);
    if (read_uint(node, f::kVisualQuota, u)) tech.technique->setVisualParticleQuota(u);
    if (read_text(node, f::kTexture, text)) tech.texture = text;
    if (read_text(node, f::kRenderStyle, text)) {
        tech.render_style = static_cast<unsigned>(index_of(kRenderStyles, text, 1));
    }
    if (read_text(node, f::kMesh, text)) tech.set_model_path(text);
    if (read_bool(node, f::kDepthCheck, b)) tech.depth_check = b;
    if (read_text(node, f::kRotationType, text)) tech.set_rotation_type(text == "Texture" ? 1 : 0);
    if (read_text(node, f::kOrigin, text))
        tech.set_origin(static_cast<unsigned>(index_of(kOrigins, text, 4)));
    if (read_float(node, f::kLength, x)) tech.set_ribbon_length(x);
    if (read_float(node, f::kWidth, x)) tech.set_ribbon_width(x);
    if (read_uint(node, f::kSegments, u)) tech.set_ribbon_segments(u);
    if (read_bool(node, f::kFadeEnds, b)) tech.set_ribbon_fade(b);
    if (read_bool(node, f::kLocalTrail, b)) tech.set_ribbon_local(b);
    if (read_uint(node, f::kFramesWide, u)) tech.set_flipbook_width(static_cast<int>(u));
    if (read_uint(node, f::kFramesHigh, u)) tech.set_flipbook_height(static_cast<int>(u));
    // `CParticleTechWrapper::setUScroll` and
    // `CParticleTechWrapper::setVScroll`:
    // kept; only the editor rebuilds the material and restarts the system on
    // them. `create_texture_material` hands them to the texture below.
    if (read_float(node, f::kURate, x)) tech.u_scroll = x;
    if (read_float(node, f::kVRate, x)) tech.v_scroll = x;

    // The children, parent first and depth first.
    std::vector<const CmpNode*> parts;
    collect_parts(layout, node, parts);
    std::vector<EmitterWrapper> emitters;
    emitters.reserve(parts.size());
    for (const CmpNode* part : parts) {
        if (part->type == cmp_type::kEmitter) {
            emitters.emplace_back();
            construct_emitter(emitters.back(), tech);
            read_emitter(emitters.back(), *part);
            out.emitters.emplace_back(part->id, emitters.back().emitter);
        } else if (is_affector(part->type)) {
            build_affector(tech, *part);
        }
    }

    // `CParticleTechWrapperDescriptor::DescriptorObjectHasBeenInited`.
    tech.create_texture_material();

    out.system = tech.system;
    out.movable = tech.system;
    out.is_light = tech.is_light;
    out.node = tech.node;
    out.enabled = tech.enabled;
    out.camera = context.camera;
    out.affectors = std::move(tech.affectors);
    out.fixed_timeout = tech.system->getFixedTimeout();
    out.finite_window = 0.0f;
    out.non_expiring = false;
    // `CParticle::Start`: an emitter with
    // loops left runs for `(loops + 1) x duration + delay`, each read at 0.
    for (PU::ParticleEmitter* emitter : tech.emitters) {
        if (emitter->isNonExpiring()) out.non_expiring = true;
        const int loops = emitter->getNumberOfLoops();
        if (loops >= 0) {
            const float window =
                (static_cast<float>(loops) + 1.0f) * emitter->getDynDuration()->getValue(0.0f) +
                emitter->getDynRepeatDelay()->getValue(0.0f);
            out.finite_window = std::max(out.finite_window, window);
        }
    }
    return true;
}

void start_torchlight_particle(TorchlightParticle& particle) {
    if (particle.system == nullptr) return;
    // `CParticle::Start`: the camera
    // refreshed, every emitter free to release again, then
    // `ParticleSystem::start`.
    particle.system->notifyCurrentCamera(nullptr);
    for (size_t t = 0; t < particle.system->getNumTechniques(); ++t) {
        PU::ParticleTechnique* technique = particle.system->getTechnique(t);
        for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
            technique->getEmitter(e)->setEmissionStopped(false);
        }
    }
    particle.system->start();
}

void stop_torchlight_particle_emission(TorchlightParticle& particle) {
    if (particle.system == nullptr) return;
    // `CParticle::Stop(false)`: an emitter
    // whose particles expire stops releasing; one that does not stops its whole
    // system.
    for (size_t t = 0; t < particle.system->getNumTechniques(); ++t) {
        PU::ParticleTechnique* technique = particle.system->getTechnique(t);
        for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
            PU::ParticleEmitter* emitter = technique->getEmitter(e);
            if (emitter->isNonExpiring()) {
                particle.system->stop();
            } else {
                emitter->setEmissionStopped(true);
            }
        }
    }
}

void stop_torchlight_particle(TorchlightParticle& particle) {
    if (particle.system == nullptr) return;
    particle.system->stop();
}

// `CAffectorWrapper::setEnabled`, on the wrapper made from one node.
bool set_torchlight_emitter_enabled(TorchlightParticle& particle, int64_t emitter_node,
                                    bool enabled) {
    bool found = false;
    for (const auto& entry : particle.emitters) {
        if (entry.first != emitter_node || entry.second == nullptr) continue;
        entry.second->setEmissionStopped(!enabled);
        found = true;
    }
    return found;
}

bool set_torchlight_affector_enabled(TorchlightParticle& particle, int64_t affector_node,
                                     bool enabled, bool* changed) {
    bool found = false;
    for (const auto& entry : particle.affectors) {
        if (entry.first != affector_node || entry.second == nullptr) continue;
        if (changed != nullptr && entry.second->isTorchlightDisabled() == enabled) *changed = true;
        set_affector_enabled(entry.second, enabled);
        found = true;
    }
    return found;
}

namespace {

/// `CParticleEmitterWrapper::setEnabled` on every emitter of the
/// wrapper's technique (`CParticleTechWrapper +0x1d8`): `+0x138`, and the
/// emitter's `+0x25b` its negation.
void set_wrapper_emitters_enabled(TorchlightParticle& particle, bool enabled) {
    if (particle.system->getNumTechniques() == 0) return;
    PU::ParticleTechnique* technique = particle.system->getTechnique(0);
    for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
        technique->getEmitter(e)->setEmissionStopped(!enabled);
    }
}

/// `CParticleWrapper::stop`.
void stop_wrapper_system(TorchlightParticle& particle) {
    particle.playing = false;
    particle.system->stop();
    particle.system->_resetBounds();
}

}  // namespace

// `CParticleWrapper::setVisible`: `CSceneNodeObject::setVisible` on
// its node, then the system's own (vtable +0xf8).
void set_torchlight_wrapper_visible(TorchlightParticle& particle, bool visible) {
    if (particle.node != nullptr) particle.node->setVisible(visible);
    if (particle.system != nullptr) particle.system->setVisible(visible);
}

bool torchlight_particle_no_cull(const TorchlightParticle& particle) {
    if (particle.system == nullptr) return false;
    for (size_t i = 0; i < particle.system->getNumTechniques(); ++i) {
        if (particle.system->getTechnique(i)->isNoCull()) return true;
    }
    return false;
}

// `CParticle::forceParticleUpdate(float)` (inline in
// `CLevel::updateVisibleParticles` on the console):
// `_update(0.1)` until `seconds`, counted down by 0.1, is not above 0.
void force_torchlight_particle_update(TorchlightParticle& particle, float seconds) {
    if (particle.system == nullptr || !(0.0f < seconds)) return;
    float left = seconds;
    do {
        particle.system->_update(0.1f);
        left = left - 0.1f;
    } while (0.0f < left);
}

// `CParticleTechWrapper::start`.
void start_torchlight_wrapper(TorchlightParticle& particle) {
    if (particle.system == nullptr) return;
    // `CParticleTechWrapper::start`, past its
    // enabled test (the script's: a wrapper that is not enabled is stopped
    // instead).
    if (particle.playing) stop_torchlight_wrapper(particle);
    set_torchlight_wrapper_visible(particle, true);
    // Each emitter `CParticleEmitterWrapper::starting` (`+0x138` applied again) and
    // then enabled: every one releases.
    set_wrapper_emitters_enabled(particle, true);
    // `CParticleWrapper::start`.
    particle.playing = true;
    particle.system->notifyCurrentCamera(particle.camera);
    particle.system->_resetBounds();
    particle.system->start();
    // And the technique's renderer shown (vtable +0x28, `setVisible`).
    if (particle.system->getNumTechniques() != 0) {
        if (PU::ParticleRenderer* renderer = particle.system->getTechnique(0)->getRenderer()) {
            renderer->setVisible(true);
        }
    }
}

// `CParticleTechWrapper::stop`.
void stop_torchlight_wrapper(TorchlightParticle& particle) {
    if (particle.system == nullptr) return;
    // `CParticleTechWrapper::stop`. The
    // wrapper is in a level (`+0x68 -> +0x18`) wherever the port places one.
    PU::ParticleTechnique* technique =
        particle.system->getNumTechniques() != 0 ? particle.system->getTechnique(0) : nullptr;
    bool non_expiring = false;
    if (technique != nullptr) {
        for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
            if (technique->getEmitter(e)->isNonExpiring()) {
                non_expiring = true;
                break;
            }
        }
    }
    if (!non_expiring && technique != nullptr && technique->getNumberOfEmittedParticles() != 0) {
        // Left playing (`+0x130` stays set): the emitters stop, and
        // `updateLevelObject` stops the system when the particles are gone.
        particle.dying = true;
        set_wrapper_emitters_enabled(particle, false);
        return;
    }
    stop_wrapper_system(particle);
}

// `CParticleTechWrapper::updateLevelObject`.
void update_torchlight_wrapper(TorchlightParticle& particle) {
    // A `start` in the meantime does not take it off the list: it is still
    // stopped and hidden the first update that finds no particle.
    if (particle.system == nullptr || !particle.dying) return;
    if (particle.system->getNumTechniques() != 0 &&
        particle.system->getTechnique(0)->getParticlesUpdating() != 0) {
        return;
    }
    particle.dying = false;
    stop_wrapper_system(particle);
    set_torchlight_wrapper_visible(particle, false);
}

namespace {

template <typename Visit>
void visit_line_emitters(TorchlightParticle& particle, Visit visit) {
    if (particle.system == nullptr) return;
    for (size_t t = 0; t < particle.system->getNumTechniques(); ++t) {
        PU::ParticleTechnique* technique = particle.system->getTechnique(t);
        if (technique == nullptr) continue;
        for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
            PU::ParticleEmitter* emitter = technique->getEmitter(e);
            // The type string `startEvent` compares (`"Line"`).
            if (emitter != nullptr && emitter->getEmitterType() == "Line") {
                visit(static_cast<PU::LineEmitter*>(emitter));
            }
        }
    }
}

}  // namespace

size_t torchlight_particle_line_emitters(const TorchlightParticle& particle) {
    size_t count = 0;
    visit_line_emitters(const_cast<TorchlightParticle&>(particle),
                        [&count](PU::LineEmitter*) { ++count; });
    return count;
}

void set_torchlight_particle_line_end(TorchlightParticle& particle, const Ogre::Vector3& end) {
    visit_line_emitters(particle, [&end](PU::LineEmitter* line) { line->setEnd(end); });
}

bool torchlight_particle_updating(const TorchlightParticle& particle) {
    if (particle.system == nullptr || particle.system->getNumTechniques() == 0) return false;
    // `CParticle::getNumberOfParticlesUpdating(true)`:
    // technique 0 with particles, none of
    // whose emitters is non-expiring.
    PU::ParticleTechnique* technique = particle.system->getTechnique(0);
    if (technique->getNumberOfEmittedParticles() == 0) return false;
    for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
        if (technique->getEmitter(e)->isNonExpiring()) return false;
    }
    return true;
}

bool torchlight_particle_has_particles(const TorchlightParticle& particle) {
    if (particle.system == nullptr || particle.system->getNumTechniques() == 0) return false;
    return particle.system->getTechnique(0)->getNumberOfEmittedParticles() != 0;
}

bool pause_torchlight_particle(TorchlightParticle& particle) {
    if (particle.system == nullptr || !particle.system->isVisible()) return false;
    particle.system->pause();
    return true;
}

bool cparticle_system_loops(const TorchlightParticle& particle) {
    if (particle.system == nullptr) return false;
    for (size_t t = 0; t < particle.system->getNumTechniques(); ++t) {
        PU::ParticleTechnique* technique = particle.system->getTechnique(t);
        for (size_t e = 0; e < technique->getNumEmitters(); ++e) {
            PU::ParticleEmitter* emitter = technique->getEmitter(e);
            if (emitter->isNonExpiring() || emitter->getNumberOfLoops() == -1) return true;
        }
    }
    return false;
}

size_t torchlight_particle_count(const TorchlightParticle& particle) {
    return particle.system != nullptr ? particle.system->getNumberOfEmittedParticles() : 0;
}

void destroy_torchlight_particle(Ogre::SceneManager* scene, TorchlightParticle& particle) {
    if (particle.system != nullptr) {
        // A carried system hangs off a bone's TagPoint, whose `getParentSceneNode`
        // is the entity's node - not a node that holds it.
        particle.system->detachFromParent();
        PU::ParticleSystemManager::getSingletonPtr()->destroyParticleSystem(particle.system, scene);
        particle.system = nullptr;
        particle.movable = nullptr;
    }
    if (particle.node != nullptr) {
        scene->destroySceneNode(particle.node);
        particle.node = nullptr;
    }
}

}  // namespace tl::render

#else  // !TL_HAVE_PARTICLEUNIVERSE

namespace tl::render {

bool can_build_torchlight_particle(const tl::formats::CmpScene&, const tl::formats::CmpNode&,
                                   std::string* why) {
    if (why) *why = "ParticleUniverse is not built in";
    return false;
}
bool build_torchlight_particle(const ParticleBuildContext&, const tl::formats::CmpScene&,
                               const tl::formats::CmpNode&, const Ogre::Vector3&,
                               const Ogre::Quaternion&, TorchlightParticle&) {
    return false;
}
void start_torchlight_particle(TorchlightParticle&) {}
void stop_torchlight_particle_emission(TorchlightParticle&) {}
void stop_torchlight_particle(TorchlightParticle&) {}
bool set_torchlight_affector_enabled(TorchlightParticle&, int64_t, bool, bool*) { return false; }
bool set_torchlight_emitter_enabled(TorchlightParticle&, int64_t, bool) { return false; }
void start_torchlight_wrapper(TorchlightParticle&) {}
void stop_torchlight_wrapper(TorchlightParticle&) {}
void update_torchlight_wrapper(TorchlightParticle&) {}
void set_torchlight_wrapper_visible(TorchlightParticle&, bool) {}
bool torchlight_particle_no_cull(const TorchlightParticle&) { return false; }
void force_torchlight_particle_update(TorchlightParticle&, float) {}
size_t torchlight_particle_line_emitters(const TorchlightParticle&) { return 0; }
void set_torchlight_particle_line_end(TorchlightParticle&, const Ogre::Vector3&) {}
bool torchlight_particle_updating(const TorchlightParticle&) { return false; }
bool cparticle_system_loops(const TorchlightParticle&) { return false; }
bool pause_torchlight_particle(TorchlightParticle&) { return false; }
size_t torchlight_particle_count(const TorchlightParticle&) { return 0; }
void destroy_torchlight_particle(Ogre::SceneManager*, TorchlightParticle&) {}

}  // namespace tl::render

#endif  // TL_HAVE_PARTICLEUNIVERSE
