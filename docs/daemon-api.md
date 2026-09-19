# ACECode Daemon Web API

This document describes the HTTP and WebSocket protocol exposed by
`acecode daemon` / `acecode service` for the Web and Desktop frontends.

Source of truth for this document:

- Route registration: `src/web/routes/routes_*.cpp`
- Shared response helpers: `src/web/server_helpers.cpp`
- Frontend callers: `web/src/lib/api.js`, `web/src/lib/connection.js`,
  `web/src/lib/consoleDock.js`

`OPTIONS` routes are CORS preflight helpers and are not listed as first-class
endpoints below.

---

## 1. Connecting

### Starting the daemon

Run `acecode daemon` to start a detached daemon; `acecode daemon start` remains
an explicit compatibility alias. Without `--cwd`, the worker uses the directory
from which the command was invoked. `--cwd=<PATH>` selects another workspace.

### Runtime files

After the daemon starts, runtime files are written to `<data_dir>/run/`:

| File | Content |
|---|---|
| `daemon.pid` | numeric pid |
| `daemon.port` | numeric port |
| `daemon.guid` | UUID v4 |
| `heartbeat` | JSON `{pid, guid, timestamp_ms}` refreshed periodically |
| `token` | URL-safe daemon token |
| `desktop-managed.json` | Desktop-managed identity `{pid, guid, kind, protocol_version, acecode_version}` |
| `desktop-owner.json` | current Desktop owner `{pid, instance_id, timestamp_ms}` |

`<data_dir>` is `~/.acecode/` for standalone daemons and the platform service
data directory for installed services.

The native Desktop uses the reserved
`<data_dir>/run/desktop-shared/` directory. It verifies the process executable,
PID, GUID, heartbeat, port, health response, and Desktop protocol before
attaching to an existing process. A compatible process is reused; a verified
incompatible Desktop-managed generation is stopped and replaced. Standalone
CLI daemons use their normal runtime directory and are not reclaimed by this
Desktop lifecycle. Daemon-generation cleanup preserves `desktop-owner.json`;
the next Desktop instance overwrites that owner record before discovery, which
prevents a late old daemon teardown from erasing a rapid-relaunch handoff.

Closing the macOS Desktop window only hides it. Dock or menu-bar activation
shows the same window again. A real application quit either stops the managed
background process or releases it according to the global Desktop preference
“退出 ACECode 后继续运行后台进程”. The preference defaults to off, and changing
it affects the next application quit rather than immediately stopping the
current process.

### Bind and auth

The daemon always uses the canonical loopback bind `127.0.0.1`. Its fixed,
unconfigured port is `12399`; an explicit `config.web.port` overrides that
default, and `daemon --port=<N>` overrides both. It is fail-fast on a
daemon-port collision and never scans for another port. Existing configurations
that explicitly set `28080` continue to use `28080`.
Remote Web access uses a distinct supervised proxy process configured by
`config.web.remote_enabled` / `config.web.remote_port`.

| Bind | Token required? |
|---|---|
| Loopback (`127.0.0.1`, `localhost`, `::1`) | Optional for same-origin loopback requests |
| Proxy-originated loopback (`127.0.0.2`) | Explicit token required |
| Non-loopback remote client | Explicit token required |

Token locations:

- HTTP: `X-ACECode-Token: <token>`
- WebSocket: `?token=<token>`; browsers cannot set custom WS headers reliably

Auth failures return HTTP `401` with `{"error":"no token"}` or
`{"error":"bad token"}`. WebSocket handshakes are rejected.

### CORS

Loopback origins receive:

- `Access-Control-Allow-Origin: <origin>`
- `Access-Control-Allow-Headers: Content-Type, X-ACECode-Token`
- `Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS`

---

## 2. Common Shapes

### Error body

Most JSON errors are one of:

```json
{"error":"BAD_REQUEST","message":"human readable text"}
```

or:

```json
{"error":"human readable text"}
```

### Workspace

```json
{
  "hash": "16-char-cwd-hash",
  "cwd": "C:/repo",
  "name": "repo",
  "available": true
}
```

`__local__` is a compatibility workspace hash for the daemon cwd.

### Session summary

Session list endpoints return arrays of objects shaped like:

```json
{
  "id": "session-id",
  "active": true,
  "status": "idle",
  "workspace_hash": "abc123",
  "cwd": "C:/repo",
  "no_workspace": false,
  "title": "Investigate daemon routes",
  "title_source": "user",
  "summary": "latest user summary",
  "created_at": "2026-07-04T01:23:45Z",
  "updated_at": "2026-07-04T01:25:00Z",
  "provider": "openai",
  "model": "gpt-4.1",
  "model_name": "work-gpt",
  "model_preset": "work-gpt",
  "context_window": 128000,
  "deleted": false,
  "message_count": 12,
  "turn_count": 4,
  "worktree": {
    "name": "ses-session-id",
    "branch": "worktree-ses-session-id",
    "path": "C:/repo/.acecode/worktrees/ses-session-id"
  },
  "permission_mode": "default",
  "token_usage": null,
  "session_token_usage": null,
  "todos": [],
  "todo_summary": {"total":0,"pending":0,"in_progress":0,"completed":0,"cancelled":0},
  "archived": false,
  "attention_state": "read",
  "read_state": "read",
  "busy": false,
  "active_turn_id": "",
  "status_cursor": 0,
  "update_cursor": 0,
  "read_cursor": 0
}
```

Some fields are omitted when empty, especially `worktree`, `todos`, and token
usage. For a managed-worktree session, top-level `cwd` intentionally remains
the workspace/session-storage root. `worktree.path` is the active absolute
working root used by file, Git, LSP, and path-reference surfaces.

### Token usage

```json
{
  "prompt_tokens": 44100,
  "completion_tokens": 2100,
  "total_tokens": 46200,
  "cache_read_tokens": 0,
  "cache_write_tokens": 0,
  "reasoning_tokens": 0,
  "has_data": true,
  "context_breakdown": {
    "system_prompt": 478,
    "project_rules": 4400,
    "skills": 5200,
    "builtin_tools": 9900,
    "mcp_tools": 2200,
    "conversation": 21000,
    "dynamic_context": 922,
    "has_data": true
  }
}
```

`context_breakdown` is optional and describes the latest provider prompt using
ACECode-side estimates. Its seven category values are proportionally reconciled
to the provider-reported `prompt_tokens`, but remain approximate rather than
provider-tokenizer or billing measurements. Older session metadata and usage
events can omit the object.

### Session event

Server event frames and replayed events use:

```json
{
  "type": "message",
  "seq": 42,
  "timestamp_ms": 1783152000000,
  "replayed": false,
  "session_id": "session-id",
  "workspace_hash": "abc123",
  "payload": {}
}
```

`payload.session_id`, `payload.workspace_hash`, and `payload.cwd` are injected
when known.

`replayed` is delivery provenance: it is `false` for live session events and
`true` for copies replayed from the event ring on reconnect. It is not added to
the stored event payload. Clients must require explicit `false` before applying
side effects such as an AI theme installation; history and catch-up frames only
update their transcript presentation.

---

