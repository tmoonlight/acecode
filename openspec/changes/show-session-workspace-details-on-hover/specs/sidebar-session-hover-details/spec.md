## ADDED Requirements

### Requirement: Workspace-backed sessions expose hover details
Each compact sidebar session row associated with a workspace SHALL expose a non-interactive detail card on pointer hover and keyboard focus. The card SHALL show the session's effective working directory without changing the session row's normal layout.

#### Scenario: Hover a grouped workspace session
- **WHEN** the pointer enters a workspace-backed session row inside a workspace group
- **THEN** a detail card appears beside the row
- **AND** the card shows that session's effective working directory

#### Scenario: Hover a pinned workspace session
- **WHEN** the pointer enters a pinned session row
- **THEN** the same detail card behavior and effective working directory are available

#### Scenario: Focus a workspace session control
- **WHEN** keyboard focus enters a control within a workspace-backed session row
- **THEN** the detail card appears with the same information as pointer hover

### Requirement: No-workspace sessions expose no workspace details
A session marked as no-workspace SHALL NOT render a workspace detail card and SHALL NOT initiate a Git-information request from hover or focus, even if malformed input also contains a directory value.

#### Scenario: Hover a no-workspace session
- **WHEN** the pointer enters a no-workspace session row
- **THEN** no workspace detail card appears
- **AND** no Git-information request is made for that session

#### Scenario: Focus a no-workspace session
- **WHEN** keyboard focus enters a control within a no-workspace session row
- **THEN** no workspace detail card appears
- **AND** no Git-information request is made for that session

### Requirement: Git branch is conditional and read-only
The detail card SHALL show a Git branch row only when the existing Git-information endpoint confirms that the session directory is a Git repository. The branch row SHALL display the current branch returned by that endpoint and SHALL provide no Git mutation controls.

#### Scenario: Session directory is a Git repository
- **WHEN** hover details are open and Git information reports `is_repo: true`
- **THEN** the card shows the returned current branch in addition to the working directory

#### Scenario: Session directory is not a Git repository
- **WHEN** hover details are open and Git information reports `is_repo: false`
- **THEN** the card continues to show the working directory
- **AND** no Git branch row is rendered

#### Scenario: Git information cannot be loaded
- **WHEN** the Git-information request times out or fails
- **THEN** the card continues to show the working directory
- **AND** no error control or Git branch row is rendered

### Requirement: Detail card remains outside sidebar layout
The session detail card SHALL render outside the sidebar's clipped layout, remain within the visible viewport, and SHALL NOT intercept pointer input intended for session-row controls.

#### Scenario: Hover a row near a viewport edge
- **WHEN** a session row is close to the right or bottom viewport edge
- **THEN** the detail card is repositioned within the configured viewport margins
- **AND** the underlying session row dimensions remain unchanged

#### Scenario: Pointer leaves the row
- **WHEN** the pointer leaves a hovered row and the row does not retain keyboard focus
- **THEN** the detail card is removed
