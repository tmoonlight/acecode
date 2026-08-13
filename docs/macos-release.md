# macOS PKG Release Guide

ACECode's `package` GitHub Actions workflow builds separate Intel
(`macos-x64`) and Apple silicon (`macos-arm64`) Installer packages and
self-update ZIPs. Tagged releases fail instead of publishing an unsigned or
unnotarized macOS artifact when any required credential is missing.

## What Users Install

Download the PKG matching the Mac architecture, double-click it, and confirm
the normal steps in Apple's Installer application. The package offers only the
current-user installation domain, so Installer places the app at:

```text
~/Applications/ACECode.app
```

The PKG does not install into system `/Applications`, contain install scripts,
invoke `sudo`, use Authorization Services, or request administrator access. A
standard user with write access to their own home directory can install it.
Double-clicking does not silently modify the computer: the user still reviews
and confirms installation through macOS Installer.

The component metadata expresses its path as `/Applications`, while the
product enables only the `CurrentUserHomeDirectory` Installer domain. In that
domain Installer resolves the component beneath the user's home, producing
`~/Applications`; it does not select system-level `/Applications`.

Quit ACECode before reinstalling or upgrading it with a PKG. Installer declares
the production bundle identifier as a must-close application and replaces the
same user-level application component. Users can uninstall ACECode by quitting
it and deleting `~/Applications/ACECode.app`; no privileged uninstaller is
required. Installer receipts may remain in the user's receipt database.

ACECode no longer builds or publishes a DMG. There is no fake Applications
folder and no Finder drag-install path.

## Self-Update Trust And Replacement

The graphical PKG is distinct from the self-update archive. The update service
continues using `ACECode-<version>-macos-<arch>-update.zip`, `aceupdate.json`,
package size, and SHA-256. Native preflight additionally enforces:

- One complete `ACECode.app`, plus the signed flat CLI payload used by existing
  standalone CLI installations.
- Strict Apple signatures for the app and nested code.
- Bundle identifier `dev.acecode.desktop`, the manifest version, and the same
  non-empty Apple Developer Team ID as the installed copy.
- An exact, non-symlinked `~/Applications/ACECode.app` or legacy
  `/Applications/ACECode.app` destination.

The updater stages a hidden sibling, validates it, retains one previous bundle,
and rolls back when replacement validation fails. New PKG installations always
use `~/Applications`; existing `/Applications` installations remain supported
but are neither relocated nor deleted automatically.

## One-Time Apple And GitHub Setup

### Export both Developer ID identities

The release needs two different identities with their private keys:

- `Developer ID Application` signs ACECode executables and `ACECode.app`.
- `Developer ID Installer` signs the outer `.pkg` product archive.

Export each identity from Keychain Access as its own password-protected `.p12`.
Encode each file without line breaks:

```bash
openssl base64 -A -in /secure/ACECode-Developer-ID-Application.p12 | pbcopy
openssl base64 -A -in /secure/ACECode-Developer-ID-Installer.p12 | pbcopy
```

Do not commit certificate exports or passwords. Keep verified exports in secure
offline storage or delete them after configuring GitHub.

### Create an Apple app-specific password

