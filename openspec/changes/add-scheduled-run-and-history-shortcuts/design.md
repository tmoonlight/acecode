## Context

LoopPage currently offers enable/edit/delete/history. LoopStore already reserves active runs transactionally; LoopScheduler owns launch and shutdown. Shortcut settings use a shared local store; Slate and native fields currently own their editing history.

## Goals / Non-Goals

Use the existing execution and editing owners. Do not create a second session launch path, change recurrence, add database schema, or customize terminal key semantics.

## Decisions

- Add a transactional manual claim that rejects active task/workspace execution without advancing the schedule. Queue owned definition/run snapshots on the scheduler's existing worker; expose through a nullable borrowed scheduler dependency and authenticated POST endpoint. WebServer stops before the daemon destroys that scheduler. Avoid launching sessions on HTTP threads.
- Synchronize scheduler notifications with its wait predicate to avoid losing immediate-run requests. Stop joins the worker, clears pending snapshots and interrupts its durable reservations.
- The condition-variable predicate borrows only the stop flag, pending queue and change counter during the synchronous wait call; capture the observed generation by value. The predicate never leaves the worker stack, and stop joins that worker before these members are destroyed.
- Add context bindings `undo` / `redo`; shared history action resolution handles remapping and suppresses obsolete defaults. Slate handlers execute their own history; a native-input hook uses browser history commands and excludes Slate and terminal targets.
- Keep Ctrl+Y reserved because the existing home suggestion refresh uses it. Allow Ctrl+Z / Ctrl+Shift+Z only for their corresponding history actions.
- Shortcut recording saves valid complete chords immediately, retaining failures for retry; no new save/apply buttons or secondary labels.

## Risks / Trade-offs

- Manual and automatic claims can race → use the existing SQLite transaction and active-run checks for both.
- Native browser undo support varies → exercise actual Chromium inputs in addition to pure binding tests; keep unchanged native bindings on their existing default path.
- Existing browser smoke scripts assume save/cancel → update their assertions to the settings auto-save contract.

## Migration Plan

No data migration. Rebuild web assets before the native targets. Existing shortcut overrides keep their storage key and default missing history bindings automatically.
