## Purpose

Give built-in experts a compact recognizable pixel identity shared with the existing virtual office, so users can recognize the same expert across the catalog, team execution, and its live subagent without downloading separate portraits.

## ADDED Requirements

### Requirement: Shared pixel expert identity
Built-in experts SHALL use portraits derived from the existing office character templates. An expert and a subagent bound to that expert SHALL have the same stable appearance in the catalog, subagent presentation, and virtual office. Identity MUST use structured expert and member bindings rather than display-name matching or session order.

#### Scenario: Team delegates to an expert
- **WHEN** a team spawns a subagent for a declared expert member
- **THEN** its portrait and office character match that member's catalog identity even when the session ID changes

#### Scenario: Expert is the session leader
- **WHEN** an expert-bound root session is shown in the office
- **THEN** its character uses the expert identity rather than the generic conductor appearance

#### Scenario: Custom expert portrait
- **WHEN** a user has configured a custom portrait or state portrait
- **THEN** the configured image is preserved and remains available

### Requirement: Compact built-in expert resources
Release seed resources SHALL omit the replaced built-in photographic PNG portraits. Managed seed upgrades MUST retain user modifications and update only managed defaults under the existing seed reconciliation contract.

#### Scenario: Fresh installation
- **WHEN** built-in experts are seeded in an empty profile
- **THEN** all experts have pixel identities without requiring the old portrait files

#### Scenario: Existing modified expert
- **WHEN** seed reconciliation encounters a user-modified expert package
- **THEN** it preserves that package and its custom image resources
