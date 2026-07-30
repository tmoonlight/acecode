## Why

Streaming session events currently rewrite the complete workspace attention
file synchronously on the AgentLoop worker thread, reaching hundreds of file
replacements per second and blocking concurrent sessions on the same mutex.
Attention persistence needs bounded write amplification without weakening
durability at user-visible state transitions.

## What Changes

- Mark cursor-only attention updates dirty and coalesce their persistence on a
  one-second background flush interval.
- Keep read/unread/in-progress transitions and busy-state changes
  synchronously durable.
- Persist compact JSON instead of pretty-printed state.
- Retain dirty workspaces after a write failure so a later flush retries them.
- On shutdown, stop attention event producers before performing the final
  flush and joining the flusher thread.

## Capabilities

### New Capabilities

- `session-attention-persistence`: Defines throttled cursor persistence,
  immediate state-boundary durability, retry behavior, and safe shutdown.

### Modified Capabilities

None.

## Impact

- `WebServer::Impl` attention state and lifecycle in `src/web/server_impl.hpp`,
  `src/web/server_helpers.cpp`, and `src/web/server.cpp`.
- Daemon thread lifecycle documentation in `CLAUDE.md`.
- C++ build and session/Web regression validation.
