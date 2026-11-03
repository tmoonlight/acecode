## Purpose

Let users save and copy files directly from conversation turn file cards, consistently with existing file operations and the active session's working directory.

## ADDED Requirements

### Requirement: Direct file card menu
Turn file cards SHALL offer Save as, Copy path, Copy file contents, and Open in Explorer in that order, using the shared context menu. Native-only actions SHALL follow the existing runtime availability policy.

#### Scenario: Right click within a card
- **WHEN** the user right-clicks a card's filename, icon, blank area, or change action
- **THEN** the menu targets that card's file without opening a preview or diff

#### Scenario: Normal card interactions
- **WHEN** the user activates a file or change button normally
- **THEN** the existing file preview, turn-specific diff, and folding behaviors remain available

### Requirement: Resolve the card's actual file
File actions SHALL use the active session directory and the card's corrected open path, including historical worktree paths and absolute paths outside the workspace. Copy path SHALL copy an absolute path. Unresolvable targets SHALL be disabled.

#### Scenario: Historical worktree path
- **WHEN** a recorded path repeats the current worktree prefix
- **THEN** all four actions resolve the same corrected file as the existing file preview

### Requirement: Save original file bytes
Save as SHALL preserve current file bytes and filename, use the existing native save picker in desktop-capable runtimes, and use an authenticated download in ordinary browsers. Cancellation SHALL neither write a file nor report success. Errors SHALL be visible.

#### Scenario: Binary or text file export
- **WHEN** the user saves an existing file to a selected destination
- **THEN** the destination contains the original bytes with no text conversion

#### Scenario: Cancel or missing file
- **WHEN** the picker is cancelled or the source cannot be read
- **THEN** no success notification appears and a read failure is reported

### Requirement: Copy current text
Copy file contents SHALL read current text through the existing file reader and copy it through the system clipboard. Empty text SHALL be supported. Binary, oversized, missing, and clipboard failures SHALL be reported without copying a diff, preview fragment, or error text.

#### Scenario: File changed after the turn
- **WHEN** the user copies a file edited since the recorded turn
- **THEN** its current full text is copied

#### Scenario: Unsupported text
- **WHEN** the text reader rejects a binary or oversized file
- **THEN** the clipboard remains unchanged and the user sees an error
