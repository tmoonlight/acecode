## MODIFIED Requirements

### Requirement: File preview exposes a word-wrap toggle
The file preview SHALL offer its word-wrap and copy commands in the breadcrumb row's trailing menu for a loaded source view.

#### Scenario: Preview renders action buttons
- **WHEN** the user opens the file actions menu
- **THEN** it offers copy and indicates whether the wrap command enables or disables wrapping

#### Scenario: Non-preview states do not show wrap control
- **WHEN** the file is loading, in error, or showing rendered Markdown
- **THEN** the menu omits the word-wrap command
