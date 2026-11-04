## Why

The highlighted reply in the conversation's final next-step card currently requires a click. Users need Ctrl+Enter confirmation and a visible hint inside that button.

## What Changes

- Ctrl+Enter activates the first highlighted action of the active next-step card through its existing click handler.
- Display compact Ctrl and Enter keycaps inside that button.
- Keep drafts intact and suppress activation during IME composition, key repeat, sending, or competing dialogs/editors.

## Capabilities

### New Capabilities
- `conversation-next-step`: Keyboard confirmation and discoverability of the final recommended reply.

### Modified Capabilities

None.

## Impact

Web next-step card, shared shortcut context checks, and browser regression coverage. No daemon or persisted data changes.