Sign in at [Apple Account](https://account.apple.com/), open **Sign-In and
Security**, and create an app-specific password for the ACECode release
workflow. Two-factor authentication must be enabled.

### Add repository secrets

Under **GitHub repository > Settings > Secrets and variables > Actions**, add:

| Secret | Value |
| --- | --- |
| `MACOS_CERTIFICATE_BASE64` | Base64 Developer ID Application `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | Application `.p12` export password |
| `MACOS_INSTALLER_CERTIFICATE_BASE64` | Base64 Developer ID Installer `.p12` |
| `MACOS_INSTALLER_CERTIFICATE_PASSWORD` | Installer `.p12` export password |
| `APPLE_ID` | Apple Account email used for notarization |
| `APPLE_TEAM_ID` | Apple Developer Team ID |
| `APPLE_APP_SPECIFIC_PASSWORD` | Apple app-specific password |

The optional Actions variables `MACOS_CODESIGN_IDENTITY` and
`MACOS_INSTALLER_SIGNING_IDENTITY` can contain the expected full identity
names. The workflow verifies those names when configured, uses the application
certificate fingerprint for `codesign`, and uses the Installer identity name
for `productbuild`.

The independent npm publication job requires `NPM_TOKEN` when npm publishing
is desired.

## Local Notarization Credentials

Store local credentials in Keychain rather than shell history:

```bash
xcrun notarytool store-credentials "ACECode-notary"
xcrun notarytool history --keychain-profile "ACECode-notary"
```

Apple references:
[Developer ID certificates](https://developer.apple.com/help/account/certificates/create-developer-id-certificates),
[custom notarization workflows](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow),
and [app-specific passwords](https://support.apple.com/zh-cn/102654).

## Dry Run

Run **Actions > package > Run workflow** with `npm_version` empty.

- With all seven macOS secrets configured, the app and two architecture PKGs
  are signed, notarized, stapled, Gatekeeper-checked, and uploaded alongside
  trusted update ZIPs.
- Without complete credentials, a manual run emits `-unsigned.pkg` and
  `-update-unsigned.zip` artifacts for structural inspection only.
- A `v*` tag never permits unsigned fallback.

Before tagging, install each signed PKG on a clean non-admin account. Confirm
that Installer does not show an administrator authentication sheet and that the
result exists only at `~/Applications/ACECode.app`.

## Tagged Release

The version in `CMakeLists.txt` is authoritative and must exactly match the tag:

```bash
git switch master
git pull --ff-only
git tag -a v0.8.16 -m "ACECode v0.8.16"
git push origin v0.8.16
```

After all platform builds succeed, GitHub creates a Release containing the x64
and arm64 PKGs, matching self-update ZIPs, platform archives, debug symbols,
and `SHA256SUMS.txt`. Do not reuse a failed public tag; fix the cause, increment
the version, and create a new tag.

## Publish To The Update Service

Upload only the final `*-update.zip` files beside `aceupdate.json`. Do not point
update records at the PKG, npm archive, or an unsigned dry-run ZIP. Calculate
the exact digest and size from uploaded files:

```bash
shasum -a 256 ACECode-0.8.16-macos-*-update.zip
stat -f '%N %z' ACECode-0.8.16-macos-*-update.zip
```

Publish ZIPs before changing the manifest so clients never observe a release
whose package is missing. Bundled-app updates also pin the Developer ID Team;
standalone CLI updates retain the manifest, size, and checksum trust contract.

## Local Build And PKG Check

Given an already configured macOS build directory:

```bash
cmake --build build --target acecode acecode-desktop

app_identity="Developer ID Application: Name (TEAMID)"
installer_identity="Developer ID Installer: Name (TEAMID)"

scripts/macos_codesign.sh \
  --identity "$app_identity" \
  --binary build/acecode \
  --app build/ACECode.app

scripts/macos_notarize_app.sh \
  --app build/ACECode.app \
  --keychain-profile "ACECode-notary"

scripts/macos_create_update_zip.sh \
  --app build/ACECode.app \
  --output dist/ACECode-local-macos-arm64-update.zip \
  --require-trusted

scripts/macos_create_pkg.sh \
  --app build/ACECode.app \
  --output dist/ACECode-local.pkg \
  --installer-identity "$installer_identity"

scripts/macos_notarize_pkg.sh \
  --pkg dist/ACECode-local.pkg \
  --keychain-profile "ACECode-notary"

installer -dominfo -pkg dist/ACECode-local.pkg
pkgutil --check-signature dist/ACECode-local.pkg
spctl --assess --type install --verbose=4 dist/ACECode-local.pkg
```

For a local unsigned structural package, omit `--installer-identity`; never
publish that output. `macos_create_pkg.sh` expands and audits its own output and
requires `installer -dominfo` to return only `CurrentUserHomeDirectory`.
