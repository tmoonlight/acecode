## Why

The interactive home logo renders a visible canvas-sized plate and colored edge artifacts in macOS WKWebView, even though the same WebGL effect composites correctly in Windows WebView2. The transparent WebGL output must remain visually consistent across supported desktop webviews so the logo does not expose its implementation boundary.

## What Changes

- Render the interactive logo with premultiplied alpha values that are safe for browser page compositors.
- Preserve the existing lighting, shadow, halo, theme, pointer, and static-fallback behavior.
- Add regression coverage for the WebGL context and fragment-shader alpha contract.
- Rebuild and visually verify the macOS desktop application in both light and dark themes.

## Capabilities

### New Capabilities

- `interactive-home-logo-rendering`: Defines cross-platform transparent compositing and fallback requirements for the interactive home logo.

### Modified Capabilities

None.

## Impact

- Affected code: `web/src/components/InteractiveHomeLogo.jsx` and its architecture tests.
- Affected surfaces: the home screen in macOS WKWebView and Windows WebView2.
- No API, configuration, dependency, or stored-data changes.
