## 1. Standard DMG Layout

- [x] 1.1 Change the DMG creation script to expose only `ACECode.app` and an `Applications` symlink to `/Applications`
- [x] 1.2 Replace the crowded installer background and Finder positions with a clean two-item drag layout
- [x] 1.3 Update targeted DMG contract tests to reject the custom installer and visible instructions payloads

## 2. Release Build Cleanup

- [x] 2.1 Remove the custom current-user installer from the desktop CMake build and delete installer-only source and artwork
- [x] 2.2 Remove installer signing, symbols, arguments, and artifacts from the macOS release workflow
- [x] 2.3 Update macOS release script tests for the simplified packaging interface

## 3. Native Self-Update Compatibility

- [x] 3.1 Extend the pure macOS install policy to accept only the per-user and system Applications destinations
- [x] 3.2 Update native replacement validation and permission handling for `/Applications/ACECode.app`
- [x] 3.3 Add targeted policy and updater tests for both supported paths and safe rejection behavior

## 4. Documentation and Verification

- [x] 4.1 Update release and updater documentation to describe standard drag installation and both supported update paths
- [x] 4.2 Run targeted script, unit, build, signing, and mounted-image checks without running the full local test suite
- [x] 4.3 Build and open the resulting DMG for visual inspection
