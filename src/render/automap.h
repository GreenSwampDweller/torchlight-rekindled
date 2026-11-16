// In-game minimap and automap. The map is the Xbox 360 build's: `CAutomap::create`
// draws the level's walkable cells (`CLevel::createAutomapTexture`)
// on a grid mesh whose vertex colours fade in as the player uncovers it (`core/automap_grid.h`).
// The icons, camera and circle are read from the Linux release's `CAutomap` or are the port's.
//
// Draw order (the console's queue in brackets): AutomapCullQuad, material AutomapCull [100], a
// circular depth mask (alpha reject >= 150, depth write only: near depth outside the circle);
// the map mesh, AutomapRender [101], `src * src_colour + dst * (1 - src_colour)`; the icon
// BillboardSet 'bb' [102], depth-tested against the mask; AutomapOverlayQuad [103], the ring.
#pragma once

#include <OgreBillboard.h>
#include <OgreBillboardSet.h>
#include <OgreCamera.h>
#include <OgreColourValue.h>
#include <OgreEntity.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMaterial.h>
#include <OgreMath.h>
#include <OgreMesh.h>
#include <OgreQuaternion.h>
#include <OgreRectangle2D.h>
#include <OgreRenderQueueListener.h>
#include <OgreRenderTargetListener.h>
#include <OgreRenderWindow.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTexture.h>
#include <OgreVector.h>
#include <OgreViewport.h>

#include "core/automap_grid.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {
class PassabilityMap;
}

namespace tl::render {

constexpr uint32_t kVisibilityWorld = 0x00000001;
constexpr uint32_t kVisibilityAutomap = 0x00000002;

/// Render queue groups for the automap's own quads and billboards, in draw order: clear of
/// Ogre::RENDER_QUEUE_SKIES_LATE (90) and Ogre::RENDER_QUEUE_OVERLAY (100), reserved for overlays.
constexpr uint8_t kQueueAutomapCull = 83;   ///< circular depth mask
constexpr uint8_t kQueueAutomapMap = 84;    ///< the level's map mesh
constexpr uint8_t kQueueAutomapTiles = 85;  ///< map tiles and icons
constexpr uint8_t kQueueAutomapRing = 86;   ///< circular border ring

/// The automap camera's yaw: `CAutomap::CAutomap` turns `AutomapCam` -90 degrees
/// about X, then -225 about Y, matching the gameplay camera's heading
/// (`CCameraControl::updateGameCamera`).
constexpr float kAutomapCameraYawDeg = -225.0f;

enum class MapMode {
    Minimap,     ///< Bottom-right circular minimap matching Xbox HUD layout
    Fullscreen,  ///< Centered overlay showing the larger level view
    Hidden       ///< Automap turned off
};

class Automap {
public:
    Automap();
    ~Automap();

    Automap(const Automap&) = delete;
    Automap& operator=(const Automap&) = delete;

    /// Initialise OGRE materials, billboard sets, quads, camera and viewport.
    bool init(Ogre::SceneManager* scene_mgr, Ogre::RenderWindow* window, float y_ratio);

    /// Shut down and detach all resources from OGRE.
    void shutdown();

    /// Clear the map, its tiles, icons, and fog of war (on level change or restart).
    void clear();

    /// Register a map tile (an exit, a warper) or an icon. `tile_id` is 1-based; `is_icon` for a
    /// dynamic entity icon; `visible` if it starts revealed.
    Ogre::Billboard* add_tile(int tile_id, const Ogre::Vector3& pos, const Ogre::Vector3& dir,
                              bool is_icon = false, bool visible = false);

    /// `CAutomap::create`: the level's map, from its baked
    /// passability grid; then `CAutomap::finalize`, which
    /// sizes the reveal and uncovers an `exposed` level (a town) whole.
    void create(const tl::core::PassabilityMap& map, bool exposed);

    /// Update player position and orientation, revealing fog of war around the player.
    void set_player_position(const Ogre::Vector3& pos, const Ogre::Quaternion& orientation);

    /// Update pet position on the map.
    void set_pet_position(const Ogre::Vector3& pos);

    /// Show or hide the pet icon.
    void set_pet_visible(bool visible);

    /// `CAutomap::clearNPCIcons`: every icon
    /// billboard, and the hidden list.
    void clear_npc_icons();

    /// `CAutomap::setNPCBillboardVisible`: a hidden icon is drawn
    /// at alpha 0 until shown again.
    void set_npc_icon_visible(Ogre::Billboard* icon, bool visible);

    /// Advance map state each frame (camera tracking, fog of war alpha fades).
    void update(const Ogre::Vector3& player_pos, float delta);

    /// Show or hide the automap during gameplay.
    void show();
    void hide();

    /// Set map mode: Minimap, Fullscreen, or Hidden.
    void set_mode(MapMode mode);

    /// Cycle to next map mode (Minimap -> Fullscreen -> Hidden -> Minimap).
    void cycle_mode();

    /// Zoom controls (clamps between 20.0f and 60.0f, default 30.0f).
    void zoom(float delta);
    void zoom_in() { zoom(-5.0f); }
    void zoom_out() { zoom(5.0f); }
    void set_zoom(float zoom_val);
    float current_zoom() const { return mZoom; }

