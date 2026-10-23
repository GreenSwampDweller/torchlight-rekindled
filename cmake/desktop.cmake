# Desktop target configuration.
#
# Development happens here: seconds per iteration instead of minutes plus a
# console reboot. Roughly 95% of the work. Cross-check on Switch at every
# milestone, not every commit.

set(TL_OGRE_BUILD_DIR "${CMAKE_SOURCE_DIR}/build-ogre-desktop"
    CACHE PATH "Directory containing the desktop build of OGRE")
if(APPLE)
    # Xcode adds its configuration directory to target_link_directories.
    set(TL_OGRE_LIB_DIR "${TL_OGRE_BUILD_DIR}/lib/macosx")
else()
    set(TL_OGRE_LIB_DIR "${TL_OGRE_BUILD_DIR}/lib")
endif()

# OGRE is optional until M1 wires up rendering, so that a fresh clone configures
# and the tests run before anyone has spent 20 minutes building the engine.
if(EXISTS "${TL_OGRE_LIB_DIR}")
    set(TL_HAVE_OGRE TRUE)
else()
    set(TL_HAVE_OGRE FALSE)
    message(STATUS
        "No desktop OGRE at ${TL_OGRE_BUILD_DIR} - building without rendering. "
        "Run: python scripts/build_ogre.py --platform desktop")
endif()

# Desktop links OGRE statically, exactly as the Switch does. The console has no
# dynamic loader, so plugins must be registered with Root::installPlugin(); if
# desktop loaded DLLs instead, the plugin and render-system startup path
# verified here would not be the one that runs on the console. See ADR 0001 and
# hard constraint 3.
#
# The static libraries have circular dependencies, so they are wrapped in
# --start-group/--end-group rather than ordered by hand.
# OgreMainStatic first: see the note in cmake/switch.cmake - .init_array order
# follows link order, and OGRE's components have globals that copy from
# OgreMain's during static initialisation.
set(TL_OGRE_LIBS
    OgreMainStatic
    RenderSystem_GL3PlusStatic
    RenderSystem_GLStatic
    OgreGLSupportStatic
    Codec_STBIStatic
    OgreRTShaderSystemStatic
    OgreOverlayStatic
)

function(tl_configure_platform target)
    target_compile_definitions(${target} PRIVATE TL_PLATFORM_DESKTOP=1)

    if(NOT TL_HAVE_OGRE)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE TL_HAVE_OGRE=1)

    # RTShaderSystem reads its GLSL library from OGRE's Media directory at
    # runtime. On desktop it can be used straight out of the source tree; the
    # Switch build has to ship these files to the SD card, so this is a define
    # rather than a hardcoded path in the engine. --ogre-media overrides it.
    # A player folder reads the RTShaderLib/ beside the program instead, so a
    # release build passes -DTL_OGRE_MEDIA_DIR=. and names no folder of its own.
    set(TL_OGRE_MEDIA_DIR "${CMAKE_SOURCE_DIR}/external/ogre/Media" CACHE STRING
        "Where a build run from the source tree finds RTShaderLib/")
    target_compile_definitions(${target} PRIVATE TL_OGRE_MEDIA_DIR="${TL_OGRE_MEDIA_DIR}")

    # OGRE_STATIC_LIB switches OGRE's headers from dllimport to static linkage.
    # Without it every symbol resolves to an import stub that does not exist.
    target_compile_definitions(${target} PRIVATE OGRE_STATIC_LIB)

    target_include_directories(${target} SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/external/ogre/Components/Overlay/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/OgreMain/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GL3Plus/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GL/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GLSupport/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/Components/RTShaderSystem/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/PlugIns/STBICodec/include"
        "${TL_OGRE_BUILD_DIR}/include"
    )

    target_link_directories(${target} PRIVATE "${TL_OGRE_LIB_DIR}")
    if(APPLE)
        # Apple's linker has no --start-group/--end-group. Repeating this small
        # static library set resolves OGRE's cyclic references on macOS.
        target_link_libraries(${target} PRIVATE ${TL_OGRE_LIBS} ${TL_OGRE_LIBS})
    else()
        target_link_libraries(${target} PRIVATE
            -Wl,--start-group ${TL_OGRE_LIBS} -Wl,--end-group)
    endif()
    # OGRE is linked statically, so the build-tree RUNPATH CMake derives from
    # the link directory serves nothing and would name the build machine's path.
    set_target_properties(${target} PROPERTIES SKIP_BUILD_RPATH ON)

    # FreeType, for the interface layer's font: the game's own
    # `Torchlight Regular.ttf` out of its own archive. OGRE's Overlay component
    # needs it to rasterise a TrueType face, and it is present for both targets.
    if(APPLE)
        find_package(Freetype REQUIRED)
        target_link_libraries(${target} PRIVATE Freetype::Freetype)
    else()
        target_link_libraries(${target} PRIVATE freetype)
    endif()

    # zlib is not OGRE's zip reader - that is the bundled miniz - but the STBI
    # codec's PNG *writer*, which --dump-frame goes through, calls compress().
    target_link_libraries(${target} PRIVATE z)

    if(APPLE)
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo"
            "-framework AudioToolbox" "-framework GameController")
    elseif(WIN32)
        target_link_libraries(${target} PRIVATE opengl32 gdi32 user32 winmm advapi32 shlwapi)
    else()
        target_link_libraries(${target} PRIVATE GL X11 dl pthread)
        # What a Linux OGRE may have compiled in, depending on what its
        # configure found: EGL, which GLSupport uses by default everywhere but
        # Windows (OGRE_GLSUPPORT_USE_EGL), and XRandR, which OgreX11.cpp uses
        # for display modes when its headers are installed. Linked when present;
        # an OGRE built without them does not reference them.
        foreach(lib EGL Xrandr)
            find_library(TL_${lib}_LIBRARY ${lib})
            if(TL_${lib}_LIBRARY)
                target_link_libraries(${target} PRIVATE ${TL_${lib}_LIBRARY})
            endif()
        endforeach()
    endif()
endfunction()
