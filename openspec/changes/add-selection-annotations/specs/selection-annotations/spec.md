## ADDED Requirements

### Requirement: Preview selections expose direct chat actions
The Web UI SHALL show a floating action surface beside a non-empty selection inside code, text, or Markdown file previews. The surface SHALL contain `引用到聊天` followed by `批注`, SHALL remain inside the visible viewport, and SHALL coexist with the existing right-click `引用到聊天` action.

#### Scenario: Selecting preview text opens actions
- **WHEN** the user completes a non-empty selection in a supported file preview
- **THEN** the UI shows `引用到聊天` and `批注` beside the released mouse cursor

#### Scenario: Dragging a multiline selection does not move actions to the preview edge
- **WHEN** the user drags across multiple source lines
- **THEN** no action surface is shown until mouse release and the resulting surface is anchored beside the release cursor rather than the selection's union rectangle

#### Scenario: Keyboard selection opens actions
- **WHEN** the user completes a supported non-empty selection with the keyboard
- **THEN** the UI shows the same actions beside the selected range

#### Scenario: Popover interaction keeps the captured selection
- **WHEN** the user clicks `批注` or types inside the annotation editor
- **THEN** those events do not re-anchor the action surface or replace the captured preview selection

#### Scenario: Unsupported surfaces do not open actions
- **WHEN** the user selects content in a PDF, Word, spreadsheet, image, chat transcript, diff, editor, or another non-preview surface
- **THEN** the selection action surface is not shown

#### Scenario: Direct quote pins existing context
- **WHEN** the user chooses `引用到聊天`
- **THEN** the exact selected text is pinned through the existing selection-context composer flow and the floating surface closes

### Requirement: Users can annotate a selected passage
Choosing `批注` SHALL replace the action surface with a focused annotation editor that retains the captured selection after browser focus moves. Annotation text MUST contain non-whitespace content and MUST be bounded by the configured annotation limit.

#### Scenario: Submit an annotation
- **WHEN** the user enters annotation text and presses `Enter`
- **THEN** the selected text and normalized annotation are pinned together into the composer and the editor closes

#### Scenario: Enter multiline annotation
- **WHEN** the annotation editor has focus and the user presses `Shift+Enter`
- **THEN** the editor inserts a newline without submitting

#### Scenario: Cancel annotation
- **WHEN** the annotation editor has focus and the user presses `Escape`
- **THEN** the editor closes without pinning an annotation

#### Scenario: Reject blank annotation
- **WHEN** the annotation contains only whitespace
- **THEN** the UI keeps the editor open and does not create an annotated context

### Requirement: Composer contexts support multiple annotations
An annotated selection SHALL remain a selection context with a normalized `annotations` array. Pinning another annotation at a location already present in the current composer SHALL append a distinct annotation to that context instead of creating another context card.

#### Scenario: Add annotation to pending reference
- **WHEN** a plain reference for the same source location is already pinned and the user submits an annotation
- **THEN** the composer keeps one selection card and that context gains the annotation

#### Scenario: Add two pending annotations
- **WHEN** the user submits two distinct annotations for the same selected passage before sending
- **THEN** one composer context retains both annotations in creation order

#### Scenario: Plain duplicate remains deduplicated
- **WHEN** the same source location is quoted repeatedly without a new annotation
- **THEN** the composer does not add duplicate selection cards

### Requirement: Annotated references reuse chat presentation
Composer and sent-message annotated references SHALL reuse the existing selection-card surface. An annotated card SHALL expose the same per-file passage number as its source-preview annotation bubble and SHALL reveal the grouped annotation content on hover or keyboard focus without replacing the file label or line range.

#### Scenario: Composer cards match source bubble numbering
- **WHEN** one file has annotations on three different passages
- **THEN** the existing composer cards show `1`, `2`, and `3` in the same first-appearance order as the source-preview bubbles

#### Scenario: Sent cards preserve source bubble numbering
- **WHEN** annotated contexts appear in sent user messages
- **THEN** each sent-message selection card keeps the same per-file passage number as the matching source-preview bubble

#### Scenario: Card annotation hover is never empty
- **WHEN** the user hovers or focuses an annotated composer or sent-message card number
- **THEN** the tooltip shows all annotations grouped at that passage in creation order and does not render as an empty surface

#### Scenario: Separate files number independently
- **WHEN** annotated selections reference different files
- **THEN** the first annotated passage in each file is numbered `1`

#### Scenario: Plain card remains unchanged
- **WHEN** a selection context has no annotations
- **THEN** it retains the existing plain selection-card presentation

