## Purpose

Keep background summaries available when a dedicated model is missing or broken, using models proven by successful conversations without asking the user to intervene.

## ADDED Requirements

### Requirement: Remember successful conversation models
The system SHALL persist the most recently successful conversation model as a backup across restarts. Selection alone, failed, empty, incomplete or cancelled requests MUST NOT replace this backup. The record MUST NOT contain credentials or conversation content.

#### Scenario: Success followed by failure
- **WHEN** model A successfully answers and a subsequent request to B fails
- **THEN** A remains the backup

#### Scenario: Concurrent model change
- **WHEN** the selected model changes while a request is running
- **THEN** success records the model used by that request

### Requirement: Recover background generation silently
Automatic titles, home intent generation and enabled memory summaries SHALL attempt available fallback models when their preferred model is missing or fails. The most recently successful conversation model SHALL precede unproven contextual/default/other saved models. Recovery MUST NOT display notifications or change the conversation model.

#### Scenario: Missing or broken summary model
- **WHEN** a summary model is absent, deleted or returns an error
- **THEN** generation attempts the backup and then other configured candidates until successful or exhausted

#### Scenario: Reuse recovered model
- **WHEN** a replacement successfully generates a summary
- **THEN** later attempts reuse it until the preferred configuration changes or the replacement fails

#### Scenario: Cancellation and exhaustion
- **WHEN** generation is cancelled or all candidates fail
- **THEN** attempts stop without looping or replacing existing valid output with failed output

### Requirement: Dedicated summary configuration is optional
Summary settings SHALL accept enabling automatic selection with an empty model reference or retaining a removed reference. The UI MUST NOT request model repair or show fallback status; ordinary settings write errors remain actionable.

#### Scenario: Enable without selecting a model
- **WHEN** the user enables summary generation with no dedicated model
- **THEN** the setting is saved and background selection proceeds silently
