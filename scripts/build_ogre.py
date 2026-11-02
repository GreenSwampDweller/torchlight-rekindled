#!/usr/bin/env python3
"""Build OGRE for desktop or Switch with the flags this project requires.

OGRE is built separately from the engine because it needs a long list of
non-obvious flags. Every one of them is there for a reason - do not trim this
list without finding out what it is.

Usage:
    python scripts/build_ogre.py --platform desktop
    python scripts/build_ogre.py --platform switch
    python scripts/build_ogre.py --platform switch --gl3plus   # the GL3Plus experiment
    python scripts/build_ogre.py --platform switch --variant lto      # link-time optimisation
    python scripts/build_ogre.py --platform switch --variant pgogen   # instrumented, for a profile
    python scripts/build_ogre.py --platform switch --variant pgo      # optimised with that profile
    python scripts/build_ogre.py --platform android  # the phone test build
    python scripts/build_ogre.py --platform ios
    python3 scripts/build_ogre.py --platform linux   # the Linux desktop build, run on Linux or in WSL
"""
import argparse
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OGRE = ROOT / "external" / "ogre"

# The Switch build has to run inside devkitPro's own MSYS2 login shell. Its
# Switch.cmake does `include(dkp-toolchain-common)` and finds Ninja through
# environment that only the login shell sets up; run from anywhere else it fails
# with "include could not find requested file: dkp-toolchain-common" followed by
# "CMake was unable to find a build program corresponding to Ninja".
#
# That shell has no Python, which is why this script drives it from outside
# rather than being run inside it, and it starts in $HOME, so the cd is
# required rather than decorative.
DEVKITPRO_BASH = pathlib.Path("C:/devkitPro/msys2/usr/bin/bash.exe")


def msys_path(p) -> str:
    """Rewrite a Windows path as the MSYS2 path devkitPro's CMake expects.

    The Switch build uses devkitPro's CMake, which is an MSYS2 binary: handed
    "C:\\src\\torchlight\\..." it does not recognise the drive letter, treats the
    whole thing as *relative*, and fails with

        The source directory "/c/src/torchlight/C:/src/torchlight/external/ogre"
        does not exist

    which reads like a missing directory rather than a path-format problem. The
    desktop and Switch builds need different CMake binaries; they are not
    interchangeable.
    """
    drive, rest = os.path.splitdrive(str(p))
    if drive:
        return "/" + drive[0].lower() + rest.replace("\\", "/")
    return str(p).replace("\\", "/")

# devkitPro's Switch.cmake sets these. We must APPEND to them, never replace:
# overwriting CMAKE_CXX_FLAGS silently drops -march and -D__SWITCH__ and the
# build then produces subtly wrong code. See ADR 0001, blocker 7.
SWITCH_TOOLCHAIN_FLAGS = (
    "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft "
    "-ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__"
)
SWITCH_EXTRA_FLAGS = "-D_GNU_SOURCE -I/opt/devkitpro/portlibs/switch/include"

COMMON = [
    "-DCMAKE_BUILD_TYPE=Release",
    "-DOGRE_BUILD_DEPENDENCIES=OFF",
    "-DOGRE_ENABLE_PRECOMPILED_HEADERS=OFF",   # PCH bakes in an unusable path
    "-DOGRE_BUILD_SAMPLES=OFF",
    "-DOGRE_BUILD_TOOLS=OFF",
    "-DOGRE_BUILD_TESTS=OFF",
    "-DOGRE_BUILD_PLUGIN_CG=OFF",              # Cg is dead and unused by us
    "-DOGRE_BUILD_COMPONENT_PYTHON=OFF",
    "-DOGRE_BUILD_COMPONENT_CSHARP=OFF",
    "-DOGRE_BUILD_COMPONENT_JAVA=OFF",
    "-DOGRE_BUILD_COMPONENT_BITES=OFF",
    "-DOGRE_CONFIG_THREADS=0",
    # Torchlight's archive is inconsistently cased and, worse, its internal
    # references do not match the filenames on disk: Alchemist.MESH asks OGRE
    # for "alchemist.skeleton" while the file ships as "Alchemist.SKELETON".
    # That lookup happens inside OGRE, so our own AssetIndex cannot intercept
    # it. STRICT=0 is the only setting that compiles in OGRE's case-insensitive
    # resource index (ZipArchive::isCaseSensitive() returns STRICT != 0, and the
    # index itself sits behind `#if !OGRE_RESOURCEMANAGER_STRICT`); at the
    # default of 2 the mesh loads silently unanimated. Upstream marks 0 as
    # deprecated - we are pinned at v14.5.2, so revisit only on an OGRE bump.
    "-DOGRE_RESOURCEMANAGER_STRICT=0",
    # Both targets link OGRE statically and register plugins with
    # installPlugin(). Switch has no dynamic loader at all, and building desktop
    # the same way keeps one plugin-registration path instead of two, so what is
    # verified on desktop is the code that actually runs on the console.
    "-DOGRE_STATIC=ON",
    "-DBUILD_SHARED_LIBS=OFF",
]

