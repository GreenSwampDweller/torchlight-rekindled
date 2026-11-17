// See draw_dump.h.
#include "draw_dump.h"

#include "core/log.h"

#include <OgreBillboard.h>
#include <OgreBillboardSet.h>
#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreGpuProgram.h>
#include <OgreGpuProgramParams.h>
#include <OgreHardwareBuffer.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreImage.h>
#include <OgreLight.h>
#include <OgreMaterial.h>
#include <OgreMesh.h>
#include <OgreMovableObject.h>
#include <OgrePass.h>
#include <OgreRenderTarget.h>
#include <OgreRenderable.h>
#include <OgreSceneManager.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTexture.h>
#include <OgreTextureUnitState.h>
#include <OgreViewport.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace tl::render {

namespace {

std::string spaced(std::string s) {
    for (char& c : s) {
        if (c == ' ' || c == '\t' || c == '\n') c = '_';
    }
    return s.empty() ? "?" : s;
}

/// What the renderable is, as far as a reader comparing frames cares: the
/// mesh of an entity's sub-entity, otherwise the movable's type and name.
std::string describe(Ogre::Renderable* rend) {
    if (auto* sub = dynamic_cast<Ogre::SubEntity*>(rend)) {
        const Ogre::MeshPtr& mesh = sub->getParent()->getMesh();
        return "entity " + spaced(mesh ? mesh->getName() : std::string()) + " " +
               spaced(sub->getParent()->getName());
    }
    if (auto* movable = dynamic_cast<Ogre::MovableObject*>(rend)) {
        return spaced(movable->getMovableType()) + " " + spaced(movable->getName()) + " -";
    }
    return "renderable ? -";
}

unsigned queue_group(Ogre::Renderable* rend) {
    if (auto* sub = dynamic_cast<Ogre::SubEntity*>(rend))
        return sub->getParent()->getRenderQueueGroup();
    if (auto* movable = dynamic_cast<Ogre::MovableObject*>(rend))
        return movable->getRenderQueueGroup();
    return 0xffu;
}

void write_constants(std::FILE* f, const char* kind,
                     const Ogre::GpuProgramParametersSharedPtr& params) {
    if (!params || !params->hasNamedParameters()) return;
    const Ogre::GpuNamedConstants& named = params->getConstantDefinitions();
    for (const auto& [name, def] : named.map) {
        if (!name.empty() && name.back() == ']') continue;
        if (!def.isFloat()) continue;
        const size_t count = static_cast<size_t>(def.elementSize) * def.arraySize;
        const float* values = params->getFloatPointer(def.physicalIndex);
        std::fprintf(f, "const %s %s %zu", kind, name.c_str(), count);
        for (size_t k = 0; k < count; ++k) std::fprintf(f, " %.6g", static_cast<double>(values[k]));
        std::fprintf(f, "\n");
    }
}

void write_colour(std::FILE* f, const char* label, const Ogre::ColourValue& c) {
    std::fprintf(f, " %s %.4g %.4g %.4g %.4g", label, static_cast<double>(c.r),
                 static_cast<double>(c.g), static_cast<double>(c.b), static_cast<double>(c.a));
}

}  // namespace

DrawDump::DrawDump(Ogre::SceneManager* scene, const std::string& dir) : scene_(scene), dir_(dir) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir) / "glsl", ec);
    std::filesystem::create_directories(std::filesystem::path(dir) / "tex", ec);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, on the render thread
    buffers_ = std::getenv("TL_DRAW_DUMP_BUFFERS") != nullptr;
    if (buffers_) std::filesystem::create_directories(std::filesystem::path(dir) / "buf", ec);
    file_ = std::fopen((std::filesystem::path(dir) / "frame.txt").string().c_str(), "w");
    if (!file_) {
        TL_LOG_ERROR("draw dump: cannot write '%s/frame.txt'", dir.c_str());
        return;
    }
    std::fprintf(
        file_,
        "# torchlight port draw dump 1: one block per renderable drawn, in order.\n"
        "# Enumerations are OGRE's (the 1.7 and 14 values agree for blend factors, compare "
        "functions,\n"
        "# culling, vertex element types and semantics), so they read as the Xbox capture's do.\n");
    const Ogre::ColourValue ambient = scene->getAmbientLight();
    std::fprintf(file_, "scene");
    write_colour(file_, "ambient", ambient);
    std::fprintf(file_, " fog %d", static_cast<int>(scene->getFogMode()));
    write_colour(file_, "colour", scene->getFogColour());
    std::fprintf(file_, " start %.6g end %.6g density %.6g\n",
                 static_cast<double>(scene->getFogStart()), static_cast<double>(scene->getFogEnd()),
                 static_cast<double>(scene->getFogDensity()));
    scene_->addRenderObjectListener(this);
}

