## Context

OpenAI-compatible and Anthropic streaming providers currently own small, duplicated retry loops. Most retryable failures stop after three requests, timeout is a special unbounded case in only one provider, delays cap at fifteen seconds, and each loop snapshots proxy routing before its first attempt. Automatic compaction has a third bounded retry implementation. AgentLoop already knows how to discard provisional streamed output after a replay, but only does so for two error kinds.

Connector authentication currently has three automatic triggers: `on_enable`, daemon `on_startup`, and `on_auth_error` recovery after HTTP 400/401. `on_startup` executes on every daemon launch. Existing connector JSON must remain readable because setup and third-party connector packages may still publish the legacy fields.

The retry loop must never cross the provider-call/tool-execution boundary: a completed ACECode tool result is durable history and must not be replayed. Likewise, a provider that owns its own side-effectful tool runtime cannot safely be restarted from an ambiguous transport failure unless that provider can prove replay safety.

## Goals / Non-Goals

**Goals:**

- Keep pure model sampling and compaction requests alive indefinitely across explicitly retryable transient failures.
- Use one tested retry policy with a one-second exponential base and a twenty-minute cap.
- Make a long retry wait immediately interruptible by the existing stop/cancel path without polling every few milliseconds.
- Reset all provisional assistant, reasoning, usage, and tool-call accumulation before a full request replay.
- Re-resolve system/VPN proxy routing on every HTTP attempt.
- Expose a stable `model_retry` wait state to Web and TUI consumers without adding transcript messages.
- Make the first daemon startup the sole automatic connector-authentication trigger and persist its claim before launching helpers.
- Remove connector recovery wiring from AgentLoop, TUI, daemon, headless, and session construction while preserving lenient configuration compatibility.

**Non-Goals:**

- Automatically resume a waiting request after the ACECode process exits or the operating system terminates it.
- Retry authentication, invalid request/configuration, context-window, permission, malformed-JSON, hard-quota, or other permanent failures.
- Replay a provider-managed turn whose remote runtime may already have executed side-effectful tools.
- Add network-reachability probes or depend on a fixed public health-check host.
- Automatically run connector authentication when a connector is enabled after first startup.

## Decisions

### 1. Providers retain the request-attempt loop, but share policy and waiting primitives

A new provider retry utility will define transient HTTP classification, hard-quota exclusions, `Retry-After` parsing, saturating exponential delay calculation, and an interruptible condition-variable wait. OpenAI-compatible and Anthropic providers will use it and advertise retry progress through existing structured stream events. Automatic compaction will use the same delay/wait contract.

Keeping the attempt loop next to SSE parsing preserves precise knowledge of whether a response completed and of the upstream `Retry-After` header. Moving all attempts into AgentLoop was considered, but it would require turning every provider's streaming parser into a resumable single-attempt state machine and would blur provider-specific completion rules.

### 2. The retry schedule is unbounded in count and bounded in interval

The first retry waits one second. Each subsequent retry doubles the interval until it reaches 1,200,000 ms; all later retries wait twenty minutes. A valid numeric or HTTP-date `Retry-After` value replaces the local delay for that attempt but is clamped to the same twenty-minute maximum. Invalid or past values fall back to the local exponential delay.

`ProviderErrorInfo.retry_max_attempts` is `-1` for this policy. Counters and delay arithmetic saturate so an indefinitely running process cannot overflow.

### 3. Only explicitly transient failures enter the infinite loop

Network transport failures, idle/connect timeouts, HTTP 408/425/429/500/502/503/504/529, provider overload payloads, and incomplete SSE streams are retryable. Exact hard-quota/billing error codes make HTTP 429 terminal. HTTP 400/401/403, malformed JSON, context overflow, missing credentials, invalid configuration, and unclassified failures remain terminal.

This allowlist avoids turning a deterministic bad request into a twenty-minute background loop. Provider implementations remain responsible for marking a failure retryable; the shared helper makes their HTTP decisions consistent.

### 4. Every retry replays only the current immutable provider request

Messages, tool definitions, and request body remain fixed for a retry series. Before each HTTP attempt, the provider re-resolves proxy options. A retry discards all provisional stream accumulation and emits a transcript replacement event so visible partial text does not duplicate.

