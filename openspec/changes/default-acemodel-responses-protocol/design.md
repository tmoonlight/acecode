## Context

See proposal.md. The existing ModelProfile.api_protocol field, Responses Provider, API protocol selector, mutation validation and fingerprinting already support explicit selection. Provider factory currently defaults every missing protocol to Chat Completions. ACEModel's catalog descriptor does not supply a default; the Windows seeder also omits it.

## Goals / Non-Goals

Goals: share protocol resolution across configuration, runtime and API defaults; retain explicit selections; use the existing accessible selector and settings styles.

Non-goals: infer identity from model names or URLs, modify credentials or capability metadata, implement another Responses transport, publish or restart the installed Desktop application.

## Decisions

1. Add a shared effective protocol helper in saved_models: an explicit value wins; an OpenAI profile with a case-insensitive ACEModel catalog identity defaults to Responses; other profiles default to Chat Completions. Using the capability-normalization predicate would incorrectly exclude manual capabilities and additional ACEModel model IDs.
2. Materialize missing ACEModel defaults when parsing saved configurations and report the effective default from model-list serialization. Keep the model editor's existing omitted/null semantics: null removes an override, then effective resolution uses the provider default. Factory uses the helper for ordinary calls, compaction and connection tests; protocol fingerprints already encode the effective request option.
3. Add optional default_api_protocol to the ACEModel catalog descriptor. Frontend normalization validates it; provider selection initializes the draft from that field and retains existing reset behavior for other providers. Every model in a batch inherits the selected protocol. Existing saved entries come from the daemon with the effective protocol.
4. Seeder writes Responses for new presets and only fills absent/empty protocol values on upgrade. Existing explicit Chat Completions remains a valid fallback.

## Risks / Trade-offs

- Missing-field ACEModel configurations change protocol at upgrade -> preserve explicit choices and expose Chat Completions in the existing selector.
- Default and display diverge for profiles created through TUI or direct API -> serialize effective ACEModel defaults and test in-memory profiles, parser and factory paths.
- A prior factory test assumes ACEModel always uses Chat Completions -> make that test explicitly select Chat Completions and add Responses factory coverage.

## Migration Plan

No direct edits to user configuration files. Load normalizes only missing ACEModel protocol values; subsequent ordinary saves persist them. Users can select Chat Completions to revert a profile. Validate targeted C++ HTTP/configuration/API tests, seeder regression, frontend tests/build, strict OpenSpec and diff checks. The current build/ Visual Studio Release directory is reusable; compile with --parallel 1.
