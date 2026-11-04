## Purpose

Allow users to execute an existing scheduled task immediately through its normal daemon execution path while preserving its configured future schedule and execution options.

## ADDED Requirements

### Requirement: Run a scheduled task once on demand
The system SHALL provide an accessible play button on each scheduled task row and an authenticated API to execute the task once without changing its schedule or enabled state. Disabled and expired tasks SHALL support explicit manual execution.

#### Scenario: Run a disabled task
- **WHEN** the user clicks the play button on a disabled task
- **THEN** one execution uses the saved prompt, model, permissions and workspace options, appears in run history, and the task remains disabled

### Requirement: Prevent overlapping manual executions
The system SHALL reject manual execution if that task or its workspace already has an active scheduled-task execution. Pending requests and active runs SHALL disable the play button; errors SHALL allow a later retry.

#### Scenario: Concurrent requests
- **WHEN** two clients request the same task while its first execution is active
- **THEN** only one run is accepted and the other receives a conflict

#### Scenario: Daemon shutdown
- **WHEN** the daemon stops with accepted executions awaiting launch
- **THEN** those runs become terminal and cannot launch after shutdown
