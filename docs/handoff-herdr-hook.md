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
`commandWindows` handlers. Every command requires the complete Herdr pane
environment and exits successfully otherwise.

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

Windows commands were statically inspected but not executed because the build
machine has no Windows `cmd.exe` or Wine.

## Build status and blocker

Do not treat this branch as compiled or test-passing yet.

The ARM64 Linux configure completed successfully after initializing submodules:

```bash
git submodule update --init --recursive
export VCPKG_ROOT=/root/vcpkg
export VCPKG_FORCE_SYSTEM_BINARIES=1
cmake --preset linux-arm64-debug -DVCPKG_MANIFEST_FEATURES=tests
```

The first build used `-j2`:

```bash
cmake --build build/linux-arm64-debug --target acecode_unit_tests -j2
```

It reached the linking phase for helper test executables, then the kernel killed
`ld` with signal 9 because two large links exhausted the 7.5 GiB machine.
This was an environment OOM, not a compiler diagnostic from the changed code.

A second build was started with `-j1` to serialize links and was progressing
normally, but it was intentionally interrupted when the session was stopped.
No final `acecode_unit_tests` or `acecode` binary was produced.

## Recommended continuation on another machine

```bash
git submodule update --init --recursive
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset <matching-platform-debug-preset> -DVCPKG_MANIFEST_FEATURES=tests
cmake --build build/<preset> --target acecode_unit_tests -j1
```

Run the focused tests first:

```bash
./build/<preset>/tests/acecode_unit_tests \
  --gtest_filter='HookRuntime.*:HookAgentLoop.Permission*'
```

Then run the complete suite and build the CLI:

```bash
ctest --test-dir build/<preset> --output-on-failure
cmake --build build/<preset> --target acecode
```

On a Windows machine, additionally start a build containing the new seed in a
Herdr pane and verify the seeded `commandWindows` handlers with a real Herdr
executable.

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
