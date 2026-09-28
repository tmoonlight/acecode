<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md for motivation. RichComposer already uses Slate inline void nodes. External state is currently a string plus an attachment array; attachment synchronization prepends every attachment, and clipboard/drafts discard attachment positions. The daemon already loads explicit skill mentions and persists message metadata.

## Goals / Non-Goals

Goals: one ordered content contract for editing and restoration, unchanged compact appearance, existing attachment/skill services, and backwards-compatible text and message reads.

Non-goals: replacing Slate, arbitrary rich formatting, drag-to-reorder tags, changing TUI controls, or treating skill tag order as an execution workflow.

## Decisions

1. Add optional `composer_content: {version: 1, parts: [...]}` to messages and drafts. Parts are `text` (`text`), `path` (`path`, `token`, optional `directory`), `skill` (`name`, `token`, optional `path`), and `attachment` (stable `key`, uploaded `id`, display `name`, `kind`, optional MIME/path metadata). Tokens preserve canonical text without embedding Slate's document schema. Attachment parts contribute no model text; the existing attachment IDs remain the resource authority. Text compatibility is derived from the ordered parts.
2. Keep the Slate document during local edits. Serialize ordered parts alongside text. Insert new attachment placeholders at the saved caret before asynchronous upload; reconcile metadata by stable identity in place. Selection, undo, cut, and paste operate on the ordered document. Upload records remain reusable when undo restores a removed reference.
3. Offer skills at the current caret and serialize them as canonical explicit skill mentions. The backend command catalog supplies skill paths/mentions. Builtins and reusable slash commands retain their existing leading-command behavior.
4. Persist ordered content through home/session drafts, queue editing/retries, history navigation, and fork restoration. Legacy strings remain supported. New sent messages render parts in order; old messages use their existing renderer. Uploaded attachment references are hydrated from existing attachment records. Forks copy uploaded resources into the destination and remap both the restored prompt and retained structured history. Session draft writes, including detached upload completion, are serialized; loading waits for pending writes. Send receipts compare scope and semantic edits before clearing the composer.
5. The daemon validates the schema and limits, verifies submitted attachment references against materialized resources, and stores sanitized content in message metadata. Draft storage accepts the optional content alongside its existing text. No provider must understand editor-specific nodes.
6. Clipboard uses a versioned application format for lossless internal copy/paste and meaningful plain-text fallback for external applications. Missing resources must not silently produce a sendable broken attachment.
7. Focused Slate selection marks each inline reference for whole-tag highlighting. Composer text and tags share the native `Highlight`/`HighlightText` colors through local CSS variables; selected fill overrides tag hover, upload opacity and file-type text colors without changing tag geometry or editing semantics.

## Risks / Trade-offs

- IME and delayed controlled echoes can overwrite input -> retain composition/identity protections, compare ordered content and avoid needless editor replacement.
- Attachment upload completion can move the caret or resurrect deletion -> stable keys, reconcile only present references, and ignore stale callbacks after session changes.
- Text-only lifecycle paths can lose positions -> explicit tests for draft, queue, history, fork, live events, and reload.
- Legacy clients lack structured content -> preserve the text/attachment API and accept legacy drafts/messages unchanged.
- Untrusted persisted metadata -> validate types, sizes and attachment identities on the server; render React text, never supplied HTML.

## Migration Plan

Add the optional schema and helpers, wire the existing editing and persistence paths, update API documentation, and run focused/full frontend and backend validation plus browser interaction checks. Old persisted messages require no rewrite; rollback clients continue to show legacy text and attachments.
