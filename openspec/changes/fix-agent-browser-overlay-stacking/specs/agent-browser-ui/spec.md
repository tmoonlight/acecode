## MODIFIED Requirements

### Requirement: Application floating surfaces appear above Agent Browser

ACECode Desktop SHALL render registered application menus, popovers, dropdowns, tooltips, toasts, and dialogs above the visible native Agent Browser surface on every supported Desktop platform. Overlap surfaces SHALL preserve visible and interactive Browser content outside their intersection. Blocking surfaces SHALL hide the Browser while the blocking surface is active.

#### Scenario: Context menu intersects the Browser
- **WHEN** an ACECode context menu opens and any part of its registered surface intersects the visible Agent Browser viewport
- **THEN** the complete intersecting menu region SHALL be visible above Browser content
- **AND** pointer input inside that region SHALL reach the context menu
- **AND** Browser content outside that region SHALL remain visible and interactive

#### Scenario: Multiple floating surfaces intersect the Browser
- **WHEN** two or more registered overlap surfaces simultaneously intersect the visible Agent Browser viewport
- **THEN** every intersecting surface region SHALL be visible and interactive above Browser content
- **AND** the native layout payload SHALL contain a bounded normalized representation of their combined intersections

#### Scenario: Modal surface is active
- **WHEN** a registered blocking surface is visible
- **THEN** the Agent Browser SHALL be hidden until the blocking surface closes

#### Scenario: Precise native occlusion cannot be applied
- **WHEN** the native bridge rejects the current layout containing overlap rectangles
- **THEN** ACECode SHALL hide the Agent Browser while those overlap surfaces remain active
- **AND** stale bridge acknowledgements SHALL NOT override a newer layout decision

#### Scenario: Floating surface closes
- **WHEN** all registered floating surfaces stop intersecting the Browser and no blocking surface is visible
- **THEN** the Agent Browser SHALL restore its complete visible and interactive surface
