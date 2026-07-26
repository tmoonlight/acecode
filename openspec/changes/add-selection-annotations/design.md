## Context

The existing selection-context path is split across the Web UI and daemon:

- `selectionContextFromWindowSelection()` captures selected preview text plus a source path and approximate line range.
- `ChatView` owns the transient selection preview and pinned `composerContexts`.
- `InputBar` and `AttachmentStrip` render the same compact selection-context presentation before and after send.
- The session route expands selected text into a hidden model prompt, but its sanitized `selection_context` content part intentionally omits the selected text.
- `FilePreviewContent` renders code/text and Markdown through `dangerouslySetInnerHTML`; it currently has no persistent source-decoration model.

Annotations need to survive selection loss, message send, file switching, and session reload. They also need enough anchor information to avoid attaching a comment to the wrong passage after a file edit. The implementation must remain backward-compatible with existing selection contexts and must not broaden selection support to PDF or Office previews.

## Goals / Non-Goals

**Goals:**

- Expose selection actions directly beside a live preview selection.
- Carry one or more user annotations with a selection through composer, prompt expansion, session persistence, and sent-message rendering.
- Restore referenced-text marks and numbered annotation bubbles from the current session.
- Re-anchor against changed content conservatively and represent unresolved annotations as stale.
- Keep the existing selection card and context-menu behavior recognizable.

**Non-Goals:**

- Editing or deleting annotations already recorded in historical messages.
- Sharing annotations across sessions or storing them in a repository-side database.
- Supporting PDF, Word, spreadsheet, image, chat transcript, diff, or editor selections.
- Building a general review-thread, reply, resolve, or assignee system.

## Decisions

### 1. Extend the selection context instead of introducing another attachment type

An annotated reference remains `type: "selection"` and gains:

```json
{
  "text": "unsent selected text",
  "selected_text": "persisted anchor text",
  "source": {
    "path": "C:/repo/src/a.cpp",
    "kind": "text",
    "start_line": 12,
    "end_line": 14,
    "start_offset": 318,
    "end_offset": 401
  },
  "annotations": [
    {
      "id": "annotation-...",
      "text": "Explain why this branch is needed",
      "created_at": "2026-07-26T..."
    }
  ]
}
```

`note` remains reserved for the existing line-count presentation. Annotation text is normalized into the explicit `annotations` array and capped independently from selected text.

This keeps request routing, composer extras, queued inputs, and sent content parts on the established selection-context path. A distinct attachment type would duplicate all of those seams and make annotated and plain references render differently.

### 2. Capture a stable popover snapshot before focus leaves the selection

`ChatView` will keep a selection-action snapshot containing the normalized context and a viewport rectangle. A portal-based `SelectionActionPopover` renders `引用到聊天` and `批注` beside that rectangle, clamped to the viewport.

Choosing `批注` switches the same popover into an input mode. The captured context stays alive after the textarea takes focus. `Enter` submits, `Shift+Enter` inserts a newline, and `Escape` cancels. Choosing either completed action clears the native/inactive selection through an exported preview-selection reset helper.

Keeping the snapshot in React state is more reliable than asking `window.getSelection()` again after a button or textarea has taken focus.

Selection changes fired while the pointer is still dragging update only the transient
selection candidate. The action surface is created after `mouseup` and is anchored to
that event's viewport cursor position, matching the contextual toolbar behavior of
Office-style editors and avoiding the oversized union rectangle produced by multiline
table selections. Keyboard-created selections fall back to the selected range.
The global release/key listeners ignore events outside the supported preview, so
clicking the popover or typing in its annotation editor cannot re-anchor or replace it.

### 3. Merge annotations into an already-pinned location

Plain reference pinning preserves the existing location-based deduplication. Annotating a location that is already present in the current composer appends a distinct normalized annotation to that context instead of adding a duplicate card.

Historical sent messages remain immutable. A later annotation of the same passage creates a new context in the new user message; source rendering groups all current-session contexts at the same anchor into one bubble containing all annotations.

### 4. Persist a sanitized anchor and annotations in the selection content part

The daemon will continue to read the unsent `text` field when constructing the hidden prompt. Sanitized session metadata will additionally retain capped `selected_text`, source offsets, and normalized annotations. Arbitrary client fields are not copied.

