## 1. Attention Flush State

- [x] 1.1 Add one background attention flusher, a dirty-workspace set, and a one-second coalescing interval under the existing attention mutex.
- [x] 1.2 Mark cursor-only events dirty while keeping attention-state and busy-state transitions immediately durable.
- [x] 1.3 Write compact JSON and clear dirty state only after successful replacement so transient failures remain retryable.

## 2. Lifecycle Safety

- [x] 2.1 Stop listener/subscription producers before the final dirty flush and flusher-thread join.
- [x] 2.2 Document the daemon flusher thread, durability boundary, retry invariant, and shutdown ordering.

## 3. Tests And Validation

- [x] 3.1 Add a Web server regression test that forces a failed rename, verifies periodic retry, and verifies final shutdown drain.
- [x] 3.2 Build and run the focused attention persistence regression test.
- [x] 3.3 Run the full C++ test suite and code quality checks.
- [x] 3.4 Run strict OpenSpec validation and Git diff checks.
