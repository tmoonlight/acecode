## Why

Windows Desktop computes an initial size from the monitor work area, but the
WebView2 wrapper subsequently DPI-scales and reapplies that size. On scaled
displays this later native resize can undo the earlier clamp and leave the
startup window beneath the taskbar.

## What Changes

- Re-read the selected monitor work area immediately before the Windows main
  window is first shown.
- Fit the actual post-WebView2 outer-window dimensions inside that current work
  area, preserving DPI-scaled safe margins and excluding the taskbar.
- Center the final fitted rectangle in the work area for both the normal
  off-screen WebView2 host and the fallback WebView-owned window.
- Add focused regression coverage for final rectangle sizing and positioning
  with a taskbar-reduced work area.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `desktop-frameless-window`: Strengthens the Windows first-show requirement so
  the final DPI-scaled outer window remains inside the live monitor work area
  and cannot be covered by the taskbar.

## Impact

- Affects Windows native window sizing and first-show placement in
  `src/desktop/web_host.cpp` and the platform-neutral geometry helper in
  `src/desktop/window_size.hpp`.
- Adds focused native-geometry unit tests under `tests/desktop/`.
- Does not change daemon APIs, stored configuration, WebUI breakpoints, macOS,
  or Linux window behavior.
