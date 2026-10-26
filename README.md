<p align="center">
  <img src="src/logo_wide.png" alt="Torchlight Rekindled" width="300">
</p>

# Torchlight Rekindled

An engine that runs Torchlight (Runic Games, 2009) on a homebrew-enabled
Nintendo Switch, Android and desktop. Its game logic follows Runic's own
Xbox 360 port of the game (Xbox Live Arcade, 2011). It draws with OGRE 14 and
ParticleUniverse.

**No game data is included.** You need your own copy of the full game: any
purchased PC release (Steam, GOG, Epic, retail) or the Xbox 360 release. The
engine reads it at runtime.

This is a fan project. It is not affiliated with or endorsed by Runic Games or
the owners of the Torchlight trademark.

## Supported platforms

| platform | notes |
| --- | --- |
| Nintendo Switch | homebrew (hbmenu) |
| Android | arm64, Android 12 or newer |
| iOS | iPhone and iPad (unsigned build; sign before installing) |
| Windows | desktop build |
| Linux | desktop build |

**About the desktop builds:** I don't own a Steam Deck or any similar handheld
PC, so the Windows and Linux builds have only been tested on a laptop with a
gamepad connected. Reports from handheld PCs are welcome.

## Screenshots

**Nintendo Switch**: the town of Torchlight at night.

<img src="switch.jpg" alt="Torchlight Rekindled on the Nintendo Switch: the Destroyer and his wolf by the burning shrine in the town of Torchlight" width="100%">

**Android**: Orden Mines, floor 1, with the on-screen controls (a Galaxy Z Fold6, 120 fps).

<img src="android.jpg" alt="Torchlight Rekindled on Android: the Destroyer in the Orden Mines with the on-screen gamepad" width="100%">

## Why

The PC and Xbox 360 versions of Torchlight both received their last update in
2011. That is more than fifteen years without a patch.

The main motivation for this project is to make Torchlight properly playable on
today's portable devices. The PC version does run on handheld PCs like the
Steam Deck, but it was made for a mouse and keyboard: it has no real gamepad
support and no interface designed for a controller. The Xbox 360 version had
both, and that is the version this engine follows.

Runic Games, the studio behind Torchlight, no longer exists. The Torchlight IP
now belongs to another company whose focus is Torchlight Infinite. With no
sign that the original game will ever be revisited by the people who made it,
this project had to be made.

## Game data

The engine needs two things from your copy of the game:

