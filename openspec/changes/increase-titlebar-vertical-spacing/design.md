<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The top bar currently uses a 30 px height and derives a 24 px control box by subtracting 6 px. Its blank drag band is independently fixed at 44 px. AppKit owns macOS standard-button geometry; the web row reserves 80 px on the left outside native fullscreen. Increasing the row alone would also enlarge all control boxes while the drag band should continue to follow the actual row if it grows beyond 44 px.

## Goals / Non-Goals

Add precisely 5/6 px to the existing top/bottom space. Preserve icon shapes, sizes, button hit targets, left/right groups, sidebar background and wallpaper boundary. Preserve native macOS button positions and fullscreen lifecycle. Do not reparent or resize AppKit views.

## Decisions

Set the shared height token to 41 px. Keep control size at 24 px; apply 5 px top and 6 px bottom padding under border-box sizing. Centering within the remaining 30 px retains the original 3 px per-side clearance, yielding 8/9 px control margins. Compute the drag band as the maximum of the legacy 44 px minimum and measured row height, keeping all current exclusions and covered-control hit tests.

macOS keeps the existing 80 px horizontal reservation and standard AppKit button geometry. The new web padding does not alter native view hierarchy, button visibility or standard fullscreen behavior. Browser simulation validates the web layout/inset transitions; actual native macOS behavior remains a platform verification limit on this Windows workstation.

## Risks / Trade-offs

The native traffic lights retain their system vertical position, rather than being manually moved to the web controls' new centerline. This preserves native behavior through resize and fullscreen transitions. The height token must continue to define the wallpaper boundary as well as the row height.

## Validation

Check actual top/controls/icon rectangles at normal and high DPI in simulated Windows/macOS windowed/fullscreen and browser modes; verify the final row pixel drags, controls remain clickable, and macOS inset enters/exits without custom window buttons. Run frontend tests/build and strict change validation.
