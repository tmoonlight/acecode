## ADDED Requirements

### Requirement: Config mutation rejects non-file persistence targets
ACECode SHALL report a structured persistence failure when an explicit existing
config mutation target is not a regular file and SHALL NOT invoke fatal startup
recovery for that target.

#### Scenario: Explicit path names a directory
- **WHEN** a settings mutation uses an existing directory as its config path
- **THEN** the mutation returns a persistence error, leaves runtime revision state
  unchanged, and the process continues running

#### Scenario: Filesystem metadata cannot be read
- **WHEN** the mutation service cannot determine whether the target exists or is a
  regular file
- **THEN** the mutation returns a persistence error without attempting to parse or
  overwrite the target
