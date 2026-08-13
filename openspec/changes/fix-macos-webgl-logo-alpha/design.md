## Context

The interactive home logo renders its tile, lighting, shadow, and halo into a transparent WebGL2 canvas. The current implementation requests a non-premultiplied drawing buffer, builds premultiplied intermediate colors, and divides those colors by alpha before output. Windows WebView2 composites that straight-alpha result as intended, while macOS WKWebView visibly exposes the 156 px canvas boundary in both themes. The light theme shows a white plate; the dark theme shows a colored edge and makes the grid appear rectangularly clipped.

The WebGL context and shader are local to `InteractiveHomeLogo.jsx`. Existing architecture tests assert the current context attributes and shader structure, so the behavior can be changed without adding a dependency or altering the native desktop shell.

## Goals / Non-Goals

**Goals:**

- Use one compositor-safe alpha representation on macOS WKWebView and Windows WebView2.
- Remove canvas-boundary artifacts while preserving the intended tile, shadow, halo, and interaction.
- Lock the context/shader alpha contract with focused regression tests.
- Verify the production Web bundle in the macOS desktop shell under both themes.

**Non-Goals:**

- Redesigning the logo artwork, lighting model, or decorative dark-theme grid.
- Adding browser-specific user-agent detection.
- Replacing WebGL with a static image, Canvas 2D, or a third-party renderer.

## Decisions

### Use a premultiplied WebGL drawing buffer

Request `premultipliedAlpha: true`, matching the browser page compositor's common native representation. This avoids a browser-specific straight-alpha conversion at the WebGL-to-page boundary.

The alternative of disabling the dynamic logo only on macOS would remove the artifact but also remove intended behavior and create a permanent platform divergence. Keeping straight alpha and tuning halo strengths would only hide some symptoms while retaining the fragile conversion path.

### Emit the existing premultiplied intermediate directly

The fragment shader already composes shadow, halo, and opaque tile colors into `premultiplied` plus `alpha`. It will remove the final division by alpha, cap each output color channel at alpha as required by the premultiplied compositor contract, and submit the resulting premultiplied color directly. This avoids amplifying RGB values in very-low-alpha halo pixels while keeping bright opaque highlights saturated.

The alternative of applying an alpha cutoff would introduce a visible edge and discard soft shadow detail. A cutoff is therefore unnecessary when the representation is consistent.

### Preserve all non-alpha behavior

No lighting coefficients, canvas dimensions, CSS grid, frame scheduling, pointer behavior, or fallback thresholds will change. Architecture tests will assert both the premultiplied context attribute and direct shader output so a future refactor cannot reintroduce a mismatched representation.

## Risks / Trade-offs

- [Risk] WebView2 may show a small color difference after removing its straight-alpha conversion path. → Rebuild the production Web bundle and retain the same mathematically premultiplied intermediate values; run the full Web test suite and keep Windows-facing regression assertions platform-neutral.
- [Risk] A future shader edit could produce RGB values greater than alpha in transparent regions. → Keep all translucent layers composed into the `premultiplied` accumulator, cap final RGB channels at alpha, and assert that output contract in tests.
- [Risk] A cached embedded Web bundle could make desktop validation exercise stale code. → Run `pnpm build` before CMake configure so the regenerated embedded assets contain the fix.

## Migration Plan

No data or configuration migration is required. Rebuild the Web assets and desktop bundle, then relaunch the application. Rollback consists of reverting the context attribute, shader output, and matching test assertions together.

## Open Questions

None.
