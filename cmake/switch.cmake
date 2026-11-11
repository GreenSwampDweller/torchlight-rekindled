# Switch target configuration.
#
# A thin wrapper over devkitPro's Switch.cmake, which must be passed as
# CMAKE_TOOLCHAIN_FILE. This file only adds what OGRE and our own code need on
# top of it.

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR
        "cmake/switch.cmake included without devkitPro's Switch.cmake toolchain. "
        "Pass -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake")
endif()

set(DKP_PORTLIBS "${DEVKITPRO}/portlibs/switch")

# Experiment: OGRE's GL3Plus render system on switch-mesa's desktop GL, beside
# GLES2 in the same .nro, to compare their frame times on the console
# (src/render/render_system_choice.h). Needs the OGRE built by
# `python scripts/build_ogre.py --platform switch --gl3plus`; configure it into
# a build directory of its own (build-switch-gl3). OFF, nothing here changes
# what the console build is made of; ON, the game still starts on GLES2 and
# GL3Plus is picked from the debug menu or by a file on the card.
option(TL_SWITCH_GL3PLUS "Switch: link GL3Plus (desktop GL) beside GLES2, selectable at run time" OFF)

# Link-time and profile-guided optimisation (TL_LTO, TL_PGO). Before any target
# is defined, so its flags reach every one of them. Each variant links the OGRE
# built the same way - `python scripts/build_ogre.py --platform switch
# --variant lto|pgogen|pgo` - because that is where most of the frame is spent.
include(${CMAKE_CURRENT_LIST_DIR}/switch_perf.cmake)

