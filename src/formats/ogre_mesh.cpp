// OGRE's binary mesh, the parts collision reads (formats/ogre_mesh.h). The chunk ids and layouts are
// OGRE's (`OgreMeshFileFormat.h`), and so is the walk: `MeshSerializerImpl::readMesh`, `readSubMesh` and
// `readGeometry` read their chunks in sequence and stop at the first that is not theirs, never trusting a
// length - Torchlight's collision meshes carry `M_MESH` lengths shorter than the chunk.
#include "formats/ogre_mesh.h"

#include <cstring>

namespace tl::formats {

namespace {

constexpr uint16_t kHeader = 0x1000;
constexpr uint16_t kMesh = 0x3000;
constexpr uint16_t kSubmesh = 0x4000;
constexpr uint16_t kSubmeshOperation = 0x4010;
constexpr uint16_t kSubmeshBoneAssignment = 0x4100;
constexpr uint16_t kSubmeshTextureAlias = 0x4200;
constexpr uint16_t kGeometry = 0x5000;
constexpr uint16_t kVertexDeclaration = 0x5100;
constexpr uint16_t kVertexElement = 0x5110;
constexpr uint16_t kVertexBuffer = 0x5200;
constexpr uint16_t kVertexBufferData = 0x5210;
constexpr uint16_t kSkeletonLink = 0x6000;
constexpr size_t kChunkHeader = 6;  // uint16 id, uint32 length
constexpr uint16_t kSemanticPosition = 1;
constexpr uint16_t kTypeFloat3 = 2;
constexpr uint16_t kSemanticTexcoord = 7;
constexpr uint16_t kTypeFloat2 = 1;

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool ok() const { return ok_; }
    size_t at() const { return at_; }
    void back(size_t n) { at_ -= n; }
    bool eof() const { return at_ >= size_; }
    void set_swap(bool swap) { swap_ = swap; }

