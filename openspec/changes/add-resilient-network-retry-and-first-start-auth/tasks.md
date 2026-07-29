## 1. Shared retry policy

- [x] 1.1 Add a provider retry utility for transient HTTP and hard-quota classification, numeric/date Retry-After parsing, one-second exponential delay with a twenty-minute cap, and saturating counters
- [x] 1.2 Add an efficient condition-variable retry waiter to LlmProvider and wake the active provider from AgentLoop abort and shutdown paths
- [x] 1.3 Add focused unit tests for classification, delay growth/cap, Retry-After fallbacks, and prompt cancellation

## 2. Streaming request resilience

- [x] 2.1 Migrate OpenAI-compatible streaming retries to the shared unbounded policy, reset every provisional attempt, and re-resolve proxy routing per attempt
- [x] 2.2 Migrate Anthropic streaming retries to the same policy and replay/reset guarantees
- [x] 2.3 Emit retry-wait and retry-resume lifecycle events with unbounded structured metadata and update provider recovery tests for success, infinite continuation, and cancellation

## 3. Agent and surface integration

- [x] 3.1 Make AgentLoop clear all provisional response state for every replayable request and expose structured coalesced model_retry progress with a retry deadline
- [x] 3.2 Add TUI retry-wait/resume callbacks and Web transcript activity fields without adding retry messages to conversation history
- [x] 3.3 Add AgentLoop/Web/TUI regression coverage for reset, progress, and stop behavior

## 4. Compaction resilience

- [x] 4.1 Replace the bounded compaction retry budget and sleep loop with the shared unbounded retry policy while preserving context-overflow reduction semantics
- [x] 4.2 Surface compaction retry progress through AgentLoop and update compaction tests for recovery, counter reset, and abort instead of exhaustion

## 5. Connector first-start authentication

- [x] 5.1 Add an atomic versioned first-start claim in the shared runtime state file with thread/process concurrency and persistence-failure tests
- [x] 5.2 Gate enabled connector on_startup hooks on the new claim, own and join their worker threads, and test first/later/failure startup decisions
- [x] 5.3 Remove automatic on_enable execution from Web and TUI connector mutations while preserving legacy fields in parse/serialize tests
- [x] 5.4 Remove ConnectorAuthRecovery from AgentLoop, daemon, headless, TUI, session registry, provider accessors, and obsolete tests
- [x] 5.5 Update connector management copy and daemon/config documentation to describe first-start-only automatic authentication and inert legacy hooks

## 6. Verification

- [x] 6.1 Run OpenSpec validation and the focused provider, AgentLoop, compaction, connector, config, state-file, Web, and TUI test targets
- [x] 6.2 Build the testable C++ target and Web bundle, run the available broader test suites, and document any environment-limited validation
- [x] 6.3 Audit the final diff for unrelated changes, stale auth-recovery references, duplicate retry constants, and unchecked OpenSpec tasks
