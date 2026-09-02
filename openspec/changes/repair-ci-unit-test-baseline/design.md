## Context

`resolve_model_context_window_nonblocking()` currently starts one detached thread per
uncached endpoint key. Those threads own no process-lifetime boundary and can still be
inside cpr/libcurl/OpenSSL after GoogleTest has completed and C++ static destruction
has begun. Linux reliably exposes the race as a post-test segmentation fault.

The config mutation service reuses a startup loader whose unrecoverable corruption
path deliberately terminates the process. Passing a directory as an explicit config
path reaches that startup policy on POSIX even though a settings mutation must return
a structured persistence failure. Separately, the Linux update ZIP test manually
builds a package fixture and did not add the seed tree when seed validation became a
release requirement. The grep tool also recognizes only English invalid-regex
diagnostics even though Git localizes stderr. A stale-selection integration test
counts every plugin activation while its default outbound endpoint intentionally
fails; inbound acknowledgements can therefore trigger a legitimate keepalive
activation and be misreported as a stale session switch.

## Goals / Non-Goals

**Goals:**

- Keep model-context resolution nonblocking while giving all probe work an owner.
- Cancel and join probe work before dependent process globals are destroyed.
- Preserve endpoint-key deduplication and successful cache warming.
- Return a persistence error for existing non-file mutation targets on every OS.
- Restore the real Linux update ZIP contract test.
- Keep invalid Git ERE errors actionable under non-English locales.
- Make the stale-selection integration test distinguish a session switch from
  keepalive recovery.

**Non-Goals:**

- Change context-window precedence, endpoint JSON parsing, or the 15-second request
  timeout.
- Redesign general config startup recovery.
- Change the packaged update layout or seed contents.

## Decisions

### 1. Use one lifecycle-owned probe worker

A function-local probe service owns a queue, condition variables, the active request
cancellation flag, and one joinable worker thread. Enqueue remains fast and endpoint
keys stay deduplicated through the existing cache/in-flight set. Service teardown
rejects new work, discards queued work, cancels the active cpr transfer through its
progress callback, and joins the worker.

One worker bounds thread creation and is sufficient because probe results are only a
future-call optimization. A detached thread registry was rejected because it still
needs the same shutdown and cancellation protocol while permitting unbounded
concurrency.

### 2. Establish dependent singleton ordering before constructing the worker

The enqueue path resolves proxy options and initializes a cpr session before the
function-local probe service is first constructed. The task carries its prepared
proxy options, so the worker does not lazily construct the proxy resolver. This makes
the probe service's destructor run before those dependencies under reverse static
destruction order. The cache globals are translation-unit objects constructed before
the service and likewise remain alive until after it joins.

### 3. Make test reset a quiescence boundary

`reset_model_context_window_cache_for_test()` first cancels the active probe, drops
queued probes, and waits for the worker to become idle; only then does it clear cache
and in-flight state. This prevents an earlier test from repopulating state after a
later test has reset it.

### 4. Validate mutation target type before loading

The mutation transaction checks filesystem errors and requires every existing target
to be a regular file. A directory, device, or other non-file path becomes the existing
`Persistence` result through the mutation exception boundary. Startup loading policy
is unchanged for actual config files.

### 5. Copy the canonical seed tree into the Linux fixture

The contract test copies `assets/seed` into each synthetic package before invoking the
real ZIP creator. It continues to validate the source/package byte contract through
`verify_seed_bundle.py`; no duplicate mock manifest is introduced.

### 6. Use Git's stable error context in addition to English wording

Git preserves the failing `-e` option and raw pattern as substitutions in localized
invalid-regex diagnostics. Classification keeps the existing English keywords and
also accepts a diagnostic that contains both that option and the exact submitted
pattern. This retains Git as the ERE parser instead of introducing a second regex
engine whose accepted syntax could drift.

### 7. Synchronize the stale-selection test through the control queue

The test installs a successful capture sender before its inbound commands, removing
unrelated webhook failures from the keepalive state machine. After releasing the
blocked catalog, it queues a current-generation usage error and waits until that
message is published. FIFO processing makes this a deterministic fence behind the
stale selection; activation count can then describe only explicit bindings.

## Risks / Trade-offs

- **[Risk] Shutdown can wait for a transfer while libcurl is between progress
  callbacks.** -> Keep the existing bounded request timeout and use the progress
  callback for prompt cancellation whenever libcurl is active.
- **[Risk] Serial probes warm several distinct models more slowly.** -> Cached/local
  values still return immediately and endpoint probing is best-effort metadata work.
- **[Risk] Singleton order regresses if work is moved back into the worker.** -> Pass
  prepared proxy options in the queued task and cover clean per-test process exit in
  the Linux CTest suite.
- **[Risk] A future Git translation changes surrounding prose.** -> Match only the
  stable command option and pattern substitutions, while keeping generic Git failures
  generic when either is absent.
- **[Risk] A sleep-based stale-selection check flakes under load.** -> Use a visible
  current-generation command behind the stale task as the queue completion fence.

## Migration Plan

Land the worker, config guard, and fixture update together; run focused tests and the
complete Linux CTest suite. Rollback is a source revert and requires no state or data
migration.

## Open Questions

None.
