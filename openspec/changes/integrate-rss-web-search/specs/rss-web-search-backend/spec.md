## ADDED Requirements

### Requirement: Hosted RSS backend
AceCode SHALL provide an RSS implementation of `WebSearchBackend` that queries the configured RSS base URL without requiring an API key.

#### Scenario: Successful query
- **WHEN** the backend receives a non-empty query of at most 200 Unicode code points and a result limit
- **THEN** it SHALL call `<rss_base_url>/v1/search` with percent-encoded `q` and bounded `limit`
- **AND** return generic search hits containing title, HTTP(S) URL, snippet, source, and publication time when supplied

#### Scenario: Invalid external payload
- **WHEN** the service returns malformed JSON, a non-object root, or no results array
- **THEN** the backend SHALL return a parse error rather than exposing partial or invented results

#### Scenario: Unsafe result URL
- **WHEN** a result URL does not use HTTP or HTTPS
- **THEN** that result SHALL be discarded

### Requirement: Default parallel routing and compatibility
AceCode SHALL use three-source parallel search by default while preserving
explicit legacy and single-source backend selection.

#### Scenario: Default configuration
- **WHEN** no `web_search.backend` is configured
- **THEN** the active backend SHALL be `parallel`

#### Scenario: Parallel fan-out
- **WHEN** a search runs with the parallel backend active
- **THEN** AceCode SHALL start RSS, DuckDuckGo, and Bing CN without waiting for
  a previous source to complete
- **AND** each source SHALL receive the same query and bounded result limit

#### Scenario: Balanced bounded merge
- **WHEN** two or more sources return usable results
- **THEN** AceCode SHALL interleave results in stable RSS, DuckDuckGo, Bing CN
  order
- **AND** SHALL deduplicate equivalent normalized URLs
- **AND** SHALL return no more than the tool's requested total result limit

#### Scenario: Partial source failure
- **WHEN** at least one parallel source succeeds and another source fails
- **THEN** AceCode SHALL return the available successful results
- **AND** SHALL expose a bounded warning naming the failed source
- **AND** SHALL NOT change the active backend or region cache

#### Scenario: All sources fail
- **WHEN** RSS, DuckDuckGo, and Bing CN all fail
- **THEN** AceCode SHALL return one aggregate search error that identifies all
  three failed sources

#### Scenario: Explicit auto configuration
- **WHEN** `web_search.backend` is `auto`
- **THEN** AceCode SHALL select DuckDuckGo for a global region and Bing CN for China or unknown regions

#### Scenario: Explicit single-source configuration
- **WHEN** `web_search.backend` is `rss`, `duckduckgo`, or `bing_cn`
- **THEN** AceCode SHALL preserve the existing single-source and fallback
  behavior for that configured backend

#### Scenario: RSS has no matches
- **WHEN** RSS returns a valid response with zero hits
- **THEN** the router SHALL try the region-appropriate HTML backend for that request
- **AND** SHALL retain RSS as the active backend for future requests

#### Scenario: RSS is unavailable
- **WHEN** RSS returns a recoverable network, server, or rate-limit error
- **THEN** the router SHALL try the region-appropriate HTML backend
- **AND** SHALL retain RSS as the active backend and avoid rewriting the region cache based on the transient fallback

### Requirement: RSS configuration
AceCode SHALL expose a configurable RSS base URL with a working hosted default.

#### Scenario: Default endpoint
- **WHEN** `web_search.rss_base_url` is omitted
- **THEN** it SHALL default to `https://ge.bigjuan.xyz/rss-search`

#### Scenario: Persisted custom endpoint
- **WHEN** a non-default `rss_base_url` is loaded and later saved
- **THEN** AceCode SHALL preserve the custom value

#### Scenario: Invalid custom endpoint
- **WHEN** `rss_base_url` contains credentials, query parameters, a fragment, control characters, or non-loopback plaintext HTTP
- **THEN** configuration loading or saving SHALL reject it

### Requirement: Bounded hosted requests
AceCode SHALL enforce limits compatible with the hosted RSS API and SHALL bound response memory during transfer.

#### Scenario: Query exceeds service contract
- **WHEN** a query exceeds 200 Unicode code points
- **THEN** the tool SHALL reject it before making a network request

#### Scenario: Oversized response body
- **WHEN** an RSS response exceeds 2 MiB
- **THEN** AceCode SHALL abort the HTTP transfer and return a parse error without buffering the remainder
