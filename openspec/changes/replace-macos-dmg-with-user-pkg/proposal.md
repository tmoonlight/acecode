## Why

The current macOS DMG disguises a custom installer application as an Applications folder, but Finder does not reliably accept the advertised drag gesture. A native macOS Installer package can provide an honest, accessible installation flow while keeping ACECode entirely in the current user's `~/Applications` directory without administrator authorization.

## What Changes

- **BREAKING** Stop building, notarizing, uploading, documenting, and testing macOS DMG release assets.
- Publish signed, notarized, and stapled x64 and arm64 `.pkg` installers that are restricted to the current-user home installation domain.
- Install `ACECode.app` at `~/Applications/ACECode.app` without `sudo`, Authorization Services, privileged scripts, or writes outside the user's home directory.
- Remove the DMG-only fake `Applications.app` drop target, disk-image layout code, artwork, and obsolete verification helpers.
- Preserve the existing macOS CLI archives and desktop update archives consumed by npm publishing and native self-update flows.
- Update release documentation, checksums, and workflow contract tests for PKG-only graphical distribution.

## Capabilities

### New Capabilities

- `macos-user-pkg-distribution`: Defines trusted x64 and arm64 PKG release assets and their administrator-free installation into the active user's `~/Applications` directory.

### Modified Capabilities

- None.

## Impact

- Affected release automation: `.github/workflows/package.yml` and macOS signing/notarization helpers.
- Affected build and packaging: `cmake/acecode_desktop.cmake`, macOS bundle metadata, and scripts under `scripts/`.
- Removed implementation: the `Applications.app` Finder drop receiver and DMG-only resources/tests.
- Affected documentation and tests: `docs/macos-release.md`, release script contracts, and macOS installer verification.
- Release consumers downloading graphical macOS installers must switch from `.dmg` to `.pkg`; npm archives, update ZIPs, Windows, and Linux formats remain unchanged.
