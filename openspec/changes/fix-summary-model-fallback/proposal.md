# Proposal

## Why

A missing or failing summary model currently prevents background generation even when conversation models work. The user wants automatic, silent recovery using the most recently successful conversation model.

## What Changes

- Persist the identity of the last model that actually completed a conversation request successfully, across surfaces and restarts.
- Share a bounded, deduplicated fallback chain for automatic titles, home intent generation, and memory summaries. Prefer the configured model, then the last successful conversation model, then existing contextual/default/saved candidates.
- Reuse a successful replacement for subsequent summary requests until the preferred configuration changes or that replacement fails.
- Allow automatic model selection without configuring a dedicated summary model; remove missing-model prompts. Do not announce fallback or alter the conversation model.
- Supersede the no-fallback decisions in `add-summary-generation-model` and `add-home-intent-prompts`.

## Capabilities

### New Capabilities

- `summary-model-fallback`: successful conversation model tracking and silent recovery for background generation.

### Modified Capabilities

None of the affected in-flight capabilities have a main spec yet.

## Impact

Provider construction and successful conversation completion, shared background model execution, daemon/TUI titles, home intent generation, memory scheduler, summary settings validation/UI, and regression tests. No new credentials or conversation content are persisted in the fallback state.