| file | where it comes from |
| --- | --- |
| `pak.zip` | the game archive, from either release: the Xbox 360's `pak.zip` or a PC release's `Pak.zip` (from its install folder). See [PC or Xbox 360 game data](#pc-or-xbox-360-game-data) for which to use |
| `music/` | the music folder from your game install. The level themes are not inside the archive. The game runs without it, silently |

Either spelling, `pak.zip` or `Pak.zip`, is accepted.

## PC or Xbox 360 game data

Both releases' archives are supported, and the engine tells them apart by their
contents. There are three ways to play:

1. **The Xbox 360 `pak.zip`.** This is the reference: the engine follows the
   Xbox 360 release, and its archive carries the console's own HUD, menus and
   fonts. Nothing else is needed.

2. **A PC `Pak.zip` on its own.** It works on every platform, but a PC archive
   has no console interface: it carries the PC's mouse-driven one instead. The
   engine therefore draws the port's own interface pack, built in. Its art and
   layouts are drawn for this port by `scripts/build_ui_pack.py`, with nothing
   taken from the game. Every build makes it: the Switch `.nro` carries it
   inside, the Android APK carries it and unpacks it, and the Windows and Linux
   builds put it beside the program as `ui_pack/`. **This interface is basic
   and still under construction.**

3. **Hybrid: a PC `Pak.zip` with the Xbox 360 interface.** The game's assets
   come from the PC archive, and the HUD and menus from the Xbox release's
   `pak.zip` (ten files, about 4 MB), taken out with
   `scripts/extract_console_ui.py`:

   ```bash
   python scripts/extract_console_ui.py <path to the Xbox 360 pak.zip> console_ui
   ```

   The first argument is the Xbox archive and the second is the folder to
   write. The script will not write into the repository's source folders
   (`src/`, `scripts/`, `tests/`, ...). `console_ui/` at the repository root is
   ignored by git. Put `console_ui/` beside the PC `Pak.zip`, and it is used in
   place of the built-in pack. On Windows and Linux that is when the program
   runs from its own folder; from anywhere else, name it:
   `--console-ui console_ui` (a path relative to where the program runs, or an
   absolute one). Besides the console's own art and fonts, it brings what only
   the Xbox release has: the Chakawary pet, the three Xbox 360 armour sets and
   two fish.

With the Xbox 360 archive, `console_ui/` and the built-in pack are not used:
the archive already has the console interface. `--console-ui none` runs a PC
archive with neither: no HUD, and the menus as plain lists.

| | Xbox 360 `pak.zip` | PC `Pak.zip` | PC `Pak.zip` + `console_ui/` |
| --- | --- | --- | --- |
| HUD and menus | the console's own | the port's built-in pack: basic, under construction | the console's own |
| main menu and character creation | the console's | the built-in pack's | the console's |
| levels, monsters, items, quests | the Xbox 360's | the PC's | the PC's |
| monsters on a floor | fewer | about half as many again (on one late floor, 137 against 87) | as the PC's |
| Chakawary pet, Xbox 360 armour sets and fish | yes | no | yes |
| materials, lights and effects | the Xbox 360's | the PC's (a few props and effects look different) | the PC's |
| archive size | about 140 MB | about 320 MB (Steam) | about 320 MB, plus 4 MB |

**On lower-spec machines, such as the Switch, the Xbox 360 archive is the
better choice.** It places fewer monsters on a floor, and a crowd fight is what
costs a frame most: on the Switch, the same floor ran at about twice the frame
rate on the Xbox 360 data.

## Configuration: `config.txt`

`config.txt` holds the engine's own switches. These are not the game's
settings, which are kept with the saves (`save/settings.txt`). Each line is
`key=value`, where `1` is on and `0` is off, and `#` starts a comment.

| key | default | does |
| --- | --- | --- |
| `debug` | `0` | the debug tools: the debug menu, the bestiary, the wardrobe editor, Choose Level, frame dumps, and the log file. When off, the menus do not offer them at all |
| `logs` | `0` | the log file on its own (`debug=1` turns it on too) |
| `fps` | `0` | the frame-rate overlay. **L3** toggles it in game, and the change is saved here |
| `fps_limit` | `0` (Android `60`) | the most frames a second the game draws; `0` for no limit. On Android the display is asked for that rate too, so a 120 Hz screen can run at 60 |
| `width`, `height` | `1280`, `720` | **desktop builds only:** the window size, or the screen resolution in fullscreen |
| `fullscreen` | `0` | **desktop builds only:** `1` fills the screen at `width` x `height` |
| `vsync` | `1` | **desktop builds only:** `1` waits for the screen's refresh; `0` draws frames as fast as the machine can |

**Resolution is a desktop setting.** The Switch renders at its screen's
own size, Android at the share of it chosen in the launcher (below); both
ignore `width`, `height`, `fullscreen` and `vsync`. They do not write those lines into a new file.
If the screen cannot show `width` x `height` (the window is clamped to the
desktop, or the window manager makes a fullscreen window the monitor's size),
the game is laid out for the size the window really has.

Where the file lives:

- **Switch and Android:** beside the game data. It is created with the
  defaults above on first start.
- **Windows and Linux, run from a player folder** (a `pak.zip` in the working
  directory, as laid out below): in that folder, created on first start with
  the defaults above.
- **Desktop, run from the build tree** (with `--game-data`): read from the
  working directory if one is there, and never created. Without a file the
  debug tools and the log stay on.

On the desktop, `--width` and `--height` on the command line take precedence
over the file.

## Directory structure

Each platform keeps the game files in one folder: the program (or the APK's
chosen folder), `pak.zip`, `music/`, and optionally `console_ui/`. The game
writes `config.txt`, its saves and its log into that same folder.

### Switch

```
sdmc:/switch/torchlight/
├── torchlight.nro      the engine (OGRE's shader library is inside it)
├── pak.zip             the game archive
├── music/              the game's music
├── console_ui/         only beside a PC Pak.zip
└── config.txt          made on first start
```

Start it from the Homebrew Menu (hbmenu).

### Android

The APK holds the engine and OGRE's shader library, but no game files. On
first start it asks for **All files access**, then for a folder chosen through
the system's folder picker:

```
<any folder you choose, e.g. /sdcard/Torchlight/>
├── pak.zip
├── music/
├── console_ui/         only beside a PC Pak.zip
└── config.txt          made on first start
```

The launcher remembers the folder and checks it on every start. If `pak.zip` is
missing it asks again, and if `music/` is missing it offers to play without
music. Before each start it shows two sliders: the resolution, 25% to 100%
of the screen's (the system scales the frame up to the screen), and the
frame limit, `config.txt`'s `fps_limit` (no limit at either end); and two
switches, the frame-rate overlay (`fps`) and the free camera (`free_camera`). Android 12
(API 31) or newer is required, on an arm64 device.

### iOS

#### iOS 1.0.3 changes

- **Camera pitch setting is clearer:** the Options row now reads **Invert Camera Pitch: Off/On**. It reverses the vertical camera movement for the right stick and touch camera controls.
- **Cleaner touch HUD:** the redundant **Use** button has been removed. The **A** button continues to interact, confirm, equip, transfer, and buy.
- **Landscape release:** this build keeps the established landscape layout and controls.

Pet-to-town and shared-stash features are available in this source tree; they are
not new in this 1.0.3 update.

The Xcode app bundle contains the engine and OGRE's shader library. If game data
is bundled with a local build, the app copies `pak.zip`, `music/`, and
`console_ui/` into its Documents folder on first launch. Otherwise, in Files,
open **On My iPhone/iPad → Torchlight Rekindled** and copy those folders there.
The app reads and writes its saves and settings in that same Documents folder.
Character saves and the shared stash keep three rotating backups (`.bak1` through `.bak3`). If a save is damaged, the game restores the newest valid backup and keeps the damaged file alongside it for recovery.
Touch controls appear on screen when first touched.
On iOS, the right stick rotates the camera horizontally and adjusts its pitch vertically; pinch
with two fingers anywhere in the play area to zoom. On the other platforms the camera is the console's
and the right stick zooms. `free_camera=1` in `config.txt` (or **Debug > FREE CAMERA**, with
`debug=1`) turns on the free camera on any platform: the right stick across turns it, a pinch
zooms on a touch screen (iOS, Android, the Switch in handheld), and R3 puts the view back
(otherwise R3 still toggles the map). `free_camera=0` gives iOS the console's camera too.
On controller platforms, vertical stick still
zooms, and holding LB while moving it vertically adjusts pitch.
In Options, focus the Text Size row and press A to cycle through Text Size, Camera Speed,
Camera Tilt, Pinch Zoom, Invert Camera Pitch, Touch Size, Touch Opacity, Touch Style, Touch Layout,
Compact HUD, and Move Controls. Use left/right to change the selected value; the choices save with the other
settings. **Invert Camera Pitch** switches the vertical camera control between normal and reversed
for the right stick and touch camera controls. Touch Style offers Classic, Neon, and Solid button
designs. Touch Layout offers two-thumb, right-hand, and left-hand presets. The A button handles
interaction and confirmation. Enable Move Controls, close Options, then drag controls
to reposition them; each position saves when you lift your finger. In inventory screens, tap a
visible bag, pet, or vendor row to select it, then press A to equip, transfer, or buy.

iOS reads extended Bluetooth controllers through Apple's Game Controller framework. Pair the
controller in iOS Settings, then launch or resume Torchlight; connecting a controller hides the
touch overlay and game HUD to free screen space. Disconnecting returns the game to touch-first
controls.

The iOS build supports unpacked mods. Put each extracted mod in its own
subfolder under `Documents/mods/`, keeping the mod's `media/` directory
structure intact. Mod files can override game data, and town `Merge/*.layout`
files can add placed content such as merchants. Baklah's Unique Gear has been
verified in game, including his vendor interaction and inventory.

### Build iOS from source

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

### Windows

```
Torchlight/
├── torchlight.exe
├── *.dll               the MinGW runtime, FreeType, zlib, ... (copied by the packaging script)
├── RTShaderLib/        OGRE's shader library, with Main/ inside
├── LICENSE.txt, README.txt, licenses/   the licences of everything in the folder, and where its source is
├── pak.zip
├── music/
├── console_ui/         only beside a PC Pak.zip (run with --console-ui console_ui)
└── config.txt          made on first start
```

Started from this folder with no arguments, it reads the game from the folder
and opens the main menu. Missing files are reported in a message box.

### Linux

```
Torchlight/
├── torchlight
├── torchlight.sh       starts the program from its own folder
├── RTShaderLib/        OGRE's shader library, with Main/ inside
├── LICENSE.txt, README.txt, licenses/   the licences of everything in the folder, and where its source is
├── pak.zip
├── music/
├── console_ui/         only beside a PC Pak.zip (run with --console-ui console_ui)
└── config.txt          made on first start
```

Run `./torchlight.sh`. The system's own libraries are not bundled: X11,
OpenGL/EGL, ALSA, FreeType and zlib. A gamepad is read through evdev. If the
pad is not picked up, add your user to the `input` group.

## Building

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

### Windows (desktop)

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

To make a player folder (`build-desktop/package/Torchlight/`, laid out as
above):

```bash
python scripts/package_windows.py
python scripts/package_windows.py --pak <your pak.zip> --music <your music/>   # also copies your game files in
```

### Linux (desktop)

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
script.

### Nintendo Switch

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
FTP.

### Android

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
the device and choose their folder on first start (see
[Android](#android) above).

## Legal

Torchlight Rekindled is free software, released under the GNU General Public
License, version 3: see [LICENSE](LICENSE). The third-party components it
bundles or links, and their licences, are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

No part of the game's data is distributed with this project. To play, you need
your own legally obtained copy of Torchlight.

Torchlight is copyright © 2009 Runic Games, Inc. All rights reserved.
Torchlight and Runic Games are trademarks or registered trademarks of their
respective owners in the U.S. and/or other countries.

Torchlight Rekindled and its maintainers are not associated with or endorsed by
Runic Games or the current owner of the Torchlight trademark.
