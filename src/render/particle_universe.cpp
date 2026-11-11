#include "render/particle_universe.h"

#include "core/log.h"
#include "core/random.h"

#if defined(TL_HAVE_PARTICLEUNIVERSE)

#include <OgreBillboardSet.h>
#include <OgreMaterialManager.h>
#include <OgreMath.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreShaderGenerator.h>
#include <OgreTechnique.h>

#include "ParticleAffectors/ParticleUniverseColourAffector.h"
#include "ParticleRenderers/ParticleUniverseBillboardRenderer.h"
#include "ParticleRenderers/ParticleUniverseEntityRenderer.h"
#include "ParticleUniverseRoot.h"
#include "ParticleUniverseSystem.h"
#include "ParticleUniverseSystemManager.h"

#include <cstdio>
#include <map>
#include <string>

namespace tl::render {

namespace {

// `Ogre::Math::UnitRandom`: the game thread's `rand()` times 1/32767, the stream the game's own
// `rand()` draws share, so ParticleUniverse's and the ribbon trails' draws move it as on the console.
class CrtUnitRandom : public Ogre::Math::RandomValueProvider {
public:
    Ogre::Real getRandomUnit() override {
        return static_cast<float>(tl::core::crt_random().next()) * 3.0518509e-05f;
    }
};
CrtUnitRandom g_crt_unit_random;

constexpr const char* kSmokeName = "tl/pu_smoke";

ParticleUniverse::DynamicAttribute* fixed(float value) {
    auto* attribute =
        PU_NEW_T(ParticleUniverse::DynamicAttributeFixed, Ogre::MEMCATEGORY_SCENE_OBJECTS)();
    attribute->setValue(value);
    return attribute;
}

/// Untextured, unlit and additive. Vertex colour is tracked explicitly: the billboard renderer
/// writes each particle's colour there, and RTSS's shader only reads it when asked.
void ensure_smoke_material() {
    auto& materials = Ogre::MaterialManager::getSingleton();
    if (materials.resourceExists(kSmokeName, Ogre::RGN_DEFAULT)) return;
    Ogre::MaterialPtr material = materials.create(kSmokeName, Ogre::RGN_DEFAULT);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setSceneBlending(Ogre::SBT_ADD);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
}

/// A material clone the fork's entity renderer lets go of
/// (`EntityRenderer::setMaterialCloneReleaser`, a DIVERGENCE: the original keeps
/// them): its shader-generated techniques first, since the generator holds
/// pointers into the material, then the material.
void release_material_clone(const Ogre::MaterialPtr& material) {
    if (auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
        shaders->removeAllShaderBasedTechniques(*material);
    }
    Ogre::MaterialManager::getSingleton().remove(material);
}

}  // namespace

ParticleUniverseHost::~ParticleUniverseHost() { stop(); }

bool ParticleUniverseHost::start() {
    if (running_) return true;
    // The library's root is an Ogre::Singleton: construction registers it and
    // initialise() creates the ParticleSystemManager and every factory.
    auto* root = new ParticleUniverse::ParticleUniverseRoot();
    root->initialise();
    Ogre::Math::SetRandomValueProvider(&g_crt_unit_random);
    // `CGame::setup`: the manager's store of
    // visual particles goes to 20000, and the falloff to the
    // `PARTICLE_FALLOFF` setting, whose default is 130 (`CSettings::CSettings`).
    //
    auto* manager = ParticleUniverse::ParticleSystemManager::getSingletonPtr();
    manager->setMaxNumberOfParticles(20000);
    manager->setParticleFalloff(130.0f);
    // The same: systems step 1 / `PARTICLEFPS` seconds at a time, the
    // setting's default 40 (`CSettings::CSettings`); the high particle
    // detail's too (`KParticleFPSDetail` 20, 30, 40).
    ParticleUniverse::ParticleSystemManager::setParticleFPS(40.0f);
    ParticleUniverse::EntityRenderer::setMaterialCloneReleaser(&release_material_clone);
    running_ = true;
    TL_LOG_INFO("ParticleUniverse: running (OGRECave library, static, OGRE %d.%d.%d)",
                OGRE_VERSION_MAJOR, OGRE_VERSION_MINOR, OGRE_VERSION_PATCH);
    return true;
}

void ParticleUniverseHost::stop() {
    if (!running_) return;
    destroy_smoke();
    ParticleUniverse::ParticleUniverseRoot* root =
        ParticleUniverse::ParticleUniverseRoot::getSingletonPtr();
    root->shutdown();
    delete root;
    Ogre::Math::SetRandomValueProvider(nullptr);
    ParticleUniverse::EntityRenderer::setMaterialCloneReleaser(nullptr);
    running_ = false;
}

void ParticleUniverseHost::set_falloff_position(const Ogre::Vector3& position) {
    if (!running_) return;
    ParticleUniverse::ParticleSystemManager::getSingletonPtr()->setFalloffPosition(position);
}

void ParticleUniverseHost::set_game_speed(float speed) {
    if (!running_) return;
    ParticleUniverse::ParticleSystemManager::getSingletonPtr()->setGameSpeed(speed);
}

void ParticleUniverseHost::set_game_paused(bool paused) {
    if (!running_) return;
    ParticleUniverse::ParticleSystemManager::getSingletonPtr()->setGamePaused(paused);
}

bool ParticleUniverseHost::spawn_smoke(Ogre::SceneManager* scene, const Ogre::SceneNode* where,
                                       Ogre::Camera* camera) {
    if (!running_ || scene == nullptr || where == nullptr) return false;
    destroy_smoke();
    ensure_smoke_material();

    auto* manager = ParticleUniverse::ParticleSystemManager::getSingletonPtr();
    smoke_ = manager->createParticleSystem(kSmokeName, scene);
    smoke_scene_ = scene;

    ParticleUniverse::ParticleTechnique* technique = smoke_->createTechnique();
    technique->setMaterialName(kSmokeName);
    technique->setVisualParticleQuota(200);
    technique->setDefaultWidth(0.4f);
    technique->setDefaultHeight(0.4f);

    // The duration and loops a game emitter starts with (`CParticleEmitterWrapper`'s
    // constructor): with Torchlight's emitter a
    // duration of 0 would be a one-shot burst of the whole rate.
    ParticleUniverse::ParticleEmitter* emitter = technique->createEmitter("Point");
    emitter->setNumberOfLoops(-1);
    emitter->setDynDuration(fixed(1.0f));
    emitter->setDynEmissionRate(fixed(40.0f));
    emitter->setDynTotalTimeToLive(fixed(2.0f));
    emitter->setDynVelocity(fixed(3.0f));
    emitter->setDynAngle(fixed(25.0f));

    auto* colour =
        static_cast<ParticleUniverse::ColourAffector*>(technique->createAffector("Colour"));
    colour->addColour(0.0f, Ogre::ColourValue(1.0f, 0.7f, 0.2f, 1.0f));
    colour->addColour(1.0f, Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));

