## MODIFIED Requirements

### Requirement: UI renders checklist progress
The system SHALL render the latest task list as a compact checklist in Web/Desktop and TUI, and the regular TUI sidebar SHALL reveal the complete checklist while Ctrl+O detail mode is active.

#### Scenario: Task statuses are shown
- **WHEN** the current list contains pending, in-progress, completed, or cancelled items
- **THEN** pending items are shown unchecked
- **THEN** in-progress items are shown as active
- **THEN** completed items are shown checked
- **THEN** cancelled items are shown as cancelled

#### Scenario: No tasks exist
- **WHEN** the current task list is empty
- **THEN** the checklist surface is hidden

#### Scenario: Regular TUI sidebar uses compact checklist by default
- **WHEN** the regular TUI sidebar is visible and Ctrl+O detail mode is off
- **THEN** at most 10 TodoWrite items SHALL be rendered before a hidden-count row
- **AND** in-progress items SHALL be ordered before pending items
- **AND** pending items SHALL be ordered before completed or cancelled items
- **AND** items with the same status priority SHALL retain their original relative order

#### Scenario: Ctrl+O reveals the complete TUI checklist
- **WHEN** the regular TUI sidebar is visible and Ctrl+O detail mode is on
- **THEN** every TodoWrite item SHALL be rendered in the sidebar
- **AND** no TodoWrite hidden-count row SHALL be rendered
- **AND** the compact checklist's status ordering and per-item formatting SHALL be retained
