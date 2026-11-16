#include "render/automap.h"

#include "core/log.h"
#include "core/passability.h"

#include <OgreHardwareBufferManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreShaderGenerator.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace tl::render {

namespace {

struct TileDef {
    float x;
    float y;
    float size;
    float rot_deg;
    float scale;
};

// The 34 tiles from the native Linux 64-bit binary (.rodata KAutomapTileX/Y/Size/Rot/Scale).
// All atlas coordinates reference media/sharedtextures/automap.dds (512x512).
constexpr TileDef kTiles[34] = {
    {0.0f, 0.0f, 32.0f, 0.0f, 1.0f},      // 0: right angle corner
    {32.0f, 0.0f, 32.0f, 0.0f, 1.0f},     // 1: straight wall
    {64.0f, 0.0f, 32.0f, 0.0f, 1.0f},     // 2: diag bottomleft-topright
    {96.0f, 0.0f, 32.0f, 0.0f, 1.0f},     // 3: convex corner
    {128.0f, 0.0f, 32.0f, 0.0f, 1.0f},    // 4: floor
    {160.0f, 0.0f, 32.0f, 0.0f, 1.0f},    // 5: bottom-right diagonal floor
    {192.0f, 0.0f, 32.0f, 0.0f, 1.0f},    // 6: bottom-left diagonal floor
    {0.0f, 32.0f, 96.0f, 90.0f, 1.0f},    // 7: bridge ns 2
    {64.0f, 32.0f, 96.0f, 90.0f, 1.0f},   // 8: double-bridge ns2
    {-32.0f, 32.0f, 96.0f, 0.0f, 1.0f},   // 9: bridge ns2 offset 1
    {64.0f, 32.0f, 96.0f, 0.0f, 1.0f},    // 10: bridge ns2
    {0.0f, 128.0f, 96.0f, 0.0f, 1.0f},    // 11: doublewide bridge
    {224.0f, 0.0f, 32.0f, 0.0f, 1.0f},    // 12: top-left diagonal floor
    {160.0f, 128.0f, 96.0f, 0.0f, 1.0f},  // 13: diagonal bridge
    {192.0f, 32.0f, 32.0f, 0.0f, 1.0f},   // 14: top-right diagonal floor
    {192.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 15: pet icon
    {224.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 16: player icon
    {0.0f, 256.0f, 160.0f, 0.0f, 1.0f},   // 17: triple exit
    {160.0f, 384.0f, 96.0f, 0.0f, 1.0f},  // 18: triple-long bridge
    {256.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 19: stairsup
    {288.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 20: stairsdown
    {320.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 21: question
    {352.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 22: exclamation
    {384.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 23: exclamation gray
    {416.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 24: merchant
    {448.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 25: question gray
    {0.0f, 0.0f, 32.0f, 0.0f, 1.0f},      // 26: fixed map (static background)
    {0.0f, 0.0f, 32.0f, 270.0f, 1.0f},    // 27: right angle corner rotated
    {480.0f, 224.0f, 32.0f, 0.0f, 1.5f},  // 28: enchanter
    {384.0f, 256.0f, 32.0f, 0.0f, 1.5f},  // 29: gemrecover
    {416.0f, 256.0f, 32.0f, 0.0f, 1.5f},  // 30: gambler
    {448.0f, 256.0f, 32.0f, 0.0f, 1.5f},  // 31: transmuter
    {480.0f, 256.0f, 32.0f, 0.0f, 1.5f},  // 32: itemrecover
    {320.0f, 256.0f, 32.0f, 0.0f, 1.5f}   // 33: waypoint
};

/// World units of orthographic window per unit of the authored zoom: `CAutomap::zoom`
/// passes extents (-zoom, +zoom, +zoom, -zoom). The fullscreen map's own
/// factor (.rodata = 5.0) and aspect are not applied yet (MapMode::Fullscreen).
constexpr float kZoomToOrtho = 2.0f;

/// PORT: how much more of the level a second screen's map shows than the corner circle. The
/// circle there is some four times the corner's across; at the same window the icons' 32 pixel
/// tiles would be drawn four times their size.
constexpr float kSecondScreenView = 2.0f;

/// PORT: where the circle is in the mask's and the ring's pictures (`automapmask.dds`,
/// `automapoverlay.dds`, both 256 square): its centre 0.533 of the way across and down, not the
/// middle, and its radius 0.455 of the width. Drawn over the whole viewport, as the console
/// draws them, the circle is that far off the player's arrow, which is the viewport's middle;
/// small in a corner it does not show, across a whole screen it does.
constexpr float kCircleCentre = 0.5332f;
constexpr float kCircleRadius = 0.4551f;
/// How much of a second screen's viewport the circle is drawn across, from its middle. Not
/// less: the pictures end just past the circle on the right and below (0.467 from its centre),
/// and must still cover the viewport there for the mask to hide the map outside it.
constexpr float kSecondScreenCircle = 0.985f;

/// The viewports' z-orders: the map's on either window (the world is 0 on the main one), and on
/// a second screen the black the rest of it is cleared to.
constexpr int kMapZOrder = 2;
constexpr int kBackdropZOrder = 0;

constexpr float kAtlasSize = 512.0f;
constexpr float kTexelHalfOffset = 0.005f;  // half-texel inset to prevent bleeding
constexpr float kTileBasis = 4.0f;

/// A vertex colour's channel as `CAutomap::update` packs it:
/// `vmaddfp` by 255 / 2^22 onto 3.0, then `vpkd3d128` takes the low bits of
/// the mantissa - the nearest byte, saturated.
uint8_t pack_channel(float value) {
    const float biased = std::fma(value, 255.0f / 4194304.0f, 3.0f);
    const float byte = (biased - 3.0f) * 4194304.0f;
    return static_cast<uint8_t>(std::clamp(byte, 0.0f, 255.0f));
}

/// Mesh and texture names, `automap_<n>` and `automaptex_<n>`
/// (`STRINGS::uniqueName`).
std::string unique_name(const char* prefix) {
    static unsigned serial = 0;
    return prefix + std::to_string(++serial);
}

}  // namespace

Automap::Automap() = default;

Automap::~Automap() { shutdown(); }

/// The materials the map draws through: `CAutomap::CAutomap` makes
/// `AutomapCull` (the circle's depth mask, `automapmask.dds`), `AutomapOverlay` (its ring,
/// `automapoverlay.dds`) and `Automap` (the icons' atlas), unlit, unfogged and alpha blended;
/// `CAutomap::create` makes the map's own (`AutomapRender`).
///
/// A manual texture on this port must be `PF_BYTE_RGBA`: `PF_BYTE_BGRA` maps to the sized
/// `GL_BGRA8_EXT` (`GLES2PixelUtil::getGLInternalFormat`), which the Switch's ES 3
/// `glTexStorage2D()` (`GLES2Texture::_createGLTexResource`) rejects, leaving it black.
void Automap::ensure_materials() {
    if (mMaterialsCreated) return;
    mMaterialsCreated = true;

    auto& mat_mgr = Ogre::MaterialManager::getSingleton();
    const std::string& group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
    const auto console_pass = [](Ogre::Pass* pass) {
        pass->setLightingEnabled(false);
        pass->setFog(true, Ogre::FOG_NONE);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    };

    // The mask: depth checked and written, colour not, alpha rejected above 150, so outside the
    // circle the map and icons fail their depth test. With the check off GL writes no depth.
    if (!mat_mgr.resourceExists("AutomapCull", group)) {
        Ogre::Pass* pass = mat_mgr.create("AutomapCull", group)->getTechnique(0)->getPass(0);
        console_pass(pass);
        pass->setDepthCheckEnabled(true);
        pass->setDepthWriteEnabled(true);
        pass->setColourWriteEnabled(false);
        pass->setAlphaRejectSettings(Ogre::CMPF_GREATER, 150, false);
        pass->createTextureUnitState("media/sharedtextures/automapmask.dds");
    }

    // The ring, drawn last: neither depth checked nor written.
    if (!mat_mgr.resourceExists("AutomapOverlay", group)) {
        Ogre::Pass* pass = mat_mgr.create("AutomapOverlay", group)->getTechnique(0)->getPass(0);
        console_pass(pass);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->createTextureUnitState("media/sharedtextures/automapoverlay.dds");
    }

    // The icons: depth checked against the mask, not written; the atlas bilinear, no mipmaps.
    // Every fade here sets a billboard's colour (the vertex diffuse); under RTSS a default unit's
    // `LBS_CURRENT` alpha did not carry it, so `LBS_DIFFUSE` is named on both channels.
    if (!mat_mgr.resourceExists("Automap", group)) {
        Ogre::Pass* pass = mat_mgr.create("Automap", group)->getTechnique(0)->getPass(0);
        console_pass(pass);
        pass->setSelfIllumination(Ogre::ColourValue::Black);
        pass->setDepthCheckEnabled(true);
        pass->setDepthWriteEnabled(false);
        // PORT: billboards drawn from either side, their colour the diffuse.
        pass->setCullingMode(Ogre::CULL_NONE);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        Ogre::TextureUnitState* tus =
            pass->createTextureUnitState("media/sharedtextures/automap.dds");
        tus->setTextureFiltering(Ogre::FO_LINEAR, Ogre::FO_LINEAR, Ogre::FO_NONE);
        tus->setColourOperationEx(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_DIFFUSE);
        tus->setAlphaOperation(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_DIFFUSE);
    }

    // No eager createShaderBasedTechnique(): the ShaderResolver (a MaterialManager::Listener)
    // builds each material's technique in handleSchemeNotFound() the first time it is drawn.

    // What each layer samples, one line each.
    for (const char* mat_name : {"AutomapCull", "AutomapOverlay", "Automap"}) {
        Ogre::MaterialPtr m = mat_mgr.getByName(mat_name, group);
        if (!m || m->getNumTechniques() == 0) continue;
        Ogre::Pass* p = m->getTechnique(0)->getPass(0);
        if (!p || p->getNumTextureUnitStates() == 0) continue;
        Ogre::TextureUnitState* tus = p->getTextureUnitState(0);
        const Ogre::TexturePtr tex = tus->_getTexturePtr();
        if (!tex) {
            TL_LOG_INFO("automap: '%s' -> '%s' (not bound yet)", mat_name,
                        tus->getTextureName().c_str());
            continue;
        }
        TL_LOG_INFO("automap: '%s' -> '%s' %ux%u format=%d", mat_name,
                    tus->getTextureName().c_str(), tex->getWidth(), tex->getHeight(),
                    static_cast<int>(tex->getFormat()));
    }
}

bool Automap::init(Ogre::SceneManager* scene_mgr, Ogre::RenderWindow* window, float y_ratio) {
    mSceneMgr = scene_mgr;
    mWindow = window;
    mYRatio = y_ratio > 0.0f ? y_ratio : 1.0f;

    if (!mSceneMgr || !mWindow) return false;

    ensure_materials();

    // Create orthographic camera
    if (mSceneMgr->hasCamera("AutomapCam")) {
        mCamera = mSceneMgr->getCamera("AutomapCam");
    } else {
        mCamera = mSceneMgr->createCamera("AutomapCam");
    }
    mCamera->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
    mCamera->setNearClipDistance(0.1f);
    mCamera->setFarClipDistance(5000.0f);
    mCamera->setAspectRatio(1.0f);
    apply_zoom();
    mCamera->setVisibilityFlags(kVisibilityAutomap);

    if (mSceneMgr->hasSceneNode("tl/automap_cam_node")) {
        mCameraNode = mSceneMgr->getSceneNode("tl/automap_cam_node");
    } else {
        mCameraNode = mSceneMgr->getRootSceneNode()->createChildSceneNode("tl/automap_cam_node");
    }
    if (!mCamera->isAttached()) {
        mCameraNode->attachObject(mCamera);
    }
    mCameraNode->setFixedYawAxis(false);
    const Ogre::Quaternion q_x(Ogre::Degree(-90.0f), Ogre::Vector3::UNIT_X);
    const Ogre::Quaternion q_y(Ogre::Degree(kAutomapCameraYawDeg), Ogre::Vector3::UNIT_Y);
    mCameraNode->setOrientation(q_y * q_x);

    // No viewport on mWindow here: the automap's exists only during gameplay.
    mViewport = nullptr;
    mVisible = false;

    // 1. Cull quad: circular depth mask (Queue kQueueAutomapCull), its corners
    // at -1.01 and 1.01 (`AutomapCullQuad`).
    mCullQuad = new Ogre::Rectangle2D(true);
    mCullQuad->setCorners(-1.01f, 1.01f, 1.01f, -1.01f);
    mCullQuad->setBoundingBox(Ogre::AxisAlignedBox::BOX_INFINITE);
    mCullQuad->setMaterial(Ogre::MaterialManager::getSingleton().getByName("AutomapCull"));
    mCullQuad->setRenderQueueGroup(kQueueAutomapCull);
    mCullQuad->setVisibilityFlags(kVisibilityAutomap);
    mCullQuad->setVisible(false);

    mNodeCull = mSceneMgr->getRootSceneNode()->createChildSceneNode("sn_AutomapCull");
    mNodeCull->attachObject(mCullQuad);

    // 2. Ring quad: the circle's ring (Queue kQueueAutomapRing,
    // `AutomapOverlayQuad`).
    mRingQuad = new Ogre::Rectangle2D(true);
    mRingQuad->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);
    mRingQuad->setBoundingBox(Ogre::AxisAlignedBox::BOX_INFINITE);
    mRingQuad->setMaterial(Ogre::MaterialManager::getSingleton().getByName("AutomapOverlay"));
    mRingQuad->setRenderQueueGroup(kQueueAutomapRing);
    mRingQuad->setVisibilityFlags(kVisibilityAutomap);
    mRingQuad->setVisible(false);

    mNodeRing = mSceneMgr->getRootSceneNode()->createChildSceneNode("sn_AutomapRing");
    mNodeRing->attachObject(mRingQuad);

    // 3. Billboard set 'bb': map tiles and icons
    mBillboardSet = mSceneMgr->createBillboardSet("bb", 1024);
    mBillboardSet->setMaterialName("Automap");
    mBillboardSet->setDefaultDimensions(kTileBasis, kTileBasis);
    mBillboardSet->setBillboardType(Ogre::BBT_PERPENDICULAR_COMMON);
    mBillboardSet->setCommonDirection(Ogre::Vector3(0.0f, 1.0f, 0.0f));
    mBillboardSet->setCommonUpVector(Ogre::Vector3(0.0f, 0.0f, -1.0f));
    mBillboardSet->setUseAccurateFacing(true);
    mBillboardSet->setRenderQueueGroup(kQueueAutomapTiles);
    mBillboardSet->setVisibilityFlags(kVisibilityAutomap);
    mBillboardSet->setBounds(Ogre::AxisAlignedBox::BOX_INFINITE, 10000.0f);

    mNodeTiles = mSceneMgr->getRootSceneNode()->createChildSceneNode("sn_AutomapTiles");
    mNodeTiles->attachObject(mBillboardSet);

    // The player icon, tile 17: CAutomap::CAutomap's first addTile(0x11), kept at 0x70 for
    // setPlayerPosition.
    mPlayerIcon = add_tile(17, Ogre::Vector3::ZERO, Ogre::Vector3(0.0f, 0.0f, -1.0f), true, true);
    if (mPlayerIcon) {
        mPlayerIcon->setDimensions(3.5f, 3.5f);
    }

    // Create pet icon: Tile 16 (the second addTile(0x10) call, stored at offset
    // 0x78, which setPetPosition reads)
    mPetIcon = add_tile(16, Ogre::Vector3::ZERO, Ogre::Vector3(0.0f, 0.0f, -1.0f), true, false);
    if (mPetIcon) {
        mPetIcon->setDimensions(3.0f, 3.0f);
    }

    mOwnQueue.owner = this;
    mSceneMgr->removeRenderQueueListener(&mOwnQueue);
    mSceneMgr->addRenderQueueListener(&mOwnQueue);
    attach(mWindow);

    TL_LOG_INFO("automap initialized (mode=%d, zoom=%.1f)", static_cast<int>(mMode), mZoom);
    return true;
}

void Automap::attach(Ogre::RenderTarget* target) {
    if (target == nullptr) return;
    target->removeListener(&mOwnQueue);
    target->addListener(&mOwnQueue);
}

void Automap::detach(Ogre::RenderTarget* target) {
    if (target != nullptr) target->removeListener(&mOwnQueue);
}

void Automap::OwnQueue::preViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) {
    if (owner == nullptr || owner->mSceneMgr == nullptr || owner->mCamera == nullptr) return;
    if (evt.source == nullptr || evt.source->getCamera() != owner->mCamera) return;
    owner->mSceneMgr->setFindVisibleObjects(false);
    active = true;
}

void Automap::OwnQueue::postViewportUpdate(const Ogre::RenderTargetViewportEvent&) {
    if (!active) return;
    active = false;
    if (owner != nullptr && owner->mSceneMgr != nullptr)
        owner->mSceneMgr->setFindVisibleObjects(true);
}

void Automap::OwnQueue::preRenderQueues() {
    if (!active || owner == nullptr || owner->mSceneMgr == nullptr) return;
    // What `SceneNode::_findVisibleObjects` would have queued of the map:
    // each object told the camera, then queued if it is visible to this
    // viewport (its own flag and the viewport's mask, which isVisible() reads).
    // Their boxes are infinite, so no frustum test would have dropped one.
    Ogre::RenderQueue* queue = owner->mSceneMgr->getRenderQueue();
    Ogre::MovableObject* const objects[] = {owner->mCullQuad, owner->mMapEntity,
                                            owner->mBillboardSet, owner->mRingQuad};
    for (Ogre::MovableObject* object : objects) {
        if (object == nullptr || !object->isInScene()) continue;
        queue->processVisibleObject(object, owner->mCamera, false, nullptr);
    }
}

// PORT: the small map moved to a second display's window, or back.
void Automap::set_second_screen(Ogre::RenderWindow* second) {
    const bool was_visible = mVisible;
    if (second != mSecond) {
        hide();
        if (mSecond != nullptr) {
            if (mSecond->hasViewportWithZOrder(kBackdropZOrder)) {
                mSecond->removeViewport(kBackdropZOrder);
            }
            detach(mSecond);
        }
        mSecond = second;
        if (mSecond != nullptr && mCamera != nullptr) {
            // The whole screen cleared to black under the map: a viewport of the map's camera
            // that sees nothing (OwnQueue queues only what the viewport's mask lets through).
            attach(mSecond);
            Ogre::Viewport* backdrop = mSecond->addViewport(mCamera, kBackdropZOrder);
            backdrop->setClearEveryFrame(true);
            backdrop->setBackgroundColour(Ogre::ColourValue::Black);
            backdrop->setVisibilityMask(0);
            backdrop->setOverlaysEnabled(false);
            backdrop->setMaterialScheme(Ogre::MSN_SHADERGEN);
        }
        TL_LOG_INFO("automap: %s", mSecond != nullptr ? "the small map is on the second screen"
                                                      : "the small map is back on the main screen");
    }
    // Shown again on the window it now belongs to; the same window again is a new size.
    if (was_visible && mMode != MapMode::Hidden) show();
    apply_zoom();
}

// The mask's and the ring's quads. As the console places them (`AutomapCullQuad` at -1.01 and
// 1.01, `AutomapOverlayQuad` at -1 and 1), or, PORT, `centred` for a second screen: placed so
// the circle in their pictures is round the viewport's middle and nearly as wide as it.
void Automap::place_circle(bool centred) {
    float left = -1.0f, top = 1.0f, right = 1.0f, bottom = -1.0f;
    if (centred) {
        const float before = kCircleCentre / kCircleRadius * kSecondScreenCircle;
        const float after = (1.0f - kCircleCentre) / kCircleRadius * kSecondScreenCircle;
        left = -before;
        top = before;
        right = after;
        bottom = -after;
    }
    if (mCullQuad) mCullQuad->setCorners(left * 1.01f, top * 1.01f, right * 1.01f, bottom * 1.01f);
    if (mRingQuad) mRingQuad->setCorners(left, top, right, bottom);
}

// PORT: the second screen's top edge given to the HUD's strip, the map under it.
void Automap::set_second_screen_top(float fraction) {
    const float top = std::clamp(fraction, 0.0f, 0.5f);
    if (top == mSecondTop) return;
    mSecondTop = top;
    update_viewport_dimensions();
}

bool Automap::second_screen_square(float& left, float& top, float& side) const {
    if (mSecond == nullptr) return false;
    const float second_w = static_cast<float>(mSecond->getWidth());
    const float second_h = static_cast<float>(mSecond->getHeight());
    if (second_w <= 0.0f || second_h <= 0.0f) return false;
    const float room = second_h * (1.0f - mSecondTop);
    side = std::min(second_w, room) * kSecondScreenFill;
    left = (second_w - side) * 0.5f;
    top = second_h * mSecondTop + (room - side) * 0.5f;
    return true;
}

void Automap::shutdown() {
    hide();
    set_second_screen(nullptr);

    detach(mWindow);
    if (mSceneMgr) mSceneMgr->removeRenderQueueListener(&mOwnQueue);
    mOwnQueue.active = false;

    destroy_map();
    if (mSceneMgr) {
        if (mNodeTiles) {
            mNodeTiles->detachAllObjects();
            mSceneMgr->destroySceneNode(mNodeTiles);
            mNodeTiles = nullptr;
        }
        if (mBillboardSet) {
            mSceneMgr->destroyBillboardSet(mBillboardSet);
            mBillboardSet = nullptr;
        }
        if (mNodeCull) {
            mNodeCull->detachAllObjects();
            mSceneMgr->destroySceneNode(mNodeCull);
            mNodeCull = nullptr;
        }
        if (mCullQuad) {
            delete mCullQuad;
            mCullQuad = nullptr;
        }
        if (mNodeRing) {
            mNodeRing->detachAllObjects();
            mSceneMgr->destroySceneNode(mNodeRing);
            mNodeRing = nullptr;
        }
        if (mRingQuad) {
            delete mRingQuad;
            mRingQuad = nullptr;
        }
        if (mCameraNode) {
            if (mCamera && mCamera->isAttached()) {
                mCameraNode->detachObject(mCamera);
            }
            mSceneMgr->destroySceneNode(mCameraNode);
            mCameraNode = nullptr;
        }
        if (mCamera) {
            mSceneMgr->destroyCamera(mCamera);
            mCamera = nullptr;
        }
        mSceneMgr = nullptr;
    }

    mWindow = nullptr;
    mPlayerIcon = nullptr;
    mPetIcon = nullptr;
    mNpcIcons.clear();
    mHiddenNpcIcons.clear();
    mTileBillboards.clear();
    mReveal.clear();
    mMaterialsCreated = false;
}

// PORT: diagnostics.
size_t Automap::billboard_count() const {
    return mBillboardSet ? static_cast<size_t>(mBillboardSet->getNumBillboards()) : 0;
}

void Automap::clear() {
    if (mBillboardSet) {
        mBillboardSet->clear();
    }
    destroy_map();
    mReveal.clear();
    mTileBillboards.clear();
    mNpcIcons.clear();
    mHiddenNpcIcons.clear();

    // Recreate player and pet icons (17 = player, 16 = pet - see init())
    if (mBillboardSet) {
        mPlayerIcon =
            add_tile(17, Ogre::Vector3::ZERO, Ogre::Vector3(0.0f, 0.0f, -1.0f), true, true);
        if (mPlayerIcon) mPlayerIcon->setDimensions(3.5f, 3.5f);

        mPetIcon =
            add_tile(16, Ogre::Vector3::ZERO, Ogre::Vector3(0.0f, 0.0f, -1.0f), true, mPetVisible);
        if (mPetIcon) mPetIcon->setDimensions(3.0f, 3.0f);
    }
}

Ogre::Billboard* Automap::add_tile(int tile_id, const Ogre::Vector3& pos, const Ogre::Vector3& dir,

                                   bool is_icon, bool visible) {
    if (!mBillboardSet) return nullptr;

    // Convert 1-based tile index to 0-based
    int idx = tile_id - 1;
    if (idx < 0 || idx >= 34) {
        idx = 4;  // default to floor
    }

    const TileDef& def = kTiles[idx];

    Ogre::ColourValue initial_col = (visible || mReveal.exposed())
                                        ? Ogre::ColourValue::White
                                        : Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.0f);

    Ogre::Billboard* bb = mBillboardSet->createBillboard(pos, initial_col);

    // Atlas UVs with half-texel border to avoid texture bleed
    const float u0 = (def.x / kAtlasSize) + kTexelHalfOffset;
    const float v0 = (def.y / kAtlasSize) + kTexelHalfOffset;
    const float u1 = ((def.x + def.size) / kAtlasSize) - kTexelHalfOffset;
    const float v1 = ((def.y + def.size) / kAtlasSize) - kTexelHalfOffset;
    bb->setTexcoordRect(u0, v0, u1, v1);

    // Tile dimensions: 4.0 * scale * (size / 32.0)
    const float dim = kTileBasis * def.scale * (def.size / 32.0f);
    bb->setDimensions(dim, dim);

    // Orientation: direction angle + authored rotation
    const float heading = std::atan2(dir.x, dir.z);
    const float rot_rad = heading + Ogre::Math::DegreesToRadians(def.rot_deg);
    bb->setRotation(Ogre::Radian(rot_rad));

    if (is_icon) {
        if (tile_id != 16 && tile_id != 17) {  // not player/pet
            // `CAutomap::addTile` with the
            // icon flag: an icon added visible starts at alpha 0 and fades in
            // (`CAutomap::update`); one added not visible starts at 1.
            NpcIcon icon;
            icon.billboard = bb;
            icon.alpha = visible ? 0.0f : 1.0f;
            if (visible) bb->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.0f));
            mNpcIcons.push_back(icon);
        }
    } else {
        TileBillboard tb;
        tb.billboard = bb;
        tb.opacity = initial_col.a;
        mTileBillboards.push_back(tb);
    }

    return bb;
}

namespace {

/// The picture, `automaptex_<n>`. PF_BYTE_RGBA rather than the console's
/// PF_R8G8B8 (see the note above Automap::ensure_materials()); the grey is the
/// same. The console asks for the default mipmaps, and its texture unit samples
/// none of them, so none are made.
Ogre::TexturePtr upload_map_texture(const tl::core::AutomapTexture& picture) {
    Ogre::TexturePtr texture = Ogre::TextureManager::getSingleton().createManual(
        unique_name("automaptex_"), Ogre::RGN_DEFAULT, Ogre::TEX_TYPE_2D, picture.width,
        picture.height, 0, Ogre::PF_BYTE_RGBA, Ogre::TU_STATIC);
    Ogre::HardwarePixelBufferSharedPtr buffer = texture->getBuffer();
    buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& box = buffer->getCurrentLock();
    auto* data = static_cast<uint8_t*>(box.data);
    for (uint32_t y = 0; y < picture.height; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * box.rowPitch * 4;
        const uint8_t* from = picture.rgb.data() + static_cast<size_t>(y) * picture.width * 3;
        for (uint32_t x = 0; x < picture.width; ++x) {
            row[x * 4 + 0] = from[x * 3 + 0];
            row[x * 4 + 1] = from[x * 3 + 1];
            row[x * 4 + 2] = from[x * 3 + 2];
            row[x * 4 + 3] = 255;
        }
    }
    buffer->unlock();
    return texture;
}

/// `AutomapRender`, made once: lighting off, no emissive, depth checked and not
/// written, `src * src_colour + dst * (1 - src_colour)` (so black is clear and
/// the greys lighten what is under them), the vertex colour as the diffuse, and
/// the texture sampled bilinear without mipmaps. Each level's picture replaces
/// the unit's texture.
Ogre::MaterialPtr map_material(const Ogre::TexturePtr& texture) {
    auto& materials = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr material = materials.getByName("AutomapRender", Ogre::RGN_DEFAULT);
    if (!material) {
        material = materials.create("AutomapRender", Ogre::RGN_DEFAULT);
        Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setSelfIllumination(Ogre::ColourValue::Black);
        pass->setSceneBlending(Ogre::SBF_SOURCE_COLOUR, Ogre::SBF_ONE_MINUS_SOURCE_COLOUR);
        pass->setDepthCheckEnabled(true);
        pass->setDepthWriteEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        // PORT: the mesh is drawn from either side.
        pass->setCullingMode(Ogre::CULL_NONE);
        Ogre::TextureUnitState* unit = pass->createTextureUnitState();
        unit->setTextureFiltering(Ogre::FO_LINEAR, Ogre::FO_LINEAR, Ogre::FO_NONE);
        unit->setColourOperationEx(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_DIFFUSE);
        unit->setAlphaOperation(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_DIFFUSE);
    }
    material->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setTexture(texture);
    // A pass changed after its shader technique was built: drop the stale
    // technique, and ShaderResolver::handleSchemeNotFound() builds it again on
    // the next draw.
    if (auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
        shaders->removeAllShaderBasedTechniques("AutomapRender", Ogre::RGN_DEFAULT);
    }
    return material;
}

/// The first vertex buffer: a vertex every `extent / columns` units, rows along
/// z, its texture coordinate the used part of the picture.
Ogre::HardwareVertexBufferSharedPtr map_positions(const tl::core::AutomapTexture& picture,
                                                  uint32_t columns, uint32_t rows,
                                                  size_t vertex_size) {
    const auto step_x =
        static_cast<float>(static_cast<double>(picture.extent_x) / static_cast<double>(columns));
    const auto step_z =
        static_cast<float>(static_cast<double>(picture.extent_z) / static_cast<double>(rows));
    Ogre::HardwareVertexBufferSharedPtr buffer =
        Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            vertex_size, static_cast<size_t>(columns + 1) * (rows + 1),
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto* out = static_cast<float*>(buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (uint32_t row = 0; row <= rows; ++row) {
        for (uint32_t column = 0; column <= columns; ++column) {
            *out++ = static_cast<float>(static_cast<double>(column) * step_x);
            *out++ = 0.0f;
            *out++ = static_cast<float>(static_cast<double>(row) * step_z);
            *out++ =
                static_cast<float>(static_cast<double>(column) / static_cast<double>(columns)) *
                picture.used_u;
            *out++ = static_cast<float>(static_cast<double>(row) / static_cast<double>(rows)) *
                     picture.used_v;
        }
    }
    buffer->unlock();
    return buffer;
}

/// The second vertex buffer, the colours: white on an exposed level, black
/// (alpha 1) elsewhere.
Ogre::HardwareVertexBufferSharedPtr map_colours(size_t vertices, size_t vertex_size, bool exposed) {
    Ogre::HardwareVertexBufferSharedPtr buffer =
        Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            vertex_size, vertices, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const uint8_t grey = exposed ? 255 : 0;
    auto* out = static_cast<uint8_t*>(buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (size_t i = 0; i < vertices; ++i, out += 4) {
        out[0] = out[1] = out[2] = grey;
        out[3] = 255;
    }
    buffer->unlock();
    return buffer;
}

/// Six indices a cell, `(at, at + 1, at + w + 2)` and `(at, at + w + 2, at + w
/// + 1)`. Sixteen-bit, as the console's; PORT: thirty-two past 65,535 vertices,
/// where the console's would wrap.
Ogre::HardwareIndexBufferSharedPtr map_indices(uint32_t columns, uint32_t rows) {
    const size_t indices = static_cast<size_t>(columns) * rows * 6;
    const bool wide = static_cast<size_t>(columns + 1) * (rows + 1) > 0xffff;
    Ogre::HardwareIndexBufferSharedPtr buffer =
        Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            wide ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
            indices, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    std::vector<uint32_t> list;
    list.reserve(indices);
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            const uint32_t at = row * (columns + 1) + column;
            list.insert(list.end(),
                        {at, at + 1, at + columns + 2, at, at + columns + 2, at + columns + 1});
        }
    }
    void* out = buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    if (wide) {
        std::memcpy(out, list.data(), list.size() * sizeof(uint32_t));
    } else {
        auto* shorts = static_cast<uint16_t*>(out);
        for (size_t i = 0; i < list.size(); ++i) shorts[i] = static_cast<uint16_t>(list[i]);
    }
    buffer->unlock();
    return buffer;
}

}  // namespace

// CAutomap::create, then CAutomap::finalize.
void Automap::create(const tl::core::PassabilityMap& map, bool exposed) {
    destroy_map();
    mReveal.clear();
    if (mSceneMgr == nullptr) return;
    const tl::core::AutomapTexture picture = tl::core::create_automap_texture(map);
    if (picture.empty()) return;
    mReveal.create(picture, exposed);
    mMapTexture = upload_map_texture(picture);
    const Ogre::MaterialPtr material = map_material(mMapTexture);

    // The mesh: `(w + 1) x (h + 1)` vertices, `w` the extent over 4 truncated;
    // position and texture coordinates in one buffer, the colour in a second.
    const auto columns = static_cast<uint32_t>(picture.extent_x * 0.25f);
    const auto rows = static_cast<uint32_t>(picture.extent_z * 0.25f);
    // PORT: a level under 4 units a side has no cells (the console would
    // divide by zero).
    if (columns == 0 || rows == 0) return;
    const size_t vertices = static_cast<size_t>(columns + 1) * (rows + 1);
    mMapMesh =
        Ogre::MeshManager::getSingleton().createManual(unique_name("automap_"), Ogre::RGN_DEFAULT);
    Ogre::SubMesh* sub = mMapMesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    sub->vertexData->vertexStart = 0;
    sub->vertexData->vertexCount = vertices;
    Ogre::VertexDeclaration* decl = sub->vertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(0, 12, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES, 0);
    decl->addElement(1, 0, Ogre::VET_UBYTE4_NORM, Ogre::VES_DIFFUSE);
    sub->vertexData->vertexBufferBinding->setBinding(
        0, map_positions(picture, columns, rows, decl->getVertexSize(0)));
    mMapColours = map_colours(vertices, decl->getVertexSize(1), exposed);
    sub->vertexData->vertexBufferBinding->setBinding(1, mMapColours);
    sub->indexData->indexBuffer = map_indices(columns, rows);
    sub->indexData->indexStart = 0;
    sub->indexData->indexCount = static_cast<size_t>(columns) * rows * 6;
    sub->setMaterial(material);
    mMapMesh->_setBounds(
        Ogre::AxisAlignedBox(-100000.0f, -100000.0f, -100000.0f, 100000.0f, 100000.0f, 100000.0f));
    mMapMesh->load();

    mMapEntity = mSceneMgr->createEntity(mMapMesh);
    mMapEntity->setRenderQueueGroup(kQueueAutomapMap);
    mMapEntity->setVisibilityFlags(kVisibilityAutomap);
    mMapNode = mSceneMgr->getRootSceneNode()->createChildSceneNode(
        Ogre::Vector3(picture.origin_x, 0.0f, picture.origin_z));
    mMapNode->attachObject(mMapEntity);

    // CAutomap::finalize: on an exposed level the tiles and
    // icons are white too.
    mReveal.finalize();
    if (mReveal.exposed()) {
        for (TileBillboard& tile : mTileBillboards) {
            if (tile.billboard != nullptr) tile.billboard->setColour(Ogre::ColourValue::White);
        }
        for (NpcIcon& icon : mNpcIcons) {
            if (icon.billboard != nullptr) icon.billboard->setColour(Ogre::ColourValue::White);
        }
    }
    TL_LOG_INFO(
        "automap: the level's map, %ux%u texture at %d cells a pixel (%.3f x %.3f of it), "
        "%u x %u vertices over (%.1f, %.1f)+(%.1f, %.1f), reveal %d x %d%s",
        picture.width, picture.height, picture.step, static_cast<double>(picture.used_u),
        static_cast<double>(picture.used_v), columns + 1, rows + 1,
        static_cast<double>(picture.origin_x), static_cast<double>(picture.origin_z),
        static_cast<double>(picture.extent_x), static_cast<double>(picture.extent_z),
        mReveal.tiles_x(), mReveal.tiles_y(), exposed ? ", exposed (a town)" : "");
}

// PORT: the map's OGRE objects let go.
void Automap::destroy_map() {
    if (mSceneMgr != nullptr) {
        if (mMapNode != nullptr) {
            mMapNode->detachAllObjects();
            mSceneMgr->destroySceneNode(mMapNode);
        }
        if (mMapEntity != nullptr) mSceneMgr->destroyEntity(mMapEntity);
    }
    mMapNode = nullptr;
    mMapEntity = nullptr;
    mMapColours.reset();
    if (mMapMesh) {
        Ogre::MeshManager::getSingleton().remove(mMapMesh);
        mMapMesh.reset();
    }
    if (mMapTexture) {
        Ogre::TextureManager::getSingleton().remove(mMapTexture);
        mMapTexture.reset();
    }
}

// CAutomap::update: the colour buffer locked whole and every
// vertex written, rows of tiles across (`+0x70`), as (opacity, opacity, opacity, 1).
void Automap::write_map_colours() {
    if (!mMapColours) return;
    const std::vector<float>& opacity = mReveal.opacity();
    const size_t vertices = mMapColours->getNumVertices();
    auto* out = static_cast<uint8_t*>(mMapColours->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (size_t i = 0; i < vertices; ++i, out += 4) {
        const uint8_t grey = i < opacity.size() ? pack_channel(opacity[i]) : 0;
        out[0] = out[1] = out[2] = grey;
        out[3] = pack_channel(1.0f);
    }
    mMapColours->unlock();
}

// CLevelState::addAutomap copies `getFOWWidth`, `getFOWHeight` and
// `getRevealedMap`: the reveal, one value per vertex of the map.
void Automap::revealed_grid(int32_t& width, int32_t& height, std::vector<float>& cells) const {
    width = mReveal.tiles_x();
    height = mReveal.tiles_y();
    cells = mReveal.revealed_cells();
}

// CLevelState::restoreAutomap: only when the size matches.
void Automap::restore_revealed(int32_t width, int32_t height, const std::vector<float>& cells) {
    if (!mReveal.restore(width, height, cells)) {
        TL_LOG_WARN("automap: saved fog is %dx%d, the map's %dx%d; not put back", width, height,
                    mReveal.tiles_x(), mReveal.tiles_y());
        return;
    }
    const size_t revealed = static_cast<size_t>(
        std::count_if(cells.begin(), cells.end(), [](float v) { return v > 0.0f; }));
    TL_LOG_INFO("automap: fog put back, %zu of %zu vertices revealed", revealed, cells.size());
}

// PORT: the minimap's player arrow placed and turned (drawing).
void Automap::set_player_position(const Ogre::Vector3& pos, const Ogre::Quaternion& orientation) {
    if (mPlayerIcon) {
        mPlayerIcon->setPosition(pos);
        // The arrow follows the facing: the camera's up is world -Z and the texture's up faces -Z
        // at rotation 0, so rotation = facing + pi. BBR_TEXCOORD rotation is CCW on screen.
        const float facing = orientation.getYaw().valueRadians();
        mPlayerIcon->setRotation(Ogre::Radian(facing + Ogre::Math::PI));
    }

    if (mCameraNode) {
        mCameraNode->setPosition(pos.x, pos.y + mZoom * 5.0f, pos.z);
    }

    // CAutomap::setPlayerPosition: the
    // square around the player uncovered.
    mReveal.reveal_around(pos.x, pos.z);
}

// PORT: the minimap's pet icon placed (drawing).
void Automap::set_pet_position(const Ogre::Vector3& pos) {
    if (mPetIcon) {
        mPetIcon->setPosition(pos);
    }
}

// PORT: the minimap's pet icon shown or hidden (drawing).
void Automap::set_pet_visible(bool visible) {
    mPetVisible = visible;
    if (mPetIcon) {
        mPetIcon->setColour(visible ? Ogre::ColourValue::White
                                    : Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.0f));
    }
}

// `CAutomap::clearNPCIcons`.
void Automap::clear_npc_icons() {
    for (const NpcIcon& icon : mNpcIcons) {
        if (mBillboardSet && icon.billboard) {
            mBillboardSet->removeBillboard(icon.billboard);
        }
    }
    mNpcIcons.clear();
    mHiddenNpcIcons.clear();
}

// `CAutomap::setNPCBillboardVisible`: showing takes the billboard
// off the hidden list (swapping the last entry into its place), hiding adds it
// once.
void Automap::set_npc_icon_visible(Ogre::Billboard* icon, bool visible) {
    if (icon == nullptr) return;
    const auto it = std::find(mHiddenNpcIcons.begin(), mHiddenNpcIcons.end(), icon);
    if (visible) {
        if (it == mHiddenNpcIcons.end()) return;
        *it = mHiddenNpcIcons.back();
        mHiddenNpcIcons.pop_back();
    } else if (it == mHiddenNpcIcons.end()) {
        mHiddenNpcIcons.push_back(icon);
    }
}

// `CAutomap::update`, on a visible map that is dirty: the
// vertices fade toward their reveal (their colours rewritten when one moved),
// and then - not on an exposed level - the tiles. A tile over a vertex not yet
// uncovered (a grid more than 3 wide) is put out of sight at once; any other
// moves up to the reveal under it by twice the delta, at height 0, and keeps
// the map dirty until it is there.
void Automap::fade_map(float delta) {
    bool stepped = false;
    if (delta <= 0.0f) {
        // PORT: a still frame settles at once (`AutomapReveal::settle`).
        if (mReveal.settle()) write_map_colours();
        stepped = true;
    } else {
        const tl::core::AutomapReveal::Step step = mReveal.fade(delta);
        if (step.repaint) write_map_colours();
        stepped = step.stepped;
    }
    if (!stepped || mReveal.exposed()) return;
    for (TileBillboard& tile : mTileBillboards) fade_tile(tile, delta);
}

// `CAutomap::update`, one tile of the loop.
void Automap::fade_tile(TileBillboard& tile, float delta) {
    if (tile.billboard == nullptr || tile.opacity == 100.0f) return;
    Ogre::Vector3 at = tile.billboard->getPosition();
    const int cx = mReveal.cell_x(at.x);
    const int cz = mReveal.cell_z(at.z);
    if (mReveal.tiles_x() > 3 && mReveal.revealed(cx, cz) <= 0.0f) {
        if (tile.opacity != 0.0f) {
            at.y = -100000.0f;
            tile.billboard->setPosition(at);
            tile.billboard->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.0f));
            tile.opacity = 0.0f;
        }
        return;
    }
    const float target = mReveal.revealed(cx, cz);
    if (tile.opacity == target) return;
    at.y = 0.0f;
    tile.billboard->setPosition(at);
    // PORT: a still frame takes the reveal at once.
    tile.opacity = delta <= 0.0f ? target : std::fma(delta, 2.0f, tile.opacity);
    if (!(tile.opacity < target)) tile.opacity = target;
    if (tile.opacity != target) mReveal.mark_dirty();
    tile.billboard->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, tile.opacity));
}

