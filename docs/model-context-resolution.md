# Model Context Resolution Memo

## Background

acecode previously treated `context_window` as a mostly static value and used a fallback of `128000`.
That was incorrect for some Copilot models.

Observed example:

- `gpt-5.4` under GitHub Copilot was shown as `128k` in acecode
- Hermes Agent resolved it to `400k`

The practical impact was not limited to display. The wrong value also affected runtime behavior:

- token status bar capacity display
- `/config` output
- auto-compact trigger threshold in `AgentLoop`

## Root Cause

GitHub Copilot's `/models` metadata is not a reliable source for the full model context window.
For some models it exposes provider-side input caps rather than the effective model context used for routing decisions.

That means a naive `GET /models -> parse context -> trust it` strategy can under-report context length.

## Reference From Hermes Agent

Hermes Agent does not blindly trust provider `/models` metadata for known providers.

Its approach is provider-aware:

1. Prefer a curated provider/model metadata source
2. Use endpoint metadata only as a fallback
3. Keep a final hardcoded fallback only for unknown cases

For Copilot, this avoids the common under-reporting problem.

## acecode Solution

acecode now uses a provider-aware runtime resolver implemented in:

- `src/provider/model_context_resolver.cpp`

Current resolution strategy:

1. If provider is `copilot`, prefer `models.dev` provider entry `github-copilot`
2. If provider is official `openai`, prefer `models.dev` provider entry `openai`
3. For generic OpenAI-compatible endpoints, try the endpoint `/models` metadata as fallback
4. If no trustworthy metadata is found, keep the configured fallback value

## Why models.dev Is Preferred For Copilot

For this specific problem, `models.dev` is used as the authoritative provider-aware source because it exposes the model context expected for the provider variant instead of just the raw endpoint limit.

Example:

- `github-copilot / gpt-5.4` resolves to `400000`

This is the value acecode should use for UX and runtime threshold calculations.

## Runtime Integration Points

The resolved value is applied at runtime in two places:

1. App startup
2. `/model <name>` switching

Whenever the model changes, acecode now refreshes:

- `config.context_window`
- `AgentLoop` context limit
- token status display

## Important Rule

For known providers, do not assume `/models` returns the real context window.

Prefer provider-aware metadata first.

## Future Follow-ups

- Cache `models.dev` results locally to avoid repeated startup fetches
- Show resolved context length directly in interactive model selection
- Expand provider mappings if more providers show endpoint metadata drift

## Related Files

- `src/provider/model_context_resolver.cpp`
- `src/provider/model_context_resolver.hpp`
- `src/commands/builtin_commands.cpp`
- `main.cpp`