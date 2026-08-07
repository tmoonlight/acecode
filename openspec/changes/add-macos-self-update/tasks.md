## 1. Bundle And Archive Foundations

- [x] 1.1 Add portable macOS app-layout discovery helpers and tests for bundled-daemon, staged-app, and standalone CLI layouts.
- [x] 1.2 Resolve the current executable with the native macOS API and keep existing Windows/Linux behavior covered.
- [x] 1.3 Preserve ZIP Unix permissions, reject link/special entries, and add archive extraction tests.

## 2. Secure macOS Installation

- [x] 2.1 Add a macOS-native preflight that enforces the exact safe user install path and authenticates current/candidate bundles by signature, Team ID, bundle identifier, nested code, and version.
- [x] 2.2 Implement sibling copy, re-verification, previous-bundle retention, atomic path switching, and rollback for failed replacement.
- [x] 2.3 Route bundled macOS daemon upgrades through the native bundle installer while retaining flat CLI and other-platform installation behavior.
- [x] 2.4 Cover bundle-install selection and non-mutating failure paths with focused unit or macOS-native tests.

## 3. Release Distribution

- [x] 3.1 Add reusable scripts that notarize/staple the signed app and create a metadata-preserving self-update ZIP.
- [x] 3.2 Publish verified signed x64/arm64 update ZIP artifacts from the package workflow without removing existing archives or DMGs.
- [x] 3.3 Extend macOS release contract tests for app notarization, update archive creation, and fail-closed tagged releases.

## 4. Documentation And Verification

- [x] 4.1 Document the runtime macOS self-update contract and architecture-specific `aceupdate.json` records.
- [x] 4.2 Run focused C++/script/web tests and a macOS build of the affected targets.
- [x] 4.3 Validate the OpenSpec change and inspect the final diff for unrelated or generated-file changes.
