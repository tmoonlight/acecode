## Why

Provider failures such as HTTP 451 quota exhaustion currently appear only as transient error events. Switching sessions or reloading history loses the red error card, making the failed turn look silently interrupted.

## What Changes

- Persist each emitted error message with its diagnostic metadata and a unique occurrence ID in the session transcript.
- Keep these records out of model context, including resumed sessions and compaction.
- Restore the existing error cards from history and reconcile live/replayed events by persisted identity without merging separate identical failures.

## Capabilities

### New Capabilities

### Modified Capabilities

- `session-storage`: Durable transcript-only error notices, with consistent history and live-event identity.

## Impact

Agent transcript writer and composition, Web message identity/replay, session-history integration tests, and daemon API documentation. Existing red error presentation is reused. Previously lost errors cannot be reconstructed without an existing record.