### Requirement: Referenced source text is decorated
The active file preview SHALL decorate every current-session selection context that can be resolved to its source passage. A plain reference SHALL gain a theme-colored border when the marked text is hovered. An annotated reference SHALL gain the same mark plus a numbered bubble.

#### Scenario: Plain reference has no bubble
- **WHEN** a resolvable plain selection reference belongs to the active preview file
- **THEN** its exact text gains the hover border and no annotation bubble is shown

#### Scenario: Annotated reference has a bubble
- **WHEN** a resolvable annotated selection belongs to the active preview file
- **THEN** its exact text gains a clearly visible theme-compatible fill and outline plus a numbered annotation bubble immediately to the left of that passage

#### Scenario: Hover bubble shows annotation content
- **WHEN** the user hovers or focuses an annotation bubble
- **THEN** the UI shows all annotations grouped at that passage in creation order

#### Scenario: Multiple passages are numbered per file
- **WHEN** the active file has annotations on multiple passages
- **THEN** its bubbles are numbered from one in first-appearance order for that file

#### Scenario: Source selection records its actual lines
- **WHEN** the user selects one or more rows in a source-mode text, log, code, or Markdown preview
- **THEN** the composer card displays the actual first and last source line numbers from those rows

#### Scenario: Source selection preserves blank lines
- **WHEN** a source-mode selection spans one or more empty rows rendered with visual placeholders
- **THEN** the stored selected text and offsets use the exact source slice without placeholder characters and the new decoration resolves immediately

#### Scenario: Decoration contrast follows light and dark themes
- **WHEN** an annotated passage is displayed in either light or dark mode
- **THEN** its persistent fill and outline remain visually distinct without obscuring the source text

### Requirement: Selection annotations persist within the session
Newly sent selection content parts SHALL preserve a sanitized selected-text anchor, source range and offsets, and normalized annotations. The Web UI SHALL derive source decorations from the active session transcript plus pending composer contexts and SHALL NOT share them with other sessions.

#### Scenario: Reopen annotated session
- **WHEN** a user sends an annotated reference, closes the file or session, and later reopens that session and file
- **THEN** the sent selection card, source mark, bubble number, and annotation content are restored

#### Scenario: Switch sessions
- **WHEN** the user switches to a different session that did not send the annotated reference
- **THEN** the other session does not display that source decoration or annotation bubble

#### Scenario: Remove pending context
- **WHEN** the user removes an unsent selection context from the composer
- **THEN** its pending source decoration disappears while sent annotations remain

#### Scenario: Read legacy context
- **WHEN** a historical selection content part has no selected-text anchor or annotations
- **THEN** its existing chat card still renders without failing

### Requirement: File changes silently hide old annotations
New annotated contexts SHALL record a stable revision of the complete source document. The preview SHALL compare that stored revision with the freshly loaded document content before applying source decorations. It MUST silently omit revision-mismatched annotations without deleting their session history, showing stale indicators, or asking the user for confirmation. Matching revisions SHALL continue through the existing exact-anchor resolution path.

#### Scenario: Unchanged document keeps annotations
- **WHEN** the freshly loaded document revision equals the revision stored with an annotation
- **THEN** the existing source mark, numbered bubble, and annotation content are displayed normally

#### Scenario: Change elsewhere hides an old annotation
- **WHEN** any document content changes while the annotated passage itself still exists exactly
- **THEN** the old annotation produces no source mark, bubble, stale notice, prompt, or confirmation when the user switches back to that document

#### Scenario: Change inside the passage hides an old annotation
- **WHEN** the annotated passage or any other document content changes
- **THEN** all annotations recorded against the previous document revision are silently absent from the preview

### Requirement: Model context includes annotation intent
The daemon SHALL include each normalized annotation immediately with its selected text in the hidden augmented request context while preserving the user's visible prompt. It SHALL sanitize annotation metadata and enforce server-side size limits.

#### Scenario: Send annotated context
- **WHEN** a request contains selected text and annotations
- **THEN** the provider prompt contains the selected text, annotation content, and source location while the visible user message remains the original prompt

#### Scenario: Ignore arbitrary annotation fields
- **WHEN** a client includes unsupported fields in an annotation object
- **THEN** the persisted content part retains only the normalized annotation id, text, and creation time

#### Scenario: Oversized annotation is bounded
- **WHEN** annotation text exceeds the server limit
- **THEN** prompt and persisted metadata use the bounded annotation text
