## Context

`ChannelPluginHost` launches a stdio command for each activate or deactivate request. A successful activation currently retains the outbound webhook URL but has no binding-instance identity. `SessionChannelBinder` serializes bind, off, and keepalive process calls with `op_mu_`, while event callbacks are fenced by `(session_id, generation)`. Explicit off snapshots the active channel and invokes a session-only deactivate. Keepalive activation can return a replacement runtime but currently updates only the outbound URL. Shutdown intentionally preserves the external plugin binding for daemon restart and therefore does not deactivate it, but its teardown is not currently serialized with command operations.

The protocol version remains `1`. Existing plugins may ignore unknown request fields and may omit all new response fields.

## Goals / Non-Goals

**Goals:**

- Give a plugin an opaque per-binding identity that ACECode returns on deactivate.
- Keep old plugins fully compatible, including the exact session-only deactivation shape.
- Make the current token follow the binding generation and the latest successful activation.
- Ensure replacement, explicit close, keepalive, and shutdown cannot pair one binding's session with another binding's token.
- Keep provider-specific names, endpoints, and behavior outside the ACECode repository.

**Non-Goals:**

- Changing the Channel v1 protocol version or manifest schema.
- Persisting binding tokens in `config.json` or exposing them in status/log output.
- Requiring old plugins to synthesize binding identity.
- Changing the existing daemon-shutdown behavior that leaves a binding available for startup rebuild.

## Decisions

### 1. `binding_token` is an optional top-level `channel.status` field

A plugin may return:

```json
{
  "type": "channel.status",
  "state": "connected",
  "binding_token": "opaque-binding-id",
  "outbound": {
    "mode": "webhook",
    "url": "http://127.0.0.1:39001/messages"
  }
}
```

ACECode treats the value as opaque. When the field is present it must be a non-empty JSON string. A wrong type or empty string rejects the status with a field-specific error. Absence remains valid, and unrelated unknown fields remain ignored.

Rejecting malformed identity is safer than silently degrading it to a legacy unscoped binding: otherwise a plugin that intended replacement safety could accidentally receive a session-only deactivate.

### 2. Deactivation echoes only a validated stored token

For a token-aware binding:

```json
{
  "type": "channel.deactivate",
  "protocol_version": 1,
  "session_id": "session-1",
  "binding_token": "opaque-binding-id"
}
```

When activation returned no token, ACECode emits the original three-field request with no `binding_token`. The serializer does not invent, persist, trim, log, or display a token.

External process failures are sanitized at the `ChannelPluginHost::deactivate` boundary. ACECode replaces the exact known token, including its JSON-escaped request representation, in runner exceptions, process diagnostics, parser errors, and status messages before returning an error to daemon logs or user-facing command text. Legacy calls have no token to redact and retain their original diagnostics.

### 3. Current identity is owned by the binding generation

Daemon `ActiveChannel` and the TUI's active binding retain the activation token beside the session and manifest. Explicit off first invalidates inbound/event callbacks, snapshots the current active binding, then deactivates using that snapshot's session and token.

A successful keepalive activation belongs to the snapshot generation that initiated it. If that generation is still current, its returned token replaces the stored token atomically with the outbound URL update. If the generation is stale, neither value is installed.

### 4. Replacement cleanup is scoped and compatibility-safe

After a new activation succeeds, a replaced token-aware binding may be cleaned up with its old token. If the old and new bindings use the same session, ACECode sends stale cleanup only when both tokens are present and different. It never sends a session-only cleanup for an old same-session legacy binding after the new binding is active, because that request could detach the new binding.

For different session ids, legacy cleanup remains safe because `session_id` still scopes the old binding. Cleanup failures are warnings and do not roll back a successfully installed new binding.

The plugin remains responsible for applying `channel.deactivate` only when both the session and, when present, `binding_token` match its current binding.

### 5. Shutdown joins the lifecycle serialization domain without detaching

Shutdown continues to stop the local listener and preserve `bound_session_id` so startup rebuild can reactivate the channel. It does not send `channel.deactivate`. After stopping the keepalive thread, shutdown acquires `op_mu_` before tearing down the service and binding state, so an activation already in progress completes before teardown and a queued activation observes `shut_down_`.

## Risks / Trade-offs

- **A plugin returns a fresh token on every health check.** ACECode installs the latest successful token; stale cleanup uses the prior token, so explicit off still targets the current binding.
- **A legacy plugin reactivates the same session.** ACECode cannot safely distinguish old and new instances, so it skips post-replacement stale cleanup and retains session-only deactivate only for the current binding.
- **A plugin ignores `binding_token`.** The protocol shape is correct but the plugin cannot provide replacement safety. This extension cannot make a non-conforming external implementation generation-safe.
- **Binding token leaks.** Tokens stay runtime-only and are excluded from config, display, and logs; tests use synthetic values.

## Migration Plan

No migration is required. Old plugins omit `binding_token` and continue receiving the prior deactivation JSON. Token-aware plugins can roll out independently while still declaring Channel protocol version `1`.

## Open Questions

None.
