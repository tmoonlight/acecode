## Why

The current DMG imitates the familiar drag-to-Applications layout but places a custom installer app on the right, which is visually confusing and unlike normal macOS distribution. The disk image should expose the standard Applications destination directly and omit documentation that users do not need during installation.

## What Changes

- **BREAKING** Replace `Install ACECode.app` with the standard `Applications` link to `/Applications` as the right-hand drag destination.
- Remove the visible installation instructions file from the DMG.
- Simplify the background and Finder layout to show only `ACECode.app`, a directional cue, and `Applications`.
- Keep the disk image signed, notarized, styled, and reproducible in GitHub Actions.
- Allow a correctly signed ACECode installed at `/Applications/ACECode.app` to use the native self-update path, while retaining the existing per-user location for already installed copies.

## Capabilities

### New Capabilities

- `macos-standard-dmg-installation`: A conventional macOS disk image installs ACECode by dragging the app to the system Applications folder and keeps that installation eligible for secure self-update.

### Modified Capabilities

None.

## Impact

- Affected packaging and layout: `scripts/macos_create_dmg.sh`, its background asset, release workflow, and DMG contract tests.
- Affected installation/update policy: macOS destination validation, native app replacement, related unit tests, and release documentation.
- The custom current-user installer is removed from the public DMG and no longer built, signed, or uploaded as part of the release workflow.
- No change to Windows/Linux packages, update manifest schema, Developer ID identity, or Apple notarization credentials.
