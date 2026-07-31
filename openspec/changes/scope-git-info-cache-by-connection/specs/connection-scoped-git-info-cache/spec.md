## ADDED Requirements

### Requirement: Hidden Git session pills do not load Git information
The Web UI SHALL decide whether a Git session pill can render before initiating its Git-information lookup, and a bar pill for an already-started or worktree-backed session MUST NOT issue `/api/git/info`.

#### Scenario: Switching to an existing session
- **WHEN** the chat switches to a session that already contains messages
- **THEN** its bar Git session pill does not initiate a Git-information request

#### Scenario: Transcript state is not loaded yet
- **WHEN** a bar Git session pill is mounted while the target session transcript is still loading
- **THEN** it does not initiate a Git-information request until the transcript confirms that the session is empty

#### Scenario: Opening a new-session surface
- **WHEN** a hero pill or an unstarted bar pill has a non-empty working directory
- **THEN** it may load Git information needed to render its controls

### Requirement: Git information is shared within one effective connection
The Web UI SHALL cache Git information by effective daemon connection and working directory for 30 seconds, and SHALL deduplicate concurrent loads for the same pair even when callers hold independently created API client objects.

#### Scenario: Concurrent consumers share one request
- **WHEN** two consumers with the same effective origin and authentication token request the same `cwd` before the first request settles
- **THEN** exactly one `/api/git/info` request is sent and both consumers receive its result

#### Scenario: Fresh cached value is reused
- **WHEN** the same effective connection requests the same `cwd` again before the TTL expires
- **THEN** the cached result is returned without another network request

### Requirement: Git information is isolated between effective connections
Cached values and in-flight Git-information promises MUST NOT be shared between clients whose effective origins or authentication tokens differ, even when their `cwd` strings are identical. Every cache miss MUST be sent through a client in the requested connection context.

#### Scenario: Same directory on different daemons
- **WHEN** clients for two different origins request the same `cwd`
- **THEN** each origin receives its own `/api/git/info` request and result

#### Scenario: Same daemon origin with different credentials
- **WHEN** clients with different authentication tokens request the same `cwd` on one origin
- **THEN** each authentication context receives its own `/api/git/info` request and result

#### Scenario: Global base changes after an earlier load
- **WHEN** the mutable global API base changes and the same `cwd` is requested again
- **THEN** the new connection does not receive a cached value or reload routed through the previous connection

### Requirement: Refresh and Git-state invalidation preserve connection routing
An explicit refresh SHALL invalidate the caller's current connection/directory entry before reloading it. A Git-state event without connection metadata SHALL invalidate the matching directory across known connection scopes, and any consumer reload MUST use that consumer's own API context.

#### Scenario: User explicitly opens the branch list
- **WHEN** the user triggers a forced refresh for a visible Git session pill
- **THEN** its scoped cached value is discarded and a new request is sent through that pill's API client

#### Scenario: Git state changes without connection metadata
- **WHEN** a Git-state event identifies a `cwd` but not a daemon connection
- **THEN** cached values for that directory are invalidated across connection scopes without routing a reload through a global client
