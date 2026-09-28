<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The dump identifies strict JSON serialization inside HookManager::dispatch_codex after a bash result. The original heap text is absent, so its exact source text is unknown. A GBK byte sequence beginning C0 reproduces the exception because the existing bit-pattern-only validator incorrectly accepts it as UTF-8.

## Decisions

Use one scalar-value-aware UTF-8 sequence scanner for validation, partial boundaries, and lossy replacement. Reject C0/C1, overlong E0/F0 sequences, ED surrogate sequences, F4 values above U+10FFFF, and F5-FF. Validate available continuation bytes before treating a suffix as incomplete.

Incremental Windows decoding retains DBCS lead bytes using the configured codepage. Once invalid UTF-8 selects a legacy codepage, retain that interpretation until reset so later GBK pairs that happen to resemble UTF-8 do not change encoding midstream. Explicit UTF-8 codepage and POSIX streams replace malformed bytes without reinterpreting valid adjacent Unicode. Windows auto-detection uses the active system codepage when the console is UTF-8 or absent, because redirected legacy child diagnostics can still use that codepage.

Hook payload serialization uses the JSON library's replacement policy, including nested strings and keys. Serialization stays lazy after matching, trust, and enabled checks. Convert runner exceptions into existing failed-hook diagnostics and continue other hooks, preserving explicit allow/deny decisions. Legacy synchronous/asynchronous hooks log failures and keep their worker alive.

Catch exceptions at the AgentLoop task boundary. Restore idle and turn state before reporting, isolate reporting callbacks individually, emit terminal error events, and avoid automatic goal continuation after an unexpected task error. Keep queued user work. This is a last-resort boundary; source decoding and hook containment handle the known dump path.

## Risks

Encoding auto-detection cannot distinguish a legacy byte stream that is entirely valid UTF-8; UTF-8 remains preferred until contradictory input. Allocation failures are not guaranteed recoverable. Tests exercise ordinary standard and non-standard exceptions, callback failures, and subsequent-task progress.

## Validation

Reproduce the dump's index-58 C0 serializer failure before repair. Test all scalar boundaries, invalid sequence classes, every chunk split for Unicode and GBK, active and inactive hooks, legacy asynchronous recovery, and AgentLoop recovery. Run the complete backend and frontend checks before the full release, then verify packaged seed migrations and all six public updater packages.
