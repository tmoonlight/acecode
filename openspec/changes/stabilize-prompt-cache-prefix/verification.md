# Verification

Windows, current master checkout, 2026-10-07. Existing unrelated Web edits were preserved. New provider regressions use local HTTP servers and stub models.

- `cmake --build build --target acecode_unit_tests --config Release --parallel 1`: final native target built successfully (exit 0), log `%TEMP%/acecode-cache-prefix-build-final-proof.log`.
- Targeted GoogleTest filter covered request assembly, normal turns, memory, compaction, parent models, session persistence/reload, TUI replay, side questions and provider cache transport: **268 tests, 268 passed**, XML `%TEMP%/acecode-cache-prefix-targeted-final.xml`.
- Added checks include cross-turn prefix equality; persisted/reloaded snapshots; append-only project/expert/catalog/plan/hook updates; complete four-status checklist restoration; automatic/manual GLM/DeepSeek/GPT prefix equality; tool-response/overflow/field-rejection fallbacks; parent model selection; same-provider concurrent per-request keys; explicit memory disable; invalid record types; and failed snapshot/checkpoint writes.
- Early validation failures were resolved rather than waived: moving-context expectations were replaced with append-only assertions, transcript-only snapshots cannot replace a valid epoch, and Windows junction-aware test fixtures use the actual SessionManager project directory.
- `openspec validate stabilize-prompt-cache-prefix --strict`: passed.
- Layer/ownership checks include new files via a temporary Git index, preserving the user's real index. Both strict checks reported zero findings; `git diff --check` passed.
- Script checks: `ctest --test-dir build -C Release -R '^(verify_package_python_unit|dev_desktop_script_unit|macos_portable_package_unit)$' --output-on-failure`: **3/3 passed**.
- The first full native run exposed 16 outdated user-message/context assertions and an uninitialized session fixture. Those were corrected while retaining payload, ordering, permissions and retry assertions. The follow-up set passed 54/55 initially; the remaining expert test incorrectly required identical static system guidance despite an explicit tool-policy change. Its context/history assertions were retained and the corrected case passed separately.

## Requirement audit

| Approved item | Current implementation | Direct regression evidence |
| --- | --- | --- |
| 1. Stable context window | Versioned hidden snapshots, append-only state/hook updates, frozen skill system index, durable restore and explicit memory-disable epoch | `ApiRequestBuilder.PrefixSurvivesNewTurnsSkillActivityAndReload`, `AgentLoopTermination.RequestPrefixIsByteStableAcrossUserTurns`, `HookAgentLoop.*`, `CompactCheckpoint.*`, `SessionResumeRestore.*` |
| 2. Todo state | No per-request checklist rewrite; four-state complete list restored once in a checkpoint | `ApiRequestBuilder.TodoWriteDoesNotReplaceEarlierChecklist`, `AgentLoopContextGolden.TodoWriteChecklistIsRestoredOnceInDurableCompactWindow` |
| 3. Compaction reuse | Common main request projection, same tools where supported, bounded validated tool-free fallback and same per-call cache key | `CacheStability/AgentLoopCompactionPrefix.*`, `CompactCore.*`, `OpenAiPromptCache.CompactionOptionsAreRequestScoped` |
| 4. Parent model | Current parent saved model when omitted, explicit selection and missing-parent fallback preserved | `SpawnSubagentTool.UnspecifiedModelUsesParentsCurrentSelectionAfterSwitch`, `SpawnSubagentTool.ExplicitModelOverridesParentsCurrentSelection`, `SpawnSubagentTool.InheritsModelFromTuiParentOutsideRegistry`, `MeshAgentServiceTest.*` |
| 5. Cache routing | Exact official HTTPS endpoint allowlist, stable session key in per-call options, narrow one-time unsupported-field fallback | `OpenAiPromptCache.*` (14 tests), `ApiRequestBuilder.SessionCacheKeySurvivesRebuildAndRemainsPerRequest` |

## Final integrated result

- `build/tests/Release/acecode_unit_tests.exe --gtest_output=xml:%TEMP%/acecode-cache-prefix-full-final.xml`: **6,170 executed, 6,161 passed, 9 skipped, 0 failures, exit 0**, 668.370 seconds. One additional pre-existing disabled test was not executed.
- The skipped tests require optional bridge/dependency installation, manual native notification opt-in, real model/search endpoints, a private historical fixture, or POSIX behavior. They were not represented as passing.
- The nine skips are `ChannelBridge.RealBridgeStartsWithoutConnectingAnAccount`, `ChannelSetup.RealDependencyInstallationIsOptionalAndUsesTemporaryState`, `NativeNotifications.OptInDeliversSelfDrawnToast`, `SystemPromptTest.PosixPromptStaysCleanOfWindowsGuidance`, both `ImageGenerationNetworkSmoke` cases, `RssSearchBackendLive.HostedEndpointReturnsSearchableResults`, `SessionReplayRealJsonl.UserSessionDbcoding5_2026_04_26`, and `Utf8PathTest.ExtendedLengthPathIsIdentityOnPosix`.
- Final full-run log/exit marker: `%TEMP%/acecode-cache-prefix-full-final.log`, `%TEMP%/acecode-cache-prefix-full-final.exit`.
- SHA-256 manifest of all 50 changed/new source and test files matched after the full run: no code changed underneath the final validation.
- Strict layer/ownership gates reported zero findings, script checks passed 3/3, OpenSpec strict validation and diff checks passed. The five approved requirements are mapped to direct regressions above.

No live provider cache-hit percentage, cross-platform runtime verification, or installed Desktop update is claimed. The running development Desktop/daemon executable is left undisturbed.