SWITCH_ONLY = [
    "-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake",
    # OGRE's platform chain does not know about Switch, so these are never set
    # and configure dies. The guards are `if(NOT ...)`, so supplying them wins.
    "-DOGRE_MEDIA_PATH=share/OGRE/Media",
    "-DOGRE_PLUGINS_PATH=lib/OGRE",
    "-DOGRE_CFG_INSTALL_PATH=share/OGRE",
    "-DOGRE_BUILD_RENDERSYSTEM_GLES2=ON",
    "-DOGRE_BUILD_RENDERSYSTEM_GL=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_D3D9=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_D3D11=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_VULKAN=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_METAL=OFF",
    "-DOGRE_BUILD_PLUGIN_PFX=ON",
    # Torchlight's 1181 materials are all fixed-function and GLES2 has no
    # fixed-function pipeline, so RTSS is not optional for us.
    "-DOGRE_BUILD_COMPONENT_RTSHADERSYSTEM=ON",
    # Not OGRE_CONFIG_ENABLE_GL_STATE_CACHE_SUPPORT: on the console (2026-10-18)
    # it drew the HUD, the menu and some scenery black and lost NPCs, worse
    # after each level change - it keys texture state by GL name, and its VAO
    # tracking missed deletions. The one saving worth having, sampler
    # parameters re-sent on every texture bind, is patches/ogre/
    # 0020-switch-gles2-sampler-once-per-texture.patch instead, keyed by texture object.
    f'-DCMAKE_CXX_FLAGS={SWITCH_TOOLCHAIN_FLAGS} {SWITCH_EXTRA_FLAGS}',
    f'-DCMAKE_C_FLAGS={SWITCH_TOOLCHAIN_FLAGS} {SWITCH_EXTRA_FLAGS}',
]

DESKTOP_ONLY = [
    "-DOGRE_BUILD_RENDERSYSTEM_GLES2=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_D3D9=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_D3D11=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_VULKAN=OFF",
    # GL3Plus is the desktop counterpart to the console's GLES2: no fixed
    # function pipeline, so it forces Torchlight's fixed-function materials
    # through RTShaderSystem here rather than leaving that untested until M4.
    "-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=ON",
    # Legacy GL keeps a fixed-function fallback available for bisecting whether
    # a bad frame is RTSS's fault or ours.
    "-DOGRE_BUILD_RENDERSYSTEM_GL=ON",
    "-DOGRE_BUILD_COMPONENT_RTSHADERSYSTEM=ON",
    # Writes the PNG for --dump-frame, and reads the game's .png textures.
    "-DOGRE_BUILD_PLUGIN_STBI=ON",
    "-DOGRE_BUILD_PLUGIN_FREEIMAGE=OFF",
]

# The Android test build: the console's render system,
# GLES2 through RTSS, on the NDK's clang. Only what the renderer installs -
# GLES2, STBI, RTSS, Overlay - so none of the Switch's unused plugins and
# components. OGRE fetches and cross-builds FreeType (Overlay's font) and
# pugixml itself (OGRE_BUILD_DEPENDENCIES, which COMMON turns off for the
# other targets); the engine links that FreeType from build-ogre-android/
# Dependencies.
IOS_ONLY = [
    "-DOGRE_BUILD_DEPENDENCIES=ON",
    "-DOGRE_BUILD_RENDERSYSTEM_GLES2=ON",
    "-DOGRE_BUILD_RENDERSYSTEM_GL=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_VULKAN=OFF",
    "-DOGRE_BUILD_COMPONENT_RTSHADERSYSTEM=ON",
    "-DOGRE_BUILD_COMPONENT_OVERLAY=ON",
    "-DOGRE_BUILD_COMPONENT_TERRAIN=OFF",
    "-DOGRE_BUILD_COMPONENT_PAGING=OFF",
    "-DOGRE_BUILD_COMPONENT_VOLUME=OFF",
    "-DOGRE_BUILD_PLUGIN_STBI=ON",
    "-DOGRE_BUILD_PLUGIN_FREEIMAGE=OFF",
    "-DOGRE_BUILD_LIBS_AS_FRAMEWORKS=OFF",
    "-DOGRE_BUILD_PLUGIN_BSP=OFF", "-DOGRE_BUILD_PLUGIN_OCTREE=OFF",
    "-DOGRE_BUILD_PLUGIN_PFX=OFF", "-DOGRE_BUILD_PLUGIN_PCZ=OFF",
    "-DOGRE_BUILD_COMPONENT_BULLET=OFF", "-DOGRE_BUILD_COMPONENT_MESHLODGENERATOR=OFF",
    "-DOGRE_BUILD_COMPONENT_PROPERTY=OFF", "-DOGRE_BUILD_COMPONENT_OVERLAY_IMGUI=OFF",
]

