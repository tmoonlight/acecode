## ADDED Requirements

### Requirement: Active TUI session can be archived from a slash command
The TUI SHALL provide `/archive` as the canonical command and `/archieve` as a
compatibility alias that archive the current persisted session using the same
reversible metadata state used by Web.

#### Scenario: Archive an active TUI session
- **WHEN** the user runs `/archive` while a persisted TUI session is active
- **THEN** that session's metadata MUST record `archived=true`
- **THEN** its JSONL transcript and other session data MUST remain on disk
- **THEN** it MUST be visible through the existing archived-session listing

#### Scenario: Use the compatibility spelling
- **WHEN** the user runs `/archieve` while a persisted TUI session is active
- **THEN** the system MUST perform the same operation as `/archive`

#### Scenario: Discover the archive commands
- **WHEN** the TUI builds slash-command discovery and help content
- **THEN** `/archive` and `/archieve` MUST both be registered
- **THEN** `/help` MUST identify `/archive` as the canonical command

### Requirement: Archive persistence gates the TUI reset
The TUI SHALL persist the archive state before clearing or ending the current
session.

#### Scenario: Archive persistence succeeds
- **WHEN** the active session metadata is successfully marked archived
- **THEN** the TUI MUST continue with the existing `/clear` reset behavior

#### Scenario: Archive persistence fails
- **WHEN** the active session metadata cannot be marked archived
- **THEN** the TUI MUST display an archive failure message
- **THEN** the current session and visible conversation MUST remain active

#### Scenario: No persisted session is active
- **WHEN** the user runs either archive command before a session has been
  created
- **THEN** the TUI MUST perform the existing `/clear` reset behavior
- **THEN** the command MUST NOT create an empty archived session
