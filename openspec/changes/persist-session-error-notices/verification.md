# Verification

- Web transcript reducer regression: passed, including restored HTTP 451 diagnostics, transcript replacement, overlapping history/replay/live events, independent equal errors, and legacy transient errors.
- `pnpm test`: passed (3084 pass lines).
- `pnpm build`: passed, including bundled regular-expression compatibility checks.
- `node web/scripts/test-session-errors.mjs`: passed with the real ChatView and controlled HTTP/WebSocket fixtures. Covered a failure while viewing another session, switching back, page refresh, duplicate replay, and a repeated identical failure. No browser page errors. Screenshot: `build/validation/session-errors/restored-errors.png`.
- Strict OpenSpec validation and scoped `git diff --check`: passed.
- Windows Release native build: passed with `cmake --build build --config Release --target acecode_unit_tests --parallel 1` in the Visual Studio development environment.
- Windows native regression: 141/141 passed. Covered error disk reload/resume, unique event/API identity, original diagnostic metadata, next-turn provider filtering, actual compaction requests, legacy message identity, turn timing, recovery callbacks, session resume, and conversation history. Results: `build/validation/session-errors/native-tests.json`. An initial run exposed a missing non-streaming method in the new test stub and callback/event ordering drift; both were corrected before the final passing run.

The browser check uses isolated simulated responses, not the affected remote server. This change has not been packaged, published, or installed into a running Desktop process. Older errors that were never persisted are not reconstructed.
