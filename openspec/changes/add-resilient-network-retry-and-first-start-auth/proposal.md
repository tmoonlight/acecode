## Why

Long-running ACECode tasks can currently fail after only a few attempts when a laptop changes networks, sleeps briefly, or moves between VPN and direct connectivity. Connector authentication also runs from multiple lifecycle triggers, which can unexpectedly open or execute authentication helpers long after installation.

## What Changes

- Keep an in-flight model step alive across transient network, timeout, throttling, and recoverable service failures with an unbounded retry count.
- Use abort-aware exponential backoff from one second up to a maximum interval of twenty minutes, honor bounded server retry guidance, and expose structured retry-wait state without appending noisy transcript messages.
- Re-resolve transport routing before each attempt so changed system/VPN proxy state can take effect after connectivity returns while the logical request remains unchanged.
- Apply the same transient-failure contract to streaming model requests and automatic compaction while preserving the exact logical request and never replaying an already-executed tool.
- Keep authentication, invalid configuration/request, permission, context-limit, and hard-quota failures terminal instead of retrying them forever.
- Remove automatic connector authentication recovery on HTTP 400/401.
- **BREAKING**: stop automatically executing legacy connector `on_enable` and `on_auth_error` hooks. Continue accepting their configuration fields for compatibility, but treat `on_startup` as the only automatic authentication hook.
- Run enabled connectors' `on_startup` authentication hook once on the first ACECode daemon startup for that ACECode home, record a durable completion marker before later startups, and do not re-run it because of enable toggles or authentication errors.

## Capabilities

### New Capabilities

- `resilient-model-requests`: Defines transient-error classification, unbounded capped exponential retry, cancellation, request consistency, retry observability, and compaction behavior.
- `connector-first-start-authentication`: Defines the sole automatic connector authentication trigger, its durable first-start gate, compatibility behavior, and removal of authentication-error recovery.

### Modified Capabilities

None. This repository currently has no canonical capabilities under `openspec/specs`; the new capabilities supersede the relevant retry and connector lifecycle behavior recorded in earlier change-local specifications.

## Impact

- Provider request and retry code under `src/provider/`, AgentLoop retry/error handling, automatic compaction, and their unit tests.
- Daemon startup, connector configuration/runtime wiring, Web and TUI connector enablement surfaces, and connector/config tests.
- Session/Web/TUI progress reporting for retry waits and cancellation.
- Connector authors may retain legacy fields during migration, but only `hooks.on_startup` remains an automatic executable lifecycle hook.
