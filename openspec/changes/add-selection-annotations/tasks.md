## 1. Selection annotation data model

- [x] 1.1 Extend selection-context creation and normalization with source offsets, persisted anchor text, bounded annotations, stable annotation IDs, and annotation-aware presentation.
- [x] 1.2 Add pure helpers for merging annotations at one composer location and collecting current-session selection contexts from transcript content parts.
- [x] 1.3 Add JavaScript regression tests for annotation normalization, merging, legacy compatibility, transcript collection, and existing plain-reference behavior.

## 2. Selection action and annotation UI

- [x] 2.1 Add tested viewport placement helpers and a portal-based selection action popover with exact `引用到聊天` / `批注` actions.
- [x] 2.2 Add the focused annotation editor with required text, `Enter` submit, `Shift+Enter` newline, `Escape` cancel, outside dismissal, and accessible labels.
- [x] 2.3 Wire selection snapshots, quote pinning, annotation merging, and explicit inactive-selection cleanup into `ChatView`.
- [x] 2.4 Extend composer and sent-message selection cards with a compact numbered annotation marker and hover/focus annotation content while retaining the existing card surface.

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
- Real Web UI — confirmed the exact action labels, annotation editor validation and keyboard behavior, same-location annotation merging, sent-card markers, source marks, plain-reference/no-bubble behavior, grouped hover content, reload persistence, session isolation, and non-overlapping `原文已变化` bubbles. Browser error/warning log was empty.

## 6. Acceptance feedback polish

- [x] 6.1 Show the selection actions only after mouse release and anchor them beside the release cursor, with a range fallback for keyboard selections.
- [x] 6.2 Capture source-mode start/end lines from explicit row metadata so composer labels remain accurate for multiline log, code, text, and Markdown selections.
- [x] 6.3 Position resolved annotation bubbles to the left of their marked passage and strengthen annotated marks for both light and dark themes.
- [x] 6.4 Add focused regression coverage and run the Web test suite plus production build.
- [x] 6.5 In the real UI, annotate selections longer than two lines in both `1.log` and `2.md`, then verify cursor placement, card line ranges, left-side bubbles, hover content, and light/dark contrast.

## Acceptance feedback verification

- Raw Playwright Web UI — selected and annotated `1.log` lines 17–20 and `2.md` lines 1–4 in the pinned `测试任务`.
- Cursor placement — both action surfaces used the `pointer` anchor and opened 8–10 px to the right of the mouse-release point rather than at the multiline union edge.
- Composer annotations — cards displayed `1.log:17-20` and `2.md:1-4`, each with the expected single-annotation indicator.
- Source decorations — resolved bubbles were immediately left of the first marked fragment; hover cards opened to the bubble's right and showed the submitted annotation.
- Blank-line anchoring — the Markdown selection included empty rows and resolved immediately without `原文已变化`.
- Theme contrast — light mode used a 0.18 accent fill with a 0.68 inset outline; dark mode used a 0.26 fill with a 0.82 inset outline.

## 7. Chat annotation numbering regression

- [x] 7.1 Derive one per-file passage-number presentation from active-session and pending selection contexts, then reuse it for preview bubbles, composer cards, and sent-message cards.
- [x] 7.2 Make annotated card hover/focus content use the grouped normalized annotations and prevent empty annotation tooltips.
- [x] 7.3 Add focused regression coverage for `1` / `2` / `3` parity, same-passage grouping, per-file reset, plain contexts, and non-empty hover content.
- [x] 7.4 Run the Web suite/build and verify in the real UI that three annotated passages show matching `1` / `2` / `3` numbers in details and chat, with the correct content visible on hover or keyboard focus.

## Chat annotation numbering verification

- `pnpm test` — passed.
- `pnpm build` — passed.
- `openspec validate add-selection-annotations --strict` — passed.
- Real Web UI — loaded three sent annotations for `1.log` lines 4, 8, and 12; details and chat both displayed `1`, `2`, and `3`, all three source anchors resolved, and no stale annotation was reported.
- Card tooltip — focusing the chat marker `2` rendered a fixed top-level tooltip with `第二处批注：检查 grep 工具注册`; computed visibility was `visible` with opacity `1`.

## 8. Hide annotations after document changes

- [x] 8.1 Capture and persist a bounded full-document content revision with new selection annotations, then gate preview decorations by the freshly loaded revision without adding any interaction.
- [x] 8.2 Add focused regressions proving unchanged documents still display annotations while any content change silently hides old annotations.
- [x] 8.3 Run the focused Web and C++ tests, the full Web suite/build, strict OpenSpec validation, and `git diff --check`.

## Document-change hiding verification

- Focused selection-context, source-decoration, and annotation-architecture JavaScript suites — passed, including unchanged-document display and changed-elsewhere hiding.
- `cmake --build build --target acecode_unit_tests --config Release -- /m:1 /nodeReuse:false` — passed.
- `build\tests\Release\acecode_unit_tests.exe --gtest_filter=SelectionContextAnnotation.*` — 7 tests passed.
- `pnpm test` — passed.
- `pnpm build` — passed.
- `openspec validate add-selection-annotations --strict` — passed.
- `git diff --check` — passed.
