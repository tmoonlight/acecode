# macOS DMG Release Guide

ACECode's `package` GitHub Actions workflow builds separate Intel (`macos-x64`)
and Apple silicon (`macos-arm64`) disk images and self-update ZIPs. Tagged
releases require a Developer ID signature and Apple notarization; the workflow
will fail instead of publishing an unsigned macOS artifact when any required
credential is missing.

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

After the first DMG installation, the desktop's existing **Check for updates**
flow can replace this per-user app in place. Self-update is deliberately limited
to the exact `~/Applications/ACECode.app` installation; an app launched from the
DMG, `/Applications`, or another copied location asks the user to install with
the signed DMG first.

## Self-Update Trust And Replacement

The update service still uses `aceupdate.json`, package size, and SHA-256. On
macOS those checks are followed by an independent native trust check before the
installed app is touched:

- The archive must contain one complete top-level (or single-wrapper)
  `ACECode.app` and no symbolic-link or special-file ZIP entries. Release ZIPs
  also carry a root `acecode` copied from the notarized bundle so standalone CLI
  installations keep their flat upgrade behavior.
- The app and all nested code must pass strict Apple code-signature validation.
- The candidate must use bundle identifier `dev.acecode.desktop`, report the
  manifest's exact version, and use the same non-empty Apple Developer Team ID
  as the installed app.
- `~/Applications` and `ACECode.app` must not be symlinks or resolve outside the
  supported current-user destination.

The updater copies and validates the new app as a hidden sibling, moves the old
app to `~/Applications/.ACECode.previous.app`, then switches the new bundle into
place. If the switch or final validation fails, it restores the previous app.
The desktop's normal restart action stops its daemons and tray resources before
launching the new executable.

Only one previous app is retained. After confirming the new release works, the
hidden backup may be deleted manually. If a new release cannot launch, quit all
ACECode processes and restore that bundle from Terminal or Finder before trying
the update again.

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

- With all five macOS secrets configured, the apps and two DMGs are signed,
  notarized, stapled, Gatekeeper-checked, and uploaded together with verified
  `*-update.zip` artifacts.
- Without those secrets, a manual run still creates artifacts whose filenames
  end in `-unsigned.dmg` or `-update-unsigned.zip`, for packaging inspection
  only. Never place those ZIPs on the update service.
- A `v*` tag never permits the unsigned fallback.

Download both DMG artifacts and test the matching architecture on a clean Mac
user account before creating the release tag.

## Tagged Release

The version in `CMakeLists.txt` is authoritative. The tag must match it exactly:

```bash
git switch master
git pull --ff-only
git tag -a v0.8.8 -m "ACECode v0.8.8"
git push origin v0.8.8
```

The tag starts the full package workflow. After every platform build succeeds,
GitHub creates a Release containing the versioned x64 and arm64 DMGs, matching
`ACECode-<version>-macos-<arch>-update.zip` files, platform archives, debug
symbols, and `SHA256SUMS.txt`. Do not reuse a failed public tag; fix the cause,
increment the version, and create a new tag.

## Publish To The Update Service

Upload both final update ZIPs beside `aceupdate.json` under the configured
upgrade base URL. Do not point macOS records at a DMG, the npm tar archive, or an
unsigned dry-run ZIP. Calculate the exact metadata from the files that were
uploaded:

```bash
shasum -a 256 ACECode-0.8.9-macos-*-update.zip
stat -f '%N %z' ACECode-0.8.9-macos-*-update.zip
```

Add both architecture records to the release. Replace the illustrative hashes
and sizes below with the command output:

```json
{
  "schema_version": 1,
  "latest": "0.8.9",
  "releases": [
    {
      "version": "0.8.9",
      "published_at": "2026-08-06T20:00:00Z",
      "notes": "macOS self-update support.",
      "packages": [
        {
          "target": "macos-x64",
          "file": "ACECode-0.8.9-macos-x64-update.zip",
          "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "size": 12345678
        },
        {
          "target": "macos-arm64",
          "file": "ACECode-0.8.9-macos-arm64-update.zip",
          "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
          "size": 12345678
        }
      ]
    }
  ]
}
```

Publish ZIPs before changing the manifest so clients never observe a release
whose package is missing. HTTPS is strongly recommended. Bundled-app updates
also pin the Developer ID Team during native preflight; standalone CLI updates
retain the existing manifest, size, and checksum trust contract.

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

scripts/macos_notarize_app.sh \
  --app build/ACECode.app \
  --keychain-profile "ACECode-notary"

scripts/macos_create_update_zip.sh \
  --app build/ACECode.app \
  --output dist/ACECode-local-macos-arm64-update.zip \
  --require-trusted

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

The notarization helpers wait for Apple's result, require `Accepted`, staple and
validate the app or DMG ticket, and run a Gatekeeper assessment. The update ZIP
helper follows Apple's `ditto --keepParent` workflow and verifies a clean
extraction before publishing. GitHub Actions runs the same helpers using
secrets.
