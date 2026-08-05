## Why

AceCode's current built-in web search depends on scraping DuckDuckGo or Bing HTML. Those sources can be rate-limited, structurally unstable, or unreliable from mainland China. AceCode now has a hosted RSS search service with a stable JSON API and no end-user API-key setup, but the client does not yet use it.

## What Changes

- Add the hosted AceCode RSS service as a `web_search` backend.
- Make a three-source parallel mode the default: every search starts RSS,
  DuckDuckGo, and Bing CN concurrently.
- Merge the three responses with stable round-robin ordering, URL
  deduplication, and the existing total result limit so no single source can
  monopolize the model context.
- Return available results when one or two sources fail, while retaining
  `auto`, RSS, DuckDuckGo, and Bing CN as explicit single-source options.
- Preserve source and publication metadata in search results shown to the model.
- Add a configurable RSS base URL for self-hosted or alternate deployments.
- Add parser, routing, configuration, formatting, and live smoke coverage.

## Capabilities

### New Capabilities

- `rss-web-search-backend`: Defines the hosted RSS JSON client, safe response parsing, parallel/default routing, metadata rendering, and explicit RSS fallback behavior.

### Modified Capabilities

- `web-search-tool`: The existing tool gains three-source parallel search, per-result backend provenance, partial-failure warnings, and RSS source/publication metadata.

## Impact

- Affected C++ code: `src/tool/web_search/`, `src/config/config.*`, and related tests.
- No new third-party dependency; implementation reuses cpr and nlohmann/json.
- Default web search contacts `https://ge.bigjuan.xyz/rss-search`, DuckDuckGo,
  and Bing CN concurrently; wall-clock latency is bounded by the slowest
  source rather than the sum of three sequential requests.
- Existing users can retain prior regional or single-source behavior with
  `web_search.backend = "auto"`, `rss`, `duckduckgo`, or `bing_cn`.
