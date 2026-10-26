cmake_minimum_required(VERSION 3.13)
project(torchlight-switch CXX C)

# One tree, three targets, selected by toolchain file. devkitPro's Switch.cmake
# sets NINTENDO_SWITCH, the NDK's android.toolchain.cmake sets ANDROID (the phone
# test build); everything else is treated as desktop.
#
# Ninja is required on both. GNU Make parses the "C:" in Windows paths as a rule
# separator and fails with "multiple target patterns". See ADR 0001.

include(cmake/file_prefix_map.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NINTENDO_SWITCH)
    set(TL_PLATFORM switch)
elseif(ANDROID)
    set(TL_PLATFORM android)
elseif(APPLE_IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(TL_PLATFORM ios)
    # Apply the deployment floor to every static target as well as the app.
    set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0")
else()
    set(TL_PLATFORM desktop)
endif()
message(STATUS "Torchlight: building for ${TL_PLATFORM}")

# The port's own interface pack, which every build carries (cmake/ui_pack.cmake):
# the Python that makes it is found now, or the configure stops.
include(cmake/ui_pack.cmake)
tl_find_ui_pack_python()

# ---------------------------------------------------------------------------
# Warnings are errors on our own code, never on OGRE. Applied per-target via
# tl_set_warnings() rather than globally, so external/ogre is untouched.
# ---------------------------------------------------------------------------
function(tl_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wno-unused-parameter)
        if(APPLE)
            # Xcode enables this non-portable warning globally; existing code
            # intentionally passes container sizes into OGRE's uint APIs.
            target_compile_options(${target} PRIVATE -Wno-shorten-64-to-32)
        endif()
        # Not a warning, but the same per-target hook: transcribed arithmetic
        # must round the way the reference does. The Xbox 360 fuses a multiply
        # and an add only where its code says so (`fmadds` and the like), and
        # the port writes those as std::fma; GCC's default -ffp-contract=fast
        # would fuse others on the Switch's AArch64 by itself.
        target_compile_options(${target} PRIVATE -ffp-contract=off)
        # And std::fma has to be a true fused multiply-add. AArch64 always has
        # one; on x86-64 without -mfma it is mingw-w64's software fma, which is
        # not correctly rounded (the console's sine came out a unit off for -3,
        # 7.5 and pi/2, tests/runic_math_test.cpp). -mfma makes it the
        # processor's own instruction, and needs a CPU with FMA3 (2013 on).
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
            target_compile_options(${target} PRIVATE -mfma)
        endif()
    endif()
endfunction()

include(cmake/${TL_PLATFORM}.cmake)
include(cmake/particleuniverse.cmake)

# ---------------------------------------------------------------------------
# src/formats - the dependency-free parser library.
#
# This deliberately links nothing. Byte span in, struct out: no file I/O, no
# OGRE, no SDL, no logging. That is what makes it testable off-device and is a
# hard constraint, not a style preference. Do not add target_link_libraries here.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TL_FORMATS_SOURCES CONFIGURE_DEPENDS src/formats/*.cpp)
file(GLOB_RECURSE TL_FORMATS_HEADERS CONFIGURE_DEPENDS src/formats/*.h)

add_library(tl_formats STATIC ${TL_FORMATS_SOURCES} ${TL_FORMATS_HEADERS})
target_include_directories(tl_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
tl_set_warnings(tl_formats)

# ---------------------------------------------------------------------------
# stb_vorbis - the Ogg Vorbis decoder, vendored rather than committed.
#
# Its own library, and not part of tl_core, for one reason: tl_set_warnings()
# turns on -Wall -Wextra -Wpedantic -Werror, and third-party C does not survive
# that. Keeping it separate is what lets our own code stay at -Werror.
#
# Optional the same way OGRE is. `python scripts/fetch_stb.py` puts it there;
# without it the engine builds, runs, and says once that it has no sound.
# ---------------------------------------------------------------------------
set(TL_STB_VORBIS "${CMAKE_CURRENT_SOURCE_DIR}/external/stb/stb_vorbis.c")
if(EXISTS "${TL_STB_VORBIS}")
    add_library(tl_stb STATIC "${TL_STB_VORBIS}")
    target_include_directories(tl_stb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/stb")
    # STB_VORBIS_NO_STDIO drops the FILE* half of the decoder. Everything this
    # engine plays arrives as bytes already - an ambient loop comes out of
    # Pak.zip and the music out of a directory beside it - so the file API is
    # dead weight, and on the console it would be the only thing in the audio
    # path that knew what a path looked like.
    target_compile_definitions(tl_stb PRIVATE STB_VORBIS_NO_STDIO=1)
    set(TL_HAVE_VORBIS TRUE)
else()
    set(TL_HAVE_VORBIS FALSE)
    message(STATUS
        "No ${TL_STB_VORBIS} - building without sound. "
        "Run: python scripts/fetch_stb.py")
endif()

# ---------------------------------------------------------------------------
# tl_core - platform-agnostic engine code plus the platform glue for this
# target. A library rather than loose sources so the tests can link it.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TL_CORE_SOURCES     CONFIGURE_DEPENDS src/core/*.cpp)
file(GLOB_RECURSE TL_PLATFORM_SOURCES CONFIGURE_DEPENDS src/platform/${TL_PLATFORM}/*.cpp)

add_library(tl_core STATIC ${TL_CORE_SOURCES} ${TL_PLATFORM_SOURCES})
target_include_directories(tl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tl_core PUBLIC tl_formats)
tl_set_warnings(tl_core)

if(NINTENDO_SWITCH)
    target_compile_definitions(tl_core PUBLIC TL_PLATFORM_SWITCH=1 _GNU_SOURCE)
elseif(ANDROID)
    # The activity's glue (cmake/android.cmake) and AAudio, which platform/android uses.
    target_compile_definitions(tl_core PUBLIC TL_PLATFORM_ANDROID=1)
    target_link_libraries(tl_core PUBLIC tl_android_glue aaudio)
elseif(APPLE_IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(tl_core PUBLIC TL_PLATFORM_IOS=1)
    target_sources(tl_core PRIVATE src/platform/ios/platform_audio.mm)
else()
    target_compile_definitions(tl_core PUBLIC TL_PLATFORM_DESKTOP=1)
    # The desktop reads an Xbox pad through XInput 9.1.0, the version every
    # Windows ships (platform/desktop/platform_input.cpp). Desktop only: the
    # Switch has no dynamic loader and needs nothing like it.
    # The sound goes out through waveOut (platform/desktop/platform_audio.cpp),
    # so anything that links the mixer - the tests do - needs winmm too.
    if(WIN32)
        target_link_libraries(tl_core PUBLIC xinput9_1_0 winmm)
    else()
        # Linux: the keyboard through X11, the sound through ALSA (the pad is
        # evdev, the kernel's own, and needs no library).
        target_link_libraries(tl_core PUBLIC X11 asound)
    endif()
endif()

if(TL_HAVE_VORBIS)
    target_link_libraries(tl_core PUBLIC tl_stb)
    target_compile_definitions(tl_core PUBLIC TL_HAVE_VORBIS=1)
endif()

# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TL_RENDER_SOURCES CONFIGURE_DEPENDS src/render/*.cpp)

if(ANDROID)
    # What a NativeActivity loads: libtorchlight.so (cmake/android.cmake).
    add_library(torchlight SHARED src/main.cpp src/console_interface.cpp src/usage.cpp
        ${TL_RENDER_SOURCES})
elseif(APPLE_IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    set(TL_IOS_GAME_DATA_DIR "" CACHE PATH
        "Optional local game data directory (pak.zip, music/, console_ui/) to bundle in the iOS app")
    add_executable(torchlight MACOSX_BUNDLE src/main.cpp src/console_interface.cpp src/usage.cpp
        src/platform/ios/ios_app.mm ${TL_RENDER_SOURCES})
    set_target_properties(torchlight PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.torchlightrekindled.ios"
        MACOSX_BUNDLE_BUNDLE_NAME "Torchlight Rekindled"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.3"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.3"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.torchlightrekindled.ios"
        XCODE_ATTRIBUTE_INFOPLIST_KEY_UIFileSharingEnabled "YES"
        XCODE_ATTRIBUTE_INFOPLIST_KEY_LSSupportsOpeningDocumentsInPlace "YES")
    add_custom_command(TARGET torchlight POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/external/ogre/Media/RTShaderLib"
            "$<TARGET_BUNDLE_DIR:torchlight>/RTShaderLib"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/external/ogre/Media/Main"
            "$<TARGET_BUNDLE_DIR:torchlight>/RTShaderLib/Main"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/ui_pack"
            "$<TARGET_BUNDLE_DIR:torchlight>/ui_pack"
        COMMAND /usr/bin/xcrun actool
            --platform iphoneos
            --minimum-deployment-target 15.0
            --target-device iphone
            --target-device ipad
            --app-icon AppIcon
            --compile "$<TARGET_BUNDLE_DIR:torchlight>"
            --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/ios-icon-partial.plist"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Assets.xcassets"
        COMMAND /usr/libexec/PlistBuddy
            -c "Merge ${CMAKE_CURRENT_BINARY_DIR}/ios-icon-partial.plist :"
            "$<TARGET_BUNDLE_DIR:torchlight>/Info.plist")
    if(TL_IOS_GAME_DATA_DIR)
        add_custom_command(TARGET torchlight POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${TL_IOS_GAME_DATA_DIR}"
                "$<TARGET_BUNDLE_DIR:torchlight>/GameData")
    endif()
else()
    add_executable(torchlight src/main.cpp src/console_interface.cpp src/usage.cpp
        ${TL_RENDER_SOURCES})
endif()
target_include_directories(torchlight PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(torchlight PRIVATE tl_core)
tl_set_warnings(torchlight)

# ParticleUniverse ahead of tl_configure_platform: it calls into OgreMain, so on
# the link line it has to come before OGRE's --start-group, not after it.
if(TL_HAVE_PARTICLEUNIVERSE)
    target_link_libraries(torchlight PRIVATE tl_particleuniverse)
    target_compile_definitions(torchlight PRIVATE TL_HAVE_PARTICLEUNIVERSE=1)
endif()

tl_configure_platform(torchlight)   # defined in cmake/<platform>.cmake

# The interface pack beside the program: `ui_pack/` in the build tree, which the
# desktop packages copy and scripts/build_apk.py packs. The Switch's goes into
# its RomFS instead (cmake/switch.cmake).
if(NOT NINTENDO_SWITCH)
    tl_add_ui_pack(torchlight "${CMAKE_BINARY_DIR}/ui_pack")
endif()

# ---------------------------------------------------------------------------
# Desktop-only measurement build: every function of the port's own code counts
# its calls, for the parity report. Off by default and never on for the
# console - build it in a directory of its own (build-calls).
# ---------------------------------------------------------------------------
option(TL_CALL_COUNTS "count calls into the port's own functions (parity report)" OFF)
if(TL_CALL_COUNTS AND NOT NINTENDO_SWITCH)
    foreach(target tl_core torchlight)
        target_compile_options(${target} PRIVATE -g -finstrument-functions
            "-finstrument-functions-exclude-file-list=/include/,external/,msys64,call_counter.cpp")
        target_compile_definitions(${target} PRIVATE TL_CALL_COUNTS=1)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Desktop-only: tools and tests. The Switch and Android builds skip both.
# ---------------------------------------------------------------------------
if(NOT NINTENDO_SWITCH AND NOT ANDROID AND NOT (APPLE_IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    include(CTest)

    # The data checks and diagnostics; a source tree without them builds the same.
    # A list file, not merely the folder: an empty leftover folder is not the tools.
    set(_tl_tools ${CMAKE_SOURCE_DIR}/tools)
    if(EXISTS ${_tl_tools}/CMakeLists.txt)
        add_subdirectory(tools)
    endif()

    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
