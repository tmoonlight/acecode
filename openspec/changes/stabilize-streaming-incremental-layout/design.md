## Context

PR #26 combined three optimization layers. The completed-message cache (L1) is conceptually safe, but its storage is only sized during whole-transcript resets, so ordinary message growth bypasses it. The line/token-freezing path (L2/L3) is live for the final streaming assistant message, yet it commits Markdown before later lines can reclassify the block and it renders raw deltas without the full formatter's XML filtering. Existing tests assert that elements exist or measure speed; they do not establish semantic equivalence.

The governing constraint is that a render cache is optional: disabling it may reduce performance, but it must never alter visible content. The established `format_markdown` path already owns XML stripping, exception fallback, link collection, and the parser's complete-context semantics.

## Goals / Non-Goals

**Goals:**

- Restore live-stream rendering to the same semantic path used for final assistant messages.
- Retain L1 caching for unchanged completed messages and make it work as the transcript grows.
- Turn each reported P1 into a deterministic regression or contract test.
- Keep long-running measurement code out of the default unit-test suite.
- Leave an explicit gate for any future incremental formatter: semantic equivalence must be proven before production use.

**Non-Goals:**

- Designing a fully resumable CommonMark block parser in this repair.
- Preserving the PR's claimed per-delta O(1) behavior at the expense of correctness.
- Changing web/desktop Markdown behavior, the provider stream protocol, or stored transcripts.
- Adding configuration switches for an unsafe implementation.

## Decisions

### 1. Roll back L2/L3 from the production TUI path

The live assistant message will again be rendered through the existing full-message helper at redraw time. `on_delta` will only append transcript content and request a redraw; it will not construct or mutate a separate Markdown element tree.

This is preferred over patching individual delimiter cases because paragraph continuation, tables, lazy blockquote continuation, list continuation, setext headings, and other block constructs all depend on future lines. A delimiter stack cannot make a completed line a safe Markdown commit boundary. It also avoids rendering work on every provider delta when the UI intentionally redraws at a slower cadence.

Alternative considered: keep `StreamingFormatter` live and rebuild from the full raw message whenever XML or context becomes ambiguous. That restores correctness but can do more full parses than the pre-merge redraw path and leaves two rendering authorities to synchronize, so it is rejected for this stabilization.

### 2. Grow L1 cache storage without resetting valid entries

`MessageRenderCache` will expose a grow-only capacity operation. Normal transcript synchronization will call it with the conversation size. Existing `resize` remains the destructive reset operation used when replacing or resetting a transcript.

This separates two meanings that the current API conflates: structural reset and append capacity. Tests will store an old entry, grow the cache, verify the old entry remains valid, then store the newly appended index.

### 3. Remove the partial lexer and keep only a correctness-first compatibility wrapper

Unsafe production references and state ownership will be removed. `LexerState` and its line-freezing helpers will be removed because a callable partial block parser creates ambiguity without providing safe value. The pre-existing `StreamingFormatter` API will remain outside production but will format its complete accumulated source on every append, making it a correctness-first compatibility wrapper.

Future reactivation requires differential tests comparing full and chunked rendering for paragraphs, tables, lists, blockquotes, fences, inline delimiters, split XML tags, width changes, and theme changes. Performance results alone are insufficient.

### 4. Make the benchmark explicitly opt-in

The timing harness will be a disabled GoogleTest. Developers can run it deliberately with GoogleTest's disabled-test flag and an exact filter. Default focused and full test runs must not execute it.

### 5. Correct acceptance documentation

The original design record will state that L2/L3 production integration was rolled back because its equivalence claim was not established. It will retain L1 as the accepted optimization and list the differential-test gate for future work.

## Risks / Trade-offs

- [Risk] Very long currently streaming Markdown again incurs full formatting on redraw. → Mitigation: redraw pacing bounds frequency, unchanged completed messages use L1, and future incremental work can be reintroduced after equivalence tests pass.
- [Risk] Experimental APIs could be mistaken for supported production behavior. → Mitigation: remove all production references, adjust tests/comments, and correct the acceptance record.
- [Risk] Cache growth can retain more elements over a long session. → Mitigation: storage remains indexed by transcript messages and existing reset/invalidation paths clear it when the transcript is replaced.
- [Risk] A static regression may miss integration wiring. → Mitigation: combine unit tests with source-path assertions or focused TUI render-helper tests where practical, then run the full C++ suite.

## Migration Plan

1. Add regressions for cache growth and Markdown full-vs-chunked counterexamples.
2. Add grow-preserving L1 capacity and wire it into conversation synchronization.
3. Remove the production `StreamingFormatter` lifecycle and render branch.
4. Disable the benchmark by default and correct the prior acceptance record.
5. Run focused tests, a full suite excluding no tests beyond GoogleTest-disabled benchmarks, and `git diff --check`.

Rollback is straightforward: the changes are confined to the merged optimization surface. If L1 itself proves unstable, the message cache can be bypassed while retaining the full formatter path.

## Open Questions

None for this repair. A future incremental-parser proposal must separately choose and prove a block-state model.
