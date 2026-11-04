## Context

ChatView derives the active card with activeNextStep and sends replies through replyToNextStep, preserving composer content. NextStepCard marks action zero as primary. Ctrl+Enter already has contextual editor behavior, so the active card must consume its confirmation before the composer handles it.

## Decisions

- Own the capture listener in the mounted NextStepCard and dispatch a click on its primary button. This shares disabled state and reply behavior with pointer activation and removes the shortcut when the card unmounts.
- Match exactly Ctrl+Enter using existing shortcut binding utilities. Respect composition, consumed events, repeats, blocking overlays, and focus in side chats, terminals or other editors. Track composition through its settling event loop turn.
- Use two compact outlined keycaps in the existing primary button, with a return-arrow SVG and aria-keyshortcuts="Control+Enter". Keep label truncation separate so the hint remains visible on narrow screens.
- Keep this contextual confirmation separate from the configurable app shortcut catalog, like the existing question picker confirmation. Do not change persisted shortcut preferences.

## Risks

Capture handling could intercept an unrelated editor or submit twice. Browser coverage must exercise the real ChatView/composer, overlays, IME, repeat events, disabled sending, and draft preservation. Historical, dismissed, busy and read-only cards continue to be governed by activeNextStep.