DrawDump::~DrawDump() {
    scene_->removeRenderObjectListener(this);
    for (Ogre::RenderTarget* target : watched_) target->removeListener(this);
    if (file_) {
        std::fprintf(file_, "end %d\n", draw_);
        std::fclose(file_);
    }
}

// PORT: `TL_DRAW_DUMP_BUFFERS`, the draw's world matrix and the vertex and index ranges it
// reads, as raw bytes in `buf/`, so a frame can be rebuilt elsewhere.
void DrawDump::write_buffers(std::FILE* f, const Ogre::Affine3& world, const Ogre::VertexData* vd,
                             const Ogre::IndexData* id) {
    if (!buffers_) return;
    std::fprintf(f, "world3x4");
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) std::fprintf(f, " %.7g", static_cast<double>(world[r][c]));
    }
    std::fprintf(f, "\n");
    const auto save = [&](const std::string& name, Ogre::HardwareBuffer& buffer, size_t offset,
                          size_t bytes) {
        try {
            const void* data = buffer.lock(offset, bytes, Ogre::HardwareBuffer::HBL_READ_ONLY);
            if (std::FILE* out = std::fopen(
                    (std::filesystem::path(dir_) / "buf" / name).string().c_str(), "wb")) {
                std::fwrite(data, 1, bytes, out);
                std::fclose(out);
            }
            buffer.unlock();
            return true;
        } catch (const Ogre::Exception&) {
            return false;
        }
    };
    if (vd && vd->vertexBufferBinding && vd->vertexCount > 0) {
        for (const auto& [source, buffer] : vd->vertexBufferBinding->getBindings()) {
            const size_t stride = buffer->getVertexSize();
            const std::string name =
                std::to_string(draw_ - 1) + "_vb" + std::to_string(source) + ".bin";
            if (save(name, *buffer, vd->vertexStart * stride, vd->vertexCount * stride)) {
                std::fprintf(f, "vb %u buf/%s %zu %zu\n", source, name.c_str(), stride,
                             static_cast<size_t>(vd->vertexCount));
            }
        }
    }
    if (id && id->indexBuffer && id->indexCount > 0) {
        const size_t size = id->indexBuffer->getIndexSize();
        const std::string name = std::to_string(draw_ - 1) + "_ib.bin";
        if (save(name, *id->indexBuffer, id->indexStart * size, id->indexCount * size)) {
            std::fprintf(f, "ib buf/%s %zu %zu\n", name.c_str(), size * 8,
                         static_cast<size_t>(id->indexCount));
        }
    }
}

void DrawDump::watch(Ogre::RenderTarget* target) {
    if (target && watched_.insert(target).second) target->addListener(this);
}

// PORT: `--draw-dump`, the frame's draw calls written out for comparison with the Xbox capture.
void DrawDump::preRenderTargetUpdate(const Ogre::RenderTargetEvent& event) {
    if (!file_) return;
    Ogre::RenderTarget* t = event.source;
    std::fprintf(file_, "target %s %ux%u\n", spaced(t->getName()).c_str(), t->getWidth(),
                 t->getHeight());
}

// PORT: `--draw-dump`, the frame's draw calls written out for comparison with the Xbox capture.
void DrawDump::postRenderTargetUpdate(const Ogre::RenderTargetEvent&) {
    if (file_) std::fprintf(file_, "end_target\n");
}

