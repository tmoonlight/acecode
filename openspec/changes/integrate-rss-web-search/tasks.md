## 1. Specification and Planning

- [x] 1.1 Record client architecture, default behavior, fallback semantics, and security boundaries.
- [x] 1.2 Add the RSS web-search capability specification and pass strict validation.

## 2. Test-Driven Backend Work

- [x] 2.1 Add failing tests for URL construction, JSON parsing, metadata, response bounds, abort, and HTTP error mapping.
- [x] 2.2 Add failing router tests for default RSS selection and non-sticky regional fallback on empty/unavailable RSS results.
- [x] 2.3 Add failing configuration and output-format tests.

## 3. Implementation

- [x] 3.1 Implement and register `RssSearchBackend` using existing network/proxy primitives.
- [x] 3.2 Add RSS configuration defaults, parsing, persistence, and backend validation.
- [x] 3.3 Extend generic search hits and Markdown formatting with optional RSS metadata.
- [x] 3.4 Implement region-aware, non-sticky RSS fallback while preserving existing HTML fallback behavior.
- [x] 3.5 Update user-facing web-search documentation and status text.

## 4. Validation

- [x] 4.1 Run focused web-search/config tests and the complete unit suite.
- [x] 4.2 Build AceCode and run repository quality checks.
- [x] 4.3 Verify the live hosted API and an AceCode web-search smoke path.
- [x] 4.4 Run strict OpenSpec validation and inspect the final diff/status.

## 5. Three-Source Parallel Search

- [x] 5.1 Update proposal, design, and capability requirements for default
  parallel RSS + DuckDuckGo + Bing CN search, bounded merge semantics, and
  partial failure behavior.
- [x] 5.2 Add router tests proving concurrent fan-out, stable interleaving,
  URL deduplication, total limit enforcement, partial success, and aggregate
  all-source failure.
- [x] 5.3 Add configuration, slash-command, and rendering tests for the
  `parallel` default and per-result backend provenance.
- [x] 5.4 Implement the virtual `parallel` router mode while retaining explicit
  `auto`, `rss`, `duckduckgo`, and `bing_cn` compatibility.
- [x] 5.5 Render source provenance and bounded partial-failure warnings, update
  user documentation, and fix the Windows `min` macro compilation failure.
- [x] 5.6 Run focused tests, the complete unit suite, production build, live
  RSS smoke, strict OpenSpec validation, and final diff/status checks.
