## MODIFIED Requirements

### Requirement: Expanded workspace lists can be collapsed again
The web sidebar SHALL reveal a long workspace session list in batches of five and SHALL provide an explicit collapse operation once every visible session has been revealed. Each batch SHALL be loaded from the daemon on demand with a bounded request; the sidebar MUST NOT request the complete session list of a workspace to expand it.

#### Scenario: Expand long list
- **WHEN** a workspace list currently shows N rows, more visible sessions exist, and the user clicks "展开显示"
- **THEN** the sidebar MUST render the next five sessions, for N+5 rows in total
- **THEN** the request it sends to the daemon MUST be bounded to the sessions needed for those rows plus pinned entries, and MUST NOT ask for the complete session list
- **THEN** it MUST render "展开显示" again while more visible sessions remain

#### Scenario: All sessions revealed
- **WHEN** the revealed rows include every visible session in that workspace
- **THEN** the sidebar MUST render a "折叠显示" control as the last row

#### Scenario: Collapse long list
- **WHEN** the user clicks "折叠显示"
- **THEN** the sidebar MUST return to rendering only the first five sessions
- **THEN** it MUST render "展开显示" again

#### Scenario: Refresh while expanded
- **WHEN** a periodic sidebar refresh runs while a workspace list is expanded to N rows
- **THEN** the refresh MUST request only the sessions needed for those N rows plus pinned entries, and MUST NOT request the complete session list