void Automap::update(const Ogre::Vector3& player_pos, float delta) {
    if (mMode == MapMode::Hidden) return;

    // Follow player position from above
    if (mCameraNode) {
        mCameraNode->setPosition(player_pos.x, player_pos.y + mZoom * 5.0f, player_pos.z);
    }

    if (mVisible) fade_map(delta);

    // `CAutomap::update`, the icon loop: a
    // hidden icon, or one over a vertex not yet uncovered (not on an exposed
    // level, and a grid more than 3 wide), goes to alpha 0 at once; any other
    // fades in by twice the frame's delta a second up to 1.
    for (NpcIcon& icon : mNpcIcons) {
        if (!icon.billboard) continue;
        bool shown = std::find(mHiddenNpcIcons.begin(), mHiddenNpcIcons.end(), icon.billboard) ==
                     mHiddenNpcIcons.end();
        if (shown && !mReveal.exposed() && mReveal.tiles_x() > 3) {
            const Ogre::Vector3 p = icon.billboard->getPosition();
            shown = mReveal.revealed(mReveal.cell_x(p.x), mReveal.cell_z(p.z)) != 0.0f;
        }
        if (!shown) {
            icon.alpha = 0.0f;
        } else if (delta <= 0.0f) {
            // A still frame (a dump) snaps, as the tiles above do.
            icon.alpha = 1.0f;
        } else if (icon.alpha != 1.0f) {
            icon.alpha = std::min(1.0f, delta + delta + icon.alpha);
        }
        icon.billboard->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, icon.alpha));
    }
}

