# Handoff: Herdr custom-agent hook

## Goal and non-goals

This branch adds live Herdr state reporting without modifying Herdr and without
embedding a Herdr reporter in AceCode runtime code.

The final architecture is:

1. AceCode adds one generic lifecycle extension, `PermissionResolved`.
2. A versioned default hook seed maps generic lifecycle events to Herdr CLI
   commands: [`../assets/seed/hooks/agent-reporting/hooks.json`](../assets/seed/hooks/agent-reporting/hooks.json).
3. Default startup reconciliation installs the hook under
   `~/.acecode/hooks/agent-reporting/`; outside Herdr its guards make it a no-op.

Explicit non-goals:

- no `HERDR_*` environment detection in `src/`
- no Herdr reporter, installer, provider, or socket client in AceCode
- no Herdr source changes
- no OpenCode impersonation
- no automatic Herdr cold-resume integration

## Implementation summary

### Generic `PermissionResolved` hook

Files:

- `src/hooks/hook_runtime.hpp`
- `src/hooks/hook_runtime.cpp`
- `src/agent_loop.cpp`

`PermissionResolved` is emitted exactly once after a previously dispatched
`PermissionRequest` is finalized. It runs before the approved tool starts or
before the denial result is returned to the model.

Payload additions:

```json
{
  "permission_decision": "allow | always_allow | deny",
  "permission_source": "hook | interactive | headless | implicit"
}
```

The event is observational. Its output cannot change the permission decision.
Normal hook diagnostics and system messages are still processed, while
additional context and allow/deny output are ignored.

No resolution event is emitted for tools that were auto-allowed without a
`PermissionRequest`.

### Herdr hook JSON and default seed

Files:

- `assets/seed/hooks/agent-reporting/hooks.json`
- `docs/examples/herdr-hooks.json`
- `docs/herdr-hooks.md`

The seed transaction installs, upgrades, and recovers this hook alongside the
default Skills and Experts without rewriting user hook files. The registry only
marks the installed source `ManagedTrusted` while its parsed definition matches
the official built-in fingerprint; a modified copy is preserved but not run as
a managed hook.

The single JSON file contains both POSIX `command` and Windows
`commandWindows` handlers. Every command requires the Herdr marker, socket, and
pane identity. `HERDR_BIN_PATH` is optional: handlers fall back to the platform
installation/PATH lookup and exit successfully when no Herdr CLI is available.
They always report the injected pane ID and never infer identity from focus.

State mapping:

- `SessionStart` -> metadata + `idle`
- `UserPromptSubmit` -> `working`
- `PermissionRequest` -> `blocked`
- `PermissionResolved` -> `working`
- `PreToolUse(AskUserQuestion)` -> `blocked`
- `PostToolUse(AskUserQuestion)` -> `working`
- `Stop` -> `idle`

Lifecycle source is `custom:acecode`; display metadata renders `AceCode`.

## Tests added

Files:

- `tests/hooks/hook_runtime_test.cpp`
- `tests/hooks/hook_agent_loop_test.cpp`

Coverage includes:

- `PermissionResolved` payload fields
- interactive denial and source attribution
- hook approval and exactly-once pairing
- no resolution event when no request was emitted

## Verification already completed

- `git diff --check` passed.
- A source-tree search found no `Herdr`, `HERDR_`, or `custom:acecode` strings
  under `src/`.
- The JSON file passed strict JSON parsing.
- All POSIX commands were executed against a fake `HERDR_BIN_PATH`:
  - 8 CLI calls were captured
  - metadata arguments were correct
  - state sequence was
    `idle, working, blocked, working, blocked, working, idle`
  - running the same handlers without `HERDR_ENV` produced zero calls

At the time of the original handoff, Windows commands were only statically
inspected because that build machine had no Windows `cmd.exe` or Wine. That gap
allowed the optional `HERDR_BIN_PATH` behavior below to escape validation.

## 2026-08-12 Windows managed-pane regression

A real Herdr pane provided `HERDR_ENV`, `HERDR_SOCKET_PATH`, and
`HERDR_PANE_ID`, but not `HERDR_BIN_PATH`. In `cmd.exe`, an undefined
`%HERDR_BIN_PATH%` remains a literal token in the original command, so its
string guard passed, attempted to run a nonexistent literal executable, hid the
error, and then returned success. ACECode logs therefore showed successful hook
dispatch while Herdr's agent list stayed empty.

The seed now resolves the CLI in this order:

- POSIX: `HERDR_BIN_PATH`, then `PATH`.
- Windows: `HERDR_BIN_PATH`, the standard per-user Herdr executable, then
  `PATH`.

A cross-platform test executes all eight packaged handlers with
`HERDR_BIN_PATH` absent, captures the fake CLI calls, and verifies every call
retains the injected pane ID. The fixed Windows command was also exercised
against a real Herdr CLI and produced an agent-list entry.

One separate external condition was observed: a second pane in the installed
Herdr preview inherited the first pane's `HERDR_PANE_ID`. The hook deliberately
does not replace that identity with the currently focused pane; update/restart
Herdr and recreate affected panes so the process receives the correct ID.

## Current verification

The original ARM64 Linux handoff was blocked by linker OOM, but the continued
work has now been compiled and tested on Windows x64:

- the focused seed/hook suites passed 81 of 81 tests;
- the broad suite passed 3310 tests and skipped 4 environment-only smokes;
- the 6 process-level tests initially omitted from that run passed separately
  after their helper executables were built with the same toolchain;
- 7 `DesktopSingleInstance` tests were not run because an ACECode desktop
  instance was active;
- an isolated Release CLI target linked successfully;
- `openspec validate add-herdr-custom-agent-reporting --strict`,
  `git diff --check`, and the shell code-quality check passed;
- all 8 POSIX and Windows handlers executed against fake CLIs with
  `HERDR_BIN_PATH` absent and preserved the injected pane ID;
- a real Windows Herdr CLI probe created the expected `w1:p2` agent-list entry
  and its validation source was then released.

The remaining deployment step is to ship a package containing seed revision
`2026-08-12.1`. Existing users with an unchanged ACECode-owned hook receive the
new definition during startup reconciliation; modified hook packages remain
preserved and require manual review.

## Review checklist

- Confirm `PermissionResolved` is emitted before execution/denial return on all
  permission paths.
- Confirm outputs from the observational event cannot change a decision.
- Confirm the Herdr example remains entirely outside `src/`.
- Confirm only the fingerprint-matched default seed is managed-trusted; user
  hook sources retain normal trust review.
- Confirm the one-second synchronous CLI timeout is acceptable; async Codex
  command hooks are currently unsupported.
- Keep the documented limitation: custom Herdr sources do not provide native
  cold resume after a Herdr restart.
