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

The daemon always uses the canonical loopback bind `127.0.0.1` and defaults to
port `28080` (`config.web.port`). It is fail-fast on a daemon-port collision.
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
  "session_id": "session-id",
  "workspace_hash": "abc123",
  "payload": {}
}
```

`payload.session_id`, `payload.workspace_hash`, and `payload.cwd` are injected
when known.

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
| GET | `/api/workspaces/:hash/sessions` | list sessions in workspace |
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
| GET | `/api/pinned-sessions/order` | read cross-workspace pin order |
| PUT | `/api/pinned-sessions/order` | set cross-workspace pin order |
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
| POST | `/api/sessions/:id/export-markdown` | choose a folder and export the visible transcript as Markdown |
| POST | `/api/sessions/:id/messages` | queue user input |
| POST | `/api/sessions/:id/turn/steer` | append input to the matching active turn |
| POST | `/api/sessions/:id/turn/interrupt` | interrupt the matching active turn and start a priority replacement turn |
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
| POST | `/api/sessions/:id/fork` | fork a transcript prefix |
| POST | `/api/sessions/:id/file-checkpoints/:message_id/restore` | restore files to checkpoint |
| GET | `/api/files` | list directory |
| GET | `/api/files/content` | read text file |
| GET | `/api/files/blob` | read previewable binary file |
| GET / PUT | `/api/files/editable` | read or safely save a Desktop workspace text file |
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
| PUT | `/api/models/:name` | update saved model profile |
| DELETE | `/api/models/:name` | remove saved model profile |
| POST | `/api/models/probe` | probe provider model ids |
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
| GET | `/api/config/ui-locale` | read Desktop/WebUI locale preference |
| PUT | `/api/config/ui-locale` | write Desktop/WebUI locale preference |
| GET | `/api/config/custom-instructions` | read custom instructions |
| PUT | `/api/config/custom-instructions` | write custom instructions |
| GET | `/api/config/connectors` | read connector settings |
| PUT | `/api/config/connectors` | write connector settings |
| GET | `/api/config/default-permission-mode` | read default permission mode |
| PUT | `/api/config/default-permission-mode` | write default permission mode |
| GET | `/api/config/remote-web` | read remote Web proxy state and connection URLs |
| PUT | `/api/config/remote-web` | enable or disable remote Web mode |
| GET | `/api/config/upgrade` | read update service config |
| PUT | `/api/config/upgrade` | write update service config |
| GET | `/api/update/status` | check update availability |
| POST | `/api/update/start` | start explicit WebUI update job |
| GET | `/api/update/job` | read latest WebUI update job |
| GET | `/api/update/jobs/:id` | poll one WebUI update job |
| POST | `/api/update/jobs/:id/cancel` | cancel one WebUI update job before installation |
| GET | `/api/mcp` | read MCP config |
| PUT | `/api/mcp` | write MCP config |
| POST | `/api/mcp/reload` | currently returns 501 |
| GET | `/api/feedback/desktop/recent-sessions` | list sessions for feedback attachment |
| POST | `/api/feedback/desktop` | package and upload desktop feedback |
| GET | `/api/pty/shells` | list console shell choices |
| GET | `/api/pty` | list PTY sessions |
| POST | `/api/pty` | create PTY session |
| DELETE | `/api/pty/:id` | remove PTY session |
| POST | `/api/pty/:id/resize` | resize PTY |
| POST | `/api/pty/:id/title` | set PTY title |
| PUT | `/api/console/config` | write console shell config |

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
  "port": 28080,
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

### `POST /api/workspaces/:hash/sessions`

Creates a session in the workspace. Body fields are optional:

```json
{
  "model": "saved-model-name",
  "name": "saved-model-name",
  "permission_mode": "default",
  "permissionMode": "default",
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

`GET /api/sessions` returns `SessionSummary[]`, not a wrapper object.

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

### Model-facing thread and workspace tools

Daemon, TUI, and headless runtimes expose the same in-process thread and
workspace tools to the model. They reuse the session/workspace registries and
storage directly; they do not call the daemon HTTP API:

| Tool | Behavior |
|---|---|
| `create_thread` | create a background thread and queue its initial prompt |
| `fork_thread` | fork completed persisted history into a new thread |
| `list_threads` | return pinned threads plus a bounded recent list |
| `read_thread` | read bounded, cursor-paginated turns |
| `send_message_to_thread` | queue a follow-up prompt |
| `wait_threads` | wait for up to eight targets using event cursors |
| `set_thread_title` | rename a thread |
| `set_thread_pinned` | update the existing pinned-session state |
| `set_thread_archived` | archive or unarchive a thread |
| `delete_thread` | permanently delete a thread and all descendants |
| `repair_thread` | append a deterministic repair checkpoint to another thread |
| `create_workspace` | register an existing absolute directory as a visible workspace |

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
| PUT | `.../sessions/:id/draft` | `{"text":"..."}` | `{"session_id","id","text"}` |
| DELETE | `.../sessions/:id/todos` | none | `{"session_id","id","workspace_hash","todos":[],"todo_summary":{...}}` |

Title writes trim whitespace and validate with `sanitize_title`.

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
endpoint is available only when the desktop native folder picker is enabled.
The optional body identifies the workspace when the session id is not globally
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

If the user cancels the folder picker, the response is `{"ok":true,"cancelled":true}`
and no file is created. Hidden goal context, compact checkpoints, and file
checkpoints are excluded from the export. The endpoint does not mutate the
session. Errors use `400` for invalid JSON or destination folders, `404` for
unknown sessions/workspaces, `501` when the native picker is unavailable, `503`
when its callback is unavailable, and `500` for file creation or write failures.

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

### `POST /api/sessions/:id/side-question`

Runs one isolated `/btw` or `/side` side question against the active session's
latest thread-safe provider-facing context snapshot:

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
- `409 SIDE_QUESTION_CONTEXT_NOT_READY`: the main loop has not yet published a
  safe provider-facing context snapshot.
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

Valid modes are `default`, `accept-edits`, `plan`, and `yolo`. Switching to
`yolo` also resolves any open permission prompt with allow. Returns the same
shape as `GET`.

### `POST /api/sessions/:id/fork`

Copies the source session prefix through `at_message_id` into a new session and
resumes it into the current daemon. It does not start a new turn.

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

### `GET /api/pinned-sessions/order`

Returns cross-workspace ordering:

```json
{"items":[{"workspace_hash":"abc123","session_id":"sid-1"}]}
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
- documents: `pdf`, `docx`, `xlsx`, `xlsm`

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
  "branches": ["master", "dev"],
  "dirty": false
}
```

- `branch` is `"HEAD"` when detached.
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

### `POST /api/models/probe`

Probes provider model ids. OpenAI-compatible providers call upstream
`GET /models`; Copilot uses saved GitHub auth; `provider:"grok"` uses the
daemon-managed xAI OAuth credentials and the fixed Grok Build `/v1/models`
endpoint. Anthropic model ids are entered manually and are not probed. Grok
catalog parsing accepts the official `id`、`model`、`modelId` and `_meta`
fallback shapes, ignores hidden entries, preserves first-seen order, and
deduplicates model ids.

ACEModel 官方端点的内置 `starrylight`、`moonlight` 和 `aurora`
统一使用 `200000` Token 上下文上限。当上游 `/models` 未返回
上下文时，Daemon 从内置目录回填 `model_context_windows`；上游值
超过 `200000` 时按该上限截断，更小的有效值则保留。

Success:

```json
{
  "models": ["gpt-4.1"],
  "model_context_windows": {"gpt-4.1": 1047576}
}
```

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
`starrylight`、`moonlight` 与 `aurora`，三者 `context_window` 均为 `200000`。Copilot 与 Grok Coding Plan 使用 `managed`，分别由
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

### `POST /api/sessions/:id/model`

Body:

```json
{"name":"saved-model-name"}
```

Switches the active session to that saved model profile and returns model
state. Returns `404` when the session is not active in the registry.

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
  "font_size": "medium"
}
```

`theme` accepts `system`, `light`, or `dark`; `color_theme` accepts `blue` or
`orange`; and `font_size` accepts `small`, `medium`, or `large`. These values
are stored in `~/.acecode/config.json`, so Desktop restores them even when its
managed daemon uses a different loopback port. The avatar preference is kept
for compatibility and is always normalized to `false`.

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
  "daemon_port": 28080,
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

Returns `409 UPDATE_IN_PROGRESS` when another job is pending or running. The
response includes that job under `job`, so another WebUI tab can attach to it.

On macOS, a daemon running from either the current-user
`~/Applications/ACECode.app/Contents/MacOS/acecode-daemon` location or the
supported system `/Applications/ACECode.app/Contents/MacOS/acecode-daemon`
location installs a complete
`ACECode.app` update ZIP rather than copying files into `Contents/MacOS`. Before
replacement, the daemon requires one of those exact non-symlinked install paths,
a writable containing directory, a strict nested Apple signature, bundle
identifier `dev.acecode.desktop`, the selected manifest version, and the same
Developer Team ID as the installed app. An app running from any other location
fails the job without mutating that bundle.

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
Choosing restart later leaves the current process running. Normal browser and
Edge-app compatibility clients do not own the desktop lifecycle, so they show
manual full-exit-and-relaunch guidance instead of an automatic restart action.
For a successful macOS bundle update, `backup_dir` identifies the retained
`.ACECode.previous.app` beside the running installation.

### `GET /api/mcp`

Reads `mcp_servers` from config. `auth_token` is intentionally not returned.

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

Overwrites `mcp_servers`. Body is an object keyed by server name. Success:

```json
{"saved":true,"reload_required":true}
```

### `POST /api/mcp/reload`

Currently returns `501`:

```json
{"error":"mcp reload not implemented in v1; restart daemon to pick up changes"}
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

