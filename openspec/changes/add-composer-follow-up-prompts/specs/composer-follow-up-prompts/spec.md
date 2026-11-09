## Purpose

Help users continue a completed conversation with one contextual, editable next-message suggestion while preserving their control over drafting and sending.

## ADDED Requirements

### Requirement: Completed-turn triggers
Web and Desktop SHALL request one recommendation immediately after the visible conversation finishes normally or after first loading an already completed conversation. Loading, running, aborted, failed, empty and read-only conversations SHALL NOT trigger generation.

#### Scenario: First visit and live completion
- **WHEN** the user opens a completed conversation or its visible running turn finishes
- **THEN** generation starts once its completed state is available, without waiting for composer focus
- **AND** an opened running conversation waits for completion

### Requirement: Completion-scoped deduplication
Both triggers SHALL share a completion identity and retain requests, results and accepted or dismissed state for the current application lifetime. Repeated navigation, history refresh and duplicate terminal events SHALL NOT regenerate the same completion. A new completed turn SHALL be eligible again.

#### Scenario: Switch away and return
- **WHEN** a recommendation is pending or available and the user leaves and returns to the same completion
- **THEN** the same request or result is reused
- **AND** accepting or dismissing it prevents it from reappearing for that completion

### Requirement: Draft-safe ghost text
A recommendation SHALL appear as muted text in the empty main composer, without becoming draft content or a transcript message. Existing text, attachments, paste blocks, speech input and command menus SHALL take precedence. Late results SHALL only be shown for their original completion and an unchanged eligible draft.

#### Scenario: Edits or navigation during generation
- **WHEN** a user types, attaches content, starts another turn or changes conversation before generation returns
- **THEN** the result does not overwrite the draft or appear in another conversation

#### Scenario: Clear a draft after a recommendation is visible
- **WHEN** the user types over a visible recommendation and clears the entire draft in the same completed turn
- **THEN** the empty composer shows the same recommendation again without another request
- **AND** a recommendation previously accepted with Tab or dismissed with Escape stays consumed

### Requirement: Empty-draft generation gate
Before requesting a recommendation, the client SHALL confirm the current session draft has loaded successfully and contains no unsent text, attachments, pasted blocks or context. Whitespace counts as draft text; historical messages do not. A completion encountered with draft content SHALL be skipped without a model request and SHALL NOT be retriggered merely by clearing that draft.

#### Scenario: Restored or current unsent text
- **WHEN** a completed session is opened with a saved draft, or the visible turn finishes while the user has unsent text
- **THEN** no recommendation request is sent for that completion
- **AND** clearing the draft or reopening that same completion does not send a deferred request

#### Scenario: Draft restoration is pending or fails
- **WHEN** conversation history has loaded but the session draft has not loaded successfully
- **THEN** recommendation generation waits without a request
- **AND** a successfully loaded empty draft permits the original completion trigger

### Requirement: Explicit keyboard acceptance
Unmodified Tab in the focused, empty composer SHALL accept a visible recommendation as editable text without submitting it. Escape SHALL dismiss it. IME, command or mention menus, modified Tab and ordinary focus navigation SHALL retain their existing priority.

#### Scenario: Accept then edit
- **WHEN** the user presses Tab while a recommendation is visible and no higher-priority interaction is active
- **THEN** it becomes ordinary draft text with the caret at the end
- **AND** no message is sent until the user submits it

### Requirement: Bounded contextual generation
Generation SHALL use a separate tool-free model request and bounded recent visible conversation data. It SHALL return one concise user-voice request or no suggestion, honor the UI language, leave the main conversation unchanged and silently omit failed or stale recommendations.

#### Scenario: Stale or failed model result
- **WHEN** generation fails, exceeds its time budget, returns invalid content or targets a superseded completion
- **THEN** the composer remains usable with its normal placeholder and no automatic retry loop
