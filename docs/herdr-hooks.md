# Herdr custom-agent hook

ACECode does not contain a Herdr reporter or inspect Herdr environment variables in core code. The default versioned seed installs [`examples/herdr-hooks.json`](examples/herdr-hooks.json) as a managed hook package that connects generic ACECode lifecycle hooks to Herdr's custom-agent CLI.

## Default installation

When a build containing this seed starts, ACECode reconciles it to
`~/.acecode/hooks/agent-reporting/hooks.json` before loading the hook registry.
The official definition is loaded as `ManagedTrusted`, so no manual merge or trust
approval is required. Existing `~/.acecode/hooks.json`, `~/.codex/hooks.json`, and
project hook files are left untouched.

The seed is versioned and idempotent. ACECode updates an unchanged ACECode-owned
copy on a later bundle revision, but preserves a user-modified copy. A modified or
malformed copy no longer receives managed automatic trust.

Start ACECode inside a Herdr pane to use the integration. The example JSON remains
available as a readable reference; copying it manually is unnecessary and can cause
duplicate reports if that copy is separately trusted.

The same JSON includes POSIX `command` entries and Windows `commandWindows`
entries. Each command requires `HERDR_ENV=1`, `HERDR_PANE_ID`, and
`HERDR_SOCKET_PATH`. `HERDR_BIN_PATH` is optional: POSIX first uses it and then
searches `PATH`; Windows first uses it, then checks the standard per-user Herdr
installation, and finally searches `PATH`. If the required pane environment or
the CLI is unavailable, the command exits successfully without invoking anything.

The hook always reports the exact injected `HERDR_PANE_ID`. It never guesses from
the focused pane because focus can change while a hook is running. If a pane has
inherited another pane's ID, update/restart Herdr and recreate that pane before
starting ACECode there.

## State mapping

- `SessionStart` -> metadata + `idle`
- `UserPromptSubmit` -> `working`
- `PermissionRequest` -> `blocked`
- `PermissionResolved` -> `working`
- `PreToolUse(AskUserQuestion)` -> `blocked`
- `PostToolUse(AskUserQuestion)` -> `working`
- `Stop` -> `idle`

`PermissionResolved` is an ACECode lifecycle extension. Its payload contains the standard tool fields plus:

```json
{
  "permission_decision": "allow | always_allow | deny",
  "permission_source": "hook | interactive | headless | implicit"
}
```

## Scope and limitations

The hook reports lifecycle source `custom:acecode`, agent label `acecode`, display name `AceCode`, and custom state labels. The commands are local and synchronous with a one-second timeout because Codex-format asynchronous command hooks are not supported yet.

Disabling ACECode's hooks feature disables the managed seed hook together with all
other hooks.

The same lifecycle dispatch runs in the terminal TUI, daemon/headless flows, and
the desktop/web surfaces. In the TUI, submitting a prompt changes the Herdr state
to `working`, permission or `AskUserQuestion` waits change it to `blocked`, and
completion returns it to `idle`.

Herdr does not persist native cold-resume references from custom sources. This hook therefore provides live identity and state reporting, not automatic AceCode session restoration after a Herdr restart. Use `acecode --resume <session-id>` for AceCode's own resume flow.
