> **Superseded（2026-11-22）**：本文档描述的路线（预览为主 / 整页截图 / 不可变记录 / 收据去重 / 导航确认）已被 `openspec/changes/redesign-html-page-annotations/` 取代，不再维护。
> 现行方案见 `docs/specs/2026-11-21-html-page-annotations-technical-design.md`，WIP 现状见 `docs/specs/2026-11-22-html-page-annotations-wip-alignment.md`。

# HTML Page Annotations

## Purpose

Allow desktop users to communicate visual webpage changes through reversible previews, numbered target annotations and reliable conversation batches, without editing source files until they explicitly send the request to the model.

## ADDED Requirements

### Requirement: Desktop page annotation controls
The system SHALL support annotation mode on loaded Windows/macOS Agent Browser webpages, with toolbar entry, an element outline and a host-controlled popup above the browser. A single unmodified primary click on an element SHALL open the annotation card directly; the runtime renders no separate in-page "Annotate" control button. It MUST NOT replace the popup with chat-column controls or expose general host/conversation capabilities to webpages.

#### Scenario: Save a description without sending
- **WHEN** the user selects an element, opens its control, enters a description and saves
- **THEN** one numbered pin and pending count appear, without a conversation message or HTML/CSS disk write
- **AND** closing annotation mode hides editing controls but keeps saved pins and previews

### Requirement: Pointer-accurate annotation surface
While annotation mode is active and no popup is blocking selection, moving the pointer over a page element SHALL show a hover outline and a compact information card containing the target tag, rendered size, color and font. A single primary click SHALL open the host-controlled annotation card immediately. The hover outline and information card SHALL be suppressed while any WebUI menu is open or a modal dialog is active, and SHALL be removed when the pointer leaves the webview (`pointerout` with no related target) or the window loses focus, so the last card never stays pinned over the page. These feedback-only state changes MUST NOT mark the annotation session busy.

The annotation card SHALL keep its header and action footer fixed, cap its scrolling property editor at 230px, omit a separate target-element type row, left-align property labels by text, split numeric values from units, expose a restore action for edited values, and preview property edits live. Opacity SHALL use a visible `0..100%` control and clamp out-of-range input. Width/height and vertical/horizontal padding/margin locks SHALL center a circular lock control between upper and lower T-shaped guides, distinguish default, hover and locked states, and propagate paired values independently. Collapsed padding and margin rows SHALL retain four editable values.

Paired numeric controls SHALL keep each value and its unit fully inside the control before and after a restore action appears. Collapsed four-way spacing controls SHALL provide at least 45px of usable width per input and wrap rather than shrink below that width. Width/height and spacing pair locks SHALL use a shared T-shaped bracket with a centered circular lock badge in default, hover and locked states. In the expanded card the target tag (for example `/p` or `/div`) SHALL share the drag-handle row with the grip, and the scrolling editor's last visible-text field SHALL keep a 2px gap above the action-bar divider.

The editor SHALL also support letter spacing, word spacing and separate row/column gaps, a font picker with preset families and nine weight steps (100–900), and inferred-and-propagating vertical/horizontal spacing pairs. The description field SHALL switch its placeholder between collapsed and adjusting states and cap input at 4096 characters.

#### Scenario: Dismiss an open annotation card
- **WHEN** the annotation card is open and the user clicks an unmodified primary click outside it
- **THEN** the current unsaved card draft is cancelled and annotation mode remains active

#### Scenario: Use the floating annotation toolbar
- **WHEN** annotation mode is active without an open card or active selection mode
- **THEN** the toolbar exposes screenshot copy, hide markers, hold-to-view-original, discard-all, send and exit with their defined tooltips and pressed states
- **AND** discard-all first shows the exact pending-annotation confirmation
- **AND** the toolbar is hidden while a card or selection mode is active

### Requirement: Reversible actual-change previews
The system SHALL preview visible text, text color, background, opacity, font, font size/weight, letter spacing, word spacing, radius, border color/width, width/height, row/column gap, spacing, padding and margin. It SHALL record only actual old/new differences, preserve text child nodes and original inline styles, and revert only the current draft on cancel. Width/height lock SHALL default off and use the current aspect ratio when enabled.

#### Scenario: Cancel a second edit of the same element
- **WHEN** a saved preview exists and the user modifies that target again then cancels
- **THEN** the earlier saved preview and child-node identities remain, and only the current draft is reverted