// PORT: `--draw-dump`, the frame's draw calls written out for comparison with the Xbox capture.
void DrawDump::preViewportUpdate(const Ogre::RenderTargetViewportEvent& event) {
    if (!file_) return;
    Ogre::Viewport* vp = event.source;
    std::fprintf(file_, "viewport %d %d %d %d camera %s clear %u", vp->getActualLeft(),
                 vp->getActualTop(), vp->getActualWidth(), vp->getActualHeight(),
                 spaced(vp->getCamera() ? vp->getCamera()->getName() : std::string()).c_str(),
                 vp->getClearEveryFrame() ? vp->getClearBuffers() : 0u);
    write_colour(file_, "colour", vp->getBackgroundColour());
    std::fprintf(file_, " mask %08x scheme %s\n", vp->getVisibilityMask(),
                 spaced(vp->getMaterialScheme()).c_str());
    // The camera, to be compared with the Xbox capture's `view_matrix`.
    if (const Ogre::Camera* camera = vp->getCamera()) {
        const Ogre::Vector3 p = camera->getDerivedPosition();
        const Ogre::Vector3 d = camera->getDerivedDirection();
        std::fprintf(file_, "camera_pose %s pos %.4f %.4f %.4f dir %.5f %.5f %.5f fovy %.4f\n",
                     spaced(camera->getName()).c_str(), p.x, p.y, p.z, d.x, d.y, d.z,
                     camera->getFOVy().valueDegrees());
    }
}

