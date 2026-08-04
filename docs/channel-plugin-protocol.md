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
the existing binding remains usable. Archive suppression is scoped by complete
session identity and workspace context, so an equal session id in another
storage scope is not hidden.

Long lists are split only at UTF-8 codepoint boundaries. The production byte
limit is therefore safe for multibyte text. If a caller supplies an artificial
limit smaller than one codepoint, that one codepoint forms an over-limit chunk
rather than being emitted as invalid UTF-8.

Each queued command is scoped to the source session and binding generation.
An `off` or rebind makes older queued work stale, and stale work is discarded
without sending a result or changing the new binding. Catalog/list/search work
is coalesced to one operation per generation; the remaining control queue is
fixed-size. Excess requests receive `Session navigation is already processing.`
instead of growing daemon memory.

Every control response is published while holding a lease on the context that
owns it. Rebind/off first suspend new inbound submissions, wait for old leases,
then drain and change the Hub route. Numeric-switch success is leased to the new
target context, so neither old output nor switch confirmation can be attributed
to a later binding.

Hub suspension also fences accepted inbound dispatches. Acceptance snapshots
the route, queues the immediate acknowledgement, and increments an in-flight
dispatch count before releasing the Hub lock. Suspension atomically rejects new
inbound work and waits for those callbacks (including exception unwinding) before
the binder deactivates the old context. Route-changing commands remain queued on
the binder control worker; calling suspension synchronously from the accepted
callback itself is rejected as a logic error rather than waiting on itself.
`disable()` and Hub destruction use the same fence before releasing runtime
state. The dispatch guard owns a shared fence state rather than a Hub pointer;
therefore a callback-triggered disable skips only its impossible self-wait, and
its guard can still finish safely even if the Hub owner is being torn down.

Binding persistence is the final preparation step before runtime replacement.
Reload, merge, and save complete before the in-memory config is changed; a
load/save exception leaves both configs and the live route unchanged. `/rc off`
uses the same ordering and keeps the live binding when clearing persistence
fails.

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
