## ADDED Requirements

### Requirement: Live assistant Markdown matches complete-message semantics

The TUI SHALL render a currently streaming assistant message with the same Markdown and XML-filtering semantics as rendering the accumulated message as a complete string.

#### Scenario: Following line extends a paragraph
- **WHEN** an assistant stream first completes `hello\n` and later appends `world`
- **THEN** the live rendering SHALL treat the accumulated text as the same paragraph produced by full formatting
- **AND** it SHALL NOT permanently freeze the first line as a separate paragraph

#### Scenario: Following line reclassifies a table header
- **WHEN** a streamed pipe-delimited line is followed by a valid Markdown table separator and body row
- **THEN** the live rendering SHALL match full formatting of the accumulated table
- **AND** it SHALL NOT retain the header as a previously frozen paragraph

#### Scenario: XML wrapper is split across deltas
- **WHEN** a hidden XML wrapper and its closing tag arrive across multiple assistant deltas
- **THEN** content hidden by the complete-message formatter SHALL not remain visible after the closing tag arrives
- **AND** visible content following the wrapper SHALL match complete-message formatting

### Requirement: Unproven incremental rendering is fail-safe

The TUI MUST use the established full-message Markdown formatter for live production rendering unless an incremental implementation has differential tests proving semantic equivalence for supported block, inline, XML, width, and theme cases.

#### Scenario: Incremental equivalence is not established
- **WHEN** the incremental lexer or formatter cannot prove equivalence for a supported Markdown construct
- **THEN** the production TUI SHALL bypass that incremental path
- **AND** the assistant message SHALL remain renderable through the full formatter

#### Scenario: Formatting raises an exception
- **WHEN** Markdown formatting fails while building an assistant message element
- **THEN** the existing safe fallback SHALL remain available
- **AND** the optimization SHALL NOT make the TUI crash or lose the message text

### Requirement: Completed-message cache grows with the transcript

The per-message render cache SHALL accept newly appended transcript indices without discarding valid entries for unchanged earlier messages.

#### Scenario: New conversation starts from an empty cache
- **WHEN** a conversation starts with zero cached messages and then appends its first message
- **THEN** cache storage SHALL grow to cover index zero before a store is attempted
- **AND** a subsequent lookup with the same key SHALL hit

#### Scenario: Existing conversation appends a message
- **WHEN** an existing cache entry is valid and the transcript appends another message
- **THEN** growing the cache SHALL preserve the existing valid entry
- **AND** the new message index SHALL be storable independently

#### Scenario: Theme version invalidates cached colors
- **WHEN** a renderer observes a new theme palette version
- **THEN** it SHALL also observe the palette published for that version
- **AND** it SHALL NOT store old-theme colors under the new cache key

### Requirement: Performance benchmarks are excluded from default unit runs

Timing-only streaming layout benchmarks SHALL require explicit opt-in and SHALL NOT execute during the default unit-test suite.

#### Scenario: Default unit suite runs
- **WHEN** `acecode_unit_tests` runs without a benchmark-specific opt-in flag
- **THEN** the streaming timing harness SHALL be skipped by GoogleTest
- **AND** functional correctness tests SHALL continue to run normally

#### Scenario: Developer explicitly requests the benchmark
- **WHEN** a developer enables disabled tests and filters for the streaming benchmark
- **THEN** the timing harness SHALL remain runnable for manual performance investigation
