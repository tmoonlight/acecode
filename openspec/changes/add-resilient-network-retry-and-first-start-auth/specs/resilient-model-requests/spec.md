## ADDED Requirements

### Requirement: Unbounded transient request retry
The system SHALL retry an in-flight pure model-sampling request an unbounded number of times while its latest failure is explicitly classified as transient. The first retry interval SHALL be one second, subsequent local intervals SHALL grow exponentially, and no interval SHALL exceed twenty minutes.

#### Scenario: Laptop loses and later regains connectivity
- **WHEN** an active sampling request repeatedly fails with retryable network errors and a later attempt succeeds
- **THEN** ACECode keeps the same logical model step active through every failure and accepts the successful response without ending the task

#### Scenario: Retry interval reaches its cap
- **WHEN** transient failures continue beyond the exponential growth period
- **THEN** every later retry remains scheduled at an interval no greater than twenty minutes and the retry count is not exhausted

### Requirement: Narrow transient classification
The system MUST retry network transport failures, connection or stream-idle timeouts, incomplete SSE responses without a completed request, HTTP 408, 425, 429, 500, 502, 503, 504, and 529, and explicit provider-overload payloads. It MUST keep authentication, authorization, invalid request/configuration, context-limit, malformed-JSON, hard-quota/billing, and unclassified failures terminal.

#### Scenario: Temporary service overload
- **WHEN** a provider returns HTTP 503 or an explicit overload payload
- **THEN** ACECode enters the unbounded transient retry schedule

#### Scenario: Invalid credential
- **WHEN** a provider returns HTTP 401
- **THEN** ACECode reports a terminal provider error without launching authentication recovery or entering network retry

#### Scenario: Hard quota response
- **WHEN** a provider returns HTTP 429 with an exact hard-quota or billing-limit error code
- **THEN** ACECode treats the response as terminal rather than retrying indefinitely

### Requirement: Server retry guidance
The system SHALL accept both delta-seconds and HTTP-date `Retry-After` guidance for a retryable response. A valid future value SHALL determine that attempt's wait up to the twenty-minute cap; an invalid or past value SHALL fall back to the local exponential interval.

#### Scenario: Retry-After exceeds local cap
- **WHEN** a retryable response asks the client to wait longer than twenty minutes
- **THEN** ACECode schedules that retry for twenty minutes

#### Scenario: Malformed Retry-After
- **WHEN** a retryable response contains an invalid `Retry-After` value
- **THEN** ACECode uses the retry number's local exponential interval

### Requirement: Abort-aware efficient waiting
The system SHALL implement retry waits without short-interval polling and SHALL allow the existing stop, cancel, and shutdown paths to wake a waiting request promptly.

#### Scenario: User stops a capped retry wait
- **WHEN** the user stops a task while its provider is waiting for a twenty-minute retry interval
- **THEN** the wait wakes promptly, no new request attempt starts, and the turn ends as interrupted

### Requirement: Replay-safe request boundary
The system MUST retry only the current immutable provider request, MUST discard all provisional assistant, reasoning, usage, and tool-call state before replay, and MUST NOT re-execute an ACECode tool result. A provider-owned turn with ambiguous side effects MUST remain terminal unless the provider supplies a replay-safe contract.

#### Scenario: Network drops after partial assistant output
- **WHEN** a retryable transport failure occurs after provisional text, reasoning, or tool-call fragments have streamed
- **THEN** ACECode removes those provisional fragments before the next full request and the final visible output contains only the successful attempt

#### Scenario: Failure occurs after an ACECode tool completed
- **WHEN** a later model request fails after an earlier tool result was already persisted
- **THEN** ACECode retries only that later model request and does not invoke the completed tool again

### Requirement: Runtime routing refresh
The system SHALL re-resolve transport proxy routing before every HTTP retry attempt while preserving the same messages, tool definitions, and logical request body.

#### Scenario: VPN or system proxy changes during wait
- **WHEN** proxy routing changes after a failed request and before its next retry
- **THEN** the next HTTP attempt uses newly resolved routing without rebuilding conversation semantics

### Requirement: Observable non-transcript retry state
The system SHALL expose a coalesced `model_retry` progress state containing the retry number, delay, deadline, and unbounded retry budget, and SHALL return to a waiting state immediately before the next attempt. Retry status MUST NOT be appended to model-visible or persisted conversation history.

#### Scenario: Web or TUI observes a retry wait
- **WHEN** a transient failure schedules another request
- **THEN** the active surface can show the retry attempt and wait while the transcript remains unchanged

### Requirement: Compaction uses the resilient retry contract
Automatic and manual local compaction SHALL apply the same transient classification, unbounded retry count, capped exponential interval, cancellation, and immutable-request guarantees as streaming sampling. Context-overflow history reduction SHALL remain separate and SHALL reset the transient retry sequence.

#### Scenario: Automatic compaction crosses an outage
- **WHEN** automatic compaction receives retryable transient errors before a later successful summary response
- **THEN** it keeps the original compaction input unchanged, eventually installs the successful compacted history, and does not fail the active task because a retry budget was exhausted

#### Scenario: Context overflow follows transient failures
- **WHEN** a compaction request first sees transient failures and later receives an explicit context-window overflow
- **THEN** it removes one oldest logical history item according to the existing overflow contract and restarts transient backoff from its first interval
