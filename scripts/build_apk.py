#!/usr/bin/env python3
"""Build a signed debug APK for the Android test build.

No Gradle. A NativeActivity APK is a manifest, the native library and a
signature, and four of the SDK's own tools make it:

  1. CMake with the NDK's android.toolchain.cmake builds lib<name>.so (Ninja,
     the copy the SDK ships beside its CMake);
  2. aapt2 links android/AndroidManifest.xml.in, filled in, against android.jar;
  3. the library is added under lib/arm64-v8a/, and zipalign aligns the lot;
  4. apksigner signs it with the debug key, ~/.android/debug.keystore - the key
     Android Studio uses too, made here with keytool when it is missing.

Usage:
    python scripts/build_apk.py game             # the engine: the whole tree -> build-android/
    python scripts/build_apk.py game --clean     # configure from scratch

The APK holds no game data: that is pushed beside it by the deploy step. It
does carry OGRE's shader library and the port's own interface pack (which the
native build makes, cmake/ui_pack.cmake), and the launcher unpacks both into the
app's files.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import zipfile

import android_sdk

ROOT = pathlib.Path(__file__).resolve().parent.parent

# What each build is made from: a CMake source tree, the target in it that is
# the activity's library, and how the launcher names it.
TARGETS = {
    "game": {
        "source": ROOT,
        "build": ROOT / "build-android",
        "lib": "torchlight",
        "cmake_target": "torchlight",
        "label": "Torchlight Rekindled",
        "version": "1.1.1",
    },
}

DEBUG_KEYSTORE = pathlib.Path.home() / ".android" / "debug.keystore"
# The debug key's well-known passwords, the same in every Android SDK.
DEBUG_PASS = "android"
DEBUG_ALIAS = "androiddebugkey"


def run(command, **kwargs) -> None:
    print("  " + " ".join(str(c) for c in command), flush=True)
    result = subprocess.run([str(c) for c in command], **kwargs)
    if result.returncode != 0:
        raise SystemExit(f"failed ({result.returncode}): {command[0]}")


def build_library(target: dict, clean: bool) -> pathlib.Path:
    build = target["build"] / "native"
    if clean and build.exists():
        shutil.rmtree(build)
    cmake_bin = android_sdk.cmake_bin()
    ndk = android_sdk.ndk()
    if not (build / "CMakeCache.txt").exists():
        run([
            cmake_bin / "cmake.exe", "-S", target["source"], "-B", build, "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={(cmake_bin / 'ninja.exe').as_posix()}",
            f"-DCMAKE_TOOLCHAIN_FILE={(ndk / 'build' / 'cmake' / 'android.toolchain.cmake').as_posix()}",
            f"-DANDROID_ABI={android_sdk.ABI}",
            f"-DANDROID_PLATFORM=android-{android_sdk.MIN_SDK}",
            "-DCMAKE_BUILD_TYPE=Release",
        ])
    command = [cmake_bin / "cmake.exe", "--build", build]
    if "cmake_target" in target:
        command += ["--target", target["cmake_target"]]
    run(command)
    library = build / f"lib{target['lib']}.so"
    if not library.is_file():
        raise SystemExit(f"the build made no {library}")
    return library


def write_manifest(target: dict, out: pathlib.Path) -> pathlib.Path:
    text = (ROOT / "android" / "AndroidManifest.xml.in").read_text(encoding="utf-8")
    for key, value in {"PACKAGE": android_sdk.PACKAGE, "LABEL": target["label"],
                       "LIB": target["lib"], "VERSION": target["version"]}.items():
        text = text.replace(f"@{key}@", value)
    manifest = out / "AndroidManifest.xml"
    manifest.write_text(text, encoding="utf-8")
    return manifest


def ensure_debug_key() -> None:
    if DEBUG_KEYSTORE.is_file():
        return
    DEBUG_KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    print(f"making the debug key: {DEBUG_KEYSTORE}")
    run([android_sdk.STUDIO / "jbr" / "bin" / "keytool.exe", "-genkeypair", "-keystore", DEBUG_KEYSTORE, "-storepass", DEBUG_PASS,
         "-alias", DEBUG_ALIAS, "-keypass", DEBUG_PASS, "-keyalg", "RSA", "-keysize", "2048",
         "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"],
        env=android_sdk.java_env())


# The launcher's icon: the .nro's own (src/logo_icon.jpg), at each density's
# launcher size.
ICON = ROOT / "src" / "logo_icon.jpg"
ICON_SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
# OGRE's shader library, as the .nro's RomFS lays it out (cmake/switch.cmake):
# RTShaderLib/ with Main/ inside. The launcher unpacks it into the app's files.
OGRE_MEDIA = ROOT / "external" / "ogre" / "Media"
JAVA_SOURCE = ROOT / "android" / "java"


def compile_icons(tools: pathlib.Path, out: pathlib.Path) -> list[pathlib.Path]:
    """The icon scaled for each density and compiled by aapt2, as `.flat` files to link."""
    from PIL import Image  # pip install pillow
    res = out / "res"
    flats = out / "res-flat"
    shutil.rmtree(flats, ignore_errors=True)
    flats.mkdir(parents=True)
    image = Image.open(ICON).convert("RGBA")
    for density, size in ICON_SIZES.items():
        folder = res / f"mipmap-{density}"
        folder.mkdir(parents=True, exist_ok=True)
        png = folder / "ic_launcher.png"
        image.resize((size, size), Image.LANCZOS).save(png)
        run([tools / "aapt2.exe", "compile", "-o", flats, png])
    return sorted(flats.glob("*.flat"))


def compile_java(tools: pathlib.Path, out: pathlib.Path) -> pathlib.Path:
    """The launcher activity (android/java), compiled and dexed: `classes.dex`."""
    classes = out / "classes"
    dex = out / "dex"
    for d in (classes, dex):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
    sources = sorted(JAVA_SOURCE.rglob("*.java"))
    env = android_sdk.java_env()
    jdk = android_sdk.STUDIO / "jbr" / "bin"
    # The lambdas' java.lang.invoke is not in android.jar; d8 desugars them, and
    # javac only needs the build-tools' stubs to compile them.
    boot = f"{android_sdk.android_jar()};{tools / 'core-lambda-stubs.jar'}"
    run([jdk / "javac.exe", "-encoding", "UTF-8", "-source", "8", "-target", "8", "-nowarn",
         "-Xlint:-options", "-bootclasspath", boot, "-d", classes, *sources], env=env)
    run([tools / "d8.bat", "--release", "--min-api", str(android_sdk.MIN_SDK),
         "--lib", android_sdk.android_jar(), "--output", dex, *sorted(classes.rglob("*.class"))], env=env)
    return dex / "classes.dex"


def stage_assets(out: pathlib.Path, ui_pack: pathlib.Path) -> pathlib.Path:
    """`assets/RTShaderLib/` (with `Main/`), OGRE's own files, and `assets/ui_pack/`,
    the port's interface pack, which the native build made (cmake/ui_pack.cmake):
    the interface a PC pak is drawn with when no console_ui/ is beside it.
    Neither is the game's."""
    assets = out / "assets"
    shutil.rmtree(assets, ignore_errors=True)
    for source, dest in (("RTShaderLib", "RTShaderLib"), ("Main", "RTShaderLib/Main")):
        (assets / dest).mkdir(parents=True, exist_ok=True)
        for f in (OGRE_MEDIA / source).iterdir():
            if f.is_file():
                shutil.copy2(f, assets / dest / f.name)
    if not (ui_pack / "media" / "UI").is_dir():
        raise SystemExit(f"no interface pack at {ui_pack}: the native build makes it")
    shutil.copytree(ui_pack, assets / "ui_pack")
    return assets


