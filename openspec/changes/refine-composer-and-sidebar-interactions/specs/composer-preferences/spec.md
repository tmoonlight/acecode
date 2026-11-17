## Purpose

Let users control predicted messages and the default action for messages entered during a reply, retaining those choices across application restarts.

## ADDED Requirements

### Requirement: Composer preferences persist immediately
General settings SHALL provide a predicted-message toggle and a busy-message choice of steer or queue. Changes SHALL use the existing serialized autosave path without a separate save button. Missing or invalid stored values SHALL default to predictions enabled and steer.

#### Scenario: Restore nondefault preferences
- **WHEN** the user disables predictions and chooses queue, then reloads the application
- **THEN** both choices are restored from the daemon configuration
- **AND** changing another UI preference preserves them

#### Scenario: Reject invalid API input
- **WHEN** a preferences request contains a nonboolean prediction value or an unsupported busy-message value
- **THEN** the server rejects the whole request without changing existing preferences

### Requirement: Disabled predictions do not generate
Disabling predicted messages SHALL suppress recommendations in the composer. The server SHALL return an empty prompt without invoking a model for an otherwise eligible recommendation request while the preference is disabled.

#### Scenario: An older tab still requests a recommendation
- **WHEN** predictions are disabled and an older tab requests a recommendation for a completed turn
- **THEN** the response contains an empty prompt and no model request occurs

### Requirement: Busy submission follows the selected action
While a turn is running, Enter and the send button SHALL use the selected busy-message action. Ctrl+Enter, or the existing platform-equivalent shortcut, SHALL use the other action. Shortcut hints SHALL match. A stopping turn SHALL continue to accept only queued input; idle and fork submissions SHALL retain their existing behavior.

#### Scenario: Queue is selected
- **WHEN** the user selects queue and presses Enter during a reply
- **THEN** the message waits for the running turn to finish
- **AND** the alternate shortcut inserts a message into the running turn

#### Scenario: Steer is selected
- **WHEN** steer is selected and the user submits during a reply
- **THEN** Enter inserts into the running turn and the alternate shortcut queues for later
