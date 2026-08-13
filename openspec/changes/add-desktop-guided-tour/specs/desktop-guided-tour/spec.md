## ADDED Requirements

### Requirement: Desktop guide eligibility
The system SHALL automatically prepare and start the current guided-tour version only in embedded Desktop or Edge compatibility mode after authentication, startup navigation, the Home surface, and higher-priority overlays are settled. The system SHALL enter its transient Home preparation state before requiring all guided-tour targets to be present.

#### Scenario: First eligible Desktop home entry
- **WHEN** the current guide version is not dismissed and the Desktop home surface becomes eligible
- **THEN** the system prepares the Home layout, waits for all required targets, and starts the guided tour at its first step

#### Scenario: Required targets mount after eligibility
- **WHEN** logical first-run eligibility is satisfied before one or more Home targets have mounted
- **THEN** the system enters preparation and retries target discovery for a bounded interval instead of permanently suppressing the guide

#### Scenario: Browser-direct entry
- **WHEN** the same web UI is opened as a normal browser client without a Desktop mode marker
- **THEN** the system does not automatically start the guided tour

#### Scenario: Deep-linked session entry
- **WHEN** Desktop starts with a session deep link
- **THEN** the system lets session navigation settle without showing the tour and defers eligibility until a later Home entry

#### Scenario: Onboarding state cannot be read
- **WHEN** the daemon onboarding-state request fails
- **THEN** the system keeps the application usable and does not automatically show the tour

### Requirement: Seven-step guided workflow
The system SHALL present exactly seven ordered steps covering the project sidebar, Add Project, New Conversation, Home workspace selector, Home composer, status-bar model and permission controls, and the Settings entry.

#### Scenario: Normal configured workflow
- **WHEN** an eligible user advances through the guide with a configured model
- **THEN** each of the seven stable targets is highlighted in order and the final action completes the guide

#### Scenario: Project entry points are distinguished
- **WHEN** the guide reaches Add Project and New Conversation
- **THEN** it explains that Add Project selects an existing local directory while New Conversation starts a task and does not create a project directory

#### Scenario: No model configured
- **WHEN** the guide reaches the status and Settings steps without a configured model
- **THEN** the guide explains that a model is required and the final action dismisses the guide before opening Model Settings

#### Scenario: No workspace selected
- **WHEN** the Home workspace selector is set to no workspace
- **THEN** the guide continues and explains that no-workspace tasks are supported

### Requirement: Stable and safe target interaction
The system MUST use unique semantic target markers and MUST prevent the guided tour from triggering the highlighted application controls.

#### Scenario: Sidebar preference is collapsed
- **WHEN** the guide starts while the stored sidebar preference is collapsed
- **THEN** the preparation state visibly expands the sidebar before target validation without changing the stored preference, and the sidebar is restored after the guide ends

#### Scenario: Required target disappears
- **WHEN** a required target unmounts during a tour because the Home or session state changes
- **THEN** the system aborts the visible tour without persisting dismissal

#### Scenario: Window layout changes
- **WHEN** the Desktop window resizes or a target transition completes during an active step
- **THEN** the spotlight and tooltip are recalculated from the current target bounds

### Requirement: Dismissal and replay
The system SHALL treat Close, Escape, Skip, and final completion as dismissal of the current guide version and SHALL provide a Settings action that replays the guide without clearing the dismissal marker.

#### Scenario: User closes midway
- **WHEN** the user selects Close, presses Escape, or skips before the final step
- **THEN** the current guide version is persisted as dismissed and the tour does not auto-run again

#### Scenario: User completes the guide
- **WHEN** the user completes the final step
- **THEN** the current guide version is persisted as dismissed

#### Scenario: User replays from Settings
- **WHEN** the user activates "重新查看新手指引" in Settings
- **THEN** Settings closes, Home becomes active, and the guide starts from step one without clearing the stored dismissal marker

### Requirement: Durable versioned state
The daemon SHALL expose authenticated, idempotent onboarding status and dismissal endpoints backed by a versioned `state.json` flag that survives loopback-port and Edge-profile changes.

#### Scenario: Read current status
- **WHEN** an authenticated client requests Desktop onboarding status
- **THEN** the daemon returns the backend-owned guide version and current dismissed state

#### Scenario: Persist dismissal
- **WHEN** an authenticated client dismisses the current guide version
- **THEN** the daemon atomically records the versioned dismissal flag and returns the dismissed state

#### Scenario: Persistence fails
- **WHEN** the state file cannot be updated
- **THEN** the daemon returns a persistence error and does not report dismissal success

#### Scenario: Repeated dismissal
- **WHEN** the dismissal endpoint is called more than once for the same version
- **THEN** each successful call returns the same dismissed state without corrupting other state fields

### Requirement: Overlay and accessibility coordination
The guided tour MUST pause for higher-priority business overlays and MUST remain usable by keyboard and assistive technologies.

#### Scenario: Permission or question overlay appears
- **WHEN** a permission, question, search, or Settings overlay becomes active during the guide
- **THEN** the guide pauses and yields focus and Escape handling until the overlay closes

#### Scenario: Keyboard-only navigation
- **WHEN** a user operates the tour using only the keyboard
- **THEN** focus stays within the active tour dialog, labeled controls are reachable in order, and focus is restored when the tour ends

#### Scenario: Reduced-motion preference
- **WHEN** the operating environment requests reduced motion
- **THEN** tour transitions and automatic smooth scrolling are disabled

### Requirement: Bounded production package growth
The implementation MUST measure the single-file production build against the recorded baseline and MUST NOT accept a tour dependency that grows the artifact by more than 180 KiB raw or 50 KiB gzip.

#### Scenario: Preferred dependency stays within the gate
- **WHEN** the React Joyride implementation is built for production
- **THEN** both raw and gzip deltas are recorded and the dependency is retained only if both limits pass

#### Scenario: Preferred dependency exceeds the gate
- **WHEN** either production delta exceeds its limit
- **THEN** the implementation replaces React Joyride with a lighter compatible tour engine and repeats the measurement before completion
