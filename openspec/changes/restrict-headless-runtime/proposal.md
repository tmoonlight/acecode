## Why

A service invoking print mode needs an explicit capability ceiling and a fully owned role description.

## What Changes

Add a registration-time exact allowlist and a file-backed replacement for generated prompt context.

## Capabilities

### New Capabilities

- `headless-runtime-restrictions` — Add a registration-time exact allowlist and a file-backed replacement for generated prompt context.

### Modified Capabilities

## Impact

Filter new registrations against canonical builtin identities or the MCP owner and original tool name. Apply prompt override before generation and when assembling history for every request. Existing invocations retain their defaults.
