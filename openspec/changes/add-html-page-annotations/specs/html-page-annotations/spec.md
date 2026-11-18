# HTML Page Annotations

## Purpose

Allow desktop users to communicate visual webpage changes through reversible previews, numbered target annotations and reliable conversation batches, without editing source files until they explicitly send the request to the model.

## ADDED Requirements

### Requirement: Desktop page annotation controls
The system SHALL support annotation mode on loaded Windows/macOS Agent Browser webpages, with toolbar entry, element outline/control button and host-controlled popup above the browser. It MUST NOT replace the popup with chat-column controls or expose general host/conversation capabilities to webpages.

#### Scenario: Save a description without sending
- **WHEN** the user selects an element, opens its control, enters a description and saves
- **THEN** one numbered pin and pending count appear, without a conversation message or HTML/CSS disk write
- **AND** closing annotation mode hides editing controls but keeps saved pins and previews

### Requirement: Reversible actual-change previews
The system SHALL preview visible text, text color, background, opacity, font, font size/weight, radius, border color/width, width/height, spacing, padding and margin. It SHALL record only actual old/new differences, preserve text child nodes and original inline styles, and revert only the current draft on cancel. Width/height lock SHALL default off and use the current aspect ratio when enabled.

#### Scenario: Cancel a second edit of the same element
- **WHEN** a saved preview exists and the user modifies that target again then cancels
- **THEN** the earlier saved preview and child-node identities remain, and only the current draft is reverted

#### Scenario: Compare with the original
- **WHEN** the user toggles original view
- **THEN** all preview modifications are temporarily disabled while pins remain visible
- **AND** toggling back restores saved previews without deleting annotations

### Requirement: Multiple targets and one region
The system SHALL allow multiple elements and at most one page-coordinate region per annotation, with one shared description and numbered pin. Regions SHALL NOT offer property/text editing. Saved annotations SHALL NOT be editable or deletable.

#### Scenario: Replace a selected region
- **WHEN** the user adds another region to a draft already containing one
- **THEN** the new region replaces the old and retains the draft's other targets and shared description

### Requirement: Acknowledged conversation batches
The system SHALL freeze pending sendable annotations into a batch bound to its initiating conversation and submit one user message containing numbered targets/descriptions/old-new differences and exactly one send-time full-page screenshot, including beyond the viewport. It SHALL prepare the screenshot/attachment before submission, preserve composer drafts/attachments, queue rather than steer busy model turns, and mark sent only after receipt. Retried batch identities MUST be deduplicated by the receiving boundary.

#### Scenario: Send while switching conversation
- **WHEN** a batch is being sent and the user switches conversation and adds another annotation
- **THEN** the original batch is submitted to its initiating conversation and the addition remains for the next batch
- **AND** accepted records keep pins/previews, pending count clears only for those records, and new numbers continue increasing

#### Scenario: Empty description and capture failure
- **WHEN** a batch has differences but no description
- **THEN** it is sendable, but capture/attachment failure prevents any partial submission and preserves retryable pending work
- **AND** records with neither description nor differences are not sent

#### Scenario: Uncertain submission receipt
- **WHEN** submission fails or acceptance is uncertain
- **THEN** the same batch identity is retained for reconciliation/retry and duplicate model requests are prevented

### Requirement: Page lifecycle and invalidation safety
The system SHALL prompt before active navigation/refresh with draft/pending work, default to staying, and never auto-send. Document replacement SHALL stop old previews and retain unsent invalid records without reattaching locators. Target removal SHALL invalidate the record and block its affected batch rather than matching similar elements. No usable conversation SHALL block send without creating or redirecting a conversation. Retention SHALL be limited to the current app run.

#### Scenario: Target disappears before send
- **WHEN** an annotated element is removed or its document is replaced
- **THEN** its unsent record is retained as invalid, no similar/new target is modified, and its batch cannot send

### Requirement: Acceptance and compatibility
The feature SHALL preserve existing source-selection annotations and require behavioral automated coverage plus real Windows and macOS complete-workflow acceptance, including full-page screenshot pixels beyond the viewport. Unexecuted checks MUST remain explicitly unverified.

#### Scenario: Platform acceptance is unavailable
- **WHEN** a platform's real desktop acceptance cannot be executed
- **THEN** it is recorded as unverified, not substituted by automated or reference-app observations
