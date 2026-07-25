## ADDED Requirements

### Requirement: Successful TUI archive starts a fresh lazy session
After the active TUI session is archived, the system SHALL delegate to the
existing `/clear` lifecycle so archive-and-clear cannot drift from ordinary
clear behavior.

#### Scenario: Reset after a successful archive
- **WHEN** `/archive` or `/archieve` successfully archives the active session
- **THEN** the conversation display and Agent Loop message history MUST be reset
- **THEN** token, goal, title, and other state reset by `/clear` MUST also be
  reset
- **THEN** the archived session MUST end as the current active session

#### Scenario: First message after archive
- **WHEN** the user sends the first normal message after a successful archive
- **THEN** the system MUST lazily create a new session with an id different from
  the archived session
- **THEN** the new session MUST NOT inherit the archived state

#### Scenario: Archive command does not delete the old session
- **WHEN** the archive-and-clear lifecycle completes
- **THEN** the old session's JSONL and metadata files MUST remain available for
  restore
