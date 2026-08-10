## ADDED Requirements

### Requirement: Permission request resolution is observable

When AceCode dispatches `PermissionRequest`, it SHALL dispatch exactly one
`PermissionResolved` event after the permission decision is finalized.

#### Scenario: Interactive denial

- **WHEN** a tool requires confirmation and the user denies it
- **THEN** `PermissionResolved` runs with decision `deny` and source `interactive`
- **AND** it runs before the denial result returns to the model

#### Scenario: Interactive approval

- **WHEN** a tool requires confirmation and the user approves it
- **THEN** `PermissionResolved` runs with decision `allow` or `always_allow`
- **AND** it runs before tool execution starts

#### Scenario: Hook decision

- **WHEN** `PermissionRequest` itself allows or denies the tool
- **THEN** `PermissionResolved` runs with source `hook`

#### Scenario: No request was emitted

- **WHEN** policy auto-allows a tool without dispatching `PermissionRequest`
- **THEN** AceCode does not dispatch `PermissionResolved`

### Requirement: PermissionResolved is generic and observational

The event SHALL expose common hook fields, tool identity/input,
`permission_decision`, and `permission_source`. Hook output SHALL NOT change the
already finalized permission decision.

### Requirement: Herdr support is optional hook configuration

The repository SHALL provide a single cross-platform hook JSON example that
reports AceCode lifecycle state through Herdr's custom-agent CLI.

#### Scenario: Running outside Herdr

- **WHEN** any example hook runs without complete Herdr pane environment
- **THEN** it exits successfully without invoking Herdr

#### Scenario: Running inside Herdr

- **WHEN** the example is installed and trusted in a Herdr pane
- **THEN** it reports `idle`, `working`, and `blocked` from generic lifecycle events
- **AND** AceCode core contains no Herdr-specific runtime detection or reporter
