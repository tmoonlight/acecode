## Context

The Windows shell selects a monitor, reads `MONITORINFO::rcWork`, and clamps the
requested `1280x820` size before creating its off-screen host. The WebView
wrapper later treats that request as a 96-DPI logical size, scales it to the
window DPI, adds the native frame, and calls `SetWindowPos` again. The existing
work-area clamp runs before this final resize, so high-DPI systems can end up
with an actual outer window larger than the taskbar-reduced work area.

The selected work rectangle is also captured early in startup. A taskbar or
display-layout change before first paint can make that snapshot stale.

## Goals / Non-Goals

**Goals:**

- Guarantee that the actual Windows outer window shown to the user fits inside
  the current selected monitor work area after all WebView DPI scaling.
- Keep the configured DPI-aware safe margins and center the final rectangle.
- Cover off-screen custom-host and WebView-owned fallback startup paths.
- Keep final geometry calculation deterministic and unit-testable.

**Non-Goals:**

- Persist user-resized or maximized window state.
- Constrain later user-driven moves, resizes, or maximization.
- Change macOS, Linux, WebUI layout, or browser fallback behavior.

## Decisions

1. Add a pure helper that takes the actual post-scaling outer-window size and
   an absolute work-area rectangle, applies the existing DPI-aware safe-size
   policy, and returns a centered absolute rectangle. This tests both size and
   taskbar-relative placement without Win32 handles.
2. Immediately before the first visible show, resolve the startup monitor again
   from the stored work-area center and call `GetMonitorInfoW` again. This uses
   the latest `rcWork`, including a resized or relocated taskbar.
3. Read the actual HWND rectangle after `webview::set_size()` has completed, run
   it through the pure helper, and apply the final position and dimensions with
   one `SetWindowPos`. Post-scaling enforcement is chosen over predicting the
   wrapper's DPI and frame math because it covers both wrapper upgrades and
   externally owned versus wrapper-owned windows.
4. Track first-show centering separately from whether the custom off-screen host
   was created. The fallback WebView-owned window therefore receives the same
   final geometry correction even when off-screen hosting fails.
5. Retain the earlier size clamp as an initialization bound. The new final pass
   is the authoritative taskbar-safety guarantee.

## Risks / Trade-offs

- [The WebView-owned fallback may be briefly visible before final correction] →
  Apply the same actual-size fit immediately after the initial size request and
  repeat it at first show with the latest work area.
- [Display topology can change so the stored monitor center no longer identifies
  the same display] → Use `MONITOR_DEFAULTTONEAREST` and fall back to the current
  window or active monitor.
- [Very small work areas leave little content space] → Preserve the existing
  positive-dimension normalization and safe-margin degradation behavior.

## Migration Plan

No persisted state or schema changes are required. Shipping the updated Desktop
binary changes only its next startup geometry; rollback restores the prior
startup behavior.

## Open Questions

None.
