## Context

A service invoking print mode needs an explicit capability ceiling and a fully owned role description.

## Goals

Add a registration-time exact allowlist and a file-backed replacement for generated prompt context.

## Decisions

Filter new registrations against canonical builtin identities or the MCP owner and original tool name. Apply prompt override before generation and when assembling history for every request. Existing invocations retain their defaults.
