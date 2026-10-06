## ADDED Requirements

### Requirement: Hover controls with delayed hiding
The office SHALL hide its top controls at rest, reveal them immediately on hover, and wait 1000ms after pointer exit before hiding. Re-entry SHALL cancel pending hiding, and keyboard focus SHALL keep controls visible.

#### Scenario: Cross the transparent gap to the menu
- **WHEN** the pointer leaves the room and enters the menu within 1000ms
- **THEN** the menu remains visible and usable

#### Scenario: Pointer leaves after clicking a control
- **WHEN** a mouse click leaves focus on a menu button and the pointer exits the office
- **THEN** the menu hides after 1000ms and its native hit region is removed

#### Scenario: Keyboard navigation
- **WHEN** keyboard focus enters the controls
- **THEN** controls become visible and remain visible while keyboard focus stays within the office controls

### Requirement: Native pin state
The office SHALL provide a pin button at the right side of its menu, default to pinned, and acknowledge the native state through the bridge. Unpinned windows SHALL be coverable by normal windows and remain unpinned during size and placement updates.

#### Scenario: Unpin and resize
- **WHEN** the user unpins the office and then zooms, resets size or docks it
- **THEN** it retains the normal window level

#### Scenario: Pin and reload
- **WHEN** the user pins the office or the office page reloads
- **THEN** the button reflects the actual native pinned state

### Requirement: Close only the office
The office SHALL provide an accessible close icon after the pin button and close only the pet window using the native close lifecycle.

#### Scenario: Close from the top menu
- **WHEN** the user activates the close button
- **THEN** the pet closes while the main Desktop and agent sessions continue
