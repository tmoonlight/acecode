# iOS Simulator

ACECode can run, test and visually check iOS apps in the Xcode iOS Simulator on a Mac. The agent sees the screen, taps, swipes, types, presses hardware buttons, installs and launches apps, reads logs and inspects the accessibility tree; you watch and interact through a live panel in the side panel. Simulators only: a physical iPhone cannot be driven.

## Requirements

- macOS 14 or later and a full Xcode (not just the Command Line Tools) with at least one iOS simulator runtime.
- Xcode must have been opened once (or `xcodebuild -runFirstLaunch` run) so the simulator components are installed.
- The feature ships only in macOS packages (`acecode-ios-sim` next to the daemon / inside `ACECode.app/Contents/MacOS`).

## Choosing Xcode

`xcode-select` does not need to point at Xcode. ACECode looks, in this order, for: the path configured in **Settings > Tools > iOS Simulator**, `DEVELOPER_DIR`, the xcode-select target (if it is a full Xcode), then the newest Xcode found in /Applications, ~/Applications or by Spotlight. A copy unpacked anywhere (for example `~/Downloads/Xcode.app`) is found by Spotlight; if not, pick it with **Browse…** or type its path (`…/Xcode.app` or `…/Xcode.app/Contents/Developer`). **Rescan** repeats discovery. The page also shows whether the simulator components are installed and which input transport the loaded CoreSimulator uses.

## How the agent uses it

The model tool `ios_simulator` accepts actions such as `list_devices`, `boot`, `attach`, `screenshot`, `tap`, `swipe`, `touch_path`, `touch2_path`, `text`, `key`, `button`, `open_url`, `launch`, `terminate`, `list_apps`, `inspect`, `logs`, `hardware_keyboard` and `detach`. Coordinates are device points in the current orientation, identical to the screenshot pixels. `ios_simulator_build` builds an Xcode project or workspace for the simulator with the selected Xcode and can install and launch the result.

- The first time the agent wants to control a simulator, ACECode asks **允许 ACECode 控制模拟器…?**. The answer is remembered per simulator and can be changed under Settings > Tools > iOS Simulator. Unanswered or timed-out questions count as "no".
- `ios_simulator` does not ask for permission on every tap (like the built-in browser). `ios_simulator_build` runs your project's build scripts and therefore uses the normal tool permission prompt.
- In plan mode only observing actions (screenshot, inspect, logs, list) are available.

## The live panel

`attach` opens a **Simulator** tab in the side panel; you can also open it manually. Click or drag on the screen to touch, use the scroll wheel to scroll, click the screen to give it keyboard focus and type or paste (Chinese and other non-ASCII text is pasted through the simulator pasteboard). The toolbar has Home, Lock and Save screenshot. Stream frame rate, quality and scale are configurable in Settings.

In ACECode Desktop on macOS the screen is drawn natively: the simulator's own framebuffer is shown in a layer over the panel, at full resolution and frame rate, without JPEG encoding or copies (and without the Intel readback stall described below). Touch and keyboard input still go through the panel. A web browser (or a remote Web UI) uses the JPEG stream, and Desktop falls back to it automatically when the native view cannot attach; **Reconnect** retries the native view. The native view keeps the CoreSimulator session of the first Xcode it used; restart Desktop after switching Xcode for it to follow.

## Notes and limits

- The tool uses Apple's private CoreSimulator / SimulatorKit frameworks from your Xcode at runtime (as idb and Xcode's own tools do). Xcode updates can change them; ACECode reports precise errors when something is missing.
- The helper runs inside a macOS Seatbelt profile: no IP networking, no reading of files under /Users, /Volumes and your home folder except the selected Xcode, the ACECode bundle, simulator data and logs and the simulator preference files, and writes only to simulator data, logs and the temporary/cache folders. If `sandbox-exec` cannot apply the profile (for example when ACECode itself runs inside a sandbox) the helper runs unconfined; Settings shows a warning and the daemon log says why. `ACECODE_IOS_SIM_SANDBOX=0` turns it off for diagnosis.
- Xcode 27 (CoreSimulator 1155.4+) delivers input through `dtuhidd`; earlier versions use the legacy Indigo channel. Set `ACECODE_IOS_SIM_HID=indigo|dtuhid` to override for diagnosis. Verified on Xcode 27.0 with an iOS 27.0 simulator (GitHub's `xcode-27` image): taps, drags, typing and hardware buttons through `dtuhidd`, the CoreDevice orientation lookup, the live stream, the helper sandbox and the Desktop native view.
- On Intel Macs, reading the framebuffer waits for the simulator's GPU work, so during long animations the JPEG stream and screenshots can lag (Apple's `simctl io screenshot` behaves the same). Input and the Desktop native view are not affected.
- Landscape apps are shown upright. Xcode 27 runtimes report the orientation themselves (the lookup is cached and refreshed in the background); on older runtimes it comes from Simulator.app's device window setting.

## Checking a new Xcode

The `ios-simulator-live` GitHub workflow (run it manually, or push a `ci/ios-simulator-live*` branch) builds the helper on a hosted Mac — by default GitHub's `xcode-27` image — boots a simulator and checks that taps, drags, typing and hardware buttons really take effect (read back through the accessibility tree), plus screenshots, the live stream, the helper sandbox and the Desktop native view. Locally: `python3 scripts/ios_simulator/live_check.py --json-include <dir containing nlohmann/json.hpp> [--developer-dir …/Contents/Developer]`.
