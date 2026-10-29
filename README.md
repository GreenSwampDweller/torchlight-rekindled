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
| `mods_menu` | `1` | the Mods page before the game (not on iOS); `0` skips it and starts without any mods |
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
├── mods/               PC Torchlight mods, one folder each (optional)
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
├── mods/               PC Torchlight mods, one folder each (optional)
└── config.txt          made on first start
```

The launcher remembers the folder and checks it on every start. If `pak.zip` is
missing it asks again, and if `music/` is missing it offers to play without
music. Before each start it shows two sliders: the resolution, 25% to 100%
of the screen's (the system scales the frame up to the screen), and the
frame limit, `config.txt`'s `fps_limit` (no limit at either end); and the
**Mod menu** switch (`mods_menu`, see [Mods](#mods) below). Android 12
(API 31) or newer is required, on an arm64 device.

### iOS

The Xcode app bundle contains the engine and OGRE's shader library. If game data
is bundled with a local build, the app copies `pak.zip`, `music/`, and
`console_ui/` into its Documents folder on first launch. Otherwise, in Files,
open **On My iPhone/iPad → Torchlight Rekindled** and copy those folders there.
The app reads and writes its saves and settings in that same Documents folder.
Touch controls appear on screen when first touched.
On iOS, the right stick rotates the camera horizontally and adjusts its pitch vertically; pinch
with two fingers anywhere in the play area to zoom. On the other platforms the camera is the console's
and the right stick zooms. The **Free Camera** mod (`mods/FreeCamera/`, which the game makes
on its first start: on for iOS, off elsewhere, or as an older `config.txt`'s `free_camera` line
said) turns on the free camera on any platform: the right stick across turns it, a pinch
zooms on a touch screen (iOS, Android, the Switch in handheld), and R3 puts the view back
(otherwise R3 still toggles the map). Switch it on the Mods page (below), or in
`mods/mods.dat` (`PRIORITY:-1` is off).

The iOS build supports unpacked mods. Put each extracted mod in its own
subfolder under `Documents/mods/`, keeping the mod's `media/` directory
structure intact. Mod files can override game data, and town `Merge/*.layout`
files can add placed content such as merchants. Baklah's Unique Gear has been
verified in game, including his vendor interaction and inventory.

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
├── mods/               PC Torchlight mods, one folder each (optional)
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
├── mods/               PC Torchlight mods, one folder each (optional)
└── config.txt          made on first start
```

Run `./torchlight.sh`. The system's own libraries are not bundled: X11,
OpenGL/EGL, ALSA, FreeType and zlib. A gamepad is read through evdev. If the
pad is not picked up, add your user to the `input` group.

### Mods

A mod is a PC mod's folder (the one holding its `media/`) dropped into `mods/`,
found without anything else to do. On the Switch, Windows, Linux and Android
the game opens on a **Mods** page before the main menu. On the left, one row
switches all the mods, then there is a row for each mod; the list scrolls when
there are more than fit, and up and down go round it. The highlighted row's
description and author are shown on the right, and under them a **Don't show
this menu again** checkbox, which only right reaches (left goes back). A
switches the highlighted row or ticks the checkbox; Start (or B) goes on to the
main menu. The choice is kept in `mods/mods.dat`, as the PC game keeps it. A
mod's `music/` stands in for the game's. Two mods are the engine's own, made on
the first start: the free camera, `mods/FreeCamera/`, and **FPS Overlay**,
`mods/FpsOverlay/`, the frame rate in a corner (on as an older `config.txt`'s
`fps` line said; **L3** still hides and shows it for the run).

Ticking **Don't show this menu again** sets `mods_menu=0` in `config.txt`: the
game then starts without the page and **without any mods**. Set it back to
`mods_menu=1` (on Android, the launcher's **Mod menu** switch) to bring both
back. iOS has no page and takes `mods/mods.dat` as it is.

## Building

How to build the engine for each platform (Windows, Linux, Nintendo Switch,
Android and iOS) is in [docs/building.md](docs/building.md).

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
