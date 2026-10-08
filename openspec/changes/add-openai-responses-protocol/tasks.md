## 1. Configuration and selection

- [x] 1.1 Persist and validate api_protocol through profile/draft/API equality and Web editor; verify profile/draft/handler/Web tests.
- [x] 1.2 Route Responses profiles through factory and include protocol in provider fingerprints; verify provider factory regression tests and legacy defaults.

## 2. Protocol and transport

- [x] 2.1 Implement Responses request/response conversion and strict SSE parsing; verify codec tests for text, images, reasoning, refusal, tools, usage and malformed/incomplete streams.
- [x] 2.2 Implement cancellable Responses HTTP/SSE transport with retry isolation and correct endpoint/header handling; verify local server tests including continuation, truncation and cancellation.
- [x] 2.3 Verify ordered native output survives session persistence and agent tool continuation, and stale native items cannot override canonical history; add focused integration regressions.

## 3. Delivery validation

- [x] 3.1 Document model protocol selection and API compatibility; verify documented fields against implemented profile/API behavior.
- [x] 3.2 Run Windows provider/config/session/agent regression tests, Web pnpm test/build, source-layer checks, strict OpenSpec validation and git diff --check; record results and runtime limitations.
- [x] 3.3 Release integration: apply the shared rate-limit retry delay policy to Responses and verify gateway error classification and cancellation alongside the integrated changes.

## Validation record (2026-10-08)

- Windows MSVC Release: `cmake --build build --config Release --target acecode_unit_tests --parallel 1` passed after loading `scripts/dev_windows_env.bat`.
- `python scripts/refactor/run_fast_tests.py --binary build/tests/Release/acecode_unit_tests.exe --profile full --shards 1 --filter "*Provider*.*:*Responses*.*:*SavedModel*.*:ModelsHandler.*:ModelConnectionTest*.*:SessionSerializer.*:SessionModelBinding.*:ModelProfileRuntimeOptions.*:AgentLoopToolProtocolNames.*:AgentLoopEmptyResponse*.*:TurnRecoveryTest.*:ToolExecutor*.*:OpenAiContentParts*.*:ToolImageFeedback*.*" --output build/responses-regression-tests.json`: 404 executed, zero failures/skips. Inventory 6213; the rest of the complete unit suite was not run. The executed set includes all 17 OpenAiResponsesTest and 15 OpenAiResponsesProviderTest cases.
- Explicitly checked the linked binary's test inventory after MSBuild regeneration to ensure the new codec suite was present.
- `pnpm test` and `pnpm build` in web/ passed. The initial Web build encountered a temporary output lock during concurrent CMake asset reading; the final build succeeded and the final CMake generation embedded its 315 assets.
- Edge browser: 8 checks passed for default protocol, keyboard selection, connection-test/save payloads, reopening a Responses profile and 420px dark layout. Screenshots inspected in desktop light and mobile dark modes.
- `scripts/layers/check_layers.py --repo . --layout final --strict`, `check_ownership.py --strict`, and `check_file_size.py --strict` passed including all new files; no ownership baseline exemptions added.
- `openspec validate add-openai-responses-protocol --strict` and `git diff --check` passed.
- Live paid OpenAI inference, installed Desktop runtime, non-Windows native builds and release packaging were not exercised. Existing running Desktop processes remain unchanged. No commit, push or release was performed.

## Release integration validation (2026-10-08)

- Responses now uses the shared body-aware rate-limit retry cap. Added local HTTP regression coverage for rate-limit text with HTTP 400, context-overflow text with HTTP 429, hard quota, and cancellation during the retry callback.
- Windows Release build and full suite passed: inventory 6224, executed 6223, skipped 9, failed 0, not_run 0; one test remains disabled. Evidence: `build/release-1.0.1-tests.json`, isolated logs `N:/acecode-gtest-iso/194305`.
- Full `pnpm test` and `pnpm build`, strict layer/ownership/file-size/doc-path/include/map checks, strict OpenSpec validation and `git diff --check` passed.
- Live paid-provider inference remains unverified. Cross-platform packaging and exact released-package verification belong to the v1.0.1 release gate.
