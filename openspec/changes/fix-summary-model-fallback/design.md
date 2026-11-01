# Design

## Context

See proposal.md. Title generation has a shared resolver but separate daemon/TUI executors. Home intent and memory generation have separate provider calls. Model selection timestamps do not prove request success.

## Goals / Non-Goals

**Goals:** share silent failover, persist successful model identities, preserve cancellation and feature switches.

**Non-Goals:** change interactive chat selection, enable disabled memory processing, alter compaction prompts, or add UI fallback status.

## Decisions

1. Provider construction attaches an immutable, owned success observer. Conversation execution invokes it only after a completed, nonempty response without error/cancellation. Background generations never update the conversation backup. This avoids consulting mutable session metadata after a concurrent model change.
2. Persist opaque connection identities in a small SQLite runtime database under the data directory. SQLite serializes concurrent process writes; no credentials, endpoints or transcript text are stored. Current configuration supplies credentials at use time; removed or changed connections do not match old success records.
3. A shared executor enumerates recovered model, explicit selection, last successful conversation model, contextual model, default and remaining saved models, with connection deduplication. Every candidate is tried at most once per execution; callers retain existing timeouts and cancellation. Empty/invalid task output and provider exceptions count as failure.
4. Successful fallback is saved against the explicit selection's identity. A changed selection or connection resets this preference. Failed replacements allow trying the next candidate. Existing explicit settings remain user-controlled.
5. Missing/empty summary settings become automatic selection. Remove selection-required validation and missing-model warning; retain actual settings persistence errors. Follow the existing serialized autosave store.

## Risks / Trade-offs

- All providers unavailable -> retain existing result/data and error behavior, without adding fallback notices.
- More requests during an outage -> deduplicate candidates and respect cancellation/time budgets.
- Runtime state cannot be written -> generation still succeeds; persistence is best effort.
- Previously successful model edited or deleted -> ignore stale identity and continue candidates.

## Migration Plan

No configuration migration. The runtime database is created lazily; existing configurations work without success history. Rebuild backend and frontend together.

## Verification (2026-11-01)

- `cmake --build build/windows-x64-msvc --config Release --target acecode_unit_tests --parallel 6` passed. The shared header change required rebuilding dependent native libraries and tests.
- 125 relevant native tests passed: successful model tracking/execution (10), stream collection (3), provider construction (15), side chat (20), TUI title runner (4), title behavior (25), home intent helpers (21), memory scheduling (14), registry title generation (1), summary settings (6), and HTTP integration (6). Real local HTTP fixtures verify a rejected preferred model followed by a working backup, reuse of the replacement, optional configuration, and no fallback request after cancellation. Tests use temporary data roots.
- The broader selected run additionally included three newly appearing `FollowUpPromptHttp` tests, initially returning HTTP 404 before generation. After rebuilding the current workspace, all three passed. To allow the shared test binary to build, this change made only a NOMINMAX guard and JSON fixture-initializer correction in that concurrently introduced test file; follow-up endpoint behavior remains owned by its separate work.
- Final memory-budget review keeps the selected model's context window instead of shrinking healthy requests to the smallest unused backup. The final incremental build passed; all 14 memory scheduler regressions and the three follow-up HTTP tests passed on rerun (17/17). Across the relevant runs, 128 distinct native tests passed.
- `pnpm --dir web test` passed after regenerating the static-copy catalog with `pnpm --dir web i18n:catalog`. `pnpm --dir web build` passed, including regex compatibility checks.
- Browser verification used the complete `SettingsPage` Tools group with a local fixture: enabling with an empty selection saves without expanding configuration, no summary alert appears, switching model then back to automatic saves serially, collapsing preserves the final value, and no page errors occur. Evidence: `build/summary-fallback-ui.log` and `build/summary-fallback-settings.png`. The fixture's unrelated speech service request could not reach a daemon; summary interaction passed.
- `openspec validate fix-summary-model-fallback --strict` and scoped `git diff --check` passed. Layer inspection including untracked source files reports no dependency/include violations; existing desktop-pet and session-registry file-size violations remain. The normal lint command only indexes tracked files, so new headers were included explicitly for this inspection.
- The workspace contained extensive concurrent edits. No commit, release, installation or restart of the user's running application was performed.
