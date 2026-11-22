## Purpose

Attach the session routing and client identification required by a selected model provider consistently across inference transports.

## ADDED Requirements

### Requirement: Supply OpenCode Go inference headers

ACECode SHALL automatically identify itself and send a stable per-conversation x-opencode-session on Go inference requests, across the supported HTTP protocols. The runtime SHALL recognize catalog-selected Go profiles and official Go endpoints in older profiles. Explicit custom headers SHALL take precedence case-insensitively.

#### Scenario: Catalog selection and session isolation
- **WHEN** two conversations use the same catalog-selected Go model provider
- **THEN** repeated turns, tool calls and retries within one conversation carry the same opaque routing ID and the other conversation carries a different ID

#### Scenario: Standalone connection test
- **WHEN** the model editor tests a Go profile without an active conversation
- **THEN** it sends a fresh routing ID and client identifier using the same policy as normal inference

#### Scenario: Legacy endpoint and other providers
- **WHEN** a profile omits the catalog provider ID
- **THEN** official Go inference URLs receive Go headers and a model with the same name on an unrelated endpoint receives no automatic Go headers
