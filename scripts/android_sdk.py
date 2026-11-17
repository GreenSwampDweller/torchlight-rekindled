"""Where the Android SDK's pieces are on this machine, for build_apk.py and the deploy step.

The SDK is the one Android Studio uses, `%LOCALAPPDATA%\\Android\\Sdk`, or
`ANDROID_HOME` when set. Within it the newest of each package is taken: NDK,
CMake, build-tools, platform. The JDK is Android Studio's own (its `jbr`), which
apksigner and keytool need and nothing else on this machine provides.
"""
import os
import pathlib

PACKAGE = "io.github.greenswampdweller.rekindled"
ACTIVITY = "port.android.GameActivity"
ABI = "arm64-v8a"
MIN_SDK = 31
TARGET_SDK = 36

STUDIO = pathlib.Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Android" / "Android Studio"


def _version_key(path: pathlib.Path):
    parts = []
    for piece in path.name.replace("android-", "").replace("-", ".").split("."):
        parts.append(int(piece) if piece.isdigit() else -1)
    return parts


def _newest(directory: pathlib.Path) -> pathlib.Path:
    candidates = [p for p in directory.iterdir() if p.is_dir()] if directory.is_dir() else []
    if not candidates:
        raise SystemExit(f"nothing installed under {directory} - install it with Android Studio's SDK Manager")
    return max(candidates, key=_version_key)


def sdk() -> pathlib.Path:
    root = os.environ.get("ANDROID_HOME") or os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk")
    path = pathlib.Path(root)
    if not path.is_dir():
        raise SystemExit(f"no Android SDK at {path} - install Android Studio, or set ANDROID_HOME")
    return path


def ndk() -> pathlib.Path:
    env = os.environ.get("ANDROID_NDK_HOME")
    return pathlib.Path(env) if env else _newest(sdk() / "ndk")


def llvm_strip() -> pathlib.Path:
    return ndk() / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-strip.exe"


def cmake_bin() -> pathlib.Path:
    return _newest(sdk() / "cmake") / "bin"


def build_tools() -> pathlib.Path:
    return _newest(sdk() / "build-tools")


def android_jar() -> pathlib.Path:
    # Only the platform the APK targets: a newer preview would set what the
    # manifest may name, not what the phone runs.
    jar = sdk() / "platforms" / f"android-{TARGET_SDK}" / "android.jar"
    if not jar.is_file():
        raise SystemExit(f"no {jar} - install platforms/android-{TARGET_SDK}")
    return jar


def adb() -> pathlib.Path:
    return sdk() / "platform-tools" / "adb.exe"


def jdk() -> pathlib.Path:
    """Android Studio's own JDK (its `jbr`), or, where there is no Android Studio
    (a GitHub Actions runner, .github/workflows/release.yml), the one JAVA_HOME names."""
    jbr = STUDIO / "jbr"
    if not jbr.is_dir() and os.environ.get("JAVA_HOME"):
        return pathlib.Path(os.environ["JAVA_HOME"])
    return jbr


def java_env() -> dict:
    """The environment apksigner and keytool run in: that JDK first on PATH."""
    env = dict(os.environ)
    home = jdk()
    env["JAVA_HOME"] = str(home)
    env["PATH"] = str(home / "bin") + os.pathsep + env.get("PATH", "")
    return env
