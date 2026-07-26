## Context

ACECode already has a durable attachment store and a neutral `ChatMessage.content_parts` field. The current path is user-input focused: desktop uploads images, message submission validates attachment ids, session JSONL persists `content_parts`, and OpenAI-compatible providers turn user image parts into `image_url` payloads. Tool results and assistant outputs still flow mainly as `content` strings plus optional tool metadata (`summary`, `hunks`), so a tool that creates or returns an image has no structured way to show that image in the desktop transcript or preserve it after resume.

The desktop renderer also has a user-only attachment strip. Assistant messages and tool blocks render markdown/text/diffs, which means output images can only appear if they are embedded as markdown links or paths. That is fragile, bloats transcripts when data URLs are used, and does not match the existing attachment-store boundary.

## Goals / Non-Goals

**Goals:**

- Add a structured output attachment model for assistant and tool results.
- Store image bytes through the existing session attachment store and persist only metadata references in JSONL.
- Render assistant and tool output image attachments in the desktop/web chat timeline with thumbnails and click-to-preview.
- Preserve existing provider-visible tool result text and large-text replacement behavior.
- Keep TUI output textual and width-stable.

**Non-Goals:**

- Terminal inline image rendering.
- Automatic provider re-ingestion of every tool output image as a model-visible image part.
- A new binary storage system separate from the existing session attachment store.
- Broad support for arbitrary generated binary formats beyond carrying metadata and rendering image MIME types.

## Decisions

### Use the existing attachment store for output artifacts

Output images will be saved as session attachments and referenced by the same metadata shape used for user attachments (`id`, `name`, `mime_type`, `size_bytes`, `path`, `blob_url`, `kind`). This keeps JSONL compact and lets the current blob endpoint serve thumbnails.

Alternative considered: store `data:image/...` directly in `content_parts`. This was rejected because it inflates transcript files, makes resume slower, and bypasses the existing authenticated blob boundary.

### Extend `ToolResult` with structured attachments

`ToolResult` will gain an `attachments` JSON array or typed vector carrying attachment records. Tools can return output text plus attachments without inventing ad hoc JSON inside `output`. `ToolExecutor::format_tool_result` will transfer those attachments onto the tool `ChatMessage` as `content_parts` and/or metadata.

Alternative considered: parse image paths from tool output. This was rejected because output text is user-visible prose and cannot distinguish incidental paths from intended display artifacts.

### Materialize image data URLs and local image paths before persistence

When a tool emits a structured output attachment with a data URL, the daemon decodes and stores it. When it emits a local path, the daemon reads and stores the bytes only if the file is an image type ACECode can safely classify. Stored attachment metadata is what reaches session JSONL and the UI.

Alternative considered: let frontend fetch `file://` or local paths. This was rejected because the browser sandbox cannot safely read arbitrary local files and resume would break outside the original path.

### Surface existing local images through a focused built-in tool

The shared built-in tool set exposes a read-only `show_image` tool. It resolves an absolute or workspace-relative path, validates that it is a supported regular image file, and returns a local-path output-attachment descriptor. The existing agent-loop materializer remains responsible for path-policy enforcement, byte validation, durable attachment storage, Web/Desktop rendering, and the TUI text fallback.

Alternative considered: restore the legacy tool's direct TUI file-opening state and platform launcher. This was rejected because `Ctrl+O` now controls transcript expansion and output attachments already have a single cross-surface rendering and persistence path.

### Put display attachments on message parts, not provider semantics

The UI consumes assistant/tool `content_parts` for display. Provider request builders continue to filter unsupported roles and maintain existing tool result text semantics. A future provider feature can opt into reusing these parts, but this change does not silently send tool images back to the model.

Alternative considered: append synthetic user image messages after tool results. This matches some systems but changes model history semantics; ACECode should first make display durable without altering the agent loop contract.

### Share one attachment renderer across roles

The existing user attachment strip will be generalized into a role-agnostic component used by user messages, assistant messages, and `ToolBlock`. It will render image MIME types as thumbnails and non-image files as compact chips. Clicking an image opens an in-app preview overlay.

Alternative considered: separate assistant/tool implementations. This was rejected because the record shape and preview behavior are intentionally shared.

## Risks / Trade-offs

- [Risk] Tools may produce very large images or unsupported data URLs. -> Reuse attachment size/MIME validation, reject invalid attachments with a clear tool-visible error, and never persist raw data URLs in JSONL.
- [Risk] Output attachments could be lost when large tool text is replaced by persisted previews. -> Keep attachment metadata separate from replacement text and include it in tool event payloads and session JSONL.
- [Risk] Existing frontend projection may hide standalone tool result rows. -> Ensure attachment-bearing tool results remain visible either inside their `ToolBlock` or as a fallback result row.
- [Risk] Older sessions lack output attachment fields. -> Treat absent `content_parts`/attachments as empty arrays and preserve current rendering.

## Migration Plan

The change is additive. Existing sessions continue to load with empty output attachment arrays. New sessions persist structured output attachments in JSONL; rollback leaves those fields ignored by older clients while preserving text output.
