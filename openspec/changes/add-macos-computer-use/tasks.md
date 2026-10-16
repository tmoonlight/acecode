## 1. Helper and platform boundary
- [x] 1.1 Introduce a shared NativeBackend declaration and macOS nonblocking helper transport with cancellation, reaping, framing and parent lifetime handling.
- [x] 1.2 Build the macOS helper with an AppKit main loop and an interprocess desktop lease; verify the broker fixture through the production transport.

## 2. Native macOS behavior
- [x] 2.1 Implement application/window discovery, launch, verified AX/window mapping and activation.
- [x] 2.2 Implement bounded AX observations including focus, selection, secure-field filtering and related transient surfaces.
- [x] 2.3 Implement ScreenCaptureKit screenshots, per-surface pixel/point mapping and attachment metadata; test scaling and invalid coordinates.
- [x] 2.4 Implement all existing input/control actions with one-use observation, identity, geometry, focus and hit-target checks.
- [x] 2.5 Implement ACE/plain themed pointer overlay and screenshot composition with cleanup and no focus/input interception.

## 3. Integration and distribution
- [x] 3.1 Add non-prompting helper readiness diagnostics and explicit user-triggered permission requests; update API, settings UI, translations and documentation.
- [x] 3.2 Make tool descriptions, support gates and keyboard/scroll semantics platform-aware without changing Windows behavior.
- [x] 3.3 Package and sign the helper for desktop and standalone macOS artifacts; add package verification.

## 4. Validation
- [x] 4.1 Pass focused native-independent C++ tests and broker lifecycle/transport tests, including malformed input, cancellation, large responses and ownership.
- [x] 4.2 Pass native fixture checks through the actual helper for capture, AX, Unicode/key input, clicks, scroll, drag, control actions, transient UI and pointer behavior; record real permission and display coverage.
- [x] 4.3 Run frontend tests/build, affected package checks, C++ build and relevant regression tests; review final diff and record remaining verification limits honestly.

Native interaction acceptance passed on the authorized Intel Mac on 2026-09-21. See `verification.md` for executed checks and permission/hardware limits.

## 5. Pointer visibility follow-up
- [x] 5.1 Record the inspected Codex client contract and distinguish it from the independently implemented ACECode native backend; correct the earlier pointer acceptance claim.
- [x] 5.2 Add independent checks of the helper's actual WindowServer overlay and rendered pixels, including position, input transparency and cleanup, instead of relying only on screenshot composition.
- [ ] 5.3 Restore visible action feedback for keyboard input and a perceptible click state, using the existing ACECode Windows behavior as the compatibility reference.
- [ ] 5.4 Build and run the affected regression checks and unlocked native fixture; record any remaining on-screen verification limits explicitly.

2026-09-24: Code for 5.3 is present and all affected builds, 70 focused tests and helper lifecycle checks pass. The real desktop is locked; the native fixture returns 77. Keep 5.3/5.4 pending until the independent overlay checks can execute on the unlocked desktop. See `verification.md`.