def package(target: dict, library: pathlib.Path) -> pathlib.Path:
    out = target["build"] / "apk"
    out.mkdir(parents=True, exist_ok=True)
    tools = android_sdk.build_tools()
    manifest = write_manifest(target, out)

    icons = compile_icons(tools, out)
    dex = compile_java(tools, out)
    assets = stage_assets(out, target["build"] / "native" / "ui_pack")

    linked = out / "linked.apk"
    run([tools / "aapt2.exe", "link", "-o", linked, "--manifest", manifest,
         "-I", android_sdk.android_jar(), "--debug-mode",
         "--min-sdk-version", str(android_sdk.MIN_SDK),
         "--target-sdk-version", str(android_sdk.TARGET_SDK),
         "-A", assets, *icons])

    # The APK carries the library without its symbols and debug information
    # (which also name the folders it was built in); the unstripped one stays
    # in the build directory for reading a crash.
    stripped = out / library.name
    run([android_sdk.llvm_strip(), "--strip-all", "-o", stripped, library])

    # No game data here, ever: the player's own pak.zip and music/ stay in a
    # folder the launcher asks for.
    with zipfile.ZipFile(linked, "a", compression=zipfile.ZIP_DEFLATED) as apk:
        apk.write(stripped, f"lib/{android_sdk.ABI}/{library.name}")
        apk.write(dex, "classes.dex")

    aligned = out / "aligned.apk"
    # -P 16: uncompressed native libraries on 16 KB pages, which Android 15 and
    # later may use; harmless for the deflated one here.
    run([tools / "zipalign.exe", "-P", "16", "-f", "4", linked, aligned])

    ensure_debug_key()
    final = target["build"] / f"{target['lib'].replace('_', '-')}.apk"
    env = android_sdk.java_env()
    run([tools / "apksigner.bat", "sign", "--ks", DEBUG_KEYSTORE, "--ks-pass", f"pass:{DEBUG_PASS}",
         "--key-pass", f"pass:{DEBUG_PASS}", "--ks-key-alias", DEBUG_ALIAS, "--out", final, aligned],
        env=env)
    run([tools / "apksigner.bat", "verify", final], env=env)
    return final


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("target", choices=sorted(TARGETS))
    ap.add_argument("--clean", action="store_true", help="configure the native build from scratch")
    args = ap.parse_args()

    target = TARGETS[args.target]
    print(f"building {args.target}: {target['source']}")
    library = build_library(target, args.clean)
    apk = package(target, library)
    print(f"\n{apk} ({apk.stat().st_size / 1024:.0f} KB), package {android_sdk.PACKAGE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
