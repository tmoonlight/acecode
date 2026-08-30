## Why

The recently merged TUI streaming-layout optimization freezes Markdown lines before enough following context exists, bypasses `strip_xml` during streaming, and never grows its completed-message cache during normal conversation growth. These are correctness regressions that can make live output disagree with the final message or silently disable the only safe optimization, so the merge must be stabilized before further performance work builds on it.

## What Changes

- Add deterministic regressions for paragraph continuation, tables, split XML tags, and cache growth so the reported P1 failures are directly reproducible.
- Preserve the completed-message render cache, but make its capacity grow without invalidating already cached messages.
- Remove the unproven line/token-freezing formatter from the live TUI path and restore the established full `format_markdown` path for the currently streaming message.
- Remove the unproven incremental lexer and keep the legacy streaming formatter API as a correctness-first full-content compatibility wrapper outside production.
- Make the expensive timing harness opt-in so normal unit-test runs do not spend minutes in a non-asserting benchmark.
- Correct the earlier acceptance documentation to record the rollback and the remaining performance work instead of claiming the unsafe L2/L3 path is production-ready.

## Capabilities

### New Capabilities

- `tui-streaming-markdown-stability`: Defines correctness, cache-growth, fallback, and benchmark-isolation requirements for TUI assistant streaming.

### Modified Capabilities

None.

## Impact

- Affected production code: `src/main.cpp`, `src/tui_state.hpp`, and `src/tui/message_render_cache.hpp`.
- Affected Markdown code/tests: `src/markdown/markdown_formatter.*`, removal of the PR's `LexerState` additions from `src/markdown/markdown_lexer.*`, and full-vs-chunked regressions under `tests/markdown/`.
- Affected documentation: the 2026-08-27 streaming incremental-layout design and implementation records.
- No daemon, web API, persisted-session, or user-configuration contract changes.
