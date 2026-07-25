## ADDED Requirements

### Requirement: Typed narrow mutations
Shared settings writes SHALL accept a typed field/domain operation rather than an arbitrary replacement configuration document.

#### Scenario: Change one scalar field
- **WHEN** a caller changes theme, notifications, default permission, upgrade URL, or custom instructions
- **THEN** only the targeted field is changed and unrelated latest on-disk values are preserved

#### Scenario: Change a domain collection
- **WHEN** a caller changes models, MCP servers, connectors, skills, or configurable tools/hooks
- **THEN** the domain's canonical validation and reconciliation helper applies before persistence

### Requirement: Locked reload-before-patch transaction
Each shared mutation SHALL hold an in-process and interprocess config lock while reloading, patching, validating, and atomically replacing the canonical config file.

#### Scenario: Non-overlapping concurrent mutations
- **WHEN** two processes mutate different settings in overlapping time
- **THEN** both confirmed changes exist in the final config without losing either process's update

#### Scenario: Process crashes during write
- **WHEN** a process terminates after writing the temporary file but before replacement completes
- **THEN** the previous canonical config remains readable and a later transaction can proceed

### Requirement: Validation rollback
Invalid or failed mutations SHALL leave both disk and caller-confirmed state unchanged.

#### Scenario: Validation fails
- **WHEN** a typed mutation supplies an invalid value
- **THEN** the service returns a structured validation error and performs no canonical-file replacement

#### Scenario: Atomic replacement fails
- **WHEN** the temporary write or replacement fails
- **THEN** the service reports persistence failure and callers continue displaying the last confirmed value

### Requirement: Secret-safe results
Mutation results, logs, and UI errors SHALL not expose API keys, authorization tokens, connector secrets, custom headers, or unredacted credential-bearing URLs.

#### Scenario: Secret-bearing mutation fails
- **WHEN** a model, MCP, or connector mutation fails validation or persistence
- **THEN** the returned diagnostic identifies the field/domain without including the secret value

### Requirement: Explicit runtime application
Each successful mutation SHALL report whether the current process applied it live, requires restart, or failed runtime application after persistence.

#### Scenario: Live apply succeeds
- **WHEN** a persisted theme, registry, manager, or default-template change can apply live
- **THEN** the caller receives persisted-and-live status after the runtime hook succeeds

#### Scenario: Restart is required
- **WHEN** persistence succeeds but the active process cannot safely reload the setting
- **THEN** the caller receives persisted-restart-required status and the UI does not claim immediate effect

### Requirement: Shared daemon and TUI semantics
TUI operations and daemon/Desktop routes for the same exposed setting SHALL call the same typed mutation and validation code.

#### Scenario: Equivalent cross-surface write
- **WHEN** TUI and Desktop set the same valid setting value
- **THEN** normalization, validation, disk representation, and new-session semantics are equivalent

#### Scenario: Long-running process observes external defaults
- **WHEN** another process changes the default model or default permission
- **THEN** a long-running daemon refreshes those defaults before creating the next session
