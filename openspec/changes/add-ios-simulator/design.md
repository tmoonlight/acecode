## Context

Reverse engineering Claude Desktop's iOS Simulator feature showed: a Swift helper (`claude-ios-sim`, FBSimulatorControl inside) launched under a Seatbelt profile, a 5-byte tag framed stdio protocol, simctl for install/launch/openurl/boot, xcodebuild for builds, a native zero-copy IOSurface view in the desktop window with an H.264 stream fallback, per-device consent and an MCP `control` tool. ACECode already has the building blocks: a helper-process pattern (computer use), auto-approved isolated tools (Agent Browser), image attachments, Crow WebSockets, preview tabs and a settings shell.

## Goals / Non-Goals

**Goals:** feature parity for driving simulators (observe, touch, type, buttons, apps, logs, accessibility, builds), a live interactive panel, robust Xcode discovery including green installs, macOS-only packaging, no build-time dependency on Xcode private frameworks.

**Non-Goals:** physical devices, Android emulators, tvOS trackpad / iPhone Duo hinge, vendoring FBSimulatorControl (needs xcodegen/xcodebuild and would dominate the build), a Seatbelt sandbox for the helper (follow-up), the native zero-copy desktop view (follow-up; see Decision 6).

## Decisions

1. **Own Objective-C++ helper instead of FBSimulatorControl.** The needed surface (device lookup, Indigo/DTUHID HID, framebuffer, AXPTranslator) is ~1.5k lines when written against runtime-resolved protocols. All private classes are looked up with NSClassFromString / dlsym and messaged through protocols, every call is wrapped in @try, so no link-time symbol pins an Xcode layout. Indigo structures and DTUHID message shapes follow idb (MIT, attributed).
2. **Framing.** `u32le body | u32le json_len | json | binary` for requests, responses, events and frames (header-only `wire.hpp`, shared by daemon and helper, unit tested). The daemon side keeps one helper per selected Developer directory, restarts it on Xcode changes or exit, and multiplexes requests by id; frames fan out to subscribers.
3. **Xcode selection.** configured path > DEVELOPER_DIR > xcode-select (only if it is a full Xcode) > newest of /Applications, ~/Applications and Spotlight (`kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'`). simctl and xcodebuild are executed from `<Developer>/usr/bin`, which resolves its own Xcode. SimulatorKit is loaded from `Contents/SharedFrameworks` (Xcode 27) or `Contents/Developer/Library/PrivateFrameworks` (≤ 26).
4. **HID transport.** Indigo on CoreSimulator < 1155.4; dtuhidd digitizer XPC (via `xpc_endpoint_create_mach_port_4sim`) from 1155.4, where the guest drops Indigo input. `ACECODE_IOS_SIM_HID` overrides for diagnosis. Gestures are planned in the daemon (pure, tested: interpolation, edge gestures, two-finger paths, US key map) and executed in the helper so timing is not subject to IPC jitter. Non-ASCII text uses `simctl pbcopy` + Cmd+V.
5. **Coordinates = interface-orientation points.** Screenshots for the model are rendered at 1 px = 1 pt and turned to the interface orientation; touches are unrotated in the helper. Rotation comes from the CoreDevice display-info provider (Xcode 27 runtimes) or, on older runtimes, from Simulator.app's per-device window orientation preference.
6. **Framebuffer reads.** A synchronizing IOSurface lock is required (AvoidSync returns stale CPU memory). On Intel Macs the lock waits for the simulator's GPU work and can stall for the length of an animation — `simctl io screenshot` behaves the same — so captures/streams run on per-device queues separate from input. Scaling/rotation use vImage on the private copy; JPEG via ImageIO. A native IOSurface CALayer view in Desktop would avoid readback entirely and is the planned follow-up.
7. **Consent and permissions.** `ios_simulator` is auto-approved (like browser tools) and gated by a per-UDID consent question (remembered in state.json, revocable in Settings, timeouts never count as consent, yolo grants implicitly). `ios_simulator_build` runs project scripts and keeps the ordinary permission prompt. State-changing actions are refused in plan mode.
8. **Panel transport.** `WS /ws/ios-simulator/<udid>` (daemon token auth like the session WS) carries binary JPEG frames with a small JSON header and accepts touch/button/key/text messages; stream start runs off Crow's IO thread. One helper stream per device is shared by all viewers.
9. **Distribution.** CMake builds `acecode-ios-sim` only `if(APPLE)`; Desktop copies it into `ACECode.app/Contents/MacOS`, standalone packages install component `ios_simulator_runtime`; pkg/update/portable/npm/CI checks require it on macOS only.

## Risks / Trade-offs

- Private APIs change with Xcode releases → runtime lookups, explicit errors, version-gated transport; DTUHID path is untested on this machine (Xcode 26.6) and must be verified on Xcode 27.
- Intel readback stalls → acceptable degradation; follow-up native view.
- Accessibility translator warm-up: first request after boot may be empty → three retries.
