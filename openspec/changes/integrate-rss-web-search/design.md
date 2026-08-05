## Context

The existing `BackendRouter` owns DuckDuckGo and Bing CN backends and selects one from configuration plus detected region. RSS is a curated search source, not a complete web index, so treating it as an ordinary mutually exclusive backend would make zero-result queries look like definitive web misses.

## Decisions

### 1. Keep one LLM tool

The integration keeps the existing `web_search` tool name and schema. A separate `rss_search` tool would expose overlapping choices to the model and would not satisfy the goal of providing a built-in default search path.

### 2. Three-source parallel search is the default

`WebSearchConfig::backend` defaults to `parallel`. A search snapshots the RSS,
DuckDuckGo, and Bing CN backends and starts all three with `std::async` using
`std::launch::async`. Explicit `auto` continues selecting DuckDuckGo for global
regions and Bing CN for China/unknown regions. Explicit `rss`, `duckduckgo`,
and `bing_cn` remain available for debugging and controlled deployments.

### 3. Parallel aggregation is bounded and source-balanced

Each backend receives the requested limit, but the combined response still
returns at most that many hits. Results are consumed one at a time in stable
round-robin order (`rss`, `duckduckgo`, `bing_cn`) and deduplicated by a
normalized URL key. This ensures a default limit of five can represent all
three sources instead of being filled by whichever future completes first.

Every merged hit records its originating backend. RSS publisher and
publication metadata remain separate from backend provenance.

If at least one backend succeeds, its available results are returned and
failures from the other sources become bounded warnings. Only when all three
backends fail does the tool return an aggregate error. Parallel searches never
change the active backend or rewrite the region cache.

### 4. Explicit single-source fallback remains compatible

When the configured/session-selected backend is `rss`, its existing empty or
recoverable-error regional fallback remains per-request and non-sticky. The
legacy DuckDuckGo/Bing network fallback remains sticky as before. `auto`
continues to use the cached region.

### 5. Validate external JSON at the boundary

The RSS backend accepts only an object containing a results array. Each result must have non-empty string title and URL fields; only bounded HTTP(S) URLs without control characters are emitted. Optional snippet, source, and published_at values must be strings and are whitespace-normalized. Result count and text sizes are bounded before creating `SearchHit` values. The HTTP transfer itself stops at 2 MiB, so the bound is not merely a post-download check.

### 6. Configuration

`web_search.rss_base_url` defaults to `https://ge.bigjuan.xyz/rss-search`. The backend normalizes a trailing slash and appends `/v1/search`. Configuration requires HTTPS, except that loopback self-hosting may use HTTP; credentials, query strings, fragments, whitespace, and control characters are rejected.

### 7. Stable backend lifetime

The router stores backends with shared ownership and snapshots a `shared_ptr` before releasing its mutex. Test-time or future runtime backend replacement therefore cannot invalidate a search already in flight.

## Risks

- Hosted-service privacy: search queries are sent to the configured RSS endpoint. This matches the behavior of all remote search backends and must be documented clearly.
- Corpus coverage: mitigated by always combining the curated RSS source with
  both public web engines in the default mode.
- Source outage: mitigated by partial-success semantics; one failed source does
  not discard usable results from the other two.
- Parallel fan-out increases request count to three per tool call, accepted to
  improve domestic and global coverage. The hosted RSS service owns its
  server-side rate limiting and corpus operations.
