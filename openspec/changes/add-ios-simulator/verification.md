# Verification (2026-10-16, Intel Mac, macOS 26.5.2)

Environment: Xcode 26.6 (17F113) green-installed at `~/Downloads/Xcode.app`, `xcode-select` pointing at the Command Line Tools, CoreSimulator 1051.55, iOS 26.5 runtime with iPhone 17 Pro (an app running in landscape) and iPhone 17e booted. Claude Desktop's own simulator integration refuses this setup ("Xcode command-line tools selected…").

## Executed

- Helper protocol driven directly: hello, list_devices, describe, screenshot (portrait 390×844 and landscape 874×402 at 1 px = 1 pt), display_info, Home button, swipe between home-screen pages, tap launching Settings/Photos and opening "通用", keyboard events, accessibility tree (Settings, SpringBoard, a landscape game) and point hit test, JPEG stream start/stop.
- `acecode_unit_tests --gtest_filter='IosSimulator*:XcodeLocator*:SimctlParse*'`: 30 passed; `IosSimulatorE2E.*` against the booted iPhone 17e (attach with consent, screenshot attachment, home, inspect, swipe, cmd+a, out-of-screen rejection, stream frames): passed.
- Full unit suite (first pass): 6281 passed; 11 failures and one crash, all macOS-only. Follow-up pass: fixed (remote Web proxy could not bind 127.0.0.2 on macOS — a product bug that broke remote Web on Macs; plus test-environment issues: macOS serializes `system()`, Crow stop drops the close frame, autofs `/home`, stale browser-tool expectations, long temp path truncating a settings label, dangling pointer in a channel log test). Full suite then: 6326 passed, 0 failed.
- Daemon on an isolated port: `/api/ios-simulator/status` (Xcode found via Spotlight), `/devices`, `/xcode/inspect`, `/devices/<udid>/screenshot` (PNG), PUT `/api/config/ios-simulator` (invalid path rejected, valid path saved sparsely) and `WS /ws/ios-simulator/<udid>` (device message, JPEG frames, panel Home button).
- Web UI in a browser against that daemon: Settings > Tools > iOS 模拟器 (installs, sources, devices with consent switches and boot/shutdown), header button opens the simulator tab, device chooser, live view, drag swipes pages, toolbar Home works. `pnpm test` and `pnpm build` pass.
- CI lint set (layers, size, ownership --final, includes, doc paths, map) with the new files staged: no findings. macOS packaging script tests (pkg, update zip, portable zip, release scripts, npm, verify-package) pass.

## Follow-up pass (sandbox, native view)

- Seatbelt: every helper op (hello, list_devices, describe, display_info, screenshot, ui_describe tree/point, button, touch, keys, start/stop stream, hardware keyboard, release) under the generated profile; the same profile refuses reading `~/.zshrc`, `curl https://example.com` and writing into the home folder. `IosSimulatorE2E.*` now asserts `helper.sandboxed`; passes. Daemon status reports `sandboxed:true`.
- Native view: offscreen CARenderer render of the IOSurface layer for 0/90/180/270° in flipped and unflipped geometry (orientation and clockwise sign); `ios_simulator_view_mac_smoke` on the booted iPhone 17e: IOSurface layer contents (zero copy), DOM frame, rounded corners, occlusion mask, click-through, hide/show without re-attach, unmount, DEVICE_NOT_FOUND for an unknown UDID — SMOKE_OK.
- WebSocket `frames` toggle against a daemon: `frames=0` delivers device info and no JPEG, enabling delivers frames, disabling stops them while touches still work.
- Panel in a browser with a stubbed macOS bridge: layout carries the canvas rect, corner radius and developer dir; `frames:false` sent once the native view renders (including when the socket connects late); taps map to device points from the native framebuffer; a native failure re-enables frames and JPEG resumes; leaving the session unmounts the view.
- DTUHID message builders unit tested (`dtuhid_messages_test.cpp`).

## Xcode 27 (GitHub `xcode-27` runner, arm64)

Environment: macOS 27.0.1, Xcode 27.0 (27A266a), iOS 27.0 runtime, iPhone 17e, CoreSimulator 1174.15 (helper selects `dtuhid`). `scripts/ios_simulator/live_check.py` builds the helper, the Seatbelt argv printer and the native view smoke with clang++ and runs the helper sandboxed:

- Tap on the Settings icon opens Settings (frontmost pid changes); a drag scrolls the Settings list (General cell y 391 → off screen); typing `wifi` into the search field gives the accessibility value `'wifi'`; Home returns to SpringBoard — all through dtuhidd.
- CoreDevice display info answers with `displays[].currentOrientation = rot0` for the primary display (format captured from the real reply); the helper's cached lookup reports `answered: true`.
- Screenshot, JPEG stream (32–33 frames over the page swipes), sandbox denials (home read, network, home write) and the Desktop native view smoke (SimScreen callbacks path) pass.
- Findings fixed on the way: unknown/missing orientation strings used to read as portrait; a provider that missed its 2 s window dropped rotation to Simulator.app's setting (absent when Simulator.app is not running) and blocked input every 600 ms; a cold first `describe` could exceed the daemon's 20 s timeout. Rotation now uses a kept connection, a synchronous first lookup and background refreshes; the daemon allows 90 s for the first describe.

## Not verified

- Landscape orientation on Xcode 27 (no way to rotate a simulator from CI); the `rotN` parse is the same code path as `rot0`, and older runtimes were verified in landscape locally.
- The native view inside the full Desktop app on screen (no Screen Recording permission for this session); covered by the smoke, the CARenderer render and the stubbed-bridge panel test.
- Signed/notarized packages (CI).
- Automatic panel opening from a real model's `attach` result (covered by frontend unit tests).