    /// The fog of war as `CLevelState::addAutomap` copies it.
    void revealed_grid(int32_t& width, int32_t& height, std::vector<float>& cells) const;

    /// The billboards in the icon set (a census for the log).
    size_t billboard_count() const;

    /// `CLevelState::restoreAutomap`: only when the size matches.
    void restore_revealed(int32_t width, int32_t height, const std::vector<float>& cells);

    /// The map's camera, for a frame dump that draws the map onto its own
    /// render target.
    Ogre::Camera* camera() const { return mCamera; }

    /// Draw the map onto `target` without OGRE searching the level for it: for a viewport of the
    /// map's camera the scene search is switched off and the map's five objects are queued
    /// directly (the search cost several ms a frame on the console). init() attaches the window.
    void attach(Ogre::RenderTarget* target);
    void detach(Ogre::RenderTarget* target);

    /// PORT: a second display's window (a two-screen handheld), or null for none. While there is
    /// one the small map is drawn on it, as large as fits, in place of the main window's corner
    /// circle; the full screen map stays on the main window. The window must outlive the map or
    /// be taken away again (null) first.
    void set_second_screen(Ogre::RenderWindow* second);
    bool on_second_screen() const { return mSecond != nullptr && mMode == MapMode::Minimap; }

    MapMode mode() const { return mMode; }
    bool visible() const { return mVisible; }
    bool is_visible() const { return mVisible && mMode != MapMode::Hidden; }

private:
    /// `CAutomap +0x8c` (the tiles) and `+0x9c` (their opacities).
    struct TileBillboard {
        Ogre::Billboard* billboard = nullptr;
        float opacity = 0.0f;
    };

    /// attach()'s listener: the search off for the map's viewports, and the
    /// map's objects queued in its place.
    struct OwnQueue final : Ogre::RenderTargetListener, Ogre::RenderQueueListener {
        Automap* owner = nullptr;
        bool active = false;  ///< the viewport being drawn is the map's
        void preViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) override;
        void postViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) override;
        void preRenderQueues() override;
    };
    OwnQueue mOwnQueue;

    void ensure_materials();
    /// The window the map's viewport is on in this mode.
    Ogre::RenderWindow* map_window() const { return on_second_screen() ? mSecond : mWindow; }
    /// The camera's window from the zoom, wider on a second screen.
    void apply_zoom();
    void update_viewport_dimensions();
    /// The map's mesh, texture and entity let go.
    void destroy_map();
    /// `CAutomap::update`: every vertex's colour rewritten
    /// from its opacity, grey with alpha 1.
    void write_map_colours();
    /// `CAutomap::update`: the vertices' and the tiles' fades.
    void fade_map(float delta);
    void fade_tile(TileBillboard& tile, float delta);

    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::RenderWindow* mWindow = nullptr;
    Ogre::Camera* mCamera = nullptr;
    Ogre::SceneNode* mCameraNode = nullptr;
    Ogre::Viewport* mViewport = nullptr;
    Ogre::RenderWindow* mSecond = nullptr;  ///< set_second_screen()
    Ogre::SceneNode* mNodeTiles = nullptr;
    Ogre::SceneNode* mNodeCull = nullptr;
    Ogre::SceneNode* mNodeRing = nullptr;

    Ogre::BillboardSet* mBillboardSet = nullptr;  // 'bb', kQueueAutomapTiles
    Ogre::Rectangle2D* mCullQuad = nullptr;       // 'AutomapCull', kQueueAutomapCull
    Ogre::Rectangle2D* mRingQuad = nullptr;       // 'AutomapOverlay', kQueueAutomapRing

    /// The map: `CAutomap +0x2c` (the base texture), `+0x24` (the mesh),
    /// `+0x20` (its entity), `+0x28` (its scene node), and the mesh's second
    /// vertex buffer, its colours.
    Ogre::TexturePtr mMapTexture;
    Ogre::MeshPtr mMapMesh;
    Ogre::Entity* mMapEntity = nullptr;
    Ogre::SceneNode* mMapNode = nullptr;
    Ogre::HardwareVertexBufferSharedPtr mMapColours;
    /// The reveal, one value per vertex of the mesh.
    tl::core::AutomapReveal mReveal;

    Ogre::Billboard* mPlayerIcon = nullptr;
    Ogre::Billboard* mPetIcon = nullptr;
    /// `CAutomap +0xd8` (the icon billboards) and `+0xf0` (their alphas).
    struct NpcIcon {
        Ogre::Billboard* billboard = nullptr;
        float alpha = 0.0f;
    };
    std::vector<NpcIcon> mNpcIcons;
    /// `CAutomap +0x108`: icons whose unit's mesh is hidden.
    std::vector<Ogre::Billboard*> mHiddenNpcIcons;

    std::vector<TileBillboard> mTileBillboards;

    float mZoom = 30.0f;  ///< `+0x4c`, the zoom level
    float mYRatio = 1.0f;
    MapMode mMode = MapMode::Minimap;
    bool mVisible = false;
    bool mPetVisible = false;
    bool mMaterialsCreated = false;
};

}  // namespace tl::render
