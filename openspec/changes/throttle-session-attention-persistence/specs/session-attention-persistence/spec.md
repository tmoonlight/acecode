## ADDED Requirements

### Requirement: Cursor-only attention persistence is coalesced
The daemon SHALL update attention records in memory for every relevant session
event but SHALL coalesce cursor-only persistence by workspace on a bounded
background interval.

#### Scenario: Streaming events advance only cursors and timestamps
- **WHEN** many token, reasoning, or tool events update one workspace without changing attention state or busy state
- **THEN** each event updates the in-memory attention record
- **AND** the workspace is marked dirty without synchronously rewriting the state file for every event
- **AND** the background flusher writes the latest coalesced state no later than its next normal interval

#### Scenario: Multiple sessions share a workspace
- **WHEN** concurrent sessions mark the same workspace dirty before a flush
- **THEN** the flusher writes one workspace snapshot containing the latest records for all sessions

### Requirement: User-visible attention transitions remain immediately durable
The daemon SHALL persist attention state and busy-state transitions
synchronously before treating the transition as complete.

#### Scenario: Read or busy state changes
- **WHEN** an event changes read, unread, in-progress, or busy state
- **THEN** pending dirty attention state is flushed immediately
- **AND** the status broadcast uses the updated in-memory record

#### Scenario: Session is explicitly marked read
- **WHEN** a client advances a session read cursor
- **THEN** the updated read state is saved immediately
- **AND** a failed save remains eligible for background retry

### Requirement: Failed attention writes remain retryable
The daemon MUST clear a workspace's dirty marker only after its attention state
file has been written and atomically replaced successfully.

#### Scenario: Temporary file or rename fails
- **WHEN** directory creation, temporary-file creation, writing, or final rename fails
- **THEN** the workspace remains dirty
- **AND** a later background flush attempts persistence again
- **AND** the failure is logged without terminating the daemon

#### Scenario: Attention state is saved successfully
- **WHEN** the complete compact JSON snapshot is written and renamed successfully
- **THEN** the workspace dirty marker is cleared
- **AND** the on-disk schema remains compatible with version 1 readers

### Requirement: Normal shutdown drains attention persistence safely
The daemon SHALL stop attention event producers before stopping the attention
flusher and SHALL perform a final flush of pending workspace state.

#### Scenario: Web server implementation is destroyed
- **WHEN** normal shutdown begins
- **THEN** listeners and tracked attention subscriptions are stopped or detached
- **AND** the flusher performs a final dirty-state flush
- **AND** the flusher thread is joined before attention state is destroyed

#### Scenario: Event callback overlaps shutdown
- **WHEN** an attention event callback is already in progress during shutdown
- **THEN** destruction waits for producer detachment before the final flush
- **AND** no callback can mark new dirty state after the flusher has stopped
