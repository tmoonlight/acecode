## Why

ACECode can already pin selected preview text into the chat composer, but the action is hidden behind a context menu and the source document loses all durable visual connection to the resulting chat context. Users need a direct selection toolbar and lightweight annotations so they can explain why a passage matters while keeping that explanation attached to both the source text and the chat request.

## What Changes

- Show a floating `引用到聊天` / `批注` toolbar beside a live selection in code, text, and Markdown previews while preserving the existing context-menu action.
- Let `批注` open a focused multiline editor and pin the selected text together with a required annotation into the existing composer-context flow.
- Render theme-colored source decorations for referenced selections and numbered annotation bubbles whose hover card exposes all annotations anchored at that location.
- Preserve sent annotations in session message metadata so reopening a session can restore source decorations and bubbles without sharing them across unrelated sessions.
- Record the source document content revision with each new annotation and silently hide that annotation in previews after any document content change.
- Reuse the existing composer and sent-message selection cards, adding a compact annotation indicator and hover content instead of introducing a second attachment surface.
- Send both the selected text and annotation content to the model as hidden request context while keeping the user's visible prompt unchanged.

## Capabilities

### New Capabilities

- `selection-annotations`: Selection toolbar, annotation editor, source decorations, session-scoped persistence, document-revision gating, and annotated chat-context presentation.

### Modified Capabilities

None. The repository currently has no canonical `openspec/specs` capability for the existing selection-chat-context implementation.

## Impact

- Web selection and composer state in `web/src/components/ChatView.jsx`, `InputBar.jsx`, and `web/src/lib/selectionChatContext.js`.
- File-preview rendering and source decoration in `web/src/components/FilePreviewContent.jsx` plus focused styling in `web/src/styles/globals.css`.
- Sent-message context presentation in `web/src/components/AttachmentStrip.jsx` and content-part normalization.
- Web request expansion and sanitized session metadata in `src/web/server_helpers.cpp` and `src/web/routes/routes_sessions.cpp`.
- JavaScript and C++ regression coverage for selection anchoring, annotation normalization/persistence, prompt expansion, and UI architecture.
