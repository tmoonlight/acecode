# Verification

- Web transcript reducer regression: passed, including restored HTTP 451 diagnostics, transcript replacement, overlapping history/replay/live events, independent equal errors, and legacy transient errors.
- `pnpm test`: passed (3084 pass lines).
- `pnpm build`: passed, including bundled regular-expression compatibility checks.
- `node web/scripts/test-session-errors.mjs`: passed with the real ChatView and controlled HTTP/WebSocket fixtures. Covered a failure while viewing another session, switching back, page refresh, duplicate replay, and a repeated identical failure. No browser page errors. Screenshot: `build/validation/session-errors/restored-errors.png`.
- Strict OpenSpec validation and scoped `git diff --check`: passed.
- Windows Release native build: passed with `cmake --build build --config Release --target acecode_unit_tests --parallel 1` in the Visual Studio development environment.
- Windows native regression: 141/141 passed. Covered error disk reload/resume, unique event/API identity, original diagnostic metadata, next-turn provider filtering, actual compaction requests, legacy message identity, turn timing, recovery callbacks, session resume, and conversation history. Results: `build/validation/session-errors/native-tests.json`. An initial run exposed a missing non-streaming method in the new test stub and callback/event ordering drift; both were corrected before the final passing run.

The browser check uses isolated simulated responses, not the affected remote server. This change has not been packaged, published, or installed into a running Desktop process. Older errors that were never persisted are not reconstructed.

## Release integration validation (2026-10-10)

- Full Windows regression exposed two old assumptions that failures leave no visible error record, plus a diagnostic regression when saving the notice also failed. Retained the original storage error under the session lock, and updated blocked-input and failed-compaction assertions to require transcript-only errors while preserving the original history prefix and context window.
- Focused release regressions: 41 passed, zero failures.
- Final full Windows regression: 6239 inventory entries, 6238 executed, 6229 passed, 9 skipped, zero failures; one disabled benchmark. Ran all suites with `run_fast_tests.py --profile full --shards 6` and isolated profiles. Report: `C:/Users/shao/AppData/Local/Temp/acecode-release-1.0.3/windows-full-final.json`.
- The CI file-size gate then rejected eight added lines in the main session manager. Moved the public checked-append lock/diagnostic facade into the existing history implementation and retained a private locked append helper. All seven strict gates passed individually with fail-fast handling, including the cloud layer gate. Rebuilt Windows tests and passed 91 session, storage, history, hook, reasoning, and payload regressions after this structural move (`final-layout-tests.json`).
