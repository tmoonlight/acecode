## 1. Shared tree model

- [x] 1.1 Add validated Changes view-mode preference constants and a pure projection from change rows to normalized directory/file nodes.
- [x] 1.2 Cover nested/root paths, Windows/workspace path normalization, deterministic directory-first ordering, duplicate names, and invalid preference fallback with Node tests.

## 2. Changes tab presentation

- [x] 2.1 Extract a shared compact change-file renderer that preserves the existing flat rows and adds accessible tree directories, indentation, collapse/expand, selected-file ancestor expansion, and selection scrolling.
- [x] 2.2 Add the persistent flat/tree toolbar control in `SidePanel` and route both Git and session change rows through the shared renderer without changing refresh, base, diff, preview, or deleted-file behavior.
- [x] 2.3 Add scoped token-based styles for the view control and hierarchical rows, including narrow-panel truncation, selected/hover states, and focus visibility.

## 3. Regression coverage and verification

- [x] 3.1 Update architecture coverage to require the shared Git/session renderer, single preference owner, accessible toggle, and retained desktop review metadata.
- [x] 3.2 Run focused tests, the full Web test suite and production build, strict OpenSpec validation, and Git whitespace/scope checks.

## 4. Tree default and working-directory preference

- [x] 4.1 Make tree mode the validated default and replace the scalar preference with a working-directory-keyed persistent model.
- [x] 4.2 Wire `SidePanel` to read and update only the current working directory's mode while retaining one preference owner for Git and session changes.
- [x] 4.3 Extend pure-model and architecture tests for default-tree fallback, normalized directory buckets, workspace isolation, and scoped persistence wiring.
- [x] 4.4 Run focused tests, the full Web test/build/i18n gates, strict OpenSpec validation, and Git scope checks for the follow-up.