    smoke_node_ = scene->getRootSceneNode()->createChildSceneNode(where->_getDerivedPosition());
    smoke_node_->attachObject(smoke_);
    smoke_->prepare();
    // What `CParticleWrapper::createParticleSystem`
    // does for every system: hand it the
    // player's camera. Torchlight's techniques cull against that camera and
    // draw nothing without one.
    smoke_->notifyCurrentCamera(camera);
    smoke_->start();
    TL_LOG_INFO("--pu-smoke: a ParticleUniverse system at (%.1f, %.1f, %.1f)",
                static_cast<double>(smoke_node_->getPosition().x),
                static_cast<double>(smoke_node_->getPosition().y),
                static_cast<double>(smoke_node_->getPosition().z));
    return true;
}

size_t ParticleUniverseHost::smoke_particles() const {
    return smoke_ != nullptr ? smoke_->getNumberOfEmittedParticles() : 0;
}

void ParticleUniverseHost::log_materials(Ogre::SceneManager* scene) const {
    if (!running_ || scene == nullptr) return;
    auto& materials = Ogre::MaterialManager::getSingleton();
    size_t techniques = 0;
    size_t odd = 0;
    // One line a reason and material, with how many techniques share it.
    std::map<std::string, size_t> found;
    for (const auto& entry :
         scene->getMovableObjects(ParticleUniverse::ParticleSystemFactory::PU_FACTORY_TYPE_NAME)) {
        auto* system = static_cast<ParticleUniverse::ParticleSystem*>(entry.second);
        for (size_t i = 0; i < system->getNumTechniques(); ++i) {
            ParticleUniverse::ParticleTechnique* technique = system->getTechnique(i);
            auto* renderer =
                dynamic_cast<ParticleUniverse::BillboardRenderer*>(technique->getRenderer());
            if (renderer == nullptr || renderer->getBillboardSet() == nullptr) continue;
            ++techniques;
            const std::string& wanted = technique->getMaterialName();
            const Ogre::MaterialPtr& drawn = renderer->getBillboardSet()->getMaterial();
            const Ogre::MaterialPtr owned =
                drawn ? materials.getByName(drawn->getName(), drawn->getGroup())
                      : Ogre::MaterialPtr();
            // An unloaded material is compiled again when next queued
            // (`RenderQueue::addRenderable`'s `prepare()`); one prepared or loaded with no
            // supported technique is not, and draws as blank white from then on.
            const char* why = nullptr;
            if (!drawn) {
                why = "no material";
            } else if (drawn->getName() != wanted) {
                why = "another material";
            } else if (owned != drawn) {
                why = "a material the manager no longer owns";
            } else if (drawn->getSupportedTechniques().empty() &&
                       drawn->getLoadingState() != Ogre::Resource::LOADSTATE_UNLOADED) {
                why = "a material with no supported technique (blank white)";
            }
            if (why == nullptr) continue;
            ++odd;
            char line[512];
            std::snprintf(line, sizeof(line),
                          "%s: named '%s', holds '%s' (state %d, %zu technique(s))", why,
                          wanted.c_str(), drawn ? drawn->getName().c_str() : "",
                          drawn ? static_cast<int>(drawn->getLoadingState()) : -1,
                          drawn ? static_cast<size_t>(drawn->getNumTechniques()) : size_t{0});
            ++found[line];
        }
    }
    size_t lines = 0;
    for (const auto& [line, count] : found) {
        if (++lines > 16) break;
        TL_LOG_WARN("shot particle: %zu technique(s) draw with %s", count, line.c_str());
    }
    TL_LOG_INFO("shot particles: %zu billboard technique(s), %zu drawing with something else",
                techniques, odd);
}

