#!/usr/bin/env python3
"""Put the Windows desktop build in a folder a player can run from.

    python scripts/package_windows.py
    python scripts/package_windows.py --pak torchlight-xbox/extracted/pak.zip \\
        --music torchlight-steam/music          # a copy to play on this machine

The folder - `build-desktop/package/Torchlight/` - holds:

  torchlight.exe      the desktop build (build it first)
  *.dll               the MSYS2 libraries it needs, followed through their own
                      imports: the C++ runtime, FreeType, zlib, ...
  RTShaderLib/        OGRE's shader library, `Main/` inside, as the devices
                      carry it (OGRE's own files, not the game's)
  ui_pack/            the port's own interface pack, which the build makes:
                      what a PC Pak.zip is drawn with (no file of the game's)
  LICENSE.txt, README.txt, licenses/
                      the licences of the engine and of everything in the
                      folder, and where their source is (package_notices.py)

and, from the player, `pak.zip` and `music/` beside them. Started there with
no arguments, the program reads the game from its own folder, opens the main
menu, and keeps `config.txt` (debug, logs, fps), saves and its log there
(main.cpp's player folder). Missing files are said in a message box.

The game's files are never part of the folder unless asked for with --pak and
--music, which copy the user's own for a run on this machine.
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

from package_notices import report, write_notices

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build-desktop" / "torchlight.exe"
# TL_MINGW_BIN: an MSYS2 installed elsewhere (the release workflow's runner).
MINGW_BIN = pathlib.Path(os.environ.get("TL_MINGW_BIN", "C:/msys64/mingw64/bin"))
OBJDUMP = MINGW_BIN / "objdump.exe"
MEDIA = ROOT / "external" / "ogre" / "Media"
OUT = ROOT / "build-desktop" / "package" / "Torchlight"


def imports(binary: pathlib.Path) -> list[str]:
    text = subprocess.run([str(OBJDUMP), "-p", str(binary)], capture_output=True, text=True).stdout
    return re.findall(r"DLL Name: (\S+)", text)


def dll_closure(exe: pathlib.Path) -> list[pathlib.Path]:
    """Every DLL `exe` needs that MSYS2 provides, through each one's own imports."""
    found: dict[str, pathlib.Path] = {}
    pending = [exe]
    while pending:
        for name in imports(pending.pop()):
            candidate = MINGW_BIN / name
            if name.lower() in found or not candidate.is_file():
                continue  # already taken, or the system's own (KERNEL32, OPENGL32, ...)
            found[name.lower()] = candidate
            pending.append(candidate)
    return sorted(found.values())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pak", help="also copy this pak.zip in (the user's own, for a local run)")
    ap.add_argument("--music", help="also copy this music/ folder in")
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()

    if not EXE.is_file():
        raise SystemExit(f"no {EXE}: build the desktop target first")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    shutil.copy2(EXE, out / EXE.name)
    dlls = dll_closure(EXE)
    for dll in dlls:
        shutil.copy2(dll, out / dll.name)

    shaders = out / "RTShaderLib"
    shutil.rmtree(shaders, ignore_errors=True)
    for source, dest in (("RTShaderLib", shaders), ("Main", shaders / "Main")):
        dest.mkdir(parents=True, exist_ok=True)
        for f in (MEDIA / source).iterdir():
            if f.is_file():
                shutil.copy2(f, dest / f.name)

    # The port's interface pack, which the build made beside the program
    # (cmake/ui_pack.cmake): what a PC pak is drawn with when no console_ui/ is beside it.
    pack = EXE.parent / "ui_pack"
    if not (pack / "media" / "UI").is_dir():
        raise SystemExit(f"no interface pack at {pack}: build the target again (CMake makes it)")
    shutil.rmtree(out / "ui_pack", ignore_errors=True)
    shutil.copytree(pack, out / "ui_pack")

    if args.pak:
        shutil.copy2(args.pak, out / "pak.zip")
    if args.music:
        shutil.rmtree(out / "music", ignore_errors=True)
        shutil.copytree(args.music, out / "music")

    problems = write_notices(out, "Windows", dlls, MINGW_BIN.parent.parent)

    print(f"{out}: {EXE.name}, {len(dlls)} DLL(s), RTShaderLib/, ui_pack/"
          + (", pak.zip" if (out / "pak.zip").is_file() or (out / "Pak.zip").is_file() else "")
          + (", music/" if (out / "music").is_dir() else ""))
    for dll in dlls:
        print(f"  {dll.name}")
    report(problems)
    return 0


if __name__ == "__main__":
    sys.exit(main())
