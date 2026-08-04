# Channel Plugin Protocol

ACECode Channel protocol version 1 lets an external plugin attach a channel to
one ACECode session. The plugin is a separate executable described by an
`acecode.channel-plugin.v1` manifest. ACECode starts the executable once per
lifecycle request, writes one JSON object followed by a newline to stdin, and
reads an optional `channel.status` JSON object from stdout.

## Activation

ACECode sends:

```json
{
  "type": "channel.activate",
  "protocol_version": 1,
  "session_id": "session-1",
  "inbound": {
    "url": "http://127.0.0.1:28190/rc/send",
    "token_header": "X-ACECode-RC-Token",
    "token": "inbound-auth-token"
  },
  "outbound": {
    "preferred": "webhook"
  },
  "settings": {}
}
```

The plugin returns a connected webhook status:

```json
{
  "type": "channel.status",
  "state": "connected",
  "already_running": false,
  "binding_token": "opaque-binding-id",
  "outbound": {
    "mode": "webhook",
    "url": "http://127.0.0.1:39001/messages"
  }
}
```

`binding_token` is an optional Channel v1 extension. If present, it must be a
non-empty string. ACECode treats it as opaque, keeps it only in the current
runtime binding, and does not persist, display, or log it. A status that omits
the field remains valid for older plugins. Unknown status fields are ignored.
An empty or non-string `binding_token` rejects the status instead of silently
falling back to an unscoped legacy binding.

Activation is idempotent. A keepalive activation may return a new outbound URL
and a new binding token; ACECode installs both only if the activation still
belongs to the current binding generation.

## Deactivation

For a token-aware binding, ACECode echoes the exact token returned by the
matching activation:

```json
{
  "type": "channel.deactivate",
  "protocol_version": 1,
  "session_id": "session-1",
  "binding_token": "opaque-binding-id"
}
```

For a legacy binding, ACECode preserves the original request shape:

```json
{
  "type": "channel.deactivate",
  "protocol_version": 1,
  "session_id": "session-1"
}
```

A token-aware plugin must apply deactivation only when `session_id` and the
provided `binding_token` match its current binding. This prevents delayed
cleanup for token A from detaching a newer same-session binding with token B.
ACECode does not send a stale session-only cleanup after replacing a legacy
binding with another binding of the same session, because that request cannot
distinguish the old and new instances.

If a failed deactivation process echoes the binding token in an exception,
process diagnostic, request dump, or `channel.status.message`, ACECode replaces
the exact token before the error can reach logs, command results, or UI text.
Legacy deactivation errors are preserved because no binding token was sent.

Explicit `/remote-control off` deactivates the current binding snapshot.
Daemon shutdown intentionally stops only the local listener and retains the
configured session for startup rebuild, so it does not send plugin
deactivation.

## Channel-Side Session Navigation

While a channel is actively bound, the inbound control plane recognizes
`/session`, `/sessions`, and `/resume` as equivalent aliases. These commands
are consumed by the daemon and are never submitted to the selected agent
conversation.

- Bare alias lists the ten newest ordinary, unarchived user sessions.
- `more` or `all` lists the complete catalog.
- `search <query>` returns at most five deterministic matches, including
  visible user-message content when the local index is available.
- A positive number selects from the last result set; before any list, the
  daemon first creates the default newest-ten snapshot.

The catalog contains persisted workspace and no-workspace conversations but
excludes archived and child sessions. An inactive selection resumes with the
recorded workspace context before replacing the binding. If preparation fails,
the existing binding remains usable.

After a successful numeric selection, ACECode broadcasts this secret-free
WebSocket hint to every connected frontend:

```json
{
  "type": "remote_control_session_selected",
  "payload": {
    "session_id": "session-1",
    "workspace_hash": "workspace-hash",
    "cwd": "/workspace",
    "no_workspace": false,
    "title": "Example session",
    "updated_at": "2026-08-05T10:20:00Z",
    "remote_control_bound": true
  }
}
```

The hint never contains remote-control tokens, plugin configuration, or any
other channel secret. Frontends may be closed; selection and routing do not
depend on a listener being connected.

## Status Response Rules

`state` must be one of:

- `connected`: activation succeeded.
- `pending`: not accepted as a completed activation.
- `failed`: the request failed; `message` may explain why.

`already_running` is optional and boolean. `outbound`, `message`, and
`binding_token` are optional at the parser level, but a completed activation
requires webhook mode and an HTTP(S) outbound URL. A deactivation process may
exit successfully with no stdout; if it returns JSON, the same status parser
and validation rules apply.
