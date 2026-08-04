## Context

The existing `BackendRouter` owns DuckDuckGo and Bing CN backends and selects one from configuration plus detected region. RSS is a curated search source, not a complete web index, so treating it as an ordinary mutually exclusive backend would make zero-result queries look like definitive web misses.

## Decisions

### 1. Keep one LLM tool

The integration keeps the existing `web_search` tool name and schema. A separate `rss_search` tool would expose overlapping choices to the model and would not satisfy the goal of providing a built-in default search path.

### 2. RSS is the default, `auto` preserves legacy regional behavior

`WebSearchConfig::backend` defaults to `rss`. Explicit `auto` continues selecting DuckDuckGo for global regions and Bing CN for China/unknown regions. This gives new users the no-key hosted service while preserving an opt-out.

### 3. RSS fallback is per request and non-sticky

For RSS only, an empty successful result or a Network/RateLimited failure triggers the region-appropriate HTML backend. Parse and configuration failures remain visible instead of being masked by fallback. A successful fallback does not change the active backend or region cache, because an RSS miss does not imply the service should be abandoned for later queries.

Existing DuckDuckGo/Bing network fallback remains sticky as before.

### 4. Validate external JSON at the boundary

The RSS backend accepts only an object containing a results array. Each result must have non-empty string title and URL fields; only bounded HTTP(S) URLs without control characters are emitted. Optional snippet, source, and published_at values must be strings and are whitespace-normalized. Result count and text sizes are bounded before creating `SearchHit` values. The HTTP transfer itself stops at 2 MiB, so the bound is not merely a post-download check.

### 5. Configuration

`web_search.rss_base_url` defaults to `https://ge.bigjuan.xyz/rss-search`. The backend normalizes a trailing slash and appends `/v1/search`. Configuration requires HTTPS, except that loopback self-hosting may use HTTP; credentials, query strings, fragments, whitespace, and control characters are rejected.

### 6. Stable backend lifetime

The router stores backends with shared ownership and snapshots a `shared_ptr` before releasing its mutex. Test-time or future runtime backend replacement therefore cannot invalidate a search already in flight.

## Risks

- Hosted-service privacy: search queries are sent to the configured RSS endpoint. This matches the behavior of all remote search backends and must be documented clearly.
- Corpus coverage: mitigated by empty-result fallback.
- Service outage or incompatible payload: mitigated by safe errors and fallback.
