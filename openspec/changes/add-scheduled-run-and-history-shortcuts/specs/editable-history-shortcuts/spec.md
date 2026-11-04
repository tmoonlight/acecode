## Purpose

Give users consistent, configurable undo and redo shortcuts across message composition and text editing, preserving editor history, input method composition and shortcut recording behavior.

## ADDED Requirements

### Requirement: Configurable editing history shortcuts
The system SHALL list undo and redo under input/editing shortcuts, defaulting to Ctrl+Z and Ctrl+Shift+Z (Cmd on macOS). Changes SHALL persist, synchronize across windows, validate conflicts and support restoring defaults with immediate saving.

#### Scenario: Customize undo
- **WHEN** a valid replacement is recorded for undo in settings
- **THEN** the new binding takes effect immediately and survives reload; the old binding no longer undoes edits

#### Scenario: Invalid or failed save
- **WHEN** a binding conflicts or storage fails
- **THEN** the previous working binding remains active and the attempted binding remains visible with an error and retry where applicable

### Requirement: Respect editing ownership
The system SHALL apply history actions only to the focused editable input, using its existing history. Terminal input, read-only content, IME composition and shortcut recorders SHALL not execute these actions.

#### Scenario: Rich text history
- **WHEN** a user edits a message or Markdown file and invokes the configured undo then redo
- **THEN** the existing editor history restores and reapplies the change exactly once

#### Scenario: Native text input
- **WHEN** a user invokes a custom history binding in an editable native text field
- **THEN** that field uses its native undo or redo history
