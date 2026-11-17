// A record of one frame, draw by draw, for comparing the port's rendering with the Xbox build's:
// one block per renderable with the target, material and pass, programs and constants by name,
// textures with their sampling, and the pass's fixed state. Nothing here changes what is drawn.
#pragma once

#include <OgreMatrix4.h>
#include <OgreRenderObjectListener.h>
#include <OgreRenderTargetListener.h>

#include <cstdio>
#include <set>
#include <string>

namespace Ogre {
class SceneManager;
class RenderTarget;
class VertexData;
class IndexData;
}  // namespace Ogre

namespace tl::render {

class DrawDump final : public Ogre::RenderObjectListener, public Ogre::RenderTargetListener {
public:
    /// Records into `dir`/frame.txt (and `dir`/glsl/<program>.glsl) from the
    /// moment it is attached until it is destroyed.
    DrawDump(Ogre::SceneManager* scene, const std::string& dir);
    ~DrawDump() override;
    DrawDump(const DrawDump&) = delete;
    DrawDump& operator=(const DrawDump&) = delete;

    /// Follow a render target: its updates are marked in the record, in order.
    void watch(Ogre::RenderTarget* target);

    bool ok() const { return file_ != nullptr; }
    int draws() const { return draw_; }

    void notifyRenderSingleObject(Ogre::Renderable* rend, const Ogre::Pass* pass,
                                  const Ogre::AutoParamDataSource* source,
                                  const Ogre::LightList* lights, bool suppress) override;
    void preRenderTargetUpdate(const Ogre::RenderTargetEvent& event) override;
    void postRenderTargetUpdate(const Ogre::RenderTargetEvent& event) override;
    void preViewportUpdate(const Ogre::RenderTargetViewportEvent& event) override;

private:
    void write_buffers(std::FILE* f, const Ogre::Affine3& world, const Ogre::VertexData* vd,
                       const Ogre::IndexData* id);

    Ogre::SceneManager* scene_;
    std::string dir_;
    std::FILE* file_ = nullptr;
    int draw_ = 0;
    bool buffers_ = false;
    std::set<std::string> programs_written_;
    std::set<std::string> textures_written_;
    std::set<Ogre::RenderTarget*> watched_;
};

}  // namespace tl::render
