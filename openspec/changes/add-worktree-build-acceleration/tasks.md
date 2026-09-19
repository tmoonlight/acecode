# Tasks

## 1. Compiler-cache integration

- [x] 1.1 Add cross-platform `sccache` discovery, installation guidance, and cache-state helpers; verify focused unit tests cover PATH, conventional paths, and absent-cache fallback.
- [x] 1.2 Configure or remove CMake compiler launchers when cache availability changes, with ordinary-build fallback on cache configuration or cache-enabled build errors; verify subprocess command tests cover both transitions.
- [x] 1.3 Collect and display best-effort per-launch `sccache` hit, miss, and error deltas; verify statistic parsing and failure fallback with focused tests.

## 2. Frontend artifact reuse

- [x] 2.1 Implement clean same-commit worktree eligibility and newest `web/dist` source selection; verify dirty, mismatched-commit, missing-output, and newest-source cases.
- [x] 2.2 Copy and validate a frontend seed before the local Vite build, with local-build fallback on copy or freshness failure; verify focused tests cover successful skip and fallback.

## 3. Integration and verification

- [x] 3.1 Wire compiler-cache and frontend-seed behavior into Web and Desktop launcher flows while preserving isolated build directories; verify focused launcher tests pass.
- [x] 3.2 Run Python compilation, OpenSpec strict validation, and an end-to-end Web startup with cache absent or present; verify daemon page reachability and report the observed acceleration path.
