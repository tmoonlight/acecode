## Why

iOS developers want the agent to run, test and visually check their apps in the Xcode iOS Simulator, and to watch and interact with the simulator inside ACECode while it works. Claude's desktop app ships a comparable feature (a sandboxed helper built on Meta's FBSimulatorControl plus simctl), but it requires `xcode-select` to point at a full Xcode. Our iOS colleagues often run a copy of Xcode outside /Applications ("green install"), so the Xcode location must be discovered automatically and also be configurable.

## What Changes

- Add `acecode-ios-sim`, a macOS-only Objective-C++ helper that loads the selected Xcode's private CoreSimulator / SimulatorKit at runtime and provides device description, framebuffer screenshots, rate-limited JPEG live streams, HID input (legacy Indigo for Xcode ≤ 26, dtuhidd digitizer for CoreSimulator ≥ 1155.4 / Xcode 27), interface-rotation handling and accessibility-tree reads. It speaks a bounded framed JSON+binary protocol over stdio and exits with its parent.
- Add Xcode discovery (configured path, DEVELOPER_DIR, xcode-select, /Applications, ~/Applications, Spotlight) and run simctl / xcodebuild from the chosen Xcode's own Developer directory, so xcode-select is never required.
- Add model tools `ios_simulator` (list/boot/shutdown/attach/detach/screenshot/tap/swipe/touch_path/touch2_path/text/key/button/open_url/launch/terminate/list_apps/inspect/logs/hardware_keyboard) and `ios_simulator_build` (xcodebuild for the simulator, optional install+launch). The control tool is auto-approved like Agent Browser but asks the user once per simulator before first use; the build tool keeps the normal permission prompt.
- Add a live simulator panel (SidePanel preview tab) with mouse/keyboard/paste interaction, opened automatically on attach, and a Settings > Tools > iOS Simulator section (enable, Xcode selection with custom path, device consent, stream quality).
- Package the helper only in macOS artifacts (app bundle, portable zip, pkg, update zip, npm darwin packages); other platforms never build or ship it.

## Capabilities

### New Capabilities
- `ios-simulator`: Xcode selection, simulator control helper, model tools, device consent, live panel and macOS-only distribution.

### Modified Capabilities

None.

## Impact

New code under src/adapters/tool/ios_simulator/ (daemon runtime + helper), src/adapters/tool/ios_simulator_tool.*, src/apps/web/routes/routes_ios_simulator.cpp, config section `ios_simulator`, a permission auto-allow entry, web UI components, CMake (APPLE-only target), macOS packaging scripts and their tests, docs/ios-simulator.md and docs/daemon-api.md. The helper uses private Apple frameworks only on the user's machine at runtime; nothing private is linked at build time. Protocol shapes are adapted from Meta's idb (MIT; license included next to the helper sources).
