## Why

The Web UI currently repeats the expensive `/api/git/info` lookup when session-bound components remount, but the proposed shared cache keys only by `cwd` and always loads through the global API client. That can send a session-scoped request to the wrong daemon or reuse Git data across different port/token connection contexts.

## What Changes

- Keep the pre-render visibility guard so a Git session pill that cannot render does not request Git information.
- Share the 30-second Git-information TTL and in-flight request deduplication across consumers that use the same effective daemon connection.
- Isolate cached and in-flight Git information between different effective origins or authentication tokens, even when `cwd` is identical.
- Preserve explicit refresh and Git-state invalidation without allowing one connection context to reload through another context's API client.
- Add behavioral regression tests for same-context deduplication and cross-context isolation.

## Capabilities

### New Capabilities

- `connection-scoped-git-info-cache`: Defines visibility-aware Git-information loading, same-connection sharing, and strict isolation across daemon/authentication contexts.

### Modified Capabilities

None.

## Impact

- Affected Web code: `web/src/lib/api.js`, the shared Git-information cache, `GitSessionPill`, `SidePanel`, `Sidebar`, and their tests.
- No daemon endpoint, payload, persistence, or external dependency changes.
- Existing callers keep using `/api/git/info`; only frontend request routing and cache ownership change.
