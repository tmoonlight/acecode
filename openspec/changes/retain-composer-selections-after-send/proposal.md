## Why

Sending a chat message currently clears the selected swarm mode even though the composer still represents the same conversation. Composer capability selections should remain stable so users do not have to reselect swarm mode or an expert before every message.

## What Changes

- Keep the composer swarm-mode selection enabled after successful, queued, and first-message sends.
- Preserve the selected expert through the existing session binding and restoration path after a send.
- Continue clearing one-shot attachments, pinned contexts, and transient selections after submission.
- Clear swarm mode or the selected expert only when the user explicitly disables or removes it, or when navigation restores a different conversation's state.
- Add focused regression coverage for the persistent selection lifecycle.

## Capabilities

### New Capabilities

- `composer-selection-lifecycle`: Defines how swarm mode and expert selections survive chat submission and how users explicitly change or cancel them.

### Modified Capabilities

None.

## Impact

- Desktop WebUI composer state and send handling in `web/src/components/ChatView.jsx`.
- Existing composer architecture tests under `web/src/lib/`.
- No daemon API, session schema, or runtime prompt changes are required; expert persistence continues to use the existing session `expert_id` binding and swarm mode continues to be copied into each submitted message payload while selected.
