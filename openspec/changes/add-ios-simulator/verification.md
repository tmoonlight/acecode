# Verification (2026-10-16, Intel Mac, macOS 26.5.2)

Environment: Xcode 26.6 (17F113) green-installed at `~/Downloads/Xcode.app`, `xcode-select` pointing at the Command Line Tools, CoreSimulator 1051.55, iOS 26.5 runtime with iPhone 17 Pro (an app running in landscape) and iPhone 17e booted. Claude Desktop's own simulator integration refuses this setup ("Xcode command-line tools selected…").

## Executed

- Helper protocol driven directly: hello, list_devices, describe, screenshot (portrait 390×844 and landscape 874×402 at 1 px = 1 pt), display_info, Home button, swipe between home-screen pages, tap launching Settings/Photos and opening "通用", keyboard events, accessibility tree (Settings, SpringBoard, a landscape game) and point hit test, JPEG stream start/stop.
- `acecode_unit_tests --gtest_filter='IosSimulator*:XcodeLocator*:SimctlParse*'`: 30 passed; `IosSimulatorE2E.*` against the booted iPhone 17e (attach with consent, screenshot attachment, home, inspect, swipe, cmd+a, out-of-screen rejection, stream frames): passed.
- Full unit suite: 6281 passed; 11 failures and one crash (`RemoteWebTcpProxy`) are in remote-web proxy, WebSocket client, cross-process state lock, sandbox path, TUI settings rendering, channel state log and macOS browser-tool registration tests that do not touch this change (macOS host only; CI runs unit tests on Linux).
- Daemon on an isolated port: `/api/ios-simulator/status` (Xcode found via Spotlight), `/devices`, `/xcode/inspect`, `/devices/<udid>/screenshot` (PNG), PUT `/api/config/ios-simulator` (invalid path rejected, valid path saved sparsely) and `WS /ws/ios-simulator/<udid>` (device message, JPEG frames, panel Home button).
- Web UI in a browser against that daemon: Settings > Tools > iOS 模拟器 (installs, sources, devices with consent switches and boot/shutdown), header button opens the simulator tab, device chooser, live view, drag swipes pages, toolbar Home works. `pnpm test` and `pnpm build` pass.
- CI lint set (layers, size, ownership --final, includes, doc paths, map) with the new files staged: no findings. macOS packaging script tests (pkg, update zip, portable zip, release scripts, npm, verify-package) pass.

## Not verified

- DTUHID input on Xcode 27 / CoreSimulator ≥ 1155.4 (no such toolchain on this machine).
- Signed/notarized packages (CI).
- Automatic panel opening from a real model's `attach` result (covered by frontend unit tests).
