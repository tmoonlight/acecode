# Verification record

## Environment

2026-09-20–21; macOS 26.5.2, Intel x86_64, SDK 26.5. CMake build tree: `build/macos-x64-agent-browser-test`. Native helper has Mach-O minimum OS 14.0. An arm64 helper also compiled and linked against the same SDK; it was not executed on this Intel host. Native interaction ran on the built-in display at 1440×900 screen points, AppKit backing scale 2.0; the current keyboard-layout preference was `com.apple.keylayout.PinyinKeyboard`.

## Passed

- CMake targets: `acecode-desktop`, `acecode_unit_tests`, `acecode-computer-use`, `computer_use_broker_smoke`, and `computer_use_native_mac_smoke`.
- 70 focused GoogleTests: filter `ComputerUse*:*ComputerUse*:*ToolImage*`. Includes coordinate limits/scaling, action/schema gates, settings persistence, authenticated HTTP permission validation and model image delivery.
- Production POSIX broker with its isolated fixture: ownership, appearance continuity, 8 MiB response, cancellation, disable/re-enable, crash, invalid JSON reply, 20-second timeout, and daemon startup with stdin/stdout closed.
- Actual helper lifecycle suite: malformed/oversize/versioned requests, forged observation, exclusive interprocess lease, parent death, and permission probes that do not take the control lease. Registered CTest `computer_use_macos_helper_lifecycle` passed.
- `pnpm test` and `pnpm build`; production regex compatibility check passed. A temporary component preview using the real generated CSS confirmed the permission rows, pointer cards and simulated permission/enable state transitions. This was UI verification with stubbed API responses, not an OS permission grant.
- Seven npm packaging tests cover all six platform artifacts and missing/empty Computer Use sidecars. Five affected package CTests passed: verification script units, portable ZIP units, release-script contracts, PKG installation contracts, and verification flow contracts.
- `verify-package --skip-build --target all` passed all 20 checks on the actual built desktop/CLI payload: standalone/bundled helper adjacency, models.dev and seed resources, CLI version/resource lookup, and isolated desktop launch/termination.
- Actual update ZIP created and extracted with `macos_create_update_zip.sh`; both helper copies are executable and identical. App-bundle helper also matches the CMake output byte-for-byte.
- OpenSpec strict validation and `git diff --check`.
- Before native acceptance, the locked desktop was explicitly rejected by the helper with `desktop_locked`; public NSWorkspace metadata identified `com.apple.loginwindow` as frontmost.

## Native acceptance — task 4.2 passed

On 2026-09-21, the user unlocked the desktop and granted Accessibility. The actual bundled helper reports both Accessibility and Screen Recording granted, with `ready: true`. The CMake-built native fixture then completed with exit **0**:

```sh
build/macos-x64-agent-browser-test/tests/computer_use_native_mac_smoke.app/Contents/MacOS/computer_use_native_mac_smoke --run-owned-window
```

The fixture operates only on its own disposable windows. The passing native run exercised real screenshot pixels against AX coordinates; protected-field filtering; element and coordinate clicks; Unicode typing; Command, Option, Control and Shift chords; AX value/invoke/toggle; horizontal and vertical scroll; drag and mouse-button release; one-use observations; stale geometry and changed-focus rejection including another window of the same process; sheet and context-menu capture/input; ACE/plain pointer pixels and cleanup in the composed screenshots; and exact app discovery/launch.

**Correction, 2026-09-24:** The pointer assertions above sampled `pointer_composite` output. They did not independently capture the native overlay or verify its visibility to a person on the desktop. They must not be treated as acceptance of the real on-screen pointer. The pointer visibility follow-up in `tasks.md` reopens that coverage.

This run exposed and fixed a real occlusion bug: the system Dock can own a screen-sized transparent backing window. The helper now ignores that specific system-owned backing rectangle while retaining the system-wide AX hit test. The native fixture also verifies that an actual opaque input panel still blocks the click. Fixture improvements include refreshing the bundled helper when its binary changes, checking bitmap samples in their actual color profile, providing the standard Edit menu for Cmd+A, reobserving explicitly reported AX/WindowServer transitions, and waiting for the menu action callback rather than assuming its animation finishes in 150 ms. Input actions are never retried by the fixture.

