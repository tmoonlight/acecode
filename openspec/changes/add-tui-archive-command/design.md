## Context

The TUI `/clear` handler owns more than presentation: it clears the conversation
and Agent Loop history, resets token and goal state, ends the current
`SessionManager`, reapplies defaults for the next session, and preserves lazy
session creation. Web archive routes use the existing `SessionMeta::archived`
flag and leave the transcript intact.

The new command crosses built-in command dispatch and session persistence. It
must not duplicate the clear lifecycle or report success when metadata could
not be written.

## Goals / Non-Goals

**Goals:**

- Expose canonical `/archive` and compatibility `/archieve` TUI commands.
- Persist exactly the archive state already consumed by Web and the TUI
  archived-settings page.
- Run the exact existing clear/reset lifecycle only after archive persistence
  succeeds.
- Preserve the active session and transcript on persistence failure.
- Avoid creating an empty session when no persisted session is active.

**Non-Goals:**

- Changing Web or daemon archive endpoints.
- Adding permanent deletion, restore, confirmation, or archive arguments.
- Changing the session metadata schema.
- Redesigning the slash-command dropdown or settings archived page.

## Decisions

### Add a result-bearing SessionManager archive operation

Add a narrow `archive_current_session()` operation that owns the in-memory
archive flag and immediate metadata write. It returns distinct results for
success, no active session, and persistence failure.

This is preferred over writing `.meta.json` directly in the command handler:
`SessionManager` already owns the active session's metadata fields and lock, so
reconstructing metadata outside it risks overwriting concurrent title, token,
todo, or worktree state.

### Report metadata write success

Make the existing metadata writer return the `atomic_write_file` result and
propagate that result through `SessionManager::update_meta()`. Existing
best-effort callers may continue ignoring the return value; the new archive
operation uses it as a commit gate and restores the in-memory flag if the write
fails.

This is preferred over rereading the file after a void write because a stale
pre-existing `archived=true` value cannot prove that the current write
succeeded.

### Delegate successful completion to the existing clear handler

Keep one internal clear/reset function and call it from `/clear`, `/new`, and
both archive spellings after persistence succeeds. The archive command does not
manually clear individual fields.

When no persisted session is active, the archive command still delegates to
clear. This gives the requested console-clear effect while preserving the
existing rule that an empty session is not created.

### Keep `/archive` canonical and `/archieve` explicit

Register both spellings so each appears in slash discovery. `/help` presents
`/archive` as the feature and describes `/archieve` as its alias, preventing the
misspelling from becoming the documented canonical API.

## Risks / Trade-offs

- **[Metadata succeeds but the subsequent best-effort clear finalization write
  fails]** -> The archive flag was already atomically committed before clear,
  so the session remains safely archived.
- **[Changing metadata write methods to return `bool` touches many callers]** ->
  Preserve all existing call sites and semantics; ignored return values remain
  valid C++, while focused tests cover the new result-bearing path.
- **[The command runs without an active persisted session]** -> Reuse `/clear`
  and do not call lazy session creation, avoiding a meaningless archived file.
- **[Archive alias behavior drifts]** -> Register both spellings with the same
  handler and cover registration plus lifecycle behavior in tests.

## Migration Plan

No data migration is required. Existing metadata without `archived` continues
to mean false. Rollback removes the two command registrations and the
result-bearing helper without changing any stored session data.

## Open Questions

None.
