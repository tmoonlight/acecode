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

## Notes and limits

- The tool uses Apple's private CoreSimulator / SimulatorKit frameworks from your Xcode at runtime (as idb and Xcode's own tools do). Xcode updates can change them; ACECode reports precise errors when something is missing.
- Xcode 27 (CoreSimulator 1155.4+) delivers input through `dtuhidd`; earlier versions use the legacy Indigo channel. Set `ACECODE_IOS_SIM_HID=indigo|dtuhid` to override for diagnosis.
- On Intel Macs, reading the framebuffer waits for the simulator's GPU work, so during long animations the live view and screenshots can lag (Apple's `simctl io screenshot` behaves the same). Input is not affected.
- Landscape apps are shown upright; on runtimes older than Xcode 27 the orientation comes from Simulator.app's device window setting.
