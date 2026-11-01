# Design

## Context

ChatView consumes a unified transcript store for loaded history and live events. Persisted turn-timing records identify a completion by user-message UUID and completion timestamp and include completed/aborted/error status. InputBar owns the draft and keyboard shortcuts; RichComposer already renders a muted placeholder and guards IME. Existing side questions allow tools, so they are unsuitable for automatic recommendations.

## Goals / Non-Goals

Deliver the two agreed triggers through one state gate, with no synthetic main-session turn, draft overwrite, additional panel or settings control. Do not redesign the composer or alter existing task suggestion cards.

## Decisions

- Add a host helper that reads a bounded recent transcript tail, validates the latest completion and produces a small JSON context. Use persisted completion identity in both frontend and backend; avoid transient busy=false during navigation and renderer-generated item IDs.
- Add authenticated POST `/api/sessions/:id/follow-up-prompt` with the expected completion key. Verify the session is idle and the completion remains current before and after generation. The result contains the same key and one prompt (possibly empty).
- Reuse the summary-model fallback mechanism with the session model as contextual fallback. Construct an independent provider, use the tool-free compaction adapter and a bounded cancellation scope whose abandoned work owns all captured state. Source messages are data, not instructions. No tool definitions, mutation or transcript append occurs.
- Keep one shared frontend cache for the application lifetime, keyed by backend/workspace/session and completion. Register an attempt before starting the promise, cache empty/failure results, and retain consumed state across remounts. A state effect covers both loaded-history and live-completion transitions, including tool-only endings that the notification callback does not emit.
- Before generation, wait for successful restoration of the current session's draft. Unsent text (including whitespace), structured content, attachments or context consume that completion without issuing a request; clearing the draft is not a new trigger. Draft load failure leaves generation blocked without changing the existing draft recovery/autosave behavior. Historical messages remain recommendation context and do not count as drafts.
- Only expose the ghost when the draft is fully empty. Editing while a request is pending suppresses that completion, including cancelling a queued request before its microtask starts. Tab writes through the existing controlled composer update path and moves the caret; Escape consumes the suggestion. No loading label, new button, automatic focus or auto-send.
- Preserve existing font, spacing and theme tokens. Use the existing placeholder rendering with a stable accessible input name and a screen-reader-only acceptance description.

## Risks / Trade-offs

- Very long or malformed transcript tails may have insufficient bounded context: omit the optional suggestion, never scan without a limit.
- Provider latency: trigger immediately but display only on result; failure is quiet and does not retry on render.
- New turns during generation: validate identity again server-side and gate response display client-side.
- Cache is application-lifetime only: restarting the UI can generate again; no prompt text is persisted in browser storage.

## Migration Plan

Additive endpoint and client feature. Older backends fail silently for this optional request. Reverting the feature requires no data migration.

## Placeholder Implementation Note

Suggested placeholders use `ComposerPlaceholder`, which forwards Slate's placeholder ref and preserves its attributes. They retain the existing `text-fg-mute` theme token with effective opacity `1`. A `ResizeObserver` measures wrapped placeholder height and reserves that space within the editor's existing minimum and maximum heights. Ordinary placeholders retain their existing opacity and sizing behavior.

`InputBar` includes the full suggested prompt and the Tab/Escape instructions in `aria-description`, while keeping the input's accessible name stable. The accepted trigger, draft suppression and keyboard behavior above remain unchanged.

The independent finish review resolved the contrast and complete mobile-preview findings in `build/follow-up-prompt-ui/desktop.png` and `mobile.png`, with measured contrast of 6.29:1 in light mode and 5.18:1 in dark mode. This evidence covers the reviewed composer captures, not full-application approval.