void Automap::hide() {
    mVisible = false;
    for (Ogre::RenderWindow* window : {mWindow, mSecond}) {
        if (window && window->hasViewportWithZOrder(kMapZOrder)) window->removeViewport(kMapZOrder);
    }
    mViewport = nullptr;
    if (mCullQuad) mCullQuad->setVisible(false);
    if (mRingQuad) mRingQuad->setVisible(false);
}

void Automap::show() {
    if (mMode == MapMode::Hidden) {
        mMode = MapMode::Minimap;
    }
    mVisible = true;
    if (!mWindow || !mCamera) return;

    // On one window only: the second screen's for the small map, when there is one.
    Ogre::RenderWindow* target = map_window();
    Ogre::RenderWindow* other = target == mWindow ? mSecond : mWindow;
    if (other && other->hasViewportWithZOrder(kMapZOrder)) other->removeViewport(kMapZOrder);
    if (!target->hasViewportWithZOrder(kMapZOrder)) {
        mViewport = target->addViewport(mCamera, kMapZOrder, 0.0f, 0.0f, 1.0f, 1.0f);
    } else {
        mViewport = target->getViewportByZOrder(kMapZOrder);
        mViewport->setCamera(mCamera);
    }
    mViewport->setClearEveryFrame(true, Ogre::FBT_DEPTH);
    mViewport->setBackgroundColour(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f));
    mViewport->setVisibilityMask(kVisibilityAutomap);
    mViewport->setOverlaysEnabled(false);
    mViewport->setMaterialScheme(Ogre::MSN_SHADERGEN);

    update_viewport_dimensions();
}

