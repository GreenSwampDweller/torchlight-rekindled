#!/usr/bin/env python3
"""Build the Linux release in Valve's Steam Runtime SDK, so it runs on glibc 2.31 and newer.

The Linux build is made here and nowhere else. A build made on the machine's
own distribution needs that distribution's glibc, and a player with an older
one cannot start it (`GLIBC_2.43' not found on SteamOS). Valve's Steam Runtime
"sniper" SDK is Debian 11 with glibc 2.31; built there with GCC 14 and the C++
runtime linked in, the program runs on any distribution with glibc 2.31 or
newer: SteamOS, Ubuntu 20.04, Debian 11, Fedora 32 and everything after them.

All of it runs in the container, as the calling user, on this checkout:

    1. OGRE, `build_ogre.py --platform linux` -> build-ogre-linux/;
    2. the engine -> build-linux/, configured as a player folder wants it
       (TL_OGRE_MEDIA_DIR=., the RTShaderLib/ beside the program);
    3. its tests;
    4. the player folder, made afresh, `package_linux.py` -> build-linux/package/Torchlight/
       (emptied first: a local run's `--pak` and `--music` copies never reach a release);
    5. a check that the program needs no glibc newer than 2.31 and no libstdc++.

With --zip it then writes that folder to a zip that keeps the run permission of
`torchlight` and `torchlight.sh` (a zip made on Windows loses it).

It needs Docker, on Linux or in WSL. The image is the SDK with Pillow added
(the interface pack, build_ui_pack.py), kept by Docker as `torchlight-linux-sdk`;
the first run downloads the SDK, about 5 GB unpacked. A build folder left by a
build outside the container is cleared of its CMake cache first.

Usage:
    python3 scripts/build_linux_release.py [--zip TorchlightRekindled-linux-x64.zip]
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = "registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest"
IMAGE = "torchlight-linux-sdk"
# Debian's pip, then Pillow from PyPI: build_ui_pack.py needs 10.1 or newer
# (ImageFont.load_default(size=)), and Debian 11 has 8.1. 11.3 is the last for Python 3.9.
DOCKERFILE = f"""FROM {SDK}
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip \\
 && python3 -m pip install --no-cache-dir pillow==11.3.0
"""
GLIBC = "2.31"
PACKAGE = ROOT / "build-linux" / "package"
PROGRAMS = {"Torchlight/torchlight", "Torchlight/torchlight.sh"}

INNER = f"""set -eu
export HOME=/tmp CC=gcc-14 CXX=g++-14
python3 scripts/build_ogre.py --platform linux
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \\
      -DTL_OGRE_BUILD_DIR=build-ogre-linux -DTL_OGRE_MEDIA_DIR=. -DTL_PYTHON=/usr/bin/python3 \\
      "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
rm -rf build-linux/package/Torchlight  # never a pak.zip or music/ left by a local run
python3 scripts/package_linux.py
program=build-linux/package/Torchlight/torchlight
need=$(objdump -T "$program" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
echo "the program needs $need at most"
if [ "$(printf '%s\\n' GLIBC_{GLIBC} "$need" | sort -V | tail -1)" != GLIBC_{GLIBC} ]; then
    echo "it needs a glibc newer than {GLIBC}" >&2; exit 1
fi
if objdump -p "$program" | grep -q 'NEEDED.*libstdc++'; then
    echo "it links libstdc++ dynamically" >&2; exit 1
fi
"""


def run(command, **kwargs) -> None:
    print("  " + " ".join(str(c) for c in command), flush=True)
    if subprocess.run([str(c) for c in command], **kwargs).returncode != 0:
        raise SystemExit(f"failed: {command[0]} {command[1]}")


def clear_foreign_cache(build: pathlib.Path) -> None:
    """A CMake cache made outside the container names other paths and another compiler."""
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_CACHEFILE_DIR:INTERNAL="):
            if line.split("=", 1)[1].startswith("/work/"):
                return
            break
    print(f"  {build.name}: a cache from a build outside the container, cleared")
    cache.unlink()
    shutil.rmtree(build / "CMakeFiles", ignore_errors=True)


def write_zip(out: pathlib.Path) -> None:
    folder = PACKAGE / "Torchlight"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(p for p in folder.rglob("*") if p.is_file()):
            name = path.relative_to(PACKAGE).as_posix()
            info = zipfile.ZipInfo(name, time.localtime(path.stat().st_mtime)[:6])
            info.create_system = 3  # Unix, so the mode below is read
            info.external_attr = (0o100755 if name in PROGRAMS else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, path.read_bytes())
    print(f"{out}: {len(z.namelist())} files, {out.stat().st_size // 1024} KB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zip", type=pathlib.Path, help="also write the player folder to this zip")
    args = ap.parse_args()
    if os.name == "nt" or not shutil.which("docker"):
        raise SystemExit("run this on Linux or in WSL, with Docker")

    print(f"image {IMAGE} ({SDK} with Pillow):")
    run(["docker", "build", "-t", IMAGE, "-"], input=DOCKERFILE.encode())
    for build in ("build-ogre-linux", "build-linux"):
        clear_foreign_cache(ROOT / build)
    print("build:")
    run(["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
         "-v", f"{ROOT}:/work", "-w", "/work", IMAGE, "bash", "-c", INNER])
    if args.zip:
        write_zip(args.zip.resolve())
    print(f"done: {PACKAGE / 'Torchlight'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
