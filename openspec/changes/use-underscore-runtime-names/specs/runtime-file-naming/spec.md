## ADDED Requirements

### Requirement: Generated runtime names use underscores

ACECode SHALL use `_` instead of `-` as a separator in names it generates for runtime files, logs, attachments, exports, backups and temporary artifacts. User-supplied paths and externally mandated file names MUST retain their meaning.

#### Scenario: Generate a dated runtime artifact
- **WHEN** ACECode generates a log, feedback package or tool output file
- **THEN** the newly generated file name contains underscore separators and no hyphen separators

### Requirement: Existing runtime data remains accessible

ACECode MUST continue discovering and reading existing files with hyphen names. Persisted session IDs and explicit artifact references MUST remain unchanged. The system MUST NOT silently create an empty replacement store when a legacy data store already exists.

#### Scenario: Upgrade an existing data directory
- **WHEN** the data directory contains a legacy persistent resource and no new-name replacement
- **THEN** ACECode reads and updates the existing resource without losing its content

#### Scenario: Feedback collects mixed log names
- **WHEN** old hyphen logs and new underscore logs coexist
- **THEN** log discovery includes the applicable logs and selects recent files using modification times
