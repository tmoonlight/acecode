## 1. Helper
- [x] 1.1 Framed protocol (`wire.hpp`) with incremental decoder and limits; unit tests.
- [x] 1.2 Private framework loading for the selected Xcode (CoreSimulator, SimulatorKit in both Xcode layouts), device lookup, describe.
- [x] 1.3 HID: Indigo touch / multi-touch / buttons / keyboard; DTUHID digitizer transport with liveness probe; version-based selection.
- [x] 1.4 Framebuffer capture (vImage scale + rotation, PNG/JPEG) and rate-limited deduplicated JPEG streams.
- [x] 1.5 Interface rotation (CoreDevice display info, Simulator.app orientation fallback) and touch unrotation.
- [x] 1.6 Accessibility tree / hit test via AXPTranslator with batched attribute reads and warm-up retries.

## 2. Daemon runtime and tools
- [x] 2.1 Xcode discovery and selection (configured, DEVELOPER_DIR, xcode-select, Applications, Spotlight); unit tests.
- [x] 2.2 simctl wrapper (list/boot/shutdown/install/launch/terminate/openurl/pbcopy/listapps/log); parsing tests.
- [x] 2.3 Helper client and process-wide runtime (session binding, device choice, stream fan-out).
- [x] 2.4 Gesture and keyboard planning; unit tests.
- [x] 2.5 `ios_simulator` and `ios_simulator_build` tools, device consent, plan-mode refusal, auto-approval entry; tests.
- [x] 2.6 Config section `ios_simulator` (sparse); tests.

## 3. Web and distribution
- [x] 3.1 REST routes (status, config, inspect, devices, boot/shutdown/consent, screenshot) and the panel WebSocket.
- [x] 3.2 Live panel tab, auto-open on attach, Settings section, i18n; frontend tests.
- [x] 3.3 CMake APPLE-only target, Desktop bundle copy, layer contract registration.
- [x] 3.4 macOS packaging (pkg, update zip, portable zip, CI, npm darwin) and verify-package; script tests.
- [x] 3.5 Docs: docs/ios-simulator.md, docs/daemon-api.md, CLAUDE.md.

## 4. Validation
- [x] 4.1 Unit tests and frontend tests/build pass.
- [x] 4.2 End-to-end on a green-installed Xcode 26.6 with booted simulators: describe, screenshot (portrait and landscape), tap, swipe, keyboard, accessibility, stream.
- [ ] 4.3 Verify the DTUHID path on Xcode 27 / CoreSimulator ≥ 1155.4.
- [ ] 4.4 Follow-ups: Seatbelt profile for the helper; native zero-copy simulator view in Desktop.
