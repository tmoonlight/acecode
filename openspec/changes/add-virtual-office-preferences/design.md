## Context

The office is a Windows WebView2 or macOS WKWebView window driven by Web UI session snapshots. Closing currently destroys the only instance. Bridges are registered after main navigation; useDesktopOffice checks once on mount, creating a first-navigation race on macOS.

## Decisions

1. A GUI-owned shared service retains bridge bindings and the last snapshot independently of each disposable native window. Reopening creates a fresh controller; old async callbacks only weakly reference the old controller. Register the service before navigation.
2. Persist `enabled` and `welcome_seen` atomically in a separate `office.json` beside the existing scale settings, outside the migratable ACECode data directory. Default disabled, explicitly confirmed by user. ACECODE_DESKTOP_PET=0 remains an override.
3. Native version eligibility is >=0.9.37, excluding earlier versions. Persist welcome acknowledgement when displayed; enabling through settings also acknowledges it. Do not bump the project version for this change.
4. One frontend owner manages preference state and live controller lifetime. Menu/settings receive that state; native close events update it. Stop polling and subscriptions when disabled. Unsupported Web/Linux shows a disabled settings switch with a short explanation and no actionable quick-menu entry or invitation.
5. Only while the invitation is open, request the already embedded HTML over the native bridge and render it in an `allow-scripts` sandboxed iframe with local demo snapshots. No video/GIF, network, real sessions, duplicated HTML bundle, or surviving animation after dismissal. Honor reduced motion and provide pause/play.
6. Invitation uses shared Modal, exact requested description, explicit close/later/enable buttons, no backdrop dismissal, and avoids other startup blocking dialogs. Existing settings tokens and a simple 20-unit desk/room SVG are reused.

## Validation

Portable native service tests cover default, version gate, persisted choice, one-time claim, close/reopen, stale callbacks and save failure. Web tests cover menu/nav ordering, bridge state and snapshot lifecycle; browser checks exercise settings and invitation, preview isolation and reduced motion. Run pnpm test/build, focused native compilation/tests, existing office browser regression, OpenSpec strict validation and diff checks. macOS build/runtime remains a documented handoff when unavailable on Windows.
