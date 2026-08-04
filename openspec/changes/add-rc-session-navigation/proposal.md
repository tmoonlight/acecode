## Why

Remote control currently binds one ACECode session, but an IM user cannot inspect or change that binding without returning to the ACECode UI. This makes long-running remote workflows awkward and prevents a user from moving the channel to another conversation when away from the desktop.

## What Changes

- Treat `/session`, `/sessions`, and `/resume` received through an active remote-control channel as aliases for one channel-side session command family.
- List the ten most recently updated resumable user sessions by default, across all persisted workspaces and no-workspace conversations.
- Let `more` and `all` list the complete resumable user-session catalog.
- Let `search <query>` return at most five matching sessions.
- Number every result and retain the last presented result snapshot so a numeric command selects the session the user actually saw.
- Resume an inactive target with its recorded workspace context, then atomically replace the current remote-control binding.
- Broadcast a generic session-navigation event to connected Web/Desktop clients so an open frontend switches to the selected conversation and visibly replays the existing remote-control lightning surge.

## Capabilities

### New Capabilities

- `remote-control-session-navigation`: Channel-side global session discovery, stable numeric selection, cross-workspace resume, and optional frontend navigation notification.

### Modified Capabilities

- `session-resume`: Remote-control selection may resume a persisted session using its own workspace or no-workspace metadata.

## Impact

- Generic remote-control command parsing and binder lifecycle under `src/remote_control/`.
- Daemon-wide session catalog construction and WebSocket notification wiring under `src/daemon/` and `src/web/`.
- Desktop/Web navigation and the existing remote-control surge under `web/src/`.
- Focused C++ and Node tests plus protocol documentation.

No provider, connector, company, or channel-product identifier may enter ACECode core.
