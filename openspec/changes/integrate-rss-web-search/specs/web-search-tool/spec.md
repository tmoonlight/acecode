## MODIFIED Requirements

### Requirement: Search result rendering
The `web_search` tool SHALL render generic title, URL, and snippet fields and SHALL include source and publication time when the selected backend provides them.

#### Scenario: RSS metadata is available
- **WHEN** a search hit contains source and publication time
- **THEN** the tool output SHALL present both values adjacent to that result

#### Scenario: Legacy backend has no metadata
- **WHEN** a DuckDuckGo or Bing result omits source and publication time
- **THEN** the existing title, URL, and snippet rendering SHALL remain valid without empty metadata labels
