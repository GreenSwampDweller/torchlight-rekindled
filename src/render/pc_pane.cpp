#include "render/pc_pane.h"

#include "core/log.h"
#include "formats/ogre_mesh.h"
#include "render/menu_chrome.h"

#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreResourceGroupManager.h>
#include <OgreShaderGenerator.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <map>
#include <sstream>

namespace tl::render {

namespace {

/// The screen the models' pixels are measured on.
constexpr float kPaneScreenWidth = 1024.0f;
constexpr float kPaneScreenHeight = 768.0f;

/// Each material's first texture, from a `.material` script.
std::map<std::string, std::string> material_textures(const std::vector<uint8_t>& script) {
    std::map<std::string, std::string> out;
    std::istringstream lines(std::string(script.begin(), script.end()));
    std::string line;
    std::string current;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string key;
        std::string value;
        words >> key >> value;
        if (key == "material") current = value;
        if (key == "texture" && !current.empty() && out.count(current) == 0) out[current] = value;
    }
    return out;
}

/// A texture a material names, in the model's own folder: as named, or as the `.dds` the
/// archive ships in its place.
Ogre::TexturePtr pane_texture(const std::string& group, const std::string& dir,
                              const std::string& name) {
    auto& groups = Ogre::ResourceGroupManager::getSingleton();
    std::string path = dir + name;
    if (!groups.resourceExists(group, path)) {
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            path.resize(dot);
            path += ".dds";
        }
    }
    if (!groups.resourceExists(group, path)) return {};
    return Ogre::TextureManager::getSingleton().load(path, group);
}

}  // namespace

PcPane::~PcPane() { destroy(); }

/// A quad's box in the model's pixels, and the texture's corner at each of its edges.
void PcPane::box_of(const tl::formats::OgreMeshVertices& v, Quad& quad) {
    quad.x0 = quad.x1 = v.positions[0];
    quad.y0 = quad.y1 = v.positions[1];
    quad.depth = v.positions[2];
    for (uint32_t i = 0; i < v.count; ++i) {
        quad.x0 = std::min(quad.x0, v.positions[i * 3]);
        quad.x1 = std::max(quad.x1, v.positions[i * 3]);
        quad.y0 = std::max(quad.y0, v.positions[i * 3 + 1]);
        quad.y1 = std::min(quad.y1, v.positions[i * 3 + 1]);
    }
    for (uint32_t i = 0; i < v.count; ++i) {
        if (v.positions[i * 3] == quad.x0) quad.u0 = v.texcoords[i * 2];
        if (v.positions[i * 3] == quad.x1) quad.u1 = v.texcoords[i * 2];
        if (v.positions[i * 3 + 1] == quad.y0) quad.v0 = v.texcoords[i * 2 + 1];
        if (v.positions[i * 3 + 1] == quad.y1) quad.v1 = v.texcoords[i * 2 + 1];
    }
}

