// A `.zip` mod read in place (core/mods.h): the reader the renderer gives the core, and the
// archive type a zip mod is mounted as, both over the zip code OGRE's own archives use.
#pragma once

namespace tl::render {

/// Install the zip reader the mod list reads a `.zip` mod's `mod.dat` and entries with. Done
/// at start-up by mod_archive.cpp itself, before main(): the mods are read before the
/// renderer exists, and the reader needs no OGRE root.
void install_mod_archive_reader();

/// The archive type a `.zip` mod is mounted as (`ResourceGroupManager::addResourceLocation`):
/// its entries by their path under the folder the zip wraps the mod in, if any; `exists` is
/// exact for a path, as a folder's is (OGRE's own `Zip` answers by basename alone, and then
/// opens a same-named file of another folder, or none); entries are read on demand, the zip
/// itself never held in memory. Registered by register_mod_zip_archive(), once a root exists.
extern const char* const kModZipArchiveType;
void register_mod_zip_archive();

}  // namespace tl::render
