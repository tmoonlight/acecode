## ADDED Requirements

### Requirement: New session IDs use underscore separators

SessionStorage SHALL generate session IDs in the form `YYYYMMDD_HHMMSS_xxxx`, retaining the existing UTC timestamp and four random hexadecimal digits.

#### Scenario: Create a session
- **WHEN** a new session ID is automatically generated
- **THEN** its ID and canonical JSONL / metadata file names use underscores instead of hyphens

### Requirement: Mixed session naming remains compatible

Session discovery, recovery and diagnostics MUST support legacy hyphen IDs and new underscore IDs without rewriting either identity. Legacy PID-suffixed experimental files MUST remain excluded from canonical session listings.

#### Scenario: Resume legacy history
- **WHEN** a legacy hyphen-named session is resumed
- **THEN** the original session ID and file paths are used and its messages remain readable

#### Scenario: Discover both generations
- **WHEN** a workspace contains legacy and underscore sessions
- **THEN** both canonical sessions appear in metadata listings and diagnostics
