## ADDED Requirements

### Requirement: Remember session summary visibility

The session summary SHALL start closed when the profile has no saved summary preference. Opening or closing it SHALL save that choice for subsequent sessions, page reloads and application restarts in the same profile.

#### Scenario: First session or upgrade from an older profile

- **WHEN** the user enters a session with no saved summary visibility
- **THEN** the summary is hidden and its toolbar toggle is off

#### Scenario: Open or close the summary

- **WHEN** the user opens or closes the summary and reloads the session
- **THEN** the panel and toolbar toggle reflect the saved state, including a value previously saved by an older version

#### Scenario: Narrow session window

- **WHEN** the saved choice is open and there is insufficient room to dock the summary
- **THEN** the summary opens as a popup, and dismissing it saves the closed choice
