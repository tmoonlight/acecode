## Context

See proposal.md. OpenAiCompatProvider owns Chat Completions normalization, attachment routing, request headers and cache hints. Grok already has a dedicated Responses codec and transport. Session content_parts and agent Done events can preserve opaque provider blocks. Factory fingerprints determine whether a provider snapshot may be reused.

## Goals / Non-Goals

Goals: a selectable API-key Responses provider, complete local tool loop, deterministic fixture coverage, unchanged legacy defaults.

Non-goals: automatic model-name-based protocol migration, hosted OpenAI tools, asynchronous tool execution, native compaction and configuration_update. Existing local compaction continues through the selected provider's normal inference API.

## Decisions

1. Add optional ModelProfile.api_protocol with a missing-value legacy default. Carry it through draft/API/UI/equality and provider fingerprinting. Use a distinct OpenAiResponsesProvider implementation while retaining provider kind openai. This avoids guessing from URLs or model strings and permits compatible gateways.
2. Split a pure openai_responses codec from transport. Reuse established Chat request normalization for role/tool repair and attachment policy, then convert to Responses input/tools with max_output_tokens and nested reasoning. Validate canonical histories when projecting provider-native items. Explicit strict=false preserves existing tool schema semantics.
3. Use store=false and reasoning.encrypted_content, without previous_response_id. Persist ordered output under content_parts entries with type openai_responses_item and item containing the raw output object. Canonical content and tool calls remain the application-owned truth; replay whole native groups only if they still match. Other providers ignore these opaque parts.
4. SSE parser accepts normal text/reasoning/refusal/function/usage events. Function execution is enabled only after a successful terminal envelope. Validate known events; ignore unknown events. Final output items replace preliminary items so encrypted state is complete. Preserve message phase and avoid double emission from output_item.done and response.completed.
5. Transport uses existing retry policy and idle-progress cancellation boundaries, fresh parser state per retry, standard retry/reset events, and non-retryable quota classification. The selected protocol applies to streaming, non-streaming, model probes and local compaction.
6. Root integrates factory, base capability seams and native history boundaries; separate workers own codec, transport and configuration/UI. No simultaneous edits to shared files. Sources are discovered by CMake's existing group ownership; validate regenerated source ownership.

## Risks / Trade-offs

Release integration: Responses uses the same body-aware retry delay cap as Chat Completions and Anthropic. Rate-limit text takes precedence over unreliable gateway status codes; context overflow must reach recovery without retrying the same oversized request, while hard quota stays terminal. Explicit Retry-After retains the shared server-delay policy.

- Opaque history can become stale after repair -> validate against canonical content and calls, and test resume/repair paths.
- Stream completion can duplicate or prematurely execute tools -> reconcile final envelope, emit only successful complete calls, and test failure after partial calls.
- Existing Chat image adaptation moves tool images into user messages -> preserve current routing and test image payloads through Responses conversion.
- Gateways differ in optional fields -> use standard Responses fields and make selection explicit; preserve old Chat defaults.
- Pre-request token estimates remain approximate and include opaque native state -> preserve the existing conservative estimator; actual Responses input/output/cache/reasoning usage remains authoritative when reported. Provider-specific tokenization is outside this protocol adapter.
- Shared build output may be occupied -> reuse the current Windows build, load MSVC environment and build with --parallel 1. Inspection found the current build/ Visual Studio Release output was updated on October 8; available Ninja directories were stale September snapshots, so validation uses build/.

## Migration Plan

No automatic configuration migration. Users select Responses in existing OpenAI model settings. Rollback selects Chat Completions on endpoints/models supporting it. Validate parser and local HTTP fixtures, session history round-trip, provider factory and profile tests, full Web tests/build, layer checks, strict OpenSpec validation and diff checks. Live paid-provider verification requires available credentials and is reported separately.
