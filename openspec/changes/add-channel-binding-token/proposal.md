## Why

Channel v1 currently identifies activation and deactivation only by `session_id`. If the same session is activated again, a delayed cleanup from the old binding is indistinguishable from a request to detach the new binding. The host also has no binding-instance identity to retain when keepalive activation replaces a plugin runtime.

## What Changes

- Allow a successful `channel.status` response to include an optional opaque `binding_token`.
- Validate a present token as a non-empty string while preserving status compatibility for plugins that omit it and for unknown response fields.
- Echo the current binding's token in `channel.deactivate`; preserve the exact legacy session-only request shape when no token was returned.
- Associate the token with the current session binding generation across activation, keepalive reactivation, replacement, explicit close, and shutdown serialization.
- Prevent stale same-session legacy cleanup from emitting an unscoped deactivation after a newer binding has become current.
- Document and test the optional Channel v1 extension without adding product-specific provider behavior.

## Capabilities

### New Capabilities

- `channel-plugin-binding-identity`: Optional Channel v1 binding-instance identity, lifecycle ownership, compatibility, and concurrency rules.

### Modified Capabilities

None. There is no current canonical OpenSpec capability for the channel plugin protocol in this tree.

## Impact

- Channel protocol serialization and status parsing in `src/remote_control/channel_plugin.*`.
- TUI and daemon current-binding state in `src/commands/remote_control_command.cpp` and `src/remote_control/session_channel_binder.*`.
- Host, binder lifecycle, replacement, keepalive, and concurrency tests under `tests/remote_control/`.
- The durable channel protocol section in `README.md`.
