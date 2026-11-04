# Spec Delta

## Purpose

Control the names of built-in tools exposed to models while preserving user settings across application restarts and upgrades.

## ADDED Requirements

### Requirement: One-time write_file startup default
On the first successful startup migration for a data directory, ACECode SHALL enable previously disabled tool rewriting and set `file_write` to `write_file`, retaining all other mappings. The migration SHALL run before tool registration in daemon, TUI and headless modes and persist completion with the settings.

#### Scenario: Existing disabled settings
- **WHEN** a data directory contains disabled rewrite settings without migration completion
- **THEN** rewriting is enabled and `file_write` maps to `write_file`
- **AND** all other mappings are retained and completion is persisted

#### Scenario: Missing settings
- **WHEN** a data directory has no tool rewrite settings
- **THEN** the existing seed mappings are initialized with `file_write` changed to `write_file`
- **AND** enabled settings and completion are persisted before publication

#### Scenario: Already enabled settings
- **WHEN** rewriting is already enabled without migration completion
- **THEN** enabled state and every mapping remain unchanged
- **AND** completion is persisted so later disabling does not trigger migration

### Requirement: Subsequent user choices survive startup
After migration or an explicit settings save in the new version, ACECode SHALL preserve the user's enabled state and mappings on subsequent startups. Settings API saves SHALL record completion independently of client-supplied metadata.

#### Scenario: Disable after migration
- **WHEN** the user disables rewriting through the settings API after migration
- **THEN** later startups keep rewriting disabled

#### Scenario: Change mapping after migration
- **WHEN** the user changes or removes the `file_write` mapping after migration
- **THEN** later startups retain that choice

### Requirement: Failed migration preserves existing data
ACECode SHALL preserve unreadable, malformed or invalid rewrite files and SHALL NOT apply or record an unsuccessful migration. Migration persistence failures or mapping conflicts SHALL be logged without preventing startup, allowing retry on a later startup.

#### Scenario: Corrupt settings
- **WHEN** settings cannot be read or validated
- **THEN** the original file remains untouched and startup uses the existing disabled fallback

#### Scenario: Persistence failure
- **WHEN** writing migrated settings fails
- **THEN** the migration is not published or marked complete
- **AND** startup continues with the previously loaded settings

#### Scenario: Conflicting mapping
- **WHEN** another retained mapping already uses `write_file`
- **THEN** migration does not overwrite that mapping or write an invalid configuration

#### Scenario: Legacy settings filename
- **WHEN** settings use the supported legacy filename
- **THEN** migration updates that same file and respects its completion marker on later startups
