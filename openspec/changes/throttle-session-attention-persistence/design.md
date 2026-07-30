## Context

`EventDispatcher::emit` drains subscribers synchronously on the thread that
emits an AgentLoop event. `note_session_event_for_attention` is one of those
subscribers, and token, reasoning, and tool events all advance attention
cursor/timestamp fields. The current implementation serializes the complete
workspace attention map, writes a temporary file, and renames it while holding
`attention_mu` for every such event.

Real feedback logs show peaks near 500 events per second with concurrent
sessions sharing the same workspace state file. The persistence format is
small, but repeated filesystem replacement and mutex hold time block the
AgentLoop hot path.

## Goals / Non-Goals

**Goals:**

- Bound cursor-only attention writes to at most one coalesced flush per
  interval during steady streaming.
- Keep read/unread/in-progress and busy-state transitions immediately durable.
- Retry transient write failures instead of silently declaring the workspace
  clean.
- Drain pending attention state safely during normal shutdown.

**Non-Goals:**

- Change attention state derivation, cursor semantics, WebSocket payloads, or
  the on-disk schema.
- Guarantee the last cursor-only update across process crashes or forced
  termination.
- Move filesystem I/O outside `attention_mu` in this change; consistency stays
  serialized by the existing lock.

## Decisions

1. **Use one flusher thread and a dirty workspace set.**

   Cursor-changing events update the in-memory record and insert its workspace
   hash into a set under `attention_mu`. A single condition-variable thread
   wakes every second and saves a snapshot of dirty workspace hashes. A thread
   per workspace or per event was rejected because it would add scheduling and
   lifetime complexity without improving the serialized file writes.

2. **Flush synchronously at state boundaries.**

   If attention state or the busy bit changes, the event path flushes current
   dirty work immediately before broadcasting status. These transitions occur
   only a few times per turn and are the durability points users observe.
   Cursor/timestamp-only progress can tolerate the one-interval crash window.

3. **Clear dirty state only after a successful atomic replacement.**

   `save_attention_workspace_locked` marks direct callers dirty first and
   removes the hash only after directory creation, temporary-file write, and
   rename all succeed. Failures remain in the set for the next periodic retry.
   A workspace with no known cwd is removed because no persistence path can
   ever be derived.

4. **Stop producers before the final flush.**

   Destruction first stops the listener and disables/unsubscribes tracked
   attention event producers. It then signals the flusher to perform one final
   dirty flush and joins the thread. Stopping the flusher first was rejected
   because a still-live callback could mark state dirty after the final write.

5. **Write compact JSON with the existing schema.**

   Removing pretty-print indentation reduces bytes and filesystem work without
   a migration. Existing readers continue to parse version 1 and the same
   `sessions` records.

## Risks / Trade-offs

- **Forced termination can lose cursor-only progress from the last interval.**
  → State-boundary changes still flush immediately, and the next event
  reconstructs cursor progress from live state.
- **A persistent filesystem failure retries once per interval.** → Keep the
  retry so transient failures recover; existing warning logs make a persistent
  failure diagnosable.
- **The flusher holds `attention_mu` during I/O.** → Writes are reduced from
  hundreds per second to one coalesced interval; moving snapshots outside the
  lock would require a separate consistency design.
- **Shutdown callbacks race with destruction.** → Disable producers before the
  final flush and join, keeping the implementation alive until the thread exits.
