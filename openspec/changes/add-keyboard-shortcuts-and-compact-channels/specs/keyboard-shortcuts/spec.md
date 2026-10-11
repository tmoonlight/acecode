## Purpose

Make frequent ACECode Web and Desktop actions discoverable and executable through a searchable keyboard shortcut directory while protecting text composition and contextual controls.

## ADDED Requirements

### Requirement: Editable persistent bindings
Each listed shortcut SHALL expose an edit icon after its key display. Recording SHALL capture the new combination without performing any application action. Saving SHALL replace the old binding immediately, update visible hints, and persist across reopening. Cancel SHALL leave the binding unchanged. Restore default SHALL affect only the selected binding. Previous/next history keys SHALL be independently editable.

#### Scenario: Rebind a common action
- **WHEN** the user edits New session from Ctrl+Alt+N to Ctrl+Alt+J and saves
- **THEN** only the new chord creates a session, its label updates, and the binding survives reload

#### Scenario: Conflict or persistence failure
- **WHEN** the chosen chord conflicts with another action, is reserved, or cannot be saved
- **THEN** an actionable inline message is shown and the old binding remains active

#### Scenario: Contextual shortcut
- **WHEN** the user edits a sending, newline, history, saving, finding or dismissal binding
- **THEN** the new chord works in the owning context with existing IME and focus protections

### Requirement: Searchable shortcut settings
Settings SHALL expose a 快捷键 section containing both existing and new shortcuts, searchable by action, description and key combination. The search bar SHALL remain visible at the top of the settings content scroll area.

#### Scenario: Search while scrolling
- **WHEN** the user opens 快捷键, scrolls its list and searches for Ctrl or a function name
- **THEN** the opaque search bar stays at the top and the list filters to matching shortcuts

### Requirement: Common application actions
With default bindings, the application SHALL support Ctrl+Alt+N for a new current-workspace session, Ctrl+Alt+I to focus its input without changing draft text, Ctrl+Alt+B/E for left/right panels, Ctrl+, for settings, Ctrl+/ for shortcut settings, and Ctrl+Shift+. for stopping current work. Existing search, console, find and contextual editing shortcuts SHALL retain their behavior until explicitly rebound. macOS SHALL display and accept corresponding Cmd/Option defaults except the existing Ctrl+backquote console binding.

#### Scenario: Navigation history
- **WHEN** the user presses Ctrl+- or Ctrl+Shift+-
- **THEN** ACECode goes forward or back respectively through its navigation history, retains unsaved-file protections and does not zoom

#### Scenario: Protected input contexts
- **WHEN** IME composition, a modal, a menu or a focused secondary editor owns input
- **THEN** unrelated application actions do not run behind it, and one keypress never executes an action twice

#### Scenario: New conversation and stop
- **WHEN** the user requests a new session or stops a running session by keyboard
- **THEN** the corresponding existing action runs once in the active workspace/session and preserves drafts and cancellation behavior

### Requirement: Incremental reasoning depth
With default bindings, Ctrl+Alt+> SHALL increase and Ctrl+Alt+< SHALL decrease the active conversation's supported reasoning depth. The physical Period/Comma keys with or without Shift SHALL be accepted by default. The effective default depth SHALL be respected, unsupported levels skipped, bounds clamped and in-flight changes guarded.

#### Scenario: Running conversation
- **WHEN** a running conversation changes depth using the keyboard
- **THEN** its displayed selection updates via the existing reasoning control, future requests use the new depth, and the current request continues

#### Scenario: Boundary or unsupported model
- **WHEN** the user tries to move beyond an extreme or a model has no reasoning selector
- **THEN** no wraparound or unsupported update is sent
