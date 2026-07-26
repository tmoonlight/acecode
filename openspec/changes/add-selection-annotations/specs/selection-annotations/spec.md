## ADDED Requirements

### Requirement: Preview selections expose direct chat actions
The Web UI SHALL show a floating action surface beside a non-empty selection inside code, text, or Markdown file previews. The surface SHALL contain `引用到聊天` followed by `批注`, SHALL remain inside the visible viewport, and SHALL coexist with the existing right-click `引用到聊天` action.

#### Scenario: Selecting preview text opens actions
- **WHEN** the user completes a non-empty selection in a supported file preview
- **THEN** the UI shows `引用到聊天` and `批注` beside the selection

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
Composer and sent-message annotated references SHALL reuse the existing selection-card surface. An annotated card SHALL expose a compact annotation count and SHALL reveal annotation content on hover or keyboard focus without replacing the file label or line range.

#### Scenario: Composer card shows annotation affordance
- **WHEN** a selection context has one or more annotations
- **THEN** its existing composer card shows the annotation count and exposes the annotation text

#### Scenario: Sent card preserves annotation affordance
- **WHEN** an annotated context appears in a sent user message
- **THEN** the sent-message selection card uses the same annotation count and hover or focus content

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
- **THEN** its exact text gains the hover border and a numbered annotation bubble is aligned to that passage

#### Scenario: Hover bubble shows annotation content
- **WHEN** the user hovers or focuses an annotation bubble
- **THEN** the UI shows all annotations grouped at that passage in creation order

#### Scenario: Multiple passages are numbered per file
- **WHEN** the active file has annotations on multiple passages
- **THEN** its bubbles are numbered from one in first-appearance order for that file

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

### Requirement: File changes use conservative re-anchoring
The preview SHALL first validate stored offsets against the selected text, then fall back to the nearest exact selected-text occurrence using the stored offset or line range. It MUST NOT attach an annotation to non-matching text.

#### Scenario: Stored offset still matches
- **WHEN** the file content at the stored offsets still equals the selected text
- **THEN** the decoration uses that exact range

#### Scenario: Text moved after edits
- **WHEN** the selected text still exists exactly but moved away from the stored range
- **THEN** the decoration follows the nearest exact occurrence

#### Scenario: Original text no longer exists
- **WHEN** no exact occurrence of the selected text exists in the current preview
- **THEN** the annotation is marked `原文已变化` at the preview edge and no unrelated text is decorated

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