bool PcPane::create(const std::string& group, const std::string& model, bool right, unsigned width,
                    unsigned height, const std::string& tag, unsigned short z_order) {
    destroy();
    const std::string dir = "media/UI/models/" + model + "/";
    std::vector<uint8_t> bytes;
    if (!Ogre::ResourceGroupManager::getSingleton().resourceExists(group, dir + model + ".MESH") ||
        !read_menu_resource(dir + model + ".MESH", group, bytes) || bytes.empty()) {
        return false;
    }
    const tl::formats::OgreMesh mesh = tl::formats::parse_ogre_mesh(bytes.data(), bytes.size());
    if (!mesh.ok) {
        TL_LOG_WARN("%s: %s%s.MESH: %s", tag.c_str(), dir.c_str(), model.c_str(),
                    mesh.error.c_str());
        return false;
    }
    std::vector<uint8_t> script;
    read_menu_resource(dir + model + ".material", group, script);
    const std::map<std::string, std::string> textures = material_textures(script);

    for (const tl::formats::OgreMeshSubmesh& sub : mesh.submeshes) {
        const tl::formats::OgreMeshVertices& v =
            sub.uses_shared_vertices ? mesh.shared : sub.vertices;
        const auto found = textures.find(sub.material);
        if (v.count < 3 || v.texcoords.size() < v.count * 2 || found == textures.end()) continue;
        Quad quad;
        quad.texture = pane_texture(group, dir, found->second);
        if (!quad.texture) continue;
        box_of(v, quad);
        quads_.push_back(quad);
    }
    if (quads_.empty()) return false;
    // The further half first: the top one stands in front of the bottom one.
    std::stable_sort(quads_.begin(), quads_.end(),
                     [](const Quad& a, const Quad& b) { return a.depth < b.depth; });

    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_ = overlays.create(tag + "/pane");
    overlay_->setZOrder(z_order);
    root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", tag + "/paneroot"));
    root_->setMetricsMode(Ogre::GMM_RELATIVE);
    root_->setPosition(0.0f, 0.0f);
    root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(root_)->setTransparent(true);
    overlay_->add2D(root_);
    Ogre::ushort z = static_cast<Ogre::ushort>(root_->getZOrder() + 1);
    for (size_t i = 0; i < quads_.size(); ++i) {
        Quad& quad = quads_[i];
        const std::string name = tag + "/pane" + std::to_string(i);
        quad.material = build_menu_material(name, quad.texture);
        quad.panel =
            static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement("Panel", name));
        quad.panel->setMetricsMode(Ogre::GMM_RELATIVE);
        quad.panel->setMaterial(quad.material);
        quad.panel->setUV(quad.u0, quad.v0, quad.u1, quad.v1);
        root_->addChild(quad.panel);
        quad.panel->_notifyZOrder(z++);
    }
    right_ = right;
    width_ = width;
    height_ = height;
    layout();
    overlay_->hide();
    TL_LOG_INFO("%s: the %s pane, %zu quad(s) from %s%s.MESH", tag.c_str(), model.c_str(),
                quads_.size(), dir.c_str(), model.c_str());
    return true;
}

void PcPane::layout() {
    if (width_ == 0 || height_ == 0) return;
    // The model's pixels by the screen's height; its outer edge on the screen's.
    const float scale = static_cast<float>(height_) / kPaneScreenHeight;
    const float wide = static_cast<float>(width_);
    const float tall = static_cast<float>(height_);
    for (Quad& quad : quads_) {
        if (quad.panel == nullptr) continue;
        const float left = right_ ? wide - (kPaneScreenWidth * 0.5f - quad.x0) * scale
                                  : (quad.x0 + kPaneScreenWidth * 0.5f) * scale;
        const float top = (kPaneScreenHeight * 0.5f - quad.y0) * scale;
        quad.panel->setPosition(left / wide, top / tall);
        quad.panel->setDimensions((quad.x1 - quad.x0) * scale / wide,
                                  (quad.y0 - quad.y1) * scale / tall);
    }
}

void PcPane::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    layout();
}

void PcPane::show() {
    if (overlay_ != nullptr) overlay_->show();
}

void PcPane::hide() {
    if (overlay_ != nullptr) overlay_->hide();
}

void PcPane::destroy() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr && overlay_ != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Quad& quad : quads_) {
            if (quad.panel == nullptr) continue;
            root_->removeChild(quad.panel->getName());
            overlays.destroyOverlayElement(quad.panel);
        }
        overlay_->remove2D(root_);
        overlays.destroyOverlayElement(root_);
        overlays.destroy(overlay_);
    }
    if (Ogre::MaterialManager::getSingletonPtr() != nullptr) {
        for (Quad& quad : quads_) {
            if (!quad.material) continue;
            // The shader system's techniques first, or they outlive the material.
            if (auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
                shaders->removeAllShaderBasedTechniques(*quad.material);
            }
            Ogre::MaterialManager::getSingleton().remove(quad.material);
        }
    }
    quads_.clear();
    overlay_ = nullptr;
    root_ = nullptr;
}

}  // namespace tl::render
