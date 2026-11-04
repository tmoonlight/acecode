# Tasks

## 1. Persistent migration

- [x] 1.1 Add completion metadata, migrate disabled/missing settings before publication, and record explicit saves as completed; verify with startup, round-trip, user-disable, custom-mapping, failure/retry and legacy-path unit tests.
- [x] 1.2 Document migration and API save semantics in the existing tool rewrite API section and update settings header comments; verify all three runtime callers still load before tool registration.

## 2. Integration verification

- [x] 2.1 Build the native unit target and run focused rewrite, protocol-name, resolution and API-handler regressions; record results and any environment limitations.
- [x] 2.2 Run strict OpenSpec validation and git diff checks; review the final patch against the pre-existing dirty worktree.
