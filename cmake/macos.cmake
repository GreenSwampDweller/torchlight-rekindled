# Native Apple Silicon/macOS build using OGRE's Cocoa window and OpenGL.
set(TL_OGRE_BUILD_DIR "${CMAKE_SOURCE_DIR}/build-ogre-macos-xcode"
    CACHE PATH "Directory containing the macOS build of OGRE")
include(cmake/desktop.cmake)
