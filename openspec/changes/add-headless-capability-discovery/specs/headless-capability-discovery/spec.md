## ADDED Requirements

### Requirement: Headless built-in tool discovery

ACECode SHALL accept `acecode -p --list-tools` without a prompt and print the
exact currently registered built-in tool names accepted by `--disable-tools`
for a default headless invocation.

#### Scenario: List available built-in tools

- **WHEN** the user runs `acecode -p --list-tools`
- **THEN** ACECode prints a deterministic catalog containing each exact
  built-in name accepted by `--disable-tools`
- **AND** no tool is executed

#### Scenario: Configuration changes the default tool set

- **WHEN** a configuration-controlled built-in capability such as web search,
  LSP, or the browser bridge is disabled
- **THEN** its unregistered tool names are absent from `--list-tools`

### Requirement: Headless Skill discovery

ACECode SHALL accept `acecode -p --list-skills` without a prompt and print the
exact names of installed, platform-compatible Skills that are available under
the current workspace and global configuration. Globally disabled Skills SHALL
not be listed.

#### Scenario: List available Skills

- **WHEN** the current workspace exposes available Skills and the user runs
  `acecode -p --list-skills`
- **THEN** ACECode prints a deterministic Skill catalog containing each exact
  name accepted by `--enable-skills`
- **AND** each non-empty Skill description is shown as supporting human-readable
  context

#### Scenario: No Skills are available

- **WHEN** no Skill is available under the current workspace and configuration
- **THEN** the Skill catalog reports a zero count and an explicit `(none)` entry
- **AND** the command exits successfully

### Requirement: Headless MCP discovery

ACECode SHALL accept `acecode -p --list-mcp` without a prompt and print the exact
names of configured MCP servers whose global configuration is not disabled.

#### Scenario: List selectable MCP servers

- **WHEN** configured MCP servers `github` and `linear` are globally enabled and
  the user runs `acecode -p --list-mcp`
- **THEN** the output includes the exact names `github` and `linear` in
  deterministic order
- **AND** ACECode does not connect to either server

#### Scenario: Hide disabled MCP servers

- **WHEN** a configured MCP server has `disabled: true`
- **THEN** that server is absent from `--list-mcp` output because
  `--enable-mcp` would reject it as unavailable

#### Scenario: No MCP servers are selectable

- **WHEN** no configured MCP server is globally enabled
- **THEN** the MCP catalog reports a zero count and an explicit `(none)` entry
- **AND** the command exits successfully

### Requirement: Discovery mode is side-effect bounded

Capability discovery SHALL terminate after loading local configuration and the
requested catalogs. It SHALL NOT consume prompt input, create or resume a
session, initialize a model, dispatch startup hooks, execute a tool, start LSP,
or start MCP transports.

#### Scenario: List all catalogs

- **WHEN** the user runs
  `acecode -p --list-tools --list-skills --list-mcp`
- **THEN** ACECode prints the built-in tool catalog followed by the Skill and
  MCP catalogs
- **AND** exits successfully without requiring a prompt

#### Scenario: Redirected stdin is not consumed

- **WHEN** discovery mode is invoked while stdin is redirected or held open
- **THEN** ACECode lists the requested catalogs without waiting for or reading
  stdin

#### Scenario: Reject mixed discovery and execution

- **WHEN** a discovery flag is combined with a prompt or an execution-only
  option
- **THEN** ACECode returns a usage error instead of ignoring the execution input
  or mixing catalog and agent output

### Requirement: Help exposes the discovery workflow

The `acecode -p --help` output SHALL document all discovery flags beside the
matching selector options and SHALL show how discovered names are supplied to a
normal headless invocation.

#### Scenario: User reads print-mode help

- **WHEN** the user runs `acecode -p --help`
- **THEN** the output includes `--list-tools`, `--list-skills`, and `--list-mcp`
- **AND** explains that discovery requires no prompt and performs no MCP
  connections
- **AND** includes a discovery-to-selection example