void ParticleUniverseHost::destroy_smoke() {
    if (smoke_ == nullptr) return;
    if (smoke_node_ != nullptr) {
        smoke_node_->detachAllObjects();
        smoke_scene_->destroySceneNode(smoke_node_);
        smoke_node_ = nullptr;
    }
    ParticleUniverse::ParticleSystemManager::getSingletonPtr()->destroyParticleSystem(smoke_,
                                                                                      smoke_scene_);
    smoke_ = nullptr;
    smoke_scene_ = nullptr;
}

void count_particle_systems(Ogre::SceneManager* scene, size_t& total, size_t& started,
                            size_t& attached) {
    total = started = attached = 0;
    if (scene == nullptr) return;
    for (const auto& entry :
         scene->getMovableObjects(ParticleUniverse::ParticleSystemFactory::PU_FACTORY_TYPE_NAME)) {
        auto* system = static_cast<ParticleUniverse::ParticleSystem*>(entry.second);
        ++total;
        if (system->getState() == ParticleUniverse::ParticleSystem::PSS_STARTED) ++started;
        if (system->isAttached()) ++attached;
    }
}

}  // namespace tl::render

#else  // !TL_HAVE_PARTICLEUNIVERSE: not checked out, or left out by TL_WITH_PARTICLEUNIVERSE

namespace tl::render {

ParticleUniverseHost::~ParticleUniverseHost() = default;

bool ParticleUniverseHost::start() {
    TL_LOG_INFO(
        "ParticleUniverse: not built in (no external/particleuniverse, or "
        "TL_WITH_PARTICLEUNIVERSE=OFF)");
    return false;
}

void ParticleUniverseHost::stop() {}

void ParticleUniverseHost::set_falloff_position(const Ogre::Vector3&) {}

void ParticleUniverseHost::set_game_speed(float) {}

void ParticleUniverseHost::set_game_paused(bool) {}

bool ParticleUniverseHost::spawn_smoke(Ogre::SceneManager*, const Ogre::SceneNode*, Ogre::Camera*) {
    return false;
}

size_t ParticleUniverseHost::smoke_particles() const { return 0; }

void ParticleUniverseHost::log_materials(Ogre::SceneManager*) const {}

void ParticleUniverseHost::destroy_smoke() {}

void count_particle_systems(Ogre::SceneManager*, size_t& total, size_t& started, size_t& attached) {
    total = started = attached = 0;
}

}  // namespace tl::render

#endif  // TL_HAVE_PARTICLEUNIVERSE
