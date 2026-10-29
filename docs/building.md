# Building Torchlight Rekindled

How to build the engine from source for each platform. To play, you need
your own copy of the game too; see [Game data](../README.md#game-data) in the README.

- [Every platform](#every-platform)
- [Windows](#windows)
- [Linux](#linux)
- [Nintendo Switch](#nintendo-switch)
- [Android](#android)
- [iOS](#ios)

## Every platform

Every platform starts from the same checkout:

```bash
git clone --recurse-submodules https://github.com/GreenSwampDweller/torchlight-rekindled.git
cd torchlight-rekindled
python scripts/fetch_stb.py        # the Vorbis decoder (one header), once
python scripts/apply_patches.py    # patches external/ogre and external/particleuniverse
```

Each platform then builds OGRE once (`scripts/build_ogre.py`) and the engine
after it. **Ninja is required everywhere:** on Windows, GNU Make reads the `C:`
in paths as a rule separator and fails.

**Every build also needs Python 3 with [Pillow](https://pypi.org/project/pillow/)**
(`python -m pip install pillow`): it draws the port's own interface pack, which
every build carries (`cmake/ui_pack.cmake`). CMake looks for one on `PATH` and in
Windows' usual install folders, and stops at configure when none has Pillow;
`-DTL_PYTHON=<path to python>` names one. The Switch build uses a Windows
Python, since devkitPro's shell has none of its own.

## Windows

Requirements: [MSYS2](https://www.msys2.org/) with the MINGW64 toolchain.

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-freetype mingw-w64-x86_64-zlib mingw-w64-x86_64-python
```

`C:\msys64\mingw64\bin` must be on `PATH`, not only spelled out in the
commands. Without it the compiler cannot find its own DLLs, and the build fails
with no message.

```bash
python scripts/build_ogre.py --platform desktop
cmake -S . -B build-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-desktop
ctest --test-dir build-desktop          # optional: the unit tests
```

To run it straight from the build tree:

```bash
./build-desktop/torchlight.exe --game-data <folder with Pak.zip and music/>
./build-desktop/torchlight.exe --game-data <PC install> --console-ui console_ui
```

To make a player folder (`build-desktop/package/Torchlight/`, laid out as in
the README's [Windows](../README.md#windows) section):

```bash
python scripts/package_windows.py
python scripts/package_windows.py --pak <your pak.zip> --music <your music/>   # also copies your game files in
```

## Linux

Run this on Linux or in WSL. On Debian or Ubuntu the requirements are:

```bash
sudo apt install build-essential cmake ninja-build python3 \
    libx11-dev libxrandr-dev libgl-dev libegl-dev libasound2-dev \
    libfreetype-dev zlib1g-dev
```

```bash
python3 scripts/build_ogre.py --platform linux
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DTL_OGRE_BUILD_DIR=build-ogre-linux
cmake --build build-linux
python3 scripts/package_linux.py        # -> build-linux/package/Torchlight/
```

`package_linux.py` takes the same `--pak` and `--music` options as the Windows
script. The player folder is laid out as in the README's
[Linux](../README.md#linux) section.

## Nintendo Switch

Requirements: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`switch-dev` group and the `switch-mesa`, `switch-sdl2` and `switch-pkg-config`
portlibs. The Switch build uses **devkitPro's own CMake**, run in devkitPro's
MSYS2 shell on Windows. It is not interchangeable with MSYS2's.

**A patched Mesa** (Mesa with glthread, worth about 5 ms a frame on the
console) is linked from `build-mesa-switch/install`. Build it once, in WSL or
Linux with devkitPro installed: `python3 scripts/build_mesa.py`.

Without it the build still succeeds. It links devkitPro's stock Mesa and prints
a warning, and the game runs slower.

```bash
python scripts/build_ogre.py --platform switch
```

On Windows, in devkitPro's MSYS2 shell (it starts in your home folder, so `cd`
first):

```bash
cd /c/path/to/torchlight-rekindled
cmake -S . -B build-switch -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release
ninja -C build-switch
```

This produces `build-switch/torchlight.nro`. Copy it to
`sdmc:/switch/torchlight/` beside `pak.zip` and `music/`, by SD card or over
FTP (see the README's [Switch](../README.md#switch) section).

## Android

Requirements, on Windows: [Android Studio](https://developer.android.com/studio),
with the SDK Manager's **NDK (Side by side)**, **CMake** and an **Android SDK
Platform** installed into the default SDK folder (`%LOCALAPPDATA%\Android\Sdk`,
or set `ANDROID_HOME`). The newest installed version of each is used. Gradle is
not needed, and Android Studio's own JDK signs the APK. `build_apk.py` draws the
launcher icons with [Pillow](https://pypi.org/project/pillow/), so the Python
that runs it needs it: `pip install pillow` (or, for MSYS2's Python,
`pacman -S mingw-w64-x86_64-python-pillow`).

```bash
python scripts/build_ogre.py --platform android
python scripts/build_apk.py game                  # -> build-android/torchlight.apk
adb install -r build-android/torchlight.apk
```

The APK is signed with the standard Android debug key
(`~/.android/debug.keystore`, created if it is missing). Put the game files on
the device and choose their folder on first start (see the README's
[Android](../README.md#android) section).

## iOS

Build on macOS with Xcode (including the iOS SDK), CMake, Python 3 and Git
installed. Initialise the repository's submodules first, then fetch the small
header dependency and apply the pinned OGRE patches:

```bash
git submodule update --init --recursive
python3 scripts/fetch_stb.py
python3 scripts/apply_patches.py
python3 scripts/build_ogre.py --platform ios
cmake -S . -B build-ios -G Xcode \
  -DTL_OGRE_BUILD_DIR="$PWD/build-ogre-ios-xcode" \
  -DTL_IOS_GAME_DATA_DIR="" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build build-ios --config Release --target torchlight
```

`TL_IOS_GAME_DATA_DIR` is deliberately empty in this recipe: the resulting app
contains the engine, icon and built-in interface, but no copyrighted game data.
To build a local app that copies your own game files into Documents on first
launch, set it to a folder containing `pak.zip` and optionally `music/` and
`console_ui/`. Do not commit or distribute that data.

The unsigned app is `build-ios/Release-iphoneos/torchlight.app`. To make an
unsigned IPA for personal signing, package the app with `Payload/` at the ZIP
root:

```bash
mkdir -p build-ios/ipa/Payload
cp -R build-ios/Release-iphoneos/torchlight.app build-ios/ipa/Payload/
ditto -c -k --norsrc --keepParent build-ios/ipa/Payload build-ios/Torchlight-Rekindled-iOS.ipa
```

The IPA is unsigned. Sign it with your own Apple development certificate and
provisioning profile before installing it on a device. Select a development
team in Xcode if you prefer to build and install directly from Xcode.

For an Apple Silicon Simulator build, use
`python3 scripts/build_ogre.py --platform ios --ios-simulator` and set
`TL_OGRE_BUILD_DIR` to `build-ogre-ios-simulator-xcode` when configuring CMake.