MACOS_ONLY = [
    *DESKTOP_ONLY,
    "-DOGRE_BUILD_LIBS_AS_FRAMEWORKS=OFF",
    "-DCMAKE_OSX_ARCHITECTURES=arm64",
]

ANDROID_ONLY = [
    "-DOGRE_BUILD_DEPENDENCIES=ON",
    "-DOGRE_BUILD_RENDERSYSTEM_GLES2=ON",
    "-DOGRE_BUILD_RENDERSYSTEM_GL=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_VULKAN=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_TINY=OFF",
    "-DOGRE_BUILD_COMPONENT_RTSHADERSYSTEM=ON",
    "-DOGRE_BUILD_COMPONENT_OVERLAY=ON",
    "-DOGRE_BUILD_COMPONENT_TERRAIN=OFF",
    "-DOGRE_BUILD_COMPONENT_PAGING=OFF",
    "-DOGRE_BUILD_COMPONENT_VOLUME=OFF",
    "-DOGRE_BUILD_COMPONENT_PROPERTY=OFF",
    "-DOGRE_BUILD_COMPONENT_MESHLODGENERATOR=OFF",
    "-DOGRE_BUILD_COMPONENT_OVERLAY_IMGUI=OFF",
    "-DOGRE_BUILD_PLUGIN_STBI=ON",
    "-DOGRE_BUILD_PLUGIN_FREEIMAGE=OFF",
    "-DOGRE_BUILD_PLUGIN_ASSIMP=OFF",
    "-DOGRE_BUILD_PLUGIN_DOT_SCENE=OFF",
    "-DOGRE_BUILD_PLUGIN_EXRCODEC=OFF",
    "-DOGRE_BUILD_PLUGIN_GLSLANG=OFF",
    "-DOGRE_BUILD_PLUGIN_PFX=OFF",
    "-DOGRE_BUILD_PLUGIN_OCTREE=OFF",
    "-DOGRE_BUILD_PLUGIN_BSP=OFF",
    "-DOGRE_BUILD_PLUGIN_PCZ=OFF",
    "-DOGRE_BUILD_ANDROID_JNI_SAMPLE=OFF",
]