void Automap::set_mode(MapMode mode) {
    mMode = mode;
    if (mMode == MapMode::Hidden) {
        hide();
    } else if (mVisible) {
        show();  // the mode may put the map on the other window
    }
    apply_zoom();
}

// `CLevel::toggleAutomap`: shown small it
// goes full screen (`KSETTINGS_AUTOMAP` 2), full screen it goes away (0), and
// hidden it comes back small (1).
void Automap::cycle_mode() {
    switch (mMode) {
        case MapMode::Minimap: set_mode(MapMode::Fullscreen); break;
        case MapMode::Fullscreen: set_mode(MapMode::Hidden); break;
        case MapMode::Hidden:
            set_mode(MapMode::Minimap);
            show();
            break;
    }
}

void Automap::zoom(float delta) { set_zoom(mZoom + delta); }

void Automap::set_zoom(float zoom_val) {
    // `CAutomap::zoom` (Linux) clamps the running total to 20..60 and
    // persists it as KSETTINGS_F_AUTOMAP_ZOOM (default 30).
    mZoom = std::clamp(zoom_val, 20.0f, 60.0f);
    apply_zoom();
}

// The setting is not the window: `CAutomap::zoom` passes the extents
// (-zoom, +zoom, +zoom, -zoom), a window of `2 * zoom` on both axes.
void Automap::apply_zoom() {
    if (!mCamera) return;
    const float window = mZoom * kZoomToOrtho * (on_second_screen() ? kSecondScreenView : 1.0f);
    mCamera->setOrthoWindow(window, window);
}

