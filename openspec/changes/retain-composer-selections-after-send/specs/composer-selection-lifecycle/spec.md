## ADDED Requirements

### Requirement: Composer selections survive message submission
The Desktop composer SHALL retain the selected swarm mode and expert after a message is accepted for the same conversation. Attachments, pinned contexts, and transient selection UI MAY still be cleared as one-shot submission data.

#### Scenario: Send with swarm mode selected
- **WHEN** a user enables swarm mode and sends a message
- **THEN** the accepted message includes swarm-mode metadata
- **AND** swarm mode remains selected for the next message in that conversation

#### Scenario: Send with an expert selected
- **WHEN** a user selects an expert and sends a message
- **THEN** the session remains bound to that expert
- **AND** the composer continues to display the selected expert for the next message

#### Scenario: Promote a home composer into a session
- **WHEN** a user selects swarm mode or an expert in the home composer and sends the first message
- **THEN** the promoted session retains those selections after creation and submission

#### Scenario: Queue a message while a turn is busy
- **WHEN** a user sends a message while the selected conversation is busy
- **THEN** the queued message captures the current swarm-mode selection
- **AND** accepting the queued message does not clear swarm mode or the expert selection from the composer

### Requirement: Composer selections change only through selection lifecycle actions
The Desktop composer MUST NOT treat successful message submission as a swarm-mode or expert cancellation action. It SHALL remove or replace a selection when the user explicitly disables, removes, or switches it, and MAY restore different selection state when the composer changes to another conversation context.

#### Scenario: Explicitly disable swarm mode
- **WHEN** a user clicks the active swarm-mode control or toggles it off from the capability menu
- **THEN** subsequent messages omit swarm-mode metadata until the user enables it again

#### Scenario: Explicitly remove or replace an expert
- **WHEN** a user removes the active expert or selects a different expert
- **THEN** the session binding and composer status update to match that explicit action

#### Scenario: Navigate to another conversation
- **WHEN** the composer changes to a different home or session context
- **THEN** it restores the selection state associated with that context rather than reusing submission cleanup as the reset trigger