def android_toolchain() -> tuple:
    """The SDK's CMake and the NDK toolchain flags (scripts/android_sdk.py)."""
    import android_sdk
    cmake_bin = android_sdk.cmake_bin()
    flags = [
        f"-DCMAKE_TOOLCHAIN_FILE={(android_sdk.ndk() / 'build' / 'cmake' / 'android.toolchain.cmake').as_posix()}",
        f"-DCMAKE_MAKE_PROGRAM={(cmake_bin / 'ninja.exe').as_posix()}",
        f"-DANDROID_ABI={android_sdk.ABI}",
        f"-DANDROID_PLATFORM=android-{android_sdk.MIN_SDK}",
    ]
    return str(cmake_bin / "cmake.exe"), flags


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", choices=("desktop", "switch", "android", "ios", "linux", "macos"), required=True)
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--ios-simulator", action="store_true", help="build the iOS simulator arm64 variant")
    # Experiment: OGRE's GL3Plus render system on the console, on switch-mesa's
    # desktop GL instead of GLES. Built beside GLES2 into its own directory, so
    # the normal Switch OGRE is never touched (cmake -DTL_SWITCH_GL3PLUS=ON).
    # Both share OgreGLSupport, whose Switch EGL backend makes whichever context
    # the render system asks for (patch 0005); patch 0013 is GL3Plus's own part.
    ap.add_argument("--gl3plus", action="store_true",
                    help="switch only: also build GL3Plus, into build-ogre-switch-gl3")
    # Link-time and profile-guided optimisation (cmake/switch_perf.cmake, which
    # says what each does). Each is an OGRE of its own beside the ordinary one,
    # in build-ogre-switch-<variant>, and the engine configured the same way
    # (-DTL_LTO=ON, -DTL_PGO=generate, -DTL_PGO=use -DTL_LTO=ON) picks it up.
    ap.add_argument("--variant", choices=("lto", "pgogen", "pgo"),
                    help="switch only: build an optimisation variant into build-ogre-switch-<variant>")
    args = ap.parse_args()

    if not (OGRE / "CMakeLists.txt").exists():
        print("external/ogre is empty - run:")
        print("    git submodule update --init --recursive")
        return 1

    build = ROOT / f"build-ogre-{args.platform}"
    generator = "Xcode" if args.platform in ("ios", "macos") else "Ninja"
    if args.platform == "ios":
        if args.ios_simulator:
            build = ROOT / "build-ogre-ios-simulator-xcode"
        else:
            build = ROOT / "build-ogre-ios-xcode"
    elif args.ios_simulator:
        print("--ios-simulator requires --platform ios")
        return 1
    elif args.platform == "macos":
        build = ROOT / "build-ogre-macos-xcode"
    extra = {"switch": SWITCH_ONLY, "desktop": DESKTOP_ONLY, "android": ANDROID_ONLY, "ios": IOS_ONLY,
             "linux": DESKTOP_ONLY, "macos": MACOS_ONLY}[args.platform]
    cmake = "cmake"
    if args.platform == "android":
        cmake, toolchain = android_toolchain()
        extra = toolchain + extra
    elif args.platform == "ios":
        extra = [f"-DCMAKE_TOOLCHAIN_FILE={OGRE / 'CMake/toolchain/ios.toolchain.xcode.cmake'}",
                 f"-DIOS_PLATFORM={'SIMULATOR' if args.ios_simulator else 'OS'}",
                 "-DCMAKE_OSX_ARCHITECTURES=arm64",
                 "-DCMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET=15.0",
                 f"-DCMAKE_OBJC_COMPILER={subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()}",
                 f"-DCMAKE_OBJCXX_COMPILER={subprocess.check_output(['xcrun', '--find', 'clang++'], text=True).strip()}",
                 f"-DCMAKE_C_COMPILER={subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()}",
                 f"-DCMAKE_CXX_COMPILER={subprocess.check_output(['xcrun', '--find', 'clang++'], text=True).strip()}", *extra]
    if args.gl3plus:
        if args.platform != "switch":
            print("--gl3plus is for --platform switch (desktop always builds GL3Plus)")
            return 1
        build = ROOT / "build-ogre-switch-gl3"
        extra = [a for a in extra if not a.startswith("-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=")]
        extra.append("-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=ON")

    if args.variant:
        if args.platform != "switch" or args.gl3plus:
            print("--variant is for --platform switch, without --gl3plus")
            return 1
        build = ROOT / f"build-ogre-switch-{args.variant}"
        # OGRE's build knows nothing of these options: the engine's own module
        # is included at the end of its project() call and adds the flags to
        # every target defined after it, which is all of them.
        extra = extra + [
            f"-DCMAKE_PROJECT_INCLUDE={msys_path(ROOT / 'cmake' / 'switch_perf.cmake')}",
            *{"lto": ["-DTL_LTO=ON"],
              "pgogen": ["-DTL_PGO=generate"],
              "pgo": ["-DTL_PGO=use", "-DTL_LTO=ON"]}[args.variant],
        ]

    # Desktop uses mingw-w64's native CMake and wants native paths; Switch uses
    # devkitPro's MSYS2 CMake and wants POSIX ones. See msys_path().
    as_path = msys_path if args.platform == "switch" else str
    source_dir, build_dir = as_path(OGRE), as_path(build)

    # Ninja is mandatory: GNU Make reads the "C:" in Windows paths as a rule
    # separator and dies with "multiple target patterns".
    # Source paths in OGRE's objects relative to this checkout, as in the engine's.
    prefix_map = f"-DCMAKE_PROJECT_INCLUDE_BEFORE={as_path(ROOT / 'cmake' / 'file_prefix_map.cmake')}"
    cmd = [cmake, "-S", source_dir, "-B", build_dir, "-G", generator, *COMMON, *extra,
           prefix_map]

    def run(command) -> int:
        if args.platform != "switch":
            return subprocess.run(command).returncode
        line = " ".join(shlex.quote(a) for a in command)
        return subprocess.run(
            [str(DEVKITPRO_BASH), "-lc", f"cd {shlex.quote(msys_path(ROOT))} && {line}"]
        ).returncode

    print("configuring:\n  " + " \\\n  ".join(cmd) + "\n")
    if run(cmd) != 0:
        return 1

    build_cmd = [cmake, "--build", build_dir]
    if args.platform in ("ios", "macos"):
        build_cmd += ["--config", "Release"]
    if args.jobs:
        build_cmd += ["--parallel", str(args.jobs)]
    if run(build_cmd) != 0:
        return 1

    print(f"\nOGRE built into {build}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
