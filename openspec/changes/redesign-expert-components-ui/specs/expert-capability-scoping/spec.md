## ADDED Requirements

### Requirement: Advanced editor lists real ACECode capabilities
The advanced tab SHALL load selectable Skills, configured MCP servers, and registered ACECode built-in tools from runtime-backed APIs. It MUST NOT use a hard-coded showcase list. Each option SHALL expose a stable identifier, a human-readable label or description, and an enabled, disabled, disconnected, missing, or unavailable state as applicable.

#### Scenario: Open advanced configuration
- **WHEN** the advanced tab finishes loading
- **THEN** its Skill, MCP, and local-tool options correspond to capabilities known by the current ACECode daemon

#### Scenario: A selected capability is no longer installed
- **WHEN** a saved expert references a Skill, MCP server, or tool that is no longer available
- **THEN** the editor retains the reference, marks it unavailable, and allows the user to remove it

### Requirement: Capability scopes have backward-compatible persistence semantics
Expert manifests and API DTOs SHALL persist three independently optional scopes: selected Skill names, selected MCP server identifiers, and selected built-in tool names. An absent scope SHALL mean “inherit every capability currently allowed globally” for existing manifests; a present empty list SHALL mean “allow none” for that capability class; a present non-empty list SHALL mean “allow only the listed identifiers.”

#### Scenario: Load a legacy expert
- **WHEN** an existing expert manifest has none of the new scope fields
- **THEN** its runtime behavior remains compatible by inheriting globally allowed capabilities

#### Scenario: Save all tools switched off
- **WHEN** a user explicitly disables every local tool and saves
- **THEN** the manifest preserves a present empty local-tool scope rather than treating it as inheritance

#### Scenario: Round-trip unavailable references
- **WHEN** an expert contains a selected capability identifier that is temporarily unavailable
- **THEN** reading and saving unrelated fields does not silently discard that identifier

### Requirement: Explicit expert scopes take precedence over global enablement
When an expert scope is explicitly present, its selected known Skill names, configured MCP server identifiers, and registered local-tool identifiers SHALL take precedence over ACECode’s global enable/disable state. An absent expert scope SHALL continue to inherit global defaults. Expert precedence MUST NOT install an absent Skill, invent MCP configuration or credentials, register an absent tool, elevate permissions, or bypass approval, plan/dangerous-mode, path, sandbox, or other security restrictions.

#### Scenario: Expert selects a globally disabled capability
- **WHEN** an expert allowlist names a Skill or MCP server disabled in global configuration
- **THEN** ACECode makes that known capability available to the expert context while keeping it hidden from sessions that inherit the global disabled state

#### Scenario: Expert selects a write tool in restrictive permission mode
- **WHEN** the expert allows a write tool but the active permission policy requires approval or forbids it
- **THEN** the existing permission policy still governs or rejects the call

### Requirement: Tool filtering is enforced for schemas and execution
For an expert-bound turn, the system SHALL omit disallowed built-in and MCP tools from provider-facing tool definitions and system-prompt tool descriptions. It SHALL also reject a disallowed tool name at the execution boundary so a model-produced or replayed call cannot bypass schema filtering.

#### Scenario: Provider request is built
- **WHEN** an expert allows `file_read` but not `file_write`
- **THEN** the provider receives the allowed tool schema and does not receive the disallowed tool schema

#### Scenario: Model emits a disallowed tool call
- **WHEN** a model nevertheless requests a tool outside the effective expert scope
- **THEN** ACECode returns a deterministic denied result and does not execute that tool

### Requirement: MCP selection operates at server identity
The expert editor SHALL select MCP capabilities by configured server identifier. At runtime, selecting a configured globally disabled server SHALL be allowed to start/connect it for the expert, and the expert policy SHALL allow only that server’s currently registered MCP tools. It MUST NOT accidentally expose those tools to global-inheriting sessions or allow tools from another server with a similar display name or tool name.

#### Scenario: Two MCP servers expose the same unqualified tool name
- **WHEN** an expert selects only one of those server identifiers
- **THEN** only the qualified tools owned by the selected server are visible and executable

#### Scenario: Selected MCP server is disconnected
- **WHEN** the configured selected server is not connected
- **THEN** the expert remains loadable, the editor shows its unavailable state, and no tools from that server are advertised

### Requirement: Skill selection limits expert-visible Skills
When a Skill scope is present, expert session initialization SHALL retain installed Skills whose stable names appear in that scope even when those Skills are globally disabled or outside the global allowlist, while still honoring valid Skill content bundled inside that expert package. Skill discovery, prompt injection, slash invocation, and Skill-reading tools SHALL not expose unselected global Skills.

#### Scenario: Expert selects a subset of global Skills
- **WHEN** an expert selects two of five globally enabled Skills
- **THEN** only those two global Skills, plus valid Skills bundled with the expert itself, are available in that expert context

#### Scenario: Selected Skill is globally disabled
- **WHEN** a selected Skill is disabled globally
- **THEN** it remains present in that expert’s Skill discovery and prompt content while globally inheriting sessions continue to omit it

### Requirement: Local-tool choices use checkbox defaults
The advanced editor SHALL render each ACECode local-tool choice as a checkbox rather than a Toggle. When a capability class is inheriting global defaults, globally enabled and registered tools SHALL appear checked and disabled/unavailable tools SHALL appear unchecked. Switching to explicit configuration SHALL begin from those visible defaults.

#### Scenario: Open a new expert with inherited tools
- **WHEN** the capability catalog reports `file_write` enabled and another tool disabled
- **THEN** `file_write` is visibly checked and the disabled tool is visibly unchecked

#### Scenario: Customize inherited local tools
- **WHEN** the user turns off inheritance for local tools
- **THEN** the explicit selection starts with the previously displayed enabled tools checked

### Requirement: In-place expert switches atomically update all expert scopes
Switching an active conversation to another expert SHALL update expert prompt context, Skill registry, MCP allowlist, and built-in-tool allowlist together in the same queued control operation. A later turn MUST NOT observe a mixture of the old and new experts’ capability scopes.

#### Scenario: Switch between experts with different scopes
- **WHEN** a queued switch changes from an expert with write tools to an expert with read-only tools
- **THEN** the next turn receives the new prompt and all three new capability scopes atomically

### Requirement: Teams preserve member-specific capability scopes
An expert team SHALL reference existing expert definitions rather than copying or merging their advanced settings. The lead and spawned member contexts SHALL use each selected member expert’s own capability scopes, still intersected with global policy.

#### Scenario: Team members have different allowed tools
- **WHEN** a team lead delegates to a member whose expert allows a different tool set
- **THEN** the delegated member session uses that member’s scope and does not inherit a broader union from the team
