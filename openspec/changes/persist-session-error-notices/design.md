## Context

See proposal.md for the incident. Agent error messages use `TranscriptWriter::dispatch_message`, which emits callbacks and events without storing a ChatMessage. The Web reducer deliberately treats old error IDs as content-derived event identities. Durable transcript records already support `metadata.transcript_only`, and provider history filtering excludes these records and non-provider roles.

## Goals / Non-Goals

**Goals:** Persist every emitted red error message before publishing it; preserve diagnostics and independent occurrences; restore through ordinary transcript reads; keep current, resumed, and compacted model requests free of these records.

**Non-Goals:** Changing quota detection, retry policy, error-card design, or reconstructing errors never written by older versions. Transport connection errors and tool-result errors retain their existing semantics.

## Decisions

1. Inject the loop's nullable borrowed SessionManager into TranscriptWriter at construction. At the shared error dispatch boundary create a UUID, timestamp, and transcript-only metadata, append to history and session storage, then emit the existing callback/event. This covers all red error messages rather than only HTTP 451. A browser cache was rejected because it misses background failures and does not survive a daemon restart.
2. Use UUID identity for error records when available; retain existing content-derived IDs for legacy records and all other roles. A timestamp alone is insufficient for two equal failures within the same clock interval.
3. Distinguish durable errors by `role=error`, `metadata.transcript_only=true`, and a nonempty ID. Reconcile these by ID in both replay paths; bypass content-only deduplication for different error IDs. Unmarked legacy errors retain occurrence/sequence semantics.
4. Reuse the existing provider-history filter, and test both the actual next provider request and resumed history. No UI style changes or additional context protocol is required.

## Risks / Trade-offs

- [Duplicate live/history cards] -> Assert the persisted UUID equals the event/API ID and cover overlapping REST plus WebSocket replay.
- [Loss of equal errors] -> Separate UUIDs for each dispatch; test identical content in separate turns.
- [Accidental prompt contamination] -> Assert transcript-only markers and provider request exclusion before and after resume/compaction.
- [Existing dirty files] -> Restrict edits to this feature and preserve pre-existing daemon documentation changes.

## Migration Plan

Deploy backend and Web bundle together. Records are additive JSONL messages understood by existing storage; no migration is required. Old clients still display errors, but may duplicate overlapping replay until upgraded. Rollback leaves durable records safely excluded from model context.
