## Context

ACECode Desktop uses one WebView for the React shell and a second native WebView for Agent Browser content. The Browser must remain a native view so arbitrary sites are not constrained by iframe policy. Because the Browser view is placed above the shell view, React z-index alone cannot put a menu or popover over Browser pixels.

The current implementation reports rectangles from elements marked with `data-ace-native-overlay`, then cuts matching holes from the native Browser surface. This is the correct lightweight architecture, but its discovery logic treats DOM hit-testing as authoritative, its asynchronous native acknowledgement is discarded, and smoke tests assert only that a mask object exists rather than the complete multi-overlay contract.

## Goals / Non-Goals

**Goals:**

- Make every registered ACECode floating surface render and receive pointer input above the Agent Browser.
- Support multiple simultaneous overlap surfaces and blocking/modal surfaces.
- Preserve Browser pixels everywhere not intersected by an overlap surface.
- Guarantee a usable floating surface if native local occlusion cannot be applied.
- Keep the behavior aligned on Windows and macOS.

**Non-Goals:**

- Replacing Browser with an iframe.
- Reimplementing React menus as native AppKit controls.
- Treating arbitrary positioned application content as a floating surface without an explicit contract.
- Changing the context menu supplied by websites inside the Agent Browser.

## Decisions

### 1. Keep explicit overlay registration as the stacking contract

`data-ace-native-overlay="overlap"` identifies a transient surface whose intersecting rectangle must be cut out of the Browser. `blocking` identifies a modal surface that hides the Browser completely. Standard floating accessibility roles (`menu`, `listbox`, and `tooltip`) implicitly opt into overlap behavior, while explicit attributes remain required for other popovers and blocking surfaces. This bounded selector avoids scanning every DOM element on every animation frame and keeps accidental layout elements from punching holes in the native surface.

Registered visible overlays are authoritative. DOM `elementFromPoint` remains useful for rejecting a registered overlay covered by another registered surface, but the Browser placeholder and its descendants cannot invalidate the registration because the actual native view has no DOM stacking representation.

### 2. Centralize discovery and safely merge compatible rectangles

The coordinator returns normalized, clipped, de-duplicated rectangles and coalesces only contained or edge-aligned rectangles whose exact union is still rectangular before crossing the native bridge. This provides one rule for context menus, dropdowns, tooltips, toasts, and other popovers, while keeping the bounded native payload small without exposing Browser pixels that no floating surface actually covers.

### 3. Make local occlusion delivery acknowledged and fail closed

Layout delivery remains asynchronous, but the panel tracks the latest revision. A failed acknowledgement for the current revision marks local occlusion unhealthy and immediately sends a visible=false layout while an overlay intersects the Browser. A later successful unoccluded layout restores the normal path. Stale acknowledgements cannot hide or restore a newer layout.

This fallback may temporarily hide the whole Browser only when the native bridge cannot honor the precise mask; it never leaves a menu visually or interactively underneath the Browser.

### 4. Keep native clipping and hit testing in one surface abstraction

The macOS wrapper owns both the Core Animation mask and hit-test exclusions. It rebuilds the mask when bounds or occlusion rectangles change, clips all rectangles to current bounds, and supports multiple holes. Browser content outside holes remains interactive; points inside holes fall through to the React shell.

## Risks / Trade-offs

- [An overlay is not registered] -> Add the shared attribute at the floating surface root and cover it with an architecture test.
- [Native acknowledgement arrives out of order] -> Compare layout revisions before changing health or fallback state.
- [Too many small surfaces are visible] -> Normalize and merge intersecting rectangles before sending the bounded payload.
- [Local mask fails on a future WebKit release] -> Use whole-surface hiding while the registered overlay is visible.

## Verification

- Unit-test discovery, clipping, merging, blocking behavior, and acknowledgement fallback.
- Extend the macOS smoke helper to apply multiple holes and verify mask plus hit testing.
- Build and run web tests, web production assets, the macOS smoke target, and the Desktop bundle.
