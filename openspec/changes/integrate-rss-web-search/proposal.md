## Why

AceCode's built-in web search combines a hosted RSS index with public HTML
search. Bing CN result quality is not acceptable for model context, so its
content must no longer enter WebSearch results or automatic fallback paths.

## What Changes

- Add the hosted AceCode RSS service as a `web_search` backend.
- Make a two-source parallel mode the default: every search starts RSS and
  DuckDuckGo concurrently.
- Merge the two responses with stable round-robin ordering, URL
  deduplication, and the existing total result limit so no single source can
  monopolize the model context.
- Return available results when one source fails, while retaining `auto`, RSS,
  and DuckDuckGo modes.
- Suppress Bing CN from production registration, automatic routing, fallback,
  session selection, and rendered results. Legacy `bing_cn` configuration is
  accepted but safely resolves to DuckDuckGo.
- Preserve source and publication metadata in search results shown to the model.
- Add a configurable RSS base URL for self-hosted or alternate deployments.
- Add parser, routing, configuration, formatting, and live smoke coverage.

## Capabilities

### New Capabilities

- `rss-web-search-backend`: Defines the hosted RSS JSON client, safe response parsing, parallel/default routing, metadata rendering, and explicit RSS fallback behavior.

### Modified Capabilities

- `web-search-tool`: The existing tool gains two-source parallel search, Bing CN suppression, per-result backend provenance, partial-failure warnings, and RSS source/publication metadata.

## Impact

- Affected C++ code: `src/tool/web_search/`, `src/config/config.*`, and related tests.
- No new third-party dependency; implementation reuses cpr and nlohmann/json.
- Default web search contacts `https://ge.bigjuan.xyz/rss-search` and
  DuckDuckGo concurrently; wall-clock latency is bounded by the slower source.
- Existing users can select `auto`, `rss`, or `duckduckgo`; a persisted
  `bing_cn` value is migrated at runtime to DuckDuckGo without emitting Bing
  content.
