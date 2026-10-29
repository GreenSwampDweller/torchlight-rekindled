// The archive's lookups (renderer_archive.cpp): the basename index, paths by directory, the
// data files and what a mod folder lays over them.
#pragma once

#include <OgreArchive.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tl::render::renderer_detail {

extern std::unordered_map<std::string, std::vector<Ogre::FileInfo>> g_archive_index;
extern std::unordered_map<std::string, std::vector<std::string>> g_materials_by_dir;
extern bool g_archive_indexed;
bool ends_with_lower(const std::string& text, const char* suffix);
std::string lowered(const std::string& text);
void forget_archive_index();
void ensure_archive_index();
size_t material_script_rank(const std::string& script);
const std::vector<Ogre::FileInfo>* archive_entries(const std::string& basename);
std::vector<std::string> archive_paths_under(const std::string& dir, const char* suffix);
std::vector<std::string> archive_paths_in_listing_order(const std::string& dir, const char* suffix);
std::string resolve_archive_path(const std::string& reference);
/// Keep read_resource()'s data files only while the same archives are mounted.
void remember_mounts(const std::string& mounts);
/// Absolute roots of extracted mod folders (`--mod-data`), in priority order; these
/// filesystems are searched case-insensitively before the base archive.
void set_mod_roots(const std::vector<std::string>& roots);
bool read_mod_resource(const std::string& path, std::vector<uint8_t>& bytes);
/// Whether a mod answers `path` (read_mod_resource() would read it).
bool mod_has_file(const std::string& path);
/// The file a mod answers `path` with, on disk, or empty.
std::string mod_file_path(const std::string& path);
/// Each mod's own copy of `path`, in the mods' order.
std::vector<std::vector<uint8_t>> read_mod_copies(const std::string& path);
std::vector<std::string> mod_paths_under(const std::string& dir, const char* suffix);
std::vector<std::string> data_paths_under(const std::string& dir);
std::vector<std::string> data_paths_in_listing_order(const std::string& dir);
std::vector<uint8_t> read_resource(const std::string& path);

}  // namespace tl::render::renderer_detail
