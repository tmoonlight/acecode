## 1. Seed Version Contract

- [x] 1.1 Add the canonical `assets/seed/seed.version` asset and keep its revision
  synchronized with `MANIFEST.json`.
- [x] 1.2 Add validated `YYYY-MM-DD.N` parsing/comparison and resolve the packaged and
  user marker paths without a compiled bundle-version constant.

## 2. Ownership-Aware Reconciliation

- [x] 2.1 Add deterministic full-directory SHA-256 calculation and backward-compatible
  reading/writing of `.seed_skills_state.json`.
- [x] 2.2 Implement the missing, pristine-update, unchanged, user-preserved, failure,
  and downgrade branches with staging and rollback.
- [x] 2.3 Serialize reconciliation across threads/processes and atomically persist
  detailed state before advancing `~/.acecode/seed.version`.

## 3. Startup Integration and Documentation

- [x] 3.1 Replace first-initialization-only TUI and daemon calls with best-effort
  startup reconciliation before the first registry scan.
- [x] 3.2 Update skill documentation to describe the eight-skill bundle, version
  marker semantics, ownership-aware updates, and release bump requirement.

## 4. Verification

- [x] 4.1 Extend focused seeder tests for existing homes without markers, equal and
  newer markers, pristine upgrades, modified/unknown preservation, invalid bundles,
  retryable failures, complete-directory hashes, and concurrent calls.
- [x] 4.2 Validate manifest/version/asset agreement and same-startup registry
  visibility.
- [x] 4.3 Run the focused C++ unit tests, relevant build gate, strict OpenSpec
  validation, code-quality checks, and `git diff --check`.