void Automap::update_viewport_dimensions() {
    if (!mViewport || !mWindow || !mCamera || !mVisible) return;

    const float win_w = static_cast<float>(mWindow->getWidth());
    const float win_h = static_cast<float>(mWindow->getHeight());
    if (win_w <= 0.0f || win_h <= 0.0f) return;

    if (mMode == MapMode::Hidden) {
        hide();
        return;
    }

    mViewport->setVisibilityMask(kVisibilityAutomap);
    mViewport->setOverlaysEnabled(false);
    mViewport->setMaterialScheme(Ogre::MSN_SHADERGEN);

    if (mMode == MapMode::Fullscreen) {
        // Fullscreen automap overlay
        mViewport->setDimensions(0.0f, 0.0f, 1.0f, 1.0f);
        mCamera->setAspectRatio(win_w / win_h);
        if (mCullQuad) mCullQuad->setVisible(false);
        if (mRingQuad) mRingQuad->setVisible(false);
        place_circle(false);
    } else if (on_second_screen()) {
        // PORT: the second screen's own: the circle in the middle of the room it has there.
        float left = 0.0f, top = 0.0f, side = 0.0f;
        if (!second_screen_square(left, top, side)) return;
        const float second_w = static_cast<float>(mSecond->getWidth());
        const float second_h = static_cast<float>(mSecond->getHeight());
        mViewport->setDimensions(left / second_w, top / second_h, side / second_w,
                                 side / second_h);
        mCamera->setAspectRatio(1.0f);

        if (mCullQuad) mCullQuad->setVisible(true);
        if (mRingQuad) mRingQuad->setVisible(true);
        place_circle(true);
    } else {
        // Minimap mode: a bottom-right circle.
        const float aspect = win_w / win_h;
        const float h = 0.240f;
        const float w = h / aspect;              // 1:1 circular aspect ratio
        const float top = 0.7896f - (h * 0.5f);  // 0.6696f
        const float left = 0.8973f - (w * 0.5f);

        mViewport->setDimensions(std::max(0.0f, left), std::max(0.0f, top), std::min(1.0f, w),
                                 std::min(1.0f, h));
        mCamera->setAspectRatio(1.0f);

        if (mCullQuad) mCullQuad->setVisible(true);
        if (mRingQuad) mRingQuad->setVisible(true);
        place_circle(false);
    }
}

}  // namespace tl::render
