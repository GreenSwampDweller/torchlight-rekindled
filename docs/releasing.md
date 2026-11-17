# Releasing

Releases are built by GitHub Actions from the tagged commit
(`.github/workflows/release.yml`), not on anyone's PC. Pushing a version tag
builds the four assets and attaches them to a **draft** release. You check the
draft and publish it yourself.

| Asset | Built on | By |
|---|---|---|
| `TorchlightRekindled-<version>-windows-x64.zip` | `windows-latest`, MSYS2 MINGW64 | `build_ogre.py`, CMake, `package_windows.py` |
| `TorchlightRekindled-<version>-linux-x64.zip` | Steam Runtime SDK container | `build_linux_release.py --zip` |
| `TorchlightRekindled-<version>.apk` | `windows-latest`, Android SDK, NDK r28c | `build_ogre.py`, `build_apk.py` |
| `torchlight.nro` | `devkitpro/devkita64` container | `build_mesa.py`, `build_ogre.py`, CMake |

No game data is part of any build. Before uploading, the release job checks
every archive for `pak.zip` and `music/`.

Every push and pull request also runs `.github/workflows/ci.yml`. It runs the
unit tests and builds the Linux engine against OGRE (cached between runs).
Tests that need game data skip by themselves.

## One-time setup: the Android signing key

Android installs an update over an installed app only if both are signed with
the **same key**. The APKs of 1.0.0 to 1.1.1 were signed on the maintainer's PC
with `%USERPROFILE%\.android\debug.keystore` (`build_apk.py`). That exact file
has to go into the repository's secrets. A new key, or the runner's own debug
key, would make every player uninstall the app before updating, and an
uninstall removes everything the app keeps in its own storage.

The key is never committed. `.gitignore` excludes `*.keystore`.

In **Settings > Secrets and variables > Actions** of the upstream repository:

| Secret | Value |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | the keystore file, base64-encoded |
| `ANDROID_KEYSTORE_PASSWORD` | the keystore's password (`android` for a debug keystore) |
| `ANDROID_KEY_ALIAS` | the key's alias (`androiddebugkey` for a debug keystore) |
| `ANDROID_KEY_PASSWORD` | optional; the key's own password, if it differs from the keystore's |

| Variable (not secret) | Value |
|---|---|
| `ANDROID_CERT_SHA256` | optional, recommended; the signing certificate's SHA-256 digest. With it set, a build signed by any other key fails. |

With the GitHub CLI, from the PC that holds the key (PowerShell):

```powershell
$ks = "$env:USERPROFILE\.android\debug.keystore"
[Convert]::ToBase64String([IO.File]::ReadAllBytes($ks)) | gh secret set ANDROID_KEYSTORE_BASE64 -R GreenSwampDweller/torchlight-rekindled
gh secret set ANDROID_KEYSTORE_PASSWORD -R GreenSwampDweller/torchlight-rekindled --body android
gh secret set ANDROID_KEY_ALIAS -R GreenSwampDweller/torchlight-rekindled --body androiddebugkey
```

To find the digest, run either command and copy the SHA-256 value:

```powershell
apksigner verify --print-certs TorchlightRekindled-1.1.1.apk          # "certificate SHA-256 digest"
keytool -list -v -keystore $ks -alias androiddebugkey -storepass android  # "SHA256:"
```

Then save it as the variable (colons and case don't matter):

```powershell
gh variable set ANDROID_CERT_SHA256 -R GreenSwampDweller/torchlight-rekindled --body <digest>
```

A debug keystore's password is public (`android`), so the keystore **file** is
what has to stay secret. Anyone with it can sign an APK that installs over
yours. Keep a backup of it somewhere safe as well: if it is lost, no future
APK can update the installed app.

Without `ANDROID_KEYSTORE_BASE64`, a tag build stops at the Android job and no
release is drafted. A build started by hand signs with a throwaway debug key
instead and says so.

## Cutting a release

1. Set the new version in the three places that carry it:
   - `scripts/build_apk.py`: `"version": "<version>"`
   - `cmake/switch.cmake`: `VERSION "<version>"` in `nx_generate_nacp`
   - `android/AndroidManifest.xml.in`: raise `android:versionCode` by one

   The release workflow checks the first two against the tag and fails if they
   differ. It warns if `versionCode` is no higher than the last release's, and
   fails if it is lower.
2. Commit that to `main` and push.
3. Tag the commit and push the tag:

   ```bash
   git tag 1.2.0
   git push upstream 1.2.0
   ```

   Only plain `X.Y.Z` tags start a release; `ios-1.0.2` and the like do not.
4. Follow the run under **Actions > Release**. It takes a while, because OGRE
   (and Mesa, for the Switch) are built from scratch for every platform. When
   it finishes, the draft is under **Releases**, with generated notes.
5. Download the assets, try them, edit the notes, and press **Publish**.

If a job fails, fix the cause, then either re-run the failed jobs from the run
page, or delete the tag and push it again. A re-run replaces the draft's
assets. A release that is already published is never changed by the workflow.

## Building without releasing

**Actions > Release > Run workflow** builds any tag, branch or commit the same
way. The four files are kept as the run's artifacts, and no release is made.
On a fork this is how to try the workflow without touching the real releases.
The button only appears once the workflow file is on the default branch.

## What it does not build

The macOS (`-macos-arm64.zip`) and iOS (`.ipa`) builds that 1.1.0 carried are
not part of the workflow. Add them as jobs on `macos-latest` if they come back.

The toolchains are pinned only where it is cheap to do so: the NDK version is
pinned. The MSYS2 packages, the Steam Runtime SDK image and the devkitPro
image are each the latest at build time, as they were on the maintainer's PC.
