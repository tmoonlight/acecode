# macOS Installer And PKG Release Guide

ACECode's `package` GitHub Actions workflow builds separate Intel
(`macos-x64`) and Apple silicon (`macos-arm64`) installer DMGs, self-update ZIPs,
and archives.
When the separate Developer ID Installer credentials are configured, it also
builds PKG installers for both architectures. Tagged releases always fail
instead of publishing an unsigned or unnotarized macOS application artifact;
when only the Installer credentials are absent, they publish no PKG at all.

## What Users Install

Download `ACECode-<version>-macos-<arch>.dmg`, open it, and launch
**ACECode Installer.app**. The v6 native installer offers personal installation
(normally `~/Applications`, falling back to `~/Apps`), system `/Applications`,
and a custom writable folder. System/custom installation uses the current
account's actual permissions; it never requests elevation. Quit the destination
ACECode before replacement. Replacement requires confirmation, stages and
validates a copy, and attempts rollback on failure without deleting user data.
If rollback or old-file cleanup fails, the installer reports the retained path.
The graphical installer and in-app updater share `.ACECode.update.lock`, so
they cannot replace the same installation concurrently. Both reject unsafe lock
files without blocking or changing the installed application.

When the Release contains PKGs, download the one matching the Mac architecture,
double-click it, and confirm the normal steps in Apple's Installer application.
The package offers only the current-user installation domain, so Installer
places the app at:

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

The DMG contains only the signed, notarized, stapled native installer. There is
no fake Applications folder or Finder drag-install target. PKGs remain an
optional alternative; absence of Developer ID Installer credentials does not
disable DMGs, which use the Developer ID Application identity.

## Self-Update Trust And Replacement

The graphical DMG and PKG are distinct from the self-update archive. The update service
continues using `ACECode-<version>-macos-<arch>-update.zip`, `aceupdate.json`,
package size, and SHA-256. Native preflight additionally enforces:

- One complete `ACECode.app`, plus the signed flat CLI payload used by existing
  standalone CLI installations.
- Strict Apple signatures for the app and nested code.
- Bundle identifier `dev.acecode.desktop`, the manifest version, and the same
  non-empty Apple Developer Team ID as the installed copy.
- An absolute, canonical, real `ACECode.app` destination with a writable parent,
  outside App Translocation and other app bundles. Custom directories are
  supported alongside `~/Applications` and `/Applications`.

The updater creates a private sibling directory `.acecode-update-<UUID>` with
`staging/ACECode.app`, `previous/ACECode.app`, and `failed/ACECode.app` slots.
It validates the staged app, retains the previous bundle, and rolls back when
replacement validation fails. All copies retain the actual name `ACECode.app`:
an old process can remain alive until restart, and renaming its bundle to a
random name can expose that name in macOS permission prompts. The UUID belongs
to the containing directory, not the application name. Empty containers are
removed on exit; remaining application copies are preserved. Copies inside an
update directory cannot be used as self-update installation targets.
New PKG installations always
use `~/Applications`; existing `/Applications` installations remain supported
but are neither relocated nor deleted automatically.

Custom installer locations (including the `~/Apps` fallback) update in place
without elevation when the current account has permission. Unwritable locations
require a manual installation or moving the app to a writable folder. Retained
`.acecode-update-<UUID>/previous/ACECode.app` backups require manual cleanup
when no longer needed and their old processes have exited. Historical
`.ACECode-<UUID>.previous.app` backups are left untouched; the updater does not
delete unrelated or existing backups. An update executed by an older version
still uses that version's naming policy. After updating, quit and reopen the
installed `ACECode.app` to stop running from the old copy. macOS permission
dialog attribution must be verified on a Mac; portable filesystem tests do not
prove the text of a system permission dialog.

Older ACECode versions with the two-directory restriction need one manual
installation of the new version before custom-location self-update is available.
The new installer cannot change the updater policy inside an old binary.

