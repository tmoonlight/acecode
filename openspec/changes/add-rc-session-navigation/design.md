## Context

`SessionChannelBinder` owns exactly one active channel binding. Its inbound route currently forwards every accepted text to that session after the hub has queued the immediate acknowledgement. The daemon already persists `remote_control.bound_session_id`, can resume no-workspace sessions, and exposes a one-shot lightning surge when a sidebar row changes from unbound to bound.

The missing pieces are a daemon-global user-session catalog, a channel-only command parser, stable result numbering, exact-workspace resume, and a frontend notification. Numeric selection is initiated inside the current inbound callback, so implementation must not synchronously deactivate that callback's binding context while its lease is held.

## Goals / Non-Goals

**Goals**

- Discover all ordinary, unarchived user sessions stored under the ACECode projects root, including no-workspace sessions.
- Keep `/session`, `/sessions`, and `/resume` behavior identical and case-insensitive in RC mode.
- Make the default list ten rows, `all`/`more` unbounded by row count, and search at most five rows.
- Preserve a numbered snapshot shared by all aliases; numeric selection resolves against that snapshot rather than a newly sorted catalog.
- Resume inactive targets with their recorded `cwd`, `workspace_hash`, and `no_workspace` values before rebinding.
- Navigate every currently connected frontend to a successfully selected session without making frontend presence a prerequisite for the switch.
- Force the existing remote-control surge to be visible even if the target row mounts during cross-workspace navigation.

**Non-Goals**

- Multiple simultaneous channel bindings.
- Switching providers, connectors, or channel plugins.
- Listing internal sub-agent sessions or archived sessions.
- Changing TUI `/resume` semantics.
- Using a short-lived numeric index as a durable identifier across daemon restarts.

## Command Contract

All aliases use the same grammar after trimming surrounding whitespace:

```text
/session
/sessions
/resume
/sessions more
/sessions all
/sessions search <query>
/sessions <positive-number>
```

- Bare: rebuild and display the newest ten catalog entries.
- `more` / `all`: rebuild and display every catalog entry.
- `search <query>`: rebuild and display up to five ranked matches. Empty query returns usage guidance.
- Positive number: select from the most recently displayed snapshot. If no snapshot exists yet, build the default newest-ten snapshot first. Zero, malformed, or out-of-range numbers return a clear error without changing the binding.
- Unknown arguments return compact usage text and never enter the agent conversation.

Every displayed row contains its one-based number, title with a session-id fallback, workspace label (`无工作区` for no-workspace), and updated time. Long output may be emitted in bounded chunks, but numbering remains continuous.

Search matches title, summary, session id, workspace name/path, and indexed visible user-message content. Ranking is deterministic; ties use newest `updated_at` first.

## Concurrency And Lifecycle

The inbound route first recognizes the session command family and only enqueues an owned control operation. Listing, search, catalog scans, user-message index maintenance, and selection SHALL all run on the binder-owned control worker; none of them may block the RC HTTP inbound callback after the existing immediate acknowledgement. Selection must not call replacement while the old `BindingContext` lease is still counted, because replacement drains that context. Shutdown clears the route, rejects new control work, joins owned workers, and cannot access a destroyed binder.

The latest displayed snapshot is protected by binder-owned synchronization and stores complete target metadata, not only ids. A later list/search atomically replaces it. Binding generation checks continue to suppress stale outbound events.

## Frontend Event

After persistence and successful activation, the daemon broadcasts a generic event containing at least:

```json
{
  "type": "remote_control_session_selected",
  "payload": {
    "session_id": "...",
    "workspace_hash": "...",
    "cwd": "...",
    "no_workspace": false,
    "title": "...",
    "remote_control_bound": true
  }
}
```

The frontend treats it as an optional hint: refresh/upsert the session list, navigate with the existing resume/open helper, and force one lightning surge on the target row. If no frontend is connected, session switching still succeeds.

## UI Layout And Interaction

```text
IM conversation
┌──────────────────────────────────────────────────────────┐
│ /sessions                                                │
│ [ACE] 思考中...                                          │
│ [ACE] 最近会话                                           │
│   1. 修复登录阻塞 | acecode | 2026-08-05 10:20           │
│   2. 数据库查询     | 无工作区 | 2026-08-05 09:52         │
│   ...                                                    │
│ /sessions 2                                              │
│ [ACE] 成功发起远程连接，会话名：数据库查询                │
└──────────────────────────────────────────────────────────┘

Open ACECode frontend
┌──────────── sidebar ────────────┬──── selected chat ─────┐
│ Workspace A                     │ 数据库查询              │
│   修复登录阻塞                  │ existing transcript... │
│ No workspace                    │                         │
│  ⚡数据库查询  ← surge + bound  │                         │
└─────────────────────────────────┴─────────────────────────┘
```

Interaction rules:

- Navigation happens only after a successful bind; failures leave the current page and binding unchanged.
- The target row is revealed and receives the existing one-shot surge; its low-contrast bound background remains until another bind or `off`.
- Reduced-motion users keep the persistent bound background but do not receive forced animation.
- Multiple connected frontends may all follow the authoritative remote selection.

## Risks / Trade-offs

- Listing every session can produce a large IM response. Chunking prevents a single oversized outbound payload but intentionally honors the explicit `all` request.
- Numeric references are intentionally ephemeral. The snapshot rule prevents reordering mistakes within a daemon lifetime, while a restart rebuilds the default ten before first numeric selection.
- Full-text indexing can fail for one project. Search degrades to metadata matches for that project and logs the indexing problem without breaking other scopes.
