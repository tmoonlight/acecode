# Proposal

## Why

The new native Windows installer is implemented independently of the release workflow. A full ACECode release must publish installers built from the same tag and Windows payloads as its portable and self-update packages.

## What Changes

- Build and publish native Windows x64 and ARM64 setup executables alongside the existing ZIP downloads.
- Require both installers in release completeness and size checks, and include them in SHA256SUMS.txt.
- Validate embedded payload metadata, architecture and hashes against the exact released Windows ZIP; smoke-test installation and uninstallation in an isolated Windows profile.
- Mirror the verified installers as separate manual downloads, preserving the six-target ZIP updater manifest.

## Capabilities

### New Capabilities

- `windows-installer-distribution`: versioned native Windows installer assets and their release verification.

### Modified Capabilities

None.

## Impact

GitHub package workflow, release asset verification and tests, Windows packaging validation, and installer documentation. No third-party installer framework or signing credentials are introduced. The existing unsigned Windows distribution policy remains in effect.
