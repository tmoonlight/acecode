## Purpose

Make frequent ACECode Web and Desktop actions discoverable and executable through a searchable keyboard shortcut directory while protecting text composition and contextual controls.

## ADDED Requirements

### Requirement: Searchable shortcut settings
Settings SHALL expose a 快捷键 section containing both existing and new shortcuts, searchable by action, description and key combination. The search bar SHALL remain visible at the top of the settings content scroll area.

#### Scenario: Search while scrolling
- **WHEN** the user opens 快捷键, scrolls its list and searches for Ctrl or a function name
- **THEN** the opaque search bar stays at the top and the list filters to matching shortcuts

### Requirement: Common application actions
The application SHALL support Ctrl+Alt+N for a new current-workspace session, Ctrl+Alt+I to focus its input without changing draft text, Ctrl+Alt+B/E for left/right panels, Ctrl+, for settings, Ctrl+/ for shortcut settings, and Ctrl+Shift+. for stopping current work. Existing search, console, find and contextual editing shortcuts SHALL retain their behavior. macOS SHALL display and accept corresponding Cmd/Option bindings except the existing Ctrl+backquote console binding.

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
Ctrl+Alt+> SHALL increase and Ctrl+Alt+< SHALL decrease the active conversation's supported reasoning depth. The physical Period/Comma keys with or without Shift SHALL be accepted. The effective default SHALL be respected, unsupported levels skipped, bounds clamped and in-flight changes guarded.

#### Scenario: Running conversation
- **WHEN** a running conversation changes depth using the keyboard
- **THEN** its displayed selection updates via the existing reasoning control, future requests use the new depth, and the current request continues

#### Scenario: Boundary or unsupported model
- **WHEN** the user tries to move beyond an extreme or a model has no reasoning selector
- **THEN** no wraparound or unsupported update is sent
