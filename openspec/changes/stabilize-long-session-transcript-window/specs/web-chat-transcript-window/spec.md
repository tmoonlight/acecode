## ADDED Requirements

### Requirement: Transcript window boundaries survive history reconstruction
The Web/Desktop chat view SHALL identify a window boundary by stable persisted message
identity when available, rather than only by a reducer-generated item id.

#### Scenario: Retry replacement rebuilds temporary item ids
- **WHEN** a large transcript is windowed at a persisted user message
- **AND** `transcript_replace` reconstructs the same persisted messages with new temporary item ids
- **THEN** the view MUST resolve the original boundary in the replacement projection
- **AND** the mounted transcript MUST remain a tail window rather than expanding to the full projection

#### Scenario: Stable and temporary ids cannot collide
- **WHEN** a persisted message id has the same text representation as a temporary item id
- **THEN** the two identities MUST produce different window keys
- **AND** boundary lookup MUST use the namespaced identity consistently

### Requirement: Missing boundaries recover synchronously to a bounded tail window
The Web/Desktop chat view MUST reconcile its stored boundary before slicing the current
projection and MUST NOT depend on a later effect to repair a missing boundary.

#### Scenario: Replacement removes the anchored message
- **WHEN** a large multi-turn replacement projection no longer contains the stored boundary
- **THEN** the view MUST select a new initial tail boundary during the same render
- **AND** the current render MUST continue hiding an earlier prefix instead of mounting the complete projection

#### Scenario: Transcript becomes empty before reloading
- **WHEN** the current transcript has no projected rows
- **THEN** the window state MUST become uninitialized
- **AND** the first later non-empty projection MUST select its normal initial tail boundary

#### Scenario: Short transcript requires no window
- **WHEN** the projected transcript does not exceed the initial tail-window threshold
- **THEN** all projected rows MUST remain visible
- **AND** no artificial boundary MUST be introduced

### Requirement: Explicit transcript expansion remains authoritative
The Web/Desktop chat view SHALL distinguish an explicit full-history view from an
uninitialized or invalid boundary.

#### Scenario: User displays all before replacement
- **WHEN** the user explicitly selects "显示全部"
- **AND** a later `transcript_replace` rebuilds the transcript
- **THEN** the view MUST remain fully expanded
- **AND** automatic boundary reconciliation MUST NOT collapse it back to a tail window

#### Scenario: Progressively revealed boundary survives replacement
- **WHEN** the user reveals earlier messages and the resulting stable boundary still exists
- **AND** a later `transcript_replace` reconstructs the same persisted messages
- **THEN** the view MUST preserve that progressively revealed boundary
- **AND** it MUST NOT jump back to the default latest tail boundary

### Requirement: Window repair preserves existing viewport intent
Transcript-window repair SHALL reuse the existing tail-follow and manual-review authority
and SHALL NOT introduce an independent automatic scroll path.

#### Scenario: Following user receives replacement and new tokens
- **WHEN** the user is following the transcript tail
- **AND** history replacement is followed by live assistant tokens
- **THEN** the existing tail-follow behavior MUST keep the newest visible content reachable
- **AND** window repair MUST NOT require rendering the hidden prefix

#### Scenario: Reviewing user receives replacement and new tokens
- **WHEN** the user is reviewing historical content away from the tail
- **AND** history replacement is followed by live assistant tokens
- **THEN** window repair MUST NOT force the viewport to the live tail
- **AND** boundary repair MUST NOT introduce a new unconditional `scrollTop` write
