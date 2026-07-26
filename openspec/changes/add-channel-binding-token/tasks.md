## 1. Protocol Contract

- [x] 1.1 Extend status parsing with optional validated `binding_token` while retaining unknown-field and absent-field compatibility
- [x] 1.2 Extend deactivation serialization and `ChannelPluginHost` to echo a stored token or preserve the legacy request shape
- [x] 1.3 Add parser, serializer, activation, and deactivation host tests

## 2. Binding Lifecycle

- [x] 2.1 Store the token in the TUI and daemon current binding instances
- [x] 2.2 Update daemon replace, keepalive, off, and shutdown serialization so session/generation/token snapshots cannot be mismatched
- [x] 2.3 Preserve compatibility by suppressing unscoped stale cleanup for same-session legacy replacements

## 3. Lifecycle Coverage

- [x] 3.1 Cover token-aware and legacy binder activation/deactivation round trips
- [x] 3.2 Cover same-session token A/B replacement and stale-deactivate protocol shape
- [x] 3.3 Cover keepalive token rotation and concurrent replace/off token ownership

## 4. Documentation and Verification

- [x] 4.1 Update the durable Channel v1 protocol documentation
- [x] 4.2 Run focused Channel tests, the feasible full `acecode_unit_tests` suite, OpenSpec validation, and diff hygiene checks
- [x] 4.3 Audit for provider-specific strings, complete task checkboxes, and commit the scoped change
