# Herdr custom-agent hook

ACECode does not contain a Herdr reporter or inspect Herdr environment variables. The optional [`examples/herdr-hooks.json`](examples/herdr-hooks.json) file connects generic ACECode lifecycle hooks to Herdr's custom-agent CLI.

## Install

1. Merge the JSON file's `hooks` object into `~/.acecode/hooks.json` (or copy it as `<project>/.acecode/hooks.json`). Do not overwrite unrelated hooks already in that file.
2. Start ACECode inside a Herdr pane.
3. Open `/hooks` in ACECode and approve the new local command hooks when prompted.

The same JSON includes POSIX `command` entries and Windows `commandWindows` entries. Each command checks `HERDR_ENV`, `HERDR_PANE_ID`, `HERDR_SOCKET_PATH`, and `HERDR_BIN_PATH`; outside a Herdr pane it exits successfully without invoking anything.

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

Herdr does not persist native cold-resume references from custom sources. This hook therefore provides live identity and state reporting, not automatic AceCode session restoration after a Herdr restart. Use `acecode --resume <session-id>` for AceCode's own resume flow.
