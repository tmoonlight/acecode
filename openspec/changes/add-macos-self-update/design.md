## Context

ACECode already exposes update-check, background download, progress, completion, and desktop-restart behavior through the daemon and WebUI. The generic installer is designed for flat platform archives: it locates the running executable, treats its parent as the install directory, and copies staged files into that directory. On macOS the WebUI daemon is `ACECode.app/Contents/MacOS/acecode-daemon`, while a release contains a complete signed `ACECode.app`; applying the flat algorithm therefore targets the wrong directory and would invalidate the bundle layout.

Public macOS releases are now Developer ID-signed and installed at the deliberately unprivileged `~/Applications/ACECode.app` path. The self-updater must preserve that boundary, validate Apple signing identity independently of the update manifest, replace the bundle without leaving mixed signed content, and keep enough state to roll back a failed replacement. The existing update server and `aceupdate.json` schema remain the distribution control plane.

## Goals / Non-Goals

**Goals:**

- Reuse the current update UI, daemon API, manifest selection, download progress, SHA-256 check, and restart bridge.
- Update the enclosing macOS application bundle when the daemon runs from the supported per-user installation.
- Require a valid Developer ID signature for `dev.acecode.desktop`, the same non-empty Team ID as the installed app, nested-code validity, and an application version equal to the selected manifest release.
- Preserve archive executable modes and reject symbolic links or other unsafe ZIP entry types.
- Replace the app as a unit with a retained previous bundle and automatic rollback on an incomplete filesystem transition.
- Publish signed, notarized, stapled update ZIPs for both macOS architectures.

**Non-Goals:**

- Do not add privileged writes to `/Applications`, an authorization helper, or a package installer.
- Do not add unattended background installation; the existing explicit user-triggered upgrade remains the install trigger.
- Do not replace `aceupdate.json` with Sparkle appcasts or GitHub Releases as an update service.
- Do not change flat macOS CLI installations or Windows/Linux update semantics.
- Do not support self-updating an app launched from a DMG, an arbitrary copied path, or a redirected `~/Applications` directory.

## Decisions

### Decision 1: Extend the existing updater instead of integrating Sparkle

The daemon already owns update selection, download progress, UI state, and restart coordination across all supported platforms. The macOS path will branch only after resolving the real running executable and detecting an enclosing `ACECode.app`. It will otherwise continue through the flat installer.

Sparkle was considered because it provides a mature macOS updater, but it would introduce a second feed/signing system, framework packaging, Objective-C lifecycle integration, and duplicate UI state. A focused bundle installer keeps the current server and user experience consistent.

### Decision 2: Distribute a complete app in a ZIP made after app notarization

The release workflow will notarize and staple the signed `ACECode.app`, then create `ACECode-<version>-macos-<arch>-update.zip` with `ditto --keepParent`. The archive also contains a root `acecode` copied from the app's already signed and notarized bundled daemon, because desktop and standalone CLI installations share the same manifest target. The existing notarized DMG remains the first-install artifact. The update manifest points to the ZIP and continues to provide its lowercase SHA-256 and byte size.

A DMG-based runtime updater was rejected because it would need mounting, detach/error recovery, and a separate installation handoff. Reusing the existing tar archive was rejected because the current secure extractor is ZIP-based and tar link handling would add another attack surface.

### Decision 3: Resolve the macOS executable and bundle explicitly

`current_executable_path` will use `_NSGetExecutablePath` on Apple platforms. A portable layout helper recognizes only an executable under `ACECode.app/Contents/MacOS` and finds only a top-level or single-wrapper `ACECode.app` in staging. The native preflight then requires the resolved current bundle to be the exact non-symlinked `~/Applications/ACECode.app` destination already defined by the user-install policy.

This prevents a daemon from treating its `Contents/MacOS` directory as a flat install and prevents self-update from mutating a mounted DMG or an unexpected writable bundle.

### Decision 4: Treat Apple code signing as the trust boundary

After the archive SHA-256 passes, the macOS native helper validates both current and candidate bundles with Security.framework strict, all-architecture, and nested-code checks. It obtains the current Team ID, requires it to be non-empty, copies the installed app's designated Developer ID requirement, and applies that requirement to the candidate before comparing Team IDs. It also reads `CFBundleShortVersionString` and requires the selected manifest version.

The manifest may be hosted on plain HTTP for backward compatibility, so SHA-256 alone is not an authenticity guarantee. The installed Developer ID designated requirement and explicit Team-ID comparison prevent a changed manifest and package from authorizing code signed by a different developer, while the version check prevents replaying an older same-team build under a newer manifest version. Certificate renewal remains possible because Apple's designated requirement pins the stable Team ID and Developer ID certificate class rather than one leaf certificate fingerprint.

### Decision 5: Extract only regular files/directories and restore archive permissions

The libzip extractor will inspect Unix external attributes, reject symbolic links and unsupported file types, and apply stored permission bits after writing. Directory permissions are applied after extraction so read-only directory metadata cannot interrupt extraction. Existing path traversal defenses remain in force.

This supports the executable bits required by signed app binaries without invoking a shell extractor and prevents link entries from escaping staging during later bundle copy operations.

### Decision 6: Copy, re-verify, then atomically switch bundles with rollback

The native installer copies the verified staged app to a unique hidden sibling under `~/Applications`, re-validates that copy, and only then mutates the installed path. It rotates the current app to `.ACECode.previous.app`, moves the candidate to `ACECode.app`, and restores the previous app if the second move or final verification fails. A successful update keeps one previous bundle for manual recovery and reports that path through the existing update job.

Renaming a bundle containing running Mach-O images is supported by the filesystem; those processes continue from their open images. The current UI then performs its existing orderly daemon/tray teardown and launches the already-recorded executable path, which now resolves inside the new bundle. Copying files directly into `Contents` was rejected because it creates a partially updated, temporarily invalid code signature.

## Risks / Trade-offs

- [Risk] An unusual filesystem may not permit moving the running bundle. -> The candidate is copied and verified first; any failed first move leaves the current app untouched, and any failed second move triggers rollback.
- [Risk] A crash in the narrow interval between the two moves can leave only `.ACECode.previous.app`. -> The updater uses same-directory moves to minimize the interval, keeps the backup, and documents manual recovery; startup recovery can be added later if field evidence warrants it.
- [Risk] ZIP metadata differs between producers. -> Release ZIPs use the repository's `ditto` helper, the extractor explicitly restores Unix modes, and final Security.framework validation rejects altered app content.
- [Risk] Strict signature validation makes local unsigned builds unable to exercise installation end-to-end. -> Portable layout/archive logic remains unit-testable everywhere, while macOS smoke validation uses an ad-hoc or Developer ID-signed fixture only in explicit local/CI checks; public self-update intentionally fails closed for unsigned apps.
- [Risk] Notarizing the app and DMG adds another Apple notary submission per architecture. -> Tagged releases already require notary credentials and fail closed; the additional submission is isolated to macOS release jobs.

## Migration Plan

1. Ship the runtime support and release ZIP generation together.
2. Run the package workflow with signing/notarization credentials and verify both update ZIPs after a clean extraction.
3. Upload the new ZIPs to the configured update service and add `macos-x64` and `macos-arm64` package records to `aceupdate.json` with exact hashes and sizes.
4. Test an upgrade from the preceding signed release in a clean macOS user account for both architectures before publishing the manifest.
5. Roll back distribution by removing the new release from `aceupdate.json`; clients retain the previous bundle at `~/Applications/.ACECode.previous.app` after a successful upgrade.

## Open Questions

None. The supported installation remains the exact current-user destination established by the existing signed DMG installer.
