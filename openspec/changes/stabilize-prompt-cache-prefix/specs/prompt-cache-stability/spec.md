## Purpose

Keep reusable model request prefixes stable across turns, session reloads and compaction windows while retaining fresh context and compatible provider routing.

## ADDED Requirements

### Requirement: Context snapshots remain stable within a window
The system SHALL persist a hidden initial context snapshot for each compaction window and preserve its model-facing position and bytes across unchanged turns and session reloads. Changed context SHALL be appended without rewriting previously sent context. Internal records MUST NOT appear as user turns or visible transcript messages.

#### Scenario: Another turn and reload reuse the prefix
- **WHEN** a session receives a second user input or reloads with unchanged context
- **THEN** its existing context and conversation prefix remain identical and new input is appended

#### Scenario: Instructions or runtime context changes
- **WHEN** project instructions, expert context, hooks or plan state changes
- **THEN** the model receives hidden appended context describing the new state and existing messages remain unchanged

#### Scenario: Memory is explicitly disabled
- **WHEN** an existing session disables memory
- **THEN** outgoing requests exclude the earlier automatically injected memory by starting a new context epoch, even before the next normal turn

#### Scenario: Explicit skill catalog change
- **WHEN** the available skill catalog changes through configuration or expert selection
- **THEN** the model receives an appended replacement index while the existing system index stays unchanged

#### Scenario: Context storage fails
- **WHEN** the new hidden context cannot be persisted
- **THEN** no normal provider request uses that uncommitted context and the user receives an error

### Requirement: Todo state does not rewrite the request prefix
The system SHALL rely on TodoWrite tool results for normal checklist updates and restore the current checklist once when a fresh compaction window is installed.

#### Scenario: Todo changes inside a turn
- **WHEN** TodoWrite returns an updated list
- **THEN** subsequent requests retain their earlier prefix and do not reinject a changed checklist before it

#### Scenario: Compaction replaces the history
- **WHEN** compaction succeeds
- **THEN** the next window contains the complete current checklist and does not repeat it on every request

### Requirement: Unspecified subagent model inherits the parent
The system SHALL use the parent's current saved model when creating a subagent without an explicit model. Explicit model selections SHALL take precedence and missing parent model state SHALL retain the configured fallback.

#### Scenario: Parent changed models before spawning
- **WHEN** the parent switches to a saved model and spawns a child without a model
- **THEN** the child uses that current model rather than the global default

#### Scenario: Explicit child selection
- **WHEN** spawning specifies a model
- **THEN** the specified model is used regardless of the parent's model

### Requirement: Cache routing remains session stable and endpoint compatible
The system SHALL send a stable session cache key to verified supporting endpoints and omit it for unknown endpoints. Explicit unsupported-cache-field rejection SHALL cause at most one retry without that field and prevent its repeated use for the same endpoint/model in that provider instance.

#### Scenario: Provider is recreated for a session
- **WHEN** a supported provider is rebuilt or a saved session resumes
- **THEN** subsequent requests retain the same session cache key

#### Scenario: Strict gateway or unrelated error
- **WHEN** the endpoint is unknown or returns an unrelated error
- **THEN** unknown endpoints receive no cache field and unrelated errors preserve normal error handling

#### Scenario: Cache field rejected
- **WHEN** a supported endpoint explicitly rejects the cache key field
- **THEN** the request is retried once without that field and future requests in that provider instance omit it
