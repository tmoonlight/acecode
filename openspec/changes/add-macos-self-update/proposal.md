## Why

The desktop update UI and manifest flow already download updates, but on macOS the daemon treats `ACECode.app/Contents/MacOS` as a flat installation directory and cannot replace the signed application bundle. Now that releases are Developer ID-signed, ACECode can add a trusted, user-level self-update path that preserves the existing `~/Applications/ACECode.app` installation contract.

## What Changes

- Detect when the upgrade engine is running from the bundled macOS daemon and update the enclosing `ACECode.app` rather than its `Contents/MacOS` directory.
- Accept a macOS update ZIP containing a complete signed `ACECode.app`, preserve executable permissions while extracting it, and reject unsafe archive entries.
- Verify the candidate bundle's code signature, bundle identifier, expected version, and Apple Developer Team ID against the currently installed app before touching the installation.
- Replace `~/Applications/ACECode.app` as one bundle with rollback to a retained previous bundle if installation fails, then reuse the existing desktop restart flow.
- Produce signed macOS self-update ZIPs in the release workflow and document how to reference them from `aceupdate.json`.
- Keep flat CLI upgrades and Windows/Linux update behavior unchanged.

## Capabilities

### New Capabilities

- `macos-self-update`: Secure discovery, validation, replacement, rollback, and restart behavior for a signed per-user macOS app installation.
- `macos-update-distribution`: Release artifacts and update-manifest guidance for distributing signed macOS app bundles to the self-updater.

### Modified Capabilities

None.

## Impact

- Affected runtime code: `src/upgrade/`, the existing WebUI update job, and macOS framework linkage.
- Affected release code: `.github/workflows/package.yml`, macOS release contract tests, and release documentation.
- Affected tests: portable bundle-layout/archive tests plus macOS-native signature and replacement coverage where available.
- No new third-party runtime dependency is introduced; macOS validation uses system Foundation and Security frameworks.
