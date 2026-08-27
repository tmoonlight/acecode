## ADDED Requirements

### Requirement: Computer use capability gate

ACECode SHALL register the computer use tool group only when
`config.computer_use.enabled` is true and a screen backend exists for the
running platform. The setting SHALL default to false.

#### Scenario: Capability disabled by default

- **WHEN** a configuration without a `computer_use` section is loaded
- **THEN** no computer use tool is registered
- **AND** the model's tool list contains no screen capture or synthetic input
  capability

#### Scenario: Capability enabled on a supported platform

- **WHEN** `config.computer_use.enabled` is true and the platform has a backend
- **THEN** `computer_screenshot`, `computer_click`, `computer_move`,
  `computer_drag`, `computer_scroll`, `computer_key`, and `computer_type` are
  registered

#### Scenario: Unsupported platform

- **WHEN** `config.computer_use.enabled` is true and the platform has no backend
- **THEN** no computer use tool is registered
- **AND** startup succeeds without an error

### Requirement: Screen capture as a model-visible attachment

ACECode SHALL provide `computer_screenshot`, which captures the current screen
and returns it as a PNG attachment through the tool result attachment path so
vision-capable models receive it as image content. The tool SHALL be read-only.

#### Scenario: Capture reaches the model as an image

- **WHEN** the model calls `computer_screenshot` in a session using a
  vision-capable model
- **THEN** the result carries a PNG attachment
- **AND** the attachment is materialized into the session and sent to the
  provider as image content

#### Scenario: Capture reports its coordinate geometry

- **WHEN** a capture succeeds
- **THEN** the result reports the captured image's pixel width and height and
  the screen geometry the capture was taken against

#### Scenario: Capture requires no control grant

- **WHEN** no control grant is active in the session
- **THEN** `computer_screenshot` still executes because it is read-only

### Requirement: Captured-image pixel coordinate contract

ACECode SHALL interpret the `x` and `y` arguments of every computer use action
tool as pixel coordinates in the most recent capture of the current session,
with the origin at the captured image's top-left corner, and SHALL translate
them to logical desktop coordinates before dispatching input.

#### Scenario: Coordinates translate across display scaling

- **WHEN** a capture is downsampled from physical to logical pixels on a scaled
  display and the model targets a point in that image
- **THEN** the dispatched pointer event lands at the corresponding logical
  desktop position rather than at the raw image coordinate

#### Scenario: Multi-monitor origin is applied

- **WHEN** the captured virtual desktop begins at a non-zero origin
- **THEN** the capture origin is applied during translation so a point in the
  image maps to the same point on the physical desktop

#### Scenario: Action before any capture

- **WHEN** an action tool with coordinates is called before any capture in the
  session
- **THEN** the call fails with a result directing the model to capture first
- **AND** no input event is dispatched

#### Scenario: Screen geometry changed since capture

- **WHEN** the screen configuration changed after the most recent capture
- **THEN** a coordinate-bearing action fails with an explicit staleness error
  directing the model to capture again
- **AND** no input event is dispatched

#### Scenario: Coordinates outside the captured image

- **WHEN** an action targets a coordinate outside the captured image bounds
- **THEN** the call fails
- **AND** no input event is dispatched

### Requirement: Session-scoped control grant

ACECode SHALL require a session-scoped control grant before executing any
computer use action tool. The first such call in a session SHALL raise one
permission decision covering the capability. The grant SHALL NOT be persisted
and SHALL be dropped on session end, on user abort, and on explicit revocation.

#### Scenario: First action requests the grant

- **WHEN** the model calls an action tool in a session with no active grant
- **THEN** ACECode raises a permission decision describing desktop control
- **AND** the action executes only after approval

#### Scenario: Subsequent actions reuse the grant

- **WHEN** a grant is active in the session
- **THEN** further action tools execute without an additional permission prompt

#### Scenario: Denied grant blocks the action

- **WHEN** the user denies the control grant
- **THEN** the action fails with a result explaining that desktop control was
  denied
- **AND** no input event is dispatched
- **AND** no grant is recorded

#### Scenario: Abort drops the grant

- **WHEN** the user aborts the running turn while a grant is active
- **THEN** the grant is dropped
- **AND** the next action tool call raises a new permission decision

#### Scenario: Grant does not survive the session

- **WHEN** a session with an active grant ends and a new session starts
- **THEN** the new session has no grant
- **AND** its first action tool call raises a permission decision

### Requirement: Synthetic pointer and keyboard actions

ACECode SHALL provide pointer and keyboard action tools that dispatch synthetic
input through the platform backend: `computer_click` with button and repeat
count, `computer_move`, `computer_drag` between two points, `computer_scroll`
with a direction and amount, `computer_key` for named keys and chords including
explicit hold and release, and `computer_type` for literal text.

#### Scenario: Click dispatches at a translated position

- **WHEN** `computer_click` is called with in-bounds coordinates and an active
  grant
- **THEN** a pointer button event is dispatched at the translated logical
  position
- **AND** the result reports the action and the position used

#### Scenario: Key chord dispatches modifiers around the main key

- **WHEN** `computer_key` is called with a modifier chord
- **THEN** modifiers are pressed before and released after the main key

#### Scenario: Held keys are released

- **WHEN** `computer_key` holds a key without a matching release and the turn
  ends or is aborted
- **THEN** the backend releases keys it is holding so no modifier is left stuck

#### Scenario: Abort stops a running action

- **WHEN** the session abort flag is set while an action tool is executing
- **THEN** the tool returns promptly without dispatching further input

#### Scenario: Unknown key name

- **WHEN** `computer_key` is called with a key name the backend does not know
- **THEN** the call fails with a result naming the unknown key
- **AND** no input event is dispatched

### Requirement: Capture budget without history rewriting

ACECode SHALL bound captured image dimensions to the configured maximum edge
length and SHALL bound the number of captures per turn. ACECode SHALL NOT remove
or downgrade image content already present in earlier conversation messages.

#### Scenario: Oversized capture is downscaled

- **WHEN** the screen exceeds the configured maximum edge length
- **THEN** the capture is downscaled before encoding
- **AND** the reported image dimensions match the delivered image so coordinate
  translation stays correct

#### Scenario: Per-turn capture limit reached

- **WHEN** the model requests more captures in one turn than the configured
  limit
- **THEN** the call returns an explanatory result instead of a new image
- **AND** the turn continues

#### Scenario: Earlier captures are left intact

- **WHEN** several captures accumulate across a session
- **THEN** earlier image content in the conversation is left unmodified so the
  request prefix stays byte-stable across iterations within a turn

### Requirement: Computer use status command

ACECode SHALL provide a `/computer` command in the TUI, the daemon builtin
command set, and the web slash-command list that reports backend availability,
screen geometry, grant state, and configured bounds, and that revokes an active
grant on request.

#### Scenario: Report status

- **WHEN** the user runs `/computer`
- **THEN** ACECode reports whether the capability is enabled, whether a backend
  is available, the current screen geometry, whether a grant is active, and the
  configured capture bounds

#### Scenario: Revoke an active grant

- **WHEN** a grant is active and the user runs `/computer off`
- **THEN** the grant is dropped
- **AND** the next action tool call raises a new permission decision

#### Scenario: Status without a backend

- **WHEN** the platform has no backend and the user runs `/computer`
- **THEN** the command reports that the capability is unavailable on this
  platform
- **AND** exits successfully
