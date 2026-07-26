## 1. Selection annotation data model

- [x] 1.1 Extend selection-context creation and normalization with source offsets, persisted anchor text, bounded annotations, stable annotation IDs, and annotation-aware presentation.
- [x] 1.2 Add pure helpers for merging annotations at one composer location and collecting current-session selection contexts from transcript content parts.
- [x] 1.3 Add JavaScript regression tests for annotation normalization, merging, legacy compatibility, transcript collection, and existing plain-reference behavior.

## 2. Selection action and annotation UI

- [x] 2.1 Add tested viewport placement helpers and a portal-based selection action popover with exact `引用到聊天` / `批注` actions.
- [x] 2.2 Add the focused annotation editor with required text, `Enter` submit, `Shift+Enter` newline, `Escape` cancel, outside dismissal, and accessible labels.
- [x] 2.3 Wire selection snapshots, quote pinning, annotation merging, and explicit inactive-selection cleanup into `ChatView`.
- [x] 2.4 Extend composer and sent-message selection cards with a compact annotation count and hover/focus annotation content while retaining the existing card surface.

## 3. Source decorations and persistence

- [x] 3.1 Implement pure exact-anchor resolution with stored-offset validation, nearest exact fallback, per-file grouping/numbering, and stale results.
- [x] 3.2 Implement DOM mark application for source and rendered Markdown plus cleanup that coexists with transient selection marks.
- [x] 3.3 Add the preview annotation overlay with aligned numbered bubbles, grouped hover cards, scroll/resize measurement, and stale indicators.
- [x] 3.4 Pass active-session transcript and pending-composer selection contexts through `PreviewDetailsPanel` into supported file previews only.
- [x] 3.5 Add focused JavaScript and architecture tests for anchoring, grouping, DOM integration seams, supported scope, and plain-reference/no-bubble behavior.

## 4. Daemon prompt and session schema

- [x] 4.1 Sanitize and persist selected anchor text, source offsets, and bounded annotation records in selection content parts.
- [x] 4.2 Include annotations with their selected text in the hidden provider prompt without changing visible `display_text`.
- [x] 4.3 Add C++ unit coverage for annotation sanitization, limits, prompt expansion, source fields, and legacy plain selections.

## 5. Verification and delivery

- [x] 5.1 Run the Web test suite and production build, fixing all feature regressions.
- [x] 5.2 Build and run the focused C++ unit coverage available in the current environment.
- [x] 5.3 Exercise quote, annotation, duplicate annotation, send/reload, session isolation, hover, and stale-anchor behavior in the real Web UI and capture visual evidence.
- [x] 5.4 Run code-quality checks, audit every specification requirement, and record final verification results.

## Verification

- `pnpm test` — passed.
- `pnpm build` — passed.
- `cmake --build build --target acecode acecode_unit_tests --config Release -j 8` — passed.
- `build\tests\Release\acecode_unit_tests.exe --gtest_filter=SelectionContextAnnotation.*` — 6 tests passed.
- `scripts\code_quality_check.bat` — completed with the repository's existing advisory findings.
- Real Web UI — confirmed the exact action labels, annotation editor validation and keyboard behavior, same-location annotation merging, sent-card counts, source marks, plain-reference/no-bubble behavior, grouped hover content, reload persistence, session isolation, and non-overlapping `原文已变化` bubbles. Browser error/warning log was empty.
