#include "formats/adm.h"

#include "formats/byte_reader.h"
#include "formats/dat_text.h"
#include "formats/float_convert.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace tl::formats {
namespace {

/// A record is 12 bytes, except a Guid, whose value is 64 bits.
constexpr size_t kRecordSize = 12;
constexpr size_t kGuidRecordSize = 16;

constexpr uint32_t kMaxTableEntries = 100000;
constexpr uint32_t kMaxChildren = 100000;

/// Sections nest, and a malformed file must not recurse until the stack gives
/// out. The deepest seen in the archive is a handful.
constexpr int kMaxDepth = 32;

uint32_t u32_at(const uint8_t* d, size_t size, size_t off) {
    if (off + 4 > size) return 0;
    uint32_t v = 0;
    std::memcpy(&v, d + off, 4);
    return v;
}

int64_t i64_at(const uint8_t* d, size_t size, size_t off) {
    if (off + 8 > size) return 0;
    int64_t v = 0;
    std::memcpy(&v, d + off, 8);
    return v;
}

}  // namespace

const AdmRecord* AdmSection::find(const std::string& key) const {
    for (const AdmRecord& r : records) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

uint32_t AdmSection::integer_or(const std::string& key, uint32_t fallback) const {
    const AdmRecord* r = find(key);
    if (!r) return fallback;
    // A key authored as a float and read as an integer: x86's conversion on
    // every CPU (a `-1` is 0xffffffff there and 0 on AArch64 by plain cast).
    if (r->type == AdmType::Float) return x86_float_to_uint32(r->number);
    return r->integer;
}

namespace {

// PORT: the 32 bits of a value word as a float.
float bits_as_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// PORT: the value word's 32 bits as the console holds them.
uint32_t word_bits(const AdmRecord& r) {
    switch (r.type) {
        case AdmType::Float: {
            uint32_t bits;
            std::memcpy(&bits, &r.number, sizeof bits);
            return bits;
        }
        case AdmType::Integer:
        case AdmType::Unsigned: return r.integer;
        case AdmType::Bool: return r.integer != 0 ? 0x01000000u : 0u;
        case AdmType::Guid: return static_cast<uint32_t>(static_cast<uint64_t>(r.guid) >> 32);
        default: return 0;
    }
}

}  // namespace

float AdmRecord::word_as_float() const {
    return type == AdmType::Float ? number : bits_as_float(word_bits(*this));
}

uint8_t AdmRecord::word_first_byte() const { return static_cast<uint8_t>(word_bits(*this) >> 24); }

uint32_t AdmRecord::word_as_integer() const { return word_bits(*this); }

float AdmSection::float_or(const std::string& key, float fallback) const {
    const AdmRecord* r = find(key);
    if (!r) return fallback;
    if (r->type == AdmType::Float) return r->number;
    return static_cast<float>(r->integer);
}

std::string AdmSection::string_or(const std::string& key, const std::string& fallback) const {
    const AdmRecord* r = find(key);
    if (!r) return fallback;
    return (r->type == AdmType::String || r->type == AdmType::Translate) ? r->text : fallback;
}

bool AdmSection::bool_or(const std::string& key, bool fallback) const {
    const AdmRecord* r = find(key);
    if (!r) return fallback;
    return r->integer != 0;
}

int64_t AdmSection::guid_or(const std::string& key, int64_t fallback) const {
    const AdmRecord* r = find(key);
    return (r && r->type == AdmType::Guid) ? r->guid : fallback;
}

bool AdmSection::colour(const std::string& prefix, float& r, float& g, float& b) const {
    const AdmRecord* red = find(prefix + " RED");
    const AdmRecord* green = find(prefix + " GREEN");
    const AdmRecord* blue = find(prefix + " BLUE");
    if (!red && !green && !blue) return false;

    // Stored 0-255 as integers. A missing component means zero rather than a
    // failure, matching how the data leaves defaults out.
    auto channel = [](const AdmRecord* record) {
        if (!record) return 0.0f;
        const float raw =
            (record->type == AdmType::Float) ? record->number : static_cast<float>(record->integer);
        return raw / 255.0f;
    };
    r = channel(red);
    g = channel(green);
    b = channel(blue);
    return true;
}

void AdmSection::visit(const std::function<void(const AdmSection&)>& fn) const {
    fn(*this);
    for (const AdmSection& child : children) child.visit(fn);
}

namespace {
// Not a compiled file: `CDataGroup::LoadFile` reads the text form too (a mod's files are text).
AdmFile not_compiled(const uint8_t* data, size_t size, uint32_t version) {
    if (looks_like_dat_text(data, size)) return parse_dat_text(data, size);
    AdmFile file;
    file.error =
        size < 8 ? "too small to be a .adm" : "unexpected .adm version " + std::to_string(version);
    return file;
}
}  // namespace

AdmFile parse_adm(const uint8_t* data, size_t size) {
    AdmFile file;

    const uint32_t version = u32_at(data, size, 0);
    const uint32_t table_entries = u32_at(data, size, 4);
    if (version != 1 || size < 8) return not_compiled(data, size, version);
    if (table_entries > kMaxTableEntries) {
        file.error = "implausible string table size";
        return file;
    }

    // The string table: `table_entries` of [id][character count][UTF-16LE].
    // The count in the header is exact, so this needs no heuristic for where
    // the table ends.
    std::unordered_map<uint32_t, std::string> table;
    table.reserve(table_entries);

    size_t off = 8;
    for (uint32_t i = 0; i < table_entries; ++i) {
        if (off + 8 > size) {
            file.error = "string table ran past the end";
            return file;
        }
        const uint32_t ident = u32_at(data, size, off);
        const uint32_t chars = u32_at(data, size, off + 4);
        if (off + 8 + 2 * static_cast<size_t>(chars) > size) {
            file.error = "string table entry ran past the end";
            return file;
        }
        ByteReader reader(data + off + 8, 2 * static_cast<size_t>(chars));
        table.emplace(ident, reader.utf16le(chars));
        off += 8 + 2 * static_cast<size_t>(chars);
    }

    // No padding: sections begin right after the last string (aligning to four
    // bytes shifts values by 16 bits without failing).
    bool failed = false;
    std::string failure;

    // [nameId][recordCount] records... [childCount] children...
    std::function<AdmSection(size_t&, int)> read_section = [&](size_t& cursor,
                                                               int depth) -> AdmSection {
        AdmSection node;
        if (failed || depth > kMaxDepth || cursor + 8 > size) {
            if (!failed) {
                failed = true;
                failure = (depth > kMaxDepth) ? "sections nested too deep"
                                              : "section header ran past the end";
            }
            return node;
        }

        const uint32_t name_id = u32_at(data, size, cursor);
        const uint32_t record_count = u32_at(data, size, cursor + 4);
        cursor += 8;

        auto name = table.find(name_id);
        if (name == table.end()) {
            failed = true;
            failure = "section name is not in the string table";
            return node;
        }
        node.name = name->second;

        // Reserved for no more than the bytes left can hold: the count is the
        // file's word, and a corrupt one would otherwise ask for gigabytes.
        node.records.reserve(std::min<size_t>(record_count, (size - cursor) / kRecordSize));
        for (uint32_t i = 0; i < record_count; ++i) {
            if (cursor + kRecordSize > size) {
                failed = true;
                failure = "records ran past the end in section " + node.name;
                return node;
            }
            const uint32_t key_id = u32_at(data, size, cursor);
            auto key = table.find(key_id);
            if (key == table.end()) {
                failed = true;
                failure = "record key is not in the string table";
                return node;
            }

            AdmRecord record;
            record.key = key->second;
            record.type = static_cast<AdmType>(u32_at(data, size, cursor + 4));

            if (record.type == AdmType::Guid) {
                // The only 16-byte record: a 64-bit value.
                if (cursor + kGuidRecordSize > size) {
                    failed = true;
                    failure = "guid record ran past the end";
                    return node;
                }
                record.guid = i64_at(data, size, cursor + 8);
                cursor += kGuidRecordSize;
            } else {
                const uint32_t raw = u32_at(data, size, cursor + 8);
                if (record.type == AdmType::Float) {
                    std::memcpy(&record.number, &raw, 4);
                } else if (record.type == AdmType::String || record.type == AdmType::Translate) {
                    // A `<TRANSLATE>` is stored exactly like a `<STRING>`.
                    auto value = table.find(raw);
                    if (value == table.end()) {
                        failed = true;
                        failure = "string value is not in the string table";
                        return node;
                    }
                    record.text = value->second;
                } else {
                    record.integer = raw;
                }
                cursor += kRecordSize;
            }
            node.records.push_back(std::move(record));
        }

        if (cursor + 4 > size) {
            failed = true;
            failure = "missing child count in section " + node.name;
            return node;
        }
        const uint32_t children = u32_at(data, size, cursor);
        cursor += 4;
        if (children > kMaxChildren) {
            failed = true;
            failure = "implausible child count";
            return node;
        }

        // A child is at least its header and child count (12 bytes).
        node.children.reserve(std::min<size_t>(children, (size - cursor) / 12));
        for (uint32_t i = 0; i < children && !failed; ++i) {
            node.children.push_back(read_section(cursor, depth + 1));
        }
        return node;
    };

    file.root = read_section(off, 0);
    if (failed) {
        file.error = failure;
        return file;
    }

    file.undecoded_bytes = (off < size) ? size - off : 0;
    file.ok = true;
    return file;
}

}  // namespace tl::formats
