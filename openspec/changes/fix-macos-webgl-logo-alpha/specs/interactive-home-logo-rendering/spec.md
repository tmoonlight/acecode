## ADDED Requirements

### Requirement: Transparent interactive-logo compositing
The home screen SHALL composite the interactive WebGL logo over the page without exposing the rectangular canvas boundary on every supported desktop webview.

#### Scenario: Light theme on macOS
- **WHEN** the interactive logo renders in a macOS WKWebView using the light theme
- **THEN** pixels outside the logo, shadow, and halo remain transparent without a white canvas-sized plate

#### Scenario: Dark theme on macOS
- **WHEN** the interactive logo renders in a macOS WKWebView using the dark theme
- **THEN** transparent pixels do not create a colored canvas edge or rectangularly clip the decorative grid

#### Scenario: Windows rendering parity
- **WHEN** the same interactive logo renders in Windows WebView2
- **THEN** its transparent background, lighting, shadow, and halo remain visually equivalent to the intended macOS rendering

### Requirement: Consistent alpha representation
The WebGL context and fragment shader SHALL use the same alpha representation for every pixel submitted to the page compositor.

#### Scenario: Partially transparent effect pixel
- **WHEN** the shader emits a shadow or halo pixel with alpha below one
- **THEN** its color channels are represented consistently with the WebGL context's premultiplied-alpha contract

#### Scenario: Fully transparent pixel
- **WHEN** a fragment lies outside the rendered logo effect
- **THEN** the shader emits zero color and zero alpha

### Requirement: Existing interaction and fallback behavior
The home logo SHALL retain its theme-aware lighting, pointer and idle-light interaction, reduced-motion handling, performance fallback, and context-loss fallback behavior.

#### Scenario: Dynamic renderer is available
- **WHEN** WebGL2 initializes and maintains acceptable frame rate
- **THEN** the interactive logo continues to respond to theme and light-position changes

#### Scenario: Dynamic renderer is unavailable
- **WHEN** WebGL2 initialization fails, its context is lost, or measured frame rate falls below the configured threshold
- **THEN** the existing static logo fallback remains visible
