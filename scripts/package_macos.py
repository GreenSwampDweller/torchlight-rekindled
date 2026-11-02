#!/usr/bin/env python3
"""Package the Apple Silicon desktop build as a data-free macOS app bundle.

Build first with the macOS steps in README.md, then run:

    python3 scripts/package_macos.py

The bundle and uploadable zip are written under build-macos/package/.
Licensed game data is never copied into either artifact.
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXECUTABLE = ROOT / "build-macos" / "Release" / "torchlight"
MEDIA = ROOT / "external" / "ogre" / "Media"
ICON = ROOT / "src" / "platform" / "ios" / "Assets.xcassets" / "AppIcon.appiconset" / "AppIcon.png"
OUT = ROOT / "build-macos" / "package"
APP_NAME = "Torchlight Rekindled.app"


LSREGISTER = ("/System/Library/Frameworks/CoreServices.framework/Frameworks/"
              "LaunchServices.framework/Support/lsregister")


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def make_icns(destination: pathlib.Path) -> None:
    if not ICON.is_file():
        raise SystemExit(f"missing app icon: {ICON}")
    iconset = destination.with_suffix(".iconset")
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)
    for name, pixels in (
        ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32),
        ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
        ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256),
        ("icon_256x256.png", 256), ("icon_256x256@2x.png", 512),
        ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
    ):
        run("sips", "-z", str(pixels), str(pixels), str(ICON), "--out", str(iconset / name))
    run("iconutil", "-c", "icns", str(iconset), "-o", str(destination))
    shutil.rmtree(iconset)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=str(OUT), help="output directory (default: build-macos/package)")
    args = parser.parse_args()
    out = pathlib.Path(args.out).resolve()
    if not EXECUTABLE.is_file():
        raise SystemExit(f"missing {EXECUTABLE}; build the macOS Release target first")
    if not (MEDIA / "RTShaderLib").is_dir() or not (MEDIA / "Main").is_dir():
        raise SystemExit(f"missing OGRE shader assets under {MEDIA}")
    ui_pack = ROOT / "build-macos" / "ui_pack"
    if not (ui_pack / "media" / "UI").is_dir():
        raise SystemExit("missing built-in ui_pack; build the macOS target again")

    out.mkdir(parents=True, exist_ok=True)
    app = out / APP_NAME
    shutil.rmtree(app, ignore_errors=True)
    contents = app / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    macos.mkdir(parents=True)
    resources.mkdir(parents=True)
    shutil.copy2(EXECUTABLE, macos / "torchlight")
    shutil.copytree(ui_pack, macos / "ui_pack")
    ogre_media = resources / "OGRE" / "Media"
    shutil.copytree(MEDIA / "RTShaderLib", ogre_media / "RTShaderLib")
    shutil.copytree(MEDIA / "Main", ogre_media / "Main")
    make_icns(resources / "torchlight.icns")
    (contents / "Info.plist").write_text("""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>torchlight</string>
  <key>CFBundleIconFile</key><string>torchlight</string>
  <key>CFBundleIdentifier</key><string>com.nuxx5.torchlight-rekindled</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>Torchlight Rekindled</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
""", encoding="utf-8")
    (macos / "torchlight").chmod(0o755)
    archive = out / "Torchlight-Rekindled-macOS-arm64.zip"
    archive.unlink(missing_ok=True)
    # A bundle rebuilt in place keeps Finder's cached (missing) icon until Launch Services
    # reads it again.
    app.touch()
    subprocess.run([LSREGISTER, "-f", str(app)], check=False)
    run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(app), str(archive))
    print(f"App: {app}\nUploadable archive: {archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