    void raw(void* out, size_t n) {
        if (!ok_ || size_ - at_ < n) {
            ok_ = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, data_ + at_, n);
        at_ += n;
    }
    void skip(size_t n) {
        if (!ok_ || size_ - at_ < n) ok_ = false;
        else at_ += n;
    }
    uint16_t u16() {
        uint8_t b[2];
        raw(b, 2);
        return swap_ ? static_cast<uint16_t>((b[0] << 8) | b[1])
                     : static_cast<uint16_t>(b[0] | (b[1] << 8));
    }
    uint32_t u32() {
        uint8_t b[4];
        raw(b, 4);
        return word(b);
    }
    float f32_at(size_t at) const {
        const uint32_t u = word(data_ + at);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    bool boolean() {
        uint8_t b = 0;
        raw(&b, 1);
        return b != 0;
    }
    /// A string to its '\n' (`Serializer::readString`).
    std::string line() {
        std::string out;
        while (ok_ && at_ < size_ && data_[at_] != '\n')
            out.push_back(static_cast<char>(data_[at_++]));
        if (at_ < size_) ++at_;
        else ok_ = false;
        return out;
    }
    bool has(size_t n) const { return ok_ && size_ - at_ >= n; }
    /// A chunk's id (its length read past, `Serializer::readChunk`); 0 at the end.
    uint16_t chunk() {
        if (!has(kChunkHeader)) {
            at_ = size_;
            return 0;
        }
        const uint16_t id = u16();
        last_length_ = u32();
        return id;
    }
    uint32_t last_length() const { return last_length_; }

private:
    uint32_t word(const uint8_t* b) const {
        if (swap_)
            return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                   (static_cast<uint32_t>(b[2]) << 8) | b[3];
        return b[0] | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
               (static_cast<uint32_t>(b[3]) << 24);
    }
    const uint8_t* data_;
    size_t size_;
    size_t at_ = 0;
    bool ok_ = true;
    bool swap_ = false;
    uint32_t last_length_ = 0;
};

struct Element {
    uint16_t source = 0, type = 0, semantic = 0, offset = 0;
};

/// `readGeometryVertexDeclaration`: its elements, until another chunk.
void read_vertex_declaration(Reader& r, std::vector<Element>& elements) {
    while (r.ok() && !r.eof()) {
        const uint16_t id = r.chunk();
        if (id != kVertexElement) {
            if (id != 0) r.back(kChunkHeader);
            return;
        }
        Element e;
        e.source = r.u16();
        e.type = r.u16();
        e.semantic = r.u16();
        e.offset = r.u16();
        (void)r.u16();  // index
        elements.push_back(e);
    }
}

/// The float3 positions of buffer `bind`, read from its data at `data_at`.
void copy_positions(const Reader& r, OgreMeshVertices& out, const std::vector<Element>& elements,
                    uint16_t bind, uint16_t vertex_size, size_t data_at) {
    for (const Element& e : elements) {
        if (e.semantic != kSemanticPosition || e.source != bind || e.type != kTypeFloat3) continue;
        if (e.offset + 12u > vertex_size) break;
        out.positions.resize(static_cast<size_t>(out.count) * 3);
        for (uint32_t v = 0; v < out.count; ++v) {
            const size_t at = data_at + static_cast<size_t>(v) * vertex_size + e.offset;
            out.positions[v * 3 + 0] = r.f32_at(at);
            out.positions[v * 3 + 1] = r.f32_at(at + 4);
            out.positions[v * 3 + 2] = r.f32_at(at + 8);
        }
    }
}

/// The first float2 texture coordinates of buffer `bind`.
void copy_texcoords(const Reader& r, OgreMeshVertices& out, const std::vector<Element>& elements,
                    uint16_t bind, uint16_t vertex_size, size_t data_at) {
    if (!out.texcoords.empty()) return;
    for (const Element& e : elements) {
        if (e.semantic != kSemanticTexcoord || e.source != bind || e.type != kTypeFloat2) continue;
        if (e.offset + 8u > vertex_size) break;
        out.texcoords.resize(static_cast<size_t>(out.count) * 2);
        for (uint32_t v = 0; v < out.count; ++v) {
            const size_t at = data_at + static_cast<size_t>(v) * vertex_size + e.offset;
            out.texcoords[v * 2 + 0] = r.f32_at(at);
            out.texcoords[v * 2 + 1] = r.f32_at(at + 4);
        }
        return;
    }
}

/// `readGeometryVertexBuffer`: bind index, vertex size, then the data chunk. False where the
/// geometry's walk ends: no data chunk, or the file ends inside it.
bool read_vertex_buffer(Reader& r, OgreMeshVertices& out, const std::vector<Element>& elements) {
    const uint16_t bind = r.u16();
    const uint16_t vertex_size = r.u16();
    if (r.chunk() != kVertexBufferData) return false;
    const size_t data_at = r.at();
    const size_t bytes = static_cast<size_t>(out.count) * vertex_size;
    if (!r.has(bytes)) {
        r.skip(bytes);
        return false;
    }
    copy_positions(r, out, elements, bind, vertex_size, data_at);
    copy_texcoords(r, out, elements, bind, vertex_size, data_at);
    r.skip(bytes);
    return true;
}

/// `readGeometry`: the vertex count, then declarations and buffers until another chunk.
void read_geometry(Reader& r, OgreMeshVertices& out) {
    out.present = true;
    out.count = r.u32();
    std::vector<Element> elements;
    while (r.ok() && !r.eof()) {
        const uint16_t id = r.chunk();
        if (id == kVertexDeclaration) {
            read_vertex_declaration(r, elements);
        } else if (id == kVertexBuffer) {
            if (!read_vertex_buffer(r, out, elements)) return;
        } else {
            if (id != 0) r.back(kChunkHeader);
            return;
        }
    }
}

/// The submesh's indices; false where the file ends inside them.
bool read_indices(Reader& r, OgreMeshSubmesh& sub, uint32_t index_count) {
    if (index_count == 0) return true;
    const size_t bytes = static_cast<size_t>(index_count) * (sub.indices_32bit ? 4 : 2);
    if (!r.has(bytes)) {
        r.skip(bytes);
        return false;
    }
    sub.indices.resize(index_count);
    for (uint32_t i = 0; i < index_count; ++i)
        sub.indices[i] = sub.indices_32bit ? r.u32() : r.u16();
    return true;
}

/// A submesh's operation, bone assignments and texture aliases, until another chunk.
void skip_submesh_tail(Reader& r) {
    while (r.ok() && !r.eof()) {
        const uint16_t id = r.chunk();
        if (id == kSubmeshOperation) {
            (void)r.u16();
        } else if (id == kSubmeshBoneAssignment) {
            r.skip(4 + 2 + 4);  // vertex index, bone index, weight
        } else if (id == kSubmeshTextureAlias) {
            (void)r.line();
            (void)r.line();
        } else {
            if (id != 0) r.back(kChunkHeader);
            return;
        }
    }
}

/// `readSubMesh`: the material, the shared flag, the indices, its own geometry, then its operation, bone
/// assignments and texture aliases until another chunk.
void read_submesh(Reader& r, OgreMeshSubmesh& sub) {
    sub.material = r.line();
    sub.uses_shared_vertices = r.boolean();
    const uint32_t index_count = r.u32();
    sub.indices_32bit = r.boolean();
    if (!read_indices(r, sub, index_count)) return;
    if (!sub.uses_shared_vertices) {
        if (r.chunk() != kGeometry) return;
        read_geometry(r, sub.vertices);
    }
    skip_submesh_tail(r);
}

}  // namespace

OgreMesh parse_ogre_mesh(const uint8_t* data, size_t size) {
    OgreMesh mesh;
    if (size < 2) {
        mesh.error = "too short";
        return mesh;
    }
    Reader r(data, size);
    // `Serializer::determineEndianness`: the header id as written tells the byte order.
    const uint16_t first = static_cast<uint16_t>(data[0] | (data[1] << 8));
    if (first == 0x0010) {
        mesh.big_endian = true;
        r.set_swap(true);
    } else if (first != kHeader) {
        mesh.error = "not an OGRE mesh";
        return mesh;
    }
    (void)r.u16();
    mesh.version = r.line();
    // `importMesh`: chunks until the mesh itself.
    while (r.ok() && !r.eof()) {
        const uint16_t id = r.chunk();
        if (id != kMesh) {
            if (id == 0 || r.last_length() < kChunkHeader) break;
            r.skip(r.last_length() - kChunkHeader);
            continue;
        }
        (void)r.boolean();  // skeletally animated
        // The shared geometry and the submeshes come first, then the skeleton link
        // (`MeshSerializerImpl::writeMesh`); nothing after it is read.
        while (r.ok() && !r.eof()) {
            const uint16_t c_id = r.chunk();
            if (c_id == kGeometry) {
                read_geometry(r, mesh.shared);
            } else if (c_id == kSubmesh) {
                OgreMeshSubmesh sub;
                read_submesh(r, sub);
                mesh.submeshes.push_back(std::move(sub));
            } else {
                if (c_id == kSkeletonLink) mesh.skeleton = r.line();
                break;
            }
        }
        break;
    }
    mesh.ok = r.ok();
    if (!mesh.ok) mesh.error = "the file ends early";
    return mesh;
}

}  // namespace tl::formats
