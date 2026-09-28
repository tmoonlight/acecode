<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The referenced prototype (`/home/shaouos/ttt`, session `01a0d841-fa49-7d73-906a-c14de6ae76a5`) uses DTK 5 / Qt 5.11 and a transparent titlebar. ACECode already owns its titlebar and window operations in web content, and must retain WebKitGTK 4.0. The previous scaling fix remains uncommitted in this worktree.

## Goals / Non-Goals

**Goals:** Isolate the distribution-specific dependencies at configure and compile time; preserve GTK event, focus, drag, tray and WebView ownership; build dedicated packages on the existing GLIBC 2.28 baseline.

**Non-Goals:** Replace WebKit with QtWebEngine, create a second titlebar, change host packages, or publish a release during local validation.

## Decisions

1. `ACECODE_DEEPIN` defaults to OFF, rejects non-Linux targets, and defines a build marker for update identity. Only Desktop links Qt Widgets and DTK Widget when enabled. TUI/daemon receive the identity without Qt linkage. Fractional scaling activation also requires this macro plus the existing runtime desktop/backend checks.
2. Attach a DTK platform window handle to a non-owning `QWindow::fromWinId` wrapper around the existing GTK X11 window. A local prototype on this UOS system confirmed enabled DTK/no-titlebar and native radius 8. The window manager supplies frame effects; GTK retains native window ownership and all control/close callbacks. WebView has already mapped its GTK window at this point: flush the DTK properties and remap the window so the compositor applies corner clipping on its first visible frame. This avoids GTK-in-Qt embedding and overlapping native controls. Native Wayland skips X11 integration; XWayland is supported through its actual GDK X11 backend, irrespective of CPU architecture.
3. Keep Qt headers in a dedicated Desktop translation unit with Qt keyword macros disabled. Discover Qt/DTK in a child CMake directory: DTK's package configuration otherwise adds global definitions to previously created daemon/test targets. Initialize Xlib threading at process entry, before GTK uses any display resources; the older UOS Xlib crashes if Qt initializes threading after GTK has created resource databases. Require successful early initialization, then initialize the DTK application lazily only after runtime checks, followed by the GTK scale controller. Service Qt through the common GLib dispatcher; release wrapper/handle and signal owners before `request_quit` destroys the GTK host, while preserving them across show/hide. A native GTK delete-event may already have removed widget signals; check the connection before disconnecting it. The user explicitly confirmed that DTK must supply only rounded corners and shadows: add no Qt widgets or input handlers, and leave the native move/resize policy unchanged for GTK's existing operations.
4. Retain the Buster container baseline and build pinned DTK 5 sources against its Qt 5.11 in a separate CI SDK prefix. Current Deepin repositories can require newer Qt and must not silently raise the package ABI. Record immutable source revisions and hashes. x64/arm64 include Desktop; the existing ARMv7 CLI-only coverage remains CLI-only.
5. Rename the old release family to `linux-deepin` throughout workflow dependencies, archives, completeness checks and docs. Use a separate Deepin update target; do not fall back to a generic Linux manifest entry. Dedicated update publication is separate from the archive migration and is not introduced here.

## Risks / Trade-offs

- DTK depends on the installed Deepin platform plugin and compositor -> verify native properties and screenshot locally; require DTK 5 runtime in dedicated-package documentation.
- Qt and GTK share a process -> keep ownership separate and verify focus, window state, resize, close-to-tray and shutdown with the real Desktop.
- ARM execution is unavailable locally -> retain architecture-independent code and CI native ARM64 build/ABI gates; report this validation limit.
- No published Deepin update manifest yet -> report no compatible update instead of replacing this build with a generic Linux artifact.

## Migration Plan

Enable `-DACECODE_DEEPIN=ON` only in the dedicated jobs and explicit local builds. Release the renamed archives after normal CI/release gates. Rollback is to disable the option and restore the previous archive matrix; generic Linux, Windows and macOS require no migration.

## Validation results

- Built generic Desktop with the option OFF and dedicated Desktop with it ON. Generic Desktop and both daemon variants have no Qt/DTK linkage; the dedicated Desktop resolves system WebKitGTK 4.0 and DTK 5. Both dedicated executables retain the GLIBC 2.28 symbol ceiling. A non-Linux configure probe rejects the option.
- Built all three pinned DTK 5.2 SDK modules in an isolated Debian Buster root filesystem. Compiled the actual frame adapter and linked the required DTK APIs against that SDK and Qt 5.11.3, including the remap synchronization call.
- Passed 16 focused C++ tests covering DPI policy, manifest selection and versions; passed 6 release-asset verifier tests. Separate ON/OFF probes confirmed `linux-deepin-x64` versus `linux-x64` identity.
- Parsed the workflow and checked its Deepin matrix and release dependency. All nine Deepin shell steps passed `bash -n`; actionlint passed with only the pre-existing paused npm job's constant-false expression excluded.
- On real UOS X11, the final Desktop advertises `_DEEPIN_NO_TITLEBAR=1`, has compositor shadow data and reports radius 8. The final screenshot confirms native rounded corners, shadow and the existing web controls. The user also confirmed the visual result.
- The isolated X11 display verified web maximize/restore/minimize, drag, edge resize, keyboard input and close. Its generic KWin instance did not advertise Deepin frame support, so it was used for interaction and DPI checks only. Final continuous native DPI changes 120 → 144 → 96 → 120 produced page zoom 1.25 → 1.5 → 1.0 → 1.25. Real UOS also verified maximize/restore/minimize and normal close, with no stale GTK signal-handler warning after the lifetime fix.
- The final Release build on real UOS accepted compositor move/resize from
  (320,105,1280,820) to (120,120,1000,720) and restored its original geometry.
  Real UOS XTest pointer gestures were inconclusive, although the isolated
  display accepted them. Physical mouse gestures on the locked real desktop
  remain unverified; the automated result cannot certify that input path.
- ARM64/ARMv7 execution, an actual XWayland session and the full GitHub packaging matrix remain unexecuted locally. No release was published.
