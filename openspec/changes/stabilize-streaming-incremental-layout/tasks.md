## 1. Regression Coverage

- [x] 1.1 Add a cache-growth regression proving an empty cache accepts the first appended message and preserves prior entries while growing.
- [x] 1.2 Add full-vs-chunked formatter regressions for paragraph continuation, tables, lists, and lazy blockquote continuation.
- [x] 1.3 Add a streaming formatter regression for XML wrappers split across deltas and complete-message output equivalence.

## 2. Safe Cache Retention

- [x] 2.1 Add a grow-preserving `MessageRenderCache` capacity operation while retaining destructive transcript reset semantics.
- [x] 2.2 Wire grow-preserving cache capacity to normal conversation-size synchronization.
- [x] 2.3 Publish a swapped theme palette before its cache-invalidating version so a new version cannot identify old colors.

## 3. Correctness-First Streaming Rollback

- [x] 3.1 Remove the unproven `LexerState` and its line-freezing helpers instead of leaving a callable partial parser.
- [x] 3.2 Make `StreamingFormatter` use complete accumulated-content formatting so XML and Markdown semantics remain authoritative.
- [x] 3.3 Remove the `StreamingFormatter` lifecycle, delta work, and special render branch from the production TUI path.

## 4. Test and Documentation Hygiene

- [x] 4.1 Disable the non-asserting streaming timing benchmark by default while preserving explicit opt-in execution.
- [x] 4.2 Correct the previous design and acceptance records to document the L2/L3 rollback and future equivalence gate.

## 5. Verification

- [x] 5.1 Build and run focused cache, formatter, theme, and TUI render tests (71 passed); the unsafe incremental lexer was removed.
- [x] 5.2 Run the complete default unit-test suite on the final implementation: 3677 ran, 3673 passed, 4 environment/platform smokes skipped, and 1 timing benchmark remained disabled.
- [x] 5.3 Run code-quality and diff checks, then reconcile the OpenSpec checklist: strict validation and `git diff --check` pass; the quality script completes with only repository-existing hard-coded-error and token-estimate notices.