## 3. HTTP Endpoint Index

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/health` | daemon liveness and capabilities |
| GET | `/api/model-pool-status` | model pool load snapshot |
| GET | `/api/usage` | token usage aggregation |
| GET | `/api/history` | input history by cwd |
| POST | `/api/history` | append input history |
| GET | `/api/workspaces` | list registered workspaces |
| POST | `/api/workspaces` | register cwd as workspace |
| POST | `/api/workspaces/pick-folder` | desktop native folder picker |
| GET | `/api/projects/defaults` | new-project default parent directory |
| POST | `/api/projects` | create and register a new project directory |
| POST | `/api/open-in-explorer` | open a folder or reveal a file in the OS file manager |
| GET | `/api/workspaces/:hash/sessions` | list sessions in workspace; `limit=N` returns `{sessions,total,total_exact,has_more}` |
| POST | `/api/workspaces/:hash/sessions` | create workspace session |
| POST | `/api/workspaces/:hash/sessions/:id/resume` | resume workspace session |
| DELETE | `/api/workspaces/:hash/sessions/:id?purge=1` | permanently delete archived workspace session |
| PUT | `/api/workspaces/:hash/sessions/:id/archive` | archive workspace session |
| DELETE | `/api/workspaces/:hash/sessions/:id/archive` | unarchive workspace session |
| PUT | `/api/workspaces/:hash/sessions/:id/title` | set session title |
| GET | `/api/workspaces/:hash/sessions/:id/draft` | read composer draft |
| PUT | `/api/workspaces/:hash/sessions/:id/draft` | write composer draft |
| DELETE | `/api/workspaces/:hash/sessions/:id/todos` | clear session todos |
| GET | `/api/workspaces/:hash/opencode-import` | preview opencode import |
| POST | `/api/workspaces/:hash/opencode-import` | start opencode import job |
| GET | `/api/workspaces/:hash/opencode-import/:job_id` | poll opencode import job |
| GET | `/api/workspaces/:hash/pinned-sessions` | list pinned session ids |
| PUT | `/api/workspaces/:hash/pinned-sessions` | set pinned session ids |
| GET | `/api/no-workspace/pinned-sessions` | list pinned no-workspace task ids |
| PUT | `/api/no-workspace/pinned-sessions` | set pinned no-workspace task ids |
| GET | `/api/pinned-sessions/order` | read global cross-scope pin order |
| PUT | `/api/pinned-sessions/order` | set global cross-scope pin order |
| GET | `/api/sessions` | compatibility session list |
| GET | `/api/session-search/sessions?q=...&limit=N&cursor=...&request_id=...` | incremental global session catalog page |
| GET | `/api/session-search/user-messages?q=...&limit=N&request_id=...` | advance one bounded visible-user-message search batch |
| POST | `/api/session-search/requests/:request_id/cancel` | cancel/pause an incremental global search |
| POST | `/api/sessions` | compatibility session create |
| POST | `/api/sessions/:id/resume` | compatibility session resume |
| DELETE | `/api/sessions/:id` | destroy active session |
| DELETE | `/api/sessions/:id?purge=1` | permanently delete archived or sub-agent session |
| PUT | `/api/sessions/:id/archive` | archive compatibility session |
| DELETE | `/api/sessions/:id/archive` | unarchive compatibility session |
| PUT | `/api/sessions/:id/title` | set compatibility session title |
| GET | `/api/sessions/:id/draft` | read compatibility draft |
| PUT | `/api/sessions/:id/draft` | write compatibility draft |
| DELETE | `/api/sessions/:id/todos` | clear compatibility todos |
| GET | `/api/sessions/:id/messages` | transcript snapshot or event replay |
| GET | `/api/sessions/:id/trajectory` | paged durable session trajectory and legacy projection |
| POST | `/api/sessions/:id/export-markdown` | open Save As and export the visible transcript as Markdown |
| POST | `/api/sessions/:id/messages` | queue user input |
| POST | `/api/sessions/:id/turn/steer` | append input to the matching active turn |
| POST | `/api/sessions/:id/turn/interrupt` | interrupt the matching active turn and start a priority replacement turn |
| POST | `/api/sessions/:id/questions/interject` | resolve a pending AskUserQuestion with a free-form message and continue the same turn |
| POST | `/api/sessions/:id/attachments` | upload a session snapshot or create a Desktop source reference |
| GET | `/api/sessions/:id/attachments/:attachment_id/blob` | download attachment bytes |
| POST | `/api/sessions/:id/commands` | run daemon builtin slash command |
| POST | `/api/sessions/:id/side-question` | run isolated one-turn `/btw` question |
| PUT | `/api/sessions/:id/expert` | switch the active session expert for subsequent turns |
| DELETE | `/api/sessions/:id/expert` | clear the persisted/UI expert binding without rebuilding live context |
| GET | `/api/experts` | list expert components for a workspace |
| POST | `/api/experts` | create a managed global expert component |
| GET | `/api/experts/capabilities` | list sanitized expert capability choices |
| GET | `/api/experts/:id` | read one expert component |
| GET | `/api/experts/:id/avatar` | read a contained expert avatar image, optionally for a fixed state |
| PUT | `/api/experts/:id` | update a managed global expert component |
| DELETE | `/api/experts/:id` | delete a managed global expert component |
| GET | `/api/sessions/:id/permissions` | read session permission mode |
| PUT | `/api/sessions/:id/permissions` | set session permission mode |
| GET | `/api/sessions/:id/model` | read session model state |
| POST | `/api/sessions/:id/model` | switch session model |
| POST | `/api/sessions/:id/reasoning` | set or reset the idle session's reasoning effort |
| POST | `/api/sessions/:id/model/reload` | force reload the selected saved-model profile for one active session |
| POST | `/api/sessions/:id/fork` | fork a transcript prefix |
| POST | `/api/sessions/:id/file-checkpoints/:message_id/restore` | restore files to checkpoint |
| GET | `/api/files` | list directory |
| GET | `/api/files/content` | read text file |
| GET | `/api/files/blob` | read previewable binary file |
| GET / PUT | `/api/files/editable` | read or safely save a Desktop workspace text file |
| GET | `/api/fs/roots` | browsable roots for the Web path picker |
| GET | `/api/fs/list` | list any absolute directory for the Web path picker |
| GET | `/api/git/info` | git repo info for a workspace |
| POST | `/api/git/checkout` | switch branch (stash-aware) |
| GET | `/api/git/changes` | working tree changes vs base |
| GET | `/api/git/diff` | single-file patch vs base |
| GET | `/api/commands` | list slash commands |
| GET | `/api/skills/root` | resolve effective skills directory |
| GET | `/api/skills` | list registered skills |
| PUT | `/api/skills/:name` | enable or disable skill |
| GET | `/api/skills/:name/body` | read `SKILL.md` body |
| GET | `/api/hooks` | list hooks snapshot |
| POST | `/api/hooks/refresh` | reload hook registry |
| POST | `/api/hooks/:id/trust` | trust hook |
| POST | `/api/hooks/:id/disable` | disable hook |
| POST | `/api/hooks/:id/enable` | enable hook |
| GET | `/api/models` | list saved model profiles |
| POST | `/api/models` | add saved model profile |
| POST | `/api/config/model-order` | persist saved model order (`{names: string[]}`) |
| PUT | `/api/models/:name` | update saved model profile |
| DELETE | `/api/models/:name` | remove saved model profile |
| POST | `/api/models/probe` | probe provider model ids |
| POST | `/api/models/test` | test an unsaved model with a short conversation |
| GET | `/api/models/catalog` | read local model catalog summary and reviewed recommendations |
| GET | `/api/models/catalog/:provider_id` | search one provider's local model catalog |
| POST | `/api/models/catalog/refresh` | explicitly refresh the models.dev registry when network refresh is enabled |
| GET | `/api/config/default-model` | read default saved model name |
| POST | `/api/config/default-model` | set default saved model |
| GET | `/api/copilot/auth` | read Copilot auth status |
| DELETE | `/api/copilot/auth` | delete saved GitHub token |
| POST | `/api/copilot/auth/device` | start GitHub device flow |
| POST | `/api/copilot/auth/device/poll` | poll device flow |
| GET | `/api/grok/auth` | read Grok Coding Plan auth status |
| DELETE | `/api/grok/auth` | delete saved xAI OAuth credentials |
| POST | `/api/grok/auth/device` | start xAI device flow |
| POST | `/api/grok/auth/device/poll` | poll one xAI device-flow tick |
| GET | `/api/ui/onboarding/desktop` | read Desktop guided-tour status |
| POST | `/api/ui/onboarding/desktop/dismiss` | dismiss the current Desktop guided-tour version |
| GET | `/api/config/ui-preferences` | read UI preferences |
| PUT | `/api/config/ui-preferences` | write UI preferences |
| GET | `/api/themes` | downloadable catalogue plus installed local AI themes |
| POST | `/api/themes/first-run` | durably claim the one-time National Day startup attempt |
| POST | `/api/themes/import/preview` | validate a raw theme ZIP and return a read-only preview |
| POST | `/api/themes/import?sha256=<digest>` | import the same previewed ZIP after confirmation |
| GET | `/api/themes/job` | current theme download progress |
| POST | `/api/themes/job/cancel` | cancel the current theme download |
| GET | `/api/themes/<id>` | verified locally installed theme definition |
| POST | `/api/themes/<id>/install` | download a theme after size/hash confirmation |
| GET | `/api/themes/<id>/images/<kind>` | theme thumbnail or installed background |
| POST | `/api/themes/<id>/export` | prepare a custom theme ZIP, optionally saving through the native picker |
| GET | `/api/themes/exports/<job_id>` | read theme export/packing progress |
| POST | `/api/themes/exports/<job_id>/cancel` | cancel the matching theme export job |
| GET | `/api/themes/exports/<job_id>/download` | download the completed, verified theme ZIP |
| DELETE | `/api/themes/<id>` | delete a custom theme and update the active preference when needed |
| GET | `/api/config/ui-locale` | read Desktop/WebUI locale preference |
| PUT | `/api/config/ui-locale` | write Desktop/WebUI locale preference |
| GET | `/api/config/custom-instructions` | read custom instructions |
| PUT | `/api/config/custom-instructions` | write custom instructions |
| GET | `/api/config/connectors` | read connector settings |
| GET | `/api/config/image-generation` | read sanitized image generation settings |
| GET | `/api/config/summary-generation` | read summary-model override and available models |
| PUT | `/api/config/summary-generation` | save summary-model override for automatic session titles |
| PUT | `/api/config/image-generation` | save image generation settings and refresh the tool |
| POST | `/api/config/image-generation/test` | explicitly generate one standard-quality test image |
| GET | `/api/config/tool-rewrites` | read tool rewrite settings plus the built-in tool catalog |
| PUT | `/api/config/tool-rewrites` | replace tool rewrite settings, persist `tool-rewrites.json`, apply live |
| GET | `/api/config/sandbox` | read sandbox switches, filesystem lists, defaults and platform probe |
| PUT | `/api/config/sandbox` | save sandbox switches / lists to `config.json`, push to active sessions |
| GET | `/api/security/exec-rules` | list `<data_dir>/rules/*.rules` (managed files editable) |
| PUT | `/api/security/exec-rules` | rewrite the managed rules files, reload rules in active sessions |
| GET | `/api/security/audit` | audit log page with filters and cursor pagination |
| GET | `/api/security/audit/summary` | audit counts and recently blocked paths |
| GET | `/api/security/audit/export` | audit log as a JSONL or CSV attachment |
| DELETE | `/api/security/audit` | clear the audit log |
| PUT | `/api/config/connectors` | write connector settings |
| GET | `/api/config/default-permission-mode` | read default permission mode |
| PUT | `/api/config/default-permission-mode` | write default permission mode |
| GET | `/api/config/remote-web` | read remote Web proxy state and connection URLs |
| PUT | `/api/config/remote-web` | enable or disable remote Web mode |
| GET | `/api/config/upgrade` | read update service config |
| PUT | `/api/config/upgrade` | write update service config |
| GET / PUT | `/api/config/toolchains` | read or save Python, Node.js and C# directories |
| POST | `/api/config/toolchains/detect` | detect installed toolchain directories again |
| GET | `/api/console/config` | terminal configuration and launch-probe results |
| POST | `/api/console/config/detect` | probe and save the default terminal again |
| GET | `/api/config/data-dir` | current data directory, migration and backup status |
| POST | `/api/config/data-dir/migrate` | copy data to an empty directory; restart required |
| GET | `/api/config/data-dir/migration` | poll the background migration |
| POST | `/api/config/data-dir/cleanup` | keep or delete the previous data directory |
| POST | `/api/dialog/pick-folder` | select a folder without registering a project |
| POST | `/api/dialog/pick-file` | select a terminal program |
| GET | `/api/update/status` | check update availability |
| POST | `/api/update/start` | start explicit WebUI update job |
| GET | `/api/update/job` | read latest WebUI update job |
| GET | `/api/update/jobs/:id` | poll one WebUI update job |
| POST | `/api/update/jobs/:id/cancel` | cancel one WebUI update job before installation |
| GET | `/api/mcp` | read MCP config |
| GET | `/api/mcp/schema` | read MCP configuration JSON schemas |
| PUT | `/api/mcp` | validate, persist, and apply MCP config |
| POST | `/api/mcp/toggle` | validate and persist one server's enabled state |
| POST | `/api/mcp/reload` | validate/recover persisted config and apply it |
| GET | `/api/feedback/desktop/recent-sessions` | list sessions for feedback attachment |
| POST | `/api/feedback/desktop` | package and upload desktop feedback |
| GET | `/api/pty/shells` | list console shell choices |
| GET | `/api/pty` | list PTY sessions |
| POST | `/api/pty` | create PTY session |
| DELETE | `/api/pty/:id` | remove PTY session |
| POST | `/api/pty/:id/resize` | resize PTY |
| POST | `/api/pty/:id/title` | set PTY title |
| PUT | `/api/console/config` | write console shell config |

`POST /api/config/model-order` requires authentication and a complete permutation of
the current model names returned by `GET /api/models`. Legacy profiles belonging
to disabled providers retain their positions and contents. It returns `{"ok": true}` on success, `400` for
malformed JSON or a non-string-array `names`, `409 MODEL_ORDER_CONFLICT` for a
duplicate, missing, or unknown name, and `500 PERSIST_FAILED` if saving fails.
The mutation reorders the latest profiles atomically without changing their
contents or `default_model_name`. `GET /api/models` returns the persisted order;
an unchanged order performs no write.

---

## 4. Health, Usage, and History

### `GET /api/health`

Returns daemon identity and frontend capabilities. Loopback requests remain
token-optional; non-loopback requests must authenticate before any metadata is
returned.

```json
{
  "guid": "ea86842a-fb1c-4242-b2b4-74be2aff1058",
  "pid": 18204,
  "port": 12399,
  "version": "0.5.10",
  "cwd": "C:/repo",
  "uptime_seconds": 423,
  "desktop_managed": true,
  "desktop_protocol_version": 1,
  "notifications": {
    "enabled": true,
    "on_question": true,
    "on_completion": true,
    "suppress_when_focused": true
  },
  "features": {
    "completed_turn_self_heal": {"enabled": true}
  },
  "console": {
    "available": true,
    "backend": "conpty"
  }
}
```

`desktop_managed` is `true` only for the daemon generation started for the
native Desktop. `desktop_protocol_version` is the attach/reuse compatibility
contract; Desktop verifies it together with the runtime identity before
reusing a process.

`console.backend` is `conpty`, `winpty`, `pipe`, or `posix`.

### `GET /api/model-pool-status`

No auth requirement. Used by the chat UI to show model pool load. A configured
provider `model` is pool-backed only when it exactly equals a returned
`modelPoolName`; no naming prefix is required.

```json
{
  "models": [
    {
      "modelPoolName": "DeepSeek-V4-Flash",
      "usageRate": 42,
      "maxWindowTokens": 150000,
      "effectiveContextWindow": 120000
    }
  ]
}
```

### `GET /api/usage`

Query parameters:

- `days`: optional, defaults to `30`
- `workspace`: optional workspace hash; `__local__` means daemon cwd
- `timezone_offset_minutes`: optional JS `Date#getTimezoneOffset()` value

Returns usage summary, daily buckets, model buckets, workspace buckets, and
metadata. Durable usage is forward-only; older session metadata is not
backfilled.

### `GET /api/history?cwd=<cwd>&max=N`

Returns an array of input history strings for `cwd`.

### `POST /api/history`

Body:

```json
{"text":"last prompt"}
```

Appends to the daemon cwd input history. Returns `204`. If input history is
disabled, the write is silently ignored.

---

## 5. Workspaces and Sessions

### `GET /api/workspaces`

Returns `Workspace[]`. The registry is scanned before listing. If no registry
is available, the compatibility workspace may be returned.

### `POST /api/workspaces`

Body:

```json
{"cwd":"C:/repo"}
```

Registers the cwd and returns `201` plus a `Workspace`. Errors:

- `400` bad JSON or missing `cwd`
- `503` workspace registry unavailable

### `POST /api/workspaces/pick-folder`

Desktop-only native folder picker. Returns a registered `Workspace` or `null`
when the user cancels. Errors:

- `501` native folder picker unavailable
- `503` registry or callback unavailable

### `GET /api/projects/defaults`

Returns the default parent used by the new-project modal:

```json
{"parent_dir":"C:/Users/me/.acecode/workspaces"}
```

This user-source root is a sibling of the internal hash-indexed
`<data-dir>/projects` metadata directory. The endpoint does not create the
directory; default project creation creates it on demand.

### `POST /api/projects`

Creates one empty child directory, registers that child as a visible workspace,
and returns `201` with the normal `Workspace` fields plus creation details.

```json
{
  "name": "demo-api",
  "parent_dir": "C:/Users/me/.acecode/workspaces",
  "project_dir": "C:/Users/me/.acecode/workspaces/demo-api"
}
```

`parent_dir` is optional. When omitted or empty, ACECode uses the parent from
`GET /api/projects/defaults` and creates that default parent on demand. A custom
parent must already exist, be absolute, and be a directory.

The returned object includes `hash`, `cwd`, `name`, `available`,
`requested_name`, `directory_name`, `parent_dir`, `project_dir`, and
`sanitized`. The cross-platform directory-name contract:

- trims surrounding ASCII whitespace and trailing dots/spaces;
- replaces ASCII control characters and `<>:"/\\|?*` with `-`;
- appends `-project` to Windows reserved device names such as `CON`, `NUL`,
  `COM1`, or a reserved base followed by an extension;
- truncates to 60 Unicode code points without splitting a code point;
- falls back to `project` when normalization would otherwise leave an empty or
  dot-only component.

Creation never adopts or overwrites an existing path. Errors:

- `400 PROJECT_BAD_REQUEST` malformed JSON or field types
- `400 PROJECT_NAME_REQUIRED` empty project name
- `400 PROJECT_PARENT_ABSOLUTE_REQUIRED` relative custom parent
- `400 PROJECT_PARENT_NOT_FOUND` missing custom parent
- `400 PROJECT_PARENT_NOT_DIRECTORY` custom parent is not a directory
- `409 PROJECT_ALREADY_EXISTS` target file or directory already exists
- `500 PROJECT_CREATE_FAILED` default-parent or child-directory creation failed
- `503` workspace registry unavailable

### `POST /api/open-in-explorer`

Body:

```json
{"path":"C:/repo"}
```

Opens an absolute directory in Explorer/Finder/xdg-open, or reveals an existing
regular file in its containing folder. Windows Explorer and macOS Finder select
the file; Linux opens the containing directory because there is no portable
freedesktop selection protocol. The desktop callback accepts any existing local
absolute regular file or directory that the daemon process can access; it does
not restrict the target to registered workspaces, the daemon cwd, or
ACECode-managed roots. Empty, relative, missing, and unsupported target types
remain invalid. Returns `{"ok":true}`. Returns `501` when the daemon has no
desktop callback.

### `GET /api/workspaces/:hash/sessions?archived=1`

Returns `SessionSummary[]` for a workspace. Without `archived=1`, active and
unarchived disk sessions are returned. With `archived=1`, only archived disk
sessions are returned.

Listing reads session metadata only; it does not open JSONL transcripts to
backfill legacy summary or count fields.

`limit=N` (positive integer, capped at 10000) returns a compact page instead of
the raw array:

```json
{
  "sessions": [ { "id": "..." } ],
  "total": 42,
  "total_exact": true,
  "has_more": false
}
```

`sessions` is the newest N rows, ordered by `updated_at` descending — the same
order the unlimited list uses. Omitting `limit`, or passing `0`, keeps the
original array body and never adds the paging fields.

A bounded page stops reading the project directory as soon as it has enough
rows, so it does not learn the exact post-filter count:

- `total_exact: true` — the whole directory was read; `total` is the exact
  count after archive and parent filters.
- `total_exact: false` — reading stopped early; `total` is an upper bound
  derived from the number of candidate metadata files (it still counts
  archived rows, sub-agent sessions, and rows already reported as active).
- `has_more` is authoritative in either case. **Clients deciding whether a
  workspace still has unloaded rows must read `has_more`**, not compare
  `sessions.length` against `total` — that comparison is wrong whenever
  `total_exact` is false.

This bound is why the sidebar's collapsed 5-row list is cheap: a workspace with
1400 sessions opens ~13 metadata files instead of all 1400, which on Windows is
the difference between roughly 7 s and a few ms per request.

### `POST /api/workspaces/:hash/sessions`

Creates a session in the workspace. Body fields are optional:

```json
{
  "model": "saved-model-name",
  "name": "saved-model-name",
  "permission_mode": "default",
  "permissionMode": "default",
  "reasoning_effort": "high",
  "initial_user_message": "hello",
  "auto_start": true,
  "no_workspace": false,
  "noWorkspace": false
}
```

`model` and `name` are aliases. `permission_mode` and `permissionMode` are
aliases. `auto_start` defaults to `false`; it only starts a turn when an
`initial_user_message` is present. Returns:

```json
{
  "session_id": "sid",
  "id": "sid",
  "workspace_hash": "abc123",
  "cwd": "C:/repo"
}
```

Errors include `404` unknown workspace, `409` workspace path unavailable,
`400` invalid permission mode, and `503` session client unavailable.

`reasoning_effort` is optional (`null` inherits the saved model default). A
string must be one of the enabled model's declared efforts; invalid or
unsupported values fail before a session or initial turn is created. The
override belongs to the new session and does not modify the saved model.

### `POST /api/workspaces/:hash/sessions/:id/resume`

Loads an existing disk session into the current daemon registry. Returns
`{"session_id","id","active":true,"workspace_hash","cwd"}`. Errors:

- `404` workspace or session not found
- `409` session is active in another live process, old incompatible PID data,
  or workspace path unavailable
- `503` session client unavailable

Workspace registration controls Desktop visibility, not whether a persisted
session can be opened. An exact 16-hex workspace hash that is absent from the
visible registry may be resolved read-only from
`projects/<hash>/workspace.json`, or from a hash-matching ordinary session
meta in that project directory. This fallback never registers the workspace
or changes `desktop_visible`; `/api/workspaces` therefore remains a list of
visible projects only.

### `DELETE /api/workspaces/:hash/sessions/:id?purge=1`

Permanently deletes an archived session from the specified workspace. The
daemon destroys any lingering in-memory registration, removes the session from
the user-message search index, then removes `<id>.jsonl`, the per-session
`<id>/` persisted-data directory, and `<id>.meta.json` last. Returns `204` only
after cleanup succeeds.

Guard rails and errors:

- `400` when `purge=1` is missing or the session id is invalid
- `404` when the workspace or session does not exist
- `409 {"error":"session must be archived before permanent deletion"}` when
  the target is not archived
- `409` when the target is unexpectedly busy
- `500` when search-index or file cleanup fails; metadata is retained until
  the other known session data has been removed so the operation remains
  retryable
- `503` session client unavailable

### Global session search

`GET /api/session-search/sessions` reads a daemon-lifetime, per-project catalog
that is prewarmed in the background. It never synchronously rescans the whole
project store. The optional parameters are:

- `request_id`: client-generated id shared by metadata and content calls;
  required for explicit cancellation.
- `q`: server-side title, summary, workspace, id, and fuzzy-title filter.
- `limit`: `1..100`. An empty query is always capped at the 50 most recently
  updated sessions.
- `cursor`: opaque `generation:offset` value returned by a completed page.
  Pagination is offered only after the current catalog generation is complete.

The response uses a search-only DTO rather than the full session serializer:

```json
{
  "sessions": [
    {
      "id": "sid",
      "workspace_hash": "0123456789abcdef",
      "workspaceName": "repo",
      "workspace_cwd": "C:/repo",
      "workspace_visible": false,
      "no_workspace": false,
      "title": "Investigate search",
      "summary": "...",
      "updated_at": "2026-08-20T01:02:03Z"
    }
  ],
  "errors": [],
  "progress": {
    "scanned_projects": 120,
    "total_projects": 15864,
    "generation": 121,
    "complete": false,
    "paused": false
  },
  "next_cursor": null
}
```

The search DTO is limited to identity, display title/summary/time, workspace
navigation fields, and `active`/`busy`. It intentionally omits drafts, todos,
permission state, token usage, transcripts, and other full-session fields.
Clients should render every successful partial page immediately and poll again
while `progress.complete` is false. Once complete, `next_cursor` loads another
stable page. A cursor from an older generation returns
`409 {"error":"SESSION_SEARCH_CURSOR_STALE"}`; restart from the first page.

Workspace visibility and even the presence of `workspace.json` do not control
session inclusion. Workspace hash, name, cwd, and visibility are result
attributes. `no_workspace` sessions use an empty workspace hash. Archived
sessions and records with a non-empty `parent_session_id` are excluded. A
project-level read failure is reported in `errors` without suppressing valid
sessions from other projects.

`GET /api/session-search/user-messages?q=<query>&limit=<1..100>&request_id=<id>`
uses the same global boundary and existing per-project derived indexes. One
call advances only a small project batch with a short wall-clock budget. The
client repeats the call until `progress.complete` and renders accumulated
matches after every response:

```json
{
  "search_match": {
    "kind": "user_message",
    "score": 1200,
    "message_ordinal": 7,
    "snippet": "matching visible user text",
    "attachments": ["design.pdf"]
  }
}
```

The top-level response is:

```json
{
  "matches": [],
  "errors": [],
  "progress": {
    "scanned_projects": 32,
    "total_projects": 15864,
    "complete": false
  }
}
```

Only visible user-message text and attachment names are indexed; full
transcripts, hidden context, assistant messages, and tool results are not
returned. Empty queries return no matches, queries longer than 512 bytes
return `400`, and one failed project index does not block other projects.

`POST /api/session-search/requests/:request_id/cancel` is idempotent. It marks
all metadata/content work for that id cancelled, interrupts SQLite work at its
progress boundary, stops metadata/message enumeration at file or message
boundaries, and pauses catalog prewarming when no other search is attached.
Completed per-project shards remain cached, so a later search with a new id
resumes rather than starting over. A late content poll for a cancelled id
returns `409 {"error":"SESSION_SEARCH_CANCELLED"}`. UI close, Escape, backdrop,
query replacement, and unmount should abort the local HTTP request and call
this endpoint independently.

### Compatibility session routes

The following routes operate on the daemon compatibility workspace:

- `GET /api/sessions?archived=1`
- `POST /api/sessions`
- `POST /api/sessions/:id/resume`
- `DELETE /api/sessions/:id?purge=1`
- `PUT /api/sessions/:id/archive`
- `DELETE /api/sessions/:id/archive`
- `PUT /api/sessions/:id/title`
- `GET /api/sessions/:id/draft`
- `PUT /api/sessions/:id/draft`
- `DELETE /api/sessions/:id/todos`

`GET /api/sessions` returns `SessionSummary[]`, not a wrapper object. Both this
route and the workspace-scoped list read session metadata only; they do not
open JSONL transcripts to backfill legacy summary or count fields.

Sub-agent sessions (created by the `spawn_subagent` tool; their meta carries a
persisted `parent_session_id`) are excluded from session lists by default so
they never appear in the sidebar or the global search. Query them explicitly:

- `GET /api/sessions?parent=<session_id>` — only the sub-agent sessions spawned
  by that parent (background-tasks panel data source). The active part of the
  merge skips workspace filtering; the disk part still scans the requested
  workspace's project directory.
- `GET /api/workspaces/:hash/sessions?parent=<session_id>` — same semantics on
  the workspace-scoped route.

`SessionSummary` includes a `parent_session_id` field (empty string for normal
sessions).

Sessions created directly by the LOOP scheduler additionally include persisted
provenance:

```json
{"loop_execution":{"loop_id":"loop-id","run_id":"run-id"}}
```

The field is returned for active and inactive sessions and survives daemon
restart. Manually forked sessions do not inherit it.

The compatibility `POST /api/sessions` response includes:

```json
{
  "session_id": "sid",
  "id": "sid",
  "workspace_hash": "abc123",
  "cwd": "C:/repo",
  "no_workspace": false
}
```

Session create/resume failures that escape the registry as a generic
`std::exception` (for example a filesystem or encoding error while resolving
the workspace directory) return `500` with a JSON body instead of an empty
Crow error page, on both the compatibility and the workspace-scoped routes:

```json
{"error":"SESSION_CREATE_FAILED","message":"<exception text>","cwd":"E:/repo"}
```

Resume uses `"error":"SESSION_RESUME_FAILED"`. Invalid expert bindings keep
returning `400 {"error":"INVALID_EXPERT"}`. Any other route handler that lets an
exception escape returns `500 {"error":"INTERNAL_ERROR","message":"<exception
text>"}`; the same text is written to the daemon log as an `ERR` line, so a
bare `500 Internal Server Error` body no longer occurs for daemon-side
exceptions.

These responses follow the same loopback CORS policy as successful requests,
including errors handled by the global exception handler. A supported loopback
`Origin` receives `Access-Control-Allow-Origin` on the JSON error response;
cross-origin requests still require the daemon token.

On Windows, directory model settings saved by older versions remain readable
when the canonical UTF-8 setting is absent. An existing canonical file takes
priority even if malformed. New saves use the canonical location, while explicit
removal clears both canonical and applicable legacy copies so an old model
choice cannot become active again through compatibility lookup.

### Model-facing thread and workspace tools

Daemon, TUI, and headless runtimes expose the same in-process thread and
workspace tools to the model. They reuse the session/workspace registries and
storage directly; they do not call the daemon HTTP API:

| Tool | Behavior |
|---|---|
| `create_thread` | create a background thread and queue its initial prompt |
| `fork_thread` | fork completed persisted history into a new thread |
| `list_threads` | globally list pinned threads plus cursor-paginated recent threads |
| `read_thread` | read bounded, cursor-paginated turns from any workspace |
| `send_message_to_thread` | queue a follow-up prompt |
| `wait_threads` | wait for up to eight targets across workspaces using event cursors |
| `set_thread_title` | rename a thread |
| `set_thread_pinned` | update the existing pinned-session state |
| `set_thread_archived` | archive or unarchive a thread |
| `delete_thread` | permanently delete a thread and all descendants |
| `repair_thread` | append a deterministic repair checkpoint to another thread |
| `create_workspace` | register an existing absolute directory as a visible workspace |

`list_threads`, `read_thread`, and `wait_threads` discover sessions across the
entire ACECode data directory, including hidden/unregistered workspaces and
workspace-free sessions. A caller cwd is not required. List rows include
`workspaceHash` (the storage project hash, also for workspace-free sessions),
`cwd`, `noWorkspace`, `workspaceName`, and `workspaceVisible` as metadata.
Child sessions retain `parentThreadId`. The list excludes archived sessions
unless `includeArchived:true`; explicit reads can access archived sessions.
All non-archived pins are returned in `pinnedThreads`, ordered by project hash
and then that project's persisted pin order. Only `threads` is subject to
`limit` (default 20, maximum 50). Pass `nextCursor` back as `cursor` to read
the next page; it is an offset into the current ordering, not a frozen snapshot.
`errors` reports incomplete project scans. `read_thread` and each `wait_threads`
target accept optional `workspaceHash` from the list for direct lookup and
disambiguation when the same `threadId` exists in multiple projects.
Creation and mutation tools retain their existing calling-workspace semantics.

`delete_thread` also removes search-index and pin records. It may target its
calling thread, including a cascade whose tree contains the caller. In that
case it records the canonical tool result and turn timing, emits the terminal
turn events, stops any later write tool calls in the same response, and only
then tears down and purges the calling thread. The returned payload includes
`"scheduled":true`; no later model turn runs in the deleted thread.

`repair_thread` does not invoke a model, replay tools, or rewrite visible
transcript rows. It reconstructs provider history, repairs malformed
tool-call/result structure, and prunes only complete old user turn groups while
preserving the current input.

`create_workspace({"path":"C:/repo"})` accepts only an existing absolute
directory, resolves it to a canonical path, and idempotently persists the same
visible workspace record used by `POST /api/workspaces`. It returns
`{"hash","cwd","name","available":true}`. It never creates the target
directory, switches the calling thread's cwd, or starts a thread/worktree.

Separately, an explicit pre-output provider context-overflow error triggers a
finite recovery sequence inside `AgentLoop`: one history-repair retry, then one
retry with an emergency request profile, then a terminal error. Partial model
output is never replayed, and ordinary rate-limit, server, timeout, and network
errors do not enter this recovery sequence.

### `DELETE /api/sessions/:id`

Destroys an active in-memory session: aborts the current turn, joins the worker
thread, and removes it from the registry. It does not delete disk history.
Returns `204`; returns `503` when the session client is unavailable.

`DELETE /api/sessions/:id?purge=1` performs the same durable cleanup for either
an archived main session or a sub-agent session. It remains the background-task
"clear" action for sub-agents and is also the compatibility fallback used by
the archived-session settings page. Guard rails:

- `400 {"error":"only subagent sessions can be purged"}` for a non-archived
  main session
- `409 {"error":"session is busy; abort it first"}` while the target is
  running a turn
- `404` for a missing session, `500` for incomplete durable cleanup, and `503`
  when the session client is unavailable

### Archive, title, draft, and todos

Workspace-scoped and compatibility paths share the same behavior:

| Method | Path shape | Body | Response |
|---|---|---|---|
| PUT | `.../sessions/:id/archive` | ignored | updated `SessionSummary` |
| DELETE | `.../sessions/:id/archive` | none | updated `SessionSummary` |
| PUT | `.../sessions/:id/title` | `{"title":"..."}` | updated `SessionSummary` |
| GET | `.../sessions/:id/draft` | none | `{"session_id","id","text"}` |
| PUT | `.../sessions/:id/draft` | `{"text":"...","composer_content":{...}}` (content optional) | `{"session_id","id","text","composer_content"?}` |
| DELETE | `.../sessions/:id/todos` | none | `{"session_id","id","workspace_hash","todos":[],"todo_summary":{...}}` |

Title writes trim whitespace and validate with `sanitize_title`.

A title write always reaches both representations of the session. The persisted
meta is updated, and a copy of the same session that is live in this daemon also
gets the new title even when it did not match the request's workspace (a
no-workspace session never matches one). Across daemons — Desktop runs one per
workspace and only the active one serves the UI — the write only lands on disk,
so a persisted `title_source` of `user`/`user-cleared` outranks a stale live
title, including an older manual title: session listings report the persisted
rename, and the daemon holding the stale copy adopts it instead of writing it
back on its next meta write. Only an explicit local title write that is
currently being committed outranks the disk snapshot.

### `GET /api/sessions/:id/messages?since=N`

When `since=0` or omitted, returns a full snapshot object:

```json
{
  "events": [],
  "messages": [],
  "busy": false,
  "active_turn_id": "",
  "turn_count": 4,
  "permission_mode": "default",
  "token_usage": null,
  "session_token_usage": null,
  "todos": [],
  "todo_summary": {},
  "goal": null
}
```

Hidden file checkpoints, compact checkpoints, and hidden goal context messages
are filtered from `messages`.

Compact checkpoints are append-only. Version 2 records the Codex-shaped
replacement model history together with `window_number`, `first_window_id`,
`previous_window_id`, and `window_id`. Resume and fork start from the newest
valid checkpoint and replay only its suffix. A fork preserves the inherited
replacement history but resets its latest inherited checkpoint to a fresh
UUIDv7 window zero, so later compactions form a fork-local chain. Version 1
checkpoints without window metadata remain readable. Visible transcript rows
before compaction are not removed.

Pre-turn automatic compaction estimates the pending user input for threshold
purposes but compacts only already-recorded model history; the input is appended
exactly once after the compact attempt. Normal request construction keeps mutable
session/time/CWD, hook, plan, and todo context as separate user-role items and
inserts them before the last real user message (or fallback summary), so it never
rewrites the compact summary prefix or trails a mid-turn summary.

Non-streaming provider failures carry structured `ProviderErrorInfo`. Retryable
non-context failures keep the same immutable compaction request and retry
without a count limit. Local delays start at one second, double to a maximum of
twenty minutes, honor valid `Retry-After` guidance up to the same cap, and wake
promptly on cancellation. History trimming is reserved for explicitly
classified context-window overflow and resets the transient backoff sequence;
a generic HTTP 413 or ambiguous payload-size message is terminal and does not
delete items.

When `since>0`, returns an event array directly:

```json
[
  {"type":"message","seq":43,"timestamp_ms":1783152000000,"payload":{}}
]
```

If the requested sequence predates the in-memory replay ring, the array can be
empty. The frontend should fall back to `since=0`.

### `GET /api/sessions/:id/trajectory`

Returns model-invisible diagnostic records for exactly one session. The route
uses the normal daemon authentication and session/workspace scope checks. Pass
`workspace=<hash>` for a workspace session when its id is not globally unique;
omit it for a no-workspace session.

Query parameters:

| Name | Default | Meaning |
|---|---:|---|
| `after` | `0` | return recorded events whose monotonic `sequence` is greater than this cursor |
| `legacy_after` | `0` | offset into confirmed facts projected from the canonical transcript |
| `limit` | `250` | page size for each source, clamped to `1..1000` |
| `workspace` | empty | optional workspace hash scope |

```json
{
  "schema_version": 1,
  "session_id": "20260815-113921-a520",
  "workspace_hash": "...",
  "no_workspace": false,
  "source": "mixed",
  "records": [
    {
      "schema_version": 1,
      "sequence": 42,
      "timestamp_ms": 1786783204730,
      "type": "tool_end",
      "source": "recorded",
      "payload": {}
    }
  ],
  "next_after": 42,
  "legacy_next_after": 3,
  "has_more": false,
  "recorded_has_more": false,
  "legacy_has_more": false,
  "legacy_total": 3,
  "missing_capabilities": ["ttft", "tool_timing"],
  "diagnostics": {
    "malformed_complete_records": 0,
    "ignored_partial_tail": false,
    "recovered_unterminated_record": false
  }
}
```

`source` is `recorded`, `legacy`, `mixed`, or `empty`. Recorded events come
from `<project_dir>/<session_id>/trajectory.jsonl`; legacy records have
`sequence: null`, a `legacy_index`, and never infer missing timestamps,
request payloads, TTFT, tool schemas, or tool durations. `missing_capabilities`
names those unavailable facts. The two cursors are independent and must both
be retained by paged or polling clients. Malformed complete JSONL records and
an incomplete crash tail are skipped and reported in `diagnostics` without
affecting the canonical transcript.

### `POST /api/sessions/:id/export-markdown`

Exports the current session's visible transcript as a UTF-8 Markdown file. This
endpoint is available only when the desktop native save-file picker is enabled.
After resolving the session, the daemon opens the platform Save As dialog with
a filesystem-safe `<session-title>.md` filename prefilled. The user can edit the
filename and destination; the daemon writes to the confirmed full path. The
optional body identifies the workspace when the session id is not globally
unique:

```json
{
  "workspace_hash": "..."
}
```

On success, the response is:

```json
{
  "ok": true,
  "cancelled": false,
  "filename": "session-title.md"
}
```

If the user cancels Save As, the response is `{"ok":true,"cancelled":true}` and
no file is created. Native dialogs provide their normal overwrite confirmation;
paths returned without an extension receive `.md`. Hidden goal context, compact
checkpoints, and file checkpoints are excluded from the export. The endpoint
does not mutate the session. Errors use `400` for invalid JSON or destination
paths, `404` for unknown sessions/workspaces, `501` when the native picker is
unavailable, `503` when its callback is unavailable, and `500` for picker, file
creation, or write failures.

### Ordered composer content

Messages, turn steering/interruption and session draft writes accept optional
`composer_content`. Its editor-independent version 1 schema preserves the order
of text and references:

```json
{"version":1,"parts":[
  {"type":"text","text":"Use "},
  {"type":"skill","name":"review","token":"[$review](C:/skills/review/SKILL.md)","path":"C:/skills/review/SKILL.md"},
  {"type":"text","text":" with "},
  {"type":"attachment","key":"local-1","id":"att-...","name":"notes.txt","kind":"file"},
  {"type":"text","text":" and "},
  {"type":"path","path":"src/main.cpp","token":"@src/main.cpp","directory":false}
]}
```

Required fields are shown above except `skill.path` and `path.directory`, which
are optional. Attachments may also carry optional `mime_type` and `path` strings.
`key` identifies an occurrence across upload reconciliation; `id` identifies the
uploaded resource. Draft placeholders may omit `id`; submitted messages must
include each attachment's ID in the regular `attachments` payload, and the daemon
must successfully load that record from the target session. Display name, kind,
MIME and path are then hydrated from those verified records. Client preview URLs
and unknown fields are stripped.

The limit is 4096 parts and 2 MiB of declared string fields. Path/token fields
allow 64 KiB, names 16 KiB, MIME 1024 bytes, kind 64 bytes and key/id 256 bytes.
Unknown versions/types, invalid field types or unresolved attachment identities
return HTTP 400. Canonical text concatenates text parts and path/skill tokens;
attachment parts contribute no text. Messages normally derive their text from
this structure; the compatibility `text` is retained when `session_references`
requires its existing display projection. Skills activate through the existing
explicit-mention mechanism; their visual order does not define execution order.

The sanitized structure is persisted as `metadata.composer_content` and returned
in live message events and history. Provider `content_parts` retain their existing
contract. Legacy messages and clients without this field remain supported.

Draft GET/PUT responses return optional `composer_content`; PUT derives its text
from the content and saves both atomically. A text-only PUT or explicit null
clears the structured draft. Forking a structured user prompt returns
`restored_composer_content` plus `restored_attachments`, copies referenced uploads
into the new session with new IDs, and saves the restored structured draft.
Structured attachment references in the retained history are also copied and
remapped, including their provider content parts and preview records; recalling
those messages does not depend on the source session's attachment storage.

### `POST /api/sessions/:id/messages/retry`

Retries the trailing user message, or the last user message of a manually
stopped turn, in an idle live session. Body:

```json
{"expected_user_message_id":"persisted-user-message-id"}
```

The request accepts only this field. The backend verifies the full visible
transcript and model history end with that user message, with no active or
queued work. It checks again when the worker starts. Hidden bookkeeping
records do not count as transcript messages; assistant (including empty
messages), tool, system and error messages prevent ordinary retry.

A completed manual stop persists a transcript-only system message with
`metadata.user_aborted: true` and `metadata.retry_user_message_id`. If that
marker is the final visible transcript entry, the backend may retry its
specified last user message even after partial assistant output or tool
results. A stop request alone, an interjection, or plain interruption text
does not grant this exception. Later visible events invalidate it.
Already streamed text is saved as an assistant message with
`metadata.transcript_only: true` and `metadata.interrupted_output: true`, so
history reloads retain it without sending incomplete output/tool calls back
to the provider.

The original message, attachments and context are reused. If model history
still ends with that user, no user record is appended. If an aborted turn
already has assistant/tool records, they are preserved and the original
structured user content is appended with a new identity, without expanding
skills again or creating adjacent user messages. The stop marker is never
sent to the model. Returns `202 {"queued":true,"user_message_id":"..."}`;
malformed requests return `400`, and unavailable sessions, stale message IDs
or active/queued work return `409`. This endpoint does not create or resume a
session, expand commands, or accept new input.

### `POST /api/sessions/:id/messages`

Queues a user input turn. Body:

```json
{
  "text": "Explain this code",
  "client_message_id": "queued-session-id-1",
  "attachments": [{"id":"att-..."}],
  "contexts": [
    {
      "type": "selection",
      "label": "README.md:23-24",
      "text": "selected text",
      "source": {
        "path": "C:/repo/README.md",
        "start_line": 23,
        "end_line": 24,
        "line_count": 2
      }
    }
  ]
}
```

`attachments` may contain strings or objects with an `id` field. `contexts`
with `type:"selection"` are sanitized and expanded into model-visible context
while preserving the user's original display text. Other context objects are
passed as browser context content parts.

`client_message_id` is an optional non-empty string (maximum 256 bytes) used by
Desktop queued-input handoff. When accepted, it is preserved as
`metadata.client_message_id` on the canonical user message so an optimistic
local item can reconcile with persistence and WebSocket replay. It does not
deduplicate backend execution; callers that omit it retain the existing behavior.

If the text is a skill slash command for the session workspace, the daemon
expands it to the skill invocation prompt and records `metadata.display_text`.
Returns `202 {"queued":true}`.

### `POST /api/sessions/:id/turn/steer`

Appends structured user input to the currently running regular agent turn.
The request accepts the same `text`, `attachments`, `contexts`, and optional
`client_message_id` fields as the ordinary messages endpoint, plus the required
turn identity:

```json
{
  "text": "Keep the public API stable",
  "client_message_id": "queued-session-id-1",
  "expected_turn_id": "initial-user-message-uuid"
}
```

The expected id must equal `active_turn_id` from a session summary, the initial
messages snapshot, or the latest `busy_changed` event. The equality check and
FIFO enqueue happen atomically. Accepted input is committed as a normal visible
user message at the next model-request boundary, remains in the same busy turn,
and preserves structured content and `client_message_id`.

Acceptance is not the durable commit acknowledgement. A successful request
returns `202`:

```json
{
  "accepted": true,
  "turn_id": "initial-user-message-uuid",
  "client_message_id": "queued-session-id-1"
}
```

Clients should keep pending UI until the canonical user `message` event with
the matching `metadata.client_message_id` arrives. If the turn terminates
before commit, its terminal busy event lets the client restore that pending
input.

Errors use structured codes: `400 EXPECTED_TURN_ID_REQUIRED` or
`INVALID_TURN_INPUT`, `404 UNKNOWN_SESSION`, `409 NO_ACTIVE_TURN`,
`TURN_NOT_STEERABLE`, or `TURN_MISMATCH`, and
`429 TURN_STEER_QUEUE_FULL`. A mismatch response includes the current
`active_turn_id`.

### `POST /api/sessions/:id/turn/interrupt`

Interrupts the matching active regular turn and atomically promises the input
as a new high-priority regular turn. The request body and validation rules are
the same as `/turn/steer`, including required `expected_turn_id` and optional
structured attachments, contexts, and `client_message_id`:

```json
{
  "text": "Stop and use the new constraint now",
  "client_message_id": "queued-session-id-2",
  "expected_turn_id": "initial-user-message-uuid"
}
```

Unlike soft steer, acceptance sets the current provider/tool abort flag instead
of waiting for the current response to finish. The server queues the replacement
before requesting abort and runs it ahead of ordinary queued worker tasks. Thus,
if acceptance races natural completion, the operation is either rejected with
no new message or accepted with exactly one future user turn; the client never
needs to resend it after `busy=false`.

Success returns `202` after both the replacement turn and interrupt request have
been committed in memory:

```json
{
  "accepted": true,
  "interrupting": true,
  "turn_id": "initial-user-message-uuid",
  "client_message_id": "queued-session-id-2"
}
```

The old turn finishes with outcome `aborted`, emits a visible system notice
`[Interjected]` (`metadata.turn_interrupt=true`), records a hidden model-visible
`<turn_aborted>` marker, and does not pause an active goal. Manual stop still
uses `[Interrupted]`. The replacement user message is the durable acknowledgement
and carries the matching `metadata.client_message_id`. Pending UI should remain
in an interrupting state across the short old-turn `busy=false` transition until
that message arrives.

The endpoint uses the same structured error codes as `/turn/steer`. Existing
clients that want same-turn delivery at the next model boundary should continue
using `/turn/steer`; Desktop/Web interjection and `/turn` use this immediate
endpoint.

### `POST /api/sessions/:id/questions/interject`

Resolves a pending `question_request` with a free-form user message instead of
an answer. This is what Desktop/Web sends when the user types into the composer
while an AskUserQuestion picker is open, and what the remote-control channel
sends for plain (non-`/aq`) text while a question is pending. The body is the
`/turn/steer` shape plus the required `request_id` of the pending question;
`expected_turn_id` is optional and validated when present:

```json
{
  "text": "Neither, use the built-in client",
  "request_id": "question-request-uuid",
  "client_message_id": "queued-session-id-3",
  "expected_turn_id": "initial-user-message-uuid"
}
```

Acceptance is atomic with the question: the question closes with
`question_closed` `reason:"interjected"`, its tool result becomes a successful
`[User interjected] …` message telling the model the user's real instruction
follows, and the input is committed as a same-turn visible user message
immediately after that tool result (`metadata.turn_steer=true`,
`metadata.question_interjection=true`, `metadata.question_request_id`). The
turn is not aborted and no replacement turn is created; the model simply
continues from the interjection at the next model boundary. Success returns
`202`:

```json
{
  "accepted": true,
  "turn_id": "initial-user-message-uuid",
  "request_id": "question-request-uuid",
  "client_message_id": "queued-session-id-3"
}
```

If the question is no longer pending (already answered from another client,
timed out, or closed), the endpoint returns `409 NO_PENDING_QUESTION` and the
input is **not** committed; clients should fall back to the ordinary send or
queue path. `400 QUESTION_REQUEST_ID_REQUIRED` is returned when `request_id`
is missing. All other codes match `/turn/steer`.

### `POST /api/sessions/:id/side-question`

Runs one isolated `/btw` or `/side` side question against the active session's
latest thread-safe provider-facing context snapshot. The registry primes this
snapshot when a new session is configured and refreshes it after persisted
history/worktree state is restored, so the endpoint can be used before the
session's first main provider request:

```json
{"question":"Why did the current approach choose a mutex?"}
```

The daemon makes exactly one call to the session's current model with an empty
tool list. It does not append the question or answer to the main agent history,
JSONL transcript, hooks, goals, event stream, or busy lifecycle. Success:

```json
{
  "question": "Why did the current approach choose a mutex?",
  "answer": "It protects the snapshot while the main worker publishes it."
}
```

Errors use structured codes:

- `400 INVALID_SIDE_QUESTION`: `question` is missing, empty, not a string, or
  exceeds 16,000 UTF-8 bytes.
- `404 UNKNOWN_SESSION`: the target session is not active.
- `409 SIDE_QUESTION_CONTEXT_NOT_READY`: the registry could not publish a safe
  provider-facing context snapshot (an internal lifecycle invariant failure,
  not a prompt for the user to send a main-chat message first).
- `503 SIDE_QUESTION_PROVIDER_UNAVAILABLE`: the session has no current model.
- `502 SIDE_QUESTION_FAILED`: the provider call failed, returned no answer, or
  attempted a tool call.

Optional `worktree` field for the **first** message of a session (openspec
`add-webui-git-session-pill`):

```json
{"text": "...", "worktree": {"create": true, "base": "master"}}
```

Before enqueuing, the daemon creates (or fast-resumes) a worktree named
`ses-<session id>` based on `base` (a local branch; empty
falls back to the default `origin/<default-branch>` baseline) via the same
machinery as the `EnterWorktree` tool, then switches the session cwd into it.
The accepted response exposes the working root immediately:

```json
{
  "queued": true,
  "worktree": {
    "name": "ses-session-id",
    "branch": "worktree-ses-session-id",
    "path": "C:/repo/.acecode/worktrees/ses-session-id"
  }
}
```

Ordinary message submissions without worktree creation keep returning
`202 {"queued":true}`.

Session storage location does not move. Failures abort the request without
enqueuing: `404` unknown session, `409` session busy, `400` session already
has messages / already in a worktree / invalid or missing base branch,
`500` git errors.

### Private streaming side chat over WebSocket

Web/Desktop floating side chat opens its own connection to the existing
`/ws/sessions/<id>?token=...` endpoint using the same authentication rules. This
connection does not send `hello` and does not subscribe to main-session events.
The synchronous HTTP endpoint and TUI `/btw` and `/side` remain single-turn.

Start a request with:

```json
{
  "type": "side_chat_start",
  "payload": {
    "session_id": "session-id",
    "request_id": "unique-request-id",
    "question": "How does that affect the next request?",
    "history": [
      {"role": "user", "content": "Why use a mutex?"},
      {"role": "assistant", "content": "It protects the context snapshot."}
    ]
  }
}
```

`request_id` must contain 1–128 UTF-8 bytes; `question` is limited to 16,000
UTF-8 bytes. Optional `history` contains at most 200 messages and 256 KiB of
combined content, alternating nonempty text-only `user`/`assistant` pairs.
No tool, system, attachment, or additional message fields are accepted.
Clients include successful answers and nonempty stopped answers in subsequent
history; failed or empty stopped turns stay local to the floating transcript.

The model receives the latest safe main context snapshot, detached side-chat
instructions, the supplied side history, and the new question, with no tools.
Side messages do not mutate main history, transcript, hooks, goals, busy state,
or the session event stream. Responses go only to the requesting connection:

```json
{"type":"side_chat_delta","payload":{"request_id":"unique-request-id","delta":"Text fragment"}}
{"type":"side_chat_reset","payload":{"request_id":"unique-request-id"}}
{"type":"side_chat_done","payload":{"request_id":"unique-request-id","answer":"Complete or stopped partial answer","cancelled":false}}
{"type":"side_chat_error","payload":{"request_id":"unique-request-id","code":"SIDE_CHAT_FAILED","message":"Provider error"}}
```

`side_chat_reset` clears provisional answer text before a provider retry.
`side_chat_done` includes the authoritative accumulated answer. On error,
clients retain already received text for display but exclude the failed turn
from follow-up context.

Send `{"type":"side_chat_stop","payload":{"request_id":"unique-request-id"}}`
to cancel only that side request, including its retry backoff. The terminal
`side_chat_done` then has `cancelled: true` and preserves generated text.
Closing the connection also cancels the request. At most one side request may
run per connection; another start returns `SIDE_CHAT_BUSY`. Stop messages with
an unmatched request ID are harmless. Clients should wait for the terminal
frame before sending another turn or close the finished connection.

Other structured errors include `INVALID_SIDE_CHAT`, `UNKNOWN_SESSION`,
`SESSION_REGISTRY_UNAVAILABLE`, `SIDE_CHAT_UNAVAILABLE`,
`SIDE_QUESTION_CONTEXT_NOT_READY`, and `SIDE_QUESTION_PROVIDER_UNAVAILABLE`.
`SIDE_CHAT_PROVIDER_UNSUPPORTED` means the current provider cannot guarantee
tool-free calls. In particular, the native Codex app-server provider owns its
own tool runtime and is rejected before invocation; selecting a supported
model enables side chat. Its main-session behavior remains unchanged.
An older daemon returns its ordinary unknown-message error; clients must show
that error and release the composer instead of simulating streaming.

### `POST /api/sessions/:id/attachments`

Uploads bytes into the session attachment store. Body:

```json
{
  "name": "screenshot.png",
  "mime_type": "image/png",
  "data_base64": "..."
}
```

Desktop may instead create a metadata-only reference for an ordinary local
file. The server canonicalizes `source_path`, verifies that it is a regular
non-image file, and records its actual size without reading or copying its
bytes. There is no 25 MiB snapshot limit for this form:

```json
{
  "name": "large.pdf",
  "mime_type": "application/pdf",
  "source_path": "C:/docs/large.pdf",
  "reference_only": true
}
```

Returns `201`:

```json
{
  "attachment": {
    "id": "att-...",
    "session_id": "sid",
    "name": "screenshot.png",
    "kind": "image",
    "mime_type": "image/png",
    "path": "...",
    "blob_url": "/api/sessions/sid/attachments/att-.../blob",
    "size_bytes": 12345
  }
}
```

For a source reference, `path` and `blob_url` are empty, and `metadata`
contains the canonical `source_path` plus `"storage":"source_reference"`.
Raster images cannot use `reference_only`; they continue through the snapshot
upload and image-normalization path.

### `GET /api/sessions/:id/attachments/:attachment_id/blob`

Returns raw attachment bytes with the stored MIME type and
`Cache-Control: private, max-age=3600`. Metadata-only source references have
no blob and return `404` from this endpoint.

### `POST /api/sessions/:id/commands`

Runs daemon-owned builtin slash commands. Body:

```json
{"command":"compact","args":"","display_text":"/compact"}
```

`command` can also be slash text like `"/compact"`. Supported commands are
the daemon builtin commands accepted by `parse_builtin_command_request`:
`init`, `compact`, `goal`, and `plan`. Skill slash commands must use
`POST /api/sessions/:id/messages`.

Returns `202 {"queued":true,"command":"compact"}`. Errors:

- `400 {"error":"unsupported command","command":"..."}`
- `404 {"error":"unknown session"}`
- `500 {"error":"command failed"}`

### `PUT /api/sessions/:id/expert`

Switches the selected expert component for the active session without creating
or navigating to another conversation. Body:

```json
{"expert_id":"reviewer","draft_text":"Please review the active change"}
```

The expert is resolved against the session workspace. Its prompt context and
component-scoped Skills are queued on the same worker as chat turns. An
in-flight turn therefore finishes with its existing expert; the switch is
persisted and applies before the next subsequently queued turn. `draft_text`
is optional. When present (including an empty string), the expert binding and
composer draft are persisted at that same queue boundary; omitting it preserves
the current draft.

Success returns the selected expert definition plus queue state:

```json
{
  "expert": {"id":"reviewer","display_name":"Code Reviewer"},
  "accepted": true,
  "queued": false,
  "pending": false,
  "busy": false,
  "applied": true,
  "effective_boundary": "applied",
  "control_sequence": 17,
  "receipt": {
    "sequence": 17,
    "expert_id": "reviewer",
    "state": "applied",
    "applied": true,
    "effective_boundary": "applied"
  },
  "draft_text": "Please review the active change"
}
```

For compatibility the selected expert's fields are also present at the top
level. `busy` is derived from the authoritative worker-queue receipt, not only
from the loop's transient busy flag. When a turn was already active or queued,
`pending` and `queued` are true, `applied` is false, and
`effective_boundary` is `next_turn`; that turn keeps its old prompt, Skill
registry, MCP scope, built-in-tool scope, and draft. An idle switch normally
returns `applied`; `queued_control` means the control was accepted but did not
finish within the bounded synchronous wait. All expert contexts and an
optional draft change together before any later queued chat turn. A control
callback that ran but failed to persist is not reported as applied.

Errors:

- `400` when `expert_id` is missing, `draft_text` is not a string, or the
  component is unavailable.
- `404` when the active session does not exist.
- `500` when the expert Skill context cannot be prepared or the atomic
  binding/draft update cannot be persisted.

### `DELETE /api/sessions/:id/expert`

Clears the active session's persisted expert ID and the expert chip shown by
real composers:

```json
{"detached":true,"expert_id":"","context_retained":true}
```

This is deliberately a metadata-only detach. It does not enqueue an AgentLoop
control, rebuild the system prompt or capability registries, alter the current
provider context, or invalidate KV cache. Expert instructions and capability
state that were already loaded may therefore remain effective until another
normal lifecycle boundary. A detach also supersedes any older expert switch
still queued behind an in-flight turn, so that switch cannot restore the UI
binding afterward. The current composer draft is preserved.

Errors:

- `404` when the active session does not exist.
- `500` when the cleared binding cannot be persisted.

### `GET /api/sessions/:id/permissions`

Returns the active or persisted session permission mode:

```json
{"mode":"default","description":"Prompt for write/exec tools"}
```

### `PUT /api/sessions/:id/permissions`

Body:

```json
{"mode":"yolo"}
```

Canonical modes are `default`, `auto`, `plan`, and `yolo`. Legacy inputs
`accept-edits` and `acceptEdits` normalize to `auto` in configuration, persisted
session metadata, and responses. Switching to
`yolo` also resolves any open permission prompt with allow. Returns the same
shape as `GET`.

### `POST /api/sessions/:id/fork`

Copies a source session prefix into a new session and resumes it into the current
daemon. For a user message, the prefix ends before `at_message_id`; its plain text
is returned in `restored_prompt` for editing in the new session. For an assistant
message, the prefix includes the selected message and no prompt is returned.
It does not start a new turn. `fork_anchor_role` identifies the selected message's
role. Empty prompt text omits `restored_prompt`; attachments are not restored.

Body:

```json
{"at_message_id":"msg-123","title":"optional title"}
```

Response:

```json
{
  "session_id": "new-sid",
  "title": "Fork title",
  "forked_from": "source-sid",
  "fork_message_id": "msg-123",
  "fork_anchor_role": "user",
  "restored_prompt": "Original prompt to edit",
  "workspace_hash": "abc123",
  "cwd": "C:/repo",
  "no_workspace": false
}
```

### `POST /api/sessions/:id/file-checkpoints/:message_id/restore`

Restores workspace files to the checkpoint captured for that user turn. Chat
history is not rewound. Refuses while the session is busy.

Response:

```json
{
  "ok": true,
  "session_id": "sid",
  "message_id": "msg-123",
  "files_changed": 3,
  "errors": []
}
```

---

## 6. Opencode Import and Pins

### `GET /api/workspaces/:hash/opencode-import`

Previews importable opencode sessions:

```json
{
  "available": true,
  "count": 2,
  "source_database": "...",
  "error": "",
  "sessions": [
    {
      "id": "opencode-id",
      "title": "Title",
      "directory": "C:/repo",
      "provider": "openai",
      "model": "gpt-4.1",
      "archived": false,
      "time_created_ms": 0,
      "time_updated_ms": 0,
      "time_archived_ms": 0,
      "message_count": 10,
      "part_count": 20,
      "source_database": "..."
    }
  ]
}
```

### `POST /api/workspaces/:hash/opencode-import`

Starts an async import. Body is optional:

```json
{"session_ids":["opencode-id-1","opencode-id-2"]}
```

Returns `202` job status:

```json
{
  "job_id": "job-id",
  "workspace_hash": "abc123",
  "state": "pending",
  "imported": 0,
  "total": 2,
  "failed": 0,
  "skipped": 0,
  "current_title": "",
  "error": "",
  "session_ids": []
}
```

### `GET /api/workspaces/:hash/opencode-import/:job_id`

Polls the same status object. Returns `404` for unknown workspace or job.

### `GET /api/workspaces/:hash/pinned-sessions`

Returns:

```json
{"workspace_hash":"abc123","cwd":"C:/repo","session_ids":["sid-1"]}
```

The daemon prunes ids that no longer exist or are archived.

### `PUT /api/workspaces/:hash/pinned-sessions`

Body:

```json
{"session_ids":["sid-1","sid-2"]}
```

Normalizes, prunes, persists, and echoes the same shape as `GET`.

### `GET /api/no-workspace/pinned-sessions`

Returns the ordered pin state for main sessions created without a workspace:

```json
{
  "no_workspace": true,
  "pin_scope": "__no_workspace__",
  "session_ids": ["task-1"]
}
```

The state is stored under the no-workspace cache root. The daemon prunes ids
that are missing, archived, workspace-owned, or background child sessions.

### `PUT /api/no-workspace/pinned-sessions`

Body:

```json
{"session_ids":["task-1","task-2"]}
```

Normalizes, prunes, persists, and echoes the same shape as `GET`.

### `GET /api/pinned-sessions/order`

Returns the global ordering across registered workspaces and no-workspace
tasks. No-workspace items use the reserved `__no_workspace__` pin scope; this
value is not a workspace registration or a session `workspace_hash`:

```json
{
  "items": [
    {"workspace_hash":"__no_workspace__","session_id":"task-1"},
    {"workspace_hash":"abc123","session_id":"sid-1"}
  ]
}
```

### `PUT /api/pinned-sessions/order`

Body:

```json
{"items":[{"workspace_hash":"abc123","session_id":"sid-1"}]}
```

Normalizes, prunes unavailable pinned items, persists, and echoes the same
shape as `GET`.

---

## 7. Files

Directory listing validates `cwd` against the daemon cwd and registered
workspace cwds. Authenticated text and binary file-detail requests may resolve
any local file path readable by the daemon process: `cwd` must be absolute,
while `path` may be relative to `cwd` or an absolute target. The target is
canonicalized before the existing file checks run. This preview-only behavior
does not make an external parent directory listable via `/api/files` and does
not authorize Git or other workspace routes for that directory.

### `GET /api/files?cwd=<abs>&path=<rel>&show_hidden=1&show_noise=1`

Lists direct children:

```json
[
  {"name":"src","path":"src","kind":"directory","modified_ms":1783152000000},
  {"name":"README.md","path":"README.md","kind":"file","size":1234}
]
```

`show_hidden=1` includes dot-prefixed entries. `show_noise=1` additionally
includes directories normally hidden from the SidePanel tree such as `.git`,
`node_modules`, `build`, and `target`; the TUI/Desktop `@` path-reference picker
uses both flags so a bare `@` reflects the complete direct contents of the
current cwd. Enumeration remains non-recursive and all returned paths are still
canonicalized within the allowed cwd.

### `GET /api/files/content?cwd=<abs>&path=<rel-or-abs>`

Returns `text/plain; charset=utf-8` file content. Error status examples:

- `400` missing parameters or a non-absolute/invalid `cwd`
- `404` not found
- `415` binary or too large
- `500` IO error

For any workspace or no-workspace session, `cwd` may be the file's containing
directory and `path` its basename. A caller may also keep its current absolute
`cwd` and pass an absolute `path` elsewhere on the local machine. The endpoint
does not require either target to be a registered workspace, but still requires
daemon authentication and remains subject to the 5 MiB text cap, binary sniff,
filesystem permissions, and read-only response behavior.

### `GET /api/files/blob?cwd=<abs>&path=<rel-or-abs>`

Returns raw bytes for browser-native preview types:

- images: `png`, `jpg`, `jpeg`, `gif`, `webp`, `bmp`, `ico`, `svg`
- documents: `pdf`, `docx`, `xlsx`, `xlsm`, `pptx`

The route caps preview bytes at 20 MB and sets `X-Content-Type-Options:
nosniff`. It uses the same arbitrary-local-path resolution and authentication
boundary as the text-content endpoint; the external parent directory remains
unavailable to `/api/files` and Git routes.

### `GET /api/files/editable?cwd=<abs>&path=<rel-or-abs>`

Desktop-only authenticated read endpoint for the file editor. Unlike the
read-only `/api/files/content` preview route, both `cwd` and the resolved file
must stay inside a registered workspace (including its current worktree). The
target must be an existing regular text file no larger than 5 MiB.

The response returns normalized UTF-8/LF editor text plus the metadata needed
for a lossless save:

```json
{
  "text": "# Notes\n",
  "read_id": "...",
  "encoding": "utf-8",
  "line_ending": "crlf",
  "has_bom": true,
  "size": 12
}
```

### `PUT /api/files/editable`

Safely replaces the same Desktop workspace file. The JSON body is
`{"cwd":"<abs>","path":"<rel-or-abs>","text":"...","read_id":"..."}`.
The server revalidates the workspace boundary and current file bytes before
writing. If the file changed after the matching GET, it returns `409` without
overwriting either version. A successful write preserves the detected
encoding, BOM, and dominant line-ending style, uses the common safe-write
path, invalidates Git snapshots, and clears the Agent file-read baseline so a
later Agent edit must read the human-authored version first. This route is not
registered on standalone Web daemons.

### `GET /api/fs/roots`

Browsable roots for the Web path picker (openspec `add-web-path-picker`). The
picker is the browser-side replacement for the native folder and file dialogs
that only a Desktop-launched daemon can show. It browses the daemon host's own
filesystem, so every path below is a path on the machine running the daemon.

```json
{
  "host": "DESKTOP-SHAO",
  "os": "windows",
  "home": "C:/Users/shao",
  "roots": [
    {"path": "C:/", "label": "Windows", "drive_type": "fixed",
     "total_bytes": 511000000000, "free_bytes": 195000000000}
  ],
  "quick": [
    {"kind": "home", "path": "C:/Users/shao"},
    {"kind": "desktop", "path": "C:/Users/shao/Desktop"},
    {"kind": "projects", "path": "C:/Users/shao/.acecode/workspaces"}
  ],
  "workspaces": [
    {"hash": "0123456789abcdef", "name": "acecode", "path": "N:/Users/shao/acecode"}
  ]
}
```

`roots` lists every logical drive on Windows (`drive_type` is `fixed`,
`removable`, `remote`, `cdrom`, or `ramdisk`; remote drives carry no label or
capacity because probing a disconnected mapped drive can block), `/` on POSIX
(`drive_type: "root"`), plus the direct children of `/Volumes` on macOS
(`drive_type: "volume"`). `label`, `total_bytes`, and `free_bytes` are omitted
when unavailable. `quick` entries of kind `desktop` and `projects` appear only
when those directories exist. `os` is `windows`, `macos`, or `linux`. All
paths use forward slashes; drive roots keep their trailing slash (`C:/`).

### `GET /api/fs/list?path=<abs>&show_hidden=1`

Lists the direct children of any absolute directory readable by the daemon
process. Unlike `/api/files`, there is no workspace whitelist: any
authenticated client (loopback without a token, or a remote client with a valid
token) may browse the whole filesystem. The route is read-only and logs every
listing with its path.

```json
{
  "path": "C:/Users",
  "parent": "C:/",
  "truncated": false,
  "entries": [
    {"name": "shao", "path": "C:/Users/shao", "kind": "dir",
     "modified_ms": 1783152000000, "hidden": false, "link_target": "N:/Users/shao"},
    {"name": "desktop.ini", "path": "C:/Users/desktop.ini", "kind": "file",
     "size": 174, "modified_ms": 1783152000000, "hidden": true}
  ]
}
```

- `path` is the request path after lexical normalization only (`\` becomes
  `/`, `..` segments fold, the drive letter is upper-cased, a trailing slash is
  dropped except on a root). Junctions and symlinks are **not** resolved:
  browsing `C:/Users/shao` on a machine where that directory is a junction to
  `N:/Users/shao` keeps returning `C:/Users/shao/...` paths, matching what the
  native dialog returns so the same directory registers the same workspace
  hash.
- `parent` is empty when `path` is a drive or filesystem root.
- Entries are sorted directories first, then case-insensitively by name.
  Directory entries never carry `size`. `link_target` is present for junction
  or symlink directories whose target can be read. Noise directories such as
  `node_modules` or `build` are **not** filtered.
- Hidden entries (dot-prefixed, or carrying the Windows HIDDEN or SYSTEM
  attribute) are omitted unless `show_hidden=1`; when included they carry
  `hidden: true`.
- At most 5000 entries are returned; `truncated: true` marks a longer
  directory.

Errors: `400 {"error":"path must be absolute"}` for an empty or relative
`path`; `404 {"error":"not found"}` and `404 {"error":"not a directory"}`;
`403 {"error":"permission denied"}` when the daemon cannot read the directory;
`500 {"error":"io error"}` otherwise. Error bodies may add a `detail` string.

### `GET /api/git/info?cwd=<abs>`

Returns git repository info for a workspace cwd (openspec `add-git-context`).
`cwd` must be an allowed workspace path (same whitelist as `/api/files`);
unknown cwd yields `400 {"error":"unknown workspace"}`.

Non-repo cwd (or `config.git_context.enabled=false`):

```json
{"is_repo": false}
```

Git repository:

```json
{
  "is_repo": true,
  "branch": "master",
  "default_branch": "master",
  "default_base": "origin/master",
  "branches": ["dev", "master"],
  "remote_branches": ["origin/dev", "origin/master"],
  "dirty": false
}
```

- `branch` is `"HEAD"` when detached.
- `default_base` is the verified `origin/<default_branch>` ref, or an empty
  string when no such locally fetched remote-tracking ref exists.
- `branches` contains local branch short names. `remote_branches` contains
  locally available remote-tracking short names and omits symbolic aliases
  such as `origin/HEAD`; collecting either list performs no network fetch.
- `dirty` reflects tracked changes only (`status --porcelain -uno`);
  untracked files do not set it.
- All git subprocesses are read-only, use `--no-optional-locks`, and honor
  `config.git_context.timeout_ms` (timeout degrades to `{"is_repo": false}`
  semantics for the failing fields rather than erroring).

### `POST /api/git/checkout`

Body `{"cwd": "<abs>", "branch": "<local branch>", "stash": false}`
(openspec `add-webui-git-session-pill`). Safety gates, in order:

- `400` unknown workspace / git context disabled / invalid branch name
- `409 {"error":"busy"}` — any session of that workspace has a turn running;
  no git mutation happens
- `409 {"error":"dirty","files":[...]}` — tracked changes exist and `stash`
  is not true; the client should confirm with the user then retry with
  `stash: true`
- `stash: true` runs `git stash push --include-untracked -m "ACECode
  auto-stash"` before checkout (untracked files are preserved in the stash)
- `409 {"error":"checkout failed","detail":...}` — git refused (conflicts);
  stashed changes stay in the stash
- `200 {"ok":true,"branch":...}` — success; cached gitStatus prompt
  snapshots of that workspace's sessions are invalidated

### `GET /api/git/changes?cwd=<abs>&base=<ref>`

Working tree (including uncommitted changes) vs `base`
(openspec `redesign-sidepanel-git-changes`). `base` must be `HEAD` or an
allowlist-safe ref name that resolves via `rev-parse --verify`; anything else
is `400 {"error":"invalid base"}`.

```json
{
  "branch": "master",
  "base": "origin/master",
  "files": [
    {"path":"src/a.cpp","status":"M","additions":95,"deletions":36},
    {"path":"new.txt","status":"A"},
    {"path":"img.png","status":"M","binary":true}
  ],
  "total_additions": 314,
  "total_deletions": 47,
  "total_count": 5,
  "truncated": false
}
```

- Combines `diff --numstat` + `diff --name-status` (joined by path; renames
  reported at the new path) plus untracked files (status `A`, no counts).
- The list caps at 200 entries; `truncated: true` with the full
  `total_count` when exceeded. Totals always reflect the full diff.
- `504 {"error":"git timeout"}` when git exceeds 2× `git_context.timeout_ms`.

### `GET /api/git/diff?cwd=<abs>&path=<rel>&base=<ref>`

Single-file unified patch (`git diff <base> -- <path>`); untracked files get
a synthesized new-file patch via `diff --no-index`. `path` is canonicalized
and prefix-checked against `cwd` (`400` outside the workspace). Patches over
1 MB return `413 {"error":"diff too large"}`. Response: `{"patch": "..."}`.

---

## 8. Commands, Skills, and Hooks

### `GET /api/commands?workspace=<hash>`

Skill entries include `path` and `mention` (the canonical linked explicit-skill
reference) for stable inline selection; `/api/skills` includes the same fields
for filesystem-backed entries.

Returns builtin slash commands. A non-empty `workspace` hash also returns
project commands plus merged workspace/global skills. An explicitly empty
query (`/api/commands?workspace=`) represents no-workspace input and returns an
empty `commands` array plus enabled global skills only:

```json
{
  "builtins": [
    {"name":"init","description":"Analyze this codebase and generate (or improve) AGENT.md"},
    {"name":"compact","description":"Compress conversation history"},
    {"name":"goal","description":"Create, view, pause, resume, edit, or clear the thread goal"},
    {"name":"plan","description":"Enter plan mode or start planning a described task"}
  ],
  "skills": [
    {"name":"my-skill","description":"..."}
  ]
}
```

For backward compatibility, omitting the `workspace` query entirely returns
the builtin-only response and omits both `commands` and `skills`.

### Expert components

`GET /api/experts?workspace=<hash>` returns:

```json
{
  "experts": [
    {
      "id": "code-reviewer",
      "version": "1.0.0",
      "type": "agent",
      "display_name": "Code Reviewer",
      "profession": "Review engineer",
      "description": "Reviews changes before delivery.",
      "avatar_url": "/api/experts/code-reviewer/avatar?workspace=abc123",
      "state_avatar_urls": {
        "working": "/api/experts/code-reviewer/avatar?workspace=abc123&state=working",
        "needs_attention": "/api/experts/code-reviewer/avatar?workspace=abc123&state=needs_attention",
        "idle": "/api/experts/code-reviewer/avatar?workspace=abc123&state=idle"
      },
      "default_init_prompt": "Review the active change.",
      "tags": ["开发", "质量"],
      "expertise": ["架构审查", "回归风险"],
      "quick_prompts": ["Review this change", "Check tests"],
      "created_at": "2026-07-25T09:00:00Z",
      "updated_at": "2026-07-25T09:05:00Z",
      "capabilities": {
        "skills": ["review-checklist"],
        "mcp_servers": ["github"],
        "tools": ["file_read", "AskUserQuestion"]
      },
      "lead_agent_id": "lead",
      "member_agent_ids": [],
      "references_existing_experts": false,
      "lead_expert_id": "",
      "member_expert_ids": [],
      "agents": [
        {
          "id": "lead",
          "display_name": "Code Reviewer",
          "profession": "Review engineer"
        }
      ],
      "source": "global",
      "managed_global": true
    }
  ],
  "diagnostics": [],
  "workspace_hash": "...",
  "cwd": "C:/repo",
  "global_root": "C:/Users/me/.acecode/experts"
}
```

`GET /api/experts/:id?workspace=<hash>` returns the same definition and adds
the selected Agent's `instructions` plus a `state_avatars` object containing
only safe package-relative paths for the configured `working`,
`needs_attention`, and `idle` images. List responses intentionally omit
instructions and `state_avatars`. Neither response exposes the package root,
resolved avatar filesystem paths, or Skill-root paths. `avatar_url` is empty
when no main avatar is configured. `state_avatar_urls` contains each state
that has an effective image: the configured state image, or the main avatar
when that state is not configured.

`GET /api/experts/:id/avatar?workspace=<hash>` serves the main avatar.
Appending `&state=working`, `&state=needs_attention`, or `&state=idle` serves
that state image and falls back to the main avatar if the state file becomes
unavailable during the read. Unknown state names return
`400 INVALID_AVATAR_STATE`. The endpoint serves only PNG, JPEG, GIF, WebP,
BMP, or ICO images contained inside the resolved expert package (maximum
8 MiB), returns `404` for missing, escaped, unsupported, or oversized files,
and preserves GIF response bytes with `Content-Type: image/gif`.

`POST /api/experts?workspace=<hash>` creates a managed global component;
`PUT /api/experts/:id?workspace=<hash>` updates one. Both accept the fields
above using snake-case request names. A single expert supplies
`instructions` (or a `lead` object). A team supplies one
`lead_expert_id` and a non-empty `member_expert_ids` array referencing
installed single experts. Both types may supply `state_avatars` with any of
the three fixed state keys and existing package-relative image paths. Omitting
`state_avatars` preserves all state-avatar data; a supplied object
authoritatively replaces the three known keys, and an empty object clears
their references while retaining image files and unknown manifest extension
keys. Workspace-sourced packages are read-only through these routes. Updates
merge managed fields into the existing package and keep avatar configuration,
packaged Skills, resources, and unknown manifest fields, including unknown
nested Agent and `teamInfo` fields. The managed
capability keys are authoritative on update: an omitted key means inherit, an
empty array means allow none, and a non-empty array is an exact allowlist;
unknown keys under `capabilities` are preserved. The
`DELETE /api/experts/:id?workspace=<hash>` route removes only a managed global
package.

Creating an ID shadowed by a workspace package returns
`409 WORKSPACE_EXPERT_READ_ONLY`; creating an existing managed global ID
returns `409 EXPERT_ALREADY_EXISTS`. Updating or deleting a workspace-sourced
package also returns `409 WORKSPACE_EXPERT_READ_ONLY`.

Each of `capabilities.skills`, `capabilities.mcp_servers`, and
`capabilities.tools` is independently optional:

- missing key: inherit all capabilities available under global policy;
- empty array: allow none of that capability class;
- non-empty array: exact-name expert allowlist. For known installed Skills and
  configured MCP servers, this explicit list overrides the daemon-global
  allowed/disabled default.

Unknown or temporarily unavailable IDs remain persisted so the editor can
show the saved choice and its unavailable state. A referenced expert team
does not merge capability lists; every member executes under that member
expert's own scopes. The manifest's top-level `skills` field remains package
content metadata and is not the capability selection field. Expert precedence
does not bypass tool permission approval, permission/Plan/Dangerous mode,
sandboxing, credentials, or runtime availability, and it does not synthesize
an uninstalled Skill, unconfigured MCP server, or unregistered local tool.

`GET /api/experts/capabilities?workspace=<hash>` returns the read-only,
runtime-backed selection catalog:

```json
{
  "skills": [
    {
      "id": "review-checklist",
      "description": "Review checklist",
      "source": "project",
      "available": true,
      "globally_enabled": true,
      "default_enabled": true,
      "expert_selectable": true,
      "configurable": true,
      "status": "available",
      "disabled_reason": ""
    }
  ],
  "mcp_servers": [
    {
      "id": "github",
      "description": "",
      "transport": "stdio",
      "available": true,
      "globally_enabled": true,
      "default_enabled": true,
      "expert_selectable": true,
      "configurable": true,
      "runtime_available": true,
      "status": "connected",
      "disabled_reason": "",
      "tool_count": 3
    }
  ],
  "tools": [
    {
      "id": "file_write",
      "description": "Write a file",
      "available": true,
      "globally_enabled": true,
      "default_enabled": true,
      "expert_selectable": true,
      "status": "available",
      "disabled_reason": "",
      "configurable": true,
      "read_only": false
    }
  ]
}
```

`default_enabled` drives inherited checkbox state. `expert_selectable` may
remain true when `globally_enabled` and `available` are false, which lets an
expert explicitly enable a known Skill or configured MCP server. Dispatching
such an expert can start that MCP server without changing its global disabled
default; inheriting sessions continue to filter it out. MCP entries expose only
server ID, safe transport, runtime state, and tool count; command lines,
arguments, environment variables, URLs, headers, authorization tokens, and
connection error text are never returned. Tool IDs are exact registered
built-in names; MCP tools are selected by their exact owning server ID instead
of by parsing a qualified tool name.

### `GET /api/skills/root?workspace=<hash>`

Returns the effective skill directory:

```json
{
  "path": "C:/repo/.acecode/skills",
  "source": "project_acecode",
  "global_path": "C:/Users/me/.acecode/skills",
  "workspace_hash": "abc123",
  "cwd": "C:/repo"
}
```

`source` is `project_acecode`, `project_agent`, or `global_acecode`.
`global_path` always points at the user-global skills root
(`~/.acecode/skills`) regardless of which root was selected as `path`;
the settings page "open global skills directory" button relies on it.

### `GET /api/skills?workspace=<hash>`

Returns an array, not a wrapper:

```json
[
  {
    "name": "skill-name",
    "command_key": "/skill-name",
    "description": "...",
    "category": "custom",
    "enabled": true,
    "source": "project"
  }
]
```

The list is a full rescan of the workspace's project scan roots plus the
global scan roots, so disabled skills keep their real `description` and
`source`. `source` is `"project"` (discovered under the workspace's project
chain — `.acecode/skills` / `.agent/skills` walking up to, but not
including, HOME) or `"global"` (user-global roots and
`config.skills.external_dirs`). `workspace` is optional; without it the
daemon's compatibility workspace (its own cwd) is used.

Disabled config entries whose skill no longer exists on disk (ghost
entries) are still included with `enabled:false` and `source:""` so the UI
can release them from `config.skills.disabled`.

### `PUT /api/skills/:name?workspace=<hash>`

Body:

```json
{"enabled":false}
```

Returns `{"name":"skill-name","enabled":false}`.

`workspace` is optional and only affects the "known skill" validation: the
daemon's global registry only scans the daemon cwd's project chain, so
toggling a project skill that belongs to another workspace requires passing
that workspace's hash (the handler rescans that workspace's cwd to find the
skill). The disabled list itself is global config either way.

### `GET /api/skills/:name/body`

Returns `text/markdown; charset=utf-8` containing `SKILL.md`. Returns `404`
when the skill is not enabled/registered.

### Hook routes

| Method | Path | Behavior |
|---|---|---|
| GET | `/api/hooks` | returns current hook registry snapshot |
| POST | `/api/hooks/refresh` | reloads hook trust store and hook registry |
| POST | `/api/hooks/:id/trust` | persists trust for the hook definition |
| POST | `/api/hooks/:id/disable` | disables hook unless it is managed |
| POST | `/api/hooks/:id/enable` | enables hook |

Mutating hook routes return the refreshed hook registry snapshot. Managed hooks
cannot be disabled and return `409 {"error":"HOOK_MANAGED"}`.

---

## 9. Models and Managed Provider Auth

### `GET /api/models`

返回所有运行时已启用的模型配置。该路由要求通过 Web 认证，响应包含 `api_key`
原值和 `has_api_key` 布尔值，供编辑表单默认遮罩回填；调用方必须把整个响应视为
敏感数据，日志与错误消息仍不得包含密钥。高级字段有值时会原样返回，包括
`endpoint_mode`、`max_output_tokens`、`capabilities_source`、`reasoning`、
`request_headers`、`context_window` 与 `stream_timeout_ms`。

### `POST /api/models`

新增模型配置。请求体为 saved model draft，例如：

```json
{
  "name": "gateway-gpt",
  "provider": "openai",
  "model": "gpt-4.1",
  "base_url": "https://example.com/v1",
  "api_key": "{env:OPENAI_API_KEY}",
  "endpoint_mode": "base_url",
  "max_output_tokens": 32768,
  "request_headers": {"X-Team":"acecode"},
  "context_window": 128000,
  "stream_timeout_ms": 600000,
  "capabilities": ["vision", "tool_use", "reasoning"],
  "capabilities_source": "catalog",
  "reasoning": {
    "supported": true,
    "mandatory": false,
    "default_enabled": true,
    "enabled": true,
    "supported_efforts": ["low", "medium", "high"],
    "default_effort": "medium",
    "effort": "high",
    "supports_max_tokens": false
  }
}
```

返回包含 `api_key` 原值的模型配置。校验错误使用 `BAD_JSON`、`BAD_REQUEST` 或
`SavedModelEditError` 字符串；持久化失败会回滚内存并返回
`500 PERSIST_FAILED`。

### `PUT /api/models/:name`

更新模型配置并可重命名。省略 `api_key` 会保留原密钥；传入非空
`api_key` 会替换密钥。只有允许无认证的端点才接受
`clear_api_key:true`，否则请求失败。`credential_source_name` 可复用另一份
配置的凭据，但仅在 runtime provider、规范化 Base URL 和
`models_dev_provider_id` 完全兼容时允许；HTTP/HTTPS 默认端口会规范化，URL
路径仍区分大小写。

省略 `base_url`、`context_window`、`stream_timeout_ms`、`capabilities`、
`endpoint_mode`、`max_output_tokens`、`capabilities_source`、`reasoning` 或
`request_headers` 会保留原值。显式空 `request_headers` 会清空请求头；对应高级
字段传 `null` 时按各字段合同清除。外部登录器留下的 legacy `readonly:true` 只是
兼容元数据，不阻止编辑。响应包含 `api_key` 原值与 `has_api_key`，并仅允许经
Web 认证的调用方读取。

### `DELETE /api/models/:name`

Removes a saved model profile. If a busy active session is using the profile,
returns `409 MODEL_IN_USE`. On success:

```json
{"ok":true}
```

### `POST /api/models/test`

Tests one OpenAI-compatible or Anthropic model using the current unsaved draft.
Requires the same authentication as model management. Accepts the model mutation
fields (`provider`, `model`, `base_url`, `api_key`, `request_headers`,
`endpoint_mode`, and supported advanced options). The display `name` is optional
and ignored, so a conflicting or unfinished preset name does not block testing.

```json
{"provider":"openai","model":"example-model","base_url":"https://example.com/v1","api_key":"example-key"}
```

For a new model, `credential_source_name` reuses a compatible saved credential
inside the daemon. For editing, `original_name` identifies the existing profile
and applies update semantics, including retaining an omitted API key. Validation
and merging happen on a private configuration snapshot. No configuration,
default-model setting, session history, or probe cache is saved.

The daemon sends one user message, `Reply with OK.`, with no tools or history.
It uses the existing Provider implementation and request options, with a timeout
of at most 30 seconds and no automatic retry. Multiple selections are tested
sequentially by the UI; a failure stops that batch.

Success requires non-whitespace visible reply text and no Provider error:

```json
{"ok":true}
```

Invalid drafts return `400` with a model validation code. Provider failures
return `502` with `MODEL_TEST_NETWORK`, `MODEL_TEST_HTTP_ERROR`,
`MODEL_TEST_EMPTY_REPLY`, or `MODEL_TEST_FAILED`; timeouts return
`504 MODEL_TEST_TIMEOUT`. An upstream HTTP status may be included as
`upstream_status`. Replies, credentials, exception text and raw upstream bodies
are never included in this endpoint's response.

### `POST /api/models/probe`

Probes provider model ids. OpenAI-compatible providers call upstream
`GET /models`; Copilot uses saved GitHub auth; `provider:"grok"` uses the
daemon-managed xAI OAuth credentials and the fixed Grok Build `/v1/models`
endpoint. Anthropic model ids are entered manually and are not probed. Grok
catalog parsing accepts the official `id`、`model`、`modelId` and `_meta`
fallback shapes, ignores hidden entries, preserves first-seen order, and
deduplicates model ids.

ACEModel 官方端点的内置 `starrylight`、`moonlight` 和 `aurora`
优先使用上游 `/models` 返回的有效最大上下文字段。Daemon 会将该值
原样写入 `model_context_windows`，不按本地默认值截断；字段缺失、无效
或无法解析时，才从内置目录回填 `250000` Token。探测和本地探测缓存响应
还会返回 `model_capabilities`，确保三个内置模型继续使用目录声明的
`["vision", "tool_use"]`，不会因重新探测丢失默认能力。

Success:

```json
{
  "models": ["gpt-4.1"],
  "model_context_windows": {"gpt-4.1": 1047576},
  "model_capabilities": {"gpt-4.1": ["vision", "tool_use"]},
  "model_reasoning": {"gpt-4.1": null}
}
```

#### Explicit reasoning discovery contract

An OpenAI-compatible upstream, including ACEModel, can declare controllable
reasoning in each `GET /models` entry:

```json
{
  "id": "gpt-6-astra",
  "reasoning": {
    "supported_efforts": ["low", "medium", "high", "xhigh", "max"],
    "default_effort": "high"
  }
}
```

`supported_efforts` must be a nonempty array of distinct values from `minimal`,
`low`, `medium`, `high`, `xhigh`, `max`. `default_effort` is optional and, when
present, must belong to that list. ACECode does not infer reasoning from model
names or fill in undeclared levels. Missing, empty, or invalid declarations
produce `null`, including after a previously valid declaration is removed.

Both fresh and cached probe responses contain a `model_reasoning` entry for
every returned model. Valid declarations are normalized to the saved-model
reasoning shape, with `supported:true`, `default_enabled:true`, the declared
efforts and default, and `supports_max_tokens:false`. Model settings use this
metadata to select “推理”; an ACEModel without a valid declaration is unchecked
and has no composer control. Custom models begin with reasoning disabled;
explicitly checking “推理” initializes an editable `low`, `medium`, `high` list.

Enabled generic OpenAI-compatible Chat Completions requests send
`reasoning_effort` only when an explicit or declared default effort exists.
Disabling reasoning or omitting its configuration omits that request field.
Existing Anthropic and OpenRouter request encodings remain provider-specific.

Errors include `COPILOT_AUTH_REQUIRED`, `GROK_AUTH_REQUIRED`,
`GROK_AUTH_EXPIRED`, `GROK_MODELS_UNREACHABLE`, `GROK_MODELS_HTTP_ERROR`,
`GROK_MODELS_BAD_JSON`, `INVALID_REQUEST_HEADER`, `PROBE_FAILED`,
`PROBE_HTTP_ERROR`, and `PROBE_BAD_JSON`.

### `GET /api/models/catalog`

只读取当前本地注册表，不触发网络请求。返回固定顶层结构：

```json
{
  "catalog": {
    "source": "bundled",
    "version": 1,
    "updated_at": "2026-08-10T00:00:00Z",
    "freshness": "bundled"
  },
  "providers": [
    {
      "id": "openai",
      "name": "OpenAI",
      "runtime_provider": "openai",
      "base_url": "https://api.openai.com/v1",
      "doc": "https://platform.openai.com/docs",
      "auth_mode": "required",
      "endpoint_editable": false,
      "model_input": "catalog",
      "api_key_env": "OPENAI_API_KEY",
      "models_dev_provider_id": "openai",
      "group": "native",
      "endpoint_modes": ["base_url"]
    }
  ]
}
```

`catalog.version` 是非负整数。`auth_mode` 只会是 `required`、`optional`、
`none` 或 `managed`。Custom OpenAI-compatible Provider 明确支持
`endpoint_modes:["base_url","full_url"]`，并要求 API Key 或兼容的
`credential_source_name`。一等自营 Provider `acemodel`（展示名 ACEModel）
使用与 OpenAI 相同的 OpenAI-compatible 字段，固定 Base URL 为
`https://ge.bigjuan.xyz/aceapi/v1`，`group` 为 `custom`（Web 再按 id 提到「自营模型」），查询时返回内置
`starrylight`、`moonlight` 与 `aurora`，三者本地回退 `context_window` 均为 `250000`，且默认返回 `capabilities:["vision","tool_use"]`；模型探测得到的有效服务器值优先。Copilot 与 Grok Coding Plan 使用 `managed`，分别由
ACECode 的 GitHub/xAI 设备登录与固定受管端点负责认证。普通 `xai` Provider
仍保留为 OpenAI-compatible API Key 接入；只有目录 id `grok` 使用 Coding Plan。

### `GET /api/models/catalog/:provider_id`

从指定 Provider 的本地目录进行大小写不敏感搜索。可选查询参数为 `q` 和
`limit`；默认最多返回 50 项，服务端硬上限为 100，完全匹配模型 ID 的结果优先，
其余结果稳定排序。返回结构：

```json
{
  "provider_id": "openrouter",
  "models": [
    {
      "id": "openai/gpt-4.1",
      "name": "GPT-4.1",
      "context_window": 1047576,
      "max_output_tokens": 32768,
      "capabilities": ["vision", "tool_use"],
      "reasoning": {
        "supported": false,
        "mandatory": false,
        "default_enabled": false,
        "supported_efforts": [],
        "supports_max_tokens": false
      },
      "deprecated": false,
      "input_modalities": ["text", "image"],
      "output_modalities": ["text"],
      "knowledge_cutoff": "2024-06",
      "pricing": {"input": 2.0, "output": 8.0}
    }
  ],
  "limit": 50
}
```

目录模型使用 `id`；只有推荐模板使用 `model_id`。`supported_efforts` 只会包含
`minimal`、`low`、`medium`、`high`、`xhigh`、`max`；上游的 `none` 只表示
reasoning 可关闭，`default`、`null` 等非规范值不会进入响应。

### `POST /api/models/catalog/refresh`

显式触发 models.dev 网络刷新；普通目录读取永不隐式访问网络。只有
`models_dev.allow_network=true` 时允许调用，否则返回 `403`。下载结果必须通过
最小结构校验，并至少包含 50 个 Provider、1000 个模型；下载、解析或阈值校验
失败时返回错误并继续使用最后一份有效注册表，不会安装部分结果。成功后返回与
`GET /api/models/catalog` 相同的目录摘要结构。

### `GET /api/config/default-model`

Returns:

```json
{"name":"saved-model-name"}
```

### `POST /api/config/default-model`

Body:

```json
{"name":"saved-model-name"}
```

The name must exist in `saved_models`. Success returns:

```json
{"default_model_name":"saved-model-name"}
```

### `GET /api/sessions/:id/model?workspace=<hash>`

Returns current session model state:

```json
{
  "name": "saved-model-name",
  "provider": "openai",
  "model": "gpt-4.1",
  "context_window": 128000,
  "deleted": false
}
```

The state also includes `models_dev_provider_id`, `reasoning` (the effective
saved-model reasoning options, or `null`), and `reasoning_effort` (the session
override string, or `null` when inheriting the saved model default). Clients
show a depth control only for enabled reasoning with a nonempty effort list.
Budget-only reasoning and managed Copilot/Grok providers have no effort control.

This GET is side-effect free. It reports the current public model state (and
may mark a saved-model name as deleted), but it never reconstructs or replaces
the session Provider.

### `POST /api/sessions/:id/model/reload`

Forces the requested active session to re-resolve its selected saved-model
profile and compare the effective Provider construction inputs. A successful
request returns HTTP 200 with the existing public model-state shape nested in
an outcome envelope:

```json
{
  "outcome": "reloaded",
  "model_state": {
    "name": "gateway",
    "provider": "openai",
    "model": "gpt-5",
    "context_window": 128000,
    "deleted": false
  },
  "warning": "session metadata could not be persisted"
}
```

`outcome` is one of:

- `reloaded`: effective construction inputs changed and a new Provider was
  published for future turns;
- `already_current`: the selected profile resolves to the Provider already in
  use; model state such as `context_window` may still be refreshed;
- `unresolvable`: the selected name was deleted, renamed away, or is a
  session-local ad-hoc profile. The existing Provider and model state remain in
  use; deletion is not a live connection revocation.

`warning` is omitted when empty. A warning is sanitized and non-fatal: for
example, Provider publication can succeed even when best-effort session
metadata persistence fails. Pre-publication construction or revalidation
failure returns `500 MODEL_RELOAD_FAILED` and preserves the old Provider,
state, and applied revision. Unknown sessions return 404; a server without a
session registry returns 503.

The operation is scoped to the named active session. It does not change the
workspace, cwd model override, configured default model, or any other session.

### `POST /api/sessions/:id/model`

Body:

```json
{"name":"saved-model-name"}
```

Switches the active session to that saved model profile and returns model
state. Returns `404` when the session is not active in the registry.

Changing to a different saved model clears the previous reasoning override.
Reloading the same model retains a still-supported override; disabling the
capability or removing that effort clears it.

### `POST /api/sessions/:id/reasoning`

Body:

```json
{"effort":"high"}
```

Use `{"effort":null}` to restore the saved model's defaults. Success returns
the standard session model state. The selected string must occur in the
current model's enabled `reasoning.supported_efforts`. This operation changes
only the named session: other sessions, saved profiles and workspace defaults
are unaffected. The override is persisted, restored on resume and inherited
by a fork. An explicit effort takes precedence over the saved reasoning token
budget; resetting the override restores that budget as well as the default.

Mutation runs under the worker's idle gate. An active turn or queued input
returns `409 SESSION_BUSY` without changing the selection. Malformed bodies,
non-null selections for unsupported models and undeclared efforts return
`400 INVALID_REASONING_EFFORT`;
unknown sessions return `404 SESSION_NOT_FOUND`, unavailable model profiles
return `409 MODEL_UNAVAILABLE`, and update failures return
`500 REASONING_UPDATE_FAILED`.

### Copilot auth routes

| Method | Path | Response |
|---|---|---|
| GET | `/api/copilot/auth` | `{"provider":"copilot","has_token":true,"authenticated":true}` |
| DELETE | `/api/copilot/auth` | deletes saved GitHub token, returns auth false |
| POST | `/api/copilot/auth/device` | starts GitHub device flow |
| POST | `/api/copilot/auth/device/poll` | polls one device-flow tick |

`POST /api/copilot/auth/device` response:

```json
{
  "status": "pending",
  "provider": "copilot",
  "device_code": "...",
  "user_code": "ABCD-1234",
  "verification_uri": "https://github.com/login/device",
  "interval": 5,
  "expires_in": 900,
  "expires_at_unix_ms": 1783152000000
}
```

Polling success returns `status:"authenticated"`. Pending, slow-down, and
failure states return `status`, `error`, `message`, and
`interval_delta_seconds`.

### Grok Coding Plan auth routes

| Method | Path | Response |
|---|---|---|
| GET | `/api/grok/auth` | `{"provider":"grok","authenticated":true}` |
| DELETE | `/api/grok/auth` | deletes `~/.acecode/grok_auth.json`, returns auth false |
| POST | `/api/grok/auth/device` | starts xAI Device OAuth |
| POST | `/api/grok/auth/device/poll` | polls exactly one device-flow tick |

`POST /api/grok/auth/device` response:

```json
{
  "status": "pending",
  "provider": "grok",
  "authenticated": false,
  "device_code": "...",
  "user_code": "ABCD-EFGH",
  "verification_uri": "https://accounts.x.ai/activate",
  "verification_uri_complete": "https://accounts.x.ai/activate?user_code=ABCD-EFGH",
  "interval": 5,
  "expires_in": 1800,
  "expires_at_unix_ms": 1783152000000
}
```

轮询终态为 `authenticated`、`expired` 或 `failed`；`slow_down` 会返回
`interval_delta_seconds`，前端应在原 interval 上累加。凭据保存到
`~/.acecode/grok_auth.json`，写入使用受限权限和原子替换。到期前 60 秒自动刷新；
上游返回 401 时只强制刷新并重放一次，refresh token 轮换会立即持久化。

所有 Grok auth 状态、轮询、模型探测和错误响应都不得包含 `access_token`、
`refresh_token`、账号 email 或 user id。`device_code` 只在设备授权开始响应及随后
浏览器提交的轮询请求中出现；daemon 日志与错误消息会脱敏认证字段。

---

## 10. Config, MCP, Update, and Feedback

### `GET /api/ui/onboarding/desktop`

Returns the backend-owned Desktop guided-tour version and whether that version
has been dismissed:

```json
{"guide_version":1,"dismissed":false}
```

The state is stored in `~/.acecode/state.json`, not browser storage, so it
survives Desktop loopback-port changes and Edge compatibility profiles.

### `POST /api/ui/onboarding/desktop/dismiss`

Idempotently marks the current Desktop guided-tour version as dismissed and
returns the same payload with `dismissed:true`. A state-file write failure
returns HTTP `500` with `error:"PERSIST_FAILED"`.

### `GET /api/config/ui-preferences`

Returns:

```json
{
  "show_acecode_avatar": false,
  "theme": "system",
  "color_theme": "blue",
  "font_size": "medium",
  "sidebar_session_time": true,
  "message_auto_collapse": true
}
```

`theme` accepts `system`, `light`, or `dark`; `color_theme` accepts `blue`,
`orange`, or `eva-01`; and `font_size` accepts `small`, `medium`, or `large`. These values
are stored in `~/.acecode/config.json`, so Desktop restores them even when its
managed daemon uses a different loopback port. The avatar preference is kept
for compatibility and is always normalized to `false`.

`message_auto_collapse` is a boolean, defaulting to `true` for new and legacy
configurations. When `false`, main and subagent conversations display messages
without activity/turn folding; individual tool calls remain collapsible.
The preference is persisted and included in the Desktop appearance bootstrap.

### `PUT /api/config/ui-preferences`

Body:

```json
{"theme":"dark","color_theme":"orange","font_size":"large"}
```

The body may contain one or more supported fields. Every supplied field is
validated before mutation; omitted fields keep their current values. Legacy
`{"show_acecode_avatar":false}` requests remain valid. On success the endpoint
persists the configuration and echoes the complete normalized response shown
above; a write failure returns `500` with `error:"PERSIST_FAILED"` and restores
the in-memory values.

Selecting `eva-01` requires verified local resources. Otherwise the request
returns `409` with `error:"THEME_NOT_INSTALLED"` without changing any preference.
EVA uses its package's fixed light palette. The stored `theme` preference is
preserved for switching back to an ordinary theme; OS color-scheme changes
cannot override EVA. Its background image is used only on the new-task home.

### Downloadable themes

Local ZIP import uses authenticated `POST /api/themes/import/preview` with
`Content-Type: application/zip` and the original ZIP body (maximum 16 MiB).
It returns `theme`, `package_sha256`, `package_bytes`, and a PNG `thumbnail_url`
data URI without installing. After explicit UI confirmation, send the same body
to `POST /api/themes/import?sha256=<package_sha256>`. The daemon revalidates the
archive and digest, then uses the existing atomic local install. Changed bytes
return `409/THEME_IMPORT_CHANGED`; conflicting installed versions are preserved.
Applying the imported theme is a separate normal UI preference operation and
depends on the user's checkbox. These local APIs do not contact the workshop.

All theme endpoints require the normal daemon authentication. Theme files live
in `themes/` beside the daemon configuration, independently of application
updates. The application bundles only the small card thumbnail and three
preview swatches; it does not bundle or automatically download the full EVA
theme. See [theme packaging](themes.md) for the independent publish layout.

`GET /api/themes?refresh=1` refreshes `upgrade.base_url + "themes/catalog-v2.json"`,
falling back to `themes/catalog.json` when the expanded catalogue returns HTTP
404 or 410. The legacy EVA-only public catalogue remains available to old clients.
The expanded catalogue supports `national-day-2026` and `eva-01`; entries are
validated independently and installation resolves the requested ID.
Each catalogue operation resolves the current update server, including changes
made through `PUT /api/config/upgrade`, without a daemon restart. Cached metadata
is scoped to its source server. Already started downloads retain the resource
URL and integrity metadata captured when they were confirmed.
Without `refresh`, the daemon uses the cached catalogue when available. The
response contains `schema_version:1`, `themes`, `offline`, and `job`. Each entry
has `id`, `version`, three `swatches`, `installed`, `installed_version` (empty
when no valid installation exists), `update_available` (a newer semantic
version is available), and `package`/`thumbnail`
descriptors containing a relative `path`, exact `bytes`, and lowercase `sha256`.
The API also includes `package.url` and `thumbnail.url`, resolved against the
configured update server. Error responses and failed jobs include
`error_path` with the actual catalogue/archive URL or failing local path.
An unavailable server falls back to its own cached catalogue with `offline:true`;
without a cache for that server and without any installed themes it returns
`503/THEME_CATALOG_UNAVAILABLE`. If local AI themes exist, they remain visible:
the response contains `offline:true`, a `catalog_error` object, an EVA descriptor
with `available:false` and no remote package, followed by the local entries.
Installed National Day resources are also reported and remain usable offline.
Clients disable the unavailable download without hiding installed local themes.

Local entries have `source:"local"`, `installed:true`, `name`, `mode`, `version`,
`installed_version`, three `swatches`, `update_available:false` and thumbnail
metadata whose URL uses the authenticated `/api/themes/<id>/images/thumbnail`
endpoint. Their IDs match `^ai-[a-z0-9]+(?:-[a-z0-9]+)*$` and are at most 64
characters. Definitions accept light or dark mode and all 28 HEX tokens. The
definition and image routes work offline. Built-in IDs cannot be overwritten.

Schema version 1 definitions may also include an `appearance` object:

```json
{"appearance":{"logo_color":"#9B6DFF","home_title_color":"#F5F0FF","extend_to_titlebar":true}}
```

All members are optional. `logo_color` and `home_title_color` accept only
`#RRGGBB`; `extend_to_titlebar` accepts only a JSON boolean. Unknown members,
null, non-object appearance and wrong member types are rejected. Omitting the
object preserves legacy behavior: original logo colors, `colors.fg` for the
homepage heading, no extension for custom themes and the existing extension
and white right-side controls for EVA. Explicit members override defaults.
Dark themes with extension enabled use white right-side title-bar controls
while the homepage wallpaper is visible. Full definitions and exported ZIPs
preserve the optional object; the 28-member `colors` object is unchanged.

Additional optional `appearance` members are `home_background_color`,
`session_background_color`, and `user_message_background_color` (all `#RRGGBB`),
and `home_composer_opacity`, `home_background_opacity`,
`session_background_opacity`, `user_message_background_opacity` (finite JSON
numbers from 0 to 1, where 1 is opaque). Defaults remain 0.95 for the home
composer and 1 for artwork. Artwork overlays use the corresponding background
color, falling back to `colors.bg` or `colors.accent-bg` for user messages.
These controls never reduce text opacity or replace the original three settings.

Optional top-level `session_background` and `user_message_background` descriptors
use the same byte-count/SHA-256 schema as `background`, with fixed paths
`session-background.png` and `user-message-background.png`. Only declared files
may be present. Their authenticated image routes use `images/session-background`
and `images/user-message-background`; all returned images are PNG. The latter
applies only to user-sent message bubbles, never assistant replies.

`POST /api/themes/<id>/install` requires the exact metadata displayed by the
confirmation dialog:

```json
{"confirm_download":true,"version":"1.0.0","bytes":2220741,"sha256":"<catalogue SHA-256>"}
```

Missing or mismatched consent returns `409/THEME_CONFIRMATION_REQUIRED` and
starts no download. The accepted response and `GET /api/themes/job` contain
`id`, `version`, `state`, `bytes_downloaded`, `bytes_total`, and `automatic`. States are
`idle`, `downloading`, `installing`, `completed`, `cancelled`, or `failed`;
failure adds an `error` code. Only one installation runs per daemon; a second
request returns `409/THEME_DOWNLOAD_BUSY`. `POST /api/themes/job/cancel`
requests cancellation, which is observed through job polling. The frontend
shows this progress and any retry action inside the theme card, even after
closing and reopening Settings. A later theme selection revokes automatic
application of an earlier download.

`POST /api/themes/first-run` is authenticated and blocked during configuration
migration. It atomically creates `themes/.national-day-2026-attempted` and returns
`{"id":"national-day-2026","claimed":true}` only to the first claimant; later
requests return `claimed:false`. It neither downloads nor changes appearance.
The marker persists across failures, process restarts and application upgrades.
After restoring canonical appearance preferences, the first Web/Desktop client
automatically installs the National Day package using the same exact integrity
metadata with `automatic:true`, or reuses a valid installation. Automatic jobs
retain that flag so every observing client suppresses their failure notifications.
Application follows successful resource preparation. A later explicit theme
choice wins, and persistence failures silently roll back the original appearance.
The automatic `PUT /api/config/ui-preferences` includes `expected_appearance`
containing the initial GET response. The server compares it under the configuration
lock before writing; a changed snapshot returns `409/APPEARANCE_CHANGED` without
modifying preferences. This also protects later choices made in another window.
Manual downloads continue to require confirmation and display normal errors.

The daemon verifies archive bytes, SHA-256, allowed ZIP entries, the fixed
palette schema, and all declared images before publishing an installed version. An
interrupted download does not alter the active preference; a damaged local
installation can be repaired by confirming and downloading it again.
`GET /api/themes/eva-01` returns the validated installed `theme.json` without
network access, or `404/THEME_NOT_INSTALLED`. `GET
/api/themes/eva-01/images/thumbnail` remains available for clients needing a
server preview; Appearance uses its bundled thumbnail without calling it.
`.../images/background` is available only after installation.
Both image responses are PNGs. Applying an installed theme requires no
redownload, including after an offline restart.

### Custom theme export and deletion

Only installed local `ai-*` themes support export and deletion. Blue, orange,
and the downloadable built-in EVA theme reject both operations. These routes
require normal daemon authentication and preserve the existing EVA download
job independently of theme export progress.

`POST /api/themes/<id>/export` accepts `{ "native_save": false }` by default.
With `native_save:true`, the daemon immediately opens the existing native
Save As picker with a ZIP filename. Only a picker-selected path may become a
native output target; request-supplied arbitrary destination paths are not
supported. Cancelling the picker returns
`{ "state":"cancelled", "cancelled":true }` without starting packaging.
An unavailable native picker returns `501/THEME_NATIVE_SAVE_UNAVAILABLE`.

An accepted job returns `job_id`, `id`, `version`, `filename`, `state`,
`progress` (null or a real fraction from 0 to 1), `reused`, and `native_saved`.
States are `preparing`, `compressing`, `saving`, `completed`, `cancelled`,
and `failed`. Clients poll `GET /api/themes/exports/<job_id>` and cancel via
`POST /api/themes/exports/<job_id>/cancel`. Failed jobs include `error`,
`message`, and `error_path`. A valid existing ZIP matching the installed
resources is reused; missing or stale archives are rebuilt with libzip's
actual progress/cancellation callbacks. Native completion means the selected
file was written; Web completion means the ZIP is ready for download.
Cancellation can initially return a working snapshot. Clients must resolve the
terminal state and preserve a later confirmed `completed/native_saved:true`
result, even if the cancel request failed; a file already committed must not be
reported as cancelled. The same rule applies when Web file writing has committed.

New archive caches use `themes/exports/<id>/<version>.zip`. Legacy flat
`<id>-<version>.zip` caches remain readable only after verifying their embedded
theme ID, version, and resources; deletion also checks this ownership instead
of treating a filename prefix as sufficient proof.

`GET /api/themes/exports/<job_id>/download` returns the completed ZIP as
`application/zip` with an attachment filename, `no-store`, and `nosniff`.
Clients may use an authenticated Blob download, without placing daemon
credentials in a download URL. The ZIP contains `theme.json`, `background.png`,
and `thumbnail.png`, plus any declared `session-background.png` and
`user-message-background.png` (three to five root files). Unknown, cancelled, failed, or
unfinished jobs cannot download a partial package.

`DELETE /api/themes/<id>` returns `{id,deleted:true,ui_preferences}`. Deleting
the configured active theme persists `color_theme:"blue"`; deleting another
theme preserves the configured selection. The theme files and matching
application-owned exports are isolated before preference persistence and are
restored if that commit fails. User-saved exports, original input images,
drafts, and other themes remain untouched. Invalid IDs, out-of-root paths,
symlink/reparse redirects, and conflicting operations are rejected. Preference
writes also validate local theme availability under the same transaction
ordering, so a stale queued selection cannot restore a deleted ID.
If post-commit quarantine cleanup cannot finish, deletion remains successful
and the response includes `cleanup_pending:true` and `cleanup_message`; clients must not restore
the deleted card or report that the deletion itself failed.

Representative theme-management errors:

| HTTP / code | Meaning |
|---|---|
| `400/THEME_INVALID_ID` | invalid custom theme identifier |
| `403/THEME_BUILTIN_PROTECTED` | built-in theme export/deletion is forbidden |
| `404/THEME_EXPORT_NOT_FOUND` | unknown or expired export job |
| `409/THEME_BUSY` | a conflicting theme export or transaction is active |
| `409/THEME_EXPORT_NOT_READY` | the export is not complete or was cancelled/failed |
| `409/THEME_CHANGED` | the installed theme or validated archive changed |
| `422/THEME_UNSAFE_PATH` | a theme path or filesystem redirection is unsafe |
| `500/PERSIST_FAILED` | appearance persistence failed; isolated files are restored |
| `501/THEME_NATIVE_SAVE_UNAVAILABLE` | no native Save As capability is available |

### AI theme workflow tool

The built-in `theme_create` tool is registered for daemon and TUI sessions.
`palette` accepts `name`, `mode`, all 28 `colors`, optional `appearance`, and
optionally `draft_id` to revise a draft. Each palette call replaces the whole
proposal; omitting `appearance` removes a previous override. It persists the
draft under `themes/drafts`, then uses the
native question channel for explicit palette approval. `prototype` accepts
`draft_id`, `background_path`, `preview_path`, and optional
`session_background_path` / `user_message_background_path`; it decodes and
copies all supplied images before asking for prototype approval. Each prototype
call replaces the complete image set, so omitting an optional path removes that
region from the draft. Its approved digest covers every supplied image.
`preview_path` may be a screenshot of the local HTML in ACECode Browser; neither
HTML nor SVG paths are accepted as image inputs. A draft is owned by its creating
session, and an update invalidates earlier approvals. Headless mode, deny
policy, unattended goals, cancellation, timeout and custom feedback never count
as approval. No `confirmed` input can bypass these gates.
The palette and prototype approvals cover explicit appearance members as well
as colors. A draft without appearance retains the original digest algorithm,
so already approved legacy drafts remain resumable. Adding, changing or
removing appearance invalidates previous approvals. Draft status and installed
definitions retain the confirmed object; installation retries also compare it.
Invalid palette appearance returns `422/THEME_INVALID_APPEARANCE` before
replacing a draft or requesting approval.

`install` accepts only `draft_id`, verifies approved checksums, generates the
thumbnail and three-to-five-file ZIP, and publishes the immutable version and installed
pointer. Successful tool output includes `installed_path` and `package_path`
(under `themes/exports`); metadata includes
`theme_created:{id,version,name,apply:true}`. The current live UI can refresh and
apply this theme. Repeating installation of the same draft returns the same
theme. Prior themes and the ordinary appearance mode preference remain intact.
`status` reads a draft, or lists the caller's drafts when `draft_id` is omitted.
Results include `draft_id`, `stage`, and `next_action` for resuming after an
interruption. No status call generates images or records user approval.

### `GET /api/config/ui-locale`

Returns the persisted fixed-copy locale preference:

```json
{"locale":"auto"}
```

Supported values are `auto`, `zh-CN`, and `en-US`. `auto` resolves Chinese
system locales to `zh-CN` and all other system locales to `en-US`. A legacy
configuration with no `ui.locale` remains `zh-CN`; a newly generated
configuration explicitly writes `auto`.

### `PUT /api/config/ui-locale`

Body:

```json
{"locale":"en-US"}
```

The route validates the canonical value, persists `ui.locale`, and echoes the
stored preference. Invalid values return HTTP `400` with
`error:"INVALID_UI_LOCALE"`; a persistence failure restores the previous
in-memory value and returns HTTP `500` with `error:"PERSIST_FAILED"`.

### `GET /api/config/remote-web`

Returns configured intent plus the currently effective daemon/proxy state. Because
`connections` contains bearer-token URLs, the route uses normal daemon auth
and always sends `Cache-Control: no-store`.

```json
{
  "enabled": true,
  "configured_enabled": true,
  "effective_enabled": true,
  "configured_bind": "127.0.0.1",
  "effective_bind": "0.0.0.0",
  "daemon_bind": "127.0.0.1",
  "daemon_port": 12399,
  "proxy_bind": "0.0.0.0",
  "proxy_pid": 4242,
  "proxy_state": "running",
  "proxy_ipv6": true,
  "error": "",
  "applying": false,
  "port": 28081,
  "connections": [
    {
      "host": "ACE-PC",
      "kind": "computer_name",
      "url": "http://ACE-PC:28081/?token=<encoded-token>"
    },
    {
      "host": "192.168.1.20",
      "kind": "network_address",
      "url": "http://192.168.1.20:28081/?token=<encoded-token>"
    }
  ]
}
```

The current computer name is the first/default connection candidate when it is
a valid hostname. Active non-loopback interface addresses follow it and match
the proxy listener's available IP address families. Unspecified, loopback, multicast,
and link-local destinations are omitted; `0.0.0.0` is never returned as a
destination. Multiple Wi-Fi, Ethernet, VPN, or VM adapter addresses may be
present. An empty `connections` array means neither a usable computer name nor
an address was discovered.

### `PUT /api/config/remote-web`

Body:

```json
{"enabled":true}
```

Enabling starts a separate ACECode reverse-proxy child and waits until its
external listener is ready before persisting `web.remote_enabled:true`.
Disabling persists false and stops only that child. Crow remains continuously
bound to `127.0.0.1:web.port`, preserving the daemon PID, token, sessions,
consoles, and active Agent work. A local page receives the mutation response
without listener downtime. A page opened through the remote proxy naturally
disconnects when it disables that proxy.

`web.remote_port:0` first tries the port adjacent to `web.port`, then falls back
to an OS-selected wildcard port. A non-zero value is fixed: a collision returns
HTTP `502` with `error:"REMOTE_WEB_PROXY_START_FAILED"` and the newly enabled
intent is not persisted. The actual external port is always returned as `port`.
If a configured proxy later exits, `configured_enabled` stays true while
`effective_enabled` becomes false, `proxy_state` is `failed`, and `error`
contains a sanitized diagnostic.

Legacy configurations that set a non-loopback `web.bind` without an explicit
`web.remote_enabled` are loaded as remote-enabled and normalized to the
loopback daemon plus proxy representation on the next save.

Enabling while the daemon is in dangerous mode returns HTTP `409` with
`error:"DANGEROUS_MODE_REMOTE_WEB_FORBIDDEN"` and does not change the
configuration.

### Shared settings write semantics

Saved-model/default-model writes and the custom-instructions,
default-permission, desktop-notification, remote-Web, and upgrade routes share the same
typed mutation path as the TUI settings center. A write acquires the process
and interprocess config lock, reloads the latest canonical `config.json`,
patches only the requested field or domain, validates it, and atomically
replaces the file. This prevents a concurrent TUI/Desktop/daemon write to an
unrelated setting from being overwritten. Validation or replacement failure
leaves the previous canonical file and caller-confirmed in-memory state intact.
Mutation diagnostics redact credential-bearing model fields and headers.

### `GET /api/config/custom-instructions`

Returns:

```json
{"text":"custom prompt text"}
```

### `PUT /api/config/custom-instructions`

Body:

```json
{"text":"custom prompt text"}
```

The text is byte-limited by `kCustomInstructionsMaxBytes`. Existing sessions
pick up changes on later turns through the daemon config pointer.

### Summary generation settings

`GET /api/config/summary-generation` returns `enabled` (default `false`),
`model_name` (a saved model name), `configured` (whether that saved model exists),
and `models` (available `{name, provider, model}` records). No model credentials
are included. Reads and writes require the normal API authentication and return
`Cache-Control: no-store`.

`PUT /api/config/summary-generation` accepts a partial object containing only
`enabled` (boolean) and `model_name` (string). Enabling requires an existing
saved model. Invalid patches return `400 BAD_REQUEST`; malformed JSON returns
`400 BAD_JSON`. Atomic persistence failures return `500 PERSIST_FAILED` and do
not change the live configuration. Unrelated settings are preserved by reloading
the latest disk configuration inside the config mutation lock.

When enabled, subsequent automatic-title attempts use this model before all
session, workspace, default and legacy `session_title.model_name` selections.
Each daemon title request captures its configuration before running. A removed
summary model gives `configured: false` and title generation skips that attempt
without substituting another model. Disabling retains `model_name` and restores
the previous title-resolution behavior, including the legacy override. This
setting does not change the conversation model or disable automatic titles.

### Image generation settings

`GET /api/config/image-generation` returns `enabled`, `source` (`inline` or
`saved_model`), `saved_model_name`, `base_url`, `api_key` (the stored inline key),
`has_api_key` (whether an inline key is stored), `configured` (whether the selected
connection resolves, regardless
of the enabled flag), `models` (`standard`, `high`, `ultra`), `default_quality`,
`timeout_ms`, and `connections` (reusable saved connections with `name` and
`has_api_key`). Only OpenAI-compatible base-URL connections are reusable;
chat-only full endpoint URLs are excluded. As with model settings, the inline
key is returned after normal API authentication so the password field can be
prefilled and revealed with its eye button. Reusable connections never include
their credentials. Do not log or cache settings responses.

The default image API URL comes from `constants::ACEMODEL_API_BASE_URL` in
`src/utils/constants.hpp`, shared with the ACEModel model catalog. The frontend
uses the returned URL. Saving the default leaves the URL out of the sparse
config, so later default changes apply together; custom URLs remain explicit.

`PUT /api/config/image-generation` accepts a partial update of the editable
fields. Omit `api_key` to preserve it, provide a nonempty value to replace it,
or send `"api_key":""` to clear it. URLs must use HTTPS, or loopback HTTP,
without embedded credentials, query parameters or fragments. Timeout is clamped
to 30,000–600,000 ms. Invalid input returns 400; persistence failure returns 500
without altering live settings. The write reloads and merges the on-disk config
before persisting, preserving unrelated changes. Success returns the same
settings snapshot and refreshes the shared image tool immediately. Existing
requests retain their configuration snapshot; subsequent calls use the saved
configuration. Editing/deleting a reused model connection also refreshes the tool.

The settings UI saves edited fields on blur, saves toggles/selects immediately,
and flushes remaining changes when collapsing or leaving settings. Writes are
serialized per connection, and responses preserve newer input. Reopening waits
for pending writes before reading the saved configuration and refilling the key.
There are no Save/Cancel buttons; failures retain the draft and offer retry.

`POST /api/config/image-generation/test` accepts
`{"confirm_cost":true,"config":{...partial settings...}}`. The cost flag is
required. The supplied draft overlays the saved config without persisting it or
enabling the tool. It generates exactly one image using the `standard` mapping,
even if the default quality is higher or the tool is disabled. The UI labels this
action **Generate test image** and explains that it consumes credits before it
can be triggered. Loading or saving never invokes the image provider; the UI
waits for pending automatic saves before starting a test.

Success returns `image_data_url`, `width`, `height`, `quality:"standard"`, and
`model` for a settings-only preview. These image bytes are not stored in session
history or logs. A concurrent test returns 409 `IMAGE_TEST_BUSY`; incomplete
configuration returns 400 `IMAGE_NOT_CONFIGURED`; upstream failures return 502
`IMAGE_QUOTA_ERROR` or `IMAGE_TEST_FAILED` without echoing provider error bodies.
All endpoints use normal API authentication/CORS and `Cache-Control: no-store`.

### Tool rewrite settings

Settings > Tools > 工具重写. Renames built-in tools as the model sees them: the
tool list sent to the provider, `tool_calls[].function.name` in replayed
history, the system prompt, tool descriptions and the error/guard texts tools
emit all use the rewritten name. Internal ids (session JSONL, permissions,
hooks payloads, TUI/Web rows) stay native. Inbound calls are accepted under
either name. This is an explicit opt-in for audit scenarios; the process
default is no rewriting.

The data is **not** part of `config.json`. It lives in
`<data_dir>/tool-rewrites.json` next to `config.json`, shared by the daemon,
the TUI and headless mode, and is loaded once at startup before tool
registration:

```json
{ "version": 1, "enabled": false,
  "rewrites": { "TodoWrite": "todowrite", "file_edit": "edit", "file_read": "read", "file_write": "write" } }
```

`GET /api/config/tool-rewrites` re-reads the file and returns `enabled`,
`rewrites` (`{native: public}`), `defaults` (the built-in seed above), `path`,
`tools` (Builtin-source tools registered in this daemon, sorted by name, each
`{name, description, read_only}`) and, only when the file could not be parsed,
`warning`. A missing file reads as `enabled:false` with the seed rewrites.

`PUT /api/config/tool-rewrites` takes `{enabled, rewrites}` and replaces the
whole document. Entries with an empty value or a value equal to the key are
dropped. Rules: every value matches `^[A-Za-z0-9_-]{1,64}$`, values are unique,
a value never equals another registered tool name or another rewrite key.
Violations return 400 `{error:"BAD_REQUEST", message}` without touching the
file or the live mapping; write failures return 500 `PERSIST_FAILED`. Success
writes the file atomically, publishes the mapping to the process so the next
model request uses it, and returns the same shape as GET. Hook matchers accept
the rewritten names as aliases of the native tool while a rewrite is active.

### Security center (`openspec add-security-center`)

Settings > Coding > Security Center is a UI over the sandbox model from
`align-codex-sandboxing` plus a process-wide audit store. Nothing here changes
how permissions are decided; it only exposes the switches, the lists, the
managed rules files and the decision log.

`GET /api/config/sandbox` returns:

```json
{
  "enabled": true, "network_access": false, "deny_defaults": true,
  "filesystem": { "read": [], "write": ["D:/shared/out"], "deny": ["~/.ssh", "**/.env"] },
  "windows_backend": "restricted-token", "writable_roots": [], "exclude_tmpdir": false,
  "defaults": { "deny": ["~/.ssh", "~/.aws", "~/.gnupg", "~/.netrc", "~/.docker/config.json", "~/.kube", ":acecode_home/config.json"] },
  "platform": { "os": "windows", "backend": "restricted-token", "available": true, "reason": "",
                "network_enforced": false, "network_best_effort": true, "read_isolation": false }
}
```

`platform` is the live backend probe. `read_isolation` is `false` on the
Windows restricted-token backend: `filesystem.read` and `filesystem.deny`
only stop writes there. `PUT /api/config/sandbox` accepts any subset of
`enabled`, `network_access`, `deny_defaults` and `filesystem.{read,write,deny}`
(each list replaces the stored list). Entries are trimmed and de-duplicated;
`~`, `~/path`, `:workspace_roots[/sub]`, `:tmpdir[/sub]` and
`:acecode_home[/sub]` are accepted verbatim, everything else must be an
absolute path, and wildcards are only allowed in `deny`. Violations return 400
`{error:"BAD_REQUEST", field, message}` without touching the config. Success
writes `config.json`, queues `set_sandbox_config` on every active session
(serialized with turns, so an in-flight turn keeps its policy until it ends)
and returns the GET shape plus `refreshed_sessions`.

`GET /api/security/exec-rules` scans `<data_dir>/rules/*.rules` and returns
`{dir, managed:["default.rules","default.sandboxed.rules"], files:[{name, path,
managed, scope:"global"|"sandboxed", exists, error, rules:[{pattern, display,
decision, justification}]}]}`. The two managed files are listed even when they
do not exist. `pattern` items are strings or string arrays (alternatives);
`display` is `git status|diff`. `error` is the parse error that makes the
loader skip the whole file. `PUT /api/security/exec-rules` takes
`{files:{"default.rules":[{pattern, decision, justification?}], "default.sandboxed.rules":[...]}}`
and rewrites those files completely (comments and `match` / `not_match` are
dropped). Only the two managed files are writable; the sandboxed file only
holds `allow` rules; an `allow` rule whose whole pattern is on the banned
prefix list (the same list `allow_remember` uses) or whose first token is an
interpreter, shell, `rm`, `sudo` or similar is rejected. Each file is re-parsed
before it is written; success reloads the rules in every active session and
returns the GET shape plus `refreshed_sessions`.

`GET /api/security/audit?category=&decision=&since_ms=&before_id=&q=&limit=`
returns `{entries, has_more, total, next_before_id?}` newest first. `category`
is one of `command` (bash), `file` (file_write / file_edit / apply_patch),
`tool` (other tools that needed confirmation), `sandbox` (a sandboxed run was
denied; `target` is the denied path) or `rule` (remembered rules and session
grants). `decision` is `allow`, `allow_session`, `allow_scoped`,
`allow_remember`, `deny`, `forbidden` or `blocked`; `source` on each entry is
`auto`, `rule`, `session`, `user`, `hook`, `headless`, `goal`, `sandbox` or
`none`. `q` is a substring match on target / reason / tool, `limit` is 1..500
(default 100), `before_id` continues from a previous page. Entries look like:

```json
{ "id": 42, "ts_ms": 1758070000000, "category": "command", "decision": "allow", "source": "auto",
  "reason": "known_safe", "tool": "bash", "target": "git status", "session_id": "20260917-...",
  "cwd": "N:/proj", "sandbox": "workspace-write", "detail": { "mode": "auto", "command_kind": "known_safe" } }
```

Read-only tools that are auto-allowed are not recorded. The store keeps the
newest 20000 entries in `<data_dir>/security/audit.sqlite3` (shared by the
daemon, TUI and headless mode). `GET /api/security/audit/summary` returns
`{total, by_decision, by_category, last_ts_ms, blocked_paths:[{path, count,
last_ts_ms}], path, max_entries}`. `GET /api/security/audit/export?format=jsonl|csv`
accepts the same filters as the list and answers with an attachment
(`Content-Disposition: attachment; filename="acecode-audit-<stamp>.<ext>"`).
`DELETE /api/security/audit` clears the log. All audit routes return 503
`AUDIT_UNAVAILABLE` when the process could not open the store.

### `GET /api/config/connectors`

Returns:

```json
{"connectors":[]}
```

### `PUT /api/config/connectors`

Body:

```json
{"connectors":[]}
```

Parses connector config, persists, and echoes `{"connectors":[...]}`. Changing
`enabled` never launches an authentication helper.

Automatic connector authentication is gated by the versioned
`connector_first_start_auth_v1` flag in the daemon runtime `state.json`. The
first daemon startup that durably claims this flag launches each enabled
connector's `hooks.on_startup` helper once; later daemon startups never launch
automatic connector authentication. The claim is persisted before any helper
starts. If it cannot be persisted, helpers are skipped. Helper threads are
joined before daemon teardown.

For configuration compatibility, `hooks.on_enable`, `hooks.on_auth_error`, and
`auth_error_scope` are still parsed and serialized, but they are inert: neither
a settings toggle nor an HTTP 400/401 model response executes them.

### `GET /api/config/default-permission-mode`

Returns the permission mode used by newly-created sessions:

```json
{"mode":"accept-edits","description":"Auto-allow file edits, prompt for bash"}
```

### `PUT /api/config/default-permission-mode`

Body:

```json
{"mode":"accept-edits"}
```

Persists the default and updates the in-memory session registry default.

### `GET /api/config/upgrade`

Returns:

```json
{"base_url":"https://example.com/acecode"}
```

### `PUT /api/config/upgrade`

Body:

```json
{"base_url":"https://example.com/acecode"}
```

Normalizes and validates a non-empty HTTP(S) base URL.

### `GET /api/update/status`

Checks the update manifest and returns:

```json
{
  "status": "available",
  "update_available": true,
  "current_version": "0.5.10",
  "latest_version": "0.5.11",
  "target": "windows-x64",
  "manifest_url": "https://example.com/manifest.json",
  "package_file": "acecode.zip",
  "package_url": "https://example.com/acecode.zip",
  "package_size": 123456,
  "releases": [
    {
      "version": "0.5.11",
      "published_at": "2026-07-20T08:00:00Z",
      "notes": "1. 新增版本更新记录。\n2. 优化升级稳定性。"
    },
    {
      "version": "0.5.10",
      "published_at": "2026-07-12T08:00:00Z",
      "notes": "修复 Desktop 自动重启问题。"
    }
  ]
}
```

`releases` preserves manifest order and contains only release metadata; package
lists, hashes, and URLs are not duplicated into each entry. Legacy entries with
missing notes are returned with `notes: ""`. `http_status` and `error` are
included when present.

`status` can also be `up_to_date`, `no_compatible_package`, `invalid_config`,
`unsupported_target`, `manifest_unavailable`, or `manifest_invalid`.
`no_compatible_package` means the manifest contains a newer semantic version but
does not publish a package for this client's updater-capability target; it is not
an up-to-date result. Linux keeps the user-facing `target` as `linux-x64` or
`linux-arm64` while matching packages through the internal `updater-v1` target.

Manifest checks and package transfers do not use a fixed total request timeout.
Transport, HTTP, and file-write failures are still reported normally.

Checks and upgrades append flushed JSON diagnostic records to the ACECode data
directory's `logs/upgrade-YYYY-MM-DD-PID.log` (UTC date). Each attempt has an
`operation_id`; GUI precheck, worker, and installer records share that id, and
GUI records also include `job_id`. Records include phase, elapsed milliseconds,
versions, paths, HTTP/transport status, download byte counts, checksum values,
file backup/replacement and rollback results, and the terminal outcome. Download
progress is sampled at most once every five seconds. URL credentials, queries,
and fragments are redacted; configuration objects and response bodies are not
logged. Logs survive cancellation, retries, and process restart. Desktop restart
preflight, shutdown, and replacement launch remain in `logs/desktop-<date>.log`.
User-triggered diagnostic feedback bundles (TUI `/feedback` and
`POST /api/feedback/desktop`) merge every upgrade log written during the last
three days (72 hours by file modification time) into one
`logs/upgrade.log.tail.txt` entry, concatenated oldest to newest so the entry
reads as a timeline. The existing log-tail size limit applies to the merged
result, dropping the oldest records first. When no upgrade log was written in
that window the entry is absent; older logs are not used as a fallback.

Check and job responses include `log_path` when a log was created, and
`log_error` if diagnostics are unavailable or incomplete. Logging failures do
not change the upgrade outcome. Failure text includes the underlying error and
available log path so existing clients can display actionable diagnostics.

### `POST /api/update/start`

Checks for an update and starts one daemon-managed background update job. The
job reuses the normal upgrade engine without creating a console window. Returns
`202`:

```json
{
  "started": true,
  "job_id": "20260712-120000-abcd",
  "state": "pending",
  "phase": "checking",
  "current_version": "0.6.8",
  "target_version": "0.6.9",
  "bytes_downloaded": 0,
  "bytes_total": 33554432,
  "percent": 0,
  "restart_required": false,
  "cancel_requested": false,
  "can_cancel": true
}
```

Returns `409 NO_UPDATE` when the running version is already current. Returns
`409 NO_COMPATIBLE_PACKAGE` when newer releases exist but the service has no
package for this updater-capability target; the nested status object includes
`status: "no_compatible_package"`, `latest_version`, the physical `target`, and
an actionable `error`.

Other precheck failures retain HTTP `409` and the existing `NO_UPDATE` code for
compatibility, but `message` now contains the specific check failure and log
location. The response includes top-level `log_path` and the detailed `status`
object. Failure to create the worker thread returns `500 UPDATE_START_FAILED`
with diagnostic `message` and failed `job`.

Returns `409 UPDATE_IN_PROGRESS` when another job is pending or running. The
response includes that job under `job`, so another WebUI tab can attach to it.

When the daemon already holds a successful job with `restart_required: true`,
returns `202` with that same job and `started: false`, without fetching or
installing another package. This remains true while the old daemon still
reports its previous version. Failed and cancelled jobs remain retryable.

Package installation verifies the staged backend's `--version` output against
the selected release with a bounded direct child process. Flat Windows/Linux
packages also verify the installed backend before reporting success; a failed
post-copy verification rolls back the installation. Version mismatch, timeout,
invalid output and unsuccessful probe exit fail the job with an actionable error.

On macOS, a daemon running from `ACECode.app/Contents/MacOS/acecode-daemon`
installs a complete
`ACECode.app` update ZIP rather than copying files into `Contents/MacOS`. Before
replacement, the daemon requires an absolute, canonical, real `ACECode.app`
with a real, writable containing directory, a strict nested Apple signature, bundle
identifier `dev.acecode.desktop`, the selected manifest version, and the same
Developer Team ID and designated signing requirement as the installed app.
Custom folders are supported as well as `~/Applications` and `/Applications`;
App Translocation, symlinked paths, and apps nested in another `.app` are rejected.
Read-only or otherwise unwritable locations require moving the app to a writable
folder or installing manually; the updater does not elevate privileges. This does
not change the separate personal-install destination policy.

### `GET /api/update/job`

Returns the latest update job retained by the daemon. This lets a reloaded page
recover an active, completed, or failed dialog. Returns `404
UPDATE_JOB_NOT_FOUND` before any job has been started.

### `GET /api/update/jobs/:id`

Returns structured progress for one update job. `state` is `pending`, `running`,
`succeeded`, `cancelled`, or `failed`; `phase` is `checking`, `downloading`,
`verifying`, `extracting`, `installing`, `complete`, or `cancelled`.
`cancel_requested` records an accepted cooperative cancellation request, while
`can_cancel` is true only while the active job can still stop without replacing
installed files. A successful job sets
`restart_required` to `true` because the current daemon and desktop shell remain
the already-running version until ACECode is fully restarted. Failed jobs
include `error` and may be retried with a new `POST /api/update/start`.

### `POST /api/update/jobs/:id/cancel`

Requests cooperative cancellation of an active update job. Returns `202` after
the request is accepted. Downloading stops and temporary package/staging files
are removed before the job enters the `cancelled` terminal state. A cancelled
job keeps `restart_required: false`.

The endpoint returns `409 UPDATE_NOT_CANCELLABLE` after the job reaches the
`installing` phase, because interrupting file replacement could leave an
incomplete installation. Repeating the request for the same already-cancelled
job is idempotent and returns `200`; an unknown job returns `404
UPDATE_JOB_NOT_FOUND`.

In the native desktop shell, a successful job asks whether to restart now. The
restart-now action uses the in-process desktop bridge to bypass close-to-tray,
stop the shell's managed daemon processes and tray resources, release the
single-instance guard, and launch the newly installed desktop executable.
Upgrade restart always stops and waits for the managed backend, including when
background continuation is enabled; it does not change that saved preference.
A failed backend shutdown prevents replacement launch. On startup, Desktop
reuses an owned backend only if its application version and executable
installation match, recovering old backends preserved by earlier releases.
Choosing restart later leaves the current process running. Normal browser and
Edge-app compatibility clients do not own the desktop lifecycle, so they show
manual full-exit-and-relaunch guidance instead of an automatic restart action.
For a successful macOS bundle update, `backup_dir` identifies the retained
`.ACECode-<UUID>.previous.app` beside the running installation. Existing backups
are not deleted or overwritten; retained backups require manual cleanup when no
longer needed. The updater lock is opened without following symlinks and must be
a regular, singly linked file owned by the current user.

### `GET /api/mcp`

Reads global `mcp_servers` by default. Add `?workspace=<registered-workspace-hash>`
to read only that project's `.acecode/mcp.json` entries. The same optional query
applies to PUT, toggle, and reload. Unknown workspaces return `404`; filesystem
paths are not accepted as workspace identifiers. `auth_token` is not returned.
All MCP endpoints require the server's normal authentication and return
`Cache-Control: no-store`.

```json
{
  "server-name": {
    "transport": "stdio",
    "command": "node",
    "args": ["server.js"],
    "env": {},
    "url": "",
    "sse_endpoint": "/sse",
    "headers": {},
    "timeout_seconds": 30
  }
}
```

### `PUT /api/mcp`

Replaces the selected scope's server map. The body is an object keyed by server
name, without a `mcp_servers` wrapper. Project files on disk use the wrapper;
project definitions override matching global names only in that project,
including disabled definitions. An empty project map removes those overrides.

The raw body must pass the ACECode configuration JSON Schema before any
persistence, in-memory publication, or runtime update. Omitted `auth_token`
fields preserve previously saved tokens for the same server; an explicit empty
string clears a token. Successful writes reconcile the runtime when available:

```json
{"saved":true,"reload_required":false,"applied":true}
```

Without a runtime, saving still succeeds with `applied:false` and
`reload_required:true`. Persistence failures return `500` without publishing the
candidate to application configuration. Invalid JSON or schema violations
return `400` with the complete schema and JSON Pointer diagnostics:

```json
{
  "error": "MCP_CONFIG_INVALID",
  "message": "MCP configuration failed schema validation",
  "errors": [{"path": "/example/command", "message": "must be string"}],
  "schema": {"$schema": "http://json-schema.org/draft-07/schema#", "title": "ACECode MCP server configuration"},
  "specification_url": "https://modelcontextprotocol.io/specification/2026-07-28/schema"
}
```

The schema above is abbreviated for documentation; responses include all
validation rules. Diagnostics do not echo rejected configuration values.

### `GET /api/mcp/schema`

Returns `schema` (the server map accepted by PUT) and `document_schema` (the
project file wrapper). These are ACECode client configuration schemas; the
linked MCP specification describes protocol messages. Validation works offline.

### `POST /api/mcp/toggle`

Body: `{"name":"example","enabled":false}`. Both fields are required with
their declared types. The complete resulting scope is validated and saved
before updating runtime state. Unknown names return `404`. Success:

```json
{"name":"example","enabled":false,"applied":true,"retained_for_expert":false}
```

An explicitly selected expert may retain its server connection after disabling
the default. Retention is scoped to the exact global/project owner.

### `POST /api/mcp/reload`

Re-reads the selected persisted configuration, validates it, and reconciles its
runtime registrations and session capability policies. Invalid external edits
restore the validated `<config-file>.mcp-last-good` snapshot and archive the
invalid input. Global recovery replaces only `mcp_servers`; project recovery
affects only that project file. If no valid snapshot exists, returns the schema
error and starts no servers from the invalid configuration. Returns `503` when
no runtime is available. Success:

```json
{"reloaded":true}
```

### `GET /api/feedback/desktop/recent-sessions?limit=N`

Returns recent sessions for optional feedback attachment. `limit` defaults to
`20` and is clamped to `1..100`.

```json
{"sessions":[{"id":"sid","session_id":"sid","title":"...","workspace_hash":"abc123"}]}
```

### `POST /api/feedback/desktop`

Body fields are optional strings:

```json
{
  "feedback_text": "Settings page froze",
  "session_id": "sid",
  "workspace_hash": "abc123"
}
```

The package carries the newest rotated Desktop shell log (`desktop-<date>.log`)
and the daemon log (`daemon-<date>.log`) that serves the request. Each is
truncated to its last 512 KiB and stored as `logs/desktop.log.tail.txt` /
`logs/daemon.log.tail.txt`. A runtime with no log file present is skipped
silently, so a browser-only deployment uploads the daemon log alone. Desktop
feedback does not attach `tui-<date>.log`; terminal `/feedback` instead carries
`tui-<date>.log` plus the daemon log and does not attach the Desktop log. Upgrade diagnostics
(`upgrade-<date>-<pid>.log`, one file per process) are handled as a window
rather than a single newest file: every upgrade log modified within the last
three days (72 hours) is merged oldest-first into `logs/upgrade.log.tail.txt`,
truncated as a whole to the same 512 KiB tail; with no upgrade activity in that
window the entry is omitted. If `session_id` is empty, the package contains
those logs only. The upload target is derived from `upgrade.base_url`.

Success:

```json
{
  "ok": true,
  "package_filename": "acecode-feedback-desktop-....zip",
  "log_included": true,
  "log_tail_bytes": 4312,
  "logs": [
    {
      "entry_name": "logs/desktop.log.tail.txt",
      "path": "/home/u/.acecode/logs/desktop-2026-06-18.log",
      "available": true,
      "tail_bytes": 1200
    },
    {
      "entry_name": "logs/daemon.log.tail.txt",
      "path": "/home/u/.acecode/logs/daemon-2026-06-18.log",
      "available": true,
      "tail_bytes": 3112
    }
  ],
  "included_files": ["logs/desktop.log.tail.txt","logs/daemon.log.tail.txt","feedback.json"],
  "selected_session_id": null,
  "workspace_hash": ""
}
```

`log_included` is true when at least one log made it into the archive, and
`log_tail_bytes` is the sum across all of them; `logs[]` reports each requested
source, including the ones that were unavailable. Upgrade log files merged into
`logs/upgrade.log.tail.txt` each get their own `logs[]` row sharing that
`entry_name`; a row's `tail_bytes` is what that file contributed after the
merged tail was truncated (0 when the cap dropped it entirely while
`available` stays true), and `included_files` lists the merged entry once. The
same array is mirrored into the archive's `feedback.json` under `logs`.

`feedback_text` accepts at most 10,000 Unicode code points, including spaces and
line breaks. Oversized text returns HTTP 400 with `FEEDBACK_TOO_LONG` before any
logs are collected, package is created, or upload is attempted.

Other errors include `SESSION_NOT_FOUND`, `PACKAGE_FAILED`, and `UPLOAD_FAILED`.

---

## 11. Console PTY

PTY endpoints are loopback-only because they execute shell input without the
agent tool permission gate. Non-loopback requests return `403` even with a
token.

### `GET /api/pty/shells`

Returns detected shell choices and the configured default:

```json
{
  "shells": [
    {"id":"powershell","label":"PowerShell","available":true,"needs_path":false,"path":"C:/Program Files/PowerShell/7/pwsh.exe","configured_path":"","probed":true,"usable":true,"probe_error":""}
  ],
  "default": "powershell"
}
```

The response also includes `resolved: {id,family,program,console_command,usable,fallback_reason}`.
`available` describes file discovery; `usable` reflects an actual launch probe when
`probed` is true. Unprobed choices are checked when selected. The default and new
PTY sessions use the same resolved terminal as the Agent's command tool.

### `PUT /api/console/config`

Body:

```json
{"default_shell":"powershell","shell_path":"C:/mytool/pwsh.exe"}
```

Both fields are optional. `shell_path` applies to the selected `default_shell`;
an empty path restores automatic discovery. Non-empty paths must be absolute
regular files and pass a launch probe. Unknown types, wrong JSON types, missing
files, WSL bash for the Git Bash type, and failed probes return `400` without
changing either the saved config or runtime. Persistence failures return `500`.
The legacy `git_bash_path` field remains accepted. Returns the same payload as
`GET /api/pty/shells`; running terminals retain their original process.

### Settings environment endpoints

These endpoints use the normal authenticated API access policy.

- `GET /api/config/toolchains` returns `{toolchains:[{id,label,dir,exists,anchor,applied}]}`,
  where `id` is `python`, `node` or `csharp`. `PUT` accepts a partial object of
  those directory strings. Empty clears a directory; non-empty values must be
  existing absolute directories. Validation errors return `400` with
  `INVALID_FIELD`, `DIRECTORY_NOT_ABSOLUTE` or `DIRECTORY_NOT_FOUND` and leave
  the complete previous configuration unchanged. Successful saves update the
  process PATH for subsequently launched tools, hooks, MCP servers and terminals.
- `POST /api/config/toolchains/detect` searches the original process PATH, skipping
  Windows Store aliases. Found directories replace those fields; missing tools
  preserve existing choices. It returns the same list plus `detected:{python,node,csharp}`.
- `GET /api/console/config` returns `{default_shell,shell_paths,resolved,candidates}`.
  Each candidate includes `id,label,family,available,needs_path,probed,usable,program,
  detected_path,configured_path,probe_error`. `POST /api/console/config/detect`
  reruns launch probes, persists a usable resolution and returns the same shape.
  Candidates depend on the host platform. Windows tries PowerShell 7, Windows
  PowerShell, Git Bash and cmd; POSIX uses the login shell and available alternatives.
- `POST /api/dialog/pick-folder` and `POST /api/dialog/pick-file` return `{path}`,
  or JSON `null` when cancelled. Native picker support must be enabled by the host;
  otherwise the response is `501 {error:"PICKER_UNAVAILABLE",message}` and the UI
  accepts a manually entered path.

### Data directory migration

`GET /api/config/data-dir` returns `effective_dir`, `default_dir`, `redirect_active`,
`redirect_target`, `migrated_at_ms`, `cleanup_pending` and `migration` (null or the
job below). Existing backups add `previous_dir` and `previous_size_bytes`. After
restart, a pending backup larger than 100 MiB also adds
`cleanup:{previous_dir,size_bytes}`.

`POST /api/config/data-dir/migrate` accepts `{target:"<absolute path>"}` and returns
`202` with a job containing `state`, `target`, `copied_bytes`, `total_bytes`, `error`,
`restart_required`, `started_at_ms` and `finished_at_ms`. Poll
`GET /api/config/data-dir/migration`; states are `running`, `done` or `failed`.
Before any job has started the poll endpoint returns `404 MIGRATION_NOT_FOUND`.

Target validation returns `400` with `TARGET_REQUIRED`, `TARGET_NOT_ABSOLUTE`,
`TARGET_SAME_AS_CURRENT`, `TARGET_INSIDE_CURRENT`, `TARGET_CONTAINS_CURRENT`,
`TARGET_NOT_A_DIRECTORY`, `TARGET_NOT_EMPTY` or `TARGET_NOT_WRITABLE`. Paths are
compared after canonicalization. A busy Agent returns `409 SESSIONS_BUSY`, another
live daemon returns `409 OTHER_INSTANCES_ACTIVE`, and open console terminals return
`409 CONSOLES_ACTIVE`.

The job pauses the scheduler and copies through a private staging directory,
excluding top-level `run/`, `tmp/`, the redirect pointer and lock files. SQLite
databases use online backups, including committed WAL transactions. Symlinks are
preserved (internal targets follow the new root); inability to preserve them fails
the copy. Source changes and a newly occupied target abort publication. Failure
removes only private staging, preserves the source and existing target files,
and re-enables writes. A pointer-write failure retains the copied target for recovery.

Success atomically publishes the copied directory, writes `data-dir.redirect.json`
in the platform default data directory, and sets `restart_required:true`. The
current process continues reading the old root; new turns and authenticated
mutations are rejected with `409 DATA_DIR_MIGRATION_ACTIVE` until restart.

`POST /api/config/data-dir/cleanup` accepts `{action:"keep"}` or `{action:"delete"}`.
Both acknowledge the cleanup prompt and return updated directory status. Delete
requires a restart into the new root, verifies the recorded old directory is not
the active root or its ancestor, and refuses a still-running old daemon. When the
backup is the default directory, its redirect pointer is preserved. Failures
return `409 NO_MIGRATION`, `409 SESSIONS_BUSY`, `500 CLEANUP_FAILED` or
`500 PERSIST_FAILED`; invalid actions return `400 INVALID_ACTION`.

### `POST /api/pty`

Body:

```json
{"cwd":"C:/repo","title":"Terminal","shell":"powershell"}
```

`shell` is a shell id from `/api/pty/shells`. The daemon enforces a 16-session
limit and returns `429` when exceeded.

Session info:

```json
{
  "id": "pty-1",
  "title": "Terminal 1",
  "shell": "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
  "cwd": "C:/repo",
  "status": "running",
  "pid": 12345,
  "backend": "conpty",
  "exit_code": 0
}
```

`exit_code` appears only when `status == "exited"`.

### `GET /api/pty`

Returns:

```json
{"backend":"conpty","sessions":[]}
```

### `DELETE /api/pty/:id`

Kills/removes the PTY session. Returns `204` or `404`.

### `POST /api/pty/:id/resize`

Body:

```json
{"cols":120,"rows":30}
```

`cols` and `rows` must be in `2..1000`. Returns `204`.

### `POST /api/pty/:id/title`

Body:

```json
{"title":"npm run dev"}
```

Used by the frontend to persist xterm OSC title changes. Returns `204`.

---

## 12. Session WebSocket

Route:

```text
WS /ws/sessions/:route?token=<token>
```

The frontend currently connects to `/ws/sessions/_multiplex`. The route
parameter is not the session id; sessions are bound by JSON messages after the
socket opens.

### Server event envelope

Session events are JSON objects:

```json
{
  "type": "token",
  "seq": 1,
  "timestamp_ms": 1783152000000,
  "session_id": "sid",
  "workspace_hash": "abc123",
  "payload": {}
}
```

Most session event frames include a per-session `seq` and `timestamp_ms`.
The exceptions are pending `permission_request` and `question_request`
snapshot frames sent immediately after `subscribe_ack`. These replay frames
intentionally have no `seq` so clients do not advance or warn on the reconnect
cursor. Clients should de-duplicate both interaction types by
`payload.request_id` and retain resolved tombstones until the owning turn is
terminal so a delayed snapshot cannot reopen a closed request.

Each active session retains at most 1024 replay events and an estimated 8 MiB
of event payload/container storage. The oldest retained events are evicted when
either limit is reached. A single event larger than the byte budget is delivered
live but is not retained for replay. `since` only replays retained events; it does
not guarantee recovery of older or oversized frames. Load the session history
through REST when a complete persisted transcript is needed.

Session event `type` values from `SessionEventKind`:

- `token`
- `reasoning`
- `agent_progress`
- `message`
- `tool_start`
- `tool_update`
- `tool_end`
- `permission_request`
- `permission_closed`
- `question_request`
- `question_closed`
- `usage`
- `transcript_replace`
- `goal_updated`
- `goal_cleared`
- `todo_updated`
- `session_updated`
- `busy_changed`
- `done`
- `error`

For a successful `task_complete` call, the `tool_end` payload also includes
`message_id`, the canonical id of the persisted tool-role result. Live and
trajectory/replay records use the same id so clients can attach copy, fork, and
other message actions to the completion summary without relying on its
synthetic display id.

Completed tool text above the per-result limit (30,000 bytes for Bash, 50,000
bytes for other tools) is saved under the session's `tool-results` directory
before `tool_end` or the result message is broadcast. Their output contains the
existing `<persisted-output>` file reference and a 2,000-byte preview. Structured
file-diff hunks, metadata and attachments remain available. The aggregate
model-context budget still applies later. As with the existing result budget,
if storage fails the original completed result is retained rather than discarded;
the preview reduction is therefore not guaranteed during storage failures.

`tool_end.hunks[]` entries carry `old_start`, `old_count`, `new_start`,
`new_count` and `lines[]`. A multi-file result (the `apply_patch` tool used by
GPT / Codex models) additionally sets `file` (absolute path), `additions` and
`deletions` on every hunk so clients can group hunks per file; single-file
results from `file_edit` / `file_write` omit these keys and clients keep using
`summary.object` as the file name. `apply_patch` results also expose
`metadata.files[]` with `path`, `type` (`add` | `update` | `delete` | `move`),
optional `move_path` / `from_path`, `additions` and `deletions`. The same hunk
shape is persisted under the tool message's `metadata.tool_hunks`.

The start of a regular agent turn includes
`{"busy":true,"turn_id":"initial-user-message-uuid"}`. That id stays stable
across tool calls, model retries, and accepted steering input. For the terminal
transition, `busy_changed` includes
`{"busy":false,"outcome":"completed|error|aborted","turn_id":"..."}`
and the following `done` frame repeats the same `outcome`. Other busy cycles
such as compaction may omit it. Clients should only treat `completed` as a
successful turn.

Transient pure-sampling failures use `agent_progress` rather than transcript
messages. While waiting, the payload is:

```json
{
  "phase": "model_retry",
  "label": "网络暂时不可用，等待重试",
  "retry_attempt": 12,
  "retry_delay_ms": 1200000,
  "retry_at_ms": 1783153200000,
  "retry_max_attempts": -1
}
```

`retry_max_attempts: -1` means the count is unbounded. Immediately before the
next attempt, another `agent_progress` frame changes `phase` back to
`model_waiting` (or `compacting`) and sets `retry_delay_ms` to zero. The retry
wait is cancellable through the existing abort/stop path. A replay also emits
`transcript_replace` so provisional text, reasoning, usage, and tool-call
fragments from the failed attempt disappear without becoming conversation
history.

`transcript_replace` is for retry/recovery cleanup. Normal compact success
appends visible marker messages and a hidden checkpoint instead.

Visible messages belonging to one compact operation carry lifecycle metadata:

```json
{
  "transcript_only": true,
  "compact_notice": true,
  "compact_notice_id": "019f85aa-3a00-7000-8000-000000000005",
  "compact_notice_stage": "progress|checkpoint|summary|warning|error",
  "compact_notice_complete": false
}
```

Manual and automatic compaction reuse one UUIDv7 `compact_notice_id`. Only the
terminal warning of a successful operation sets `compact_notice_complete` to
`true`; failures remain incomplete. Clients may therefore show incoming details
while the operation runs and replace a completed group with one expandable
`Context compacted` row without changing append-only transcript persistence.

### Client messages

All client frames are JSON:

```json
{"type":"subscribe","payload":{"session_id":"sid","since":42}}
```

| Type | Payload | Behavior |
|---|---|---|
| `hello` | `{session_id,since}` | legacy bind; ack is `hello_ack` |
| `subscribe` | `{session_id,since}` | subscribes one session; ack is `subscribe_ack`; may then send child status discovery and seq-less pending permission/question snapshots |
| `unsubscribe` | `{session_id}` | unsubscribes; ack is `unsubscribe_ack` |
| `status_subscribe` | `{workspace_hash}` or `{session_id}` | subscribes workspace attention status and sends snapshot |
| `status_unsubscribe` | `{workspace_hash}` | unsubscribes; ack is `status_unsubscribe_ack` |
| `mark_session_read` | `{session_id,workspace_hash,cursor}` | persists read cursor; ack is `mark_session_read_ack` |
| `user_input` | `{session_id,text}` | queues plain user input |
| `decision` | `{session_id,request_id,choice}` | responds to permission request; `choice` is `allow`, `deny`, `allow_session`, `allow_scoped`, or `allow_remember` |
| `question_answer` | `{session_id,request_id,cancelled,answers}` | responds to AskUserQuestion |
| `abort` | `{session_id}` | aborts current turn |
| `ping` | `{}` | replies `{"type":"pong"}` |

`decision` uses `choice`, not `decision`, in the payload.

For `bash`, `permission_request.args` retains the original tool arguments and
adds a server-generated `permission` object:

```json
{
  "command": "pnpm install",
  "sandbox_permissions": "require_escalated",
  "justification": "Install dependencies using the shared cache.",
  "permission": {
    "reason": "escalation_requested",
    "request": "require_escalated",
    "sandbox": "full-access",
    "always_allow_prefix": "pnpm install",
    "proposed_prefix_rule": "pnpm install",
    "scoped_write_root": "/home/u/.cache/pnpm",
    "denied_path": "/home/u/.cache/pnpm/store.lock",
    "classification": "unknown"
  }
}
```

`reason` can be `dangerous_command`, `escalation_requested`,
`additional_permissions_requested`, `unknown_command_without_sandbox`,
`rule_prompt`, `default_mode`, or `plan_mode`. `request` echoes the model's
`sandbox_permissions` value (`use_default`, `with_additional_permissions`, or
`require_escalated`; the legacy `with_escalated_permissions=true` maps to
`require_escalated`). `sandbox` describes the approved execution boundary:
`full-access`, `workspace-write`, or `read-only`. An empty `always_allow_prefix`
means no session approval can be remembered. Shell interpreters and opaque
scripts do not acquire broad session approval. Remembered sandbox approvals
never become full access when a backend becomes unavailable.

Optional fields only appear when the corresponding choice is available:

- `additional_permissions` (`{read:[],write:[],network:bool}`) lists what a
  `with_additional_permissions` request asks for; `allow_session` then keeps
  those grants for the session instead of remembering a command prefix.
- `scoped_write_root` is offered on escalation requests when the previous
  sandboxed run was denied on a known path outside the deny list; `allow_scoped`
  grants write access to only that directory for the session and runs the
  command inside the `workspace-write` sandbox.
- `denied_path` is the path extracted from that previous denial, for display.
- `proposed_prefix_rule` is the prefix `allow_remember` appends to the global
  rules file (`<data_dir>/rules/default.rules` for approvals that leave the
  sandbox, `default.sandboxed.rules` otherwise). It is absent for interpreters,
  `rm`, `sudo`, and other banned prefixes.

Clients that send `allow_scoped` or `allow_remember` for a request that did not
offer them are treated as `allow` and `allow_session` respectively.

Denied sandboxed runs return `metadata.sandbox_violation`
(`{reason, path?, snippet}`) alongside `metadata.sandbox_denied`; `reason` is
one of `operation_not_permitted`, `permission_denied`, `read_only_file_system`,
`access_denied`, `policy_denied`, `failed_to_write_file`, or `sigsys`.

The built-in `sandbox` command supports empty args (status), `off`, and `on`
through the existing session command endpoint. It is session-local and does not
save configuration. See [sandbox.md](sandbox.md) for rules and platform limits.

Each `permission_request` is followed by exactly one sequenced
`permission_closed` event with `{request_id,choice,reason}` when it stops being
actionable. `choice` is `allow`, `deny`, `allow_session`, `allow_scoped`, or
`allow_remember`; `reason` is `decision`, `permission_mode_change`, `abort`, or
`timeout`. A timeout still
emits the existing `error` event with `reason:"permission_timeout"` after the
close event.

`question_answer.answers[]` entries are:

```json
{
  "question_id": "q1",
  "selected": ["option-id"],
  "custom_text": "free form"
}
```

Completed AskUserQuestion results carry UI-only
`metadata.ask_user_question_result` in `tool_end` events and persisted tool
messages. Successful answers contain ordered `items` with `question` and
`answer` text. Explicit rejection contains `{"cancelled":true,"items":[]}`;
its `success` remains `false` and its provider-visible output remains
`[Error] User declined to answer questions.`. Clients can use this namespaced
marker to retain cancellation feedback after reloading history. A generic
tool's unrelated `metadata.cancelled` flag is not question-result metadata.
A question resolved by a free-form interjection carries
`{"interjected":true,"items":[]}` with `success:true`; its provider-visible
output starts with `[User interjected]` and the interjection itself is the
next persisted user message.

#### AskUserQuestion answer policy (`agent_loop.question_policy`)

`question_request` / `question_closed` behavior depends on the configured
answer policy (`config.agent_loop.question_policy`, or the
`--question-policy` CLI override):

- `ask` (default): `question_request` is emitted and the turn blocks until
  `question_answer` arrives (or the turn is aborted). Unchanged behavior.
- `deny`: no `question_request` is emitted at all. The tool returns an
  automatic answer instructing the model to decide autonomously.
- `timeout`: `question_request` is emitted normally; if no `question_answer`
  arrives within `question_timeout_seconds`, the daemon closes the question
  with `question_closed` `reason:"timeout"` and the tool auto-adopts the
  first (recommended) option of each question. A `question_answer` arriving
  after the timeout is ignored (unknown `request_id`).

YOLO affects tool permission confirmations only; it does not change the
question policy, so `AskUserQuestion` remains interactive in YOLO. While an
active `/goal` is running, each question uses a per-call 30-second timeout
regardless of the configured timeout value, then auto-adopts the first
(recommended) option.

`question_closed.reason` values: `answered`, `cancelled`, `aborted`,
`timeout`, `interjected` (resolved through
`POST /api/sessions/:id/questions/interject`). Frontends must dismiss the
question modal on any `question_closed` for the pending `request_id`.

When more than one session is subscribed, session-targeted messages should
include `payload.session_id`.

### Acks and status messages

Subscribe ack:

```json
{
  "type": "subscribe_ack",
  "session_id": "sid",
  "workspace_hash": "abc123",
  "payload": {"session_id":"sid","workspace_hash":"abc123","cwd":"C:/repo"}
}
```

For a subagent session, both the ack envelope and payload include the additive
`parent_session_id` field:

```json
{
  "type": "subscribe_ack",
  "session_id": "child-sid",
  "parent_session_id": "parent-sid",
  "payload": {
    "session_id": "child-sid",
    "parent_session_id": "parent-sid",
    "workspace_hash": "abc123",
    "cwd": "C:/repo"
  }
}
```

Subscribing to a parent session also registers it for status delivery. The
server sends current `session_status` frames for that parent's child sessions
after the ack, and future child status broadcasts are delivered through the
parent subscription even when there is no workspace subscription. Child status
envelopes and payloads include `parent_session_id`; unrelated parent
subscriptions do not receive them.

Workspace status snapshot:

```json
{
  "type": "session_status_snapshot",
  "timestamp_ms": 1783152000000,
  "workspace_hash": "abc123",
  "payload": {
    "workspace_hash": "abc123",
    "sessions": [
      {
        "session_id": "sid",
        "workspace_hash": "abc123",
        "cwd": "C:/repo",
        "state": "read",
        "attention_state": "read",
        "read_state": "read",
        "busy": false,
        "cursor": 0,
        "update_cursor": 0,
        "read_cursor": 0
      }
    ]
  }
}
```

Live attention updates use `type:"session_status"` with the same payload shape
for one session.

### Reconnect strategy

1. Store the highest processed `seq` per session.
2. Reconnect and send `subscribe` with `since:<lastSeq>`.
3. The daemon replays buffered events with `seq > since`.
4. If the replay gap is too old, fall back to
   `GET /api/sessions/:id/messages?since=0`.

Seq-less pending `permission_request` and `question_request` snapshots do not
affect the reconnect cursor; handle them by `request_id`.

---

## 13. PTY WebSocket

Route:

```text
WS /ws/pty/:id?cursor=N&token=<token>
```

This socket is loopback-only. Unlike the session socket, it is a raw byte
transport, not JSON envelopes.

Server to client:

- Binary frames are PTY output bytes.
- Frames whose first byte is `0x00` are UTF-8 JSON control frames, such as
  `{"cursor":123}` after backlog replay or `{"exit_code":0}` on exit.

Client to server:

- Text or binary frames are written verbatim to PTY stdin.

Each PTY session keeps a 2 MB rolling output buffer with a monotonic byte
cursor. `cursor=N` replays from that offset; `cursor=-1` skips backlog.
Resize uses `POST /api/pty/:id/resize`, not the WebSocket.

---

## 14. LOOP scheduling

LOOP (Chinese UI: “循环”) is a daemon-owned scheduler. It persists to
`<acecode_dir>/scheduled-loops.sqlite3` and continues running without an open
browser. Clients configure friendly period/interval/once fields; the compiled
schedule expression is internal and is never returned by the API.

Routes (all use the normal daemon auth and CORS rules):

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/api/loops` | List LOOP definitions |
| `POST` | `/api/loops` | Create a LOOP (`201`) |
| `GET` | `/api/loops/:id` | Read one LOOP |
| `PUT` | `/api/loops/:id` | Replace one LOOP |
| `DELETE` | `/api/loops/:id` | Delete one LOOP and its run history |
| `PUT` | `/api/loops/:id/enabled` | Enable/disable with `{"enabled":true}` |
| `GET` | `/api/loops/:id/runs?limit=N` | Recent run history (`1..500`) |

Each item returned by `GET /api/loops` includes a nullable `latest_run` field.
When present, it is the newest run-history object for that LOOP, including its
current `status` and timestamps. Clients may poll this route to refresh running,
waiting, completed, missed, and failed states without keeping the details panel
open.

Create/update body:

```json
{
  "name": "Daily code review",
  "prompt": "Review recent changes and run relevant tests.",
  "workspace_hash": "0123456789abcdef",
  "workspace_cwd": "C:/repo",
  "model_name": "gpt-5.5-codex",
  "permission_mode": "yolo",
  "use_worktree": false,
  "enabled": true,
  "schedule": {
    "kind": "period",
    "period": "workdays",
    "weekdays": [],
    "hour": 9,
    "minute": 0,
    "valid_from_ms": null,
    "valid_until_ms": null
  }
}
```

`workspace_hash` and `workspace_cwd` must either both be present and resolve to
the same registered workspace, or both be empty for a no-workspace LOOP.
`use_worktree` is a boolean and defaults to `false` for new definitions. LOOP
databases upgraded from the pre-option schema preserve existing definitions as
`true`; clients editing a definition should return the value they read.
Supported schedules are:

- `period`: `period` is `daily`, `workdays`, or `weekly`; weekly additionally
  uses `weekdays` (`0=Sunday ... 6=Saturday`), plus `hour` and `minute`.
- `interval`: `interval_value`, `interval_unit` (`minutes`, `hours`, `days`),
  and `anchor_ms`.
- `once`: `once_at_ms`.

`timezone_offset_minutes` is optional and defaults to the daemon's current
local offset. `valid_from_ms` / `valid_until_ms` are optional for every kind.
Raw Cron/dialect values are deliberately absent from public responses.

Run statuses are `scheduled`, `running`, `waiting_user`, `completed`, `failed`,
and `missed`. Missed occurrences are recorded and never queued or caught up.
Common reasons include `daemon_offline`, `workspace_busy`,
`daemon_interrupted`, `model_unavailable`, and `workspace_unavailable`. A due
LOOP is also recorded as `missed/workspace_busy` if another LOOP run for the
same workspace is active.

Creation, update, and re-enable return `409 SCHEDULE_CONFLICT` when two enabled
LOOPs in the same workspace have a future occurrence at the same minute. The
payload includes `conflict.loop_id`, `loop_name`, and `first_conflict_at_ms`.
No-workspace LOOPs are exempt.

Execution creates an ordinary visible session with `loop_execution` provenance.
When `use_worktree` is `false`, the task runs directly in the selected workspace.
When it is `true` and the workspace is a Git repository, the daemon creates an
isolated worktree; creation failure fails the run and never falls back to direct
writes. Non-Git workspaces run directly. LOOP never merges, rebases, pushes, or
removes a worktree; the final assistant response asks the user whether to merge
only when a worktree was actually created.

Permission behavior is per LOOP session: `default` preserves normal blocking
permission and AskUserQuestion prompts; `yolo` skips all tool permission prompts
but keeps AskUserQuestion interactive. LOOP Yolo may read outside the active work
root, but direct file writes and statically detectable shell writes outside that
root are rejected by the execution boundary without opening a permission prompt.

Sub-agents spawned from a LOOP session inherit the same execution boundary and
`loop_execution` provenance. Sub-agents spawned from any worktree session share
the parent's worktree (`worktree_session.inherited=true` in their metadata) and
use it as their write root; `EnterWorktree` / `ExitWorktree` refuse inside them.
Run history objects carry `workspace_touched`: paths that appeared as new or
changed in the main checkout, outside the run's worktree, between run start and
run end (detected with `git status --porcelain`). It is empty when no worktree
was created, when git could not be queried, or when nothing outside the worktree
changed. A non-empty list means some write bypassed the tool-level boundary (for
example through a shell script); the run still completes and the UI shows a
warning. `spawn_subagent` / `wait_subagent` tool results append the same
detection for one sub-agent as `metadata.workspace_touched`.

Error codes include `LOOP_UNAVAILABLE` (`501`), validation codes such as
`INVALID_MODEL` / `INVALID_WORKSPACE` (`400`), `SCHEDULE_CONFLICT` (`409`), and
SQLite subsystem failures (`503`).

---

## 15. Static Web App

The daemon also serves the built frontend:

- `GET /` serves the SPA entry.
- `GET /<path>` up to four path segments serves static files or falls back to
  the SPA entry.
- `/api/*` and `/ws/*` never fall back to the SPA; unmatched API/WS paths are
  `404`.
- Static responses send `Referrer-Policy: no-referrer`, so a bootstrap
  `?token=` URL is not propagated as a referrer.

---

## 16. HTTP Status Summary

| Status | Meaning |
|---|---|
| 200 | OK |
| 201 | Created |
| 202 | Accepted/queued |
| 204 | No content |
| 400 | Bad JSON, missing field, validation failure |
| 401 | Missing or bad daemon token |
| 403 | PTY non-loopback access |
| 404 | Unknown route, workspace, session, attachment, skill, or job |
| 409 | Conflict, busy session, active writer, no update, model in use |
| 415 | File preview unsupported, binary, or too large |
| 429 | PTY session limit |
| 500 | Persistence, command, package, restore, or internal failure |
| 501 | Feature unavailable or not implemented |
| 502 | Upstream probe, upload, or auth exchange failure |
| 503 | Required daemon subsystem unavailable |

---

## Windows desktop taskbar badge bridge

The embedded Windows desktop exposes `window.aceDesktop_setTaskbarBadge(payload)`
as a native bridge, separate from the daemon HTTP/WS API. It resolves to JSON
with an `ok` boolean. Browser, Edge app, and non-Windows hosts do not expose it.

For a visible badge, pass an integer `count` greater than zero and `background`,
`foreground`, and `outline` colors in `#RRGGBB` format. Counts above 99 display
`99+`. Passing `{ "count": 0 }` restores the original ACECode window icons.
Malformed input leaves the current icons unchanged and returns `ok: false`.

The frontend counts distinct unread main tasks from full workspace status
snapshots and workspace-free session lists. Windows subscribes status for all
registered workspaces, including collapsed projects, while task lists remain
lazy-loaded. Read acknowledgements and authoritative list refreshes remove read,
archived, and deleted tasks; running and child tasks do not contribute. Badge
colors come from the resolved theme tokens and update with theme changes.

## Task suggestions and session continuation

The Web/Desktop chat shows durable, source-session-scoped suggestion cards.
The daemon exposes `suggest_task(title, description, prompt)` and
`dismiss_task_suggestion(suggestion_id)` to the agent. Proposing a side task
only stores an offer; accepting it is a user action through the HTTP API.
The prompt should identify an independent finding, its evidence, scope, and
verification steps. There are at most three pending side-task offers per source;
duplicate and dismissed offers retain their identity across reloads.

All routes use the existing daemon authentication and CORS rules. Mutations
are rejected while data-directory migration is active.

| Method | Path | Request / result |
|---|---|---|
| GET | `/api/sessions/:source/suggestions` | `{suggestions, source_busy, workspace_busy, worktree_available}` |
| POST | `/api/sessions/:source/suggestions/:id/accept` | Exactly `{"location":"worktree"}` or `{"location":"current_branch"}`; returns `{suggestion}` |
| POST | `/api/sessions/:source/suggestions/:id/dismiss` | Returns `{suggestion}`; never stops an already started target |

Suggestion records contain `id`, `source_session_id`, `kind` (`side_task` or
`context_handoff`), `title`, `description`, `status`, and, after acceptance,
`location` and `target_session_id`. A started record includes `target_session`
for navigation. The states are `pending`, `queued`, `starting`, `started`,
`failed`, and `dismissed`. Accept returns HTTP 202 while startup is pending or
has failed and HTTP 200 for an already started target. Read the returned status
and `error`; a 202 response is not proof that model execution has begun.
Invalid request envelopes return 400; conflicting state/location returns 409;
an unavailable source or a host without this feature returns 404.

Acceptance durably reserves the target ID before provisioning. Repeated clicks
and failed-start retries reuse that ID and the selected location. Closing the
card or browser does not drive startup. Queued offers can be cancelled; startup
already in progress cannot be dismissed. A completed start can be hidden or
opened from the source card.

`worktree` pins the source's committed Git HEAD at acceptance. It does not copy
dirty or untracked source files, and missing Git/commit state fails without
falling back to the current checkout. `current_branch` uses the source session's
actual working directory, including an existing worktree. It waits for the
source's safe execution boundary and known work in other sessions sharing that
directory. This startup coordination does not lock the directory against later
manual user actions or external processes.

The optional top-level configuration `task_suggestion_compact_threshold`
defaults to 3 successful compactions; 0 disables continuation suggestions.
Accepted values are integers from 0 through 1000. It applies to newly created
or restored session loops. Automatic and manual completed checkpoints count;
repair/fallback checkpoints and failed attempts do not. The count survives
resume; an explicitly forked context starts a new count. Dismissing a
continuation offer suppresses it for that source session.

A continuation uses `current_branch` and a fresh session with a structured
reference to the source session. At the safe boundary the daemon captures a
bounded handoff containing the latest compaction summary, subsequent updates,
current goal/todos, and the source's execution configuration. The new model
does not receive the whole old transcript. It can retrieve supporting history
through the session reference. Once the first input is accepted, the old
session's automatic goal continuation is paused; queued user messages are not
discarded. The original history remains available.

## 17. Process Exit Codes

| rc | Where | Meaning |
|---|---|---|
| 0 | any | Normal exit |
| 1 | various CLI | Generic failure |
| 2 | `worker.cpp` | preflight bind check rejected |
| 3 | `worker.cpp` / `server.cpp` | Crow `app.run()` failed, usually port in use |
| 4 | `worker.cpp` | failed to write runtime file |
| 5 | `cli.cpp foreground` | config validation failed |
| 6 | `cli.cpp start` | another daemon already running |
| 7 | `cli.cpp start` | detached spawn failed |
| 8 | `cli.cpp start` | detached worker did not write pid in time |
| 9 | `cli.cpp stop` | terminate pid did not complete |
| 10 | `cli.cpp` / `service_win.cpp` | unknown subcommand |
| 11 | `cli.cpp` / `service_win.cpp` | missing subcommand |
| 21 | `service_win.cpp` | `--service-main` invoked outside SCM |
| 22 | `service_win.cpp` | `StartServiceCtrlDispatcher` failed |
| 24 | `service_win.cpp` | access denied, admin required |
| 25-33 | `service_win.cpp` | other SCM API failures |
| 64 | `main.cpp` | `--service-main` on non-Windows |
| 65 | `main.cpp` | `service` subcommand on non-Windows |

---

## 18. WhatsApp Channels

The daemon also hosts the optional WhatsApp channel runtime. Its control plane
is a separate private loopback listener, not a route on this Web API. See
[WhatsApp channels](channels.md) for standalone CLI configuration and management. The descriptor
`channels/whatsapp/owner.json` in the ACECode data directory contains protocol
version 1, PID, port and an owner token. `POST /channels` requires the
`X-ACECode-Channels-Token` header even on loopback and rejects browser Origin
headers. JSON operations are `status`, `qr`, `on`, `off`, `reconnect`, `pending`,
`approve` (pairing code), `allow`/`revoke` (JID), `sessions`, `show`, `send`, `file`
and `stop` (session ID). Do not publish this descriptor or its QR output.

`ping` reports protocol version 1. Configuration is not a daemon API operation:
legacy `setup_*` requests are rejected. `acecode channels` saves configuration
without checking or contacting running instances. Saved logins require no bridge;
first-time pairing uses an isolated, temporary pairing-only bridge and closes it
before completion. It never starts a host, creates an agent session, acquires the
runtime account lock or asks the user to stop an existing owner.
The main TUI has no channel command or surface.

On daemon/Desktop startup, configuration is read once. Disabled instances do not
claim ownership. The first enabled runtime claims the account lock and connects.
Later enabled runtimes remain standby without launching bridges until ownership
is released; takeover refreshes history but retains startup settings. Settings
in `config.json` are independent of owner-written `state.json`, including writes
from older binaries. Explicit CLI runtime commands never start
a missing host. `acecode channels status` can read saved settings without one;
its `host_running` field distinguishes offline configuration from a live owner.
