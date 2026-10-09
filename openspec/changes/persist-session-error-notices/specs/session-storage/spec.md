## ADDED Requirements

### Requirement: Durable transcript-only error notices

The system SHALL persist each emitted conversation error notice with its original text, diagnostic metadata, timestamp, and unique occurrence identity. History reads after session switching, refresh, or daemon restart SHALL restore the existing error presentation. These notices MUST NOT be included in model requests or compaction input, including after session resume.

#### Scenario: Quota failure while another session is selected
- **WHEN** a background session receives a terminal HTTP 451 quota failure
- **THEN** opening that session SHALL display the stored red error notice and its original diagnostic details

#### Scenario: Reload and resume after a failure
- **WHEN** a session with an error notice is reloaded or resumed after restart
- **THEN** the notice SHALL remain visible in its transcript position
- **AND** the next model request SHALL exclude the notice and its diagnostic content

#### Scenario: Repeated identical failures
- **WHEN** separate turns emit errors with identical content
- **THEN** history SHALL retain each occurrence with a distinct identity

#### Scenario: History overlaps event replay
- **WHEN** a persisted error appears in both loaded history and replayed or live events
- **THEN** the client SHALL display that occurrence exactly once
- **AND** a later error with identical text and a different identity SHALL remain visible

#### Scenario: Compatibility with legacy transient errors
- **WHEN** an older daemon emits identical error events without a persisted identity marker
- **THEN** the client SHALL retain distinct event occurrences and deduplicate replays of the same sequence

#### Scenario: Error notice cannot be written after a storage failure
- **WHEN** a storage operation fails and recording its error notice also fails
- **THEN** the system SHALL retain the original storage diagnostic and visible error notice
- **AND** it SHALL NOT claim the failed operation or error notice was durably saved
