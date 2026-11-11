#!/usr/bin/env python3
"""Put the Linux desktop build in a folder a player can run from (run on Linux or in WSL).

    python3 scripts/package_linux.py
    python3 scripts/package_linux.py --pak torchlight-xbox/extracted/pak.zip \\
        --music torchlight-steam/music          # a copy to play on this machine

The folder - `build-linux/package/Torchlight/` - holds:

  torchlight          the Linux desktop build (build-linux/, configured with
                      -DTL_OGRE_BUILD_DIR=build-ogre-linux)
  torchlight.sh       starts it from its own folder, which is where it reads the
                      game and keeps config.txt, saves and its log
  RTShaderLib/        OGRE's shader library, `Main/` inside (OGRE's own files)
  ui_pack/            the port's own interface pack, which the build makes:
                      what a PC Pak.zip is drawn with (no file of the game's)
  LICENSE.txt, README.txt, licenses/
                      the licences of the engine and of everything built into
                      it, and where its source is (package_notices.py)

and, from the player, `pak.zip` and `music/` beside them. The system's own
libraries are not copied: X11, OpenGL (or EGL), ALSA, FreeType and zlib, which
every desktop distribution has. The pad is read through evdev: a player's
pad is usually readable without asking; otherwise the user needs the `input` group.
"""
import argparse
import os
import pathlib
import shutil
import stat
import sys

from package_notices import report, write_notices

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = ROOT / "build-linux" / "torchlight"
MEDIA = ROOT / "external" / "ogre" / "Media"
OUT = ROOT / "build-linux" / "package" / "Torchlight"

LAUNCHER = """#!/bin/sh
# Starts the game from its own folder, where it reads pak.zip and music/ and keeps
# config.txt, its saves and its log.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
exec ./torchlight "$@"
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pak", help="also copy this pak.zip in (the user's own, for a local run)")
    ap.add_argument("--music", help="also copy this music/ folder in")
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()

    if not BINARY.is_file():
        raise SystemExit(f"no {BINARY}: build the Linux desktop target first")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    shutil.copy2(BINARY, out / BINARY.name)
    launcher = out / "torchlight.sh"
    with open(launcher, "w", encoding="utf-8", newline="\n") as fh:  # Python 3.9: the release image's
        fh.write(LAUNCHER)
    for f in (out / BINARY.name, launcher):
        f.chmod(f.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    shaders = out / "RTShaderLib"
    shutil.rmtree(shaders, ignore_errors=True)
    for source, dest in (("RTShaderLib", shaders), ("Main", shaders / "Main")):
        dest.mkdir(parents=True, exist_ok=True)
        for f in (MEDIA / source).iterdir():
            if f.is_file():
                shutil.copy2(f, dest / f.name)

    # The port's interface pack, which the build made beside the program
    # (cmake/ui_pack.cmake): what a PC pak is drawn with when no console_ui/ is beside it.
    pack = BINARY.parent / "ui_pack"
    if not (pack / "media" / "UI").is_dir():
        raise SystemExit(f"no interface pack at {pack}: build the target again (CMake makes it)")
    shutil.rmtree(out / "ui_pack", ignore_errors=True)
    shutil.copytree(pack, out / "ui_pack")

    if args.pak:
        shutil.copy2(args.pak, out / "pak.zip")
    if args.music:
        shutil.rmtree(out / "music", ignore_errors=True)
        shutil.copytree(args.music, out / "music")

    problems = write_notices(out, "Linux")

    print(f"{out}: torchlight, torchlight.sh, RTShaderLib/, ui_pack/"
          + (", pak.zip" if (out / "pak.zip").is_file() else "")
          + (", music/" if (out / "music").is_dir() else ""))
    report(problems)
    return 0


if __name__ == "__main__":
    sys.exit(main())
