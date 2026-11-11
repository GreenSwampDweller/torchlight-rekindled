"""The licence texts and notices a player folder must carry to be handed on.

Used by package_windows.py and package_linux.py. Into the folder it writes:

  LICENSE.txt         the engine's licence (GPL-3.0), the repository's LICENSE
  README.txt          what the folder is, where its exact source is, and every
                      third-party component in it with its licence
  licenses/<name>/    each component's own licence files:
                        ogre/             OGRE's LICENSE and Docs/licenses (MIT;
                                          what OGRE itself embeds)
                        particleuniverse/ its LICENSE (MIT)
                        stb_vorbis/       the licence block of stb_vorbis.c
                        <msys2 package>/  for each DLL beside the program, the
                                          files its MSYS2 package installs under
                                          share/licenses/

The DLLs are MSYS2's builds, unmodified. Several are LGPL-2.1, so README.txt names each package's exact source
archive on repo.msys2.org; a release should attach those archives beside the
zip, which is the surest way to meet the LGPL's source requirement.

write_notices() returns the problems that make the folder unfit to publish (no
LICENSE, a tree with uncommitted changes, a checkout that is not the public
repository); the packaging scripts print them.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parent.parent
PUBLIC_REPO = "https://github.com/GreenSwampDweller/torchlight-rekindled"
MSYS2_SOURCES = "https://repo.msys2.org/mingw/sources"


@dataclass
class Msys2Package:
    name: str      # mingw-w64-x86_64-glib2
    base: str      # mingw-w64-glib2, which names the source archive
    version: str   # 2.86.3-1
    licence: str   # spdx:LGPL-2.1-or-later
    licence_files: list[str]  # relative to the MSYS2 root
    dlls: list[str]

    @property
    def short(self) -> str:
        return self.name.removeprefix("mingw-w64-x86_64-")

    @property
    def source_url(self) -> str:
        return f"{MSYS2_SOURCES}/{self.base}-{self.version}.src.tar.zst"


def _git(*args: str, cwd: pathlib.Path = ROOT) -> str:
    try:
        return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def _section(desc: list[str], key: str) -> list[str]:
    if key not in desc:
        return []
    values = desc[desc.index(key) + 1:]
    return values[:values.index("")] if "" in values else values


def msys2_packages(dlls: list[pathlib.Path], msys_root: pathlib.Path) -> list[Msys2Package]:
    """The MSYS2 packages that own `dlls`, read from pacman's local database."""
    wanted = {dll.resolve().relative_to(msys_root.resolve()).as_posix().lower(): dll.name
              for dll in dlls}
    found: list[Msys2Package] = []
    owned: set[str] = set()
    for entry in sorted((msys_root / "var" / "lib" / "pacman" / "local").iterdir()):
        files_path, desc_path = entry / "files", entry / "desc"
        if not files_path.is_file() or not desc_path.is_file():
            continue
        files = files_path.read_text(encoding="utf-8").splitlines()
        mine = [wanted[f.lower()] for f in files if f.lower() in wanted]
        if not mine:
            continue
        desc = desc_path.read_text(encoding="utf-8").splitlines()
        found.append(Msys2Package(
            name=_section(desc, "%NAME%")[0],
            base=(_section(desc, "%BASE%") or _section(desc, "%NAME%"))[0],
            version=_section(desc, "%VERSION%")[0],
            licence=" AND ".join(_section(desc, "%LICENSE%")),
            licence_files=[f for f in files
                           if "/share/licenses/" in f and not f.endswith("/")],
            dlls=sorted(mine)))
        owned.update(m.lower() for m in mine)
    missing = sorted(name for name in wanted.values() if name.lower() not in owned)
    if missing:
        raise SystemExit(f"no MSYS2 package owns {', '.join(missing)}: its licence is unknown")
    return found


def _stb_vorbis_licence() -> str:
    source = (ROOT / "external" / "stb" / "stb_vorbis.c").read_text(encoding="utf-8", errors="replace")
    start = source.rfind("This software is available under 2 licenses")
    if start < 0:
        raise SystemExit("external/stb/stb_vorbis.c has no licence block (run scripts/fetch_stb.py)")
    return source[start:].rstrip().removesuffix("*/").rstrip() + "\n"


