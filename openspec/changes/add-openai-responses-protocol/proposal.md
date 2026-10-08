## Why

ACECode's API-key OpenAI provider only encodes Chat Completions, while current OpenAI models require Responses for reasoning with agent tools. Changing the endpoint URL alone cannot translate requests, stream events, or replay opaque reasoning state.

## What Changes

- Add explicit OpenAI model profile protocol selection, preserving Chat Completions for existing profiles.
- Implement Responses request conversion, cancellable HTTP and SSE transport, typed failures and retry isolation.
- Support text, image input, reasoning summaries, function tools, tool results, usage, refusals and incomplete responses.
- Persist and replay ordered provider-owned output items, including encrypted reasoning and message phase, across tool turns and resumed sessions.
- Expose protocol selection in the existing model editor and document the setting.

## Capabilities

### New Capabilities
- `openai-responses`: Stateless Responses inference and agent tool continuation with explicit protocol selection.

### Modified Capabilities
- `web-model-management`: OpenAI profiles expose and persist protocol selection.

## Impact

Model profile configuration, saved model editor/API, provider factory, new Responses adapter and parser, model history preservation, Web model settings and focused tests. No new external dependency. Existing Chat Completions, Anthropic, Copilot and Grok configurations retain their current transport. Native compaction, hosted tools, async tool execution and mid-turn steering are separate capabilities.
