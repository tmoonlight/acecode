## 1. Detail Model and Git Lookup

- [x] 1.1 Add pure helpers for workspace-detail eligibility, conditional branch presentation, and viewport-safe card placement.
- [x] 1.2 Add a bounded per-directory Git-information cache with in-flight request deduplication and explicit invalidation.
- [x] 1.3 Cover no-workspace privacy, Git/non-Git presentation, positioning, cache expiry, retry, and invalidation in Node unit tests.

## 2. Sidebar Integration

- [x] 2.1 Add a portaled hover/focus detail card to the shared `SessionRow` renderer without changing row actions or geometry.
- [x] 2.2 Fetch Git information lazily for eligible rows, share cached results between pinned and grouped rows, and invalidate results on Git-state events.
- [x] 2.3 Style the card with existing theme tokens, wrapped path text, non-interactive pointer behavior, and reduced-motion support.

## 3. Verification

- [x] 3.1 Run the focused hover-detail unit tests and relevant sidebar architecture tests.
- [x] 3.2 Run the complete WebUI test suite and production build.
- [x] 3.3 Run strict OpenSpec validation and review the final diff for no-workspace and unrelated-behavior regressions.