## One-Time Apple And GitHub Setup

### Export both Developer ID identities

The full release can use two different identities with their private keys:

- `Developer ID Application` signs ACECode executables, `ACECode.app`, the
  hardened-runtime installer wrapper, and the DMG.
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
| `MACOS_INSTALLER_CERTIFICATE_BASE64` | Optional for PKG: Base64 Developer ID Installer `.p12` |
| `MACOS_INSTALLER_CERTIFICATE_PASSWORD` | Optional for PKG: Installer `.p12` export password |
| `APPLE_ID` | Apple Account email used for notarization |
| `APPLE_TEAM_ID` | Apple Developer Team ID |
| `APPLE_APP_SPECIFIC_PASSWORD` | Apple app-specific password |

The Application certificate and Apple notarization values are mandatory for a
tagged macOS release. The two Installer values are an optional pair: configure
both to publish PKGs, or configure neither to omit PKGs. Configuring only one is
an error.

The optional Actions variables `MACOS_CODESIGN_IDENTITY` and
`MACOS_INSTALLER_SIGNING_IDENTITY` can contain the expected full identity
names. The workflow verifies those names when configured, uses the application
certificate fingerprint for `codesign`, and uses the Installer identity name
for `productbuild`.

The independent npm publication job is disabled for tagged and manual runs.
Platform packages, GitHub Releases, and updater packages continue to publish.

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

Run **Actions > package > Run workflow**. The reserved `npm_version` input has
no effect while npm publication is disabled.

- With all seven macOS secrets configured, the app and two architecture PKGs
  are signed, notarized, stapled, Gatekeeper-checked, and uploaded alongside
  trusted update ZIPs and two installer DMGs.
- With complete Application/notarization secrets but neither Installer secret,
  the app is still signed, notarized, and stapled and the workflow uploads
  trusted update ZIPs, installer DMGs, and archives, but creates no PKG artifact.
- Without complete Application/notarization credentials, a manual run may emit
  a clearly suffixed `-update-unsigned.zip` for structural inspection only.
  No installer DMG is built or uploaded in this mode.
- A `v*` tag never permits unsigned fallback, and a partial Installer pair is
  always rejected.

When PKG credentials are configured, install each signed PKG on a clean
non-admin account before tagging. Confirm that Installer does not show an
administrator authentication sheet and that the result exists only at
`~/Applications/ACECode.app`. When PKGs are intentionally omitted, confirm the
dry run has no PKG artifact and that both trusted update ZIPs completed.
For each architecture, also test the downloaded, quarantined DMG on a clean
standard account: personal install, denied system location, writable custom
location, running-app refusal, replacement confirmation, launch, and reveal.
Test on macOS 11 as well as a current OS before claiming runtime compatibility.

## Tagged Release

The version in `CMakeLists.txt` is authoritative and must exactly match the tag:

```bash
git switch master
git pull --ff-only
git tag -a v0.8.16 -m "ACECode v0.8.16"
git push origin v0.8.16
```

After all platform builds succeed, GitHub creates a Release containing matching
self-update ZIPs, platform archives, debug symbols, and `SHA256SUMS.txt`. It must
contain exactly `ACECode-<version>-macos-x64.dmg` and
`ACECode-<version>-macos-arm64.dmg`; missing, extra, duplicate, misversioned, or
unsigned-suffixed DMGs fail release collection. When
Installer credentials are available it also contains exactly the x64 and arm64
PKGs; otherwise it contains zero PKGs. Do not reuse a failed public tag; fix the
cause, increment the version, and create a new tag.

## Publish To The Update Service

Upload only the final `*-update.zip` files beside `aceupdate.json`. Do not point
update records at a DMG, PKG, npm archive, or an unsigned dry-run ZIP. Calculate
the exact digest and size from uploaded files:

```bash
shasum -a 256 ACECode-0.8.16-macos-*-update.zip
stat -f '%N %z' ACECode-0.8.16-macos-*-update.zip
```