Optional `ACECODE_MAC_FIXTURE_ARTIFACTS` saves only the fixture's own screenshots. Main-window and independent context-menu PNGs were inspected during this run. Prior permission-denied exit 77 and locked-desktop rejection remain evidence of the failure paths, not native acceptance.

## Final artifact refresh

After the fixes, the final fixture passed again with exit 0. The rebuilt production targets, all 70 focused GoogleTests, the actual-helper lifecycle CTest, and all 20 package checks also passed. The Intel helper has SHA-256 `0b564c4fb514bb4b855407f284afbab6536253a9f9d22a9e97062d3ad003c821`, identical in the build root, fixture bundle, desktop bundle, and both staged package locations. The final arm64 helper compiled and linked successfully; `lipo` confirmed arm64 and `vtool` confirmed minimum macOS 14.0 for both architectures.

Local evidence from this run:

- Native acceptance: `/tmp/acecode-macos-computer-use-native-final.log`.
- Focused regression results: `/tmp/acecode-macos-computer-use-regression-final.log`.
- Package verification: `/tmp/acecode-macos-computer-use-package-final.log`.
- Refreshed staging: `/tmp/acecode-macos-computer-use-package-final`.
- Created and extraction-verified update ZIP: `/tmp/acecode-macos-computer-use-update-final.zip`.

## Coverage limits

Real mixed-display interactions, additional keyboard layouts, Apple Silicon execution and Developer ID-signed installed-app/TUI permission attribution remain unverified. Single-display Retina interactions passed; negative origins and differing image scales have deterministic tests. Release signing now includes the standalone helper and existing nested-code signing covers its bundled copy; no release signing, notarization or publishing was performed here. Existing Windows native code was preserved, but Windows runtime tests were not executed on this host.

## Pointer visibility follow-up — 2026-09-24

Source inspection confirmed two action-feedback gaps: `press_key`/`type_text` never showed the Mac overlay, and clicks reset their pressed state immediately after posting mouse up, potentially before AppKit rendered a frame. The changes add focused-control feedback, request an immediate overlay redraw and preserve the visual pressed state for the existing Windows backend's 250 ms interval. Actual mouse-up, release and cancellation are not delayed. These are ACECode compatibility fixes, not a claim to reproduce Codex's private cursor implementation. The inspected Codex Mac client contract and remaining differences are recorded in `docs/computer-use.md`.

The native fixture now independently identifies its child helper's on-screen WindowServer window, captures only that overlay with ScreenCaptureKit and checks its pixels, hotspot, foreground preservation and removal. It also clicks through an already-visible overlay and starts keyboard checks with the previous overlay released. Its capture does not use `pointer_composite` or images returned by the Computer Use tool. Optional artifacts include `native-overlay.png` alongside the disposable fixture's images.

Executed checks:

- Rebuilt `acecode-computer-use`, `computer_use_native_mac_smoke`, `acecode_unit_tests` and `acecode-desktop` successfully.
- All 70 focused tests (`ComputerUse*:*ComputerUse*:*ToolImage*`) passed; the actual-helper lifecycle CTest passed.
- OpenSpec strict validation and `git diff --check` passed.
- Build-root, fixture-bundle and desktop-bundle helpers match SHA-256 `ed1626979b51624c66ee73a3f2ac68551f96a9ea4cc25574cde803b23b070f38`.
- The native fixture returned **77** because the desktop was locked. This is **not** a pass. The newly added independent overlay checks, perceived click feedback and keyboard feedback still require an unlocked run; follow-up tasks 5.3/5.4 remain unchecked pending that acceptance.

Local logs: `/tmp/acecode-macos-pointer-build-20260924.log`, `/tmp/acecode-macos-pointer-fixture-final-20260924.log`, `/tmp/acecode-macos-pointer-unit-20260924.log`, `/tmp/acecode-macos-pointer-lifecycle-20260924.log` and `/tmp/acecode-macos-pointer-native-20260924.log`.
