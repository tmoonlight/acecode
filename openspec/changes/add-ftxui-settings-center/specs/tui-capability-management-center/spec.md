## ADDED Requirements

### Requirement: Shared capability management shell
The TUI SHALL provide one full-screen FTXUI management surface with horizontal Skills, MCP Servers, Connectors, Tools, and Hooks tabs and no Models tab.

#### Scenario: Open a requested management tab
- **WHEN** the user invokes `/skills`, `/mcp`, `/connectors`, `/tools`, or `/hooks` without arguments
- **THEN** the same management shell opens and visibly activates the corresponding tab

#### Scenario: Switch management tabs
- **WHEN** the user uses Tab, Shift+Tab, mouse selection, or the top rail
- **THEN** focus moves between the five capability tabs while their individual filter/selection state remains available for the process lifetime

### Requirement: Contextual list interaction
Each management tab SHALL provide a search band, dense selectable list, right-aligned source/status metadata, scrollbar, and a footer containing only actions valid for that tab.

#### Scenario: Filter a capability list
- **WHEN** the user enters a filter
- **THEN** matching rows remain visible, selection moves to a valid result, and source/status metadata stays aligned

#### Scenario: Empty filtered list
- **WHEN** no row matches
- **THEN** the page explains how to clear the filter or add/configure the capability

### Requirement: Skills management
Skills SHALL list discovered skills by name and scope and SHALL support enable/disable, reload, detail inspection, and opening or copying the owning directory.

#### Scenario: Toggle a skill
- **WHEN** the user toggles a skill
- **THEN** every configured occurrence governed by the existing name-based skill rule is persisted and the registry/command bindings refresh

#### Scenario: Reload skills
- **WHEN** the user activates reload
- **THEN** skill roots are rescanned asynchronously and the list plus slash bindings update in place

### Requirement: MCP server management
MCP Servers SHALL show configured server state, transport, tool count, command/endpoint, and errors and SHALL support enable, disable, reconnect, reload, details, and validated raw JSON editing.

#### Scenario: Reconnect an MCP server
- **WHEN** the user activates reconnect on a configured server
- **THEN** the existing manager tears down and reconnects it asynchronously and the row reports intermediate and final state

#### Scenario: Save invalid MCP JSON
- **WHEN** the raw editor does not contain a JSON object valid for MCP configuration
- **THEN** no configuration or runtime server changes and the editor shows the validation error

### Requirement: Connector management
Connectors SHALL show configured connector name, description, enabled state, and authentication state and SHALL support enable/disable and refresh through existing connector lifecycle hooks.

#### Scenario: Enable a connector
- **WHEN** the user enables a connector
- **THEN** the latest connector configuration is persisted and its enable lifecycle hook runs with updated status shown

#### Scenario: Connector authentication fails
- **WHEN** a refresh or enable action reports an authentication error
- **THEN** the row exposes the failure without displaying credentials and offers the supported recovery action

### Requirement: Tool management
Tools SHALL list registered tools and their built-in, Browser Bridge, or MCP source and SHALL expose persisted toggles only for configurable tool groups.

#### Scenario: Toggle Browser Bridge tools
- **WHEN** the user toggles the ACE Browser Bridge group
- **THEN** the global default is persisted and the current registry applies the supported live change or clearly reports that restart is required

#### Scenario: Inspect immutable tool
- **WHEN** the user selects a built-in or MCP-provided tool without a direct toggle
- **THEN** the page shows its source and status without presenting a nonfunctional enable control

### Requirement: Hook management
Hooks SHALL show configured hook source, matcher, command, timeout, trust, enabled state, and diagnostics and SHALL support refresh, enable/disable, and trust changes.

#### Scenario: Enable trusted hook
- **WHEN** the user enables a trusted hook
- **THEN** hook configuration and the active HookManager update and the row reflects enabled state

#### Scenario: Attempt to enable untrusted hook
- **WHEN** the user attempts to enable a hook that requires trust
- **THEN** a confirmation explains its command/source and the hook does not run until trust is explicitly granted

### Requirement: Preserve parameterized command behavior
Existing argument-taking `/skills` and `/mcp` commands SHALL remain available and SHALL not open the management shell.

#### Scenario: Parameterized MCP command
- **WHEN** the user invokes `/mcp reconnect server-name`
- **THEN** the existing command action runs and reports through the transcript

#### Scenario: Parameterized skills command
- **WHEN** the user invokes `/skills reload`
- **THEN** the existing command action runs without opening the management shell
