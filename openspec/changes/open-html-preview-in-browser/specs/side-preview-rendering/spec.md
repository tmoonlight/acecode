## ADDED Requirements

### Requirement: Local HTML preview opens in the integrated browser
The file source preview SHALL offer a keyboard-accessible browser icon before the wrap action for `.html` and `.htm` files, ignoring extension case, when the integrated browser is available. Activating it SHALL open the saved local file in a new browser tab belonging to the current session.

#### Scenario: Resolve and encode a local file
- **WHEN** the user activates the browser action for an HTML preview
- **THEN** the address uses the file's absolute path or its preview working directory to resolve a relative path
- **AND** the address uses `file://`, encoding spaces, Unicode, percent signs, hash signs and question marks as path characters
- **AND** Windows drives, UNC shares and POSIX paths are supported

#### Scenario: Preserve unsaved work
- **WHEN** the current preview has unsaved edits and the user activates the browser action
- **THEN** the existing save/discard/cancel guard runs before creating a browser page
- **AND** cancellation or a failed save keeps the editor active without creating a page

#### Scenario: Unsupported preview or runtime
- **WHEN** the preview is not HTML/HTM, has no resolvable absolute path, or the integrated browser is unavailable
- **THEN** the browser action is not offered

#### Scenario: Browser failure
- **WHEN** browser page creation or navigation fails
- **THEN** the application displays the failure and retains the source file tab

#### Scenario: Native page is still initializing
- **WHEN** the newly created native browser page is not yet ready
- **THEN** the application waits for readiness before navigating to the file
- **AND** a closed page, startup failure, timeout, or session switch stops that navigation
