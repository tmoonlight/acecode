## Context

ACECode currently publishes signed and notarized macOS DMGs for x64 and arm64. The image displays `ACECode.app` beside a custom signed `Applications.app` droplet that visually imitates a folder and copies the app to `~/Applications`. Finder does not reliably deliver the real drag operation to that application, so the release can pass its command-line Launch Services test while the advertised installation gesture fails for users.

The application bundle and self-update ZIP already have separate signing and notarization flows. The native updater already supports the exact `~/Applications/ACECode.app` path, so only the first-install distribution format needs to change. Tagged releases fail closed when trust credentials are unavailable, while manual workflow runs may produce clearly named unsigned inspection artifacts.

## Goals / Non-Goals

**Goals:**

- Make a standard `.pkg` opened by macOS Installer the only graphical macOS installation artifact.
- Restrict installation to the current user's `~/Applications/ACECode.app` without administrator authorization.
- Sign the app with Developer ID Application, sign the product archive with Developer ID Installer, and notarize and staple the distributed PKG.
- Remove the DMG image, fake Applications droplet, Finder layout resources, and their build/test surface.
- Preserve macOS CLI tar archives and native self-update ZIPs.

**Non-Goals:**

- Do not install new copies into system `/Applications` or add privileged helpers, package scripts, launch daemons, or Authorization Services calls.
- Do not silently install immediately on double-click; users still confirm installation through Apple's Installer UI.
- Do not relocate or delete existing `/Applications/ACECode.app` installations, and do not remove legacy self-update compatibility for a copy already running there.
- Do not change Windows, Linux, npm, or update-manifest formats.

## Decisions

### Decision 1: Use an Installer product archive restricted to CurrentUserHomeDirectory

A new packaging helper will create a component package whose payload location is `/Applications`, then wrap it in a product archive whose Distribution enables only `currentUserHome` and explicitly disables `localSystem` and `anywhere`. In the current-user domain, the component location resolves to `~/Applications/ACECode.app`. The Distribution will disable customization and external scripts and will identify `dev.acecode.desktop` as an application that must be closed before replacement.

The helper will validate the app bundle identifier, version, architecture, minimum OS version, and output extension. A temporary working directory and final atomic move keep incomplete packages away from the requested output path. It will support an optional Developer ID Installer identity for tagged releases and an unsigned mode for manual structural inspection.

A conventional system-domain PKG was rejected because it writes `/Applications` and can request administrator authorization. A custom installer app was rejected because it recreates the misleading interaction being removed.

### Decision 2: Use a distinct Developer ID Installer identity for the outer PKG

The existing application identity continues signing all executable code. Tagged macOS jobs additionally import a separate password-protected PKCS#12 containing `Developer ID Installer`, verify its expected identity when configured, and pass it to `productbuild`. Both identities live only in the existing temporary CI keychain and both certificate files are deleted in the unconditional cleanup step.

Using Developer ID Application for the outer archive is not valid for Installer packages. Shipping an unsigned outer package was rejected because tagged releases must remain Gatekeeper-compatible and fail closed.

### Decision 3: Notarize the exact PKG release asset

The app remains independently notarized and stapled before generating its update ZIP. The final signed PKG is then submitted to `notarytool`, stapled, checked with `pkgutil`, and assessed by Gatekeeper using the `install` assessment type. No payload is changed after the outer product archive is signed.

The existing DMG notarization helper will be replaced with a PKG-specific helper so extension checks and Gatekeeper verification cannot accidentally use the obsolete disk-image mode.

### Decision 4: Remove the DMG implementation instead of leaving a hidden fallback

The release workflow will build and upload `ACECode-<version>-macos-<arch>.pkg` and will no longer create, sign, notarize, collect, or checksum `.dmg` files. The custom `Applications.app` target, its AppKit source and plist, DMG layout script, drop verification script, background artwork, and DMG contract test will be deleted. Documentation and updater recovery messages will direct users to the signed PKG.

Keeping an unadvertised DMG fallback was rejected because it would preserve a broken path and allow release logic to regress back to it.

### Decision 5: Verify both archive structure and effective install domain

Portable shell contract tests will assert that release automation references PKG helpers and contains no DMG path. On macOS, a focused package test will build an unsigned fixture, use `installer -dominfo` to require exactly `CurrentUserHomeDirectory`, expand the archive to inspect its Distribution and component metadata, and reject scripts, relocation, or enabled system/anywhere domains.

This avoids claiming that XML text alone is sufficient: the test checks the product archive interpreted by the macOS Installer tooling.

## Risks / Trade-offs

- [Risk] A Developer ID Installer certificate is not currently configured. -> Mitigation: add explicit secrets and documentation; tagged releases fail before emitting an unsigned PKG, while local unsigned package tests remain available.
- [Risk] A Distribution mistake silently re-enables system installation. -> Mitigation: assert the expanded Distribution and require `installer -dominfo` to report only `CurrentUserHomeDirectory`.
- [Risk] Installer receipts add persistent package metadata. -> Mitigation: use one stable component identifier and version so upgrades replace the same application predictably; uninstall remains deleting `~/Applications/ACECode.app`.
- [Risk] Existing users installed in `/Applications` still need updates. -> Mitigation: retain updater support for the exact legacy system location, but all new PKG installs target the current-user location.
- [Risk] Users expect one-click installation. -> Mitigation: document the normal double-click plus Installer confirmation flow; do not attempt unsafe silent installation.

## Migration Plan

1. Add and locally validate unsigned PKG creation, domain inspection, archive expansion, and PKG notarization contracts.
2. Switch both macOS matrix jobs to PKG construction and add the Installer identity to tag credential validation/import.
3. Remove the custom droplet and every DMG-only script, resource, test, workflow step, and documentation reference.
4. Configure the two new Installer-certificate secrets, run a manual workflow, and test each architecture from a non-admin account before tagging a release.
5. Roll back by reverting this change; previously published DMGs and existing installed applications remain unaffected.

## Open Questions

None.
