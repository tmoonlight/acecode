## Purpose

Provide a compact desktop companion that gives users access to conversations, speech input and live session notifications through a navigation capsule matching the supplied visual references.

## ADDED Requirements

### Requirement: Reuse established bubbles and send control
Mini SHALL use the same bubble component and presentation as the companion pets, including status icons and stop/read actions. Its send control SHALL reuse the conversation send icon, theme colors and actual default dimensions rather than approximate them from screenshots.

#### Scenario: Compare surfaces at native DPI
- **WHEN** the conversation input and mini are displayed with default sizing
- **THEN** their send controls have matching rendered dimensions, icon and colors
- **AND** equivalent mini and companion-pet activity bubbles use the same rendered component and font metrics

### Requirement: Screen-aware activity expansion
Mini SHALL expand notifications below its toolbar when they fit, use the space above when the bottom is insufficient, and clamp the popup to the monitor work area. The popup SHALL show no visible scrollbar, preserve access to overflow items and keep the toolbar anchored when opening and closing.

#### Scenario: Open beside the bottom edge
- **WHEN** the requested notification content cannot fit below the toolbar and more space is available above
- **THEN** the popup opens upward without crossing the work-area boundary or moving the toolbar

#### Scenario: Content exceeds either side
- **WHEN** the content exceeds the available space above and below
- **THEN** the popup height and horizontal placement are clamped to the available work area
- **AND** wheel and keyboard interaction can reach overflow items without visible scrollbar chrome

### Requirement: Select and persist mini
The desktop app SHALL offer `mini` alongside existing pets in settings and native menus, persist its selection, and restore it on restart without enabling a disabled pet implicitly.

#### Scenario: Switch to mini and restart
- **WHEN** a user selects mini and restarts the desktop app
- **THEN** mini remains selected with the previous enabled state
- **AND** switching between mini, the office and animated pets loads the appropriate appearance

### Requirement: Compact navigation and real notifications
Mini SHALL show a white capsule with compose, waveform and notification icons, subtle separators and a soft shadow. Notifications SHALL count running, waiting and unread-result tasks and expand into title/status cards beneath the capsule. Expanded notifications SHALL replace the bell with a down arrow.

#### Scenario: Open and collapse session notifications
- **WHEN** a session needs attention and the user activates notifications
- **THEN** its current title and status appear below the capsule
- **AND** activating its card opens that session
- **AND** activating the down arrow or Escape collapses the cards

#### Scenario: No activity or disconnected
- **WHEN** there are no active notifications or the connection is lost
- **THEN** the expanded area shows a concise empty or connection state without stale session actions

### Requirement: Navigation actions use existing app flows
Mini SHALL open its own compact composer for editing and record immediately when its recording control is activated. Sending text or stopping a recording SHALL create and start a background task, increment its activity count after acceptance, and keep the main application window hidden or unfocused.

#### Scenario: Use shortcut actions
- **WHEN** the user selects compose or waveform
- **THEN** mini expands its own input bar or begins independent recording respectively
- **AND** neither action shows, navigates or focuses the main application window

#### Scenario: Submit a task from mini
- **WHEN** the user sends text or stops a recording
- **THEN** text or recognized speech is submitted as the first message of a new background session
- **AND** mini returns to its navigation state and counts that task once
- **AND** a failed submission retains the draft for retry without duplicating an already created session

#### Scenario: Cancel or fail recording
- **WHEN** recording is canceled, microphone permission fails, no speech is recognized, or mini is closed
- **THEN** recording resources are released and no unintended task starts
- **AND** errors remain visible in mini without opening the main window

#### Scenario: Voice task with an existing text draft
- **WHEN** a user records a separate task after hiding a text draft with attachments
- **THEN** voice success or failure preserves that text draft and its attachments
- **AND** voice retry sends only its own recognized text with its original request identity

#### Scenario: Many selected attachments
- **WHEN** selected attachment chips exceed the available mini window height
- **THEN** the attachment area scrolls and every removal control remains reachable at minimum size

### Requirement: Scaled shadows remain complete
The visible capsule and card shadows SHALL scale with the desktop pet and fit within its painting region at every supported size, while unrelated transparent space remains click-through.

#### Scenario: Shrink the pet
- **WHEN** mini is reduced to its minimum supported size
- **THEN** its capsule and card shadows retain smooth edges without a rectangular clipping boundary

### Requirement: Transparent desktop interaction
Mini SHALL support dragging, resizing and native context menus while keeping unused window areas click-through. It SHALL not use sprite animation or edge-peeking. Buttons SHALL retain accessible names and visible keyboard focus.

#### Scenario: Drag and resize the capsule
- **WHEN** the user drags the capsule, uses its resize grip or opens its context menu
- **THEN** the existing desktop window operation runs without also triggering a navigation action
- **AND** hit regions are updated after expansion, collapse and resizing
