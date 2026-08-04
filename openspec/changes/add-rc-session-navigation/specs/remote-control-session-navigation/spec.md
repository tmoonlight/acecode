## ADDED Requirements

### Requirement: RC session command aliases

An active remote-control binding SHALL recognize `/session`, `/sessions`, and `/resume` as case-insensitive aliases and SHALL consume those commands in the control plane instead of submitting them to the bound agent conversation.

#### Scenario: Bare alias lists recent sessions

- **WHEN** an RC user sends any bare alias
- **THEN** the system returns the ten most recently updated ordinary, unarchived sessions across persisted workspaces and no-workspace conversations
- **AND** each returned session has a one-based number

#### Scenario: All aliases share behavior

- **WHEN** equivalent arguments are sent with `/session`, `/sessions`, or `/resume`
- **THEN** parsing, results, numbering, errors, and selection behavior are identical

### Requirement: Complete and searched session lists

The RC session command SHALL support complete listing and bounded search.

#### Scenario: Request all sessions

- **WHEN** the user sends an alias followed by `more` or `all`
- **THEN** every resumable ordinary, unarchived user session is listed with continuous one-based numbering

#### Scenario: Search sessions

- **WHEN** the user sends an alias followed by `search <query>`
- **THEN** at most five sessions matching metadata or indexed visible user-message content are returned
- **AND** the result order is deterministic

#### Scenario: Empty search query

- **WHEN** the user sends an alias followed only by `search`
- **THEN** compact usage guidance is returned
- **AND** the agent conversation and current binding are unchanged

### Requirement: Stable numbered selection

Numeric selection SHALL resolve against the most recently displayed result snapshot shared by all aliases.

#### Scenario: Select a displayed session

- **GIVEN** a list or search displayed a session as number 3
- **WHEN** the user sends any alias followed by `3`
- **THEN** that exact displayed session is resumed if needed and becomes the sole remote-control binding

#### Scenario: Select before listing

- **WHEN** a positive number is sent before any list snapshot exists in the current daemon lifetime
- **THEN** the system first builds the default newest-ten snapshot and resolves the number against it

#### Scenario: Invalid number

- **WHEN** the number is zero, malformed, or outside the current snapshot
- **THEN** the system returns a clear error
- **AND** the current binding remains unchanged

### Requirement: Cross-workspace resume and switching

Selection SHALL use persisted target metadata to resume inactive workspace and no-workspace sessions before replacing the current binding.

#### Scenario: Inactive workspace target

- **GIVEN** the selected session is persisted under a different workspace and is not active
- **WHEN** selection runs
- **THEN** it is resumed with its own cwd and workspace hash
- **AND** binding replacement occurs only after resume succeeds

#### Scenario: Replacement fails

- **WHEN** target resume or channel activation fails
- **THEN** the user receives an error
- **AND** the previous usable binding remains authoritative whenever replacement has not committed

### Requirement: Optional frontend follow navigation

A successful numeric selection SHALL broadcast a secret-free generic navigation hint to connected Web/Desktop clients.

#### Scenario: Frontend is open

- **WHEN** a numeric RC selection succeeds while a frontend is connected
- **THEN** the frontend opens the selected conversation
- **AND** the target sidebar row visibly runs the existing remote-control lightning surge
- **AND** the persistent bound background moves to the target row

#### Scenario: Frontend is closed

- **WHEN** no frontend is connected
- **THEN** selection and remote-control message routing still succeed

### Requirement: Selection is lifecycle safe

RC session commands SHALL preserve binder generation filtering, inbound callback lifetime, and shutdown safety.

#### Scenario: Selection originates in the old inbound callback

- **WHEN** a numeric command is accepted through the currently bound route
- **THEN** replacement does not wait on its own still-held binding-context lease
- **AND** stale callbacks cannot access a destroyed binder or forward into the replacement session

#### Scenario: Shutdown races a command

- **WHEN** daemon shutdown overlaps listing or selection
- **THEN** owned work is cancelled or joined deterministically
- **AND** no callback accesses destroyed binder, session, hub, or WebServer state

#### Scenario: Catalog or search is slow

- **WHEN** a session command requires an all-project scan or message-index refresh that stalls
- **THEN** the RC HTTP inbound callback returns promptly after queueing the control operation
- **AND** the existing immediate acknowledgement is not delayed by catalog or search work
