# Change: Add PermissionResolved hook and a Herdr hook example

## Why

`PermissionRequest` runs before interactive approval, but integrations have no
lifecycle signal when that wait ends. Generic status, audit, and telemetry hooks
therefore cannot distinguish waiting for a user from executing the approved
operation.

## What Changes

- Add a generic `PermissionResolved` command-hook event paired with
  `PermissionRequest`.
- Include decision and resolution-source fields in its stdin JSON payload.
- Document the event as observational; it cannot alter a completed permission
  decision.
- Provide an optional cross-platform Herdr `hooks.json` example that maps
  existing lifecycle events to Herdr custom-agent states.

## What Does Not Change

- AceCode does not detect Herdr environment variables or invoke Herdr itself.
- Herdr is not modified.
- No Herdr installer, reporter class, provider, or runtime branch is added.
- Users who do not install the example JSON have no Herdr-specific behavior.