void DrawDump::notifyRenderSingleObject(Ogre::Renderable* rend, const Ogre::Pass* pass,
                                        const Ogre::AutoParamDataSource* source,
                                        const Ogre::LightList* lights, bool suppress) {
    if (!file_ || suppress) return;
    std::FILE* f = file_;
    std::fprintf(f, "draw %d\n", draw_++);

    Ogre::Technique* tech = pass->getParent();
    Ogre::Material* material = tech ? tech->getParent() : nullptr;
    std::fprintf(f, "rend %s\n", describe(rend).c_str());
    std::fprintf(f, "material %s scheme %s pass %u\n",
                 spaced(material ? material->getName() : "").c_str(),
                 spaced(tech ? tech->getSchemeName() : "").c_str(), pass->getIndex());
    const Ogre::Affine3& world = source->getWorldMatrix();
    std::fprintf(f, "world %.4g %.4g %.4g\n", static_cast<double>(world[0][3]),
                 static_cast<double>(world[1][3]), static_cast<double>(world[2][3]));

    Ogre::RenderOperation op;
    rend->getRenderOperation(op);
    const Ogre::VertexData* vd = op.vertexData;
    const Ogre::IndexData* id = op.useIndexes ? op.indexData : nullptr;
    std::fprintf(f, "op %d %d %zu %zu %zu %zu\n", static_cast<int>(op.operationType), id ? 1 : 0,
                 static_cast<size_t>(vd ? vd->vertexStart : 0),
                 static_cast<size_t>(vd ? vd->vertexCount : 0),
                 static_cast<size_t>(id ? id->indexStart : 0),
                 static_cast<size_t>(id ? id->indexCount : 0));
    if (vd && vd->vertexDeclaration) {
        for (const Ogre::VertexElement& e : vd->vertexDeclaration->getElements()) {
            std::fprintf(f, "elem %u %zu %d %d %u\n", e.getSource(), e.getOffset(),
                         static_cast<int>(e.getType()), static_cast<int>(e.getSemantic()),
                         e.getIndex());
        }
    }
    write_buffers(f, world, vd, id);

    // A billboard set's billboards (the particles): each quad's centre, size and colour, read back
    // from the vertex buffer the set filled (ParticleUniverse injects them for the draw only).
    if (dynamic_cast<const Ogre::BillboardSet*>(rend) != nullptr && op.vertexData != nullptr &&
        op.vertexData->vertexDeclaration != nullptr && op.vertexData->vertexCount <= 4096) {
        const Ogre::VertexElement* pos =
            op.vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        const Ogre::VertexElement* col =
            op.vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
        if (pos != nullptr && pos->getType() == Ogre::VET_FLOAT3 &&
            op.vertexData->vertexBufferBinding->isBufferBound(pos->getSource())) {
            const Ogre::HardwareVertexBufferSharedPtr& buffer =
                op.vertexData->vertexBufferBinding->getBuffer(pos->getSource());
            try {
                const size_t stride = buffer->getVertexSize();
                const auto* base = static_cast<const unsigned char*>(buffer->lock(
                    op.vertexData->vertexStart * stride, op.vertexData->vertexCount * stride,
                    Ogre::HardwareBuffer::HBL_READ_ONLY));
                const size_t quads = std::min<size_t>(op.vertexData->vertexCount / 4, 64);
                for (size_t q = 0; q < quads; ++q) {
                    float c[3] = {0.0f, 0.0f, 0.0f};
                    float corner[4][3];
                    for (int k = 0; k < 4; ++k) {
                        std::memcpy(corner[k], base + (q * 4 + k) * stride + pos->getOffset(),
                                    sizeof(corner[k]));
                        for (int a = 0; a < 3; ++a) c[a] += corner[k][a] * 0.25f;
                    }
                    float size = 0.0f;
                    for (int i = 0; i < 4; ++i) {
                        for (int j = 0; j < 4; ++j) {
                            const float dx = corner[i][0] - corner[j][0],
                                        dy = corner[i][1] - corner[j][1],
                                        dz = corner[i][2] - corner[j][2];
                            size = std::max(size, std::sqrt(dx * dx + dy * dy + dz * dz));
                        }
                    }
                    std::uint32_t argb = 0;
                    if (col != nullptr && col->getSource() == pos->getSource()) {
                        std::memcpy(&argb, base + q * 4 * stride + col->getOffset(), 4);
                    }
                    std::fprintf(f, "bb %.3f %.3f %.3f size %.3f colour %08x\n", c[0], c[1], c[2],
                                 size / 1.41421356f, argb);
                }
                buffer->unlock();
            } catch (const Ogre::Exception&) {}
        }
    }

    // The programs, written out once by name.
    std::string names[2];
    for (int k = 0; k < 2; ++k) {
        const Ogre::GpuProgramType type =
            k == 0 ? Ogre::GPT_VERTEX_PROGRAM : Ogre::GPT_FRAGMENT_PROGRAM;
        if (!pass->hasGpuProgram(type)) {
            names[k] = "-";
            continue;
        }
        const Ogre::GpuProgramPtr& program = pass->getGpuProgram(type);
        names[k] = spaced(program->getName());
        if (programs_written_.insert(names[k]).second) {
            std::string file = names[k];
            for (char& c : file) {
                if (c == '/' || c == '\\' || c == ':') c = '_';
            }
            if (std::FILE* out = std::fopen(
                    (std::filesystem::path(dir_) / "glsl" / (file + ".glsl")).string().c_str(),
                    "w")) {
                const std::string& text = program->getSource();
                std::fwrite(text.data(), 1, text.size(), out);
                std::fclose(out);
            }
        }
    }
    std::fprintf(f, "prog %s %s\n", names[0].c_str(), names[1].c_str());

    for (size_t u = 0; u < pass->getNumTextureUnitStates(); ++u) {
        const Ogre::TextureUnitState* tus = pass->getTextureUnitState(u);
        const Ogre::TexturePtr& tex = tus->_getTexturePtr();
        // A texture the port makes itself (`tl/...`) is not in the archive, so its contents are
        // written once. Render targets are left out.
        if (tex && tex->getName().rfind("tl/", 0) == 0 &&
            !(tex->getUsage() & Ogre::TU_RENDERTARGET) &&
            textures_written_.insert(tex->getName()).second) {
            std::string file = tex->getName();
            for (char& c : file) {
                if (c == '/' || c == '\\' || c == ':') c = '_';
            }
            try {
                Ogre::Image image;
                tex->convertToImage(image);
                image.save((std::filesystem::path(dir_) / "tex" / (file + ".png")).string());
            } catch (const Ogre::Exception& e) {
                TL_LOG_WARN("draw dump: cannot save '%s': %s", tex->getName().c_str(),
                            e.getDescription().c_str());
            }
        }
        const Ogre::Sampler::UVWAddressingMode& address = tus->getTextureAddressingMode();
        std::fprintf(f, "tex %zu %s %ux%u filter %d %d %d address %d %d %d set %u\n", u,
                     spaced(tex ? tex->getName() : tus->getTextureName()).c_str(),
                     tex ? tex->getWidth() : 0u, tex ? tex->getHeight() : 0u,
                     static_cast<int>(tus->getTextureFiltering(Ogre::FT_MIN)),
                     static_cast<int>(tus->getTextureFiltering(Ogre::FT_MAG)),
                     static_cast<int>(tus->getTextureFiltering(Ogre::FT_MIP)),
                     static_cast<int>(address.u), static_cast<int>(address.v),
                     static_cast<int>(address.w), tus->getTextureCoordSet());
    }

    std::fprintf(
        f,
        "state blend %d %d %d %d %d %d %d cull %d depth %d %d %d mask %d alpha %d %u lighting %d "
        "group %u\n",
        static_cast<int>(pass->getSourceBlendFactor()),
        static_cast<int>(pass->getDestBlendFactor()),
        static_cast<int>(pass->getSceneBlendingOperation()),
        static_cast<int>(pass->getSourceBlendFactorAlpha()),
        static_cast<int>(pass->getDestBlendFactorAlpha()),
        static_cast<int>(pass->getSceneBlendingOperationAlpha()),
        pass->getSourceBlendFactor() != pass->getSourceBlendFactorAlpha() ||
                pass->getDestBlendFactor() != pass->getDestBlendFactorAlpha()
            ? 1
            : 0,
        static_cast<int>(pass->getCullingMode()), pass->getDepthCheckEnabled() ? 1 : 0,
        pass->getDepthWriteEnabled() ? 1 : 0, static_cast<int>(pass->getDepthFunction()),
        pass->getColourWriteEnabled() ? 1 : 0, static_cast<int>(pass->getAlphaRejectFunction()),
        pass->getAlphaRejectValue(), pass->getLightingEnabled() ? 1 : 0, queue_group(rend));
    std::fprintf(f, "surface");
    write_colour(f, "ambient", pass->getAmbient());
    write_colour(f, "diffuse", pass->getDiffuse());
    write_colour(f, "specular", pass->getSpecular());
    write_colour(f, "emissive", pass->getSelfIllumination());
    std::fprintf(f, " shininess %.4g tracking %d fog_override %d\n",
                 static_cast<double>(pass->getShininess()),
                 static_cast<int>(pass->getVertexColourTracking()), pass->getFogOverride() ? 1 : 0);

    if (lights) {
        for (const Ogre::Light* light : *lights) {
            const Ogre::Vector3 at = light->getDerivedPosition();
            const Ogre::Vector3 dir = light->getDerivedDirection();
            std::fprintf(f, "light %s type %d", spaced(light->getName()).c_str(),
                         static_cast<int>(light->getType()));
            write_colour(f, "diffuse", light->getDiffuseColour());
            write_colour(f, "specular", light->getSpecularColour());
            std::fprintf(f, " at %.4g %.4g %.4g dir %.4g %.4g %.4g range %.4g %.4g %.4g %.4g\n",
                         static_cast<double>(at.x), static_cast<double>(at.y),
                         static_cast<double>(at.z), static_cast<double>(dir.x),
                         static_cast<double>(dir.y), static_cast<double>(dir.z),
                         static_cast<double>(light->getAttenuationRange()),
                         static_cast<double>(light->getAttenuationConstant()),
                         static_cast<double>(light->getAttenuationLinear()),
                         static_cast<double>(light->getAttenuationQuadric()));
        }
    }

    // The constants as they are about to be bound: the automatic parameters are updated right
    // after this notification from the same source, so these are the values the draw sees.
    if (pass->isProgrammable()) {
        const_cast<Ogre::Pass*>(pass)->_updateAutoParams(source, Ogre::GPV_ALL);
        if (pass->hasVertexProgram()) write_constants(f, "vs", pass->getVertexProgramParameters());
        if (pass->hasFragmentProgram())
            write_constants(f, "ps", pass->getFragmentProgramParameters());
    }
}

}  // namespace tl::render
