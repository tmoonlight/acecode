## ADDED Requirements

### Requirement: Channel status may identify a binding instance
Channel protocol version 1 SHALL allow `channel.status` to contain an optional top-level `binding_token` that ACECode treats as an opaque binding-instance identity.

#### Scenario: Token-aware plugin activates
- **WHEN** a successful activation status contains a non-empty string `binding_token`
- **THEN** ACECode accepts the status and associates that exact token with the activated session binding instance

#### Scenario: Legacy plugin activates
- **WHEN** a successful activation status omits `binding_token`
- **THEN** ACECode accepts the status and records the binding as legacy and unscoped

#### Scenario: Status contains unrelated fields
- **WHEN** a valid status contains fields ACECode does not recognize
- **THEN** ACECode ignores those fields and preserves normal status parsing

#### Scenario: Token has an invalid type or value
- **WHEN** `binding_token` is present but is not a string or is an empty string
- **THEN** ACECode rejects the status with an error that identifies `binding_token` as invalid

### Requirement: Deactivation targets the stored binding identity
ACECode SHALL serialize `channel.deactivate` from one atomic current-binding snapshot and SHALL include `binding_token` exactly when that binding has a validated token.

#### Scenario: Token-aware binding is closed
- **WHEN** the current binding has token `B` and explicit close deactivates it
- **THEN** the request contains the binding's `session_id` and `binding_token: "B"`

#### Scenario: Legacy binding is closed
- **WHEN** the current binding was activated without a token
- **THEN** the request contains only the Channel v1 type, protocol version, and `session_id` fields

#### Scenario: Keepalive rotates binding identity
- **WHEN** a current binding with token `A` is successfully reactivated and the matching generation returns token `B`
- **THEN** ACECode atomically makes `B` current and any later explicit close uses `B`

### Requirement: Replacement cleanup cannot detach a newer same-session binding
ACECode MUST NOT issue an unscoped stale deactivation that can target a newer binding of the same session.

#### Scenario: Same session is activated with token A then token B
- **WHEN** the second activation becomes current
- **THEN** any cleanup of the first instance carries token `A`, while explicit close of the current instance carries token `B`

#### Scenario: Same legacy session is activated again
- **WHEN** both the old and new binding omit a token
- **THEN** ACECode does not send a session-only stale cleanup after the new activation becomes current

#### Scenario: Old and new bindings use different sessions
- **WHEN** a legacy binding is replaced by a binding for a different session
- **THEN** ACECode may clean up the old binding with its old `session_id` and the legacy request shape

### Requirement: Binding lifecycle operations are generation-safe
ACECode SHALL serialize plugin activation, replacement, explicit close, keepalive, and shutdown teardown so a request cannot combine one generation's session with another generation's token.

#### Scenario: Close races with replacement activation
- **WHEN** explicit close begins while a replacement activation is in progress
- **THEN** close waits for the replacement lifecycle operation and deactivates the binding that is current afterward using that binding's token

#### Scenario: Shutdown races with activation
- **WHEN** shutdown begins while activation is in progress
- **THEN** shutdown waits for the lifecycle operation, tears down any binding it completed, rejects later activation attempts, and performs no plugin deactivation because the persisted binding is intentionally rebuildable

#### Scenario: Stale keepalive completes
- **WHEN** keepalive activation completes for a generation that is no longer current
- **THEN** its outbound URL and binding token are not installed into the current binding
