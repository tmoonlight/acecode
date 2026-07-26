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

## 6. Acceptance feedback polish

- [x] 6.1 Show the selection actions only after mouse release and anchor them beside the release cursor, with a range fallback for keyboard selections.
- [x] 6.2 Capture source-mode start/end lines from explicit row metadata so composer labels remain accurate for multiline log, code, text, and Markdown selections.
- [x] 6.3 Position resolved annotation bubbles to the left of their marked passage and strengthen annotated marks for both light and dark themes.
- [x] 6.4 Add focused regression coverage and run the Web test suite plus production build.
- [x] 6.5 In the real UI, annotate selections longer than two lines in both `1.log` and `2.md`, then verify cursor placement, card line ranges, left-side bubbles, hover content, and light/dark contrast.

## Acceptance feedback verification

- Raw Playwright Web UI — selected and annotated `1.log` lines 17–20 and `2.md` lines 1–4 in the pinned `测试任务`.
- Cursor placement — both action surfaces used the `pointer` anchor and opened 8–10 px to the right of the mouse-release point rather than at the multiline union edge.
- Composer numbers — cards displayed `1.log:17-20` and `2.md:1-4`, each with the expected single-annotation count.
- Source decorations — resolved bubbles were immediately left of the first marked fragment; hover cards opened to the bubble's right and showed the submitted annotation.
- Blank-line anchoring — the Markdown selection included empty rows and resolved immediately without `原文已变化`.
- Theme contrast — light mode used a 0.18 accent fill with a 0.68 inset outline; dark mode used a 0.26 fill with a 0.82 inset outline.