ACECode tools execute only after a provider request completes, so no tool result is re-executed. Providers that own an opaque tool runtime leave ambiguous failures terminal until they can supply a replay-safe attempt contract.

### 5. Cancellation wakes the waiting provider

`LlmProvider` owns a small condition-variable retry waiter. AgentLoop records the active provider snapshot while sampling or compacting. `abort()` and shutdown set the existing atomic abort flag and notify that provider's waiter. Zero-delay tests and server guidance do not spin: the next HTTP attempt still checks cancellation first.

This avoids the current 50 ms sleep slicing, which would wake a thread 24,000 times during one capped wait.

### 6. Retry state is progress, not conversation history

Before a wait, providers emit a structured `Retry` event with retry number, delay, and unbounded maximum. AgentLoop clears provisional output for every replayable error, emits a coalesced `model_retry` progress payload including the retry deadline, and invokes a TUI callback that replaces the thinking phrase. Immediately before the next attempt, providers emit a resume event and AgentLoop returns progress to `model_waiting`.

The existing stop action remains the control for ending retries. No retry line is appended to the model-visible or user transcript.

### 7. First-start connector authentication uses an at-most-once durable claim

The daemon atomically claims a versioned boolean in ACECode's existing `state.json` runtime-state file before launching any connector helper. All runtime-state read-modify-write operations share a cross-process file lock, so concurrently starting workspace daemons cannot both win the claim or later erase it with a stale snapshot. If the claim already exists, every automatic connector hook is skipped. If the claim cannot be persisted, helpers are skipped and an error is logged so a read-only state directory cannot cause authentication to run on every launch.

On a successful claim, every enabled connector with `hooks.on_startup` is launched once asynchronously. The marker is intentionally written before launch: exact-once execution across a crash is impossible for an external process, and avoiding duplicate login prompts is safer than retrying an ambiguously launched authentication helper. Hook threads are owned and joined during daemon shutdown rather than detached.

The marker is introduced with this change, so an existing ACECode home receives one migration-time first-start claim on its first daemon launch after upgrade. Connector hooks are expected to be idempotent `--ensure` style commands.

### 8. Legacy connector fields remain data-compatible but inert

`hooks.on_enable`, `hooks.on_auth_error`, and `auth_error_scope` continue to parse and serialize so a settings save does not destroy third-party metadata. Runtime code no longer executes those hooks, and connector UI describes only the first-start automatic hook. The `ConnectorAuthRecovery` service and AgentLoop callback/accessors are removed.

Keeping the fields temporarily is less disruptive than rejecting installed connector manifests. A future schema cleanup can remove them after setup and third-party packages stop emitting them.

## Risks / Trade-offs

- [A transient-looking server bug can retry forever] → Use a narrow status allowlist, expose the wait state, and keep stop/cancel immediately effective.
- [An incomplete stream may have generated partial text or tool-call data] → Clear the entire provisional response before replay; tools are not executed until a completed response is accepted.
- [An authentication helper can crash after the claim is written] → Prefer at-most-once behavior and require connector helpers to offer explicit manual authentication outside automatic lifecycle triggers.
- [Existing users receive one startup hook after upgrading] → Treat it as the migration claim, run only idempotent `on_startup` hooks, and never run it again.
- [A provider-managed remote tool turn is interrupted] → Do not mark ambiguous provider-owned turns retryable.
- [Long-lived retry holds the logical task busy] → Preserve the active task and existing cancel semantics; do not fabricate completion or release queued input into a second concurrent turn.

## Migration Plan

1. Add and unit-test the shared retry policy/waiter without changing callers.
2. Migrate OpenAI-compatible, Anthropic, and compaction retry loops to the shared unbounded policy.
3. Expand AgentLoop/Web/TUI retry reset and progress handling.
4. Add the durable connector first-start claim and gate daemon `on_startup`.
5. Remove all `on_enable` execution and `ConnectorAuthRecovery` runtime wiring while retaining JSON fields.
6. Update connector/retry tests and documentation, then run OpenSpec validation and focused/full available test suites.

Rollback can restore bounded caller loops and connector runtime wiring. The versioned state marker is harmless to older binaries and should be left in place so a later re-deployment does not surprise the user with another automatic authentication run.

## Open Questions

None. Process-restart durability and provider-managed replay safety require separate designs because they change session recovery and tool idempotency contracts.