Publish ZIPs before changing the manifest so clients never observe a release
whose package is missing. Bundled-app updates also pin the Developer ID Team;
standalone CLI updates retain the manifest, size, and checksum trust contract.

Use `python3 scripts/select_macos_update_assets.py release-assets <version>` to
select the exact pair of macOS updater ZIPs. Release collection uses this same
fail-closed selector; DMGs and PKGs cannot become updater assets through broad
architecture-name matching. The workflow does not publish `aceupdate.json` or
upload files to the update service automatically.

## Native Installer Production Pipeline

The repository owns the v6 sources in
[`packaging/macos-installer`](../packaging/macos-installer). The Swift UI, engine,
and standalone tests are maintained with the updater's shared lock contract;
the build script is adapted for production. No local release binaries or
prebuilt payloads are used.

After the existing `build/ACECode.app` signing, notarization, and stapling step,
CI invokes [`macos_create_installer_dmg.sh`](../scripts/macos_create_installer_dmg.sh)
with the explicit matrix `x64` or `arm64` architecture, imported Application
certificate fingerprint, and temporary keychain. It:

1. Verifies the payload's architecture, bundle ID, Developer ID signature,
   pinned team `T52GZCH73Y`, stapled ticket, and Gatekeeper trust.
2. Compiles the wrapper with an explicit `x86_64-apple-macosx11.0` or
   `arm64-apple-macosx11.0` target and `LSMinimumSystemVersion=11.0`.
   Versions and icon come from the exact payload. This does not lower the
   embedded ACECode application's own minimum OS requirement.
3. Copies the payload with `ditto`, signs only the wrapper with hardened runtime
   and timestamp (never `codesign --deep` signing), and checks payload equality.
4. Submits the wrapper ZIP, requires `Accepted`, staples and validates the
   installer, then creates a compressed HFS+ DMG containing that installer only.
   A transient `hdiutil` `Resource busy` error is retried at most twice with
   separate image paths; other errors stop immediately.
5. Signs the DMG with the same Application identity, notarizes/staples it,
   validates its signature/ticket and Gatekeeper assessment, and publishes the
   output only after all checks succeed. Rejected submissions fetch the notary
   log when available. Existing output paths are refused, not overwritten.

Both stages reuse `APPLE_ID`, `APPLE_TEAM_ID`, and
`APPLE_APP_SPECIFIC_PASSWORD`, or `--keychain-profile` / `NOTARYTOOL_PROFILE`
locally. No additional certificate is required. All DMG build/upload steps use
`macos-release.enabled`; only PKG steps use `pkg_enabled`. Signing-material
cleanup remains an `always()` step after all packaging operations.
CI creates, notarizes, validates, and uploads the PKG before building the DMG,
so a later DMG failure leaves the verified PKG available as a workflow artifact.
Tagged releases still require the complete successful packaging matrix.

After preparing a trusted payload, an authorized release operator can run:

```bash
bash scripts/macos_create_installer_dmg.sh \
  --app build/ACECode.app --arch arm64 \
  --output dist/ACECode-local-macos-arm64.dmg \
  --identity "$app_identity" --keychain-profile ACECode-notary
```

Offline verification (no signing identities or live notarization):

```bash
bash tests/scripts/macos_installer_test.sh
python3 tests/scripts/macos_installer_release_test.py
bash tests/scripts/macos_release_scripts_test.sh
```

The PR test workflow runs these checks on macOS without signing credentials.
The first command runs engine fault/rollback and unsafe-lock tests with assertions
enabled and cross-compiles both UI architectures for macOS 11. It also launches
a test bundle and captures personal/system installation windows without
installing an application. The second uses mocked Apple tools to test sequencing,
architecture rejection, unchanged payloads, rejected
notarization, release DMG naming/count, and updater asset selection. These do
not replace live release signing, notarization, or clean-machine smoke tests.

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
