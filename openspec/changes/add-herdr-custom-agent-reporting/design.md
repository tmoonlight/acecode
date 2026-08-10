# Design: Generic permission resolution lifecycle

## Core event

When AceCode emits `PermissionRequest`, it records that a permission lifecycle
is open. Once the decision is known, it emits exactly one
`PermissionResolved` event before either starting the tool or returning the
denial result.

The payload reuses the common and tool fields and adds:

- `permission_decision`: `allow`, `always_allow`, or `deny`
- `permission_source`: `hook`, `interactive`, `headless`, or `implicit`

The event is observational. Its output is dispatched but does not override the
already completed permission decision.

## Pairing

A resolution event is emitted only after a request event was dispatched. The
implementation guards against duplicate resolution across hook, headless, and
interactive branches.

## Herdr example

A standalone Codex-format hook JSON calls Herdr's documented CLI with lifecycle
source `custom:acecode`. It contains both POSIX `command` and Windows
`commandWindows` handlers and exits successfully outside Herdr.

State mapping:

- `SessionStart` -> metadata and `idle`
- `UserPromptSubmit` -> `working`
- `PermissionRequest` -> `blocked`
- `PermissionResolved` -> `working`
- `AskUserQuestion` pre/post -> `blocked` / `working`
- `Stop` -> `idle`

The commands are synchronous with a one-second timeout because AceCode currently
skips async Codex command hooks. Failures are redirected and guarded so absence
of Herdr is a no-op.
