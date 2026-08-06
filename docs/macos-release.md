# macOS DMG Release Guide

ACECode's `package` GitHub Actions workflow builds separate Intel (`macos-x64`)
and Apple silicon (`macos-arm64`) disk images. Tagged releases require a
Developer ID signature and Apple notarization; the workflow will fail instead
of publishing an unsigned DMG when any required credential is missing.

## What Users Install

Each DMG contains:

- `ACECode.app`
- `Install ACECode.app`
- `Install Instructions 安装说明.txt`

Opening a DMG only mounts it; macOS does not allow a DMG to silently run an
installer. The user double-clicks `Install ACECode.app`, which validates the
bundled ACECode application, copies it to
`~/Applications/ACECode.app`, and launches that installed copy.

The installer is deliberately current-user only:

- It never writes to the system `/Applications` directory.
- It never invokes `sudo`, Authorization Services, or an administrator prompt.
- It accepts only the exact normalized destination
  `~/Applications/ACECode.app`.
- It refuses symlinked or redirected application directories and destinations.
- It asks the user to close a running ACECode instance before replacement.

Users can remove ACECode by quitting it and deleting
`~/Applications/ACECode.app`. No privileged uninstaller is required.

## One-Time GitHub Setup

### 1. Export the Developer ID identity

In Keychain Access, open the login keychain and select **My Certificates**.
Expand the `Developer ID Application` certificate and confirm that its private
key is present. Export the identity as a password-protected `.p12` file. A
`Developer ID Installer` certificate is not required because ACECode ships a
signed app inside a DMG rather than an Apple installer package (`.pkg`).

Encode the exported file without line breaks:

```bash
openssl base64 -A -in /path/to/ACECode-Developer-ID.p12 | pbcopy
```

Do not commit the `.p12` file or its password. After the GitHub setup is
verified, move the export to secure offline storage or delete it.

### 2. Create an Apple app-specific password

Sign in at [Apple Account](https://account.apple.com/), open **Sign-In and
Security**, then create an app-specific password named for the ACECode release
workflow. The Apple Account must have two-factor authentication enabled.

### 3. Add repository secrets

Open **GitHub repository > Settings > Secrets and variables > Actions** and add
these repository secrets:

| Secret | Value |
| --- | --- |
| `MACOS_CERTIFICATE_BASE64` | The single-line base64 text copied above |
| `MACOS_CERTIFICATE_PASSWORD` | Password chosen while exporting the `.p12` |
| `APPLE_ID` | Apple Account email used for notarization |
| `APPLE_TEAM_ID` | Apple Developer Team ID, for example `T52GZCH73Y` |
| `APPLE_APP_SPECIFIC_PASSWORD` | The generated app-specific password |

The existing tag workflow also starts `publish-npm`. Add `NPM_TOKEN` when npm
publishing is wanted; without it, the independent GitHub Release can still be
created, but the npm job reports a failure. This token is separate from macOS
signing.

The optional Actions variable `MACOS_CODESIGN_IDENTITY` can contain the full
identity name, for example `Developer ID Application: Name (TEAMID)`. When it is
set, the workflow uses it to verify that the imported `.p12` is the expected
identity. Signing itself always uses the unique certificate fingerprint found
in the temporary keychain, and the workflow adds that temporary keychain to the
runner user's keychain search list before invoking `codesign`. Leave the
variable unset when the exported `.p12` contains only one Developer ID
Application identity and this extra guard is not needed.

## Local Notarization Credentials

For local release testing, store notarization credentials in Keychain rather
than in a shell history:

```bash
xcrun notarytool store-credentials "ACECode-notary"
xcrun notarytool history --keychain-profile "ACECode-notary"
```

Enter the Apple Account, Team ID, and app-specific password when prompted. This
profile is local-only; GitHub Actions uses repository secrets instead.

Apple's current references are
[Developer ID certificates](https://developer.apple.com/help/account/certificates/create-developer-id-certificates),
[custom notarization workflows](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow),
and [app-specific passwords](https://support.apple.com/zh-cn/102654).

## Dry Run

After the workflow changes are on GitHub, open **Actions > package > Run
workflow** and leave `npm_version` empty.

- With all five macOS secrets configured, the two DMGs are signed, notarized,
  stapled, Gatekeeper-checked, and uploaded as workflow artifacts.
- Without those secrets, a manual run still creates artifacts whose filenames
  end in `-unsigned.dmg`, for packaging inspection only.
- A `v*` tag never permits the unsigned fallback.

Download both DMG artifacts and test the matching architecture on a clean Mac
user account before creating the release tag.

## Tagged Release

The version in `CMakeLists.txt` is authoritative. The tag must match it exactly:

```bash
git switch master
git pull --ff-only
git tag -a v0.8.7 -m "ACECode v0.8.7"
git push origin v0.8.7
```

The tag starts the full package workflow. After every platform build succeeds,
GitHub creates a Release containing the versioned x64 and arm64 DMGs, platform
archives, debug symbols, and `SHA256SUMS.txt`. Do not reuse a failed public tag;
fix the cause, increment the version, and create a new tag.

## Local Build And DMG Check

Given an already configured macOS build directory:

```bash
cmake --build build --target acecode acecode-desktop acecode-user-installer

codesign_identity="Developer ID Application: Name (TEAMID)"
scripts/macos_codesign.sh \
  --identity "$codesign_identity" \
  --binary build/acecode \
  --bundle "build/Install ACECode.app" \
  --app build/ACECode.app

scripts/macos_create_dmg.sh \
  --app build/ACECode.app \
  --installer "build/Install ACECode.app" \
  --output dist/ACECode-local.dmg

codesign --force --timestamp --sign "$codesign_identity" \
  dist/ACECode-local.dmg
scripts/macos_notarize.sh \
  --file dist/ACECode-local.dmg \
  --keychain-profile "ACECode-notary"
```

The notarization helper waits for Apple's result, requires `Accepted`, staples
and validates the ticket, and runs a Gatekeeper assessment. GitHub Actions runs
the same helper using secrets.
