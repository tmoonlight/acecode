## ADDED Requirements

### Requirement: Nonblocking context probes have a process lifetime owner
ACECode SHALL execute remote model-context warmup work through a lifecycle-owned
worker and SHALL NOT leave probe threads running after that owner shuts down.

#### Scenario: Process exits with an active probe
- **WHEN** process shutdown begins while an endpoint context probe is active or queued
- **THEN** ACECode cancels or discards that work and joins the worker before its HTTP
  and cache dependencies are destroyed

#### Scenario: Multiple calls request the same endpoint key
- **WHEN** nonblocking resolution is called repeatedly for one uncached endpoint key
- **THEN** ACECode queues at most one probe for that key until it completes or is
  cancelled

### Requirement: Background probes preserve nonblocking resolution
ACECode SHALL return the available local or fallback context without waiting for the
remote endpoint and SHALL cache a valid endpoint result for later calls.

#### Scenario: Endpoint metadata arrives after fallback
- **WHEN** the first nonblocking call has no local metadata and the endpoint later
  returns a valid context window
- **THEN** the first call returns its fallback immediately and a later call returns
  the cached endpoint context

### Requirement: Test reset is probe-quiescent
The test cache reset SHALL prevent work started before the reset from mutating the
cleared cache afterward.

#### Scenario: Reset while work is pending
- **WHEN** a test resets model-context state with active or queued probes
- **THEN** the reset cancels or drains that work before clearing cache and in-flight
  keys