def _copy_files(files: list[pathlib.Path], dest: pathlib.Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.copy2(f, dest / f.name)


def _component(name: str, licence: str, folder: str) -> list[str]:
    return [f"  {name}".ljust(44) + f"licenses/{folder}/", f"      {licence}"]


def write_notices(out: pathlib.Path, platform: str, dlls: list[pathlib.Path] | None = None,
                  msys_root: pathlib.Path | None = None) -> list[str]:
    problems: list[str] = []
    licenses = out / "licenses"
    shutil.rmtree(licenses, ignore_errors=True)

    # The repository's LICENSE: at the root of the public repo, under public/ in its source.
    licence = next((p for p in (ROOT / "LICENSE", ROOT / "public" / "LICENSE") if p.is_file()),
                   ROOT / "LICENSE")
    if licence.is_file():
        shutil.copy2(licence, out / "LICENSE.txt")
    else:
        problems.append("no LICENSE at the repository root: the folder carries no GPL-3.0 text")

    ogre = ROOT / "external" / "ogre"
    _copy_files([ogre / "LICENSE", *sorted((ogre / "Docs" / "licenses").glob("*.txt"))],
                licenses / "ogre")
    pu = ROOT / "external" / "particleuniverse"
    _copy_files([pu / "LICENSE"], licenses / "particleuniverse")
    (licenses / "stb_vorbis").mkdir(parents=True, exist_ok=True)
    (licenses / "stb_vorbis" / "LICENSE.txt").write_text(_stb_vorbis_licence(), encoding="utf-8")

    packages = msys2_packages(dlls, msys_root) if dlls else []
    for package in packages:
        dest = licenses / package.short
        for rel in package.licence_files:
            # Kept below the package's own licence directory, so intl/COPYING.LIB
            # and libasprintf/COPYING.LIB do not overwrite each other.
            inner = rel.split("/share/licenses/", 1)[1].split("/", 1)[1]
            target = dest / inner
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(msys_root / rel, target)

    commit = _git("rev-parse", "HEAD")
    dirty = bool(_git("status", "--porcelain", "--untracked-files=no"))
    origin = _git("remote", "get-url", "origin")
    if not commit:
        problems.append("not a git checkout: README.txt cannot name the source revision")
    if dirty:
        problems.append("the tree has uncommitted changes: the build matches no published revision")
    if "torchlight-rekindled" not in origin:
        problems.append(f"not built from the public repository ({PUBLIC_REPO}): "
                        "the revision named in README.txt is not published")

    def version(path: pathlib.Path) -> str:
        return _git("describe", "--tags", "--always", cwd=path)

    lines = [
        f"Torchlight Rekindled - {platform} build",
        "",
        "An engine that runs Torchlight (Runic Games, 2009). No game data is included:",
        "put your own copy's pak.zip and music/ beside the program.",
        "",
        "This is a fan project, not affiliated with or endorsed by Runic Games or the",
        "owners of the Torchlight trademark.",
        "",
        "LICENCE",
        "",
        "The engine is free software under the GNU General Public License, version 3",
        "(LICENSE.txt). Its complete source code, including the build scripts and the",
        "changes made to OGRE and ParticleUniverse, is at",
        "",
        f"  {PUBLIC_REPO}",
        "",
        f"  revision {commit or 'unknown'}" + (" (with uncommitted changes)" if dirty else ""),
        f"  {PUBLIC_REPO}/tree/{commit}" if commit else "",
        "",
        "THIRD-PARTY COMPONENTS",
        "",
        "Built into the program (name, licence, where its licence text is):",
        "",
        *_component(f"OGRE {version(ogre)}", "MIT; what OGRE embeds, licenses/ogre/*.txt", "ogre"),
        *_component(f"ParticleUniverse {version(pu)}", "MIT", "particleuniverse"),
        *_component("stb_vorbis", "MIT or public domain", "stb_vorbis"),
    ]
    if packages:
        lines += [
            "",
            "Beside the program, as DLLs (MSYS2's builds, unmodified):",
            "",
        ]
        for package in packages:
            lines += _component(f"{package.short} {package.version}",
                                package.licence.replace("spdx:", ""), package.short)
            lines.append(f"      {', '.join(package.dlls)}")
        lines += [
            "",
            "SOURCE CODE OF THE DLLS",
            "",
            "Each DLL is the unmodified build of an MSYS2 package. The exact source of each,",
            "with the recipe it was built by, is:",
            "",
            *[f"  {package.source_url}" for package in packages],
            "",
            "You may replace any of these DLLs with your own build of the same library,",
            "including a modified one.",
        ]
        lgpl = [package.short for package in packages if "LGPL" in package.licence]
        if lgpl:
            lines[-2:-2] = ["Under the GNU Lesser General Public License: " + ", ".join(lgpl) + "."]
    # open(), not write_text(newline=), which is Python 3.10: the Linux release image has 3.9.
    with open(out / "README.txt", "w", encoding="utf-8",
              newline="\r\n" if platform == "Windows" else "\n") as fh:
        fh.write("\n".join(lines).replace("\n\n\n", "\n\n") + "\n")
    return problems


def report(problems: list[str]) -> None:
    if not problems:
        print("licences and notices: LICENSE.txt, README.txt, licenses/")
        return
    print("\nNOT READY TO PUBLISH - the folder runs, but:")
    for problem in problems:
        print(f"  - {problem}")
