// A `.zip` mod's entries and bytes, through the zip library OGRE compiles into its own `Zip`
// archive type: the central directory alone is read to list it, and one entry at a time to
// read it, so a large mod costs nothing until a file is wanted. Two users: the mod list's
// reader (core/mods.h, before any OGRE root), and the archive type a zip mod is mounted as.
#include "render/mod_archive.h"

#include "core/mods.h"

#include <OgreArchive.h>
#include <OgreArchiveFactory.h>
#include <OgreArchiveManager.h>
#include <OgreDataStream.h>
#include <OgreException.h>
#include <OgreString.h>
#include <OgreStringVector.h>

#include <zip.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tl::render {

namespace {

std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// An entry's name as the port spells a path: forward slashes, no leading one.
std::string normalise(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    return name;
}

// Every file entry of the zip at `archive` (folders left out), in index order, each with its
// index; false when it is not a zip.
struct ZipEntry {
    std::string name;  ///< as stored, slashes normalised
    int index = 0;
    size_t size = 0;
};

bool list_zip_entries(const std::string& archive, std::vector<ZipEntry>& entries) {
    zip_t* zip = zip_open(archive.c_str(), 0, 'r');
    if (zip == nullptr) return false;
    const auto count = zip_entries_total(zip);
    for (decltype(zip_entries_total(zip)) i = 0; i < count; ++i) {
        if (zip_entry_openbyindex(zip, i) != 0) continue;
        const char* name = zip_entry_name(zip);
        if (name != nullptr && zip_entry_isdir(zip) == 0) {
            entries.push_back(
                {normalise(name), static_cast<int>(i), static_cast<size_t>(zip_entry_size(zip))});
        }
        zip_entry_close(zip);
    }
    zip_close(zip);
    return true;
}

// The entry at `index` of an open zip, whole.
bool read_entry(zip_t* zip, int index, std::vector<uint8_t>& bytes) {
    if (zip_entry_openbyindex(zip, index) != 0) return false;
    bytes.assign(static_cast<size_t>(zip_entry_size(zip)), 0);
    const bool read = bytes.empty() || zip_entry_noallocread(zip, bytes.data(), bytes.size()) >= 0;
    zip_entry_close(zip);
    return read;
}

// --- the mod list's reader -----------------------------------------------------------------

bool list_zip(const std::string& archive, std::vector<std::string>& entries) {
    std::vector<ZipEntry> listed;
    if (!list_zip_entries(archive, listed)) return false;
    for (ZipEntry& entry : listed) entries.push_back(std::move(entry.name));
    return true;
}

bool read_zip_entry(const std::string& archive, const std::string& entry,
                    std::vector<uint8_t>& bytes) {
    std::vector<ZipEntry> listed;
    if (!list_zip_entries(archive, listed)) return false;
    const std::string wanted = lower_ascii(normalise(entry));
    zip_t* zip = zip_open(archive.c_str(), 0, 'r');
    if (zip == nullptr) return false;
    bool read = false;
    for (const ZipEntry& candidate : listed) {
        if (lower_ascii(candidate.name) != wanted) continue;
        read = read_entry(zip, candidate.index, bytes);
        break;
    }
    zip_close(zip);
    return read;
}

// --- the archive type ----------------------------------------------------------------------

/// A zip mod as OGRE sees it: the entries under the wrapping folder (if any) by their path,
/// each a `FileInfo` as the non-strict `ZipArchive` fills one (`filename` the basename, which
/// is what the resource group indexes by), read through the zip kept open.
class ModZipArchive final : public Ogre::Archive {
public:
    ModZipArchive(const Ogre::String& name, const Ogre::String& type) : Archive(name, type) {}
    ~ModZipArchive() override { ModZipArchive::unload(); }

    bool isCaseSensitive() const override { return false; }

    void load() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (zip_ != nullptr) return;
        std::vector<ZipEntry> listed;
        if (!list_zip_entries(mName, listed)) {
            OGRE_EXCEPT(Ogre::Exception::ERR_FILE_NOT_FOUND, "could not open " + mName,
                        "ModZipArchive::load");
        }
        std::vector<std::string> names;
        names.reserve(listed.size());
        for (const ZipEntry& entry : listed) names.push_back(entry.name);
        const std::string prefix = lower_ascii(tl::core::mod_archive_prefix(names));
        for (ZipEntry& entry : listed) {
            const std::string lower = lower_ascii(entry.name);
            if (lower.size() <= prefix.size() || lower.compare(0, prefix.size(), prefix) != 0) {
                continue;  // a loose file beside the wrapping folder is not the mod's
            }
            Ogre::FileInfo info;
            info.archive = this;
            info.filename = entry.name.substr(prefix.size());
            Ogre::StringUtil::splitFilename(info.filename, info.basename, info.path);
            info.filename = info.basename;  // as `ZipArchive::load` under STRICT=0
            info.compressedSize = entry.size;
            info.uncompressedSize = entry.size;
            by_path_.emplace(lower.substr(prefix.size()), files_.size());
            by_basename_.emplace(lower_ascii(info.basename), files_.size());
            files_.push_back(std::move(info));
            indices_.push_back(entry.index);
        }
        zip_ = zip_open(mName.c_str(), 0, 'r');
    }

