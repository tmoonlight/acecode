<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The existing Windows implementation has a process-wide broker, a native worker, bounded JSON Lines, serial tools, one-action observations, screenshot metadata and model image delivery. Its process management, native implementation, tool language and support checks are Windows-specific. The desktop bundle currently declares macOS 11 compatibility and packages the daemon but no Computer Use worker.

## Goals / Non-Goals

**Goals:** Implement the existing Computer Use tool family on macOS 14+, preserve Windows behavior, make permission and installation failures actionable, and validate the production helper using disposable native fixtures.

**Non-Goals:** Linux desktop control, reproducing Codex's private runtime, bypassing TCC or protected/locked desktops, and changing the application's macOS 11 minimum. OS-denied operations fail explicitly rather than pretending to succeed.

## Decisions

1. Keep the C++ tool/protocol boundary. Give NativeBackend a platform-neutral header and implement the Mac backend in focused Objective-C++ modules for windows, AX, capture, input and pointer rendering. Do not embed AX calls into the daemon or desktop UI process.
2. The Mac broker uses posix_spawn with an isolated private socket connected to helper stdin/stdout, nonblocking bounded I/O, reaping and cancellation. A user-session lock arbitrates between ACECode instances. Parent death and normal shutdown revoke the helper; launched applications are not terminated with it.
3. The helper owns an AppKit main run loop; protocol dispatch and potentially blocking AX calls run off the main thread. AX calls have timeouts; the broker remains the final deadline. Input releases and overlay cleanup are explicit on normal stop and parent loss.
4. Use public APIs: NSWorkspace and bundle IDs for apps, CG/SC window identifiers and AX relationships for windows, ScreenCaptureKit single-frame capture, AXUIElement for controls and CGEvent for input. Bind AX windows to screenshot windows using verified process and geometry evidence; reject ambiguous matches. Do not use private AX window-number functions.
5. Keep screenshot_pixels as the public coordinate space. Store each surface's screen-point rectangle, captured pixel size and transforms, excluding capture shadows where possible. Map using actual image sizes, never a global Retina multiplier. Verify identity, geometry, focus, related surfaces and hit target again before action. Consume observations on attempted actions and require reobservation after errors.
6. Discover related sheets/popovers/menus through AX ownership and hierarchy rather than assuming that every window of one process is related. Report independent screenshot identity and bounds for captured surfaces. Missing or unsupported surfaces are explicit; avoid blind input. Secure AX values are omitted and tree traversal has node, depth, text and time budgets.
7. Preserve tool names and image attachment format. Use platform-aware key/chord examples, application identity language, scroll units and advertised accessibility actions. Implement Cmd/Option/Control/Shift, Unicode typing, clicks, drag, scroll, writable AX values and advertised secondary actions.
8. Settings expose stored enabled intent separately from supported OS, helper availability, accessibility/screen-recording permission and ready status. Reads never request permission. Authenticated explicit user actions request one named permission; no model tool can open authorization prompts. Probe with the same helper binary that performs operations so the reported identity matches the execution path.
9. A nonactivating mouse-transparent native pointer follows configured ACE/plain style and theme color. Exclude the overlay from target enumeration/hit tests and composite it at most once into returned images. Rendering uses the AppKit main thread.
10. Build the worker separately from the macOS 11 host targets and only start it on compatible OS versions. Include it alongside the desktop daemon and standalone executable; sign nested code before the outer bundle. Add package presence/architecture checks and document permission ownership for desktop and TUI launches.

## Risks / Trade-offs

- TCC attribution can depend on launch path and signing: verify both installed app and terminal paths, record actual grant behavior; never claim a read-only probe proves successful capture/input.
- A target can change between observation and action: retain one-use observations and fail on changed identity, geometry, focus or hit-testing evidence.
- Third-party AX calls can hang: messaging timeout plus cancellable helper; do not hold daemon configuration locks during probes.
- Mac keyboard layouts, mixed-DPI screens and transient windows vary: pure mapping tests plus native fixture assertions; document hardware combinations actually exercised.
- New capture APIs exclude macOS 11-13: disable this capability there while preserving all existing host functionality.

## Migration Plan

Keep existing default-disabled configuration and Windows behavior. Add response fields compatibly and preserve legacy clients. Ship helper with every Mac artifact; update tool/UI descriptions and daemon API docs. Reverting the feature leaves existing configurations readable.

## Open Questions

Native fixture verification passed on the authorized Intel/Retina desktop; see `verification.md`. Permission attribution for a Developer ID-signed installed bundle and standalone terminal launch still needs verification when those release artifacts are available. Missing OS authorization never counts as a passing test.
