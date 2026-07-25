## Why

The TUI can clear the active conversation but cannot archive it from the
keyboard-first workflow. Users should be able to move the current persisted
session into the same archive used by Web and immediately continue in a fresh
TUI session.

## What Changes

- Add `/archive` to archive the current TUI session and then perform the same
  reset as `/clear`.
- Accept `/archieve` as a compatibility alias for the user-requested spelling.
- Persist the existing reversible `archived` metadata flag without deleting the
  transcript or other session data.
- Complete the clear/reset only after the archive metadata write succeeds; on a
  persistence failure, retain the active session and visible conversation.
- List both command spellings in slash-command discovery and document the
  canonical spelling in `/help`.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `session-archive`: Allow the active TUI session to be archived through
  `/archive` or `/archieve` using the same persisted archive state as Web.
- `session-lifecycle`: Define the archive-before-clear ordering and preserve the
  existing `/clear` reset and lazy-new-session behavior after a successful
  archive.

## Impact

- TUI built-in command registration, help text, and command dispatch.
- `SessionManager` archive persistence result handling.
- Session metadata write reporting, without changing the metadata schema.
- Focused command and session lifecycle tests.
- No daemon route, Web UI, archive restore/purge behavior, dependency, or data
  migration changes.