#### Scenario: Compare with the original
- **WHEN** the user toggles original view
- **THEN** all preview modifications are temporarily disabled and numbered pins are hidden
- **AND** toggling back restores saved previews and numbered pins without deleting annotations

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

#### Scenario: Discard all pending annotations
- **WHEN** the user confirms discard-all
- **THEN** every saved record and pin is cleared and numbering restarts at 1, with the page's previews and drafts fully reverted

### Requirement: Native overlay edge fidelity
Every floating surface introduced or touched this iteration — annotation cards, the floating toolbar, the element hover card, browser "more" and tab context menus, and font dropdowns — SHALL keep a complete 1px border and SHALL NOT show a white halo or jagged edge where it overlaps a colored webpage. Native occlusion holes SHALL follow each overlay's border radius, keep corners that are clipped by the browser viewport square, and extend just beyond the overlay border box so the native 1-bit region does not consume the border. Implicit menu/listbox/tooltip overlays SHALL use a small bounded default bleed that can be disabled per element.

#### Scenario: Rounded overlay over a colored page
- **WHEN** a rounded menu or the annotation card overlaps a non-white page region
- **THEN** its border stays visible on all four sides with no white ring, and corners clipped by the viewport remain square

### Requirement: Layered modal suspension and native input blocking
When a trusted WebUI menu is open or a modal dialog is active over the browser surface, the system SHALL suspend in-page annotation chrome (selection/hover outlines, hover card and region outlines) and, for modal dialogs that keep the page visible, SHALL keep the native page painted while blocking pointer and keyboard input to it. Suspension SHALL be reference-counted by named source so overlapping blockers resume exactly once. Discard confirmation SHALL keep the page visible and MUST NOT close when the backdrop is clicked. Feedback-only suspension MUST NOT mark the annotation session busy.

#### Scenario: Menu or dialog covers the page
- **WHEN** a WebUI menu opens or the discard confirmation dialog is shown
- **THEN** in-page outlines and hover cards are hidden, native page input is blocked for the dialog, and closing every blocker restores chrome exactly once

### Requirement: Send feedback and error localisation
The toolbar SHALL show `请先选择对话` inline and disable send when no conversation is selected, rather than failing only after a click. Native send failures and page-capture failures SHALL be mapped to Chinese product messages before display — including `批注目标已失效，请重新选择元素后再发送`, `网页内容已变化，请重新批注后再发送`, `截图正在进行中，请稍后再试`, `批注页面不可用`, `网页截图失败`, `无法复制截图` — and raw machine codes or English runtime text MUST NOT leak into the UI. The visible-text editor SHALL likewise map its native text-preview errors to Chinese copy. Screenshot copy SHALL confirm with `已复制截图` and clear the confirmation automatically.

#### Scenario: Stale target or no conversation
- **WHEN** the annotated target changes before send, or no conversation is selected
- **THEN** the user sees a Chinese product message or the inline `请先选择对话` note, never a raw error code, and send is disabled when no conversation is selected

### Requirement: Annotation message presentation
A submitted batch SHALL render as a message card showing the page title, non-empty descriptions and the full-page screenshot; numeric labels SHALL appear only when more than one description exists. Only a well-formed `Page annotations` JSON payload SHALL be parsed into the card. The card SHALL expose a `{N} 条注释` footer, allow the screenshot to be opened in an image viewer, and copy as a plain-text title/description summary rather than the raw JSON message. The batch screenshot attachment, named with a `page-annotations-*.png` prefix, SHALL be excluded from the generic attachment strip to avoid duplication.

#### Scenario: View and copy an annotation message
- **WHEN** a sent batch appears in the transcript and the user opens its screenshot or copies the message
- **THEN** the screenshot opens in an image viewer and the copy is a plain-text title/description summary, with the screenshot shown only inside the card

### Requirement: Acceptance and compatibility
The feature SHALL preserve existing source-selection annotations and require behavioral automated coverage plus real Windows and macOS complete-workflow acceptance, including full-page screenshot pixels beyond the viewport. Unexecuted checks MUST remain explicitly unverified.

#### Scenario: Platform acceptance is unavailable
- **WHEN** a platform's real desktop acceptance cannot be executed
- **THEN** it is recorded as unverified, not substituted by automated or reference-app observations
