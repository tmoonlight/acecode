## ADDED Requirements

### Requirement: Changes view mode selection
The Changes tab SHALL offer flat-list and directory-tree presentation modes through an accessible view control. Flat mode SHALL remain the default when no valid stored preference exists, and a user selection SHALL be restored after the WebUI reloads.

#### Scenario: Existing user opens Changes
- **WHEN** the user has never selected a Changes presentation mode
- **THEN** the changed files are rendered with the existing flat-list presentation

#### Scenario: User selects tree mode
- **WHEN** the user activates the tree-view control and later reloads the WebUI
- **THEN** the Changes tab renders in tree mode and the tree control remains selected

#### Scenario: User returns to flat mode
- **WHEN** the user activates the flat-list control
- **THEN** the same changed files return to the flat filename and parent-path presentation

### Requirement: Shared hierarchy for change sources
Tree mode SHALL project both Git-level changes and session-level structured changes into directories derived from normalized path segments while retaining each file's original path for actions and diff lookup.

#### Scenario: Nested Git changes are displayed
- **WHEN** Git changes include `src/app/main.cpp` and `src/lib/util.cpp`
- **THEN** tree mode displays one `src` directory with `app` and `lib` child directories containing the corresponding file rows

#### Scenario: Windows separators and workspace-absolute paths are supplied
- **WHEN** a session change path uses backslashes or is an absolute descendant of the current workspace
- **THEN** tree mode uses normalized workspace-relative directory names without changing the original file path passed to open actions

#### Scenario: Root and nested files coexist
- **WHEN** the change set contains a root-level file and files in nested directories
- **THEN** tree mode displays the root-level file and the directory hierarchy without dropping either entry

### Requirement: Directory expansion behavior
Tree directories SHALL start expanded, expose their expanded state to assistive technology, and allow independent collapse and expansion. When an externally selected file is inside a collapsed branch, its ancestors SHALL expand so the selected file can be revealed.

#### Scenario: User collapses and expands a directory
- **WHEN** the user activates an expanded directory row and then activates it again
- **THEN** its descendants are first hidden and then restored without changing the underlying change set

#### Scenario: Selected file is inside a collapsed directory
- **WHEN** another Changes surface selects a file whose ancestor directory is collapsed
- **THEN** tree mode expands the ancestor chain and scrolls the selected file row into view

### Requirement: File status and actions remain available
Changing presentation mode SHALL NOT remove existing file selection, review navigation, file-preview action, change-count display, Git status badge, desktop context metadata, or deleted-file reveal protection.

#### Scenario: Git file is rendered in tree mode
- **WHEN** a changed Git file has status, addition/deletion, and preview metadata
- **THEN** its tree row displays the same status and counts and invokes the same review and preview callbacks as its flat row

#### Scenario: Deleted Git file is rendered in tree mode
- **WHEN** a Git file has deleted status
- **THEN** its tree row remains available for diff review but does not expose an actionable reveal-on-disk control

#### Scenario: Session file is rendered in tree mode
- **WHEN** a session-level structured change is shown in tree mode
- **THEN** its file row preserves the additions, deletions, selection state, and review navigation metadata
