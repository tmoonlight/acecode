## ADDED Requirements

### Requirement: Configure the complete ordered skill scan path list
The system SHALL accept `skills.scan_paths` as an optional ordered string array replacing ordinary default and legacy external scan roots. Missing or null SHALL preserve legacy discovery and priority; an empty array SHALL scan no ordinary roots. Relative paths SHALL resolve along the current workspace ancestor chain excluding HOME; absolute and home-relative paths SHALL be global. Unresolved environment variables SHALL be skipped. Expert package roots SHALL retain first priority.

#### Scenario: Custom paths replace defaults and respect order
- **WHEN** a user saves a list of global and project-relative paths
- **THEN** only those ordinary paths are scanned in list order, with nearer ancestors first for each relative path
- **AND** UI listing, commands and session skill discovery agree on duplicate resolution

#### Scenario: Empty list and restore defaults
- **WHEN** the user saves an empty array
- **THEN** ordinary skill discovery is disabled without deleting any files
- **WHEN** the user restores defaults
- **THEN** existing compatibility flags and external directories determine discovery again

### Requirement: Edit scan paths in Settings and navigate from Skills
Settings Configuration SHALL provide a labeled multiline Skill scan paths editor, explicit save, restore defaults, and loading/error/saved feedback. Skills SHALL show a keyboard-accessible link beside its description that opens Configuration and locates the editor. Settings search SHALL find the field in Chinese and English.

#### Scenario: Navigate and save
- **WHEN** a user activates the scan-path link in Skills
- **THEN** Configuration opens with the scan-path field visible and focused
- **WHEN** the user saves valid paths
- **THEN** the configuration is persisted and existing session skill policies refresh at their control boundary

#### Scenario: Invalid or failed save
- **WHEN** the request contains a non-array or non-string path, or persistence fails
- **THEN** the server rejects the request without changing active paths
- **AND** the editor retains the user's draft and displays an error

### Requirement: Settings skill catalog reflects discovered files
The settings skill catalog SHALL include only skills discovered under the effective scan paths. Disable preferences SHALL affect the enabled state of discovered skills without generating synthetic catalog entries or being removed by a list refresh.

#### Scenario: A disabled skill is deleted and later restored
- **GIVEN** a discovered skill is disabled
- **WHEN** its SKILL.md is removed and the catalog is refreshed
- **THEN** the skill disappears from the catalog
- **AND** other discovered skills remain listed
- **WHEN** the same skill is restored and the catalog is refreshed
- **THEN** it appears again with its disabled preference preserved

#### Scenario: A disabled skill leaves the scan scope
- **WHEN** its directory is removed from the configured scan paths or is outside the selected workspace
- **THEN** the skill does not appear as a synthetic global entry
- **AND** the disable preference is preserved for other workspaces or later discovery

### Requirement: Paginate skills in Settings
The Skills settings page SHALL offer page sizes of 20, 50 and 100, default to 50 when no valid preference exists, and remember the selected size. The global list and each expanded workspace list SHALL paginate independently using that shared size and SHALL render only their current page's skill cards.

#### Scenario: Large catalog and page navigation
- **WHEN** a catalog contains more than 50 skills and the user has no saved page-size preference
- **THEN** each visible list renders at most 50 cards with total count and page navigation
- **WHEN** the user changes a list's page
- **THEN** that list shows the corresponding cards without changing other lists' pages
- **AND** the page's controls remain reachable by keyboard

#### Scenario: Search and page-size changes
- **WHEN** the user searches skills
- **THEN** the filter covers the complete catalogs before pagination and each list starts on page 1
- **WHEN** the user selects 20 or 100 entries per page
- **THEN** each list returns to page 1 using the selected size

#### Scenario: Toggle and refreshed catalog
- **WHEN** a user toggles a skill on a later page
- **THEN** its enabled state updates without resetting the page and enabled counts still describe the complete catalog
- **WHEN** a refresh makes the current page out of range
- **THEN** the list shows its last valid page, or the existing empty state if no entries remain

## MODIFIED Requirements

### Requirement: Discover skills from .agent skill directories
Unless an explicit scan-path list overrides default discovery, the system SHALL discover and register skills from the default `.agent` skill directory in addition to the default `.acecode` skill directory, using the same `SKILL.md`-based directory structure and metadata parsing rules.

#### Scenario: Skills are found in the .agent directory
- **WHEN** the user has a valid skill with `SKILL.md` under the supported default `.agent` skill root and has not configured an explicit scan-path list
- **THEN** the system discovers that skill during skill registry scan
- **THEN** the skill is available to list and load through the existing skill interfaces
