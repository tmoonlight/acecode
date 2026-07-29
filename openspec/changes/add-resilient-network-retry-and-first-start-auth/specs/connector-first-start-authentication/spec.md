## ADDED Requirements

### Requirement: Sole automatic authentication trigger
The system SHALL use an enabled connector's `hooks.on_startup` as its only automatic authentication hook. It MUST NOT automatically execute connector authentication because a connector is enabled or because a model request returns HTTP 400 or 401.

#### Scenario: Connector is enabled after startup
- **WHEN** a user changes a connector from disabled to enabled
- **THEN** ACECode persists the new state without executing `hooks.on_enable`

#### Scenario: Model authentication fails
- **WHEN** an active model request returns HTTP 400 or 401
- **THEN** ACECode reports the provider error without executing `hooks.on_auth_error`, changing the provider key, or retrying through connector recovery

### Requirement: Durable first-start gate
Before launching automatic connector authentication, the daemon MUST atomically persist a versioned claim in ACECode runtime state. It SHALL launch enabled `hooks.on_startup` connectors only when that claim is newly created and SHALL skip all automatic connector authentication on every later daemon startup.

#### Scenario: First daemon startup with installed connectors
- **WHEN** the daemon starts and no first-start authentication claim exists
- **THEN** it persists the claim and launches each enabled connector's configured `hooks.on_startup` at most once

#### Scenario: Later daemon startup
- **WHEN** the daemon starts after the first-start authentication claim already exists
- **THEN** it launches no connector authentication hooks

#### Scenario: First-start claim cannot be persisted
- **WHEN** runtime state is read-only or the claim write otherwise fails
- **THEN** the daemon logs the failure and launches no automatic authentication helper

### Requirement: Crash-safe at-most-once preference
The first-start claim MUST be durable before any external authentication process is launched. A hook failure or process crash after the claim SHALL NOT make automatic authentication eligible on a later startup.

#### Scenario: Authentication helper fails
- **WHEN** a first-start connector helper cannot start, times out, or exits unsuccessfully
- **THEN** ACECode records diagnostics but does not clear the durable claim or automatically try the helper on the next daemon launch

### Requirement: Owned asynchronous hook lifetime
First-start connector hooks SHALL run without blocking daemon startup, and their worker threads SHALL remain owned until completion or daemon shutdown rather than accessing daemon state from detached threads.

#### Scenario: Daemon shuts down while authentication is running
- **WHEN** daemon shutdown begins while a first-start connector hook is still active
- **THEN** shutdown joins the owned hook worker before destroying state referenced by its completion callback

### Requirement: Legacy connector configuration compatibility
The connector parser and serializer SHALL continue accepting and preserving `hooks.on_enable`, `hooks.on_auth_error`, and `auth_error_scope` during migration, but runtime and settings behavior MUST treat them as inert legacy metadata. Connector status UI SHALL describe `on_startup` as first-start-only and SHALL not advertise automatic enable or authentication-error recovery.

#### Scenario: Legacy connector JSON is loaded and saved
- **WHEN** configuration contains legacy automatic-authentication fields and an unrelated setting is saved
- **THEN** the fields remain structurally preserved but no legacy hook is executed