if(TL_SWITCH_GL3PLUS)
    set(TL_OGRE_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/build-ogre-switch-gl3")
elseif(TL_PGO STREQUAL "generate")
    set(TL_OGRE_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/build-ogre-switch-pgogen")
elseif(TL_PGO STREQUAL "use")
    set(TL_OGRE_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/build-ogre-switch-pgo")
elseif(TL_LTO)
    set(TL_OGRE_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/build-ogre-switch-lto")
else()
    set(TL_OGRE_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/build-ogre-switch")
endif()

# OGRE must be built separately - it needs a long list of flags that would be
# invasive to impose on this tree. scripts/build_ogre.py does it.
set(TL_OGRE_BUILD_DIR "${TL_OGRE_DEFAULT_DIR}"
    CACHE PATH "Directory containing the Switch build of OGRE")

# OGRE is optional until M1 wires up rendering, so that a fresh clone configures
# for both targets before anyone has spent 20 minutes building the engine.
if(EXISTS "${TL_OGRE_BUILD_DIR}/lib")
    set(TL_HAVE_OGRE TRUE)
else()
    set(TL_HAVE_OGRE FALSE)
    message(STATUS
        "No Switch OGRE at ${TL_OGRE_BUILD_DIR} - building without rendering. "
        "Run: python scripts/build_ogre.py --platform switch")
endif()

# The GL driver: switch-mesa with patches/mesa applied (glthread - see
# scripts/build_mesa.py). That is the driver the console build is meant to have:
# on 2026-10-05 glthread measured about 5 ms a frame. devkitPro's own portlib
# is only the fallback for a checkout that has not got the patched one yet,
# and falling back is a warning, not a quiet choice. By full path rather than
# by name, so the portlib's copy can never be the one the linker finds first.
set(TL_MESA_LIB_DIR "${CMAKE_SOURCE_DIR}/build-mesa-switch/install/opt/devkitpro/portlibs/switch/lib"
    CACHE PATH "Directory holding the patched switch-mesa (scripts/build_mesa.py)")
# Experiment: danfromtico/mesa-switch (Mesa 26, its own Horizon GPU backend in
# place of libdrm_nouveau, glthread on by default with its workers on cores 1
# and 2), built by its build-opengl.sh. Pointed at its install's lib directory,
# it replaces the 20.1 driver: all its static libraries in one group, and no
# drm_nouveau.
set(TL_MESA26_LIB_DIR "" CACHE PATH "mesa-switch (Mesa 26) install lib directory; empty for the 20.1 driver")
set(TL_MESA_DRM drm_nouveau)
if(TL_MESA26_LIB_DIR AND EXISTS "${TL_MESA26_LIB_DIR}/libEGL.a")
    message(STATUS "Torchlight: Mesa 26 (mesa-switch) from ${TL_MESA26_LIB_DIR}")
    file(GLOB _tl_mesa26_libs "${TL_MESA26_LIB_DIR}/*.a")
    list(REMOVE_ITEM _tl_mesa26_libs "${TL_MESA26_LIB_DIR}/libGL.a" "${TL_MESA26_LIB_DIR}/libGLESv1_CM.a")
    set(TL_MESA_LIBS -Wl,--start-group ${_tl_mesa26_libs} zstd expat -Wl,--end-group)
    set(TL_MESA_DRM "")
elseif(EXISTS "${TL_MESA_LIB_DIR}/libEGL.a" AND EXISTS "${TL_MESA_LIB_DIR}/libGLESv2.a"
   AND EXISTS "${TL_MESA_LIB_DIR}/libglapi.a")
    message(STATUS "Torchlight: patched switch-mesa from ${TL_MESA_LIB_DIR}")
    set(TL_MESA_LIBS
        "${TL_MESA_LIB_DIR}/libGLESv2.a" "${TL_MESA_LIB_DIR}/libEGL.a" "${TL_MESA_LIB_DIR}/libglapi.a")
else()
    message(WARNING "Torchlight: no patched switch-mesa at ${TL_MESA_LIB_DIR} - linking "
                    "devkitPro's, which has no glthread (about 5 ms a frame slower on the "
                    "console). Build it with `python3 scripts/build_mesa.py` (WSL or Linux).")
    set(TL_MESA_LIBS GLESv2 EGL glapi)
endif()

# The sixteen static OGRE libraries have circular dependencies between them, so
# they are wrapped in --start-group/--end-group rather than ordered by hand.
#
# OgreMainStatic is listed FIRST anyway. --start-group makes the order
# irrelevant for symbol resolution, but .init_array is emitted in the order the
# linker pulls objects in, and OGRE's other libraries have globals that copy
# from OgreMain's at static-init time. Patch 0009 fixes the one case that
# actually crashed; this keeps the ordering sane for any that are still latent.
set(TL_OGRE_LIBS
    OgreMainStatic
    RenderSystem_GLES2Static
    OgreGLSupportStatic
    Plugin_ParticleFXStatic
    Plugin_OctreeSceneManagerStatic
    Plugin_OctreeZoneStatic
    Plugin_PCZSceneManagerStatic
    Plugin_BSPSceneManagerStatic
    Codec_STBIStatic
    OgreRTShaderSystemStatic
    OgreOverlayStatic
    OgreTerrainStatic
    OgrePagingStatic
    OgrePropertyStatic
    OgreVolumeStatic
    OgreMeshLodGeneratorStatic
)
if(TL_SWITCH_GL3PLUS)
    # After OgreMainStatic, which stays first.
    list(INSERT TL_OGRE_LIBS 1 RenderSystem_GL3PlusStatic)
endif()

# A tag for a second build run beside the first (an experiment, A/B): its log
# and the sampler's file are named after it, so one launch does not overwrite
# what the other wrote. Empty for the ordinary build.
set(TL_RUN_TAG "" CACHE STRING "Suffix of this build's log and profile names on the card")
# Every target: the log is opened in main.cpp, the sampler lives in tl_core.
add_compile_definitions(TL_RUN_TAG="${TL_RUN_TAG}")

function(tl_configure_platform target)
    target_compile_definitions(${target} PRIVATE
        TL_PLATFORM_SWITCH=1
        _GNU_SOURCE            # newlib hides locale_t and strtod_l without it
    )

    # .nro plus NACP metadata, via helpers from devkitPro's Switch.cmake. Done
    # before the OGRE check so the Switch target still produces a runnable .nro
    # while rendering is unwired.
    #
    # The home-menu icon: `src/logo_icon.jpg`, the 256x256 icon made from
    # `src/logo.jpg` (Torchlight's logo, from SteamGridDB) by
    #
    #     python scripts/make_icon.py
    #
    # and committed beside it, as the NRO asset section wants it. That size is
    # not advisory: libnx's own `default_icon.jpg`, which nx_create_nro falls
    # back to below, is exactly 256x256. The resize cannot happen during the
    # build, because devkitPro's MSYS2 shell - the only shell this target
    # configures in - has no Python at all.
    #
    # Without it the build is unaffected and takes libnx's default icon.
    # A second build beside the first (an experiment) can name itself apart in
    # hbmenu: -DTL_NRO_NAME=...
    set(TL_NRO_NAME "Torchlight Rekindled" CACHE STRING "The .nro's name in hbmenu")
    nx_generate_nacp(${target}.nacp
        NAME    "${TL_NRO_NAME}"
        AUTHOR  "SwampDweller"
        VERSION "1.1.1"
    )
    # OGRE's shader library, inside the .nro: its RomFS holds `RTShaderLib/`
    # with OGRE's `Main/` inside it, read at run time as `romfs:/RTShaderLib`
    # (TL_OGRE_MEDIA_DIR below; libnx mounts it, platform_frame.cpp). The files
    # are OGRE's own (MIT, with patches/ogre applied), not game data. Each is
    # copied into the build tree by a rule of its own, so a patched GLSL file
    # makes a new .nro, and the card needs no shader folder of its own.
    set(_tl_romfs "${CMAKE_BINARY_DIR}/romfs")
    set(_tl_romfs_files "")
    foreach(_tl_media IN ITEMS "RTShaderLib|RTShaderLib" "Main|RTShaderLib/Main")
        string(REPLACE "|" ";" _tl_pair "${_tl_media}")
        list(GET _tl_pair 0 _tl_from)
        list(GET _tl_pair 1 _tl_to)
        file(GLOB _tl_media_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/external/ogre/Media/${_tl_from}/*")
        foreach(_tl_src IN LISTS _tl_media_files)
            if(IS_DIRECTORY "${_tl_src}")
                continue()
            endif()
            get_filename_component(_tl_name "${_tl_src}" NAME)
            set(_tl_dst "${_tl_romfs}/${_tl_to}/${_tl_name}")
            add_custom_command(OUTPUT "${_tl_dst}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_tl_src}" "${_tl_dst}"
                DEPENDS "${_tl_src}" VERBATIM)
            list(APPEND _tl_romfs_files "${_tl_dst}")
        endforeach()
    endforeach()
    # The port's own interface pack, `romfs:/ui_pack`: what a PC pak is drawn
    # with when no `console_ui/` is beside it (main.cpp). Made into the RomFS by
    # every build (cmake/ui_pack.cmake); the .nro waits for it.
    tl_add_ui_pack(${target} "${_tl_romfs}/ui_pack")
    list(APPEND _tl_romfs_files "${${target}_UI_PACK_STAMP}")
    add_custom_target(${target}_romfs_files DEPENDS ${_tl_romfs_files})
    dkp_add_asset_target(${target}_romfs "${_tl_romfs}")
    set_property(TARGET ${target}_romfs APPEND PROPERTY DKP_ASSET_FILES ${_tl_romfs_files})
    add_dependencies(${target}_romfs ${target}_romfs_files)

    set(TL_NRO_ICON "${CMAKE_SOURCE_DIR}/src/logo_icon.jpg")
    if(EXISTS "${TL_NRO_ICON}")
        message(STATUS "Torchlight: .nro icon from src/logo_icon.jpg")
        nx_create_nro(${target} NACP ${target}.nacp ICON "${TL_NRO_ICON}" ROMFS ${target}_romfs)
    else()
        message(STATUS
            "Torchlight: no src/logo_icon.jpg - using libnx's default .nro icon. "
            "Run python scripts/make_icon.py to make it from src/logo.jpg")
        nx_create_nro(${target} NACP ${target}.nacp ROMFS ${target}_romfs)
    endif()

    if(NOT TL_HAVE_OGRE)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE TL_HAVE_OGRE=1)
    if(TL_SWITCH_GL3PLUS)
        target_compile_definitions(${target} PRIVATE TL_SWITCH_GL3PLUS=1)
        target_include_directories(${target} SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GL3Plus/include")
    endif()

    # Switches OGRE's headers from dllimport to static linkage.
    target_compile_definitions(${target} PRIVATE OGRE_STATIC_LIB)

    # RTShaderSystem reads its GLSL library, and the generated shaders include
    # OgreUnifiedShader.h, from OGRE's Media directory at runtime. There is no
    # source tree on the console, so the .nro carries them in its RomFS (above):
    # `romfs:/RTShaderLib`, OGRE's `Main/` inside it. --ogre-media overrides this.
    target_compile_definitions(${target} PRIVATE
        TL_OGRE_MEDIA_DIR="romfs:/")

    # devkitPro's toolchain puts only libnx on the include path, not portlibs.
    target_include_directories(${target} SYSTEM PRIVATE
        "${DKP_PORTLIBS}/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/Components/Overlay/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/OgreMain/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GLES2/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GLSupport/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/Components/RTShaderSystem/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/PlugIns/STBICodec/include"
        "${TL_OGRE_BUILD_DIR}/include"
    )

    # OGRE needs both; devkitPro's own Makefile template disables them.
    target_compile_options(${target} PRIVATE -fexceptions -frtti)

    target_link_directories(${target} PRIVATE "${TL_OGRE_BUILD_DIR}/lib")
    target_link_libraries(${target} PRIVATE
        -Wl,--start-group ${TL_OGRE_LIBS} -Wl,--end-group
        # FreeType and its own dependencies, for the interface layer's font -
        # the game's own `Torchlight Regular.ttf`. Grouped because harfbuzz and
        # freetype call into each other.
        -Wl,--start-group freetype harfbuzz png bz2 -Wl,--end-group
        ${TL_MESA_LIBS} ${TL_MESA_DRM} z m
    )

    # The console sampler records every thread the process makes
    # (platform_profiler.cpp's __wrap_threadCreate), Mesa's among them.
    target_link_options(${target} PRIVATE -Wl,--wrap=threadCreate)
    # Where the GL driver waits on the GPU (platform_gpu_waits.cpp). Mesa 26 has
    # no libdrm_nouveau to wrap.
    if(NOT TL_MESA26_LIB_DIR)
        target_link_options(${target} PRIVATE
            -Wl,--wrap=nouveau_bo_map -Wl,--wrap=nouveau_bo_wait -Wl,--wrap=nouveau_fence_wait
            -Wl,--wrap=nouveau_pushbuf_kick -Wl,--wrap=nouveau_pushbuf_space -Wl,--wrap=nvFenceWait
            -Wl,--wrap=nwindowDequeueBuffer -Wl,--wrap=nwindowQueueBuffer -Wl,--wrap=condvarWaitTimeout)
    endif()
    tl_perf_configure_executable(${target})
endfunction()
