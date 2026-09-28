<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The UOS session reports `gtk-xft-dpi=122880` (120 DPI in GTK's 1024 units) and GTK window scale 1. Deepin's `scale-factor` preference was initially 1.25 and later 1.0 while the effective font DPI remained 120. WebKitGTK 2.38.5 enlarges CSS text with font DPI while leaving CSS pixel dimensions at window scale 1. ACECode uses fixed CSS pixel dimensions throughout the Desktop page. Both a local probe and the real Desktop window confirm that normalizing this process's GTK font DPI to 96 and applying a 1.25 whole-page zoom makes text and control geometry scale together.

## Goals / Non-Goals

**Goals:**
- Correct UOS/Deepin X11 Desktop WebView proportions at fractional scale without touching the user's stored display settings.
- Preserve 100% and non-Deepin behavior and avoid double scaling when GTK supplies an integer window scale.
- Update the correction when UOS or GTK scale inputs change while the window is open.

**Non-Goals:**
- Change font preferences within ACECode or apply custom zoom to arbitrary web pages in the agent browser.
- Replace the system WebKitGTK runtime or change system-wide UOS scaling settings.

## Decisions

1. Read the native `gtk-xft-dpi` with `gdk_screen_get_setting` and derive the effective scale from DPI / 96. The native value remains available beneath the process-local GtkSettings override. Do not gate on the Deepin `scale-factor` preference: the reproduced 1.0/120 DPI combination otherwise silently disables the fix. Only enable the workaround in a Deepin X11 session; absent or invalid native DPI retains WebKit defaults.
2. Set this process's GTK font DPI to 96 DPI and set the main WebKitWebView's whole-page zoom to `effective_scale / gtk_widget_scale_factor`. This aligns fonts and CSS geometry; simply zooming the page would enlarge already-scaled text a second time. The change stays inside ACECode's Desktop process; it does not write GSettings or X resources. Since GTK font DPI is process-wide, apply a local CSS font-size percentage to ACECode's native tray menu labels to preserve their original text size.
3. Observe native XSettings property/manager events, GTK font DPI, and widget scale changes. Defer native event reads to an idle callback so GDK can update its cache first. GtkSettings property notifications alone cannot observe external updates once an application override is active. Guard the process-local write against recursion, remove the pending callback and handlers on teardown, and use `gtk_settings_reset_property` to release the override. A pure policy helper covers normal, fractional, integer, and invalid DPI inputs.

## Risks / Trade-offs

- [GTK's font DPI is per process, not per WebView] → Limit the override to UOS/Deepin X11, release the override on teardown, and compensate the tray menu labels with local CSS. A GTK probe checks their rendered font metrics at 125%.
- [UOS can retain a stale display preference] → Read the native value that GTK actually consumes, independently of the display preference and process-local override.
- [WebKitGTK versions can interpret font DPI differently] → Verify the installed 2.38.5 runtime with actual Desktop screenshots. Dynamic native DPI changes are checked on an isolated X server so the user's display settings are not changed.