    void unload() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (zip_ != nullptr) zip_close(zip_);
        zip_ = nullptr;
        files_.clear();
        indices_.clear();
        by_path_.clear();
        by_basename_.clear();
    }

    // A path exactly (case aside), else a basename that one file alone has: what the
    // non-strict `ZipArchive::open` does after its own exact open.
    Ogre::DataStreamPtr open(const Ogre::String& filename, bool /*readOnly*/) const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        const size_t at = find_one(filename);
        std::vector<uint8_t> bytes;
        if (at == SIZE_MAX || zip_ == nullptr || !read_entry(zip_, indices_[at], bytes)) {
            OGRE_EXCEPT(Ogre::Exception::ERR_FILE_NOT_FOUND, "could not open " + filename,
                        "ModZipArchive::open");
        }
        const Ogre::FileInfo& info = files_[at];
        auto stream = std::make_shared<Ogre::MemoryDataStream>(info.path + info.basename,
                                                               bytes.size(), true, true);
        if (!bytes.empty()) std::copy(bytes.begin(), bytes.end(), stream->getPtr());
        return stream;
    }

    Ogre::StringVectorPtr list(bool recursive, bool dirs) const override {
        return find("*", recursive, dirs);
    }

    Ogre::FileInfoListPtr listFileInfo(bool recursive, bool dirs) const override {
        return findFileInfo("*", recursive, dirs);
    }

    // The basenames matching `pattern`, as the non-strict `ZipArchive` lists them: the resource
    // group indexes what this returns, by basename (ADR 0002).
    Ogre::StringVectorPtr find(const Ogre::String& pattern, bool /*recursive*/,
                               bool dirs) const override {
        auto out = std::make_shared<Ogre::StringVector>();
        if (dirs) return out;
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Ogre::FileInfo& info : files_) {
            if (Ogre::StringUtil::match(info.basename, pattern, false))
                out->push_back(info.basename);
        }
        return out;
    }

    Ogre::FileInfoListPtr findFileInfo(const Ogre::String& pattern, bool /*recursive*/,
                                       bool dirs) const override {
        auto out = std::make_shared<Ogre::FileInfoList>();
        if (dirs) return out;
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Ogre::FileInfo& info : files_) {
            if (Ogre::StringUtil::match(info.basename, pattern, false)) out->push_back(info);
        }
        return out;
    }

    // A name with a folder is a path and must be there as one - a folder mod's `stat` - so a
    // same-named file elsewhere in the mod does not stand in for the pak's.
    bool exists(const Ogre::String& filename) const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::string lower = lower_ascii(normalise(filename));
        if (lower.find('/') != std::string::npos) return by_path_.count(lower) != 0;
        return by_basename_.count(lower) != 0;
    }

    time_t getModifiedTime(const Ogre::String& /*filename*/) const override { return 0; }

private:
    // The file `filename` names: its path, else the one file of that basename; SIZE_MAX for
    // none or several.
    size_t find_one(const Ogre::String& filename) const {
        const std::string lower = lower_ascii(normalise(filename));
        const auto by_path = by_path_.find(lower);
        if (by_path != by_path_.end()) return by_path->second;
        const size_t slash = lower.find_last_of('/');
        const std::string basename = slash == std::string::npos ? lower : lower.substr(slash + 1);
        if (by_basename_.count(basename) != 1) return SIZE_MAX;
        return by_basename_.find(basename)->second;
    }

    mutable std::mutex mutex_;  // the zip library is not thread-safe
    zip_t* zip_ = nullptr;
    std::vector<Ogre::FileInfo> files_;
    std::vector<int> indices_;  ///< each file's entry index in the zip
    std::unordered_map<std::string, size_t> by_path_;
    std::unordered_multimap<std::string, size_t> by_basename_;
};

class ModZipArchiveFactory final : public Ogre::ArchiveFactory {
public:
    const Ogre::String& getType() const override {
        static const Ogre::String type = kModZipArchiveType;
        return type;
    }
    using Ogre::ArchiveFactory::createInstance;
    Ogre::Archive* createInstance(const Ogre::String& name, bool /*readOnly*/) override {
        return new ModZipArchive(name, getType());
    }
    void destroyInstance(Ogre::Archive* archive) override { delete archive; }
};

}  // namespace

const char* const kModZipArchiveType = "TorchlightModZip";

void install_mod_archive_reader() {
    tl::core::ModArchiveReader reader;
    reader.list = list_zip;
    reader.read = read_zip_entry;
    tl::core::set_mod_archive_reader(reader);
}

void register_mod_zip_archive() {
    static ModZipArchiveFactory factory;
    static bool registered = false;
    if (registered) return;
    registered = true;
    Ogre::ArchiveManager::getSingleton().addArchiveFactory(&factory);
}

namespace {
// Before main(): the reader's target is a pair of null pointers until then (constant
// initialisation), so no order between translation units is relied on.
const bool g_reader_installed = (install_mod_archive_reader(), true);
}  // namespace

}  // namespace tl::render
