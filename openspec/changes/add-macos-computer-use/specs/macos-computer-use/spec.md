## ADDED Requirements

### Requirement: Explicit capability and system authorization
The system SHALL support Computer Use on macOS 14+ and distinguish enabled intent, supported OS, helper availability, accessibility permission, screen recording permission and readiness. Reading settings MUST NOT request OS permission. Only an authenticated, explicit user request SHALL trigger authorization for a validated permission name.

#### Scenario: Missing permission
- **WHEN** Computer Use is enabled and an OS permission is absent
- **THEN** Settings identifies the missing permission, native operations return an actionable error, and neither settings reads nor model tools open a permission prompt

#### Scenario: Unsupported older macOS
- **WHEN** the host runs macOS earlier than 14
- **THEN** it reports this capability unsupported without loading an incompatible helper or breaking other application features

### Requirement: Owned cancellable helper
The system SHALL use a private helper channel with bounded framing, one session owner and interprocess desktop exclusion. Disable, release, cancellation, parent exit, helper failure and timeout SHALL revoke observations and release ownership. Applications launched by the helper SHALL survive control release.

#### Scenario: Target application stops responding
- **WHEN** the helper stalls during a request and the user cancels or disables Computer Use
- **THEN** the call terminates promptly and another session can acquire a fresh worker without reusing the old observation

### Requirement: Native observation and identity
The system SHALL discover installed and running apps, verify window identities, and return window state with bounded accessibility data, focus/selection context and screenshots. Every screenshot SHALL carry its own identity, provenance and coordinate transform. Related transient surfaces SHALL be tied to verified ownership rather than process equality alone. Password values SHALL be omitted.

#### Scenario: Window is replaced or a menu changes
- **WHEN** the target window, process identity, geometry, focus or related surface changes after observation
- **THEN** the next action rejects stale state and requires a new observation

### Requirement: Complete existing action family
The system SHALL implement app launch, window activation, coordinate/element clicks, Unicode typing, native key chords, two-axis scrolling, drag, writable accessibility values and advertised secondary accessibility actions. Each observed action SHALL consume its observation, validate its target and release injected keys/buttons. Unsupported controls SHALL return explicit errors.

#### Scenario: Mixed coordinate scales
- **WHEN** a screenshot is resized or its display uses a different pixel scale
- **THEN** actions and accessible bounds refer to the actual returned image and map to the intended screen-point target without a fixed Retina multiplier

#### Scenario: Keyboard and control input
- **WHEN** a fresh observation targets a writable test control
- **THEN** Unicode text, Cmd/Option/Control/Shift chords and advertised control actions change the expected control and subsequent observation confirms the result

### Requirement: Themed pointer and model feedback
The system SHALL preserve ACE/plain themed pointer appearance, render a nonactivating input-transparent overlay, and include at most one correctly placed pointer in each screenshot. Existing model attachment ordering and screenshot labels SHALL remain intact.

#### Scenario: Appearance changes during control
- **WHEN** the user changes pointer style or theme color
- **THEN** the next render uses the new appearance without enabling control or unnecessarily destroying the current observation

#### Scenario: Visible native action feedback
- **WHEN** a verified input action targets the fixture's window
- **THEN** the actual native overlay renders at the action target, keyboard actions show feedback at the focused control, and click feedback remains perceptible without delaying mouse-button release or intercepting input

#### Scenario: Independent overlay acceptance
- **WHEN** the native pointer fixture runs on an unlocked desktop
- **THEN** it verifies the actual overlay's WindowServer presence, independently captured pixels, position and removal on release, in addition to checking screenshot composition

### Requirement: Packaged and verified execution
The system SHALL include the helper in macOS desktop and standalone distributions, sign nested executables appropriately, and resolve the delivered helper without PATH lookup. Validation SHALL cover deterministic transport and coordinate behavior, actual native fixture interactions and package contents. Unexecuted permission-dependent tests SHALL remain explicitly unverified.

#### Scenario: Installed helper missing
- **WHEN** the delivered helper is missing or cannot start
- **THEN** status and execution identify the installation problem instead of reporting the feature ready
