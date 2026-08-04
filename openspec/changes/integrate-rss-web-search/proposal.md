## Why

AceCode's current built-in web search depends on scraping DuckDuckGo or Bing HTML. Those sources can be rate-limited, structurally unstable, or unreliable from mainland China. AceCode now has a hosted RSS search service with a stable JSON API and no end-user API-key setup, but the client does not yet use it.

## What Changes

- Add the hosted AceCode RSS service as a `web_search` backend.
- Make RSS the default backend for new/default configurations while retaining `auto`, DuckDuckGo, and Bing CN as explicit options.
- Fall back to the region-appropriate HTML backend when RSS is unavailable or has no matches, without permanently abandoning RSS after a transient miss.
- Preserve source and publication metadata in search results shown to the model.
- Add a configurable RSS base URL for self-hosted or alternate deployments.
- Add parser, routing, configuration, formatting, and live smoke coverage.

## Capabilities

### New Capabilities

- `rss-web-search-backend`: Defines the hosted RSS JSON client, safe response parsing, default routing, metadata rendering, and coverage/outage fallback behavior.

### Modified Capabilities

- `web-search-tool`: The existing tool gains RSS as its default backend and exposes source/publication metadata when available.

## Impact

- Affected C++ code: `src/tool/web_search/`, `src/config/config.*`, and related tests.
- No new third-party dependency; implementation reuses cpr and nlohmann/json.
- Default web search now contacts `https://ge.bigjuan.xyz/rss-search` before HTML search.
- Existing users can retain prior behavior with `web_search.backend = "auto"`.
