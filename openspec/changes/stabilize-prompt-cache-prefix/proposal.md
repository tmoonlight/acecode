## Why

Normal requests currently move rebuilt session context before the newest user message, breaking shared prefixes across turns. Repeated todo injection, unrelated compaction request layouts, default-model subagents, and missing session cache keys further reduce reuse.

## What Changes

- Persist a hidden context snapshot per compaction window; preserve its initial position and append subsequent context updates without rewriting old messages. Freeze the skill index for the window.
- Stop reinjecting todos on every request; preserve tool results and restore the current checklist once after compaction.
- Reuse the normal request prefix and tool definitions for local compaction when supported, retaining summary validation and the existing tool-free fallback.
- Default unspecified subagent models to the current parent's saved model while preserving explicit overrides.
- Use stable session cache keys only for verified supporting endpoints; downgrade once on an explicit unsupported-field rejection.

## Capabilities

### New Capabilities
- `prompt-cache-stability`: Persisted request-prefix stability, context updates, subagent model inheritance, and compatible session cache routing.

### Modified Capabilities
- `context-compaction`: Reuse the normal model-facing prefix for summarization with safe tool-free fallback and fresh window context after success.

## Impact

Agent request assembly, session history reconstruction, local compaction, OpenAI-compatible provider transport, subagent spawning, and C++ regression tests. This supersedes the per-request moving-context requirement in `replicate-current-codex-compaction/design.md` section 10. Existing Web edits remain outside scope. No paid model experiment, release, or remote publication is required; measured cache-rate gains are not claimed without usage evidence.
