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

### Requirement: Default routing and fallback
AceCode SHALL use RSS as the default web-search backend while preserving explicit legacy backend selection.

#### Scenario: Default configuration
- **WHEN** no `web_search.backend` is configured
- **THEN** the active backend SHALL be `rss`

#### Scenario: Explicit auto configuration
- **WHEN** `web_search.backend` is `auto`
- **THEN** AceCode SHALL select DuckDuckGo for a global region and Bing CN for China or unknown regions

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
