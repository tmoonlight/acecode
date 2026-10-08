## Purpose

Allow ACECode to run its agent tools against OpenAI Responses endpoints while preserving stateless conversation history and existing provider compatibility.

## ADDED Requirements

### Requirement: Explicit protocol selection
OpenAI model profiles SHALL accept `api_protocol` values `chat_completions` and `responses`. Omission SHALL preserve Chat Completions. Protocol selection SHALL participate in runtime provider identity and persist across configuration and API round trips. Other provider kinds SHALL reject this option.

#### Scenario: Existing profile is loaded
- **WHEN** an OpenAI profile without api_protocol is loaded
- **THEN** requests retain the existing Chat Completions behavior

#### Scenario: Responses profile is selected
- **WHEN** an OpenAI profile selects responses
- **THEN** base URLs target /responses and full URLs are used exactly, with Responses request and response shapes

### Requirement: Responses agent inference
Responses inference SHALL support system and user messages, image inputs through existing vision routing, assistant text, reasoning summaries, function definitions and function results. Requests SHALL use stateless history with storage disabled and request encrypted reasoning. Output limits and reasoning effort SHALL use Responses fields. Function schemas SHALL not silently acquire strict requirements.

#### Scenario: Model calls local tools
- **WHEN** a successful response contains parallel function calls
- **THEN** ACECode receives each call exactly once and returns each result under the original call_id in the next request

#### Scenario: Streaming response arrives
- **WHEN** text and reasoning SSE events precede a successful terminal event
- **THEN** output is displayed incrementally and usage and final output metadata are retained

### Requirement: Native output replay
Responses assistant output items SHALL retain their order and opaque provider fields, including encrypted reasoning and message phase, through persistence and subsequent requests. Replay SHALL avoid duplicate text and tool calls. Native items inconsistent with repaired or edited canonical history SHALL not resurrect removed content or calls.

#### Scenario: Resume a session after a tool turn
- **WHEN** a saved session containing native Responses output is reopened
- **THEN** its next request preserves the original reasoning items and call/result linkage

#### Scenario: History no longer matches native output
- **WHEN** canonical assistant content or tool calls have been repaired or edited
- **THEN** requests use the current canonical history without replaying stale native items

### Requirement: Reliable stream termination
Responses inference SHALL treat transport truncation, malformed known events and explicit provider failures as failures, and SHALL ignore unknown future events safely. Retry attempts SHALL isolate provisional output. Cancellation SHALL stop transport promptly. Incomplete or failed responses SHALL not execute partial tools.

#### Scenario: Stream is cut before completion
- **WHEN** EOF or DONE arrives without a terminal Responses event
- **THEN** the request reports a structured failure and no incomplete tool call is executed

#### Scenario: Transient failure is retried
- **WHEN** a retryable provider failure occurs after partial output
- **THEN** the next attempt replaces provisional text, reasoning, calls and native history

#### Scenario: Permanent failure or cancellation
- **WHEN** the provider reports exhausted quota or the user cancels
- **THEN** inference terminates without an automatic retry loop