The package always carries the newest rotated log of every runtime that writes
into the logs directory: the desktop shell (`desktop-<date>.log`) and the daemon
that serves the request (`daemon-<date>.log`). Each is truncated to its last
512 KiB and stored as `logs/desktop.log.tail.txt` / `logs/daemon.log.tail.txt`.
A runtime with no log file present is skipped silently, so a browser-only
deployment uploads the daemon log alone. If `session_id` is empty, the package
contains those logs only. The upload target is derived from `upgrade.base_url`.

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
source, including the ones that were unavailable. The same array is mirrored
into the archive's `feedback.json` under `logs`.

Errors include `SESSION_NOT_FOUND`, `PACKAGE_FAILED`, and `UPLOAD_FAILED`.

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
    {"id":"powershell","label":"PowerShell","available":true,"needs_path":false}
  ],
  "default": "powershell"
}
```

### `PUT /api/console/config`

Body:

```json
{"default_shell":"powershell","git_bash_path":"C:/Program Files/Git/bin/bash.exe"}
```

Both fields are optional. `git_bash_path` is trimmed, dequoted, checked for WSL
System32 bash, and validated if non-empty. Returns the same payload as
`GET /api/pty/shells`.

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
| `decision` | `{session_id,request_id,choice}` | responds to permission request; `choice` is `allow`, `deny`, or `allow_session` |
| `question_answer` | `{session_id,request_id,cancelled,answers}` | responds to AskUserQuestion |
| `abort` | `{session_id}` | aborts current turn |
| `ping` | `{}` | replies `{"type":"pong"}` |

`decision` uses `choice`, not `decision`, in the payload.

Each `permission_request` is followed by exactly one sequenced
`permission_closed` event with `{request_id,choice,reason}` when it stops being
actionable. `choice` is `allow`, `deny`, or `allow_session`; `reason` is
`decision`, `permission_mode_change`, `abort`, or `timeout`. A timeout still
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
`timeout`. Frontends must dismiss the question modal on any
`question_closed` for the pending `request_id`.

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
