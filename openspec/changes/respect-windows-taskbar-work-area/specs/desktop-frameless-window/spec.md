## ADDED Requirements

### Requirement: Windows first-show window respects the live taskbar work area
The Windows desktop shell SHALL fit and center the actual post-DPI-scaling outer
window inside the selected monitor's current work area immediately before the
window is first shown. The final rectangle MUST exclude the taskbar-reserved
area and preserve the configured DPI-aware safe margins wherever the work area
can contain them.

#### Scenario: WebView DPI scaling enlarges the native window
- **WHEN** WebView applies DPI scaling after the initial preferred size was calculated
- **THEN** the shell MUST measure the resulting native outer window
- **AND** the shell MUST reduce and center that actual rectangle inside the current monitor work area before showing it

#### Scenario: Taskbar reduces the usable display height
- **WHEN** the selected monitor work-area bottom is above the full monitor bottom because of a taskbar
- **THEN** the first visible outer-window bottom MUST remain above the work-area bottom
- **AND** no startup content MAY be positioned beneath the taskbar

#### Scenario: Taskbar geometry changes during startup
- **WHEN** the selected monitor work area changes between off-screen host creation and first display
- **THEN** the shell MUST use the latest available work-area rectangle for final sizing and centering

#### Scenario: Off-screen custom host creation falls back
- **WHEN** the Windows shell uses a WebView-owned fallback window instead of the custom off-screen host
- **THEN** the fallback window MUST receive the same final work-area fit and centering before first-show completion
