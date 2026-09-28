<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The saved model reasoning structure already holds support, effort choices, defaults, and budget settings. Model discovery currently drops reasoning metadata, composer normalization drops saved reasoning, and ordinary OpenAI-compatible requests omit effort. Session model bindings already support provider replacement and profile revision reloads, but no session effort override exists.

## Goals / Non-Goals

**Goals:** Preserve the existing composer geometry, require explicit capabilities, keep effort scoped to a session, and make selected values observable in the request body.

**Non-Goals:** Changing the ACEModel server, inferring capabilities from model names, adding new provider SDKs, and inventing effort levels for budget-only models.

## Decisions

### Explicit discovery contract

An ACEModel `/models` entry optionally contains `reasoning: { supported_efforts: ["low", "medium", "high"], default_effort: "high" }`. The default is optional and must belong to the nonempty canonical effort list. Missing, null, empty, or malformed declarations provide no reasoning capability. Discovery returns a per-model reasoning map to the web client, persists it in the existing probe cache, and derives the reasoning capability tag from it. ACEModel declarations take precedence over stale catalog capability guesses; no automatic network discovery is added to normal chats.

### Explicit custom model opt-in

Models added through the existing custom OpenAI-compatible flow start without reasoning regardless of their name. Checking the existing reasoning capability initializes an editable low/medium/high list when none exists. Users can select other canonical levels and edit the default with the existing settings form. Unchecking clears controls and explicit settings. Catalog-backed non-ACEModel models continue using their declared metadata; managed Copilot/Grok restrictions stay intact. This change does not add provider catalog entries.

### Request contract

ACEModel and custom OpenAI-compatible Chat Completions requests send the selected `reasoning_effort` only for an explicitly enabled profile. Existing OpenRouter and Anthropic encodings remain provider-specific. Choosing an explicit effort clears any inherited thinking token budget in the effective session profile so budget precedence cannot silently neutralize the choice. Selecting default removes the session override and restores the saved profile's settings. Generic Responses support is outside this feature.

### Session state and mutation

Session creation accepts an optional `reasoning_effort`; session metadata persists the override independently from model defaults. Current session model state exposes effective reasoning metadata and the nullable session effort override. `POST /api/sessions/<id>/reasoning` accepts `{ "effort": "high" }` or `{ "effort": null }` and returns current session model state. Invalid values and busy sessions are rejected without mutating state. All validation and provider replacement go through the session model binding path; revision reloads retain valid overrides, and switching to a different model clears the override. Persistence, resume and fork retain the applicable session choice without changing any other session or saved model.

### Composer behavior

The new control sits between model and send buttons and displays the effective Chinese level or `默认`. It is present only when reasoning is enabled, the route supports effort, and the supported list is nonempty. Its menu offers default plus the model's list, uses the existing popup styling, supports keyboard dismissal and selection, and preserves composer focus. Existing model/permission/actions remain intact; narrow layouts retain a usable text effort control. It is disabled during streaming, submission, and pending switches. A new-task choice is passed into session creation rather than updating the global model default.

## Risks / Trade-offs

- Incomplete or invalid server metadata -> hide the control and omit reasoning parameters; surface invalid manual settings in the form.
- Concurrent profile reloads and user submissions -> validate and replace the binding under the same session control synchronization used by model switching, and reject busy mutations.
- Model or capability changes -> clear invalid overrides before the next provider construction.
- Current ACEModel deployments omit the optional declaration -> the released client keeps reasoning hidden until the service publishes the contract; test with deterministic fake model and inference endpoints.

## Migration Plan

New fields are optional. Existing sessions and profiles load unchanged, and old clients ignore new model metadata. No user configuration is mass-rewritten. Release the client with the documented contract; ACEModel may independently add declarations later.
