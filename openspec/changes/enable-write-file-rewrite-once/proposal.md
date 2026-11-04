# Proposal

## Why

The next ACECode version should enable tool rewriting with `file_write` exposed as `write_file` for users who have not enabled rewriting. This must be a one-time migration so later user choices survive restarts and upgrades.

## What Changes

- Run the migration from the existing shared startup loader before tool registration.
- Enable previously disabled or missing settings, replacing only the `file_write` mapping with `write_file` and retaining other mappings.
- Preserve already-enabled settings and record the migration as handled for those users too.
- Persist a completion marker together with settings; explicit settings saves retain completion, including when disabling rewriting.
- Preserve unreadable, invalid, or conflicting files and retry unsuccessful migration on a later startup.

## Capabilities

### New Capabilities

- `tool-rewrite-settings`: Persistent tool rewrite settings and one-time startup defaults.

### Modified Capabilities

None.

## Impact

- `src/adapters/tool/tool_rewrites.cpp` and its public settings model.
- Existing daemon, TUI and headless startup callers inherit the behavior.
- Tool rewrite unit tests and API documentation; no frontend or endpoint shape changes.
