<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md for motivation. Seed hooks use the Codex-shaped parser, which currently ignores top-level `enabled`; the legacy parser already supports it. Official managed files are verified against canonical JSON hashes and reconciled at startup. Hook dispatch currently serializes before checking whether any command can run.

## Goals / Non-Goals

**Goals:** Disable shipped hooks on fresh and upgraded installations, preserve user-owned files and existing hook choices, and make inactive dispatch inert even with non-UTF-8 tool output.

**Non-Goals:** Change the global feature flag, add UI, execute or modify live user hooks, publish a release, or repair every encoding/exception path.

## Decisions

- Honor boolean `enabled: false` at the source boundary. Retain the source and an informational disabled diagnostic, but normalize no handlers. Missing or true keeps existing behavior. Asset-only changes would otherwise be ignored; changing the global flag would affect user hooks.
- Add that switch only to the bundled agent-reporting definition. Keep the example enabled for manual copying into a supported user source and explicit trust review. Editing the managed file is not an activation mechanism because its fingerprint would no longer match.
- Update seed.version, MANIFEST bundle/source versions and current hash together; add the old current hash to the existing historical allowlist. Reuse existing reconciliation and ownership protections.
- Cache serialized payload lazily after event matching, handler/trust checks and command availability. Preserve matcher diagnostics and matched/skipped counts; reuse the payload for subsequent runnable commands.

## Risks / Trade-offs

- Explicitly enabled user hooks can still encounter strict serialization errors; this change is the requested default shutdown, not complete encoding remediation.
- User-modified managed definitions stay preserved and retain the existing fingerprint-mismatch behavior.
- Disabled sources appear as a source diagnostic with no active handlers; there is no new managed-hook toggle.

## Migration Plan

Use seed revision 2026-09-17.1. Startup upgrades unchanged official definitions, including recognized historical copies with stale ownership state. Verify fresh install, old/equal marker repair and preservation tests. Any later re-enable must use a newer seed revision so normal downgrade protection remains effective.

## Validation

An isolated MSVC Release harness rebuilt the changed hook registry, manager and seeder sources with the repository hook/seeder tests. All 85 tests across 8 suites passed (0 failures), including disabled malformed-UTF-8 dispatch, fresh seed installation, prior official upgrades and user configuration preservation. No dependency stubs were used. Canonical seed hashes and active example parity also passed. Full daemon/desktop packaging was not run.

A missing hooks.json in an empty directory remains repairable when the saved seed state proves ACECode ownership. Unknown empty directories and directories containing any additional user file remain preserved. The equal-version gate uses this same ownership rule.
