## MODIFIED Requirements

### Requirement: LLM-based summarization for compression
The system SHALL use the LLM provider to generate a summary that preserves technical details, including file paths, variable names, decisions and relevant code. Where supported, the summarization request SHALL reuse the normal request's model-facing message prefix and tool definitions, appending the summary instruction only at the end. Tool choice SHALL be disabled where supported. Native or textual tool calls MUST NOT be accepted as a summary; a rejected prefix-reusing request SHALL fall back to the existing tool-free summarization path.

#### Scenario: Summary generation
- **WHEN** local compaction runs through a provider supporting prefix reuse
- **THEN** its first request preserves the normal request prefix and tool schemas, adds the summary instruction, and installs the validated result using the current compact-summary format

#### Scenario: Model emits a tool call
- **WHEN** the prefix-reusing summary response contains native or textual tool calls
- **THEN** no tools are executed, that response is not installed, and summarization retries without tools

#### Scenario: Summary API call fails
- **WHEN** summarization fails after the existing retry and fallback policy
- **THEN** no invalid summary is persisted and existing compaction failure handling remains in force

## ADDED Requirements

### Requirement: Successful compaction refreshes the context window
The system SHALL install a fresh hidden context snapshot with a successful replacement history and persist it in the checkpoint. The compact summary SHALL remain the final model-history item at a mid-turn compaction boundary.

#### Scenario: Reload after compaction
- **WHEN** the session resumes from a new compact checkpoint
- **THEN** it restores the same initial context snapshot and checklist without moving or duplicating them

#### Scenario: Checkpoint storage fails
- **WHEN** a generated summary and its fresh snapshot cannot be persisted
- **THEN** the prior history and window identity remain in effect and no compaction success is reported
