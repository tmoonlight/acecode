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
