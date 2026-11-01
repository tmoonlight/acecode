# Proposal

## Why

Users returning to a completed conversation or watching a turn finish should immediately get one useful next-message suggestion in the composer. Accepting it must remain an explicit editing action that preserves drafts and does not start work.

## What Changes

- Generate one contextual user prompt on first viewing each completed turn and when the visible session finishes a turn.
- Share a session/turn identity across both triggers; returning to the same completion reuses its result.
- Show the prompt as muted composer ghost text only while the draft is empty. Tab accepts it without sending; editing, attachments, menus and IME keep their existing behavior.
- Use a separate bounded, tool-free model request with stale-response protection and quiet failure handling.

## Capabilities

### New Capabilities
- `composer-follow-up-prompts`: Completed-turn recommendation generation, deduplication, ghost text and explicit acceptance.

### Modified Capabilities
None.

## Impact

Session host generation helpers, an authenticated daemon endpoint, Web/Desktop ChatView and InputBar, native and browser tests, and daemon API documentation. No new dependency or settings surface is required.
