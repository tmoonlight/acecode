## 1. User-Domain PKG Tooling

- [x] 1.1 Add a macOS package creation helper that validates `ACECode.app`, creates a non-relocatable component, restricts the Distribution to `CurrentUserHomeDirectory`, and optionally signs with Developer ID Installer
- [x] 1.2 Replace the DMG notarization helper with PKG submission, stapling, signature, and Gatekeeper install validation
- [x] 1.3 Add focused PKG structure/domain tests and update release-script contracts to reject DMG packaging paths

## 2. GitHub Release Pipeline

- [x] 2.1 Require, import, verify, and clean up a separate Developer ID Installer identity for tagged macOS releases
- [x] 2.2 Replace DMG creation/signing/notarization/upload steps with signed PKG creation, notarization, and upload for both macOS architectures
- [x] 2.3 Include PKGs and exclude DMGs when collecting release assets and generating checksums

## 3. DMG Implementation Removal

- [x] 3.1 Remove the fake `Applications.app` CMake target, source, plist, debug-symbol handling, signing input, and Finder drop verification
- [x] 3.2 Delete the DMG creation helper, disk-image artwork, obsolete drop helper, and DMG layout test registration
- [x] 3.3 Update updater recovery messages, icon tooling text, and the macOS release guide for PKG-only current-user installation

## 4. Verification

- [x] 4.1 Run shell syntax and release contract tests, build an unsigned PKG, and verify its effective domain and expanded structure on macOS
- [x] 4.2 Build the macOS desktop and relevant unit tests, then run the focused CTest suite
- [x] 4.3 Validate workflow YAML and OpenSpec artifacts, audit active source for DMG release references, and review the final diff
