## 1. Stable window identity

- [x] 1.1 Add a namespaced transcript-window key that prefers persisted `messageId` and falls back to the current item id.
- [x] 1.2 Add a pure reconciliation helper that preserves valid keys and explicit full view, resets empty transcripts, and reselects a missing large-transcript boundary.
- [x] 1.3 Use the same key semantics for initial selection, window lookup, and progressive reveal.

## 2. ChatView integration

- [x] 2.1 Migrate the per-session transcript window state from temporary `anchorId` semantics to stable `anchorKey` semantics.
- [x] 2.2 Reconcile the key during render and slice with the resolved value in the same render, without adding an effect-time full-transcript frame.
- [x] 2.3 Preserve explicit "显示全部", progressive reveal compensation, search, turn navigation, and existing tail-follow/review behavior without new `scrollTop` writes.

## 3. Regression coverage

- [x] 3.1 Extend focused helper tests for namespaced identity, temporary-id fallback, missing-key recovery, short transcripts, and explicit full view.
- [x] 3.2 Add a regression that loads a large persisted transcript, applies reducer `transcript_replace` with new temporary ids, reprojects it, and verifies the stable bounded window survives.

## 4. Documentation and verification

- [x] 4.1 Confirm the implementation matches `docs/web-chat/transcript-window.md` and the `web-chat-transcript-window` specification.
- [x] 4.2 Run the complete Web test suite and production build from `web/`.
- [x] 4.3 Run strict OpenSpec validation and `git diff --check`.
