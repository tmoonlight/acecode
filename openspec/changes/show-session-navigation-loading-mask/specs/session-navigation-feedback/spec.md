## ADDED Requirements

### Requirement: Session jumps expose a blocking progress state
The Web UI SHALL display a full-viewport loading mask with a visible loading animation while an asynchronous conversation jump is restoring or opening its target session. The mask MUST prevent interaction with the intermediate page.

#### Scenario: Search result starts an inactive session jump
- **WHEN** a user selects a conversation result and the target session must be resumed
- **THEN** the search palette MAY close
- **THEN** a full-viewport loading mask MUST be visible before the underlying home or previous conversation can be interacted with
- **THEN** the mask MUST include an animated loading indicator and text describing that the conversation is opening

#### Scenario: Session jump completes successfully
- **WHEN** the target session has been resumed and its active session reference is committed
- **THEN** the loading mask MUST be removed as the target conversation becomes the active view
- **THEN** the home or previous conversation MUST NOT be exposed as an interactive intermediate state

#### Scenario: Session jump fails
- **WHEN** workspace activation or session resume fails without handing off to a new page
- **THEN** the loading mask MUST be removed
- **THEN** the existing navigation error feedback MUST remain visible to the user

### Requirement: Redirected session jumps preserve progress feedback
The Web UI SHALL preserve session-jump progress feedback across a cross-workspace full-page navigation.

#### Scenario: Source page hands off to a workspace URL
- **WHEN** a session jump assigns a new page URL for a different workspace
- **THEN** the source page MUST keep the loading mask visible until that page unloads

#### Scenario: Destination resumes an open target
- **WHEN** a page starts with a valid session target in its `open` query parameter
- **THEN** the destination MUST render a full-viewport loading state while it resumes that target
- **THEN** the loading state MUST end when startup navigation settles

### Requirement: Session-jump feedback is accessible
The session navigation loading mask SHALL expose its progress semantics to assistive technology.

#### Scenario: Loading mask is rendered
- **WHEN** session navigation is pending
- **THEN** the mask MUST be announced as a polite status
- **THEN** the mask MUST expose a busy state and a textual conversation-opening label

### Requirement: Session jumps always expose a bounded recovery path
The Web UI SHALL release a pending session-navigation mask after a bounded wait or an explicit user cancellation, and a cancelled operation MUST NOT activate its target later.

#### Scenario: Session resume request times out
- **WHEN** an ordinary session resume API request does not settle within the configured request timeout
- **THEN** the request fails with a structured timeout error
- **AND** the shared navigation cleanup removes the loading mask

#### Scenario: Non-request navigation step does not settle
- **WHEN** a workspace bridge or another navigation step remains pending beyond the mask fallback interval
- **THEN** the loading mask is removed
- **AND** the user receives local timeout feedback

#### Scenario: User cancels pending navigation
- **WHEN** the loading mask is visible and the user presses Escape
- **THEN** every currently pending navigation releases the mask immediately
- **AND** any later activation or resume completion from those cancelled operations is ignored before URL assignment or active-session commit

#### Scenario: Legitimately blocking API endpoint is used
- **WHEN** an API endpoint waits on a native modal interaction or an explicitly long model round trip
- **THEN** that endpoint uses its declared timeout exemption or extended budget instead of the ordinary request timeout