Prompt expansion will place each annotation immediately after its selected text so the model receives both as one context unit. The visible `display_text` behavior is unchanged.

Older session records without `selected_text` or `annotations` continue to render their existing context card. They simply cannot restore a precise source mark.

### 5. Derive session-scoped decorations from transcript plus pending composer contexts

`ChatView` will collect selection contexts from raw transcript content parts in message order and append currently pinned composer contexts. It passes that list through `PreviewDetailsPanel` to the active `FilePreviewContent`.

Because the list is derived from the active session transcript, changing sessions automatically removes unrelated annotations. Removing an unsent context removes only its pending decoration; sent content remains immutable.

Within a file, contexts are grouped by anchored passage. Groups with annotations receive stable 1-based numbers in first-appearance order. Plain references create marks but no bubble.

### 6. Resolve anchors conservatively and apply DOM marks after preview render

A focused `selectionSourceDecorations` helper will provide pure anchor resolution plus DOM application:

1. Prefer the stored offsets when the text at that range still matches.
2. Otherwise find exact selected-text occurrences and choose the one nearest the stored offset or line.
3. If no exact occurrence exists, return a stale result instead of guessing.

For code/text/Markdown source mode, raw-source offsets map to text nodes inside each `.ace-line-code` cell. For rendered Markdown, a selectable text-node index is built from the rendered DOM and matched against the selected visible text.

The helper wraps only matched text-node fragments with semantic `<mark>` elements. A React overlay measures the first mark in each annotated group and keeps its numbered bubble aligned during scroll and resize. Unresolved groups appear in a compact stale stack at the preview edge with `原文已变化`; they never point at arbitrary text.

DOM marks are cleared before each application and during unmount so they do not leak across `dangerouslySetInnerHTML` refreshes. Annotation bubbles live in a separate overlay and therefore do not pollute copy or selection text.

Source-mode rows expose their actual 1-based line number as DOM metadata. Selection
capture reads that metadata instead of counting text across the rendered table, whose
line-number cells and row boundaries do not map reliably to `Range.toString()`.
It also clamps each range endpoint to the row's real source length and reconstructs the
selected source slice from row metadata. This removes visual placeholder spaces from
empty rows, preserving exact Markdown anchors and preventing immediate false stale
results for selections that contain blank lines.

Resolved annotation bubbles are positioned immediately to the left of the first marked
fragment. Persistent annotated marks use a visible themed fill and outline in both light
and dark modes; plain references remain quieter and gain the stronger outline on hover.

### 7. Reuse the existing card with an annotation affordance

Composer and sent-message cards keep their current dimensions, file icon, label, border, and remove/pin behavior. Annotated cards add a compact count indicator. Hovering or focusing that indicator reveals the annotation text; the base card remains visually consistent with a plain reference.

## Risks / Trade-offs

- **[Risk] DOM mark mutation can conflict with transient inactive-selection marks.** → Clear only the dedicated persistent mark class before reapplying, export an explicit inactive-selection reset for completed actions, and cover both classes in architecture tests.
- **[Risk] Repeated source text can produce an ambiguous fallback match.** → Prefer stored offsets/line proximity and never use a fuzzy match; unresolved content is stale.
- **[Risk] Large selections and comments could inflate session records.** → Keep the existing selected-text cap, add a separate annotation cap, deduplicate annotations, and store only sanitized fields.
- **[Risk] Rendered Markdown text does not map one-to-one to source Markdown.** → Treat rendered mode as a rendered-text anchor and resolve it against the rendered DOM; source mode continues to use raw-source offsets.
- **[Risk] Overlay positions can drift after wrapping, zoom, or resize.** → Re-measure on scroll, resize, `ResizeObserver`, and decoration changes.
- **[Trade-off] Historical annotations are immutable.** → This matches message history semantics and avoids a second persistence API; a later message can add another annotation to the same passage.

## Migration Plan

1. Deploy the backward-compatible Web and daemon schema extension together.
2. Existing content parts continue to display through the old fields.
3. Newly sent contexts record anchor text, offsets, and annotations.
4. Rollback is safe: older clients ignore the additional JSON fields and still show the selection card.

## Open Questions

None. The user approved the documented defaults before implementation.
