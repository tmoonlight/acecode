## MODIFIED Requirements

### Requirement: Search result rendering
The `web_search` tool SHALL render generic title, URL, and snippet fields,
identify the originating backend for parallel results, and include source and
publication time when the selected backend provides them.

#### Scenario: Parallel result provenance
- **WHEN** a result was returned by parallel search
- **THEN** the rendered result SHALL identify whether it came from RSS,
  DuckDuckGo, or Bing CN

#### Scenario: Partial parallel warning
- **WHEN** parallel search returns results while one or more sources fail
- **THEN** the rendered tool result SHALL include a concise warning after the
  usable results

#### Scenario: RSS metadata is available
- **WHEN** a search hit contains source and publication time
- **THEN** the tool output SHALL present both values adjacent to that result

#### Scenario: Legacy backend has no metadata
- **WHEN** a DuckDuckGo or Bing result omits source and publication time
- **THEN** the existing title, URL, and snippet rendering SHALL remain valid without empty metadata labels
